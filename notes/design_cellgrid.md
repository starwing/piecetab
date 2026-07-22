# cellgrid 设计

> 基于 `editor.lua` Section 2 的 Lua Grid 模块经验，
> 综合 `neovim_grid_api.md` 调研，设计 C89 单头文件库。
> 前缀 `cg_`。
>
> **职责边界**：纯终端帧缓冲——存 cell、记脏、出 diff。
> UTF-8→codepoint 内置。宽度计算通过 `cg_setwcwidth` 由用户注入。
> byte↔display column 转换归编辑器层，不在 cellgrid 范围内。

## 一、定位

cellgrid 是**终端帧缓冲**——矩形 cell 网格，双重缓冲、滚动优化、
宽字符、逐 cell style，输出最小 diff（回调模式）。

**不在 cellgrid 范围内的**（归编辑器层）：
- 行号计算、tab 展开
- byte↔display column 转换（grid 只管 display column）
- 语法染色结果存储（那是 spantree 的事）
- 终端控制码生成（cellgrid 只出 diff 回调，渲染端生成 ANSI）

与 Neovim grid 的取舍：
- **取**：双重缓冲 + 全量比较 diff、滚动检测、宽字符处理
- **舍**：glyph cache、arab shaping、compositor 多层合成、
  msgpack 编码、TUI 集成——均非 C89 库职责

与 piecetab/linecache 的关系：**完全独立**。

## 二、数据结构

### 2.1 cg_Grid — 全量数组 + line_offset 旋转

```c
typedef struct cg_Grid {
    int       top, rows, cols;
    int       all_dirty, scroll;
    int       off;
    cg_Allocf  *allocf;
    void       *ud;
    cg_WcWidthf *wcwidthf;
    void       *wud;
    int        *cur_cp;
    unsigned   *cur_st;
    int        *back_cp;
    unsigned   *back_st;
} cg_Grid;
```

**无 `cg_Row` struct**——每行只是全量数组中的一段偏移。
cp/st 合为一次 `malloc`：

```c
cur_cp = allocf(ud, NULL, 0, rows*cols * (sizeof(int)+sizeof(unsigned))*2);
cur_st = (unsigned *)(cur_cp + rows*cols);
back_cp = (int *)(cur_st + rows*cols);
back_st = (unsigned *)(back_cp + rows*cols);
```

cur 和 back 在同一 alloc 中，`memset` 打底。

### 2.2 line_offset 旋转

**内部宏：**

```c
#define cgR_idx(G, r) (((G)->off + (unsigned)(r)) % (unsigned)(G)->rows)
```

`cgR_idx(G, r)` 返回逻辑行 r 在 flat array 中的行索引（0-based 物理行）。
内部函数计算 row offset：`int ro = cgR_idx(G, r) * G->cols;`，
然后 `G->cur_cp[ro + c]`、`G->cur_st[ro + c]` 访问 cell。

**为什么一个 `off` 足够**：`off` 是视口属性而非缓冲属性。
cur 和 back 处于同一视口——`cg_begin` 根据 `top - old_top` 算出 scroll 量，
对 cur 和 back **同步旋转** `off`。`cg_freeze` 不改 off。
因此 cur 和 back 的逻辑行→物理行映射始终一致，`cgR_idx` 同时适用于
cur_cp/cur_st 和 back_cp/back_st 的索引。

旋转示意（rows=5, 下滚 2 行）：

```
off=0: 逻辑行 0→物理 0, 1→1, 2→2, 3→3, 4→4
off=2: 逻辑行 0→物理 2, 1→3, 2→4, 3→0(裸露), 4→1(裸露)
```

重叠区零拷贝——仅改 `off`。裸露行 cur 重置为默认 cell（cp=0, st=0）。
`cg_freeze` 时 cur→back 全量同步，下一帧 back 自然一致。
cur 和 back 同一 `off`，diff 时同逻辑行对应相同物理偏移。

### 2.3 脏检测

**不维护独立 dirty bit array**——`cg_isdirty` 直接比较 cur vs back 的 cp/st：
```c
return cur_cp[ro+c] != back_cp[ro+c] || cur_st[ro+c] != back_st[ro+c];
```

`cg_freeze` 全量 `memcpy` cur→back，`all_dirty=0`。
`cg_diff` 同样逐 cell 比较 cur vs back（`cgD_skip`），等价语义。

理由：
- 终端尺寸典型 80×200 = 16000 cell，全量比较对现代 CPU 是瞬间完成
- cell 可能被多次 overwrite（3-pass 渲染），逐写标脏的开销未必省
- Neovim 也是全量比较方案
- 省去 dirty bit array 的分配和维护成本

## 三、公共 API

### 3.1 生命周期

```c
CG_API int  cg_init(cg_Grid *G, cg_Allocf *f, void *ud);
CG_API void cg_free(cg_Grid *G);
```

原地初始化，不返回指针。`cg_init` 零初始化 struct、记录 allocf/ud，
不分配 cell 内存。`cg_free` 释放 cell 内存后重初始化为空 grid。

与 piecetab/linecache 一致的 allocf 签名。无 `cg_State`——cellgrid 不池化、不共享。

### 3.2 帧生命周期

```c
CG_API int  cg_begin(cg_Grid *G, int top, int rows, int cols);
CG_API void cg_clear(cg_Grid *G);
```

`cg_begin(G, top, rows, cols)` 开始新帧。`rows==0 || cols==0` 返回 `CG_ERRPARAM`。

**尺寸变化（首帧或 resize）：**
- `G->rows==0`（首帧）→ 分配 cur/back 全量数组，`all_dirty=1`
- `rows != G->rows || cols != G->cols` → `cgF_resize` 保留重叠区重建，`off=0`
- 尺寸不变 → scroll 逻辑

**scroll 逻辑（尺寸不变时）：**
- `delta = G->top - top`
- `all_dirty` 或 `delta == 0`（同 top）→ 只更新 `G->top=top`, `scroll=off=0`
- `|delta| >= G->rows` → 等效全屏 redraw：`off=0`, `all_dirty=1`
- `delta > 0`（内容下滚，视口上移）→ `off = (off+delta) % rows`，裸露行重置 cur
- `delta < 0`（内容上滚，视口下移）→ `off = (off+delta+rows) % rows`，裸露行重置 cur

`G->scroll` 记录 delta 供 `cg_diff` 输出 scroll 回调。

`cg_clear(G)` 清空当前帧 cur 数据，设 `all_dirty=1`。

### 3.3 Cell 写入

```c
CG_API void cg_put(cg_Grid *G, int r, int c, int cp, unsigned st);
CG_API void cg_clearrow(cg_Grid *G, int r, int cs, int ce);
CG_API void cg_fill(cg_Grid *G, int r, int cs, int ce, int cp);
CG_API void cg_span(cg_Grid *G, int r, int cs, int ce, unsigned st);
```

`cg_put`：写单个 codepoint 到 (r,c)。宽字符（宽度=2）写 cp 到 c，c+1 设 -1
（continuation 标记），st 同时写入。行末仅剩 1 列时写 cp='>' 占位。
窄字符 overwrite 宽字符的左半/右半时自动清对侧。越界 nop。

`cg_clearrow`：清空行 r 的区间 [cs, ce) 的 cp 和 st（设为 0）。

`cg_fill`：填区间 [cs, ce) 的 cp，不改 st。用于批量填字（如 fold column）。

`cg_span`：设区间 [cs, ce) 的 st，不改 cp。越界 clamp。

写入接口正交：`cg_put` 写 cp+st，`cg_fill` 只写 cp，`cg_span` 只写 st，
`cg_clearrow` 清 cp+st。组合使用覆盖所有场景。

### 3.4 文本行写入

```c
CG_API int cg_putline(cg_Grid *G, int r, int c, const char *s, unsigned st);
```

UTF-8 字符串写入，内部解码为 codepoint 后调用 `cgF_putcp`。
跳过非法 continuation byte。返回文本末绝对列号（方便调用方知道下段起点）。
tab 展开在编辑器层预完成。

### 3.5 Diff（回调模式）

```c
CG_API int cg_diff(const cg_Grid *G, cg_Diff *D);
```

逐行逐 cell 比较 cur vs back（`cgD_skip` 跳过匹配 cell，
`cgD_rep` 统计连续相同 cp+st 的 cell 数），通过回调输出差值流：

```c
struct cg_Diff {
    int fill_min;
    int (*scroll)(cg_Diff *D, int top, int bot, int n);
    int (*move)(cg_Diff *D, int r, int c);
    int (*style)(cg_Diff *D, unsigned st);
    int (*fill)(cg_Diff *D, int n, int cp);
    int (*put)(cg_Diff *D, int cp);
    int (*finish)(cg_Diff *D);
};
```

回调语义：
- `scroll(D, top, bot, n)` — 滚动区 [top, bot]（1-based），n>0 内容上滚、n<0 下滚
- `move(D, r, c)` — 定位到逻辑行 r 列 c
- `style(D, st)` — 切换 style，st=0 表示行末重置或默认 style
- `fill(D, n, cp)` — 输出 n 个 cp（连续相同 cell ≥ fill_min 时批量输出）
- `put(D, cp)` — 输出一个 codepoint
- `finish(D)` — diff 结束

`fill_min` 阈值：连续相同 cell 数 ≥ fill_min 时用 `fill` 批量输出，
< fill_min 时逐 cell `put`。初始化时 ≤1 会复位为 `CG_DEFAULT_MINFILL`（4）。

所有回调返回 0 继续，非 0 中止并向上传播。`cg_diff` 调用序列：
1. 若有 scroll → `scroll(...)`
2. 每行：skip 匹配区 → `move(r, c)` → `style(st)`（style 变时）→ `put`/`fill` 交替
3. 每行末 → `style(0)`
4. 最后 → `finish()`

回调模式允许调用方直接消费 diff 流（生成 ANSI 控制码、构建脏区表等），
避免中间数据 struct 的信息损失。

### 3.6 帧结束

```c
CG_API void cg_freeze(cg_Grid *G);
```

cur → back 全量 `memcpy`：当前帧成为下一帧的比较基准。
`all_dirty=0`。`off` 不变。

### 3.7 Getter 接口

```c
#define cg_rows(G)  ((G) ? (G)->rows : 0)
#define cg_cols(G)  ((G) ? (G)->cols : 0)
#define cg_top(G)   ((G) ? (G)->top : 0)

CG_API int cg_cell(const cg_Grid *G, int r, int c, unsigned *st);
CG_API int cg_back(const cg_Grid *G, int r, int c, unsigned *st);
CG_API int cg_isdirty(const cg_Grid *G, int r, int c);
```

`cg_rows`/`cg_cols`/`cg_top` 为宏，G==NULL 安全返回 0。
`cg_cell`/`cg_back` 返回 cp，通过 `st` 输出 style（st=NULL 安全）。越界返回 0 且 *st=0。
`cg_isdirty` 比较 cur vs back 的 cp/st，越界返回 0。

### 3.8 宽度回调

```c
typedef int cg_WcWidthf(void *ud, int cp);
CG_API void cg_setwcwidth(cg_Grid *G, cg_WcWidthf *f, void *ud);
```

设置后所有 cell 宽度计算走该回调。未设置时默认 width=1。
每个 grid 独立持有函数指针和用户数据。

## 四、宽字符处理

宽度计算走 `cg_setwcwidth` 设置的回调。内部编码工具：`cgK_tocp`、`cgK_utflen`。

### 4.1 双宽字符行为

- `cg_put(r, c, wide, st)`：cp 写在列 c，列 c+1 写 -1（continuation，渲染时跳过）
- 窄字符写双宽**右半**（col c 处 cp=-1）→ 自动清左半（c-1）为 0
- 窄字符写双宽**左半**（col c 处 cp≠-1 且 c+1 处 cp=-1）→ 自动清右半（c+1）为 0
- 行末仅剩 1 列时写双宽 → cp='>' 占位

## 五、style 语义

`unsigned` 做不透明 ID（同 Neovim `sattr_T`），cellgrid 仅比较
相等性判 dirty。调用方可打包（32 位例）：

- bits 0-7: 前景色索引（256 色调色板）
- bits 8-15: 背景色索引
- bits 16-23: 属性标志（bold=1, italic=2, underline=4, reverse=8）
- bits 24+: 保留

或做查表索引（含 RGB 24bit 真彩色）。`0` = 默认文本，与未初始化
cell 同值——diff 时不会误标脏。

## 六、内部函数命名体系

| 前缀 | 职责 |
|------|------|
| `cgK_` | UTF-8 编码工具（tocp/utflen） |
| `cgF_` | 帧管理：initgrid/resize/putcp |
| `cgD_` | diff 工具（skip/rep/call） |
| `cgP_` | 前置条件检查（checkrc） |
| `cgR_` | ring buffer 索引（cgR_idx 宏） |
| `cgS_` | 静态/系统工具（defallocf） |

## 七、编码工具函数（`cgK_`，内部）

```c
int cgK_utflen(const char *s);          /* 首字符字节长，0=非法 */
int cgK_tocp(const char *s, int len);   /* UTF-8→codepoint */
```

## 八、边界行为

| 函数 | 越界/负值 | 零参数 | NULL |
|------|-----------|--------|------|
| `cg_init` | — | — | G=NULL→`CG_ERRPARAM` |
| `cg_free` | — | — | G=NULL→nop |
| `cg_setwcwidth` | — | — | G=NULL→nop |
| `cg_begin` | — | rows/cols==0→`CG_ERRPARAM` | — |
| `cg_clear` | — | — | G=NULL 或 rows==0→nop |
| `cg_put` | 行列越界→nop | — | — |
| `cg_clearrow` | r 越界→nop | cs>=ce→nop | — |
| `cg_fill` | r 越界→nop | cs>=ce→nop | — |
| `cg_span` | r 越界→nop，cs/ce clamp | cs>=ce→nop | — |
| `cg_putline` | 行列越界→返回 c | s=""→返回 c | s=NULL→返回 c |
| `cg_diff` | — | — | G=NULL 或 rows==0 或 D=NULL→`CG_ERRPARAM` |
| `cg_freeze` | — | — | G=NULL 或 rows==0→nop |
| `cg_cell` | 行列越界→返回 0 且 *st=0 | — | st=NULL ok |
| `cg_back` | 行列越界→返回 0 且 *st=0 | — | st=NULL ok |
| `cg_isdirty` | 行列越界→返回 0 | — | — |
