# spantree.h API 参考与实现笔记

[English](spantree.md) | **中文**

> 单头文件 C89 库，保存**最终渲染染色**的全覆盖字节 span 分区：每个字节
> 恰好属于一个 `(len > 0, sp_Id)` 段。前缀 `sp_`。与 `piecetab.h`/
> `linecache.h` 共用 B+ 树骨架与池分配风格，但只存字节长度与属性 id——
> 不存文本内容，也没有 COW 快照。

---

## 一、数据类型

### 错误码

| 宏            | 值           | 含义                       |
| ------------- | ------------ | -------------------------- |
| `SP_OK`       | 0            | 成功                       |
| `SP_ERRPARAM` | -1           | 空指针或参数越界           |
| `SP_ERRMEM`   | -2           | 内存分配失败               |
| `SP_NONE`     | `~(sp_Id)0`  | 迭代结束哨兵（永不是合法 id） |

### sp_State — 内存上下文

```c
typedef struct sp_State sp_State;
```

持有分配器回调/用户数据、一个 `sp_Node` 对象池（`sp_Pool`），以及缝合
树时使用的内嵌 scratch 节点。多棵树可共享同一 `sp_State`。

### sp_Tree — Span 树

```c
typedef struct sp_Tree sp_Tree;
```

一棵 span 树。root 内嵌于结构体，因此 `sp_newtree` 只会因树结构本身
分配失败而返回 `NULL`。树终身绑定其所属 `sp_State`。

### sp_Cursor — 游标

```c
typedef struct sp_Cursor sp_Cursor;
```

由 `sp_seek` 初始化的非持久导航器。`paths[0..SP_MAX_LEVEL]` 保存根到叶
的槽指针；`off` 是当前段之前的累计字节数，`poff` 是段内偏移。绝对字节
偏移：

```c
#define sp_offset(C) ((C)->off + (C)->poff)
```

游标**不持有**树引用；调用方须保证树在游标使用期间存活。与
`pt_Cursor`/`lc_Cursor` 一致，游标不会停留在树中段尾（唯一段尾位置是
树尾）。

### sp_Delta、sp_Id、sp_Mask

```c
typedef ptrdiff_t sp_Delta;  /* 有符号字节偏移 */
typedef size_t    sp_Id;     /* 属性 / 样式 id */
typedef size_t    sp_Mask;   /* namespace 位集；位布局对用户隐藏 */
```

- `sp_Id` 0 是合法的**未染色** id。`SP_NONE` 是迭代哨兵，绝不能存入树。
- `sp_Mask` 保存 namespace 位。位布局私有；必须通过
  `sp_addns`/`sp_delns`/`sp_hasns` 操作。合法 ns 域为
  `1..SP_MASK_BITS`，其中 `SP_MASK_BITS = sizeof(sp_Mask) * CHAR_BIT`。

### sp_Alloc — 分配器

```c
typedef void *sp_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
```

realloc 语义。`ptr=NULL, osize=0` 分配新块；`nsize=0` 释放 `ptr`。
默认 `spS_defallocf` 封装 `realloc`，失败时 `abort()`。向 `sp_open`
传 `NULL` 使用默认分配器。

### sp_Arbiterf — 混合回调

```c
typedef sp_Id sp_Arbiterf(void *ud, sp_Id id, sp_Id old, sp_Mask *mask);
```

每次样式写入/合并/删除决策都会调用。参数序为“新值在前”：`id` 是写入
值，`old` 是当前段 id，`mask` 是 in/out 的 namespace 集。

三态契约：

- `arb(ud, 0, old, &mask)` — **死亡**事件（合并/删除/trim/freetree）。
- `arb(ud, in, 0, &mask)` — **新生**事件（写空段、分裂碎片、继承）；
  必须原样返回 `in`。
- `arb(ud, in, old, &mask)` — **合并决策**；返回新 id，并将 `mask` 设为
  返回 id 的精确 ns 集（经 `sp_addns`/`sp_delns`）。

返回 id 为 0 时，树强制 mask 为 0。mask 是剪枝 namespace 迭代的优化
通道；mask 报错会导致剪枝假阴性（树不校验）。

---

## 二、配置宏

| 符号            | 默认  | 含义                               |
| --------------- | ----- | ---------------------------------- |
| `SP_FANOUT`     | 62    | 节点最大子数（必须 ≥ 4）           |
| `SP_PAGE_SIZE`  | 65536 | 池分配器页大小                     |
| `SP_MAX_LEVEL`  | 13    | 最大树深 / 游标路径数组大小（见 [max_levels.zh.md](max_levels.zh.md)） |
| `SP_STATIC_API` | —     | 定义后所有 `SP_API` 函数变为 static |

`SP_FANOUT >= 4` 由静态断言强制。

---

## 三、公共 API

### 3.1 生命周期

```c
sp_State *sp_open(sp_Alloc *allocf, void *ud);
void      sp_close(sp_State *S);
```

- **`sp_open`**：创建状态对象。`allocf == NULL` 使用默认 realloc 封装。
  OOM 返回 `NULL`。
- **`sp_close`**：释放节点池与状态结构。`S == NULL` 无操作。**不会释放
  树**——请先对每棵剩余树调用 `sp_freetree`。

### 3.2 树生命周期

```c
sp_Tree *sp_newtree(sp_State *S);
void     sp_freetree(sp_Tree *T);
size_t   sp_bytes(const sp_Tree *T);
```

- **`sp_newtree`**：创建空 span 树。`S == NULL` 或 OOM 返回 `NULL`。
- **`sp_freetree`**：清除全部节点并释放树结构。`T == NULL` 无操作。
- **`sp_bytes`**：span 覆盖的总字节数。`NULL` 返回 0。

### 3.3 混合与 Namespace

```c
void sp_setarbiter(sp_Tree *T, sp_Arbiterf *cb, void *ud);
int  sp_addns(sp_Mask *mask, int ns);
int  sp_delns(sp_Mask *mask, int ns);
int  sp_hasns(const sp_Mask *mask, int ns);
```

- **`sp_setarbiter`**：为树设置混合回调与用户数据。写入、合并、删除与
  树销毁时调用。
- **`sp_addns`** / **`sp_delns`**：在 `mask` 中加/删 namespace `ns`。
  返回 `SP_OK`；`mask` 为 `NULL` 或 `ns` 越界时返回 `SP_ERRPARAM`。
- **`sp_hasns`**：`mask` 含 `ns` 返回 1，否则 0。非法参数返回 0。

### 3.4 游标导航

```c
int  sp_seek(sp_Cursor *C, sp_Tree *T, size_t off);
int  sp_locate(sp_Cursor *C, size_t off);
int  sp_advance(sp_Cursor *C, sp_Delta d);
#define sp_offset(C) ((C)->off + (C)->poff)
```

- **`sp_seek`**：将游标初始化（或重绑）到树 `T` 的字节偏移 `off`。
  若 `off >= sp_bytes(T)`，游标进入树尾虚拟区（`poff` 为超出树尾的
  距离）。
- **`sp_locate`**：移动已有游标到 `off`，保持其树绑定。越界行为同
  `sp_seek`。
- **`sp_advance`**：按有符号字节增量移动。起点/终点自动 clamp；越过
  树尾进入虚拟区。
- **`sp_offset`**：返回绝对字节偏移的宏。

导航函数均返回 `SP_OK` 或 `SP_ERRPARAM`。

### 3.5 标记

```c
int sp_fill(sp_Cursor *C, sp_Id id, size_t len);
int sp_clear(sp_Tree *T, int ns, sp_Id id);
```

- **`sp_fill`**：将属性 `id` 写入 `[sp_offset(C), sp_offset(C)+len)`。
  区间内每个受影响段都经过 arbiter（写操作必须触达范围内所有段）。
  `len == 0` 无操作；`id == SP_NONE` 拒绝。越过树尾写入时先用 id-0
  段补齐空隙。返回 `SP_OK`、`SP_ERRPARAM` 或 `SP_ERRMEM`。
- **`sp_clear`**：按 namespace 剪枝清除：对 mask 含 `ns` 的每个 span
  调用一次 arbiter 并写回（id 0 时清 mask）。匹配的叶容器批量处理；
  不匹配的叶不访问——这正是 `sp_clear` 独立存在的理由。`ns` 必须在
  `1..SP_MASK_BITS`；`id == SP_NONE` 拒绝。返回 `SP_OK` 或
  `SP_ERRPARAM`。

### 3.6 读取

```c
sp_Id sp_style(sp_Cursor *C, size_t *plen, sp_Mask *pmask);
sp_Id sp_next(sp_Cursor *C, int ns, size_t *plen);
sp_Id sp_prev(sp_Cursor *C, int ns, size_t *plen);
```

- **`sp_style`**：返回当前段的 id，`*plen` 为段内剩余字节数，可选
  `*pmask` 为段精确 ns 集。树尾/虚拟区返回 `SP_NONE` 且 `*plen = 0`。
- **`sp_next`**：**exclusive** 下一个——跳过当前段，返回匹配 namespace
  `ns` 的下一段（`ns == 0` 表示任意 ns）。成功时返回 id、`*plen` 为整段
  长度，游标落在段头。树尾返回 `SP_NONE` 且 `*plen = 0`。`ns` 越界返回
  `SP_NONE`。
- **`sp_prev`**：上一个匹配段。若游标位于匹配段内，返回该段的前缀
  （`*plen = C->poff`）并移到段头；否则向前找上一匹配段。树头返回
  `SP_NONE`。

消费循环：`seek(0)` → `sp_style`（预查当前段）→ `sp_next(ns)`（步进）。
因 `sp_next` 是 exclusive 且落段头，循环不会死锁。

### 3.7 编辑

```c
int sp_splice(sp_Cursor *C, size_t del, size_t ins);
int sp_append(sp_Cursor *C, size_t ins);
int sp_insert(sp_Cursor *C, size_t ins);
int sp_remove(sp_Cursor *L, sp_Cursor *R);
```

- **`sp_splice`**：在游标处删除 `del` 字节，再插入 `ins` 字节。删除自动
  clamp 到树尾；插入继承**左**段 id。
- **`sp_append`**：在游标处插入 `ins` 字节，继承**左**段 id（类似
  vim/sam 的 `a`）。
- **`sp_insert`**：在游标处插入 `ins` 字节，继承**右**段 id（类似 `i`）。
- **`sp_remove`**：删除 `sp_offset(L)` 到 `sp_offset(R)` 之间的字节。
  `L`、`R` 必须属于同一棵树；`L >= R` 为 no-op。游标/树非法返回
  `SP_ERRPARAM`。

编辑函数均返回 `SP_OK`、`SP_ERRPARAM` 或 `SP_ERRMEM`。`SP_ERRMEM` 时
树保持结构一致。

---

## 四、数据结构要点

- **全覆盖 span 模型**：树是 `[0, sp_bytes)` 的 `(len > 0, id)` 段分区。
  没有零长标记、没有字节缝语义、没有 extmark 式点身份。稀疏染色就是
  大段 id 0。
- **无 COW**：spantree 是帧缓冲性质的最终结果存储。不快照、不存内容、
  不需要版本。
- **B+ 树骨架**：与 piecetab/linecache 同构：内嵌 root、池分配、字节
  度量、split/merge/stitch。
- **namespace mask 第三度量通道**：每个节点槽携带 `sp_Mask`，内节点槽
  为子槽 OR。由此 `sp_next`/`sp_prev`/`sp_clear` 可按 ns 剪枝下降，无需
  全树扫描。
- **arbiter 单层**：混合策略完全外置。树零格式知识，只存 id 并调用
  arbiter；arbiter 可在自己的 id 空间编码操作符、attr 与写者身份。
- **无 gravity**：重力由操作决定而非存于标记：`sp_append` 继承左、
  `sp_insert` 继承右、`sp_remove` 只缩段。
- **游标纪律**：游标不停留在树中段尾；唯一段尾位置是树尾（虚拟区）。
  这保证 `sp_style` + `sp_next` 消费循环形态良好。

完整设计原理、算法演进与 namespace/arbiter 细节见
[`../notes/design_spantree.md`](../notes/design_spantree.md)。Lua 绑定
（`sp.compositor` / `sp.new` / 游标 API）见 `lua/spantree.d.lua` 与
`../notes/design_spantree_lua.md`。
