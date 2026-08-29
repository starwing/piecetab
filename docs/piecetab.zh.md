# piecetab.h API 参考

**中文** | [English](piecetab.md)

> 单头文件 C89 库，通过 B+ 树维护纯字节级 piece table，支持 COW 快照
> （Buffer）、可变编辑态（hole 叶）以及事务式 commit/rollback。前缀 `pt_`。
> 行/字符映射完全外置给 `linecache.h`（前缀 `lc_`）；piecetab 不关心行、
> 编码——只处理干净的字节流。

---

## 1. 数据类型

### 错误码

| 宏             | 值   | 含义                         |
| -------------- | ---- | ---------------------------- |
| `PT_OK`        | 0    | 成功                         |
| `PT_ERRPARAM`  | -1   | 空指针或参数越界             |
| `PT_ERRMEM`    | -2   | 内存分配失败                 |

### pt_State — 内存上下文

```c
typedef struct pt_State pt_State;
```

不透明内存上下文。它持有分配器回调、所有树使用的对象池，以及全局 COW 版本
计数器。所有 buffer 和 cursor 都绑定到创建它们的 `pt_State`；节点/树不得跨
state 共享。

### pt_Buffer — COW 快照

```c
typedef const struct pt_Tree *pt_Buffer;
```

`pt_Buffer` 是 buffer 状态的**不可变视图**，底层是引用计数的 B+ 树。所有返回
`pt_Buffer` 的函数都返回**已拥有的引用**；不再需要时必须调用 `pt_release`。

**哨兵**：`pt_empty()` 返回 state 内嵌的空树，其引用计数为 1 但**永不递减**——
`pt_release` 对它立即返回 0。因此 `pt_empty` 返回的 buffer 无需 release。

### pt_Cursor — 游标

```c
struct pt_Cursor {
    struct pt_Node **paths[PT_MAX_LEVEL]; /* 根→叶路径槽指针 */
    struct pt_Tree  *tree;                /* 当前 buffer（编辑态为内部新树） */
    size_t           poff;                /* 当前 piece 内偏移 */
    size_t           off;                 /* 当前 piece 之前的累计字节数 */
    int              dirty;               /* 编辑态（transient）标志 */
};
```

- `paths[]` 保存根到叶的槽指针；`paths[levels]` 指向直接存放 piece 指针的叶容器。
- 绝对偏移 = `pt_offset(C)` = `off + poff`。
- **游标不拥有 buffer 引用**——它只借用传给 `pt_seek`/`pt_locate` 的 buffer。
  使用游标期间，调用方必须保持该 buffer 存活。
- 由 `pt_seek` 构造（清除 `dirty`）；`pt_locate`/`pt_advance` 保留 `dirty`。

### pt_Delta

```c
typedef ptrdiff_t pt_Delta;
```

移动类 API 使用的有符号字节偏移。

### pt_Alloc — 分配器

```c
typedef void *pt_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
```

realloc 语义的自定义分配器。`ptr=NULL, osize=0` 表示分配新块；`nsize=0` 表示
释放 `ptr`。默认分配器封装 `realloc`，分配失败时 `abort()`。向 `pt_open` 传
`NULL` 即可选用默认分配器。

---

## 2. 配置宏

| 符号               | 默认   | 含义                                        |
| ------------------ | ------ | ------------------------------------------- |
| `PT_FANOUT`        | 31     | 节点最大子数（≤64）                         |
| `PT_MAX_HOLESIZE`  | 64     | hole 容量（字节）                           |
| `PT_MAX_LEVEL`     | 17     | 最大树深 / 游标路径容量（见 [max_levels.zh.md](max_levels.zh.md)） |
| `PT_PAGE_SIZE`     | 65536  | 池分配器页大小                              |
| `PT_ARENA_SIZE`    | 1024   | arena 块最小容量                            |
| `PT_COMPACT_RANGES`| 64     | compact 区间数组初始容量                    |
| `PT_STATIC_API`    | —      | 定义后所有函数变为 static                   |

半满阈值 = `FANOUT/2`。`PT_FANOUT >= 4` 有静态断言（makeroom 最多需要 2 个空槽）。

---

## 3. 公共 API

### 3.1 生命周期

```c
pt_State *pt_open(pt_Alloc *allocf, void *ud);
void      pt_reset(pt_State *S);
void      pt_close(pt_State *S);
pt_Alloc *pt_getallocf(pt_State *S, void **pud);
```

- **`pt_open`**：创建 state 对象。`allocf` 为自定义分配器；传 `NULL` 使用默认
  `realloc` 包装。初始化对象池和空哨兵。失败（OOM）返回 `NULL`。
- **`pt_reset`**：释放 state 内所有池（包括所有节点、hole、树）以及所有剩余
  arena block。state 对象本身保留。所有依赖该 state 的 buffer 和 cursor 失效。
  接受 `NULL`。
- **`pt_close`**：`pt_reset` + 释放 `pt_State` 结构。接受 `NULL`。
- **`pt_getallocf`**：返回与 state 关联的分配器函数和用户数据（通过 `pud` 输出，
  `pud` 可为 `NULL`）。

**约束**：节点/hole/树不得跨 state 共享。`pt_reset` 后所有 buffer 和 cursor 失效。

### 3.2 Buffer 引用计数

```c
unsigned pt_retain(pt_Buffer b);
unsigned pt_release(pt_Buffer b);
```

- **`pt_retain`**：引用计数加一，返回新计数。`b==NULL` 返回 0。
- **`pt_release`**：引用计数减一。减到零时递归释放该树的私有节点/hole/arena，
  然后沿 COW 源链释放先前版本私有的节点，终止于 state 的空哨兵。
  `b==NULL` 或 `b==&S->empty` 返回 0。

**COW 源链**：每棵树记录它 fork 自哪棵树。释放时沿此链保证共享节点存活：只释放
版本属于当前被释放树的节点。

### 3.3 Buffer 构造与查询

```c
pt_Buffer   pt_empty(pt_State *S);
pt_Buffer   pt_from(pt_State *S, const char *s, size_t len);
pt_Buffer   pt_compact(pt_State *S, pt_Buffer b);
unsigned  pt_version(pt_Buffer b);
size_t    pt_bytes(pt_Buffer b);
```

- **`pt_empty`**：返回 state 内嵌哨兵（零分配、零字节、无 piece）。无需
  `pt_release`。`S==NULL` 返回 `NULL`。
- **`pt_from`**：从外部内存构造单 piece buffer。**不拷贝**——只记录指针和长度；
  调用方必须保证 `s` 在 buffer 生命周期内有效。`len==0` 返回空树（`bytes=0`）。
  `S==NULL` 或 `s==NULL && len>0` 返回 `NULL`。返回的 buffer 引用计数为 1。
- **`pt_compact`**：生成一个**全新的独立 buffer**，只包含 `b` 当前可达的内容，
  并切断 COW 历史链。
  - 内部 literal（位于 `b` 的 COW 源链上任一 arena 中的字节）会被拷贝到新
    buffer 自己的 compact arena；相邻内部 piece 因物理连续而合并为单 piece。
  - 外部 literal（来自 `pt_from`/`pt_insert` 的用户内存，如大块 mmap）保留原指针
    ——绝不拷贝。
  - **不会**释放 `b`；调用方之后应释放旧链以回收全部内存
    （`pt_Buffer nb = pt_compact(S, b); pt_release(b);`）。
  - `b->bytes==0`（含哨兵）返回 `pt_empty(S)`；OOM 时返回 `NULL` 且 `b` 不受影响；
    `S==NULL`、`b==NULL` 或来自其他 state 的 buffer 返回 `NULL`。
  - 成本为 O(fragments)，与原始文件大小无关——已删除内容不在树中，不会被访问。
- **`pt_version`**：返回 buffer 的版本号。`b==NULL` 返回 0。版本号是创建时从
  per-state 计数器分配的值。
- **`pt_bytes`**：返回 buffer 的总字节数。`b==NULL` 返回 0。O(1)。

### 3.4 游标查询宏

```c
#define pt_offset(C) ((C)->off + (C)->poff)
#define pt_buffer(C)   ((C)->tree)
```

- **`pt_offset`**：游标当前绝对字节偏移。直接解引用；调用方须保证 `C` 非空。
- **`pt_buffer`**：游标当前关联的树指针。编辑态时是内部新树；否则是传给
  `pt_seek` 的 buffer。

### 3.5 游标定位与移动

```c
int pt_seek(pt_Cursor *C, pt_Buffer b, size_t off);
int pt_locate(pt_Cursor *C, size_t off);
int pt_advance(pt_Cursor *C, pt_Delta d);
```

- **`pt_seek`** — 游标构造器：绑定 buffer 并清除 dirty。重置游标后，若
  `off >= b->bytes` 定位到树尾，否则自顶向下定位到目标叶。返回 `PT_OK` 或
  `PT_ERRPARAM`。
- **`pt_locate`** — 在已绑定 buffer 内重新定位。定位前清零 `C->off`/`C->poff`。
  **保留 dirty 状态**（编辑中可重新定位）。返回 `PT_OK` 或 `PT_ERRPARAM`。
- **`pt_advance`** — 按字节偏移移动游标。`d>0` 前进，`d<0` 后退。
  - 越界自动钳制：`<0` 钳到 offset=0，`>bytes` 钳到 offset=bytes。
  - `d==0` 或空树（`bytes==0`）立即返回 `PT_OK`。
  - **保留 dirty 状态**。
  - 返回 `PT_OK` 或 `PT_ERRPARAM`。

### 3.6 Piece 遍历与读取

```c
const char *pt_piece(pt_Cursor *C, size_t *plen);
const char *pt_next(pt_Cursor *C, size_t *plen);
const char *pt_prev(pt_Cursor *C, size_t *plen);
size_t      pt_read(pt_Cursor *C, char *buf, size_t len);
```

**语义：“先移动，再返回落脚点”** —— `pt_next`/`pt_prev` 先移动游标，再返回目标
piece 的数据指针。

- **`pt_piece`**：返回当前 piece 从 `C->poff` 起的**剩余**数据；出参 `plen`
  设为剩余长度。当 `C->poff >= piece->bytes[i]`（游标越过 piece 尾）、树逻辑
  字节为 0（如删除全部内容后）或不存在树时，返回 `NULL` 且 `*plen = 0`。
  典型遍历：`for (p = pt_piece(c, &n); n; p = pt_next(c, &n))`。
- **`pt_next`**：若游标在当前 piece 内部（`poff < bytes[i]`），消耗剩余字节，
  右移到下一 piece 开头，返回新 piece 的完整数据指针。若已在 piece 尾
  （`poff == bytes[i]`），直接跳到下一 piece。没有下一 piece（树尾）时返回
  `NULL` 且 `*plen=0`，游标停在树尾。
- **`pt_prev`**：若游标在当前 piece 内部（`poff > 0`），左移到 piece 开头并返回
  完整数据指针。若已在 piece 开头（`poff == 0`）且不在树头，左移到前一 piece
  开头并返回。在树头（`off==0 && poff==0`）返回 `NULL` 且 `*plen=0`。
- **`pt_read`**：从游标位置逐 piece 拷贝 `len` 字节到 `buf`，边拷边移动游标。
  自动跨越 piece 边界。可用字节不足时返回实际拷贝数。`C==NULL` 或 `buf==NULL`
  返回 0。`pt_read` 内部循环使用 `pt_piece`/`pt_next`，**会移动游标**。

### 3.7 编辑 — Hole 语义（拷贝）

```c
int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len);
```

删除游标处 `del` 字节，再插入 `s`（`len` 字节），等价于 `pt_remove + pt_append`，
但插入数据经过 **hole piece**（内部分配的固定容量缓冲，已拷贝），因此是
**拷贝语义**。

- `len` **必须** `≤ PT_MAX_HOLESIZE`，否则返回 `PT_ERRPARAM`。
- 插入前调用 `pt_remove(C, del)`（事务性，见 §4）。
- 插入优先尝试与相邻尾部 hole 合并：若当前或左侧 piece 是有足够容量的 hole，
  则局部追加而不分裂叶——快速路径。
- 否则在 B+ 树中分裂/插入新的 hole piece。
- `del==0 && len==0` 是合法 no-op。
- `s==NULL && len>0` 返回 `PT_ERRPARAM`。

### 3.8 编辑 — Literal 语义（引用）

```c
int pt_insert(pt_Cursor *C, const char *s, size_t len);
int pt_append(pt_Cursor *C, const char *s, size_t len);
int pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len);
int pt_remove(pt_Cursor *C, size_t len);
```

**不拷贝输入字节**——只记录指针/长度。调用方必须保证 `s` 在引用它的所有 buffer
生命周期内有效。

- **`pt_insert`**：在游标位置**之前**插入 `s`；游标不移动。等价于
  `pt_append + pt_advance(-len)`。`len==0` no-op；`s==NULL` 返回 `PT_ERRPARAM`。
- **`pt_append`**：在游标位置**之后**追加 `s`；游标移动到插入内容末尾
  （`poff=len`）。`len==0` no-op；`s==NULL` 返回 `PT_ERRPARAM`。
  - **零拷贝合并**：若插入点与相邻 literal piece 物理连续，直接扩展该 piece 的
    长度而不分裂叶。合并条件：左侧 literal 尾相邻，或当前 literal 尾相邻。
- **`pt_splice`**：先 `pt_remove(C, del)` 删除 `del` 字节，再
  `pt_append(C, s, len)`。`del==0 && (s==NULL || len==0)` 是合法 no-op。
- **`pt_remove`**：从游标位置删除 `len` 字节。自动将 `len` 钳制到
  `bytes - offset`。`len==0` 或游标已在树尾（`offset >= bytes`）立即返回
  `PT_OK`。
  - 同叶删除与跨叶删除均由内部处理，必要时重新平衡树。
  - 游标落点：删除后游标指向原删除起点——即删除区间后的第一个字节。

### 3.9 事务

```c
pt_Buffer pt_rollback(pt_Cursor *C);
pt_Buffer pt_commit(pt_Cursor *C);
```

**游标编辑态**：首次 `pt_edit`/`pt_insert`/`pt_append`/`pt_splice`/`pt_remove`
会自动 fork 一棵带新版本的内部新树，记录前一棵树为 COW 源并 retain 它。该内部树
只由游标持有，外部不可达。后续编辑继续在这棵内部树上进行，直到 commit 或
rollback。

两个函数都返回**已拥有的引用**并**分离游标**（`C->tree = NULL`）——如需继续，
对返回的 buffer 重新 `pt_seek`。

- **`pt_commit`**：
  - **无待提交编辑（`!C->dirty`）**：将当前 buffer retain 一次并返回。
  - **有待提交编辑（`C->dirty`）**：把 hole 数据冻结到 arena 块，将 hole piece
    替换为 literal 指针，合并物理相邻 literal，并重新平衡树。清除 dirty，返回
    新 buffer。
  - 若冻结 OOM（arena 分配失败），返回 `NULL`：树保持一致，dirty 保留，游标仍
    附着（已重新定位），可重试 commit。
- **`pt_rollback`**：
  - **无待提交编辑**：将当前 buffer retain 一次并返回（同干净 commit）。
  - **有待提交编辑**：丢弃内部 transient 树，返回 retain 过的编辑前源 buffer。
    这是无条件安全的——返回的引用即使没有其他人持有也能保持源树存活。

**所有权规则摘要**：
- `pt_commit`/`pt_rollback` 返回的 buffer **已归调用方所有**；无需额外
  `pt_retain`。
- 若调用方之前持有编辑前 buffer，commit 后应 `pt_release` 旧 buffer（被新 buffer
  取代）。
- `pt_Cursor` 本身**不拥有任何 buffer 引用**。

### 3.10 Arena 直接写入

```c
char       *pt_reserve(pt_Cursor *C, size_t len);
char       *pt_scratch(pt_Cursor *C, size_t *plen);
const char *pt_literal(pt_Cursor *C, size_t len);
```

arena 是**每棵树**的块链，用于存放冻结的 literal 数据。

**典型流程**：
1. `pt_reserve(C, n)` 预留 ≥n 字节可写空间。
2. 用户直接向返回指针写入数据。
3. `pt_literal(C, n)` 将刚写入的 n 字节作为 literal piece 消费（可追加到树中）。

- **`pt_reserve`**：在游标当前树的 arena 中预留 ≥`len` 字节连续可写空间。
  `len==0` 预留 `PT_ARENA_SIZE` 字节。在活动链中查找首个足够空间的块；没有则
  新分配一块。需要时标记树 dirty。返回可写指针，失败返回 `NULL`。
  - 满块移入单独的 full 链；后续 reserve 可能遍历 full 链寻找仍有空间的块。
  - 若 `len > PT_ARENA_SIZE` 且所有块都不够，会分配一块正好 ≥`len` 的新块。
- **`pt_scratch`**：查询当前 arena 写头剩余可写空间。不分配、不 dirty。返回当前
  块剩余空间起始指针；无块时返回 `NULL` 且 `*plen=0`。
- **`pt_literal`**：从 scratch 空间消费 `len` 字节作为 literal piece。需要时标记
  树 dirty。要求当前块剩余 ≥`len` 字节，否则返回 `NULL`。消费后若块满，移入
  full 链。返回 `const char*`（数据所有权移交 arena）。
  - `len==0` 返回 `NULL`。
  - 返回指针可直接传给 `pt_append`/`pt_insert` 等——数据已在 arena 中，随树释放。

---

## 4. 所有权、生命周期与线程

- 除 `pt_empty` 返回的哨兵外，每个 `pt_Buffer` 都是已拥有的引用。
- 游标借用其 buffer；使用游标期间保持 buffer 存活。
- `pt_reset`/`pt_close` 后，从该 state 创建的所有 buffer 和 cursor 均失效；
  剩余 arena block 也会由 state 一并回收。
- `pt_commit`/`pt_rollback` 后游标被分离；如需继续，在返回的 buffer 上重新
  `pt_seek`。
- 多个游标若从同一游标拷贝，可共享同一棵 transient 树；但树结构变化会使其他
  游标的路径指针失效。库不对此提供保护；调用方必须避免通过别名游标并发做结构
  性编辑。
- 库非线程安全：一个 state、其 buffer 和 cursor 不得在无外部同步的情况下并发
  访问。

---

## 5. 设计概览

### 5.1 Piece Table 与 B+ 树

piecetab 把字节序列存成叶节点为 **piece** 的 B+ 树。每个 piece 是：

- **literal** —— 指向用户内存或冻结 arena 数据的非拥有 `const char*`，零额外开销。
- **hole** —— 仅编辑会话期间使用的可变固定容量缓冲；内容在编辑时拷入，commit
  时冻结到 arena。

内部节点保存累计字节数，提供 O(log n) 偏移定位和 O(1) 总字节数。每个节点上的
小位图记录哪些子节点是 hole（或含 hole），让 commit 能快速跳过无 hole 子树。

### 5.2 COW 快照与临时编辑

`pt_Buffer` 不可变。编辑必须经过游标：

1. 首次编辑时，游标从 buffer 的树 fork 一棵私有 transient 树，分配新版本并
   retain 源树。
2. 只有版本与 transient 根不同的路径节点才被拷贝（copy-on-write）。
3. `pt_commit` 把 hole 冻结为 arena literal 并返回新的不可变 buffer。
4. `pt_rollback` 丢弃 transient 树并返回 retain 过的源树。

由此获得廉价快照：未修改的子树在版本间共享，源链通过引用计数保持存活。

### 5.3 事务性错误处理

编辑操作会在改动树之前预分配可能需要的所有池对象，因此分配失败严格发生在任何
修改之前。若 `pt_edit`/`pt_append`/`pt_insert`/`pt_splice`/`pt_remove` 返回
`PT_ERRMEM`：

- buffer 内容**不变**（事务性失败）。
- 游标仍在原位置有效。
- 调用方可重试该编辑、继续其他编辑，或照常 `pt_rollback`/`pt_commit`。

### 5.4 Compaction

`pt_compact` 是低频维护操作，分两个阶段：

1. **分类 piece**：遍历 buffer 的 COW 源链，收集所有 arena 块区间，再对每个源
   piece 二分判断它指向 arena（内部，需迁移）还是外部用户内存（保留）。
2. **重建**：把所有可达 piece 流入新的最右链，合并物理相邻的内部 piece，并
   平衡尾部路径。

内部 piece 通过 `pt_reserve` + `memcpy` + `pt_literal` 迁入新树 arena，连续迁移
首尾相接，自然合并。OOM 时通过 rollback 丢弃半成品 transient 并返回 `NULL`，
源 buffer 不受任何影响。

---

## 6. 延伸阅读

实现级细节、不变量和历史设计决策保存在 notes 目录：

- `notes/design_piecetab_v2.md` — 完整设计文档
- `notes/brief_piecetab.md` — 精炼实现约束与不变量
