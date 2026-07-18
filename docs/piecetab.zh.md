# piecetab.h API 参考与实现笔记

[English](piecetab.md) | **中文**

> 单头文件 C89 库，以 B+ 树维护纯字节级 piece table，支持 COW 快照 (Buffer)、
> 可变编辑态 (hole 叶)、事务 commit/rollback。前缀 `pt_`。
> 行/字符映射完全外置给 `linecache.h`（前缀 `lc_`），piecetab 不管行、不管编码——clean octet。

---

## 一、数据类型

### 错误码

| 宏            | 值  | 含义             |
| ------------- | --- | ---------------- |
| `PT_OK`       | 0   | 成功             |
| `PT_ERRPARAM` | -1  | 空指针或参数越界 |
| `PT_ERRMEM`   | -2  | 内存分配失败     |

### pt_State — 内存上下文

```c
typedef struct pt_State pt_State;
```

拥有三个对象池 (`pt_Pool`): `nodes` (`pt_Node`)、`holes` (`pt_Hole`)、`trees` (`pt_Tree`)，
以及一个内嵌哨兵 `empty` 树（零分配）。通过 `pt_Alloc` 回调获取物理内存。
`max_version` 为全局 COW 版本计数器，每分配新树根递增。

### pt_Buffer — COW 快照

```c
typedef const struct pt_Tree *pt_Buffer;
```

`pt_Buffer` 是 buffer 状态的**不可变视图**，底层为引用计数的 B+ 树 (`pt_Tree`)。
所有返回 `pt_Buffer` 的函数返回一个 **owned reference**，不再需要时调用方须 `pt_release`。
`pt_Buffer` 为 `const` 指针——不得通过 Buffer 修改树，必须经游标（`pt_Cursor`）编辑。

**哨兵**: `pt_empty()` 返回 `&S->empty`，该树引用计数 1 但**永不递减**——`pt_release` 遇 `S->empty` 直接返回 0。因此 `pt_empty` 返回的 Buffer 无需 release。

### pt_Cursor — 游标

```c
struct pt_Cursor {
    struct pt_Node **paths[PT_MAX_LEVEL]; /* 根→叶路径槽位指针 */
    struct pt_Tree  *tree;                /* 当前 Buffer（编辑态下为内部新树） */
    size_t           poff;                /* 当前 piece 内偏移 */
    size_t           off;                 /* 当前 piece 之前累计字节 */
    int              dirty;               /* 编辑态标志 (transient) */
};
```

- `paths[l]` 指向 `ptK_parent(C, l)->children` 中的某个槽位；`paths[levels]` 指向叶槽位
- 绝对偏移 = `pt_offset(C)` = `off + poff`
- **游标不持有 Buffer 的引用**——它仅借用 `pt_seek`/`pt_locate` 时传入的 Buffer。调用方须在游标使用期间保持该 Buffer 存活
- 由 `pt_seek` 构造（清 dirty）；`pt_locate`/`pt_advance` 保留 dirty

### pt_Delta

```c
typedef ptrdiff_t pt_Delta;
```

用于移动类 API 的符号字节偏移。

### pt_Alloc

```c
typedef void *pt_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
```

自定义分配器（realloc 语义）。`ptr=NULL, osize=0` 时分配新块；`nsize=0` 时释放 `ptr`。
默认 `ptS_defallocf` 封装 `realloc`，失败则 `abort()`。

---

## 二、配置宏

| 符号              | 默认  | 含义                       |
| ----------------- | ----- | -------------------------- |
| `PT_FANOUT`       | 62    | 节点最大子数（≤64）        |
| `PT_MAX_HOLESIZE` | 64    | hole 容量（字节）          |
| `PT_MAX_LEVEL`    | 16    | 最大树深（paths 数组大小） |
| `PT_PAGE_SIZE`    | 65536 | 池分配器页大小             |
| `PT_ARENA_SIZE`   | 1024  | arena 块最小容量           |
| `PT_COMPACT_RANGES` | 64  | compact 区间数组初始容量   |
| `PT_STATIC_API`   | —     | 定义后所有函数为 static    |

半满阈值 = `FANOUT/2`。`PT_FANOUT >= 4` 有静态断言（makeroom 最多需 2 空槽）。

---

## 三、公共 API

### 3.1 生命周期

```c
pt_State *pt_open(pt_Alloc *allocf, void *ud);
void      pt_reset(pt_State *S);
void      pt_close(pt_State *S);
pt_Alloc *pt_getallocf(pt_State *S, void **pud);
```

- **`pt_open`**: 创建状态对象。`allocf` 为自定义分配器，传 `NULL` 使用默认 `realloc`。
  初始化三个对象池与 `empty` 哨兵（`refc=1`）。失败返回 `NULL`（OOM）。
- **`pt_reset`**: 释放状态内所有池（含所有已分配的 node、hole、tree）。状态对象本身保留。
  所有依赖此状态的 Buffer 与游标失效。允许传入 `NULL`。
- **`pt_close`**: `pt_reset` + 释放 `pt_State` 结构。允许传入 `NULL`。
- **`pt_getallocf`**: 获取状态关联的分配器函数与用户数据（通过 `pud` 传出，可为 `NULL`）。

**约束**: 不允许多状态间共享 node/hole/tree。`pt_reset` 后所有 Buffer 与游标失效。

### 3.2 Buffer 引用计数

```c
unsigned pt_retain(pt_Buffer b);
unsigned pt_release(pt_Buffer b);
```

- **`pt_retain`**: 递增引用计数，返回新计数值。`b==NULL` 返回 0。
- **`pt_release`**: 递减引用计数。归零时递归释放私有 node + hole + arena，再沿 `from` 链
  （COW 来源链）逐级释放前一版树中仅本版本私有的节点，终止于 `S->empty` 哨兵。
  `b==NULL` 或 `b==&S->empty` 返回 0。

`from` 链机制：每个树记录其 COW fork 来源 (`tree->from`)，释放时通过该链保证
共享节点存活——只释放 `version == 本树 version` 的节点。

### 3.3 Buffer 构造与查询

```c
pt_Buffer pt_empty(pt_State *S);
pt_Buffer pt_from(pt_State *S, const char *s, size_t len);
pt_Buffer pt_compact(pt_State *S, pt_Buffer b);
unsigned  pt_version(pt_Buffer b);
size_t    pt_bytes(pt_Buffer b);
```

- **`pt_empty`**: 返回 `&S->empty`（状态内嵌哨兵，零分配、零字节、无 piece）。
  不需要 `pt_release`。`S==NULL` 返回 `NULL`。
- **`pt_from`**: 从外部内存构造单 piece buffer。**不拷贝**——仅记录指针与长度，
  调用方须保证 `s` 在 buffer 存活期间有效。`len==0` 返回空树（`bytes=0`）。
  `S==NULL` 或 `s==NULL && len>0` 返回 `NULL`。返回的 buffer 拥有一引用 (refc=1)。
- **`pt_compact`**: 产出**独立的紧凑新 Buffer**，只含 `b` 当前可达内容，
  `from = empty`（切断 COW 历史链）。
  - **internal** literal（字节位于 `b` 的 `from` 链上任一 arena 内）拷入新 Buffer
    独占的紧凑 arena；相邻 internal piece 拷贝后物理相连，自动合并为单 piece
  - **external** literal（`pt_from`/`pt_insert` 的用户内存，如大文件 mmap）
    保留原指针——绝不拷贝
  - **不内部 release** `b`；调用方随后自行 release 旧链以回收其全部内存
    （`pt_Buffer nb = pt_compact(S, b); pt_release(b);`）
  - `b->bytes==0`（含哨兵）返回 `pt_empty(S)`；OOM 返回 `NULL` 且 `b` 不受影响；
    `S==NULL`、`b==NULL` 或 `b` 属其他状态返回 `NULL`
  - 成本 O(碎片量)，与原文件大小无关——remove 掉的内容不在树里，不会被遍历
- **`pt_version`**: 返回 buffer 版本号（root 节点的 `version` 字段）。`b==NULL` 返回 0。
  版本号即创建时分配的 `++S->max_version`。
- **`pt_bytes`**: 返回 buffer 总字节数。`b==NULL` 返回 0。O(1)。

### 3.4 游标查询宏

```c
#define pt_offset(C) ((C)->off + (C)->poff)
#define pt_buffer(C) ((C)->tree)
```

- **`pt_offset`**: 游标当前绝对字节偏移。直接解引用，调用方须保证 `C` 非 NULL。
- **`pt_buffer`**: 游标当前关联的树指针。编辑态下为内部新树，否则为 `pt_seek` 时传入的 buffer。

### 3.5 游标定位与移动

```c
int pt_seek(pt_Cursor *C, pt_Buffer b, size_t off);
int pt_locate(pt_Cursor *C, size_t off);
int pt_advance(pt_Cursor *C, pt_Delta d);
```

- **`pt_seek`** — 游标构造器：绑定 buffer 并清 dirty。`memset(C,0,sizeof(pt_Cursor))` 重置游标后，
  若 `off >= b->bytes` 定位于树尾（`locend`），否则 `findleaf` 自上而下定位。返回 `PT_OK` 或 `PT_ERRPARAM`。
- **`pt_locate`** — 在已绑定的 buffer 内重定位游标。`C->off`、`C->poff` 归零后定位。
  **保留 dirty 状态**（编辑中途重定位）。返回 `PT_OK` 或 `PT_ERRPARAM`。
- **`pt_advance`** — 按字节偏移移动游标。`d>0` forward，`d<0` backward。
  - 越界自动 clamp：`<0` 时 clamp 至 offset=0，`>bytes` 时 clamp 至 offset=bytes
  - `d==0` 或空树 (`bytes==0`) 立即返回 `PT_OK`
  - **保留 dirty 状态**
  - 返回 `PT_OK` 或 `PT_ERRPARAM`

### 3.6 Piece 遍历与读取

```c
const char *pt_piece(pt_Cursor *C, size_t *plen);
const char *pt_next(pt_Cursor *C, size_t *plen);
const char *pt_prev(pt_Cursor *C, size_t *plen);
size_t      pt_read(pt_Cursor *C, char *buf, size_t len);
```

**语义: "移动后返回落脚点"** — `pt_next`/`pt_prev` 先移动游标再返回目标 piece 的数据指针。

- **`pt_piece`**: 返回当前光标所在 piece 的**剩余**数据（从 `C->poff` 起），out 参数 `plen`
  设剩余长度。`C->poff >= piece->bytes[i]`（游标越过了 piece 尾）或无树时返回 `NULL` 且
  `*plen = 0`。典型遍历惯用法: `for (p = pt_piece(c, &n); n; p = pt_next(c, &n))`。
- **`pt_next`**: 若游标在当前 piece 内部（`poff < bytes[i]`），消耗剩余字节、
  向右移动至下一 piece 开头、返回新 piece 的完整数据指针。若已在当前 piece 尾
  （`poff == bytes[i]`），直接跳到下一 piece。无下一 piece（树尾）返回 `NULL`
  且 `*plen=0`，游标留在树尾。之后 `pt_piece` 返回 `NULL`。
- **`pt_prev`**: 若游标在当前 piece 内部（`poff > 0`），向左移动至本 piece 开头、
  返回本 piece 完整数据指针。若已在 piece 开头（`poff == 0`）且不在树头，向左移动
  至前一 piece 开头并返回。已在树头 (`off==0 && poff==0`) 时返回 `NULL`
  且 `*plen=0`。
- **`pt_read`**: 从游标位置起逐 piece 拷贝 `len` 字节至 `buf`，游标随之推进。
  遇 piece 边界自动跨过。不足 `len` 时返回实际拷贝字节数。`C==NULL` 或 `buf==NULL` 返回 0。
  `pt_read` 内部调用 `pt_piece`/`pt_next` 循环，**移动游标**。

### 3.7 编辑 — Hole 语义 (copy)

```c
int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len);
```

在游标处删除 `del` 字节后插入 `s`（`len` 字节），等价于 `pt_remove + pt_append`，
但插入的数据经 **hole piece**（内部分配定容缓存并 memcpy），是**拷贝语义**。

- `len` **必须** `≤ PT_MAX_HOLESIZE`，否则返回 `PT_ERRPARAM`
- 插入前先 `pt_remove(C, del)`（事务性，见 5.2）
- 插入时优先合并尾追已有的相邻 hole：若游标所在或左侧 piece 是 hole 且容量充足
  （`ptH_fit`），直接 `memmove` 局部追加而不分裂叶——快速路径
- 否则走 `ptI_splitins` 裂叶插入新 hole
- `del==0 && len==0` 为合法 no-op
- `s==NULL && len>0` 返回 `PT_ERRPARAM`

### 3.8 编辑 — Literal 语义 (reference)

```c
int pt_insert(pt_Cursor *C, const char *s, size_t len);
int pt_append(pt_Cursor *C, const char *s, size_t len);
int pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len);
int pt_remove(pt_Cursor *C, size_t len);
```

**不拷贝输入字节** — 仅记录指针/长度。调用方须保证 `s` 在引用它的所有 buffer 存活期间有效。

- **`pt_insert`**: 在游标位置**之前**插入 `s`，游标不动。等价于 `pt_append + pt_advance(-len)`。
  `len==0` no-op；`s==NULL` 返回 `PT_ERRPARAM`。
- **`pt_append`**: 在游标位置**之后**追加 `s`，游标移至插入内容尾 (`poff=len`)。
  `len==0` no-op；`s==NULL` 返回 `PT_ERRPARAM`。
  - **零拷贝合并**: 若插入位置与相邻 literal piece 物理连续，直接扩展该 piece 的 `bytes[i]`
    而不裂叶。合并条件：`(C->poff==0, 左侧piece非hole, 指针连续)` 或
    `(C->poff==bytes[i], 当前piece非hole, 指针连续)`
- **`pt_splice`**: 先 `pt_remove(C, del)` 删 `del` 字节，再 `pt_append(C, s, len)`。
  `del==0 && (s==NULL || len==0)` 为合法 no-op。
- **`pt_remove`**: 从游标位置起删除 `len` 字节。自动 clamp `len` 至 `bytes - offset`。
  `len==0` 或游标已在树尾 (`offset >= bytes`) 时立即返回 `PT_OK`。
  - 同叶删除 → `ptD_rmleaf`（literal 中裂须 makeroom 式裂层）
  - 跨叶删除 → `ptD_rmrange`（双游标，三段法 trim→cut→stitch）
  - 游标落地：删除后游标指向原删除区间首字节位置——即被删区间之后第一个字节

### 3.9 事务

```c
pt_Buffer pt_rollback(pt_Cursor *C);
pt_Buffer pt_commit(pt_Cursor *C);
```

**游标编辑态**: 首次 `pt_edit`/`pt_insert`/`pt_append`/`pt_splice`/`pt_remove`
自动通过 `ptK_markdirty` fork 新的内部 tree（`version = ++max_version`，
`from = 旧树` 并 retain），仅游标持有，外部不可达。后续编辑继续作用于该内部 tree，
直到 commit 或 rollback。

两个函数均返回 **owned reference** 并 **detach 游标**（`C->tree = NULL`）——
用 `pt_seek` 在返回的 buffer 上重新绑定。

- **`pt_commit`**:
  - **无待提交编辑 (`!C->dirty`)**: retain 当前 buffer 一次并返回。
  - **有待提交编辑 (`C->dirty`)**: 将 hole 数据冻结至 arena（`ptC_freeze`：
    hole 内容拷入 arena 块，hole piece 被替换为 literal 指针，物理相邻
    literal 合并，树重平衡），清 dirty，返回该新 buffer。
  - 冻结过程若 OOM（arena 分配失败），返回 `NULL`：树保持合法、dirty 保留、
    游标不 detach（经 `pt_locate` 复位），可重试 commit

- **`pt_rollback`**:
  - **无待提交编辑**: retain 当前 buffer 一次并返回（同 clean commit）。
  - **有待提交编辑**: 丢弃内部 transient 树，返回编辑前的来源 buffer
    （`from`，已为调用方 retain）。**无条件安全**——返回的引用为源树续命，
    即使无人另行持有亦然。

**所有权规则小结**:
- `pt_commit` / `pt_rollback` 返回的 buffer **已为调用方持有**（owned reference），无需额外 `pt_retain`
- 若调用方原先持有编辑前的 buffer，commit 后应 release 旧 buffer（被新 buffer 替代）
- `pt_Cursor` 本身**不持有任何 buffer 引用**

### 3.10 Arena 直写

```c
char       *pt_reserve(pt_Cursor *C, size_t len);
char       *pt_scratch(pt_Cursor *C, size_t *plen);
const char *pt_literal(pt_Cursor *C, size_t len);
```

arena 是**每树独享**的块链 (`pt_Arena`)，存储冻结 literal 数据。

**典型流程**:
1. `pt_reserve(C, n)` 预留 ≥n 字节的可写缓冲区
2. 用户直接向返回指针写入数据
3. `pt_literal(C, n)` 消费刚写入的 n 字节为 literal piece（可追加入树）

- **`pt_reserve`**: 在游标当前树的 arena 中预留 ≥`len` 字节的连续可写空间。
  `len==0` 按 `PT_ARENA_SIZE` 预留。在 `current` 链中找第一个有足够空间的块；
  无则新分配。内部自动 `ptK_markdirty`（若尚未 dirty）。返回可写指针，失败返回 `NULL`。
  - 块满后移入 `full` 链，后续 reserve 可能穿过满块链寻找有空间的旧块
  - 若 `len > PT_ARENA_SIZE` 且所有块均不足，会精确分配 ≥`len` 的新块 (`ptA_alloc`)

- **`pt_scratch`**: 查询当前 arena 写入头的剩余可写空间。不分配、不 dirty。
  返回当前 `current` 块剩余空间的起始指针；若无块则返回 `NULL` 且 `*plen=0`。
  用于知道上次 `pt_reserve` 留了多少空间。

- **`pt_literal`**: 消费 scrach 空间中 `len` 字节为 literal piece。
  内部自动 `ptK_markdirty`。要求当前 `current` 块剩余 ≥ `len`，否则返回 `NULL`。
  消费后若块满则移入 `full` 链。返回 `const char*`（数据所有权移交 arena）。
  - `len==0` 返回 `NULL`
  - 返回的 `const char*` 可直接传 `pt_append`/`pt_insert` 等引用 API——
    数据已在 arena 中，树释放时随 arena 释放

---

## 四、数据结构要点

### 4.1 叶 = piece（两种）

- **literal**: 不具所有权的裸 `char*`，指向用户内存或 arena 中冻结数据。零额外开销。
  在叶容器的 `children[i]` 中存储为 `(pt_Node *)` 指针（类型双关）。
- **hole**: 编辑态可变叶，池化定容缓存:

  ```c
  typedef struct pt_Hole { char data[PT_MAX_HOLESIZE]; } pt_Hole;
  ```

  长度**不存于 hole 自身**，而是记录在父节点 `bytes[i]` 字段。由 `mask` 位图区分
  hole 与 literal。

### 4.2 pt_Node — 内节点（兼叶容器）

```c
typedef struct pt_Node {
    struct pt_Node *children[PT_FANOUT]; /* 内层=子节点指针, 叶层=数据指针 */
    size_t          bytes[PT_FANOUT];    /* 内层=子树累计字节, 叶层=piece 长 */
    pt_Mask         mask;                /* 叶层=是hole, 内层=子树含hole */
    pt_Ver          version;             /* COW 版本 vs tree root */
    unsigned short  child_count;         /* 有效子节点数 */
} pt_Node;
```

- `pt_Mask = size_t` 单字位图——`PT_FANOUT ≤ sizeof(void*)*CHAR_BIT` (64) 有静态断言
- 半满阈值 = `FANOUT/2`
- `mask` 位图用于 COW 提交冻结时快速跳过无 hole 子树

### 4.3 pt_Tree — 树 (Buffer)

```c
typedef struct pt_Tree {
    pt_Node         root;      /* 嵌入根节点（非指针），levels=0 时即为叶容器 */
    pt_State       *S;
    struct pt_Tree *from;      /* COW fork 来源，终止于 S->empty */
    pt_Arena        arena;     /* 本树 literal 数据 arena（惰性） */
    size_t          bytes;     /* 总字节 O(1) */
    unsigned        refc;      /* 引用计数 */
    unsigned short  levels;    /* 树高，0 = root 即叶容器 */
} pt_Tree;
```

- `root` 嵌入于 tree 结构中（非指针），保证 `pt_from` 建空树无需额外分配
- `from` 为 COW 生命周期链：`pt_release` 沿此链只释放版本匹配的私有节点
- `levels` 层级模型: 层号 -1=root, 0..levels-1=内节点, levels=叶容器
  - `ptK_parent(C, levels)` = 叶容器，其 `children[]` 直接存数据指针

### 4.4 对象池 (pt_Pool)

每页 `PT_PAGE_SIZE` 字节，分配固定大小的对象。三个池独立管理 `pt_Node`/`pt_Hole`/`pt_Tree`。
`ptP_reserve(n)` 保证池中至少 `n` 个可用对象（含 freelist），用于事务预分配防止编辑中途 OOM。
`ptP_ralloc` 仅从 freelist 取，`assert(freed)` 确保预分配到位。

### 4.5 pt_Arena

```c
typedef struct pt_Arena {
    pt_Block *current; /* 有空位的块链（head=活跃可写） */
    pt_Block *full;    /* 已满块链 */
} pt_Arena;
```

块链 arena（`current`=有空位链, `full`=满块链），用于在编辑或提交冻结时存储 literal 数据。
每树独享，随树释放。`pt_reserve` 在 `current` 链中寻找有足够空间的块，满块移至 `full` 链。
惰性初始化——仅当首次 `pt_reserve`/`pt_literal` 时才分配块。

---

## 五、关键算法与语义

### 5.1 COW / 事务模型

1. **Mark dirty** (`ptK_markdirty`): 首次编辑时 fork 新 tree——复制旧 tree 结构，
   设 `version = ++max_version`，`from = 旧树` 并 `pt_retain(old)` 保持来源存活。
   新 tree 仅供游标持有，外部不可达。
2. **节点 COW** (`ptK_cow`): 沿 paths 逐层检查 `node->version != root->version`，
   版本不匹配则 `ptP_ralloc` 复制节点并修复 paths 指向新节点。
3. **Beginedit** (`ptK_beginedit`): `ptP_reserve` 预分配 nodes + `ptK_markdirty`
   + 沿 paths```0..levels-1``` 全线 `ptK_cow`。
4. **Commit** (`ptC_freeze`): DFS 遍历（借内层 mask 位跳过无 hole 子树），
   将 hole 数据拷入 arena、hole piece 换为 literal 指针、释放 `pt_Hole` 对象回池。
   清 dirty，返回已冻结的 buffer。
5. **Rollback**: 丢弃内部新 tree（`pt_release`），返回已 retain 的 `from` buffer 并 detach 游标。

### 5.2 编辑事务性（ERRMEM）

编辑操作在动树**之前**预留全部可能需要的池对象
（`ptK_beginedit`/`ptP_reserve`/`ptH_reserve`）——分配失败严格
发生在任何修改之前。若 `pt_edit`/`pt_append`/`pt_insert`/
`pt_splice`/`pt_remove` 返回 `PT_ERRMEM`：
- buffer 内容**未改变**（事务性失败）
- 游标仍有效，停在原位置
- 调用方可直接重试该编辑，或继续其他编辑，照常 `pt_rollback`/`pt_commit`

### 5.3 裂叶与插入 (ptI_splitins)

`pt_edit`/`pt_append` 共用的插入路径:
1. `ptI_fillrt` — 在 `S->rt[0]` 装配新 piece（+ 原 piece 裂出的右半），needs = 1 或 2 槽
2. 叶容器有空位 → `ptN_makespace` + `ptN_copy` 直接塞入
3. 不足 → `ptI_insertrt`：自底向上寻首个非满层（全满则 `ptI_splitroot` 加深树高），
   逐层 `ptI_splitchild` 裂层，在目标层缝入剩余槽
4. `ptM_up` 分段传播度量差 (bytes) + mask 位

### 5.4 区间删除 (ptD_rmrange)

1. 右游标 `R = C + advance(len)`；`ptD_cowpaths` COW 两条路径并求分叉层 `fl`
2. 同叶 (l > levels) → `ptD_rmleaf`
3. 跨叶 → `ptD_rmrange`：
   - `trimright(L)` — 删 L 叶右侧
   - `trimleft(R)` — 删 R 叶左侧（literal 切头用指针偏移不拷贝，hole 用 memmove）
   - `cutrange` — 洋葱剥层删除 L/R 间所有子节点
   - `stitch` — `mergeleaf` 合并断口叶 → `stitchnode` 洋葱序缝回 rt → `rebalance` 收尾
   - 游标落于删除点

### 5.5 洋葱序缝合 (ptD_stitchnode)

洋葱序 `for (k=0; k<=levels; ++k)` 中 `k` 为洋葱层 (k=0=叶层, k=levels=根层)，
`kl = levels - k` 为 rt[k] 对应树中层级。
- **不动点**: 每轮首复制 (`m = min(rtcc, FANOUT-pcc)`) 填当前父节点，`ptM_up` 更新祖先度量
- **kl==0 保底**: 根层永远进入修复——无论之前各层是否已处理完毕，kl==0 处强制 foldnode → root 仅一子则缩根
- **d 的延迟生效**: 记录待修复右侧子节点数，当前轮末尾设值，**下一轮** backwardnode 才消耗

### 5.6 pt_piece 尾端空叶 (trailing-in-leaf)

删除可能导致叶容器 `p->bytes[i]==0`（非空叶，仅当前 piece 长归零）。`ptD_rmleaf` 中
`if(ptN_cc(p)==0)` 将游标定位叶中索引 0 处 (`C->paths[l] = &p->children[0]`)，
`C->off=0, C->poff=0`。`pt_piece` 因此返回 `NULL` (`poff >= bytes[i]`，因为
`bytes[0]==0`)，表示已过叶尾。

`ptD_stitch` 在 mergeleaf 前检查 `p->bytes[cc-1] == 0` 并清除该空槽。
`ptM_up(C, levels, 0)` 收尾 mask 修正。

### 5.7 零拷贝合并 (literal piece 物理连续)

`pt_append` 在以下条件直接扩展已存在的 literal piece 而无需裂叶:
- `C->poff == 0 && i>0 && !ishole(i-1) && lit[i-1]+bytes[i-1] == s` — 左侧 literal 尾邻接
- `C->poff == bytes[i] && !ishole(i) && lit[i]+bytes[i] == s` — 当前 literal 尾邻接

此机制允许 caller 使用链式分配的缓冲区逐段 `pt_append` 时产生单个物理连续的 piece。

### 5.8 压缩 (pt_compact)

两个阶段，全部内聚于这一个低频函数内：

1. **区间判定**（`ptZ_collect` / `ptZ_inranges`）：沿 `b` 的 `from` 链收集每个
   arena block 区间 `[block+1, block+1+size)` 存入可增长数组（初始容量
   `PT_COMPACT_RANGES`，几何倍增），按起址 qsort。逐 piece 二分判定：指针落在
   某区间内 = internal（搬迁），否则 = external（保留）。**不在任何地方存
   per-literal tag**——数据结构与热路径零负担。
2. **批量建树**（`ptZ_bulkbuild`，lc_scan 式，O(n)）：piece 流式填入最右叶容器
   （`ptZ_bulkleaf`），随填随合并物理相邻段；叶容器满则 `ptD_makechain` 延伸新的
   最右链（各层全满时根加深）。收尾自上而下逐层 `ptD_foldnode` + `ptD_rebalance`
   修复尾部路径的半满不变式。

internal piece 经 `pt_reserve` + `memcpy` + `pt_literal` 迁入新树 arena；连续
搬迁首尾相接，自然合并。OOM 时经 rollback 丢弃半成品 transient 并返回 `NULL`，
源 buffer 不受任何影响。

---

## 六、内部函数命名体系

| 前缀   | 全称        | 职责                               |
| ------ | ----------- | ---------------------------------- |
| `ptK_` | Kurser      | 游标导航 + 编辑态入口              |
| `ptI_` | Insert      | 插入/裂叶                          |
| `ptD_` | Delete      | 删除/平衡/缝合                     |
| `ptM_` | Mask/Metric | 位图 + 度量传播                    |
| `ptN_` | Node        | 节点运作（copy/move/remove/purge） |
| `ptH_` | Hole        | hole 操作（new/append/remove）     |
| `ptC_` | Commit      | 冻结 hole→literal                  |
| `ptA_` | Arena       | arena 分配/销毁                    |
| `ptZ_` | Zip/Compact | 压缩：区间判定 + 批量建树          |
| `ptP_` | Pool        | 池分配器                           |
| `ptS_` | State       | 默认分配器                         |

`ptM_up` 一体处理度量差 `db` + mask 逐层上推。`db==0 && mask 无变化` 时提前剪枝。
`ptM_up(C, levels, 0)` 惯用法 = 纯 mask 修正。
