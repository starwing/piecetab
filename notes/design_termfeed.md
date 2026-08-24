# termfeed 设计草案

> stb-style 纯头文件 C89 库，前缀 `tf_`。
> ZIO reader + DSA（确定性状态自动机）终端输入流解析器。
> 目标: 完整替换 Neovim 内部 vendored libtermkey，接口更正交清晰。
> 行为上除刻意的设计变更外，与 libtermkey 兼容以保证可无缝迁移。

## 一、定位

termfeed 是**终端输入流解析器**——从 reader 回调提供的字节流中
逐字节识别键盘输入、鼠标事件、控制序列。不管理 I/O。

**负责:**
- UTF-8 解码
- CSI/SS3 转义序列识别（键、鼠标、位置报告、模式报告）
- control string (DCS/OSC/APC) 定界与内容拷贝
- 修饰键解析（含 kitty CSI u 协议事件类型）
- 键名查找（`tf_Sym`↔名称）和格式化（`tf_format`）
- 字符串→Key 反解析（`tf_parse`，替代 libtermkey 的 `strpkey`）
- terminfo 键序列加载 — 通过 `tf_Lookup` 回调，调用方提供 terminfo 数据
  (不内置 terminfo 数据库解析)

**不负责:**
- I/O 轮询/阻塞/非阻塞（`tf_waitkey` 除外——提供 fd+timeout 封装）
- termios 配置
- 输出编码（key→ANSI 序列）

**与 piecetab 技术栈的关系**: 完全独立，零依赖。

## 二、核心抽象: ZIO reader + DSA

### 2.1 Reader 回调

```c
typedef const char *tf_Reader(void *ud, size_t *plen);
```

语义同 Lua ZIO: 返回数据指针 + 可用字节数。返回 `*plen=0`
表示"暂无数据"，调用方稍后重试。解析器不从 reader 返回的数据拷贝
到内部 buffer——**input zero-copy**。

reader 每次调用返回的数据是独立的——下次 reader 调用后，上次
返回的指针可能失效。因此 DSA 不能持久持有跨 reader 调用的
buffer 引用。

**Neovim 对接**: libuv 回调收到 `(buf, len)`，薄 reader 直接返回 buf:
```c
const char *reader(void *ud, size_t *plen) {
    *plen = ctx->len; return ctx->buf;
}
```
零拷贝，不设 push_bytes 式接口。

### 2.2 返回值

```c
#define TF_OK       (0)
#define TF_NONE     (1)
#define TF_AGAIN    (2)
#define TF_ERRPARAM (-1)
#define TF_ERRMEM   (-2)
```

函数返回 `int`。`TF_OK` = key 已写入 `*key`。`TF_NONE` = DSA 在 IDLE 态 +
reader 暂无可读数据（干净空闲，无未完成序列）。`TF_AGAIN` = DSA 在非 IDLE 态 +
reader 无数据（等更多字节或 flush）。`TF_ERRPARAM` = 无效参数。
`TF_ERRMEM` = 内存分配失败。

**无 EOS**: reader 可以随时返回 NULL/0 表示"暂无数据"，调用方稍后重试即可。
终端 stdin 可以被重定向、被挂起后恢复——reader 永远可能再有数据。
因此 termfeed 不区分 EOS 和"暂无数据"。

### 2.3 自定义内存分配与核心 API

```c
void tf_feed(tf_State *S, tf_Reader *r, void *ud);
int  tf_readkey(tf_State *S, tf_Key *key);
int  tf_flush(tf_State *S, tf_Key *key);
int  tf_waitkey(tf_State *S, int fd, int timeout_ms, tf_Key *key);
```

**tf_feed**: 仅设置 reader，不读数据。后续 `tf_readkey` 从此 reader
逐字节推进 DSA。**换 reader 即放弃旧 chunk 剩余**（无条件清 p/n——
旧 chunk 的未消费字节由调用方重新喂入；REPLAY 态中的 buf 段不受影响，
由 `tf_flush`/CSI 超限快照独立维护）。

**chunk 引用原子性**（tf_feed 任意时刻调用安全的前提）: 库对外部
数据的全部引用是 `p/n` 两字段。`tf_readkey` 是同步主循环，外部调用点
只在 readkey 返回处，而返回点只有两种边界:

- **TF_OK（key 边界）**: `p/n` = 多 key 剩余（或空）；且 TF_OK 恒伴
  `state == IDLE`
- **TF_NONE/TF_AGAIN（耗尽边界）**: `n == 0`，chunk 完全消费
  （`tfZ_nextbyte` 只在 n==0 时拉新 chunk，无数据才返回 -1）

中间态只存在于 readkey 执行中，外部不可达。因此 **tf_feed 任意时刻
调用安全**——清 p/n 即解除全部外部引用，无悬垂；DSA 内部状态
（buf/cs_buf/replay/state）是库自有内存，不受影响。语义后果:

- 耗尽边界调用: 零损失，DSA 部分序列（buf/buf_len）保留，新源续读——
  **核心用途**（跨输入源接续解析）
- key 边界调用: 丢弃 chunk 剩余——**含 waitkey 读入未产出的字节**
  （wait 数据就在 chunk 里，见 §2.3 tf_waitkey）。换源 = 主动放弃，
  用户需自行重新喂入。**使用纪律: 旧 chunk 消费完（readkey 返回
  TF_NONE/AGAIN）后再换源**——waitkey 内部已保证，用户混用时注意。

**tf_readkey**: 从 reader 读字节推进 DSA。DSA 完成完整序列 →
返回 `TF_OK`。reader 无数据:
- DSA 非 IDLE → `TF_AGAIN`（部分序列，等更多字节或 flush）
- DSA IDLE → `TF_NONE`（干净空闲）

**tf_flush**: 模拟 libtermkey `getkey_force` 语义（剥皮模型）——
把当前 DSA 状态解构为 key 输出，残余字节经 REPLAY 重播。
（基线确认自 Nvim `peekkey_simple` + driver force 分支: `\e[` 未完成
→ ALT+`[` 吃 2 字节; `\eO` → ALT+O 全吃; CS 剥皮 ALT+`]` 内容保留;
UTF-8 部分 → UNICODE(0xFFFD) 全吃; `\e\e` → ALT+ESC。）

```
flush for non-IDLE state:
  IDLE          → *key = TF_SYM_NONE（空 key）, 返回 TF_OK
  ESCAPE        → ESC key（经 canonicalise）
  ESCAPE(+alt)  → ALT+ESC
  CSI           → ALT+[ + replay(buf, buf_len)   /* 参数区原始字节 */
  SS3           → ALT+O
  CS_DCS        → ALT+P + IDLE 重解析 cs_buf
  CS_OSC        → ALT+] + IDLE 重解析 cs_buf
  CS_APC        → ALT+_ + IDLE 重解析 cs_buf
  UTF8          → UNICODE(0xFFFD)                  /* 非法序列, 不重播 */
  MOUSE_X10     → ALT+[ + replay("M" + buf, 已收 raw)
  replay 中     → 空 key（flush 不重入）
```

**回放（replay）处理**: 残余字节（flush 快照 / X10 已收 raw / CSI 超限
参数区）搬到 `S->buf` 尾部（`memmove(buf + 64 - n, buf, n)`，n=64 时
原地），置 `S->replay = n`。`tfZ_nextbyte` 优先从源取字节——
`buf[TF_MAX_BUFLEN - S->replay]`（**索引与 buf_len 解耦**——DSA 写 buf
时 buf_len 会被 CSI/UTF8/X10 态重置，源索引不受影响）。**残余字节经
DSA 主循环重解析**（如同新输入）:

- 单字节 → 正常 canonicalise（TAB/DELBS/SPACESYMBOL/trie 全部生效）
- `0x1b` → ESCAPE 态 → flush 或后续字节合成 `<Esc>` / ALT+key
- 多字节 UTF-8 lead → UTF8 态续读（跨 chunk 路径，源逐步推进）
- replay 耗尽 → 主循环直接接续 reader（不回 TF_NONE，避免数据滞留）

**自覆盖安全**: DSA 写 buf（CSI/UTF8/X10 态）从 buf[0] 起，而 ESCAPE+
前缀两字节不写 buf → 写位置恒落后源读取 ≥2，写到的永远是已消费区。
源 ≤64 字节 → 重解析后参数最多 62（2 前缀）不达超限阈值 → 递归
replay 不可达（接续 chunk 后的超限走正常输入路径）。

**flush 不重入**: replay 中 state 恒为 IDLE（replay 计数独立）→
`tf_flush` 返回空 key。replay 重解析中途（DSA 已推进到 ESCAPE 等态）
flush 正常剥皮，replay 剩余继续（无嵌套）。

**S->p/S->n 不参与**（chunk 剩余保持, replay 完主循环直接接续, 无丢失;
但换 reader 时 tf_feed 无条件清 p/n——见 §2.3 tf_feed）。CS 内容是唯一
不定长残余——经 p/n 借道走 IDLE **重解析**（对齐 Nvim force 的
buffer 重解析语义; 借道前提: flush 时 chunk 已空, assert(S->n==0)）。

回放输出完**不回 TF_NONE**——回到主循环继续读 reader（与
libtermkey 的"flush 后保留 buffer 继续解析"等价，避免数据滞留）。

**tf_waitkey**: 封装 fd → `tf_readkey` 的**样板实现**——展示如何用
公开 API（`tf_feed`/`tf_readkey`/`tf_flush`/poll）组合出
"fd + timeout → key"，也是验证解析 API 好用性的试金石。签名
`tf_waitkey(S, fd, timeout_ms, key)`（key 最后）。循环:
1. `tf_readkey` 返回 TF_OK → 返回 TF_OK；TF_ERRxxx → 透传
2. TF_NONE / TF_AGAIN → `poll(fd, timeout_ms)`
3. 超时: 最近一次 readkey 返回 TF_AGAIN（部分序列）→ `tf_flush`
   输出 flush key；TF_NONE（干净）→ 返回 TF_AGAIN（无数据）——
   **判断依据是 readkey 的公开返回值，不依赖 `tf_State` 内部字段**
   （§2.2 契约: TF_AGAIN ⟺ DSA 非 IDLE，TF_NONE ⟺ IDLE）
4. POLLIN → `read` 进 wait 缓冲 → `tf_feed` 包全量交付 reader
   注入 → 回到步骤 1
read 内部重试 EINTR/EAGAIN; poll 被 EINTR 打断 → 继续循环。
**`_WIN32` 分支保持 TF_ERRPARAM**——不在头文件里实现 Windows
console 支持；Windows 的 console 按键读取放在绑定层
`lua/termfeed.c`：`Ltf_waitkey` 用 `WaitForSingleObject` +
`ReadConsoleInputW` 直接填 `tf_Key`（方向键/功能键/Alt/Ctrl/普通字符），
绕过 `tf_feed`/`tf_readkey`/`tf_flush` 管线。
`lua/termfeed.c` 的 Windows `raw`/`cooked` 用 `SetConsoleMode` 关闭
echo/line/processed input，供 editor demo 使用。

**wait 缓冲不是输入源，是 read 目标**: `tf_State.wait`（`tf_WaitCtx`
{ buf, len }，缓冲懒分配 `TF_WAIT_BUFSIZE`、`tf_free` 释放——大小恒等于
宏，故无容量字段）
只是 read 的目标缓冲，数据经**标准 chunk 通道**（`tf_feed` + 全量
交付 reader `tfZ_waitread`，见下方伪代码）喂入——**无第二输入源、
无优先级**（`tfZ_nextbyte` 仍是 replay > reader 两分支）。多余字节
（一次 read 可能含多个 key）留在库 chunk（p/n → wait 缓冲），**跨
waitkey 调用自动消费，不丢**。覆盖安全: waitkey 在 read 覆盖 wait
缓冲前，readkey 必已返回 TF_NONE/AGAIN（chunk 空）。**tf_feed 换源
会丢弃未消费 chunk——包括 waitkey 读入未产出的字节**（换源 = 主动
放弃，用户需自行重新喂入，见 §2.3 tf_feed 原子性）。libtermkey
对照: `tk->buffer` + `buffstart/buffcount` 游标 + `push_bytes`——
termfeed 以 reader 回调替代 push_bytes，wait 数据走同一通道，
无独立队列抽象。

```c
/* 全量交付 reader: 库 chunk 耗尽才再调, 故可一次性交出并推进 */
static const char *tfZ_waitread(void *ud, size_t *plen) {
    tf_WaitCtx *w = (tf_WaitCtx *)ud;
    const char *p = w->buf;
    *plen = w->len;
    w->len = 0;
    return p;
}
```

**典型循环**:

```c
tf_feed(S, reader, ud);
for (;;) {
    int r = tf_readkey(S, &key);
    if (r == TF_OK)   { handle_key(&key); continue; }
    if (r == TF_NONE) { /* 干净空闲, 等下次 IO */ break; }
    if (r == TF_AGAIN) {
        /* 部分序列未完成, 等 timer/IO → 继续 tf_readkey；或超时 → tf_flush */
        break;
    }
    /* TF_ERRxxx */
    break;
}
/* timer/force: */
tf_flush(S, &key);  /* 强制当前部分序列 → key */

## 三、DSA 设计

### 3.1 tf_State

```c
typedef struct tf_State {
    tf_Alloc *allocf;      /* 内存分配器 (realloc 风格) */
    void     *alloc_ud;
    int       flags;       /* TF_FLAG_xxx */
    int       state;       /* DSA 状态 (TF_STATE_xxx, 恒 >= 0) */
    int       pending_mod; /* ESC 前缀标志 (剥皮 \e 记 ALT) */

    /* 部分序列字节缓冲——"部分序列"的全部信息的唯一数据源:
       - CSI:  buf[0..buf_len) = 参数区原始字节（flush 重播 /
         final 时一次性解析 / kitty 子参数与 text）
       - UTF-8: buf[0..buf_len) = lead + 已收续字节
       - X10:  buf[0..buf_len) = 已收 raw bytes
       状态仅表示"序列类型"，进度一律由 buf_len 描述 */
    int  buf_len;
    char buf[TF_MAX_BUFLEN];

    /* replay: flush/CSI 超限/SS3 拒绝的残余字节, 经 DSA 重解析为新输入。
       源位于 buf 尾部: buf[TF_MAX_BUFLEN - replay .. -1]（索引与 buf_len
       解耦——DSA 写 buf 时 buf_len 会被重置）。0 = 无。 */
    int replay;

    /* control string / kitty text 缓冲区 */
    char *cs_buf;
    int   cs_len;
    int   cs_cap;

    /* terminfo: trie (lookup 回调不保留——tf_load 一次性构建) */
    struct tf_Node *root;   /* trie 根节点 */
    struct tf_Node *node;   /* 当前匹配节点, NULL=死路 */

    /* reader 引用 (由 tf_feed 设置) */
    tf_Reader *reader;
    void      *reader_ud;
    /* 当前 chunk 内偏移 */
    const char *p;
    size_t      n;

    /* waitkey: read 目标缓冲 (懒分配 TF_WAIT_BUFSIZE, tf_free 释放);
       数据经 tf_feed 走标准 chunk 通道, 非独立输入源 */
    tf_WaitCtx wait;
}
```

**tf_WaitCtx**（waitkey 的 read 上下文, 公开——tf_State 逻辑私有但
结构可见, wait 字段不得不如此）:

```c
typedef struct tf_WaitCtx {
    char  *buf; /* read 目标 (懒分配 TF_WAIT_BUFSIZE) */
    size_t len; /* 已读入待交付字节 (waitkey 维护, 用户视角恒 0) */
} tf_WaitCtx;
```

**注**: `buf[TF_MAX_BUFLEN]` 是唯一违背"input zero-copy"的缓冲，且**无法避免**:

- flush 重播需要 CSI 参数区的**原始字节**——解析后的值
  无法重建（"1;02" vs "1;2"、空参数、子参数结构都会丢失）
- 替代方案均不可行: ① 解析值序列化重播 → 丢前导零，与 Nvim
  行为不兼容; ② flush 丢弃参数 → 直接丢数据; ③ 指针引用 reader
  chunk → chunk 生命周期不可控，跨 chunk 部分序列悬垂
- 因此部分序列的原始字节**必须保留**（Nvim 的 buffer 模型等价物，
  量级上限 64B ≈ Nvim 的 16 参数上限）
- **设计原则: 状态 = 序列类型, buf_len = 进度**——单一数据源，
  无"状态编码进度"的展开枚举; CSI 解析延迟到 final（Nvim
  parse_csi+interpret 模型），不需要累加器/参数数组等旁路状态

`tf_Alloc` 签名同 `cg_Allocf`:
`void *tf_Alloc(void *ud, void *p, size_t osize, size_t nsize)`。

### 3.2 DSA 状态机

**状态枚举**:

```c
enum {
    TF_STATE_IDLE,           /* 必须为 0 */

    /* 注: 回放不再编码进 state——残余字节在 S->replay（buf 尾部）,
     * 经 DSA 重解析为新输入 (见 §2.3) */

    TF_STATE_ESCAPE,
    TF_STATE_CSI,          /* 参数区在 buf, 进度 = buf_len (final 在 buf 尾) */
    TF_STATE_SS3,

    /* CS: DCS/OSC/APC */
    TF_STATE_CS_DCS,
    TF_STATE_CS_OSC,
    TF_STATE_CS_APC,

    /* UTF-8: buf[0..buf_len) = lead + 已收续字节, 还需 = len - buf_len */
    TF_STATE_UTF8,

    /* X10 mouse: buf[0..buf_len) = 已收 raw, 还需 = 3 - buf_len */
    TF_STATE_MOUSE_X10,
};
```

**设计原则: 状态只表示序列类型，进度一律由 `buf_len` 描述**——没有
"还需 n 字节"的展开枚举（UTF8_5..1 / MOUSE_X10_3..1 均删除），
跨 chunk 续读语义统一为"读字节进 `buf[buf_len++]` 直到完成"。

**ESCAPE 态的重复 ESC**: ESCAPE 态收到 `\e` → **留在 ESCAPE 态并置
`pending_mod=TF_MOD_ALT`**（模拟 Nvim 剥皮: 每剥一层 \e 记 ALT，n≥2 的 \e 全吃）。
`pending_mod` 随状态传递——进入 CSI/SS3/CS/UTF8/X10 后，输出 key 的
`modifiers |= TF_MOD_ALT`。flush 时 ESCAPE+pending_mod → ALT+ESC。

**C0 控制码处理** (IDLE 状态, 经 emit → canonicalise):

| byte | 默认行为 | TF_FLAG_KEEPC0 |
|------|---------|-------------------|
| 0x00 | (Ctrl+Space/Ctrl+@) → KEYSYM SPACE + CTRL | 同 |
| 0x01-0x1a | Ctrl+A..Z: UNICODE(小写字母) + CTRL | UNICODE(byte) |
| 0x08 | Ctrl+H → UNICODE('h') + CTRL | UNICODE(0x08) |
| 0x09 | Tab → `TF_SYM_TAB` | UNICODE(0x09) |
| 0x0d | Enter → `TF_SYM_ENTER` | UNICODE(0x0d) |
| 0x1b | → `TF_STATE_ESCAPE` (进入转义处理，非直接输出) | 同 |
| 0x1c-0x1f | Ctrl+\/]/^/_ → UNICODE + CTRL | UNICODE(byte) |
| 0x7f | → KEYSYM DEL（DELBS flag → BACKSPACE） | 同（DELBS 仍生效） |

**注**: KEEPC0 只豁免 0x00-0x1F（C0 控制码）。0x7f 不在豁免范围——
DELBS/SPACESYMBOL 分支无条件生效（对齐 Nvim KEEPC0：KEEPC0 只影响
0x00-0x1f，0x7f 恒经 DELBS → BACKSPACE）。

**Alt+byte 逻辑**: ESCAPE 态收到非 CSI/SS3/CS 前缀字节 b → b 经
canonicalise 输出（同 IDLE 单字节路径）+ `TF_MOD_ALT`，回 IDLE
（Nvim 剥皮模型 = 递归 peekkey→canonicalise，无 pushback）。
例: `\e` + `x` → UNICODE(0x78) + ALT；`\e` + Tab → TAB + ALT；
`\e` + 0x01 → CTRL+'a' + ALT；`\e` + 0x7f → DEL + ALT。
若 `pending_mod` 已置位（前有 `\e\e`），输出继续合并 ALT（同一位）。

**Alt+UTF-8**: ESCAPE 态收到 UTF-8 lead (0xC0-0xFF) → 进入 UTF8 态
（`pending_mod` 保留，解码输出合并 ALT）。一气读完路径同 IDLE
（当前 chunk 剩余够续字节 → 直接解码），但输出 + ALT；跨 chunk
拷贝 lead 进 `buf[0]`、`buf_len=1`。

**UTF-8 多字节序列**: IDLE 收到 lead byte b（0xC0-0xFF）:
- **一气读完**: 当前 chunk 剩余 ≥ len-1 字节 → 直接从 chunk 读续字节
  解码输出（zero-copy，不设状态不写 buf）
- **跨 chunk**: 拷贝 b 进 `buf[0]`，`buf_len=1`，state=UTF8；
  续读 = 每次读 1 字节进 `buf[buf_len++]`，`buf_len == tfU_utf8len(buf[0])`
  → 从 buf 解码输出
- 非法（overlong/surrogate/非法续字节）→ UNICODE(0xFFFD)（对齐 Nvim）

**X10 mouse raw bytes**: CSI dispatch 中 `case 'M'` + `tfM_dispatch`
 的 `tfM_args(S, v) < 3`（字段数不足 3 = X10；`\e[M` 时 buf 仅含 final 'M'）。
DSA 进入 MOUSE_X10:
- **一气读完**: 当前 chunk 剩余 ≥ 3 字节 → 直接从 chunk 读 3 raw 解码
- **跨 chunk**: 进入时 `buf[0] = 'M'`（flush 重播即含 "M"+raw，无需
  事后前插），raw 逐字节进 `buf[buf_len++]`（buf[1..3]），
  `buf_len == 4` → 解码输出:
  `code = buf[1]-0x20`, `key.modifiers = (code & 0x1c) >> 2`
  （bits 3-5 → SHIFT/ALT/CTRL 位图）, `key.mouse.btn = code & ~0x1c`,
  col/line = buf[2..3]-0x20。col/line 无修饰位。

### 3.3 CSI/SS3 DSA 解析与 Dispatch

CSI 序列是通用的: 参数中间字节 (0x30-0x3F, 0x20-0x2F) + final byte (0x40-0x7E)。
**延迟解析模型**（Nvim `parse_csi`+`interpret` 同构）: DSA 在 CSI_WAIT
状态中只把字节追加进 `buf`，**final byte 出现时一次性从 buf 解析**
（参数/initial/intermediate/子参数），再 `switch (cmd)` 分发——
无累加器、无注册表、无函数指针。

**DSA 状态机** (CSI 处理细节，对齐 Nvim `parse_csi` 行为):

```
CSI_WAIT (进入时 buf_len=0):
  byte:
    任意        → 追加进 buf[buf_len++] (final 也进 buf——buf 尾即 final,
                  dispatch 与 tf_csi 都从 buf 尾取)
    参数区超限    → 非 final 字节上限 TF_MAX_BUFLEN-1 (预留 final 位);
                  超限字节进预留位后序列放弃: 剥皮 ALT+[ +
                  replay(buf) 重解析 (n=64 时尾部搬移为原地 no-op;
                  S->n 不碰——chunk 剩余保持, 重播完主循环直接接续)
    字节耗尽 → AGAIN (buf 保留)
```

**final 时从 buf 解析**（对齐 Nvim `parse_csi`）:
- final: buf 尾字节 (0x40-0x7E)
- initial: buf[0] 若在 0x3C-0x3F → `cmd |= buf[0] << 8`（只认首字节）
- 参数: 从 buf 扫描, ';' 分字段, ':' 分子字段; 字段内遇 0x20-0x2F
  (intermediate) → `cmd |= b << 16` 并**停止解析**（其后字节忽略）
- 参数值解析: **前导非数字跳过**（rxvt `\e[M5;6;7` 的 final 'M' 在
  buf[0]，字段 "M5" → 5）；**数字后遇非数字停止**（`2<3` → 2，对齐
  Nvim）
- 空参数 = -1（Nvim 的 NULL param 语义）

**buf 生命周期**: 进入 CSI/UTF8/X10 态时 `buf_len = 0`（新序列覆盖）;
dispatch 输出 key 后 **buf 保留**（`tf_csi` 读取窗口）; flush/超限时
buf 内容搬到尾部置 replay 后 `buf_len = 0`。

**SS3 处理** (`\eO`, 对齐 Nvim `peekkey_ss3`):

```
SS3_WAIT (state after ESC + 'O'):
  byte:
    0x40-0x7e → final byte, 按 SS3 表 dispatch
    other     → 非法 (Nvim 不支持 SS3 intermediate): 输出 ALT+O,
                该字节经 REPLAY 重播 (模拟 Nvim 的"剩余保留")
```

**CSI Dispatch 表** (`initial` 无时 = 0; cmd = final | initial<<8 | intermediate<<16):

```
switch (cmd) {

/* ─── CSI final bytes (0x40-0x7e) ─── */
/* 各 case 用迭代器消费参数: p = S->buf; 按需 tfC_nextarg(S, &p, &len) */

case 'A': case 'B': case 'C': case 'D':
case 'E': case 'F': case 'H':          /* \e[A..F, \e[H 光标/编辑键 */
    tfC_cursorkey(key, cmd);
    break;

case 'Z':                              /* \e[Z — Shift-Tab, 可带 \e[1;2Z */
    nextarg ×2;                        /* f2 → mods (缺 → 0) */
    key.type = TF_TYPE_KEYSYM;
    key.sym  = TF_SYM_TAB;
    key.modifiers = TF_MOD_SHIFT | tfC_mods(mods);
    break;

case '~':                              /* \e[N~ 或 \e[N;M~ — 功能键/编辑键 */
    tfC_funckey(S, key);
    break;

case 'u':                              /* \e[codepoint[:alt][;modifiers[:event]][;text]u */
    tfC_kitty(S, key);
    break;

case 'M':                              /* \e[M — mouse (X10 或 rxvt) */
    tfM_dispatch(S, key);              /* tfM_args < 3 → X10 */
    break;

case 'M' | ('<' << 8):  tfM_csi(S, key, 0); break;   /* SGR <M */
case 'm' | ('<' << 8):  tfM_csi(S, key, 1); break;   /* SGR <m = release */

case 'R' | ('?' << 8): /* \e[?N;MR — 光标位置报告 (CPR) */
    nextarg → line;  nextarg → col;    /* 缺 → 0; f1 恒存在 (initial 后) */
    key.type = TF_TYPE_POSITION;
    key.pos.line = ...;  key.pos.col = ...;
    break;
case 'R':             /* \e[R — plain R 是 F3 (Nvim 行为) */
    key.type = TF_TYPE_FUNCTION; key.number = 3;
    break;

case 'y' | ('$' << 16):                /* \e[N$y — ANSI mode report */
case 'y' | ('?' << 8) | ('$' << 16):   /* \e[?N$y — DEC mode report */
    nextarg → mode;  nextarg → value;  /* 缺 → -1 */
    key.type = TF_TYPE_MODEREPORT;
    key.modereport.initial = (cmd 带 '?' ? '?' : 0);
    key.modereport.mode  = ...;  key.modereport.value = ...;
    break;

case 'u' | ('?' << 8): /* \e[?u — kitty 协商响应 (§10.4) */
    nextarg → flags;                   /* 缺 → -1 */
    key.type = TF_TYPE_KITTYREPORT;
    key.number = ...;
    break;

/* ─── 其他 CSI final bytes ─── */

default:
    key.type = TF_TYPE_UNKNOWN_CSI;
    if (tfC_cursorkey(key, cmd)) {      /* buf 保留, 调用方经 tf_csi 获取 */
        nextarg ×2 → f2 mods + sub(ev);
        tfD_event(key, mods, ev);
    }
    break;
}
```

**参数解析: 迭代器 `tfC_nextarg`**（从 `S->buf` 解析, Nvim
`interpret_csi_param` 同构）。**设计原则: 参数区是一次性消费的序列——
按索引重扫（`arg(1)` `arg(2)` `arg(3)` 逐个从头扫）是 O(n²) 陷阱，
API 形态上杜绝**。handler 用"默认值先行 + 逐字段取值"模式:

```
const char *f = NULL;                /* 游标: 字段头 + 长度 */
int len, v1 = 0, v2 = 0;
if (tfC_nextarg(S, &f, &len)) v1 = tfC_fieldval(f, len, 0);
if (tfC_nextarg(S, &f, &len)) v2 = tfC_fieldval(f, len, 0);
```

```
tfC_nextarg(S, &f, &len)
   /* 迭代器: f/len 构成 in/out 游标 (NULL 起步); 内部用已知长度
      跳过当前字段 (';' 跳过, initial/intermediate 处理内建)。
      返回 1 = 下一字段已写入; 0 = 无更多 (参数区结束/intermediate),
      此时 f = 参数区尾, 再调用幂等返回 0 (不会重启) */
tfC_fieldval(f, len, dflt)  /* 字段内十进制 (规则见上节) */
tfC_subval(f, len, dflt)    /* 字段首个子参数 (':' 后), 无 → dflt */
tfC_mods(int m)             /* 传输值 (1+位掩码) → tf_Mod 位图 */
```

**结构保证**: 纯 final / initial+final 的 case（funckey、kitty、
kittyreport、CPR）中，字段 1 恒存在——final 字节在 buf 尾构成首个
字段（或 initial 后首个字节），故 f1 取值直接 `tfC_fieldval(f, ...)`
无需 NULL 检查。带 intermediate 的 case（modereport `\e[1$y`）则
字段可为 NULL，须检查。字段缺失/空字段 → dflt（各 case 的 dflt:
鼠标/CPR/Z = 0（无参数是合法缺省）; funckey/kitty/modereport/
kittyreport = -1（无参数是错误））。X10 判定用 `tfM_args(S, v) < 3`
（返回值取代旧 `tfC_nargs`）。`tf_csi` 的 `tfC_parseargs` 同用
迭代器填 args[]。

**modifiers 与 event 的统一解析** (Nvim `handle_csi_ss3_full`/`handle_csifunc`
行为): 字段 2 主值 = 传输值 (1+位掩码)，子参数 = event type。统一由
`tfD_event(key, mods, ev)` 应用:

```
tfD_event(key, mods, ev):
   key.modifiers = tfC_mods(mods);   /* 1+位掩码 → 位图; CSI ~ / 箭头键
      terminfo 序号同构; kitty = 1+kitty 位掩码 (tf_Mod 布局与 kitty
      位序一致, 直接映射) */
   if (ev == 1 || ev == 0) key.event = PRESS;  /* 显式 1 或缺省 = press */
   else if (ev == 2) key.event = REPEAT;
   else if (ev == 3) key.event = RELEASE;
   else → 整个序列回退 UNKNOWN_CSI (Nvim 行为: 仅 1/2/3 合法)
```

**tfC_funckey** 内部 (`\e[N~`): 迭代器顺序取 f1=n, f2=(mods,ev), f3=code:
```
f = NULL;
tfC_nextarg(S, &f, &len);          /* f1: 恒存在 (结构保证) */
n = tfC_fieldval(f, len, -1);
   if (n < 0) → UNKNOWN_CSI
if (tfC_nextarg(S, &f, &len))      /* f2: mods + ev (缺 → 默认 0) */
    mods = ..., ev = tfC_subval(...);
if (tfC_nextarg(S, &f, &len))      /* f3: code (缺 → 默认 -1) */
    code = ...;
switch (n) {
case 1: FIND; 2: INSERT; 3: DELETE; 4: SELECT;
case 5: PAGEUP; 6: PAGEDOWN; 7: HOME; 8: END;
case 11: F1; 12: F2; 13: F3; 14: F4; 15: F5;
case 17: F6; 18: F7; 19: F8; 20: F9; 21: F10;
case 23: F11; 24: F12; 25: F13; 26: F14; 28: F15;
case 29: F16; 31: F17; 32: F18; 33: F19; 34: F20;
case 27: /* xterm modifyOtherKeys: \e[27;mods;code~ */
    code = v[2];
    if (code >= 0) → UNICODE(code) + modifiers(tfC_mods(v[1]))
    else → UNKNOWN_CSI
default: → TF_TYPE_FUNCTION, number = n
}
/* modifiers / event 按上节统一规则: tfD_event(key, mods, ev) */
```

**注**: fkeymap 表沿袭 libtermkey 原表（含 16/22/30 格的怪值——n=16→F16、
n=22→F22、n=30→F30），其中 n=27 格（F16 处）永不使用——n==27 提前走
modifyOtherKeys 分支。行为兼容保留。

SS3 dispatch (`\eO` + 1 字节, 对齐 Nvim `peekkey_ss3` + kpalt):

```
switch (cmd) {
case 'A': case 'B': case 'C': case 'D':   /* UP/DOWN/RIGHT/LEFT */
    tfC_cursorkey(key, cmd);
    break;
case 'H': key.sym = TF_SYM_HOME;  key.type = TF_TYPE_KEYSYM; break;
case 'F': key.sym = TF_SYM_END;   key.type = TF_TYPE_KEYSYM; break;
case 'E': key.sym = TF_SYM_BEGIN; key.type = TF_TYPE_KEYSYM; break;
case 'P': case 'Q': case 'R': case 'S':   /* F1-F4 */
    key.type = TF_TYPE_FUNCTION;
    key.number = cmd - 'P' + 1;
    break;
case 'M':           key.sym = TF_SYM_KPENTER;  break;
case 'X':           key.sym = TF_SYM_KPEQUALS; break;
case 'j':           key.sym = TF_SYM_KPMULT;   break;
case 'k':           key.sym = TF_SYM_KPPLUS;   break;
case 'l':           key.sym = TF_SYM_KPCOMMA;  break;
case 'm':           key.sym = TF_SYM_KPMINUS;  break;
case 'n':           key.sym = TF_SYM_KPPERIOD; break;
case 'o':           key.sym = TF_SYM_KPDIV;    break;
case 'p': case 'q': case 'r': case 's':
case 't': case 'u': case 'v': case 'w': case 'x': case 'y':
    key.sym = TF_SYM_KP0 + (cmd - 'p');
    break;
default:            /* 未知 SS3 (如 'G'): Nvim 剥皮 → ALT+O + 该字节
                       存 buf 尾部经 replay 重解析 (单字节正常 canonicalise;
                       S->n 不碰——chunk 剩余保持, 重播完主循环接续) */
}
/* CONVERTKP flag: KP 键转普通键 (kpalt: 'X'→'=', 'j'→'*', 'k'→'+',
   'l'→',', 'm'→'-', 'n'→'.', 'o'→'/', 'p'-'y'→'0'-'9'; KPENTER 无 alt) */
```

**cmd 拼接**: final 字节 | initial(0x3C-0x3F, 仅 buf[0])<<8 |
intermediate(0x20-0x2F, 仅一个)<<16——**无 initial 时位为 0**（不用
哨兵，纯 final 字节与带 initial 组合天然不冲突）。initial/intermediate
与参数同在 buf 中，final 时一次性解析。

### 3.4 Control String DSA

`\eP` → `TF_STATE_CS_DCS`, `\e]` → `TF_STATE_CS_OSC`, `\e_` → `TF_STATE_CS_APC`。

```
CS_WAIT (进入时 cs_len=0 —— 每次进入 CS 态重置，避免多个 CS 内容拼接):
  首次追加时懒分配 cs_buf(64, realloc 倍增), cs_len=0
  每字节:
    BEL (0x07)            → 终止输出 key
    ST (0x9c)             → 终止输出 key
    '\' (0x5c) 且上一字节是 \e → ST: cs_len -= 1, 终止输出 key
      (即 \e 照常追加进 cs_buf, ST 检测用"末尾字节是 \e"判断——
       当前 \ 从未进 cs_buf, 只需撤掉 \e; 无需 pending 子状态;
       Nvim 的 scan 行为等价)
    其他                  → 追加到 cs_buf (realloc 扩展)
   字节耗尽 → AGAIN (cs_buf 保留内容)
```

字符串在 parser 内存中，下次 `tf_readkey` 或 `tf_free` 时释放。

### 3.5 Terminfo 策略

**核心原则: TI 优先于 CSI，两者并行加载**（同 Nvim driver 顺序）。
CSI 覆盖的键仍加载 TI——非标准终端的 terminfo 条目可能不同
（如某些终端 `key_dc = "\e[3;6~"` 而非 `"\e[3~"`）。
trie 先找到就输出，找不到 fall through 到 CSI。

terminfo 仅补充 CSI 无法处理的键。不区分"driver"——TI 逻辑直接
内联在 DSA 的 ESCAPE 分支中，无需 driver 抽象层。

**terminfo入口**: 通过 `tf_Lookup` 回调注入键序列:

```c
typedef const char *tf_Lookup(void *ud, const char *name);
```

返回 terminfo 条目的序列字符串（**通常含 `\e` 前缀**，如 `"\e[24~"`，
但**不保证**——如 `key_backspace = "\x7f"` 是单字节）。返回 NULL 表示
无此条目。调用方可用 unibilium、curses 或自定义查表实现。
trie 支持任意首字节（§3.6）。

**加载时机**: `tf_load` 调用时遍历所有支持的 terminfo 键名，
将序列插入 trie。`tf_init` 时不加载。回调仅在加载期间被调用，
**不保留**——`tf_State` 不持有 lookup 引用，加载完成后回调即可释放。

**支持的 terminfo 条目**（基于 Neovim 保留集 + F 键）:

| terminfo 名 | tf_Sym | CSI 有? | 说明 |
|------------|--------|:------:|------|
| `key_backspace` | `TF_SYM_BACKSPACE` | — | stty erase 可能覆盖 |
| `key_beg` | `TF_SYM_BEGIN` | ✓(CSI E) | 某些终端用非标准序列 |
| `key_btab` | `TF_SYM_TAB` (+SHIFT) | ✓(CSI Z) | Shift-Tab |
| `key_clear` | `TF_SYM_CLEAR` | — | **仅 TI** |
| `key_dc` | `TF_SYM_DELETE` | ✓(CSI 3~) | Delete 键 |
| `key_end` | `TF_SYM_END` | ✓(CSI F/8~) | End 键 |
| `key_find` | `TF_SYM_FIND` | ✓(CSI 1~) | Find 键 |
| `key_home` | `TF_SYM_HOME` | ✓(CSI H/7~) | Home 键 |
| `key_ic` | `TF_SYM_INSERT` | ✓(CSI 2~) | Insert 键 |
| `key_left` | `TF_SYM_LEFT` | ✓(CSI D) | 左箭头 |
| `key_npage` | `TF_SYM_PAGEDOWN` | ✓(CSI 6~) | Page Down |
| `key_ppage` | `TF_SYM_PAGEUP` | ✓(CSI 5~) | Page Up |
| `key_right` | `TF_SYM_RIGHT` | ✓(CSI C) | 右箭头 |
| `key_select` | `TF_SYM_SELECT` | ✓(CSI 4~) | Select 键 |
| `key_suspend` | `TF_SYM_SUSPEND` | — | **仅 TI** |
| `key_undo` | `TF_SYM_UNDO` | — | **仅 TI** |
| `key_sbackspace` | `TF_SYM_BACKSPACE` (+SHIFT) | — | Shift-Backspace |
| `key_sdc` | `TF_SYM_DELETE` (+SHIFT) | — | Shift-Delete |
| `key_send` | `TF_SYM_END` (+SHIFT) | — | Shift-End |
| `key_shome` | `TF_SYM_HOME` (+SHIFT) | — | Shift-Home |
| `key_sic` | `TF_SYM_INSERT` (+SHIFT) | — | Shift-Insert |
| `key_sleft` | `TF_SYM_LEFT` (+SHIFT) | — | Shift-Left |
| `key_snext` | `TF_SYM_PAGEDOWN` (+SHIFT) | — | Shift-PgDn (别名) |
| `key_sprevious` | `TF_SYM_PAGEUP` (+SHIFT) | — | Shift-PgUp (别名) |
| `key_sright` | `TF_SYM_RIGHT` (+SHIFT) | — | Shift-Right |
| `key_f1`~`key_f20` | `TF_TYPE_FUNCTION` 1~20 | ✓(CSI ~) | F1-F20 |
| `key_f21`~`key_f63` | `TF_TYPE_FUNCTION` 21~63 | — | **仅 TI** |
| `key_mouse` | (禁用) | ✓(CSI M) | 不加载（Nvim 禁用，仅 CSI mouse） |

**注**: CSI 有的条目仍保留 TI，因为某些非标准终端的 terminfo
条目不同（如某些终端 `key_dc = "\e[3;6~"` 而非 `"\e[3~"`）。
TI 先于 CSI 匹配——trie 先找到就输出，不找就 fall through 到 CSI。

**不需要的条目**（原版有、Neovim 已注释）:
cancel, close, command, copy, down, enter, exit, help, mark,
message, move, next, open, options, previous, print, redo,
reference, refresh, replace, restart, resume, save, up

**shifted 键**: 仅保留 Neovim 中实际使用的 (btab 是 shifted,
Backspace/DC/End/Home/IC/Left/Right/Next/Previous 的 shifted 版)。
其余 shifted 不处理 (如 key_shelp 等，太罕见)。

### 3.6 Trie 结构

min-max extent map。根节点 (`root`) 首次插入时分配，插入时若
byte 出 [min,max] 则 realloc 单边扩范围（`tfT_resize`，物理容量
`size` 独立于逻辑范围，可正常扩张）。**无压缩**——trie 只扩张不
删除：每次扩张端点 slot 立即被插入字节填满，首/尾 slot 恒非空，
extent 收缩（首尾空槽扫描）在数据流上不可达，故不提供压缩路径。

**单节点结构**（ARR 判型: `sym == TF_SYM_NONE`——KEY 叶恒有非零
sym；slot 数组经指针算术紧跟结构体，`tfT_idx(n, b)` 宏访问）:

```c
#define tfT_isarr(n) ((n)->sym == (int)TF_SYM_NONE) /* ARR: 无 sym */
#define tfT_idx(n, b) ((tf_Node **)((n) + 1))[(int)(b) - (n)->min]

struct tf_Node {
    unsigned char min;  /* ARR: extent lower bound */
    unsigned char max;  /* ARR: extent upper bound */
    size_t        size; /* ARR: physical slot count (may exceed range) */
    int           sym;  /* KEY: tf_Sym / FUNCTION number; NONE = ARR */
    int           mod;  /* KEY: static modifiers */
    int           type; /* KEY: TF_TYPE_KEYSYM / TF_TYPE_FUNCTION */
};

tf_State 字段:
    struct tf_Node *root;   /* trie 根节点 */
    struct tf_Node *node;   /* 当前匹配节点, NULL=死路 */
```

**lookup**: `b` 在 `[min, min+size)` 内 → `tfT_idx(n, b)`；否则 NULL。O(1)。

**insert**: **从 `seq[0]` 开始**（trie 根节点 = 任意首字节——terminfo 键
序列不都是 `\e` 开头，如 `key_backspace = "\x7f"`；Nvim 的 trie 根
同样是全范围）——逐字节查找/创建 EXTENT 节点。序列末尾创建 KEY 节点。

**冲突语义: 先加载者胜**（无崩溃保证）:
- 后加载键的中间字节撞上已有 KEY 节点（先加载键是它的真前缀）→ 后加载
  键**跳过**（长键不可达，trie 匹配到最短完整前缀即止）
- 后加载短键的 KEY 位置已有内容（slot 被占）→ **跳过**
- `tf_load` **可重入**：重复调用先 free 旧 trie 再重建（无泄漏），
  之后加载顺序重新决定胜者；`lookup == NULL` → TF_ERRPARAM（无
  "清除"概念——回调从不被持有，trie 可随时重载）
- 标准 terminfo 键序列互不为前缀，冲突仅出现在自定义 lookup

**匹配时机**: IDLE 态每个字节都从 `root` 匹配（单字节键直接命中；
多字节键从首字节起匹配）；ESCAPE 态在 `\e` 字节时**重启 trie 到 root**
（ALT 剥皮语义：`\e\e\x7f` 中第二个 `\e` 是剥皮对象同时是新序列
"\e\x7f" 的首字节）。

**KEY 节点命中**（trie 匹配优先于 DSA，同 Nvim driver 顺序）:
输出 `key.type/sym/modifiers = type_ty/sym/modset`，**同时重置 DSA:
`S->state = IDLE, S->node = root`**——否则 DSA 残留中间态会导致
下个字节错位（Nvim 无此问题因其 driver 无状态、每次从 buffer 重解析）。

**跨 reader**: `node` 存当前匹配节点。ESC 字节 → 重置为 `root`。
trie 死路 → `node=NULL`。进入 UTF8/MOUSE_X10 子状态时亦重置
`node=NULL`（续读字节不参与 trie 匹配）。无 AC 自动机（所有键
以 `\e` 开头）。

### 3.7 核心处理循环: Trie + DSA 并行

reader 由 `tf_feed` 预先设置，`tf_readkey` 调用时逐字节处理。
`tfZ_nextbyte` 优先从 replay 源取字节（S->replay > 0），其余经 reader。

```
tf_readkey:
  /* ─── 0. 回放 (S->replay > 0): 字节经 DSA 重解析为新输入 ─── */
  nextbyte: replay > 0 → buf[TF_MAX_BUFLEN - replay], replay--
            (重解析字节走正常 trie+DSA, 含 ESCAPE/CSI/UTF8 等子状态;
             源耗尽 → 主循环直接接续 reader; S->n 不参与——chunk 剩余保持)

  /* ─── 1. 续读子状态 (跨 chunk 部分序列, 进度 = buf_len;
         进入时 node 已重置 NULL, 续字节不碰 trie) ─── */
  if state == UTF8 / MOUSE_X10:
    读 1 字节 b 进 buf[buf_len++];  (reader 无数据 → TF_AGAIN)
    UTF8:    buf_len == utf8len(buf[0]) → 解码输出, 回 IDLE
             (modifiers |= pending_mod, pending_mod = 0)
    MOUSE_X10: buf_len == 3 → 解码输出, 回 IDLE (modifiers |= pending_mod)
    否则 → 继续 (下字节或 AGAIN)

  /* ─── 2. trie + DSA 正常解析 (字节恒消费:
         拒绝路径 (CSI 超限/未知 SS3) 把当前字节存 buf 尾部置 replay——
         不消费则字节驻留 chunk 指针, 跨 readkey 持有 chunk 引用,
         违反"不持久持有 chunk"原则; replay 编码不占 S->n, chunk
         剩余保持, 重播完直接接续) ─── */
  while (true):
    b = nextbyte_from_reader();   /* 消费 */
    if (reader 无数据):
      state == IDLE → return TF_NONE     /* 干净空闲 */
      else → return TF_AGAIN             /* 部分序列, 等更多或 flush */

    trie_feed(b);  /* 内联启动/重启: IDLE 任意首字节从 root 起步,
                      ESCAPE+ESC 剥皮重启 root; 死路/none → 0 */
    if (result == KEY):
      output trie key (modifiers |= pending_mod);
      state = IDLE, node = root, pending_mod = 0;
      return TF_OK
    if (result == DEAD) → S->node = NULL

    dsa_step(b);   /* 内部按状态机推进, 含 pending_mod 传递 */
    if (DSA done):
      dispatch → output key (modifiers |= pending_mod);
      state = IDLE, pending_mod = 0;
      return TF_OK
```
tf_flush:
  if (S->state == IDLE) → *key = TF_SYM_NONE 空 key, return TF_OK
                          (replay 中亦空 key——flush 不重入)
  if (S->state == ESCAPE) → ESC key（pending_mod → ALT+ESC）
  if (S->state == CSI) → ALT+[ + replay(buf, buf_len)
  if (S->state == SS3) → ALT+O
  if (tfD_incs) → ALT+<P/]/_> + IDLE 重解析 cs_buf
  if (S->state == UTF8) → UNICODE(0xFFFD)
  if (S->state == MOUSE_X10) → ALT+[ + replay("M"+raw)
  state = IDLE, node = root, pending_mod = 0;
  return TF_OK;
```


## 四、输出: tf_Key

### 4.1 类型定义

`tf_Sym` 由 X macro `TF_SYMS(X)` 生成（枚举名 + 显示名两列，
显示名对齐 Vim 键名），header 中展开为枚举，实现区展开为名字表：

```c
#define TF_SYMS(X) \
    X(BACKSPACE, "Backspace")  X(TAB, "Tab")  X(ENTER, "Enter")  \
    X(ESCAPE, "Escape")  X(SPACE, "Space")  X(UP, "Up")  ...      \
    X(CAPSLOCK, "CapsLock")  X(SCROLLLOCK, "ScrollLock")  ...     \
    X(KP0, "KP0") ... X(KPLEFT, "kLeft") ...                      \
    X(MEDIAPLAY, "MediaPlay") ...  X(LEVEL5SHIFT, "Level5Shift")
```

sym 全集（103 项，含 NONE）：
- **C0/G0**: BACKSPACE TAB ENTER ESCAPE SPACE
- **方向键**: UP DOWN LEFT RIGHT
- **编辑键**: BEGIN FIND INSERT DELETE SELECT PAGEUP PAGEDOWN HOME END
  （DEL 与 DELETE 合并——0x7f、`\e[3~`、kitty 57349u 三路统一为
  `TF_SYM_DELETE`，对齐 Nvim K_DEL 单键语义；0x7f 在 DELBS flag 下
  仍映射 BACKSPACE）
- **terminfo-only**（Nvim 保留集）: CANCEL CLEAR CLOSE COMMAND COPY
  EXIT HELP MARK MESSAGE MOVE OPEN OPTIONS PRINT REDO REFERENCE
  REFRESH REPLACE RESTART RESUME SAVE SUSPEND UNDO
- **系统键**（kitty PUA）: CAPSLOCK SCROLLLOCK NUMLOCK PRINTSCREEN
  PAUSE MENU
- **小键盘**: KP0-KP9、KPENTER KPPLUS KPMINUS KPMULT KPDIV KPCOMMA
  KPPERIOD KPEQUALS。**显示名 = Vim 名**（`k0`-`k9`、`kPoint`、
  `kDivide`、`kMultiply`、`kMinus`、`kPlus`、`kEnter`、`kEqual`、
  `kComma`）——Nvim 核心只认 Vim 名（`<KP0>` 会被解析为 `kInsert`）；
  libtermkey 旧名（`KP0`、`KPPeriod` 等）保留在 `tf_parse` 的 alias 表
  （`tf_sym` 不查 alias，`tf_parse` 查）
- **小键盘扩展**（kitty PUA，显示名 = Vim 名）: KPLEFT KPRIGHT KPUP
  KPDOWN KPPAGEUP KPPAGEDOWN KPHOME KPEND KPINSERT KPDELETE KPORIGIN
  → "kLeft" "kRight" "kUp" "kDown" "kPageUp" "kPageDown" "kHome"
  "kEnd" "kInsert" "kDel" "kOrigin"
- **媒体键**: MEDIAPLAY MEDIAPAUSE MEDIAPLAYPAUSE MEDIAREVERSE
  MEDIASTOP MEDIAFASTFORWARD MEDIAREWIND MEDIATRACKNEXT
  MEDIATRACKPREVIOUS MEDIARECORD LOWERVOLUME RAISEVOLUME MUTEVOLUME
- **修饰键**: LEFTSHIFT LEFTCTRL LEFTALT LEFTSUPER LEFTHYPER LEFTMETA
  RIGHTSHIFT RIGHTCTRL RIGHTALT RIGHTSUPER RIGHTHYPER RIGHTMETA
  LEVEL3SHIFT LEVEL5SHIFT

**kitty PUA 键不暴露给用户**——解析层把码点映射为 sym 或
FUNCTION number（见 §10.2），`tf_Key` 中不存在 PUA 码点。
**PUA 映射覆盖所有输入路径**: CSI u（`tfC_kitty`）**和普通 UTF-8**
（`tfK_utf8` 内建映射）——kitty 级别 4 "all keys" 模式以纯文本发送
PUA 功能键，Nvim 测试直接发 PUA 码点的 UTF-8 编码。

**注**: `仅 TI` 标注的键不由 CSI 驱生产（CSI 不编码这些键）。
但 enum 中完整定义——`tf_name`/`tf_parse` 需要完整的键名空间。
Termfeed 当前 TI 加载列表（§3.5）仅覆盖了 Neovim 保留集，
枚举中其他键保留定义以备未来扩展或调用方通过 `tf_Lookup` 添加。

```c
typedef enum {
    TF_TYPE_NONE = 0,      /* 未初始化 key (memset) —— 不为 0 则被误当
                              UNICODE 输出垃圾; format 对其输出空括号 */
    TF_TYPE_UNICODE, TF_TYPE_FUNCTION, TF_TYPE_KEYSYM,
    TF_TYPE_MOUSE, TF_TYPE_POSITION, TF_TYPE_MODEREPORT,
    TF_TYPE_DCS, TF_TYPE_OSC, TF_TYPE_APC,
    TF_TYPE_KITTYREPORT,   /* \e[?u — kitty 协商响应 */
    TF_TYPE_UNKNOWN_CSI
} tf_Type;

typedef enum {
    TF_MOD_SHIFT = 1 << 0, TF_MOD_ALT = 1 << 1, TF_MOD_CTRL = 1 << 2,
    TF_MOD_SUPER = 1 << 3, /* kitty: Super/Meta/Win */
    TF_MOD_HYPER = 1 << 4, /* kitty */
    TF_MOD_META  = 1 << 5, /* kitty */
    TF_MOD_CAPS  = 1 << 6, /* kitty: CapsLock */
    TF_MOD_NUM   = 1 << 7  /* kitty: NumLock */
} tf_Mod;

typedef enum {
    TF_EVENT_UNKNOWN = 0,  /* tf_mouse 输出: code 无法解释 */
    TF_EVENT_PRESS = 1,    /* 协议层: kitty :event=1; tf_mouse: 按下 */
    TF_EVENT_REPEAT = 2,   /* 协议层: kitty :event=2 (仅键盘) */
    TF_EVENT_RELEASE = 3,  /* 协议层: kitty :event=3; tf_mouse: 释放 */
    TF_EVENT_DRAG = 4      /* tf_mouse 输出: 拖动 (code 0x20 位, 仅鼠标) */
} tf_Event;
```

MOUSE 的 `key->event` 恒为 `TF_EVENT_PRESS`（解析层不做解释）；
`tf_mouse` 输出的 ev 复用同一枚举（UNKNOWN/PRESS/DRAG/RELEASE）。
解释层与协议层共用枚举——PRESS/RELEASE 值天然一致，DRAG 为解释层
独有（键盘不会出现），REPEAT 为协议层独有（鼠标不会出现）。
**协议类 key（POSITION/MODEREPORT/KITTYREPORT）的 event 为
`TF_EVENT_PRESS`**（对齐 libtermkey：peekkey 统一设 PRESS；Nvim 的
tk_getkeys 只处理 PRESS/REPEAT，UNKNOWN 会被过滤丢弃——移植中踩坑）。

typedef struct tf_Key {
    tf_Type  type;
    tf_Event event;
    union {
        int     codepoint;
        int     number;
        tf_Sym  sym;
        struct { int btn, line, col, release; } mouse;  /* release: SGR m */
        struct { int line, col; } pos;
        struct { int initial, mode, value; } modereport;
    } d;
    tf_Mod modifiers;
    char   utf8[TF_UTF8SZ];
} tf_Key;
```

`tf_Key` 是值类型，所有字段是解析结果的完整拷贝——满足 ZIO 跨 reader 安全。

### 4.2 控制字符串访问

```c
const char *tf_string(tf_State *S, int *plen);
```

返回上次输出的 DCS/OSC/APC 字符串（kitty text 也经此返回）。
字符串在 `S->cs_buf`，生命周期到下次 `tf_readkey` 或 `tf_free`。
非 CS/text key 时返回 NULL 且 `*plen=0`。

## 五、键名与格式化

### 5.1 Sym 查找

```c
const char *tf_name(int sym);
int         tf_sym(const char *name);
```

内置静态名称表。`tf_sym` 返回 `-1` 表示无匹配。

### 5.2 格式化 (tf_format)

```c
enum {
    TF_FMT_LONGMOD     = 1 << 0,  /* Shift-A 而非 S-A */
    TF_FMT_CARETCTRL   = 1 << 1,  /* ^X 而非 C-X */
    TF_FMT_ALTISMETA   = 1 << 2,  /* M- 而非 A- */
    TF_FMT_WRAPBRACKET = 1 << 3,  /* <Escape> 包围特殊键 */
    TF_FMT_SPACEMOD    = 1 << 4,  /* M Foo 而非 M-Foo */
    TF_FMT_LOWERMOD    = 1 << 5,  /* meta 而非 Meta */
    TF_FMT_LOWERSPACE  = 1 << 6   /* page down 而非 PageDown */
};

int  tf_format(char *buf, int len, const tf_Key *key, tf_Fmt fmt);
int  tf_parse(const char *str, tf_Key *key);
```

**默认 fmt = `WRAPBRACKET | ALTISMETA`**（对齐 Nvim `TERMKEY_FORMAT_VIM`）：
`<Escape>`、`<PageUp>`、`<kLeft>` 保留大小写，修饰符输出 `M-`。
LOWERSPACE/LOWERMOD/SPACEMOD 需显式指定。

**modifier 输出顺序**（对齐 Nvim `handle_termkey_modifiers` + `handle_more_modifiers`）:
`D-`(Super) `T-`(Meta) `S-`(Shift) `A-`(Alt) `C-`(Ctrl)，如 `<D-S-C-x>`。
Hyper/CapsLock/NumLock 无 Vim 记法，不输出。

**kitty 功能键**: 由 `TF_KITTYKEYS` X macro（实现区）驱动——码点
→ sym/FUNCTION number 映射表（115 项：C0 4 + PUA 111）。kitty 键
解析后**直接输出 sym 或 FUNCTION**，PUA 码点不暴露给用户（§10.2）。
`tf_format` 对 KEYSYM 输出 sym 显示名（如 `<kLeft>`、`<Level5Shift>`），
FUNCTION 输出 `<F%d>`。
key 在 format 时查表输出 `<name>`。

**忠实输出**: format 忠实反映 key 内容（大写 codepoint + CTRL 无 SHIFT
→ `<C-L>`）。Vim 需要的显式 `S-` 补全（Nvim `forward_modified_utf8`
的 hack）与 `<lt>` 转义由调用方处理——与 Nvim 分层一致（termkey 层
也不做）。

**KITTYREPORT 格式**: `KittyReport`（flags 不经 format 输出，调用方
读 `key.number`）。

### 5.3 反解析 (tf_parse)

Vim 风格 `<C-x>` / `<S-F1>` / `<M-Left>` / `<PageUp>` 等。

```c
int  tf_parse(const char *str, tf_Key *key);
```

**无 fmt 参数**——语法固定为"Vim 兼容超集"，所有格式无歧义:
- 无尖括号时: 不解析 modifier 名，直接作为 UTF-8 字符串
- modifier 前缀（大写单字母或长名，可并列，`-`/空格分隔）:
  `C`/`Control`=Ctrl, `S`/`Shift`=Shift, `A`/`Alt`=Alt,
  `M`/`Meta`=**Alt**（Vim 传统 Meta=Alt）, `T`=**META 位**（Nvim 的
  kitty Meta 扩展）, `D`=**SUPER 位**（Nvim 的 kitty Super 扩展）
- **大小写区分 modifier/键名**: `C`/`S`/`A`/`M`/`D`/`T` 大写是 modifier，
  小写字母是键名——`<C-s>` = Ctrl+'s' 键，`<C-S-x>` = Ctrl+Shift+'x'，
  `<c-x>` 与 `<S>` 类缺键名形式解析失败（Vim 同）
- `^` 形式（CARETCTRL）: `<^X>` = Ctrl+'X'（`^` 后大写字母）
- 键名大小写不敏感、可含空格: `<PageUp>`/`<page up>`/`<Page up>` 等价
- 特殊 sym: `<Up>`, `<Down>`, `<F1>`-`<F63>`, `<Backspace>`, `<Tab>`, ...
  （含 kitty 功能键名 "Escape"/"k0" 等）
- 返回消费的字节数 (含 `>`，字符串: 消费到 `>`; 非字符串: 消费到 Vim-style 分隔符)
- 无法解析 → 返回 -1

## 六、Canonicalise

与 libtermkey 兼容的规范化。内部 `tfD_emit` 自动调用，不提供公开 API。
通过 `tf_Flag` 控制行为:

```c
typedef enum {
    TF_FLAG_KEEPC0  = 1 << 0,  /* C0 控制码不解释义 */
    TF_FLAG_CONVERTKP    = 1 << 1,  /* 小键盘转普通键 */
    TF_FLAG_SPACESYMBOL  = 1 << 2,  /* Space 作为 keysym */
    TF_FLAG_DELBS        = 1 << 3,  /* DEL (0x7f) → Backspace */
} tf_Flag;

int tf_setflag(tf_State *S, int flag);  /* 返回旧 flags */
```

规范化规则（对齐 Nvim `emit_codepoint` + `termkey_canonicalise`）:
- C0 控制码 (0x00-0x1F, KEEPC0 未设时):
  - `0x00` (Ctrl+Space/Ctrl+@) → KEYSYM `TF_SYM_SPACE` + CTRL
    **无条件**（KEEPC0 不豁免——对齐 libtermkey `emit_codepoint`：
    NUL 恒为 Ctrl-Space，Nvim 的 `<Esc><Nul>` → `<M-C-Space>` 依赖此）
  - 其他 → UNICODE(cp+0x40)；cp+0x40 为字母 (A-Z) 时改用小写
    cp+0x60（如 0x01 → 'a'）——保证 Ctrl+Shift+A 可表示为 CTRL+'a'
  - Tab/Enter 不在此列（IDLE 直接输出 `TF_SYM_TAB/ENTER`）; Escape
    进 ESCAPE 态（单 ESC 由 flush 输出 `TF_SYM_ESCAPE`）
  - KEEPC0 时: 原样 UNICODE(cp)
- `0x7f` (DEL): 默认 KEYSYM `TF_SYM_DEL`；`TF_FLAG_DELBS` 时 → `TF_SYM_BACKSPACE`
  （**只转 0x7f**；0x08 是 Ctrl+H → 'H'+CTRL，不转。**KEEPC0 不豁免
  0x7f**——对齐 Nvim KEEPC0 语义，DELBS/SPACESYMBOL 分支无条件生效）
- `TF_FLAG_SPACESYMBOL`: Space 作为 `TF_SYM_SPACE` 而非 UNICODE(0x20)
- `TF_FLAG_CONVERTKP`: 小键盘键转普通键（见 §3.3 SS3 kpalt 表）
- 非法 UTF-8（overlong / UTF-16 surrogate / 0xFFFE / 0xFFFF）→
  UNICODE(0xFFFD)（对齐 Nvim `parse_utf8` 校验）
- C1 单字节 (0x80-0x9F): reader 数据恒为 UTF-8 → 作为非法 lead byte →
  UNICODE(0xFFFD)（Nvim UTF8 模式同）
- 填充 `key->utf8` 字段

I/O 相关 flag (`NOTERMIOS`/`CTRLC`/`EINTR`/`NOSTART`) 不在 termfeed 范围。
`RAW`/`UTF8` — reader 提供的数据始终是 UTF-8。

## 七、Mouse Encoding

全部支持三种 encoding（对齐 Nvim `peekkey_mouse`/`handle_csi_m`）:

| 协议 | 识别方式 | 参数含义 |
|------|---------|---------|
| X10 | `\e[M` + 3 bytes | code, col+0x20, line+0x20，单字节编码 |
| rxvt | `\e[M` + CSI params (字段数≥3) | arg[0]=code, arg[1]=col, arg[2]=line |
| SGR | `\e[<...M/m` | arg[0]=code, arg[1]=col, arg[2]=line; m=release |

**坐标 1-based**（对齐 Nvim: "Termkey uses 1-based coordinates"，
Nvim 在 forward 层 `row--; col--` 转 0-based）。

**code 解码** (三种编码统一):
```
key.modifiers     = (code & 0x1c) >> 2;   /* bits 3-5 → SHIFT/ALT/CTRL 位图 */
key.mouse.btn     = code & ~0x1c;         /* 去除修饰位 */
key.mouse.line/col = 参数值 (X10: raw-0x20)
SGR 'm' (release) → key.mouse.release = 1
```

**tf_mouse 解释** (对齐 Nvim `termkey_interpret_mouse`, ev 输出复用 `tf_Event`):
```
code 0/1/2    → TF_EVENT_PRESS (或 TF_EVENT_DRAG, 若 code 含 0x20 拖动位), btn = code+1
code 3        → TF_EVENT_RELEASE (无按钮提示)
code 64-67    → btn 4-7 (滚轮), PRESS/DRAG
code 128-129  → btn 8-9, PRESS/DRAG
mouse.release → TF_EVENT_RELEASE
其他          → TF_EVENT_UNKNOWN
```

## 八、内部命名体系

| 前缀 | 职责 |
|------|------|
| `tfD_` | DSA 引擎: 状态转移、主循环、CS 缓冲管理 (append) |
| `tfC_` | CSI handler: 命令字分发、参数单次扫描 (scan/fscan)、modifiers/event、kitty |
| `tfS_` | SS3 handler |
| `tfM_` | Mouse: 三种编码解码、interpret_mouse |
| `tfU_` | UTF-8: tocp, utflen, emit_codepoint |
| `tfK_` | 键空间: key builders (unicode/keysym/function), format, parse |
| `tfT_` | terminfo: trie 构建/匹配/键表 |
| `tfZ_` | ZIO/reader 交互 (内部 helper) |

## 九、API 总览

```c
/* 生命周期 */
void tf_init(tf_State *S, tf_Alloc *allocf, void *alloc_ud);  /* allocf NULL → 默认 realloc */
void tf_free(tf_State *S);

/* terminfo 回调 */
int tf_load(tf_State *S, tf_Lookup *lookup, void *lookup_ud);  /* 一次性加载 trie, TF_OK/TF_ERRMEM; 回调不保留 */

/* 解析 */
void tf_feed(tf_State *S, tf_Reader *r, void *ud);  /* 设置 reader */
int  tf_readkey(tf_State *S, tf_Key *key);          /* 从 reader 读 → key */
int  tf_flush(tf_State *S, tf_Key *key);            /* 强制输出当前状态 */
int  tf_waitkey(tf_State *S, int fd, int timeout_ms, tf_Key *key);

/* 属性 */
int  tf_setflag(tf_State *S, int flag);  /* 返回旧 flags */

/* 键名/格式化/反解析 */
const char *tf_name(int sym);
int  tf_sym(const char *name);
int  tf_format(char *buf, int len, const tf_Key *key, int fmt);
int  tf_parse(const char *str, tf_Key *key);

/* 解释 */
int  tf_mouse(const tf_Key *key, int *ev, int *btn, int *line, int *col);
int  tf_position(const tf_Key *key, int *line, int *col);
int  tf_modereport(const tf_Key *key, int *init, int *mode, int *val);
int  tf_csi(tf_State *S, int args[], int nargs, int *cmd);
   /* 解析上次 UNKNOWN_CSI: cmd 从 buf 尾 (final) 拼装,
      参数单次扫描填充 (';' 主参数值, 非数字跳过, 空=-1);
      返回解析出的参数个数; 无快照/参数错 → TF_ERRPARAM;
      buf 生命周期: 到下次 readkey 进入需要 buf 的状态或 tf_free */

/* 控制字符串 */
const char *tf_string(tf_State *S, int *plen);  /* 上次 DCS/OSC/APC 内容 */
```

## 十、kitty keyboard protocol 支持

### 10.1 策略

**仅解析、不发起协商** — termfeed 不向终端发送控制序列（协商查询
由调用方完成）。但**协商响应解析内建**（`\e[?u` → TF_TYPE_KITTYREPORT，
§10.4）——其他编辑器无需自写 kitty 协商响应处理。

**解析所有级别** — parser 被动接收字节，无论终端启用哪个协议级别
都正确解析:

- 级别 0 (基础): 标准 CSI/SS3/legacy，已有
- 级别 1 (disambiguate): `\e[codepoint;modifiers u`, 已有
- 级别 2 (event types): `\e[codepoint;modifiers:event u`, 已有 (`tf_Event`)
- 级别 3 (alternate keys): codepoint 后 `:alts` 子字段, 忽略, 已有
- 级别 4 (all keys): `\e[codepoint;modifiers:event;text u`, text 解析已有

### 10.2 新特性

**序列格式**（kitty 官方，非独立参数）:
```
CSI unicode-key-code[:alts] ; modifiers[:event] ; text-codepoints u
```
- `unicode-key-code` 必需；其余字段可选
- 全部参数为十进制数字，字段 `;` 分隔，子字段 `:` 分隔
- 终结符 `u`（字节 0x75）——text 是数字串（'0'-'9' 与 ':'），
  **不含 0x40-0x7E 字节，CSI 状态机无需扩展即可解析**（对照之前的
  分析: 错误假设 text 是任意 UTF-8，已修正）
- 无 modifiers 时字段省略（默认 1 = 无修饰）

**modifiers**: 传输值 = `1 + 位掩码`（1=无修饰, 2=Shift, 3=Shift+Alt,
5=Ctrl... 即 kitty bit0=Shift, bit1=Alt, bit2=Ctrl, bit3=Super, bit4=Hyper,
bit5=Meta, bit6=CapsLock, bit7=NumLock）。**解析: `modifiers = 第2字段主值 - 1`**，
直接映射 `tf_Mod`（位序一致，零转换）。CSI `~` 的 modifiers 参数同编码
（xterm terminfo 序号 2-8 与之同构）。
**kitty handler 的字段解析基于 `buf`**（';' 分字段，':' 分子字段）——
final 时迭代器顺序取 f1=codepoint、f2=(mods,ev)、f3=text（延迟解析
模型，§3.3）。

**codepoint 恒为 unshifted（小写）**: Shift 键的语义编码在 modifiers
字段的 shift 位，**不还原大写、不做大小写规范化**（kitty 规范禁止
`CSI 65 u` 形式）。`\e[97;2u` → UNICODE('a') + SHIFT。

**event types**: modifiers 字段的 ':' 子参数（`\e[97;2:2u` = repeat）。
press=1（默认）, repeat=2, release=3。其余值 → 整个序列回退
UNKNOWN_CSI（Nvim 行为）。**注意: 不是独立第 3 参数**（原设计错误，
已修正）。

**alternate keys (级别 3)**: codepoint 后 ':' 子字段，最多两个:
`shifted-key` 与 `base-layout-key`（缺 base 时为空子字段 `97:65:`）。
termfeed 忽略之（仅解析第一个 codepoint）——级别 3 的 shifted key
语义与级别 1 的 shift 位等价。

**级别 4 text 字段**: 第 3 参数（`;text` 开头），':' 分隔的十进制
码点序列 → UTF-8 编码后**存入 `cs_buf`（复用 CS 缓冲，经 `tf_string`
访问）**——无容量截断问题。`key->utf8` 保持按键本身
（`\e[97;2;65u` → UNICODE('a')+SHIFT, text="A"）。

**功能键名称空间**: 非 Unicode 键用 PUA 57344-57454（kitty 官方码表，
对齐 Nvim `input_defs.h`）。**termfeed 解析层直接映射为 sym 或
FUNCTION number，PUA 码点不暴露给用户**（与 Nvim 的 termkey 层
输出 UNICODE 原样不同——termfeed 自含映射表 `TF_KITTYKEYS`，
Nvim 集成时 tui/input.c 的 `kitty_key_map` 分支可删除）。
`tf_parse` 支持 Vim/kitty 名（主名 + 别名表：`<Esc>`/`<CR>`/`<BS>`/
`<Del>`/`<k0>`/`<kPoint>` 等）。

**完整 modifier bitmask**: `tf_Mod` 的 8 位布局与 kitty 位序一一对应
（Shift=1<<0 ... NumLock=1<<7），无需转换。**Nvim 的 termkey 层只有
3 位 (TERMKEY_KEYMOD)，Super/Meta 由 input.c 层补充
（KEYMOD_SUPER=1<<3→`D-`, KEYMOD_META=1<<5→`T-`）——termfeed
内建全部 8 位，是刻意的扩展，位序与 Nvim 完全一致**。

### 10.3 fixterms 兼容性

fixterms 使用与 kitty CSI u **相同的 CSI 编码格式** — 冲突仅存在于
终端侧的行为约定（fixterms 默认激进，kitty 渐进协商），不影响 parser。
termfeed 自然兼容 fixterms 发送的所有字节序列。

| fixterms 编码 | termfeed 处理 |
|--------------|-------------|
| `\e[1;2A` (Shift-Up) | CSI handler, 识别 modifier arg |
| `\e[1;2~` (Shift-Insert) | CSI ~ handler, 已支持 |
| `\e[65;5u` (Ctrl+A) | CSI u handler, 与 kitty 同格式 |

无需额外代码。

### 10.4 kitty 协商支持

**协商响应解析**（`\e[?u`）: CSI dispatch 的 `'u' | ('?' << 8)` case
输出 `TF_TYPE_KITTYREPORT`，`key.number` = 响应的 flags 参数
（无参数 → -1；Nvim 的 `handle_unknown_csi` 只关心"是否响应"，
termfeed 把 flags 一并解析——其他编辑器无需自写解析）。

**Nvim 集成简化**: 删除 `input.c` 的 `handle_unknown_csi` 中
`case 'u'` 分支，改在 `tk_getkeys` 的 key 分发里加
`TF_TYPE_KITTYREPORT` 分支（设 `key_encoding = kKeyEncodingKitty`）。

**不提供协商发起**（维持"仅解析"定位）: 发送 `\e[?u` 查询序列由
调用方完成（Nvim 的 tui.c 已有）。termfeed 定位为纯解析器，输出
侧不做任何控制序列。

### 10.5 不处理的 kitty 特性

- **协商发起/协议 push** (DCS 发送): 终端控制，不在 termfeed 范围
- **smkx/rmkx**: 同，终端控制。parser 同时处理 normal mode 和 application mode
- **模态报告格式变化**: kitty 可以改变 `\e[?N$y` 的报告方式，不影响 handler

## 十一、编辑器接入指南

### 11.1 Neovim 接入

替换 `termkey_push_bytes` + `termkey_getkey` + timer force 模式。

**reader**: 薄封装 libuv buffer
```c
struct NvimRd { const char *buf; size_t len; int has_data; };

static const char *nvim_reader(void *ud, size_t *plen) {
    struct NvimRd *rd = ud;
    if (!rd->has_data) { *plen = 0; return NULL; }
    *plen = rd->len; rd->has_data = 0;
    return rd->buf;
}
```

**初始化** (替换 `termkey_new_abstract`):
```c
tf_init(&input->tfst, &nvim_alloc, NULL);
tf_setflag(&input->tfst, TF_FLAG_KEEPC0);  /* 同 Nvim KEEPC0:
                                                   C0 原样输出, forward 层转 */
tf_setflag(&input->tfst, TF_FLAG_DELBS);        /* DEL(0x7f) → Backspace */
tf_load(&input->tfst, &nvim_ti_lookup, &ti_data);
tf_feed(&input->tfst, nvim_reader, &input->rd);
```

**libuv 回调** (替换 `handle_raw_buffer` → `push_bytes`):
```c
input->rd.buf = raw_buf; input->rd.len = len; input->rd.has_data = 1;
tk_getkeys(input, false);
```

**kitty 协商响应** (简化 `handle_unknown_csi`): `\e[?u` 已由 termfeed
解析为 `TF_TYPE_KITTYREPORT`——删除 `handle_unknown_csi` 的 `case 'u'`
分支，在 `tk_getkeys` 分发处加:
```c
} else if (key.type == TF_TYPE_KITTYREPORT) {
    input->key_encoding = kKeyEncodingKitty;
}
```

**tk_getkeys** (替换 `tk_getkey` 循环):
```c
static void tk_getkeys(TermInput *input, bool force) {
    int r;
    while ((r = tf_readkey(&input->tfst, &key)) == TF_OK)
        handle_key(&key);
    if (r == TF_NONE) return;        /* IDLE + 无数据, 无需操作 */
    if (r == TF_AGAIN && !force) return; /* 部分序列, 设 timer 等超时 */
    if (r != TF_AGAIN) return;       /* error */
    /* force: 强刷当前部分序列 */
    r = tf_flush(&input->tfst, &key);
    if (r == TF_OK && key.type != TF_SYM_NONE)
        handle_key(&key);
}
```

**timer 回调**: `tinput_drain` 后调 `tk_getkeys(input, true)`。同现有逻辑。

### 11.2 Vis 接入

替换 `termkey_advisereadable` + `termkey_getkey`。

**reader** (non-blocking):
```c
struct VisRd { char buf[4096]; size_t len; int has_data; };

static const char *vis_reader(void *ud, size_t *plen) {
    struct VisRd *rd = ud;
    if (!rd->has_data) { *plen = 0; return NULL; }
    *plen = rd->len; rd->has_data = 0;
    return rd->buf;
}
```

**初始化**: 同 Neovim，`tf_feed` 设 reader，`tf_load` 加载 terminfo。
`tf_parse` 替代 `termkey_strpkey` 做键绑定解析。

**主循环** (替换 `termkey_advisereadable` + getkey 循环):
```c
while (running) {
    poll(fds, ...);
    if (fd_readable) {
        rd->len = read(fd, rd->buf, sizeof(rd->buf));
        rd->has_data = 1;
        int r;
        while ((r = tf_readkey(&tfst, &key)) == TF_OK)
            handle_key(&key);
        if (r == TF_AGAIN) continue;
        if (r == TF_NONE)  continue;
    }
}
```

## 十二、与 libtermkey 的核心区别

| | libtermkey | termfeed |
|---|---|---|
| 输入模型 | `push_bytes` 拷贝进内部 buffer | ZIO reader 直接 zero-copy 游走 |
| 状态维护 | 内部 `tk->buffer` + `hightide` 偏移 | DSA（唯一例外: `buf[64]` 部分序列缓冲，状态=类型、buf_len=进度） |
| CSI 参数 | `long arg[]` / `TermKeyCsiParam`（存 buffer 引用） | 延迟解析: buf 原始字节, final 时 `tfC_nextarg` 迭代器顺序消费（值类型，跨 reader 安全；O(n) 无重扫） |
| terminfo trie | trie 从 `tk->buffer` 逐字节匹配 | trie node 指针作为 DSA 状态，跨 reader |
| terminfo 接口 | 内置 unibilium/curses 读 terminfo 数据库 | `tf_Lookup` 回调，调用方提供任何数据源 |
| driver 抽象 | TermKeyDriver + 函数指针链 | 无——CSI/SS3/TI 直接内联在 DSA 中 |
| control string | 在 `tk->buffer` 中找 ST/BEL | 增量拷贝到 `cs_buf` |
| flush 语义 | `getkey_force` 剥皮: `\e[`→ALT+`[`, UTF-8 部分→FFFD | 同（残余字节重解析） |
| ESC 超时 | `AGAIN` + `waittime`，外围 `waitkey`+`force` | 同模型 |
| I/O | `read()`/`poll()` (原版) / libuv (Neovim) | 无 I/O，reader 适配 |
