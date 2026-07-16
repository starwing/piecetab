# piecetab v2 设计文档

> 基于 linecache.h 经验重构的 piecetab 设计。
> 核心变化：行/字符映射完全外置给 linecache；B+ 树退化为纯字节级 piece 索引。
> 引入 hole 叶支持可变编辑态，commit 时冻结为 immutable literal。

---

## 一、架构概览

```
┌─────────────────────────────┐
│  pt_State (内存上下文)        │
│  ├── allocf / alloc_ud       │
│  ├── pt_Pool nodes           │
│  ├── pt_Pool holes           │
│  ├── pt_BufferPool literalBuffer │
│  └── max_version             │
└─────────────────────────────┘
            │
            ├── pt_Tree (冻结核，可共享)
            │      ├── root (嵌入)
            │      │     ├── bytes[]     (度量：叶层=piece长，内层=子树累计)
            │      │     ├── mask[]      (位图：叶层=hole标识，内层=子树含hole)
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

**mask 维护铁律**：任何可能引入/移除 hole 的编辑操作，除 `ptM_upbytes` 更新度量外，
**必须调用 `ptM_upmask`** 沿 paths 向上传播 mask 位。`ptI_splitins` 仅处理了度量
（upbytes），mask 上推由调用方负责。

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

```c
#define PT_HOLE_CAP (64 - sizeof(unsigned short))  /* 保持总大小 2^n */

typedef struct pt_Hole {
    unsigned short n;                /* 已写入字节数 (0..PT_HOLE_CAP) */
    char           data[PT_HOLE_CAP]; /* 实际数据 */
} pt_Hole;
```

- 总大小 = `sizeof(unsigned short) + PT_HOLE_CAP` = 64（2 的幂，cache 友好）
- 容量为编译期宏，不占结构体字段
- 创建时机：`pt_edit` 命中非 hole 位置 → 分裂 literal → 插入 hole
- 销毁时机：空 hole（`n==0`）立即回池；commit 时遍历 hole，数据写入 scratch 后将原位换为 literal 指针

### 2.3 相邻 literal 合并

若两相邻 literal 物理地址连续（`literal_i + node->bytes[i] == literal_{i+1}`）
且版本号相同，进行叶层槽位合并：
```
node->bytes[i] += node->bytes[i+1];
// 删除槽 i+1
```

此操作在编辑后和 pt_remove 后检查，防止叶槽碎片累积。
commit 时连续 hole 转为 literal 时也按此规则自然合并。

---

## 三、叶类型辨识

内部节点新增 mask 位图数组，每个 child 占 1 bit：

```c
#define PT_MASK_SIZE ((PT_FANOUT + 63) / 64)

struct pt_Node {
    size_t              bytes[PT_FANOUT];       /* 度量：叶层=piece长，内层=子树累计 */
    unsigned long long  mask[PT_MASK_SIZE];     /* 位图：叶层=hole标识，内层=子树含hole */
    struct pt_Node     *children[PT_FANOUT];    /* 数据指针或子节点 */
    unsigned            version;
    unsigned short      child_count;
};
```

- **叶层**：`mask[i] == 1` → `children[i]` 指向 `pt_Hole*`；否则指向 `char*`（literal）
- **内层**：`mask[i] == 1` → 子树 i 内含至少一个 hole（commit 遍历优化）
- 位图随节点 COW 复制；编辑时沿 paths 向上传播（同 `lcM_up`）

性能影响：
- 每 8 字节指针仅多 1 bit 开销（≈1.5%）
- fanout 不受 64 限制（只需调整 `PT_FANOUT`）

---

## 四、版本号与 COW

### 4.1 版本号

- 每个 `pt_State` 维护全局 `max_version`，cursor 首次编辑时递增并赋给新 tree
- 节点 version 标记其所属编辑区间
- **literal 无 version**（纯 immutable 数据，COW 只作用于 Node 层）

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
| `nC = *C` 后 `pt_edit(&nC, ...)`         | 两 Cursor 共享同一 transient tree，可协同编辑                              |
| 从旧 Snapshot `pt_seek(&C2, oldSnap, 0)` | C2 拿到 persistent tree → 首次编辑 fork 出另一 transient（新版本号，独立） |

**多副本编辑**：从同一 Snapshot(version=100) 派生的两个 Cursor 各自 fork 出
独立 transient tree（101、102），互不干扰。但若通过 `nC = *C` 拷贝则共享同一
transient tree — paths 可能因树结构修改而失效，库不加锁，使用者负责。

### 4.4 commit / rollback

- **commit**：遍历 hole → 写入 scratch → 替换为 literal 指针 → `C->dirty=0` → 返回 b
- **rollback**：读 `from = C->tree->from`；若 `from->refcount == 1`（source 仅靠本
  transient 的 fork-retain 存活，release 后必死）则 `from = NULL`；`pt_release(C->tree)`
  （删本 transient 私有节点 + 链式 `release(from)`）；`C->tree = from`、`C->dirty=0`
- commit 后 tree 可被共享（返回的 Snapshot）；**rollback 后：source 仍被外部持有
  （常见）则 Cursor 回到源树继续可用；source 已被外部放弃（仅 transient 持有）则
  Cursor 失效（`b=NULL`），须重新 `pt_seek`**

**COW 生命周期（`from` 机制，关键）**：`pt_Tree.from` 指向 fork 来源并在 fork 时
`retain`。这保证"只要 transient/committed 存活，其共享的源节点就存活"，根除
"旧版本先 release、释放仍被新版本共享的节点"这一 use-after-free。`pt_release`
归零时**先**删本版本私有节点，**最后** `release(from)` 沿链回溯。

### 4.4.1 rollback 的条件失效（保留常见可用路径）

若 rollback 无条件令 `C->tree` 回到源树，则当"源树仅靠本 transient 的 `from`
存活（外部已 release）"时，`release(transient)` 会连带释放源树，`C->tree` 悬空
（实测 ASan use-after-free）。根因是 refcount 无法表达 cursor 的**借用**语义
（cursor 无脑 drop、不通知 tree，故无法维护弱计数）。

解法：**rollback 前检测 `from->refcount`**——
- `> 1`：外部仍持有源树，`release(transient)` 只让源树 rc 减 1 仍存活，Cursor
  安全回到源树（`C->tree = from`），**常见路径，rollback 后 cursor 仍可用**。
- `== 1`：源树仅靠本 transient 持有，`release` 后必死，故置 `C->tree = NULL`
  使 cursor 失效，**不悬空**。

refcount 会计干净（fork 的 retain 被 rollback 的 release 恰好抵消）。契约：
**使用者负责持有"要回退到的 buffer"**——持有则 rollback 后可继续用，放弃则
cursor 失效。

### 4.5 multi-cursor 协同编辑

`nC = *C` 后两个 Cursor 共享同一 transient tree，各自可独立编辑。
任一 Cursor 修改树结构（裂叶、释放 hole）后，其他 Cursor 的 paths 可能失效 —
这是 B+ 树无 parent 的固有代价。库不加锁不保护，使用者负责保证不冲突。

### 4.6 TODO：多 Cursor 借用失效检测（暂不实现）

**问题**：buffer 上存在多个 Cursor（C1 派生出 C2/C3…）时，某些操作会让其他
Cursor 静默失效，而库无法检测：
- 情况 1：C1 edit → 派生 C2/C3 → C1 再 edit（改树结构）→ C2/C3 的 paths 失效。
  **符合文档**（§4.5，使用者负责）。
- 情况 2：C1 派生 C2/C3 → C1 edit（own transient）→ 外部 release 源 b → C1
  rollback（源 b 真释放）→ C2/C3 失效。**仅置空 C1 无法让 C2/C3 发现问题**。
- 情况 3：b 有 C1/C2/C3 → 外部立即 release b → 使用 C1/C2/C3。**本就无任何
  保证**（借用已失效），且在 remove 开发之前就已存在、同样未检测。

**结论**：情况 2 与情况 3 同源（借用失效不可检测），根因是 refcount 无法表达
借用（见 §4.4.1）。情况 3 既已长期存在且未检测，情况 2 不单独处理。

**未来可选方案**（若需健壮化）：将 `Cursor.dirty` 扩为位域
`dirty:1, valid:1`（或 status enum）；tree 真释放时能标记关联 cursor 为 dead，
使用前 assert(valid)。需要 tree→cursor 的反向登记（与"无脑 drop"权衡），故暂缓。

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
pt_State *pt_newstate(pt_Alloc *allocf, void *ud);
void       pt_close(pt_State *S);
pt_Buffer    pt_empty(pt_State *S);
pt_Buffer    pt_from(pt_State *S, const char *s, size_t len);
int        pt_retain(pt_Buffer b);
int        pt_release(pt_Buffer b);
pt_Size    pt_bytes(pt_Buffer b);
int        pt_version(pt_Buffer b);
```

> `pt_empty(S)` 返回 `&S->empty`（state 内嵌 sentinel tree，零分配）。
> `pt_release` 遇 sentinel（含 from 链到达 sentinel）直接返回。
> `pt_from` 设置 `t->from = &S->empty`，from 链始终终止于 sentinel。
> 公共 API 均已正确处理 `NULL` 和 sentinel。

### 6.2 导航

```c
/* construction */
int  pt_seek(pt_Cursor *C, pt_Buffer b, size_t off); /* Cursor 构造器: reset + bind + 定位, 清 dirty */

/* navigate */
int  pt_locate(pt_Cursor *C, size_t off);  /* 纯绝对定位, 保留 dirty */
int  pt_advance(pt_Cursor *C, pt_Offset delta);

/* query */
#define pt_offset(C) ((C)->off + (C)->poff)
#define pt_buffer(C)   ((C)->tree)
```

- `pt_seek` 是 Cursor 构造器，重置所有内部状态（含 dirty），适合首次绑定 buffers
- `pt_locate` 和 `pt_advance` 用于已绑定 Cursor 的导航，保留 dirty 和 transaction 状态
- 无行级 API（无 `pt_seekline`/`pt_advline`/`pt_setcol`）— 行映射由外置 linecache 承担
- piece 级遍历：`pt_piece` 返回当前 piece 剩余数据；`pt_next` 前进一片并返回落脚片；
  `pt_prev` 后退一片并返回落脚片（两者对称，均为"移动后返回落脚点"语义）。
  全量遍历惯用法：`for (p = pt_piece(C,&n); n; p = pt_next(C,&n))`

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
- `del=0` → 纯插入；`len=0` → 纯删除
- `s` 生存期无要求（库已 copy）

**`pt_insert(C, s, len)`** — 插入 literal（引用语义，原位）

- 直接引用 `s`，不 copy
- `s` 必须存活至 `pt_close`
- 若 `s` 在 scratch pool 内，库自动消费 buffer 头并推进
- cursor 不移动

**`pt_append(C, s, len)`** — 插入 literal（引用语义，追尾）

- 同 `pt_insert`（literal 引用语义），但 cursor 追尾到插入内容之后
- 适用于批量载入文件块（配合 mmap 或 scratch）
- 实现上等价于 `pt_insert` + `pt_advance(len)`，但内部统一走 `ptI_splitins`

**实现注意事项**：`ptI_splitins` 仅处理 `ptM_upbytes`（度量），不做 `ptM_upmask`（mask 传播）。`pt_edit` 和 `pt_append` 的调用方负责在 `ptI_splitins` 后调用 `ptM_upmask`（见 §一.1 铁律）。

**`pt_splice(C, del, s, len)`** — splice via literal（引用语义，追尾）

- 先删除 cursor 位置起 `del` 字节
- 再插入 literal `s[0..len-1]`（引用语义）
- cursor 追尾到插入内容之后
- `del=0, s=NULL` → 纯删除（等价 `pt_remove`）
- `del=0` → 等价 `pt_append`
- 若 `s` 在 scratch pool 内，库自动消费 buffer 头

**`pt_remove(C, len)`** — 区间删除（原位）

- 删除 cursor 位置起 `len` 字节（含跨 piece/hole）
- 删除路径中若含 hole 且 hole 被清空（`n==0`），hole 对象立即回池
- 删除后检查相邻 literal 是否可合并（物理连续则合并）
- cursor 留在删除点

### 6.4 事务

```c
pt_Buffer pt_commit(pt_Cursor *C);
void        pt_rollback(pt_Cursor *C);
```

### 6.5 scratch pool

```c
char *pt_buffer(pt_State *S, size_t *plen);
```

返回 scratch pool 中当前写入头的指针和可用容量。
通过 `pt_insert`/`pt_append`/`pt_splice` 传递给库时，库自动消费该 buffer（推进写入头），
下次 `pt_buffer` 返回新区。

**已知问题（低优先级 TODO）**：`pt_buffer` 两次调用间若无 insert，返回同一块内存，
用户可能意外覆盖。编辑器开发中若实际踩坑，加引用保护。

---

## 七、commit 流程

```
pt_commit(C):
  1. 自 root DFS，利用内部节点 mask 跳过无 hole 子树
  2. 按顺序访问每个 hole：
     a. hole->data[0..n-1] append 到 scratch page
     b. 若跨页，自动分裂 literal
     c. children[i] = 新 literal 指针（指向 scratch 对应处）
     d. node->bytes[i] = n
     e. mask[i] = 0
     f. pt_Hole 对象回池
  3. 所有 hole 处理完后，检查相邻 literal 合并（物理连续则合并）
  4. 沿路径向上传播度量修正 + 清除内层 mask
  5. C->dirty = 0; 返回 b
```

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
| `PT_FANOUT`     | 节点最大子数            | 62                            |
| `PT_HOLE_CAP`   | hole 容量（字节）       | `64 - sizeof(unsigned short)` |
| `PT_MAX_LEVEL`  | 最大树深                | 16                            |
| `PT_PAGE_SIZE`  | 池分配器页大小          | 65536                         |
| `PT_STATIC_API` | 定义后所有函数为 static | —                             |

---

## 十、待定项

| 项目                         | 状态 | 说明                                                      |
| ---------------------------- | ---- | --------------------------------------------------------- |
| fanout 调优基准              | TODO | 首版用 62，后期 benchmark 决定                            |
| hole 容量调优基准            | TODO | 首版用 `64 - sizeof(unsigned short)`，后期 benchmark 决定 |
| `pt_buffer` 多次调用保护     | TODO | 编辑器开发后按需加引用保护                                |
| multi-cursor 同编辑态协同    | 未定 | API 设计待讨论                                            |
| 全局统计 `pt_chars/pt_lines` | 移除 | 由 linecache 承担                                         |

---

## 附录 A：术语表

| 术语       | 含义                                                                  |
| ---------- | --------------------------------------------------------------------- |
| literal    | append-only buffer 中的不可变数据段，叶层以 `char*` 裸指针引用        |
| hole       | 编辑态可变叶，固定容量，commit 后内容迁移至 scratch                   |
| transient  | 编辑态 tree（可变、独占、仅 Cursor 持有、不对外暴露）                 |
| persistent | 持久态 tree（immutable buffer，可 COW 共享）                          |
| mask       | `unsigned long long` 位图数组，标记 hole（叶层）或子树含 hole（内层） |

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
