# piecetab v2 设计文档

> 基于 linecache.h 经验重构的 piecetab 设计。
> 核心变化：行/字符映射完全外置给 linecache；B+ 树退化为纯字节级 piece 索引。
> 引入 hole 叶支持可变编辑态，commit 时冻结为 immutable literal。

---

## 一、架构概览

```
┌─────────────────────────────┐
│  pt_State (内存上下文)        │
│  ├── allocf / ud             │
│  ├── pt_Pool nodes           │
│  ├── pt_Pool holes           │
│  ├── pt_Pool trees           │
│  ├── pt_Arena *arenas        │
│  └── max_version             │
└─────────────────────────────┘
            │
            ├── pt_Tree (冻结核，可共享)
            │      ├── root (嵌入)
            │      │     ├── bytes[]     (度量：叶层=piece长，内层=子树累计)
            │      │     ├── mask        (单字位图：叶层=hole标识，内层=子树含hole)
            │      │     ├── children[]  (pt_Node* 或 数据指针)
            │      │     ├── version
            │      │     └── child_count
            │      └── bytes / refcount / version
            │
            └── pt_Cursor (导航 + 编辑)
                   ├── paths[]  (根→叶路径，详见 §一.1)
                   ├── tree     (当前树)
                   ├── poff     (piece内偏移)
                   ├── off      (绝对字节偏移)
                   └── dirty    (编辑态标志)
```

**核心设计原则**：
- 单 B+ 树，叶层混存 literal 指针与 hole 对象，父节点由 mask 位图区分
- piecetab 纯字节级，不管行/字符/编码 — 用户按需组合 linecache
- COW 用 path-copying + 节点版本号

### 一.1 术语规范：层级模型（必读，易误解）

```
levels = 3 的树：从 root 到 piece 共 levels+2 = 5 层

  层号        角色                  paths 获取方式
  ──────────────────────────────────────────────────
  -1          root (pt_Node)        ptK_parent(C, 0)，仅此一种取法
   0          内节点 (pt_Node)       *C->paths[0]
   1          内节点 (pt_Node)       *C->paths[1]
   2          叶容器 (pt_Node)       *C->paths[2] = ptK_parent(C, 3)
   3  (=levels) 叶 = piece (数据)    *C->paths[3] = literal 或 hole 指针
```

| 术语       | 定义                                                                             |
| ---------- | -------------------------------------------------------------------------------- |
| **叶**     | piece = `*C->paths[levels]` (char* 或 pt_Hole*)                                  |
| **叶容器** | levels-1 层 pt_Node = `ptK_parent(C, levels)`，其 children 数组直接存 piece 指针 |

**C->paths 是不变量**：`C->paths[l]` 始终指向 `ptK_parent(C, l)->children` 数组中的某个槽。
`ptK_idx(C, p, l)` = `C->paths[l] - p->children` 因此永远有效。

**具体示例**（levels=3 的树，由 `maketree` 构造）：

```
  depth -1: root (cc=3)
  depth  0: root->children[0] = inner_block_0
               ↑ C->paths[0]
  depth  1: inner_block_0->children[0] = leaf_container_0
               ↑ C->paths[1]
  depth  2: leaf_container_0 (cc=4) — 叶容器
               ↑ C->paths[2], 即 ptK_parent(C, 3)
  depth  3: leaf_container_0->children[1] = HOLE "##" — 叶（piece）
               ↑ C->paths[3], *C->paths[3] = HOLE "##"
```

**mask 维护铁律**：任何可能引入/移除 hole 的编辑操作，除 `ptM_up` 更新度量外，
**必须沿 paths 向上传播 mask 位**：`ptM_up(C, l, db)` 在每层更新 `bytes[i]` 后调
`ptM_remask(p, i, levels - l)`，mask 变化或度量为非 0 时继续上推，因此所有编辑路径
都经 `ptM_up` 完成度量与 mask 的同步传播。

**不变量：位 ≥ cc 恒为 0** — cc 收缩处（`ptN_remove`、`ptD_balancenode`、
`ptI_splitroot`/`ptI_splitchild` 裂半、`ptD_mergeleaf` merged 路径）统一以
`ptM_clamp(p)` 截断：`p->mask &= ptM_mask(ptN_cc(p))`。残值位会被未掩码读
（`mask != 0`）误判为 hole 假阳性（fuzz 实录 pt_holebit_repro.txt）。

---

## 二、叶类型（叶 = piece，术语参见 §一.1）

### 2.1 literal（轻量级）

literal 是不具有所有权的纯字节指针，可能来自用户提供的，生命周期超过 pt_close 的内存，
或者来自 scratch buffer 中的不可变数据段。B+ 树叶层以裸指针存储：

```
children[i] → literal pointer (char*)
node->bytes[i] → 该 piece 的字节长度
```

literal 无独立结构体，零额外开销（对比旧 `pt_Piece` 128 字节）。
一个 `char*` (8B) 已是存储最低限 — 总需要一个指针指向数据。

> 为何不池化为重对象？ 见附录 C。

### 2.2 hole（可变编辑叶）

hole 是编辑态中原地改写的可变缓存。固定容量，池化管理：

- 宏 `PT_MAX_HOLESIZE=64`（`piecetab.h:175-177`）。
- `pt_Hole` 唯一 field 是 `char data[PT_MAX_HOLESIZE]`；hole 的当前长度以父节点同槽的 `bytes[i]` 为准。
- 结构体总大小 64 字节（2 的幂，cache 友好）；容量是编译期宏，不占 field。
- 创建时机：`pt_edit` 命中非 hole 位置 → 分裂 literal → 插入 hole。
- 销毁时机：空 hole（`bytes[i]==0`）立即回池；commit 时遍历 hole，数据写入 scratch 后将原位换为 literal 指针。

### 2.3 相邻 literal 合并

若两相邻 literal 物理地址连续（`literal_i + node->bytes[i] == literal_{i+1}`）且版本号相同，进行叶层槽位合并：把右槽长度累加到左槽，然后删除右槽。

此操作在编辑后和 pt_remove 后检查，防止叶槽碎片累积。
commit 时连续 hole 转为 literal 时也按此规则自然合并。

### 2.4 叶异质性：`ptD_mergeleaf` 为何返回 `d`

叶槽是**异质**的（literal 指针 / `pt_Hole*`，由父 `mask` 位区分），而 §2.3 只允许**物理连续
的 literal** 合并 —— hole 与 literal 之间、以及物理不连续的 literal 之间都不能合并。
因此 `ptD_mergeleaf` 只有两条合并路径：

- `lit + lit` 且物理连续 → 整槽合并（`merged`），rt 槽消失；
- `hole + hole` → 把 rt 首槽（hole）的数据**部分或全部**追加进前槽的 hole：
  `d = min(rt->bytes[0], PT_MAX_HOLESIZE - bc)`。受容量上限约束，`d` 常常只吃掉 rt 首槽的一部分，
  于是 **rt 槽存活为一个缩短的 hole**，而"删除终点"落在这个洞的右边界上。

正因为 rt 槽可能存活、且其起点已被搬走 `d` 字节，光标**无法在 mergeleaf 内一次放准**：
mergeleaf 先把光标推迟到 survivor 槽尾（`off += poff + d; poff = 0; paths[l] = &p->children[cc]`），
再由 `ptD_stitch` 的 `i == cc` 修正与 `d > poff` 尾段落回"删除终点前一个元素"上；`d` 就是这条
修正链所需的字节数。

**这是 piecetab 独有的需要**：linecache 的 `d` 来自叶内行索引 `lnu` 的修正（其游标是行索引型的）；
spantree 与 anchortree 的叶同质、游标是纯字节型，两者都不需要 `d` —— `spD_mergeleaf` /
`atD_mergeleaf` 在自己的搬移出口就地同步游标即可（见 `notes/design_spantree.md` §7.4 光标契约）。

### 2.5 0 长槽禁止与 `ptD_cutrange` 的 0 长特判

与 linecache / spantree / anchortree 一致，**树内不得存在 0 span 槽**：`poff` 是段内偏移，
中间槽 `bytes == 0` 会让 `poff >= bytes` 把该槽误判为耗尽，`pt_next` / 导航与虚拟尾判定全部失效。

0 长槽只在"游标指向末尾"处需要处理：`ptD_cutrange` 用 `i += (k || p->bytes[i] == 0);`
（`piecetab.h:724`、`729`）把切点处的 0 长槽随切点一起删去，因此 survivor 永不为 0 长槽 ——
这就是"禁止树中段尾"在删除路径上的落点。（spantree 同款：`spD_cutrange:592/597`；
linecache 的负载度量是行数，故写 `p->breaks[i] == 0`，`lcD_cutrange:600/605`。）

`ptD_cutrange` 的整体形状（L / R 两游标 + `rt` scratch；三库同构）：

1. 自 `kl = levels` 向下到 `fl + 1`：L 侧 `ptN_remove(S, p, k, i + 1, cc)` 摘掉切点右侧；R 侧从
   `i`（必要时 `+1`，见上）起 `ptN_copy` 到 `rt[k]`，再把 `[0, i)` `ptN_purge` 并 `ptN_setcc(p, 0)`；
2. `fl` 层：同样 `ptN_copy` 到 `rt[k]`；`ptN_setcc(p, i)` 留下切点前的部分；最后 L 侧
   `ptN_remove(S, p, k, i + 1, cc)`；
3. 层号 `k = levels - kl` 决定 `rt[k]` 装哪一层（`rt[0]` = 叶层）；`rt` 由 `ptD_stitch` 逐层 graft 回去；
   trim 由调用方在 cutrange 之前完成，cutrange 只看游标槽号与 `bytes[]`。

---

## 三、叶类型辨识

内部节点用一个单字 mask 位图标记每个 child（当前默认 `PT_FANOUT=31`，`mask` 类型为 `unsigned`；`PT_FANOUT>=32` 时为 `unsigned long long`）：

- 宏 `PT_MASK_BITS = sizeof(pt_Mask) * CHAR_BIT`；静态断言 `PT_FANOUT <= PT_MASK_BITS`。
- `pt_Node` 在共同 `children`、`bytes`、`child_count` 之外，额外 field 是 `mask`（单字位图）和 `version`（COW 版本）。
- **叶层**：`mask` 第 i 位为 1 → `children[i]` 指向 `pt_Hole*`；否则指向 literal。
- **内层**：`mask` 第 i 位为 1 → 子树 i 内含至少一个 hole（commit 遍历优化）。
- 收缩 `child_count` 后必须 `ptM_clamp` 清掉高位残值；位图随节点 COW 复制；编辑时沿 paths 向上传播（同 `lcM_up`）。
- 成本：每个 child 约 1 bit；默认 fanout 31，不受单字 64 位限制。

---

## 四、版本号与 COW

### 4.1 版本号

- 每个 `pt_State` 维护全局 `max_version`，cursor 首次编辑时递增并赋给新 tree
- 节点 version 标记其所属编辑区间
- **literal 无 version**（纯 immutable 数据，COW 只作用于 Node 层）
- `pt_Tree.epoch`（`unsigned short`）是与 `version` 无关的**原地修改代次**：每次成功
  修改该 tree 时递增，供 Cursor 失效检测使用（§4.7）

### 4.2 COW 触发

编辑时遍历树，访问节点时：
- `node->version < root->version` → 分配新节点拷贝，设 version = root->version（COW）
- `node->version == root->version` → 已是当前编辑态私有，原地修改

### 4.3 编辑态 = Cursor 内部 transient tree

首次编辑 API（`pt_edit`/`pt_insert`/`pt_append`/`pt_splice`/`pt_remove`）被调用时：
1. 分配新 `pt_Tree`：`root = copy_of_buffer_root`，`version = ++S->max_version`，
   `from = 旧 b` 并 `retain(from)`（COW 生命周期：保证源树的共享节点存活到
   本 transient 释放，见 §4.4）
2. `C->tree = 新 tree`，`C->dirty = 1`

> **注**：不再用 `C->oldbuffer` 记 rollback 锚点——改由 `b->from` 表达
> "从哪个 tree fork 而来"，rollback 走 `b->from`（见 §4.4）。

**关键**：这个新 tree **不在 pt_State 的持久列表中**，仅 `C->tree` 持有。
外部无法通过任何公共 API 获取其引用。不是"Snapshot 有个态"然后拒绝二次编辑，
而是结构上杜绝了被外部重新 fork 的可能。

| 场景                                     | 行为                                                                       |
| ---------------------------------------- | -------------------------------------------------------------------------- |
| 首次 `pt_edit(C, ...)`                   | fork 旧 b → 新 transient tree，dirty=1                                     |
| 再次 `pt_edit(C, ...)` （同一 C）        | 直接在 transient tree 上操作（版本号不变）                                 |
| `nC = *C` 后 `pt_edit(&nC, ...)`         | 两 Cursor 共享同一 transient tree；一方成功修改后另一方判失效（§4.7）      |
| 从旧 Snapshot `pt_seek(&C2, oldSnap, 0)` | C2 拿到 persistent tree → 首次编辑 fork 出另一 transient（新版本号，独立） |

**多副本编辑**：从同一 Snapshot(version=100) 派生的两个 Cursor 各自 fork 出
独立 transient tree（101、102），互不干扰。若通过 `nC = *C` 拷贝则共享同一
transient tree，一方成功修改后另一方在下一次公共入口被判失效（§4.5、§4.7）。

### 4.4 commit / rollback

两个出口都返回**已拥有的引用**并 detach cursor（`C->tree = NULL`）；继续使用需对
返回的 buffer 重新 `pt_seek`。

- **commit**：`pt_valid` 判定 → `ptC_holebytes` 汇总 hole 字节 → `pt_reserve`/
  `pt_literal` 预留 arena → `ptC_nexthole` 定位首个含 hole 叶并 `ptS_ok` 递增 tree
  `epoch` → `ptC_freeze` 及其推进循环（每轮 `ptP_reserve(nodes, levels)` → 冻结该叶
  hole 数据、合并相连 literal 槽 → 欠满则 `ptD_rebalance` fold/borrow）→ detach 返回 b。
  clean cursor（`!C->dirty`）时仅 `pt_retain` 当前树并 detach 返回。
- **rollback**：`pt_valid` 判定 → clean cursor（`!C->dirty`）仅 `pt_retain` 当前树并
  detach 返回；dirty cursor 走 `b = C->tree->from` 先 `pt_retain(b)`，再 `pt_release(C->tree)`
  沿 from 链回收本 transient 的私有节点，置 `C->tree = NULL`、`C->dirty = 0`、返回 b；
  返回引用使源树无条件存活（§4.4.1）。与 `pt_commit` 同为「先过 `pt_valid`」的事务出口。
- commit 后 tree 可被共享（返回的 Snapshot）；rollback 返回编辑前源树的新引用。
- **失败语义**：arena 预留失败 → 返回 `NULL`，树与 `epoch` 均未变；`ptC_freeze` 期
  OOM → 返回 `NULL`，此时 tree 已发生部分冻结、`epoch` 已递增，cursor 经
  `pt_locate(C, off)` 复位到原偏移故仍有效，可重试 commit。

**COW 生命周期（`from` 机制，关键）**：`pt_Tree.from` 指向 fork 来源并在 fork 时
`retain`。这保证"只要 transient/committed 存活，其共享的源节点就存活"，根除
"旧版本先 release、释放仍被新版本共享的节点"这一 use-after-free。`pt_release`
归零时**先**删本版本私有节点，**最后** `release(from)` 沿链回溯。

### 4.4.1 rollback 的源树存活

源树可能仅靠本 transient 的 `from` 存活（外部已 release）。若 rollback 直接
`release(transient)` 并把 `C->tree` 指回源树，则源树随 release 一起死，`C->tree`
悬空（实测 ASan use-after-free）；根因是 refcount 无法表达 cursor 的**借用**语义
（cursor 无脑 drop、不通知 tree，故无法维护弱计数）。

解法：**rollback 先把源树引用交给调用者**——`b = C->tree->from`、`pt_retain(b)`，
再 `pt_release(C->tree)`（fork 的 retain 由这条 release 链抵消），最后
`C->tree = NULL`、返回 b。返回引用使源树无条件存活，无需检查 `from->refcount`；
cursor 在 clean / dirty 两条路径上都被 detach，回退后继续编辑需对返回的 buffer
重新 `pt_seek`。

### 4.5 multi-cursor 协同编辑

`nC = *C` 后两个 Cursor 共享同一 transient tree，各自可独立编辑。任一 Cursor 成功
修改该 tree（裂叶、释放 hole、原地改写）都会递增 tree 的 `epoch`，于是仍指向同一
tree 的其他 Cursor 在下一次公共入口调用时被判失效并返回 `PT_ERRPARAM`（§4.7）——
其他 Cursor 的 paths 不会被静默改写后继续使用。库不加锁：并发语义由调用顺序确定。

从旧 Snapshot（persistent tree）fork 出各自 transient 的多副本编辑不受影响：fork
复制 `epoch` 而不改动源 tree，源 tree 上的 Cursor 继续有效（§4.3 派生表）。

### 4.6 失效检测的覆盖范围

- **结构失效**：Cursor 指向的 tree 在存活期间被其他 Cursor 原地修改（`nC = *C`
  协同编辑，或把 transient 当 buffer 再 `pt_seek` 进去）→ 由 `epoch` 检测（§4.7），
  公共入口返回 `PT_ERRPARAM`，`pt_valid(C)` 可先行判定。
- **借用生命周期失效**：Cursor 所借的 buffer 被外部 `pt_release`、tree 被释放 →
  Cursor 不持有引用，故不在 `epoch` 覆盖内；契约是"借用期间持有该 buffer"（§6.1），
  恢复手段是在仍被持有的 tree 上重新 `pt_seek`。

### 4.7 Cursor 失效检测（epoch）

- **字段**：`pt_Tree.epoch`（`unsigned short`，原地修改代次）、`pt_Cursor.epoch`
  （`pt_seek` 捕获；`pt_locate`/`pt_advance` 只导航、不重打戳）。
- **判定**：`pt_valid(C)`（公共函数，非宏）=
  `C && C->tree && C->epoch == C->tree->epoch`；`epoch` 只做相等比较。从未 `pt_seek`
  过的零值 Cursor 因 `tree == NULL` 不通过。
- **递增点**：`pt_edit`/`pt_append`/`pt_remove` 的成功出口调用 `ptS_ok(C)`
  （`C->epoch = ++C->tree->epoch`，返回 `PT_OK`），`pt_insert`/`pt_splice` 经内部调用
  覆盖；no-op 出口与错误返回不改 `epoch`——即"返回失败 ⇒ tree 与 epoch 都不变"。
  `pt_commit` 是例外：arena 预留失败时不改 `epoch`；进入 freeze 前先 `ptS_ok`
  （冻结会原地改树，须使其上其它 Cursor 失效），此后 freeze 内 OOM 返回 `NULL` 时
  tree 已部分冻结、`epoch` 已递增，本 Cursor 经 `pt_locate(C, off)` 复位后仍有效。
- **fork 不递增**：`ptK_markdirty` 以 `*nt = *old` 复制 `epoch`，源 tree 的 `epoch`
  不变，故旧 Snapshot 上的 Cursor 继续有效（§4.3 多版本编辑不受影响）。
- **入口检查**：解引用 cursor 的公共入口在解 paths 前判定失效——
  `pt_advance`/`pt_locate`/`pt_edit`/`pt_append`/`pt_insert`/`pt_remove`/`pt_splice`
  返回 `PT_ERRPARAM`；`pt_reserve`/`pt_literal` 返回 `NULL`；`pt_piece`/`pt_next`/
  `pt_prev`/`pt_scratch` 返回 `NULL` 且置 `*plen = 0`；`pt_read` 返回 `0`；
  事务入口 `pt_commit` 同样先判 `pt_valid`，过期返回 `NULL`。
- **恢复**：`pt_seek` 是唯一 Cursor 构造器（重新捕获 `epoch`）；dirty 的 transient 先经
  `pt_rollback`（§4.4.1）。

---

## 五、度量系统

### 5.1 bytes 的约定

`node->bytes[i]` 的含义随层级改变，与 linecache 一致：
- **叶层**（`children[i]` 为数据指针）：piece 自身字节长度
- **内层**（`children[i]` 为子节点）：该子树累计字节数

内层累计通过 `lcM_up` 风格的向上传播维护，实现 O(log n) 定位。

`pt_Tree.bytes` 为全树总字节数（O(1) 查询，无需遍历 root）。

### 5.2 无 chars / breaks / lines

新 piecetab 纯字节级，节点不存 chars、breaks、lines。
行映射由外置 linecache 承担。

---

## 六、API 设计

### 6.1 生命周期

```c
pt_State  *pt_open(pt_Alloc *allocf, void *ud);
void       pt_reset(pt_State *S);
void       pt_close(pt_State *S);
pt_Alloc  *pt_getallocf(pt_State *S, void **pud);
pt_Buffer  pt_empty(const pt_State *S);
pt_Buffer  pt_from(pt_State *S, const char *s, size_t len);
pt_Buffer  pt_compact(pt_State *S, pt_Buffer b);
unsigned   pt_retain(pt_Buffer b);
unsigned   pt_release(pt_Buffer b);
size_t     pt_bytes(pt_Buffer b);
unsigned   pt_version(pt_Buffer b);
```

> `pt_empty(S)` 返回 `&S->empty`（state 内嵌 sentinel tree，零分配）。
> `pt_release` 遇 sentinel（含 from 链到达 sentinel）直接返回。
> `pt_from` 设置 `t->from = &S->empty`，from 链始终终止于 sentinel。
> 公共 API 均已正确处理 `NULL` 和 sentinel。
> Cursor 只借用 buffer（不持有引用）：绑定的 tree 必须在 Cursor 存活期间被外部持有，
> 否则 Cursor 失效（§4.6）；失效判定与错误通道见 §4.7。

### 6.2 导航

```c
/* construction */
int  pt_seek(pt_Cursor *C, pt_Buffer b, size_t off); /* 构造器: reset + bind + 捕获 epoch + 定位, 清 dirty */

/* navigate */
int  pt_locate(pt_Cursor *C, size_t off);  /* 纯绝对定位, 保留 dirty */
int  pt_advance(pt_Cursor *C, pt_Delta delta);

/* query */
int  pt_valid(const pt_Cursor *C); /* 已绑定且未被其他 Cursor 修改过 */
#define pt_offset(C) ((C)->off + (C)->poff)
#define pt_prefix(C) ((C)->poff)
#define pt_buffer(C)   ((C)->tree)
```

- `pt_seek` 是 Cursor 构造器，重置所有内部状态（含 dirty），适合首次绑定 buffers
- `pt_locate` 和 `pt_advance` 用于已绑定 Cursor 的导航，保留 dirty 和 transaction 状态
- 无行级 API（无 `pt_seekline`/`pt_advline`/`pt_setcol`）— 行映射由外置 linecache 承担
- `pt_valid(C)` 判定 Cursor 是否可用；失效时下列入口按各自错误通道返回（§4.7）
- Cursor 只借用 buffer：`pt_seek` 绑定的 tree 必须在 Cursor 存活期间被持有（§6.1）
- piece 级遍历：`pt_piece` 返回当前 piece 剩余数据；`pt_next` 前进一片并返回落脚片；
  `pt_prev` 后退一片并返回落脚片（两者对称，均为"移动后返回落脚点"语义）。
  全量遍历惯用法：`for (p = pt_piece(C,&n); n; p = pt_next(C,&n))`
- piece 尾约定：树中段不允许 Cursor 停在 piece 尾（`C->poff == 该片剩余长度` 只在
  `pt_offset(C) == pt_bytes(tree)` 时合法）；`pt_read` 的请求恰好结束在片段边界时，
  Cursor 落在下一片起点。`pt_checkcursor` 校验同一不变量
- `pt_prefix(C)` 返回当前 piece 内、当前位置之前的字节数（即 `C->poff`），不移动游标；
  与 `pt_piece` 组合可得到当前完整 piece 的 base 指针、起始 offset 和完整长度。

### 6.3 编辑（核心）

```c
int  pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len);
int  pt_insert(pt_Cursor *C, const char *s, size_t len);
int  pt_append(pt_Cursor *C, const char *s, size_t len);
int  pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len);
int  pt_remove(pt_Cursor *C, size_t len);
```

**`pt_edit(C, del, s, len)`** — splice via hole（copy 语义，追尾）

- 先删除 cursor 位置起 `del` 字节
- 再插入 `s[0..len-1]` 作为 hole 数据（copy 到 hole 内）
- cursor 追尾到插入内容之后
- 若 cursor 位置已是 hole 尾 → 直接追加至 hole（无新分配）
- 若 cursor 在 hole 中间且 hole 有空间 → memmove + copy
- 若 hole 空间不足 → 裂新 hole，数据跨 hole 分布
- `del == 0 && len > 0` → 纯插入；`del > 0 && len == 0` → 纯删除；两者皆 0 → no-op
- `s` 生存期无要求（库已 copy）
- 入口是复合预算：`ptK_reserve(C, 3 * levels + 3, len > 0 ? 2 : 0)`（删段两端 COW + 匀位 COW + 插段分裂，holes 最多 2 个），随后 `ptK_beginedit`、`ptD_remove`、内联插片

**`pt_insert(C, s, len)`** — 插入 literal（引用语义，原位）

- 直接引用 `s`，不 copy
- `s` 必须存活至 `pt_close`
- 若 `s` 在 scratch pool 内，库自动消费 buffer 头并推进
- cursor 不移动

**`pt_append(C, s, len)`** — 插入 literal（引用语义，追尾）

- 同 `pt_insert`（literal 引用语义），但 cursor 追尾到插入内容之后
- 适用于批量载入文件块（配合 mmap 或 scratch）
- 入口 `ptK_reserve(C, 2 * levels + 2, inhole)`：`inhole` = 游标严格落在 hole 片内部（插入会裂出右半 hole）
- 主体是插段机器 `ptI_append`：先 `ptK_beginedit`（markdirty + 整条路径 COW），再走
  「左邻 literal 接尾 / 本槽 literal 接尾 / 前缀重合 literal」三条合并快路径，否则
  `ptI_putpiece` 填 `rt` 并插入

**实现注意事项**：`ptI_putpiece` 只用 `ptM_up` 传播度量与 mask（`ptM_up` 内含
`ptM_remask`），因此所有编辑路径自动同步 mask 位（见 §一.1 铁律）。

**`pt_splice(C, del, s, len)`** — splice via literal（引用语义，追尾）

- 先删除 cursor 位置起 `del` 字节
- 再插入 literal `s[0..len-1]`（引用语义）
- cursor 追尾到插入内容之后
- 入口先 `ptK_reserve(C, 3 * levels + 3, len ? 1 : 0)`（nodes + holes），失败时树保持不变；
  删段 `ptD_remove` 与插段 `ptI_append` 不再各自预留，入口值即一次调用的上界
- `del=0` → 等价 `pt_append`
- 若 `s` 在 scratch pool 内，库自动消费 buffer 头

**`pt_remove(C, len)`** — 区间删除（原位）

- 删除 cursor 位置起 `len` 字节（含跨 piece/hole）
- 删除路径中若含 hole 且 hole 被清空（父槽 `bytes[i]==0`），hole 对象立即回池
- 删除后检查相邻 literal 是否可合并（物理连续则合并）
- cursor 留在删除点
- 入口 `ptK_reserve(C, 2 * levels + 2, 0)`：覆盖 `ptD_remove` 的路径 COW 与 `foldnode` 的邻居 COW

**epoch 与错误契约（见 §4.7）**

- 成功返回前递增所在 tree 的 `epoch` 并刷新执行 Cursor；no-op 与错误返回
  （`PT_ERRPARAM`/`PT_ERRMEM`）既不修改 tree 也不修改 `epoch`
- 入口先判 `pt_valid(C)`，失效 Cursor 在解引用 paths 之前返回 `PT_ERRPARAM`

### 6.4 事务

```c
pt_Buffer pt_commit(pt_Cursor *C);
pt_Buffer pt_rollback(pt_Cursor *C);
```

### 6.5 scratch pool

```c
char *pt_scratch(pt_Cursor *C, size_t *plen);
```

返回 Cursor 所在树 arena 活动块写入头的指针和剩余可写容量。
通过 `pt_literal`/`pt_insert`/`pt_append`/`pt_splice` 传递给库时，库自动消费该 buffer
（推进写入头），下次 `pt_scratch` 返回新区。
两次 `pt_scratch` 之间没有消费时，返回的仍是同一块内存（同一写入头）。

---

## 七、commit 流程

commit 流程：

1. `pt_valid(C)` 判定；clean Cursor（`!C->dirty`）直接 `pt_retain` 当前树并 detach 返回。
2. `ptC_holebytes` 自 root 汇总 hole 字节（沿内部节点 mask 跳过无 hole 子树）；
   `pt_reserve`/`pt_literal` 在 arena 预留等长空间。
3. `ptC_nexthole(C, 0)` 按 mask 下降定位首个含 hole 叶，`ptS_ok(C)` 递增 tree `epoch`
   （冻结原地改树，须使其上其它 Cursor 失效）。
4. `ptC_freeze` 冻结该叶：每轮开头 `ptK_reserve(C, 1, 0)` 预留本轮节点 → hole 数据追加到
   arena、槽换为指向 arena 的 literal、槽长度改为 hole 当前长度、清除 hole 位
   （`pt_Hole` 回池）→ 合并物理相连 literal 槽 → 叶欠满则 `ptD_rebalance` fold/borrow
   （fold 吸进邻居可能带来新 hole/接缝，故同一叶上循环至稳定；loop 可重复多轮，
   故 reserve 在每轮开头补 1 个节点）。
5. `ptC_nexthole(C, ptK_levels(C) - 1)` 前进到下一含 hole 叶并重复 4；无更多 hole 时
   结束，detach Cursor（`C->tree = NULL`）并返回 b。

失败语义：步骤 2 的 arena 预留失败 → 返回 `NULL`，树与 `epoch` 均未变；步骤 4 的
`ptK_reserve` 失败 → 返回 `NULL`，tree 已部分冻结、`epoch` 已递增，Cursor 经
`pt_locate(C, off)` 复位到原偏移故仍有效（树保持 dirty，可重试 commit）。

---

## 八、与 linecache 的关系

piecetab 与 linecache 完全解耦。用户自行组合：

```
文件载入 → pt_from(mmap, len) → lc_scan(linecache, scanner) → 构建完成
渲染    → lc_seekline(lc_cursor) → 获取行偏移 → pt_seek(pt_cursor) → 导航到位置
编辑    → pt_edit(pt_cursor, ...) → 同时更新 lc_splice(lc_cursor, ...)
```

piecetab 不内嵌调用 linecache。两个库各自独立，状态由外部分别持有。

---

## 九、配置宏

| 宏              | 含义                    | 默认                          |
| --------------- | ----------------------- | ----------------------------- |
| `PT_FANOUT`     | 节点最大子数            | 31                            |
| `PT_MAX_HOLESIZE` | hole 容量（字节）     | 64                            |
| `PT_MAX_LEVEL`  | 最大树深                | 17（FANOUT=31 安全值，完整表见 docs/max_levels.zh.md） |
| `PT_PAGE_SIZE`  | 池分配器页大小          | 65536                         |
| `PT_STATIC_API` | 定义后所有函数为 static | —                             |

---

## 十、待定项

| 项目                         | 状态 | 说明                                                      |
| ---------------------------- | ---- | --------------------------------------------------------- |
| fanout 调优基准              | TODO | 当前默认 31，后期 benchmark 决定                          |
| hole 容量调优基准            | TODO | 当前默认 `PT_MAX_HOLESIZE=64`，后期 benchmark 决定        |
| `pt_scratch` 多次调用保护    | TODO | 编辑器开发后按需加引用保护                                |
| multi-cursor 同编辑态协同    | 已定 | 一方成功修改后另一方判失效（§4.5、§4.7）                   |
| 全局统计 `pt_chars/pt_lines` | 移除 | 由 linecache 承担                                         |

---

## 附录 A：术语表

| 术语       | 含义                                                                  |
| ---------- | --------------------------------------------------------------------- |
| literal    | append-only buffer 中的不可变数据段，叶层以 `char*` 裸指针引用        |
| hole       | 编辑态可变叶，固定容量，commit 后内容迁移至 scratch                   |
| transient  | 编辑态 tree（可变、独占、仅 Cursor 持有、不对外暴露）                 |
| persistent | 持久态 tree（immutable buffer，可 COW 共享）                          |
| mask       | 单字位图（`unsigned` 或 `unsigned long long`，随 `PT_FANOUT` 选择），标记 hole（叶层）或子树含 hole（内层） |

---

## 附录 B：与旧版 piecetab 架构对比

| 项         | 旧版                             | 新版                                                     |
| ---------- | -------------------------------- | -------------------------------------------------------- |
| 行缓存     | 内置 B+ 树双重职责               | 外置 linecache                                           |
| 字符映射   | 内置 chars 度量                  | 删除（clean octet）                                      |
| piece 结构 | `pt_Piece` (128B, 含 ends[58])   | literal: 纯 `char*` (8B)                                 |
| 叶类型     | 仅 literal                       | literal + hole                                           |
| 编辑态     | COW 每次编辑创建新版本           | transient 仅在 Cursor 内，不 COW                         |
| API 参数名 | `c`                              | `C`（Cursor）/ `S`（State）                              |
| 删除 API   | `pt_remove(C, end)`              | `pt_remove(C, len)`                                      |
| 替换 API   | `pt_replace(C, s, len)`          | `pt_splice(C, del, s, len)`                              |
| 行相关 API | `pt_seekline`, `pt_advline`, ... | 全部移除                                                 |
| 导航 API   | `pt_peek`, `pt_next`, `pt_prev`  | `pt_piece`/`pt_next`/`pt_prev`（"移动后返回落脚点"语义） |

---

## 附录 C：为何 literal 不做池化对象（轻量 vs 重量分析）

假设 literal 是池对象 = `{char* data; unsigned version; ...}`，最小 16 字节

| 指标                    | 轻量级 (`char*`, 8B) | 重量级 (`pt_Literal`, 16B)    |
| ----------------------- | -------------------- | ----------------------------- |
| 净内存开销/literal      | 0（在 Node 槽内）    | +8B（槽存指针 → 对象）        |
| 额外间接                | 无                   | 一次解引用（cache miss 风险） |
| 分配次数/literal        | 0                    | 1                             |
| 1000 literal 额外内存   | 0                    | ~16KB                         |
| 100 万 literal 额外内存 | 0                    | ~16MB                         |

对 4GB 文件，百万级 literal 已不现实。任何现实规模下差异可忽略。
重量级唯一卖点 — literal 带 version/tag — 在新架构下落空：

- **version**：COW 在 Node 层，literal 纯不可变，无需自己的版本号
- **tag/标记**：已定外置于 marktree

故重量级多一层间接 + 分配 + 对象空间，无对应收益。
保留轻量级设计。若未来需要 per-literal 元数据，届时再引入。
