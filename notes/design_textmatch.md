# textmatch 设计草案

> 目标：把 `lua/lutf8lib.c` 的 Unicode Lua pattern 匹配移植成独立头文件
> `textmatch.h`，直接跑在 piecetab 的 piece 表上。
> 前缀 `tm_`，stb-style 纯头文件，C89，无堆分配。

## 一、定位

**负责**
- 在任意“可随机按 byte offset 取 piece”的文本源上做 Lua pattern 匹配
- 按 Unicode codepoint 匹配（同 `lutf8lib.c` 语义）
- 返回 byte offset（不是 Lua string index）
- 只做 forward 搜索；反向搜索由调用方用 forward 反复找

**不负责**
- 编辑/替换/gsub（需要可写 buffer 或分配，后续另设计）
- 管理 piecetab 生命周期；只通过 `tm_Reader` 回调取文本
- 正则编译/缓存（Lua pattern 足够小，逐次解释即可）
- 调用方的搜索循环推进由调用方负责，通过 `tm_seek`/`tm_advance` 完成

## 二、核心抽象：tm_Reader

```c
typedef tm_Slice tm_Reader(void *ud, size_t *poff);
```

这是 termfeed `tf_Reader` 的“可 seek”版本。区别：

- `tf_Reader` 是 forward-only chunk 拉取，没有 rewind/seek/prev；
- `tm_Reader` 的 `*poff` 是 in/out 绝对 byte offset，调用方可以跳到任意位置；
- 返回整个 piece 的 `tm_Slice`，tm 内部再从 slice 取当前字节/剩余长度。

### Reader 契约

1. 入参 `*poff` = 调用方想定位的 byte offset。
2. 若该位置有字符：
   - 出参 `*poff` = 返回 piece 的起始 offset；
   - 返回该 piece 的完整 `tm_Slice`（`s` 指向 piece 起始，`e` 指向 piece 末尾）；
   - 保证 `piece_start <= target < piece_start + len`。
3. 若该位置没有任何字符（EOF 或越界）：
   - 返回空 slice（`s == NULL && e == NULL`）；
   - `*poff` 不变（保持入参 target）。
4. `tm` 会把返回的 piece 保存在 `S->cache`/`S->cstart`，只要当前
   位置仍在这些字段描述的 piece 内就复用，不重复调用 Reader。
5. 调用方（Reader 实现）不保证返回指针永久有效；`tm` 只在 cache 有效期间
   引用它，一旦 `tmU_goto` 跳到 cache 外或再次 load 就会替换/丢弃。

### 推荐内部实现（piecetab 适配器）

```
ud 里缓存当前 Cursor
1. target 落在当前 piece 范围 → 直接返回缓存的 piece
2. 试 next/prev 一步，落在相邻 piece → 返回
3. 都不在 → pt_seek 到 target，再 pt_piece 返回
```

这样正向/反向逐字符都在 piece 内 O(1)，跨 piece 是 O(1) 的 next/prev，
任意跳转才 O(log n)。

## 三、状态与接口设计

API 是**有状态的**：Reader、flags、pattern、当前位置都绑定在
`tm_State` 上。这样：

- 签名短，全部低于 80 列；
- source 侧的 cache/pos/current/prev 集中在 `tm_State`；匹配链只传
  `tm_Match *M`，不传 `tm_State *S`，也不传 `off`/`Cursor`；
- `pat` 只是内存中的 `tm_Slice`，作为参数沿匹配链传递，不进入全局可变状态；
- literal 通过 flag 表达；
- 每个函数职责单一。

### Flags

```c
#define TM_LITERAL    (1 << 0)
#define TM_LINEANCHOR (1 << 1)
```

- `TM_LITERAL`：pattern 按字面量处理，不做 pattern 解析（memfind 路径）。
- `TM_LINEANCHOR`：`^` 匹配 offset 0 或 `\n` 之后；`$` 匹配 EOF 或 `\n` 之前。
  未设置时按 Lua 语义：`^` 锚定当前搜索起点，`$` 锚定 EOF。
- 读写通过宏：`tm_flags(S)` 读取，`tm_setflags(S, f)` 覆盖设置。

### 错误码

```c
#define TM_MATCHED   (1)
#define TM_OK        (0)
#define TM_ERRPARAM  (-1)
#define TM_ERRPATTERN (-2)
#define TM_ERRCOMPLEX (-3)
```

| 值  | 名字            | 含义                                                                                                              |
| --- | --------------- | ----------------------------------------------------------------------------------------------------------------- |
| 1   | `TM_MATCHED`    | 匹配/找到                                                                                                         |
| 0   | `TM_OK`         | 没匹配到，或操作成功                                                                                              |
| -1  | `TM_ERRPARAM`   | 参数错误：NULL、没设置 pattern、非法 capture index、非法 offset 等                                                |
| -2  | `TM_ERRPATTERN` | pattern 语法/结构错误：缺 `]`、`%` 结尾、`%b` 参数缺失、非法 capture 引用、超过最大 capture 数、未闭合 capture 等 |
| -3  | `TM_ERRCOMPLEX` | 匹配递归过深（`TM_MAXCCALLS` 超限）                                                                               |

本设计无堆分配。

### 公开接口

```c
#ifndef TM_MAX_PATTERN_COUNT
# define TM_MAX_PATTERN_COUNT 9
#endif

typedef tm_Slice tm_Reader(void *ud, size_t *poff);

typedef struct tm_Capture { size_t start, len; } tm_Capture;
typedef struct tm_State tm_State;

TM_API void tm_init(tm_State *S, tm_Reader *r, void *ud);

#define tm_flags(S)       ((S) ? (S)->flags : 0)
#define tm_setflags(S, f) ((void)((S) && ((S)->flags = (f))))

TM_API int tm_seek(tm_State *S, size_t off);
TM_API int tm_advance(tm_State *S, ptrdiff_t delta);

TM_API int tm_pattern(tm_State *S, tm_Slice pattern);
TM_API int tm_match(tm_State *S);
TM_API int tm_find(tm_State *S);

TM_API size_t tm_offset(const tm_State *S);
TM_API size_t tm_matchend(const tm_State *S);

#define tm_captures(S) ((S) ? (S)->level : 0)

TM_API int tm_capture(const tm_State *S, int i, tm_Capture *out);
```

`TM_MAX_PATTERN_COUNT` 是编译期最大 capture 数量，默认 9；使用者可在
包含 `textmatch.h` 前自行 `#define` 覆盖。
`TM_MAXCCALLS` 在 implementation 段默认 200，也可在包含前覆盖。
头文件支持 `TM_STATIC_API` / `TM_API` / `TM_NS_BEGIN` / `TM_NS_END` 的
stb 风格配置，`tm_State` 在公开头文件中完整定义。

### 语义

- `tm_init`：绑定 Reader/ud，flags=0，pattern 为空，当前位置=0，
  `current`/`prev` 为 `TM_UNKNOWN`。
- `tm_setflags(S, f)`：覆盖设置 flags；`tm_flags(S)` 读取当前 flags。
- `tm_pattern`：设置 pattern slice（借用指针，不拷贝；调用方保证生命周期）。
  没设置 pattern 时调用 `tm_match`/`tm_find` 返回 `TM_ERRPARAM`。
- `tm_seek` / `tm_advance`：负责定位。二者都走 `tmU_goto`，重置
  `current`/`prev`，不扫描源文本。`tm_advance` 是 byte delta，可正可负；
  Unicode 推进（如空匹配后前进一个 codepoint）由调用方自行计算。
- `tm_match`：只在当前位置匹配。返回 `TM_MATCHED` / `TM_OK` / 负错误；
  成功时 `S->m.end` 是 exclusive byte offset，并把 `S->pos` 恢复为匹配起点。
- `tm_find`：从当前位置 **forward** 搜索。返回 `TM_MATCHED` / `TM_OK` / 负错误；
  成功时 `S->pos` 是匹配起点，`S->m.end` 是匹配终点（exclusive）。
- 匹配长度 = `tm_matchend(S) - tm_offset(S)`。
- `tm_offset(S)` 返回 `S->pos`；`tm_matchend(S)` 返回 `S->m.end`。
- `tm_captures(S)`：返回 capture 数量（>=0）。
- `tm_capture(S, i, &out)`：返回 `TM_OK` 或错误；通过 `tm_Capture out` 返回
  `start`/`len`。所有 offset 都是 byte offset；`()` position capture 的 `len == 0`。
  `i` 是 0-based 显式 capture 下标，不含整个匹配。

## 四、集中式 source 迭代

Reader 读取是昂贵操作。`tm_State` 分成两部分：

- `tm_Match`：pattern、flags、capture、depth 等纯匹配状态；
- `tm_State` 其余字段：reader、cache、`pos/next/current/prev` 等 source 游标。

匹配层函数只接收 `tm_Match *M`，不接收 `tm_State *S`。  
所有 source 操作都通过游标 API 完成；任何接受 `off` 参数的底层函数都只接收
`tm_State *S`，因此匹配层无法直接按 offset 读字节。

```c
typedef struct tm_Match {
    tm_Slice   pat;
    size_t     end;
    int        depth;
    int        level;
    int        flags;
    tm_Capture cap[TM_MAX_PATTERN_COUNT];
} tm_Match;

struct tm_State {
    tm_Match   m;
    tm_Reader *reader;
    void      *ud;
    tm_Slice   cache;
    size_t     cstart;
    const char *p;
    size_t     pos;
    size_t     next;
    utfint     current;
    utfint     prev;
};
```

哨兵：

```c
#define TM_UNKNOWN ((utfint)-2)  /* not decoded yet */
#define TM_EOS     ((utfint)-1)  /* end of source / before start */
```

游标 API（匹配层可见）：

```c
static size_t tmU_offset(tm_Match *M);
static utfint tmU_peek(tm_Match *M);
static utfint tmU_next(tm_Match *M);
static utfint tmU_prev(tm_Match *M);
static void   tmU_save(tm_Match *M, tm_Save *save);
static void   tmU_restore(tm_Match *M, const tm_Save *save);
static int    tmU_isboundary(tm_Match *M);
static int    tmU_islinestart(tm_Match *M);
static int    tmU_islineend(tm_Match *M);
static int    tmU_backref(tm_Match *M, utfint ch);
```

底层物理/范围函数（匹配层不可见，只接收 `tm_State *S`）：


```c
static int         tmS_load(tm_State *S, size_t off);
static tm_Slice      tmS_at(tm_State *S, size_t off);
static size_t      tmS_copy(tm_State *S, size_t off, char *buf, size_t n);
static void        tmU_setp(tm_State *S);
static int         tmS_equalmem(tm_State *S, size_t off, tm_Slice mem);
static int         tmS_equal(tm_State *S, size_t a, size_t b, size_t len);
static size_t      tmS_find(tm_State *S, size_t from, tm_Slice pat);
static void        tmU_goto(tm_State *S, size_t off);
static void        tmU_reset(tm_State *S);
```

语义：

- `p`：cache 内当前字节的挥发指针；`NULL` 表示未 materialize、位于 cache
  末尾、或 cache 已被更换。
- `tmU_peek`：`current` 已知直接返回；`p == NULL` 时先建立 `p`（必要时
  load），然后解码当前字符并设置 `next`。
- `tmU_next`：`prev = current`，前进到下一 codepoint 并解码；前进后用
  `tmU_setp` 重新 materialize `p`。
- `tmU_prev`：回退到前一个 codepoint 起点并解码；`pos == 0` 时返回 `0`，
  不返回 `TM_EOS`。会正确处理 mid-character 与非法 continuation 串的边界。
- `tmU_goto`：设置 `pos = off`，清 `current/prev`，并用 `tmU_setp` 建立或
  失效 `p`；若 `off == pos`，内部直接走 `tmU_reset`。
- `tmU_reset`：不移动 `pos`，只清 `current/prev/next` 并重建 `p`。
- `tmU_save/restore`：保存/恢复 `pos/next/current/prev`；restore 后用
  `tmU_setp` 重建 `p`。
- `tmU_setp`：根据当前 `pos` 与 cache 的包含关系设置 `p`；`pos` 在 cache
  内且不是末尾时指向对应字节，否则置 `NULL`。
- `tmU_isboundary/islinestart/islineend`：基于 `prev`/`peek` 的语义谓词，
  匹配层不再直接读“pos 前面的字节”。

`next` 是 source 游标状态的一部分，任何保存/恢复游标的代码都必须同时
保存/恢复 `pos`/`next`/`current`/`prev`。`p` 不进入 save/restore，因为它
可以由 `pos` 与 cache 重建。

`tmS_load` 在真正更换 cache 时会把 `p` 置 `NULL`。`tmU_peek` 的 fill 分支
只处理 `p == NULL` 的情况；顺序前进时 `tmU_next` 已负责维护 `p`，因此
peek 的热路径不需要做 `tmS_incache` 范围判断。
`tmS_at` 只用于真正需要任意方向/任意 offset 的随机访问场合。

## 五、匹配引擎

移植 `lutf8lib.c` 的 `lu_MatchState` 逻辑：

- 固定 capture 数组，大小为 `TM_MAX_PATTERN_COUNT`（默认 9）
- 递归深度上限 `TM_MAXCCALLS = 200`
- `%a %c %d %g %l %p %s %u %w %x %z`、`[...]`、`%b`、`%f`、`%1..%9`
- `^ $ * + - ?` 后缀
- `()` position capture

与 `lutf8lib.c` 的差异：

- 位置用 `size_t` byte offset，不保存裸 `const char *`
- source 游标集中在 `tm_State`；匹配函数只接收 `tm_Match *M`，不再接收 `tm_State *S` 或 `off`
- capture 存 `{ start, len }`，backref 时通过 Reader 做 piece-wise compare
- plain/literal 不用 `memchr/memcmp` 一次性比较，改为跨 piece 的 byte compare
- 匹配失败回溯时通过 `tmU_save`/`tmU_restore` 保存/恢复 `pos/next/current/prev`
- pattern/source 中的非法 UTF-8 不报错：无法按 UTF-8 解码的字节按单字节普通字符匹配
- 字符分类函数可配置：`TM_IS(cat, c)` 宏默认展开为 C ctype 的 `is##cat`，
  不依赖 `lua/unidata.h`；使用方可在包含前自行定义 `TM_IS`（测试里用
  `lua/unidata.h` 提供 Unicode 分类）。`%t` 默认用临时 `iscompose` 宏映射为 0。

匹配内部统一使用公开的 `TM_MATCHED` / `TM_OK` / 负错误码，另加一个
`TM_CONTINUE` 表示“本 step 成功，继续匹配下一项”。不再使用 `TM_STEP_*`。

### 内部辅助类型


引擎内部使用 `tm_Slice` 表示剩余 pattern；source 位置始终在 `S` 中，
不再需要额外的 step 结构：

```c
typedef struct tm_Slice { const char *s, *e; } tm_Slice;
```

- `tm_Slice` 表示 pattern 中的一个片段（如 `[...]` 的括号体），
  传给 `tmM_bracketclass`、`tmM_maxexpand`、`tmM_minexpand`、`tmM_suffix`；
  匹配链中需要推进 pattern 时传 `tm_Slice *`。
- `tmS_len(s)` 返回 slice 字节数，`tmS_empty(s)` 判断空 slice（NULL 哨兵也视为空）；
  这两个小 helper 放在 implementation 前部，供所有 slice 操作复用。
- 内部匹配函数（`tmM_match`/`tmM_try`/`tmM_maxexpand` 等）返回 `int` 状态码
  （`TM_MATCHED`/`TM_OK`/负错误码），成功位置通过 `tmU_offset(M)` 表达；
  错误直接沿返回值传播，不再使用 `S->err` 边信道。
- `tmOK(x)` 是内部错误传播宏：执行 `x`，若结果为负立即 `return` 该错误码；
  用于把 `TM_ERRPATTERN`/`TM_ERRCOMPLEX` 沿调用链上传。
- 只读 pattern 范围传 `tm_Slice`；需要前进/返回解码位置时传 `tm_Slice *`
  （如 `tmS_decode`、`tmM_classend`、`tmM_balance`）。

### 关键匹配路径

- 单字符项：`tmU_peek` 取当前字符，匹配成功后 `tmU_next` 前进。
- `%b`：用 `tmU_next` 返回值逐字符数嵌套；闭合符命中且计数归零时再
  `tmU_next` 一次，把闭合符本身消费掉。
- `%f`：通过 `tmU_prevcp(M)` 取得前一个 codepoint，匹配层不直接碰 `prev`。
- `*` / `+` / `-`：`tmU_next` / `tmU_prev` 返回值通常丢弃，以 `tmU_offset(M)` /
  `tmU_peek(M)` 为真相；`?`/`*`/`-` 的零次回溯通过 `tmU_save`/`tmU_restore` 完成。
- Backrefs（`%1..%9`）通过 `tmU_backref(M, ch)` 完成；literal 搜索使用独立低层
  helper（`tmS_equal`、`tmS_equalmem`、`tmS_find`），这些 helper 只接收
  `tm_State *S`，不走 `peek`/`next`/`prev`。

### pattern 错误

Lua pattern 没有独立编译步骤，但匹配过程中可能失败：

- `"["` → malformed pattern (missing `]`)
- `"abc%"` → malformed pattern (ends with `%`)
- `"%b"` → missing arguments to `%b`
- `"%f"` → missing `[` after `%f`
- `")"` → invalid pattern capture
- `"(%1)"` → invalid capture index
- 超过 `TM_MAX_PATTERN_COUNT` 个 capture → too many captures
- 递归太深 → pattern too complex

所以 `TM_ERRPATTERN` 和 `TM_ERRCOMPLEX` 都是必要的。

## 六、搜索方向

### 正向 `/`

从当前位置向尾部尝试每个 codepoint 起点，直到找到匹配。直接对应
`luM_findpattern`。

### 反向 `?`

调用方用 forward 反复找、保留最后一个：

```
best = none
p = 0
while p < before:
    r = tm_seek(S, p); if r != TM_OK: ...
    if tm_find(S) != TM_MATCHED: break
    best_start = tm_offset(S); best_end = tm_matchend(S)
    p = best_end
    if best_end == best_start: p = /* caller 自己前进一个 codepoint */
return best
```

Neovim 是“行方向反向 + 行内 forward 枚举最后一个”，Vis 是“反复 forward
搜索保留最后一个”；这里采用同样的策略。

### 空匹配推进

`tm_find` 可能返回空匹配（`tm_matchend(S) == tm_offset(S)`）。为避免死循环，
调用方在连续搜索时必须自己推进：

```
while (tm_seek(S, from) == TM_OK
       && tm_find(S) == TM_MATCHED) {
    start = tm_offset(S);
    end = tm_matchend(S);
    use_match(start, end - start);
    from = (end == start) ? /* 前进一个 codepoint */ : end;
}
```

Unicode 推进由调用方负责，通过 `tm_seek`/`tm_advance` 完成。

## 七、内部数据结构草案

```c
typedef struct tm_Capture { size_t start, len; } tm_Capture;
typedef struct tm_Slice   { const char *s, *e; } tm_Slice;

typedef struct tm_Match {
    tm_Slice   pat;
    size_t     end;
    int        depth;
    int        level;
    int        flags;
    tm_Capture cap[TM_MAX_PATTERN_COUNT];
} tm_Match;

typedef struct tm_State {
    tm_Match   m;
    tm_Reader *reader;
    void      *ud;
    tm_Slice   cache;
    size_t     cstart;
    size_t     pos;
    size_t     next;
    utfint     current;
    utfint     prev;
} tm_State;
```

`tm_State` 在公开头文件中完整定义，调用方直接栈上分配。  
`tm_Match` 是 `tm_State` 的第一个成员；底层工具函数通过 `(tm_State *)M`
取回完整状态，匹配层本身不持有 `tm_State *`。

## 八、已定/待定

**已定**
- 有状态 API：Reader/flags/pattern/position 绑定在 `tm_State`
- source 侧 cache/pos/current/prev 集中在 `tm_State`；匹配链只传 `tm_Match *M`，不传 `tm_State *S`/`off`/`Cursor`
- `pat` 保持 `tm_Slice` 参数，不改成全局可变工作状态
- literal 通过 `TM_LITERAL` flag 表达
- `tm_match` / `tm_find` 分离：match 只匹配当前位置，find 负责 forward 搜索
- 结果通过 `tm_offset` / `tm_matchend` 读取，不再用出参
- 反向搜索由调用方用 forward 反复找实现
- 空匹配推进由调用方负责，通过 `tm_seek`/`tm_advance` 完成
- 匹配/查找/参数类接口返回 int 状态码；flags 与 capture 数量通过宏读取
- `TM_MAX_PATTERN_COUNT` 作为编译期最大 capture 数量，默认 9
- `TM_LINEANCHOR` 下 `^` 匹配 offset 0 或 `\n` 后，`$` 匹配 EOF 或 `\n` 前
- 空 pattern 保留 Lua 语义：匹配当前位置（含 EOF），不匹配越界位置
- 第一版支持 deprecated 的 `%z`
- 非法 UTF-8 按普通字节匹配，不返回错误
- 字符分类通过 `TM_IS(cat, c)` 宏配置，默认 C ctype，头文件不依赖 `lua/unidata.h`
- 未知大写转义类按 Lua 语义作普通字面量（如 `%Q` 匹配 `Q`，不是 `q` 的补集）
- 内部 helper 的确定前置条件用 `assert` 表达，不保留不可达的防御分支
- 匹配层由 `misc/check_textmatch.lua` 检查：匹配区域内禁止出现 `S`，从结构上防止匹配函数直接访问 source 游标
