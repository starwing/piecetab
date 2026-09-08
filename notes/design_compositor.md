# compositor.h 设计

> 状态：设计草案，尚未定案。本文是讨论与参考实现框架的记录，不是
> 权威实现标准；任何结构建议都可能被推翻。

## 0. 定位与边界

- `compositor.h` 是与 `spantree.h` 平行的 stb 风格单头文件库，使用
  C89 与 C++ 的交集（Clean C），可独立编译；不能为了 C89-only 特性牺牲
  C++ 兼容性。
- 层级：`compositor.h` 实现 mark/attr/priority/namespace 语义；绑定层
  平行组装 `spantree.h + compositor.h + 本地存储适配`；`editor.lua` 是
  应用层。
- `cp_State` 是一个文档/一棵 Tree 的语义状态。每个 Tree 绑定一个
  `cp_State`；Lua 绑定层可以任意创建。全局共享的 attr 注册表、名字
  映射、跨文档共享归绑定层。
- 字节长度归后端维护；文档编辑由绑定层调用后端原生编辑 API，并在
  `cp_edit_begin` 与 `cp_edit_end` 之间完成。attr id 由绑定层分配，
  compositor 保存、比较、透传。
- 清层入口有两个：`cp_nsclear` 清空一个 ns 的全部关联内容；
  `cp_clear(MC, len)` 从 mark cursor 当前位置清 `len` 字节。
  token ns 使用几何切分删除，object ns 使用 head-in 整对象删除。
- `spantree.h` 树叶直接存 `cp_Id`。`cp_Id` 是唯一 id 空间：一个
  mark 写树时用自己的 `cp_Id`；多个 mark 同时覆盖同一 run 时用
  combine id（也是 `cp_Id`）表达。compositor 负责 `cp_Id` 的分配、
  refcount、回收和 styled 合成。

## 1. 数据模型

### 1.0 State 内部组织

`cp_State` 持有 allocator/ud、epoch、ns 注册表、store 注册表、统一
`cp_Id` 池（`cp_IdInfo` + `idfree` + `max_id`）、`cp_Cons` 池 +
`consfree`、priority 表、sensitive 队列和内嵌 spanvec 后端。大 combine
使用独立 cpV_Header 数组。数组机制见 §1.0.1；字段骨架见 §1.1/§1.8；
object 专用结构留待 object ns 专题。

### 1.0.1 cpV_Header 数组机制

compositor 内部所有动态数组都使用同一套 cpV_Header 表示。数组在
内存中不是“一个 header 指针 + 单独的元素指针”，而是一块连续分配：

```text
[cpV_Header][elem 0][elem 1]...
```

其中 `cpV_Header` 记录 `len/cap`，`cp_State` 或局部变量保存的指针
指向 `elem 0` 的位置，而不是指向 header 的位置：

```c
typedef struct cpV_Header {
    size_t len, cap;
} cpV_Header;

#define cpV_hdr(a) ((cpV_Header *)(void *)(a) - 1)
#define cpV_len(a) ((a) ? cpV_hdr(a)->len : 0u)
#define cpV_cap(a) ((a) ? cpV_hdr(a)->cap : 0u)
```

例如：

```c
cp_Cons *cons; /* 指向 [cpV_Header][cons...] 中的第一个 cp_Cons */
```

读取长度/容量通过 `cpV_len(cons)` / `cpV_cap(cons)`，不需要在
`cp_State` 中再保存 `consn / conscap` 等冗余字段。扩容、回收由
cpV_Header 的辅助函数统一完成。

因此 `cp_State` 中全局数组字段只保存元素指针：

- `cp_NSReg *nsv`
- `cp_Store *stores`
- `cp_IdInfo *ids`
- `cp_Cons *cons`
- `cp_PrioRec *prios`
- `cp_Id *sensq`

freelist 头也是 `cp_State` 字段：

- `idfree`：一个 `cp_Id`。它指向 `ids` 中的一个空闲槽；该空闲槽的
  `data` 存下一个空闲 `cp_Id`，`0` 表示没有下一个。
- `consfree`：一个 cons 数组下标。它指向 `cons` 中第一个空闲槽；空闲
  槽的 `id1` 存下一个空闲 cons 下标，`(size_t)-1` 表示没有下一个。
  `cp_Cons` 的第二个字段在空闲时不使用。

`cp_IdInfo` 数组以 `cp_Id` 为下标时，若 `cp_Id` 采用 1-based，则实际
元素为 `ids[id - 1]`；若采用 0-based，则直接 `ids[id]`。`cp_Id` 的
`0` 语义见 §1.2。

大 combine 的成分数组也使用 cpV_Header 数组，例如：

```c
cp_Id *v; /* 指向 [cpV_Header][cp_Id...] */
```

读取长度用 `cpV_len(v)`。`cp_IdInfo[].data` 与 `ns` 的完整语义见
§1.2。

### 1.1 State / Store / Namespace

- `cp_State` 持有 ns 注册表、priority 表、统一 `cp_Id` 池、`cp_Cons`
  池。大 combine 的 cpV_Header 数组直接由 combine id 持有，不放在
  全局池。`cp_spanvec` 返回的 spanvec ops 表由实现段静态提供，不属
  于单个 State。
- `cp_Store` 是 opaque 指针类型。公开头只声明
  `typedef struct cp_Store cp_Store;`；实现段定义
  `struct cp_Store { cp_StoreOps *ops; cp_State *S; };`。
- `cp_nsnew / cp_nsclaim` 接收 `cp_StoreOps *ops`。后端约定把
  `cp_StoreOps` 作为后端结构体首字段；注册时传 `&backend->base`。
  compositor 按 ns 保存内部 `cp_Store` 包装，回调时经 `cp_ops(st)`
  恢复 backend。
- ns id 的分配权在调用方：`cp_nsnew` 让 compositor 取最小空闲正 id，
  并允许绑定层随后用 `cp_nsclaim` 指定后端可接受的 id。
- 一个 ns 绑定一个 store；多个 ns 可以共享同一个 store 实例。
  一个 State 内不同 store 实例数受 `CP_STORE_MAX` 限制。
- `cp_NS` 类型是 `int`：正数为有效 ns id，`cp_nsnew / cp_nsclaim` 用
  负的 `CP_ERR*` 值返回错误。
- namespace flags：
  - `0`：token ns，普通染色层；
  - `CP_IDENTITY`：object ns，mark 是独立对象；
  - `CP_SENSITIVE`：object ns 的修饰位，与 `CP_IDENTITY` 组合使用，
    作为该 ns 的默认 sensitive 语义。
- 绑定 `cp_spanvec(S)` 的 ns 具有 eph 语义：每次文档编辑结束后该
  ns 的全部染色与 object id 清空。

### 1.2 两个 id 空间：`cp_Attr` 与 `cp_Id`

- `cp_Attr`：全局不透明 attr id，`0 = CP_NOATTR`。attr 与 mark 的生命
  周期彼此独立；mark 携带 attr，attr 只影响 styled 输出，不参与 mark
  的身份、位置或生命周期。attr 不反向引用 mark。
- `cp_Id`：唯一 id 空间，没有区间划分。mark id、tree 中直接写的 id、
  combine id 都使用同一套 `cp_Id`。
  - `0` 不是合法 `cp_Id`，保留给“无 id/clear”语义。
  - 新 id 由 `cp_State.max_id` 或空闲链分配；所有 `cp_Id` 都允许回收。
  - 每次 mark 都分配新的 direct id；不在 `(ns, attr)` 上 intern 复用
    同一个 id。
  - 一个 run 只含一个 mark 时，直接把该 mark 的 `cp_Id` 写进
    spantree；含多个 mark 时分配 combine id，combine id 也是 `cp_Id`。
- 每个已分配 `cp_Id` 在 `cp_IdInfo[]` 中有一条记录：

  ```c
  typedef struct cp_IdInfo {
      int       ns;
      unsigned  refcount;
      ptrdiff_t data;
  } cp_IdInfo;
  ```

  - `ns > 0`：direct id。该 id 属于这个 ns 的一个 mark；
    `data` 直接保存该 mark 的 attr。
  - `ns == -1`：2 个成分的 combine id。`data` 是 cons 数组下标，
    对应一个 `cp_Cons { id1, id2 }`，两个字段都是成分 `cp_Id`。
  - `ns == -2`：3 个成分的 combine id。`data` 是 cons 链头下标：
    `cons[head].id1` 是第一个成分，`cons[head].id2` 是下一个 cons
    下标；`cons[next].id1/id2` 是第二、第三个成分。
  - `ns == -3`：超过 3 个成分的 combine id。`data` 是 `cp_Id *` 指针，
    指向一个独立 cpV_Header 数组；数组按序存放全部成分 `cp_Id`。
- refcount 规则：
  - direct id 被直接写进 spantree run 时 refcount+1；被 combine 的
    cons/vector 作为成分引用时也 refcount+1。
  - combine id 被写进 spantree run 时 refcount+1；combine id 不嵌套
    其它 combine id，vector 中只放 direct id。
  - refcount 归零后，id 进入空闲链可被复用。调用方不得在 refcount
    归零后继续使用该 `cp_Id`。

### 1.3 object 位置索引：候选 A 的详细结构（共享 AC + per-ns start-order 树）

> object ns 的语义与最终实现仍未定案。本节是 §1.5 候选 A 的详细
> 展开，只作结构描述，不代表已选方案。

候选 A 的形态：每个 `cp_State` 使用一份共享 anchor-container（AC），
并为每个 object ns 维护一棵 start-order 树。

#### anchor-container

- AC 是隐式 treap/rope：文本节点携带字节长度，锚点节点零长度。
- 所有 object ns 的 start/end 锚点都放入同一个 AC。
- 锚点节点记录所属 object、端点类型 `START/END`、treap 链接与子树
  字节和；端点的 left/right 位不单独存储，按 object 的 `cp_Gravity`
  与端点类型由内部宏推导。
- 锚点的当前绝对位置由 AC 中序前缀和得到。
- AC 对文档编辑提供整体平移能力：一次插入/删除通过树分合移动所有
  右侧锚点，对象个数保持在局部处理复杂度之外。

#### object 记录

- object 记录保存：ns、attr、raw leaf（attr 非 0 时存在）、start/end
  锚点指针、`cp_Gravity gravity`、`sensitive`、存活状态。
- `cp_range` 从 AC 计算 start/end 两个锚点位置，得到当前完整逻辑区间。
- 数值位置作为查询结果，AC 与锚点指针是位置真值源。

#### start-order 树

- 每个 object ns 一棵平衡树，按 `(start asc, id asc)` 排序。
- 树节点保存 object 引用，并保存子树 `best`：该子树中当前 end 最大的
  object。
- 节点键通过 AC 中 start 锚点的左右序比较；同 start 再按 id 比较。
- 文本编辑导致大量对象整体平移时，AC 中的锚点相对序保持稳定，
  start-order 树无需全树重排。
- 需要重排的场景是 seam 组变化：
  - 插入在 `start == p` 处按 gravity 把 seam 组稳定分区；
  - 删除把 start 位于删除区间内的对象塌缩到同一 seam，并按 id 排序。
- start-order 树只包含该 ns 的 live object，大小为 `O(m_ns)`，与文本
  长度骨架解耦。

#### 与 spantree 的分工

- 只有 `attr != CP_NOATTR` 的 object 写入 spantree，成为 styled 贡献。
- `attr == CP_NOATTR` 的 object 只存在于 compositor 的 AC 与
  start-order 树中，位置、遍历、head-in 删除语义照常提供。
- styled/seekstyled 读取 spantree 结果；compositor 位置索引用于
  object API，并负责编辑后把 spantree 修正到 object 语义一致的状态。

### 1.4 mark / attr 契约

- mark 与 attr 没有身份或生命周期关系。mark 的 attr 只决定 styled
  是否输出该 mark 的贡献：`attr == CP_NOATTR` 时 styled 忽略。
- token ns：attr 必须非 0；在 token ns 上写 mark 就是给该 ns 染色。
  同 ns 后写覆盖先写。
- object ns：attr 可为 0；多个 object 可以重叠（含同 ns 多 object），
  每个 object 的 `cp_range` 返回其完整逻辑区间，不受其他 object
  覆盖影响。attr 非 0 时，spantree 后端在同段用 cons 链中的多个同
  ns leaf 表达重叠；spanvec 后端用重叠 segment 表达，语义等价。
- `CP_NOATTR` 是透明/无贡献的表达。

### 1.5 object 位置索引的结构选择（待定）

object 位置索引决定 object 如何在文本编辑中保持位置。结构空间如下。

#### A：共享 AC + per-ns start-order 树

详细结构见 §1.3；这里只保留选择理由。A 的基本分工是：一个全局 AC
保存文本长度与所有 object 锚点；每 object ns 一棵 start-order 树负责
该 ns 的 `(start, id)` 序遍历与 `end > pos` 剪枝。

收益：

- per-ns `cp_seek/next` 不需要扫描其它 ns；
- 文本编辑的平移成本可做到 `O(logA + 局部处理)`。

代价：

- 同一份位置信息由 AC 和 start-order 树协同维护，seam 变化时需要两
  处同步；
- 同 seam 锚点排序、gravity 分区、删除塌缩都要在两棵树之间保持一
  致。

#### B：单棵按位置排序的 marktree

所有 object 的 start/end 作为独立 key 放进同一棵按位置排序的平衡树；
key 上编码 ns、id、start/end、gravity。另维护 id → 节点的反查表。

收益：

- 只有一棵位置树；
- id 反查/删除直接。

代价：

- per-ns 遍历需要过滤其它 ns，或需要额外的 ns 级元数据才能做到
  `O(log m)` 剪枝；
- 成对 start/end key 的编辑移动需要单独处理。

#### C：object 完全走 compositor 自己的区间索引，不进 spantree

object ns 只存在于 compositor 的位置索引中；object 的 attr 作为区间
贡献参与 styled，但不写后端 raw leaf。这样 composite raw leaf 只需表
达 token ns 的每 ns 一个贡献。

收益：

- object 生命周期完全自洽；
- 没有“同 ns 多 object 重叠 → cons 链多 leaf”的情况；
- composite 可退化为“每 ns 至多一个 leaf”的简单表示。

代价：

- styled 需要合并 token 后端与 object 区间索引两个来源；
- object 数量很大时区间 sweep 的复杂度需要评估。

### 1.6 combine id 的表示

`info[id].ns` 与 `data` 的三种 combine 布局由 §1.2 统一定义，这里只
给出读取与重建规则。以三个成分 `ID1、ID2、ID3` 为例：

```text
cons[0] = { ID1, 1 }
cons[1] = { ID2, ID3 }
info[id].ns  = -2
info[id].data = 0
```

读取 combine id 时根据 §1.2 的 `ns` 模式展开，得到该 run 的全部
direct id；每个 direct id 的 attr 从各自 `info[direct].data` 取得。

所有 combine 表示都必须满足 canonical 展开结果一致：同一次 merge
的展开列表排序、去重后重建；combine 只保存重建后的成分。删除到
只剩 1 个 direct id 时，不保留 combine，直接写回该 direct id。

### 1.7 结构取向（旧草案归档）

“object 不进 spantree、styled 多源合并、composite 只表达 token 层、
public id 使用 generation”等旧草案取向已在 §1.4 候选记录与 §11 待定
项中覆盖；正文不再单独展开，避免与候选 A/B/C 重复。

### 1.8 `cp_State` 内部组织

下面给出 `cp_State` 内部组织的结构骨架。Clean C 实现可能需要“头部分配 +
零长度数组”之类的技巧，此处先表达逻辑布局。

```c
struct cp_State {
    cp_Alloc *alloc;
    void     *ud;
    cp_Epoch  epoch;

    /* 下列全部是 cpV_Header 数组，只保存元素指针 */
    cp_NSReg   *nsv;
    cp_Store   *stores;
    cp_IdInfo  *ids;
    cp_Cons    *cons;
    cp_PrioRec *prios;
    cp_Id      *sensq;

    /* 空闲链头；数组 len/cap 由 cpV_Header 提供 */
    cp_Id       idfree;
    size_t      consfree;
    cp_Id       max_id;

    /* 内嵌 spanvec 后端的共享状态 */
    cp_SpanState span;
};
```

需要单独细化的小节：

- `cp_NSReg`：ns id、flags、指向 store 的索引/指针、每 ns 的 token id
  聚合/缓存；
- `cp_IdInfo`：direct/combine 标记、refcount、data 语义；
- `cp_Cons`：2/3 成分 combine 的槽位；
- `cp_Id *`：>=4 成分 combine 直接持有的 cpV_Header 数组；
- `cp_Store`/store 表：ops、S、backend 包装、`CP_STORE_MAX` 计数。

## 2. 头文件形态

- 标准 stb 形态：`compositor_h` guard、`CP_NS_BEGIN/CP_NS_END`、
  `CP_API`、`CP_STATIC_API`、`CP_IMPLEMENTATION`。
- 头文件分两段，面向两个不同读者：
  - application API：应用层创建 State、注册 ns、写 mark、编辑边界、
    迭代、styled；
  - StoreOps provider interface：`cp_Id / cp_Seg / cp_StoreCursor /
    cp_StoreOps` 与事件函数，供后端适配者实现和调用。
  两段顺序固定：application API 在前，provider interface 在后。
- 用户经统一配置头间接包含 compositor.h。配置头在 include 前定义完整
  的 `union cp_OpsCursor` tag；compositor.h 再
  `typedef union cp_OpsCursor cp_OpsCursor;` 并组成
  `cp_StoreCursor`。所有 TU 使用同一配置头，保证布局一致。
- 可覆盖宏：
  - `CP_STORE_MAX`：一个 State 内最大 store 实例数，默认 4。
- combine id 的 cons/vector 与 styled 归并按 State 当前数据动态分配。

## 3. API

```c
#define CP_OK        (0)
#define CP_ERRPARAM  (-1)
#define CP_ERRMEM    (-2)
#define CP_ESTALE    (-3)
#define CP_ELIMIT    (-4)

#define CP_NOATTR    ((cp_Attr)0)
#define CP_IDENTITY  (1 << 0)
#define CP_SENSITIVE (1 << 1)   /* modifies CP_IDENTITY */

#ifndef CP_STORE_MAX
#define CP_STORE_MAX 4
#endif

CP_NS_BEGIN

/* ========================= application API ========================= */

typedef struct cp_State cp_State;
typedef struct cp_Store cp_Store;
typedef struct cp_StoreOps cp_StoreOps;
typedef struct cp_MarkCursor cp_MarkCursor;
typedef struct cp_StyledCursor cp_StyledCursor;

typedef void *cp_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

typedef int    cp_NS;
typedef size_t cp_Id;
typedef size_t cp_Attr;
typedef size_t cp_Epoch;

typedef struct cp_Range { size_t start, end; } cp_Range;
typedef struct cp_Mark  { cp_Range range; cp_Id id; cp_Attr attr; } cp_Mark;
typedef struct cp_Styled { cp_Range range; cp_Attr attr; int last; } cp_Styled;

typedef enum cp_Gravity {
    CP_GROW,   /* both ends absorb boundary insertions */
    CP_FIXED,  /* both ends reject boundary insertions */
    CP_SHIFT,  /* start rejects, end absorbs */
    CP_LOCK    /* start absorbs, end rejects */
} cp_Gravity;

typedef struct cp_MarkOpt {
    cp_Gravity gravity; /* CP_GROW / CP_FIXED / CP_SHIFT / CP_LOCK */
    int        sensitive; /* 0/1 */
} cp_MarkOpt;

typedef enum cp_EditOp {
    CP_EDIT_REMOVE,
    CP_EDIT_APPEND,
    CP_EDIT_INSERT
} cp_EditOp;

typedef struct cp_Edit {
    cp_EditOp op;
    size_t    pos;
    size_t    len;
} cp_Edit;

/* lifecycle */
CP_API int  cp_init(cp_State *S, cp_Alloc *alloc, void *ud);
CP_API void cp_free(cp_State *S);
CP_API void cp_reset(cp_State *S);

/* namespace */
CP_API cp_NS cp_nsnew  (cp_State *S, cp_StoreOps *ops, int flags);
CP_API cp_NS cp_nsclaim(cp_State *S, cp_NS ns, cp_StoreOps *ops, int flags);
CP_API int   cp_nsdel  (cp_State *S, cp_NS ns);
CP_API int   cp_nsclear(cp_State *S, cp_NS ns);

/* document edit boundary */
CP_API int     cp_edit_begin(cp_State *S, const cp_Edit *edit);
CP_API int     cp_edit_end(cp_State *S);
CP_API cp_Epoch cp_epoch(const cp_State *S);

/* attr priority */
CP_API int cp_setpriority(cp_State *S, cp_Attr attr, int prio);

/* marks */
CP_API int cp_mark(cp_State *S, cp_NS ns, cp_Attr attr, cp_Range r,
                   const cp_MarkOpt *opt, cp_Id *id);
CP_API int cp_unmark(cp_State *S, cp_Id id);
CP_API int cp_range(cp_State *S, cp_Id id, cp_Range *out);
CP_API cp_Attr cp_attr(cp_State *S, cp_Id id);

/* mark iteration */
CP_API int cp_seek(cp_MarkCursor *MC, cp_State *S, cp_NS ns, size_t pos);
CP_API int cp_next(cp_MarkCursor *MC, cp_Mark *out);
CP_API int cp_clear(cp_MarkCursor *MC, size_t len);

/* styled iteration */
CP_API int cp_seekstyled(cp_StyledCursor *SC, cp_State *S, size_t pos);
CP_API int cp_nextstyled(cp_StyledCursor *SC, cp_Styled *out);

/* embedded spanvec reference backend */
CP_API cp_StoreOps *cp_spanvec(cp_State *S);

/* ===================== StoreOps provider interface ===================== */

/* No separate cp_Leaf: store callbacks receive the same cp_Id that the
 * application-facing compositor uses. 0 remains the clear/no-id value. */

#define cp_ops(st) (*(cp_StoreOps **)(void *)(st))

typedef struct cp_SVCursor { size_t pos; } cp_SVCursor;
typedef union cp_OpsCursor cp_OpsCursor; /* tag defined by config header */
typedef union cp_StoreCursor {
    cp_SVCursor  sv;
    cp_OpsCursor ops;
} cp_StoreCursor;

typedef struct cp_Seg { cp_Range range; cp_Id id; } cp_Seg;

struct cp_StoreOps {
    int (*write)(cp_Store *st, cp_NS ns, cp_Id id, cp_Range r);
    int (*clearleaf)(cp_Store *st, cp_NS ns, cp_Id id);
    int (*clearall)(cp_Store *st, cp_NS ns);
    int (*seek)(cp_Store *st, cp_NS ns, size_t off, cp_StoreCursor *c);
    int (*next)(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o);
    int (*prev)(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o);
};

/* store events, called by StoreOps implementations */
CP_API cp_Id cp_born(cp_Store *st, cp_Id id);
CP_API void  cp_died(cp_Store *st, cp_Id id);
CP_API void  cp_touch(cp_Store *st, cp_Id id);
CP_API cp_Id cp_mergewrite(cp_Store *st, cp_NS ns, cp_Id in, cp_Id old);
CP_API cp_Id cp_mergeclear(cp_Store *st, cp_NS ns, cp_Id old);
CP_API cp_Id cp_mergecut(cp_Store *st, cp_NS ns, cp_Id in, cp_Id old);

/* value cursor definitions: applications allocate these by value */
struct cp_MarkCursor {
    cp_State *S;
    cp_NS     ns;
    size_t    pos;
    cp_Epoch  epoch;
    void     *node;   /* object ns start-order tree node */
    size_t    segi;   /* token/spanvec iteration state */
    size_t    segn;
};

struct cp_StyledCursor {
    cp_State *S;
    size_t    pos;
    cp_Epoch  epoch;
    cp_Range  group;
    size_t    nattr;
    size_t    iattr;
};

CP_NS_END
```

## 4. 语义契约

### 4.1 生命周期

- `cp_init` 初始化 `S`。失败时释放已建内容并把 `S` 归零，返回
  `CP_ERRMEM`。
- `cp_free` 释放 State 全部内部内存；外部 store 的生命周期归调用方。
- `cp_reset` 清空全部 ns、mark、priority、spanvec 内容与位置索引，
  epoch 置 0；allocator 保留。

### 4.2 namespace 与 cursor 区间清

- `cp_nsnew(S, ops, flags)` 由 compositor 分配最小空闲正数 `cp_NS`。
- `cp_nsclaim(S, ns, ops, flags)` 由调用方指定正数 ns 精确注册。
  两者共同校验 `S / ops` 有效、`CP_SENSITIVE` 与 `CP_IDENTITY` 组合、
  新 store 实例数在 `CP_STORE_MAX` 内。
- `cp_nsclear` 清空该 ns 的全部关联内容：
  - object id 全部失效并从位置索引删除；
  - token direct id 的 tree refcount 清 0，id 可回收；上层不得在清空后
    继续使用旧 id。
- `cp_clear(MC, len)` 从 `MC->pos` 清 `[pos, pos + len)`：
  - token ns：几何区间清，删除区间内 token 贡献，区间外部分保留；
  - object ns：head-in 删除所有当前 start 落在区间内的整个 object；
  - `len == 0` 返回 `CP_OK` 且 epoch 保持；
  - mark 是覆盖在文本上的语义，删除 mark 不改变实际文本；
  - 成功后 State epoch 加一，`MC` 同步到新 epoch 且 `pos += len`；
    后续 `cp_next` 从这个新位置正常继续。
- `cp_nsdel` 先清空 ns 再注销；成功后外部 store 可复用或释放。

### 4.3 attr / priority

- `cp_setpriority(S, attr, prio)` 为 attr 设置优先级；`CP_NOATTR` 返回
  `CP_ERRPARAM`。
- 默认 priority 为 0。styled 组内顺序为 priority 升序，同 priority
  按 attr 数值升序；同 attr 输出一次。
- priority 变更成功会使 epoch 加一，旧 styled cursor 失效。

### 4.4 mark / unmark / range / attr

- `cp_mark`：
  - token ns：`r.start >= r.end` 返回 `CP_ERRPARAM`；每次调用分配一个
    新的 direct id，不在 `(ns, attr)` 上 intern 复用；
  - object ns：允许零长度对象（`r.start == r.end`）；每次返回新
    object id，attr 可为 0，按 `cp_MarkOpt` 保存 start/end gravity 与
    sensitive；ns 级 `CP_SENSITIVE` 作为默认值，`cp_MarkOpt.sensitive`
    对单个 mark 生效；
  - object 创建时在共享 AC 插入两个锚点，并写入该 ns 的
    start-order 树；attr 非 0 时把 raw leaf 写入 spantree；
  - 成功时 epoch 加一。

gravity 在公开 API 上是一条 mark 一个 `cp_Gravity`，共四种模式；内部
实现再把模式展开成 start/end 两个端点的 left/right 位（用内部宏或
helper 现场判断，不暴露 left/right 命名）。四种模式对应真实插件需求
（Neovim 的 snippet 同时使用 expand/shift/lock/static 四类）：

| cp_Gravity | start 端点 | end 端点 | 左边界插入 | 右边界插入 | 直观含义 |
|---|---|---|---|---|---|
| CP_GROW | LEFT | RIGHT | mark 吞入左侧文本 | mark 向右扩展 | 双向 grow |
| CP_FIXED | RIGHT | LEFT | mark 被推开 | mark 不扩展 | 双向固定/static |
| CP_SHIFT | RIGHT | RIGHT | mark 整体右移 | mark 向右扩展 | shift 型非对称 |
| CP_LOCK | LEFT | LEFT | mark 向左扩展 | mark 不扩展 | lock 型非对称 |

- 内部插入总是使 mark 长度增加，与 gravity 无关。
- 零长度 mark 的两个锚点可位于同一 seam，仍按 `cp_Gravity` 展开成
  两个端点位；编辑后可能保持零长或产生正长度区间。
- `cp_MarkOpt` 的 `sensitive` 是 per-mark 覆盖。
- `cp_unmark`：
  - object id：有效时从 spantree 删除 leaf（attr 非 0 时）、从
    start-order 树删除节点、从 AC erase 两个锚点，返回 1；
  - token id：删除该 direct id 当前在 tree 中仍存在的贡献，返回清除
    的段数；现存段数为 0 时返回 0；
  - 成功时 epoch 加一。
- `cp_range`：
  - object id：从 AC 查询 start/end 锚点位置，返回当前完整逻辑区间；
  - token id 或失效 id：返回 `CP_ERRPARAM`。
- `cp_attr`：object/token id 有效时返回关联 attr；无 attr 与无效 id
  返回 0。

### 4.5 文档编辑与 epoch

- `cp_edit_begin(S, edit)` 接收 `remove / append / insert` 三类编辑
  描述，建立编辑上下文并完成预检。
- 替换不是单一编辑：它永远是 remove 后接 append/insert 两个操作。
  gravity/sensitive 语义必须保证“两个操作的非原子替换”与“原子替换”
  等价；若不等价，则修改语义直到等价。
- 绑定层在 `cp_edit_begin` 与 `cp_edit_end` 之间执行后端原生编辑；
  编辑期间 spantree 的 touch/merge 回调进入 compositor 修正模式，
  按 object gravity/sensitive 调整新增或收缩 run 的 composite。
- sensitive object 的定义：任何导致该 object 内容改变的编辑都触发
  sensitive 失效；除此以外不触发。可把它理解为“per-object 的 eph ns”。
- `cp_edit_end` 依次：
  1. 更新共享 AC 与各 object ns 的 start-order 树；
  2. 对编辑期登记的 sensitive object 执行整对象 `clearleaf`；
  3. 清空所有绑定 `cp_spanvec` 的 eph ns；
  4. epoch 加一。
- mark 写删（`cp_mark / cp_unmark / cp_nsclear / cp_clear /
  cp_setpriority`）各自独立推进 epoch；文档编辑由
  `cp_edit_begin/end` 统一推进。
- 失败时编辑上下文保留并允许重试；已完成的更新阶段保持幂等，
  AC/start 树与敏感队列保持一致。

### 4.6 mark 迭代

- `cp_seek(MC, S, ns, pos)` 定位到第一个 `end > pos` 的公开 mark；
  `cp_next` 返回并推进到“下一个” mark。
- “下一个”的具体排序不是公开语义，可以由实现定义并保持一致；
  唯一要求是：从 `cp_seek` 返回的 mark 开始，连续 `cp_next` 能按相对
  稳定的顺序遍历到该 ns 的最后一个 mark。token ns 同理。
- token ns 输出 raw run；object ns 从该 ns 的 start-order 树输出完整
  object。
- object ns 的 `cp_seek` 使用 start-order 树与子树 `best` 剪枝。
- cursor epoch 与 State epoch 一致时有效，否则返回 `CP_ESTALE`。

### 4.7 styled 迭代

- `cp_seekstyled(SC, S, pos)` 定位到第一个 `end > pos` 的贡献组；
  窗口裁剪由消费者完成。
- `cp_nextstyled` 输出 `cp_Styled { range, attr, last }`：
  - `range` 是当前组的完整区间；
  - 组 = attr **集合**恒定的最大区间；同一个 attr 无论来自多少 mark
    都只输出一次；`CP_NOATTR` 不参与集合；
  - 组内按 `(priority asc, attr asc)` 输出（只作稳定顺序，不改变集合）；
  - `last == 1` 表示组尾；
  - styled 输出贡献非空的区间。
- 组归并使用实现段内部动态数组完成；cursor 值类型持有组位置与索引，
  `cp_nextstyled` 每次按 `(group, iattr)` 重建当前组并返回对应项。

### 4.8 spanvec 后端

- `cp_spanvec(S)` 返回 spanvec 的 `cp_StoreOps` 表，可反复传入
  `cp_nsnew` 注册多个 ns。
- token ns：每个 ns 一条平铺 segment 列表，后写覆盖先写。
- object ns：每个 ns 保存写入后端的 attr 非 0 object span，允许重叠；
  每个 object 保持独立区间。
- token ns 的区间清由 compositor 调 spanvec 的 `write(id == 0)` 完成；
  object ns 的区间清由 compositor 走 head-in id 删除。
- spanvec 的 `clearleaf / clearall` 为删除型回调，零分配。
- spanvec 上注册的 eph ns 在每次 `cp_edit_end` 后清空。

## 5. StoreOps 契约

### 5.1 通用

- 回调首参是 compositor 内部 `cp_Store` 包装；后端经
  `(Backend *)cp_ops(st)` 恢复自己。
- `write / clearleaf / clearall / seek` 返回 0 成功，负值为
  `CP_ERR*`；`next / prev` 返回 1 有项、0 结束、负值错误。
- `write` 全有或全无：返回错误时后端存储保持原状。
- `clearleaf / clearall` 是删除型回调，OOM 恒成功。
- `write` 收到的 `id` 是 compositor 提供的 `cp_Id`，可能是 direct id
  或 combine id。backend 只把它当作不透明 id 交给 arbiter；arbiter
  返回的新 id 是最终存储值。
- `id == 0` 表示 clear 操作：`write(st, ns, 0, r)` 清除该 ns 在
  `[r.start, r.end)` 内的全部贡献。后端在该形态下让 arbiter 走
  `cp_mergeclear` 路径。
- 后端在 merge 路径调用 §6 的事件函数维护 compositor 状态。

### 5.2 write

- `id != 0`：把 mark/combine id `id` 写入 `[r.start, r.end)`。
- `id == 0`：删除 `[r.start, r.end)` 内 ns 的全部贡献，其它 ns 的
  贡献保持原样。
- 超出当前字节范围的区域由后端按自身规则补齐空洞。
- 与已有段合并时，后端 arbiter 调 `cp_born` 或 `cp_mergewrite`；
  clear 形态调 `cp_mergeclear`。跨边界段由后端切分，区间外部分保留
  剩余 id。

### 5.3 clearleaf / clearall

- `clearleaf(st, ns, id)` 删除 ns 内所有包含 direct id `id` 的贡献。
  合并段经 arbiter 调 `cp_mergecut`。
- `clearall(st, ns)` 删除 ns 内全部贡献。合并段经 arbiter 调
  `cp_mergeclear`。

### 5.4 seek / next / prev

- `seek(st, ns, off, c)` 把 cursor 定位到第一个 `end > off` 的段。
- `ns > 0` 只返回含该 ns 贡献的段；`ns == 0` 返回全部段。
- `next / prev` 按段的 `(start asc, id asc)` 序移动，返回完整段区间
  与 id。
- 同一 store 被多个 ns 共享时，`cp_StoreCursor` 只记录位置；后端在
  回调内按 `ns` 参数选择数据。
- 后端 cursor 是值类型；`seek` 完整初始化，`next/prev` 读取
  `seek` 写入的字段。

## 6. 事件协议与资源契约

事件由 StoreOps 实现调用，首参是 `cp_Store` 包装：

- `cp_born(st, id)`：后端新建对 id `id` 的引用，refcount+1。`id` 可
  以是 direct id，也可以是 combine id。
- `cp_died(st, id)`：后端失去对 id `id` 的一个引用，refcount-1。
  refcount 归零时释放 combine id 的 cons/vector 结构，并对其中的
  direct id 逐一 refdown。
- `cp_touch(st, id)`：id 的字节长度被文档编辑改变。该事件在
  `cp_edit_begin/end` 上下文中参与 combine 修正，并登记 sensitive
  对象。
- `cp_mergewrite(st, ns, in, old)`：把 `in` 合并进旧段 id `old`。
  展开 old 后按 ns 类型处理：
  - token ns：删除 old 中同 ns 的全部旧贡献，再追加 `in`；
  - object ns：保留 old 中同 ns 的旧 direct id，追加 `in`。
  按 `(ns asc, id asc)` 排序后重建 direct/combine id，维护 refcount，
  并按 touch/died 规则登记 sensitive 对象。
- `cp_mergeclear(st, ns, old)`：展开 old，删除该 ns 的全部贡献，重建
  direct/combine id。
- `cp_mergecut(st, ns, in, old)`：展开 old，只删除 `(ns, in)` 这一个
  贡献，重建 direct/combine id。

资源契约：

- combine 展开使用 State 的 allocator-backed 动态存储，成分数按实际
  mark 数量增长。
- mark/unmark/clear 等 mutator 在调用 mutating StoreOps 回调前，用
  `seek(ns==0) / next` 只读扫描本次实际受影响的段，精确预留全部
  id/cons/vector 资源与展开 scratch；回调内事件只消费预留资源。
- 文档编辑由 `cp_edit_begin` 预检并预留编辑期可能产生的 combine
  结构；`spA_touch` 写回路径的事件消费预留资源。
- 事件回调在预检过的 StoreOps 回调内保持零分配。

## 7. OOM 与状态一致

| mutator | 失败返回 | 失败后的状态 |
|---|---|---|
| `cp_init` | `CP_ERRMEM` | `S` 全零 |
| `cp_nsnew` | `CP_ERRPARAM / CP_ERRMEM / CP_ELIMIT` | 注册表保持原状 |
| `cp_nsclaim` | `CP_ERRPARAM / CP_ERRMEM / CP_ELIMIT` | 注册表保持原状 |
| `cp_mark` | `CP_ERRPARAM / CP_ERRMEM` | mark 集合、存储与 epoch 保持原状 |
| `cp_unmark` | `CP_ERRPARAM / CP_ERRMEM` | 目标 mark、存储与 epoch 保持原状 |
| `cp_nsclear` | `CP_ERRPARAM / CP_ERRMEM` | 该 ns 内容与 id 保持原状 |
| `cp_clear` | `CP_ESTALE / CP_ERRPARAM / CP_ERRMEM` | 失败时内容、id、epoch、`MC` 原状；成功时 State epoch++，`MC` 同步且 `pos += len` |
| `cp_nsdel` | 同 `cp_nsclear` | 注册与内容保持原状 |
| `cp_setpriority` | `CP_ERRPARAM / CP_ERRMEM` | priority 与 epoch 保持原状 |
| `cp_edit_begin` | `CP_ERRPARAM / CP_ERRMEM` | 编辑上下文与全部内容保持原状 |
| `cp_edit_end` | `CP_OK / CP_ERRMEM` | sensitive 清理失败时队列保留，epoch 已加一，可重试 |
| `cp_seek / cp_next / cp_seekstyled / cp_nextstyled` | `CP_ESTALE / CP_ERRPARAM / CP_ERRMEM` | cursor 状态见 §4 |

## 8. 范围与 Lua 绑定

- 文档编辑走绑定层原生 API + `cp_edit_begin/end`；StoreOps 面是
  `write / clearleaf / clearall / seek / next / prev`。
- 字节长度归后端；compositor 公开面提供语义 API。
- 清层入口是 `cp_nsclear`（整层）与 `cp_clear(MC, len)`（区间）。
- attr id 的分配、解释与 string key 映射归绑定层。
- Lua 绑定提供 mark/unmark/clear/span/styled 子集，object ns 使用
  per-mark gravity/sensitive 参数。
- Comp 维护普通 ns 与 eph ns 两个不相交 ns id 池。Tree 用
  `cp_nsclaim` 精确注册，并本地保存已注册的 `{nsid, gen, mode, prio}`
  列表。
- Comp 的 `namespace(name, prio, flags)` 更新 comp 注册表；注销、
  reprio、普通↔eph 换 mode 时 `gen++`。每个公开 Tree 方法与迭代器在
  使用前做惰性对账：gen/mode 变化时重建本地注册，prio 变化时同步
  `cp_setpriority`。
- Lua `clear(ns, off, len)` 由绑定层 `cp_seek` 到 `off` 后调
  `cp_clear(MC, len)`。
- object ns 的 `span` 从 compositor 的 start-order 树输出完整对象，
  styled 仍从 spantree 输出。

## 9. 复杂度

参数记号：

- `A`：AC 中文本节点与锚点节点总数。
- `m`：当前 object ns 的对象数。
- `R`：spantree run 数。
- `F_x`：包含 object x 的 spantree run 数。
- `C`：composite 链平均长度。
- `logA = O(log(A + N))`。

| 操作 | metadata 复杂度 | spantree 复杂度 |
|---|---|---|
| `cp_mark` | `O(log m · logA)` | attr 非 0 时 `O(log R + F_new·C)` |
| `cp_unmark` | `O(log m + logA)` | attr 非 0 时 `O(log R + F_x·C)` |
| `cp_range` | `O(logA)` | 0 |
| `cp_seek` | `O(log m · logA)` | 0 |
| `cp_next` | 分摊 `O(1)`，最坏 `O(log m)` | 0 |
| `cp_clear` | 定位 `O(log m·logA)` + 每个被删对象 `O(logA + log m)` | 每个 attr 非 0 被删对象 `O(log R + F_x·C)` |

编辑复杂度用以下局部量表示：

- `qA`：恰在插入 seam 或删除区间内的锚点数；
- `qS`：需要 seam 重排的 start 节点数；
- `qD`：本次死亡并从 metadata 删除的对象数；
- `qClear`：需要整对象 `clearleaf` 的 sensitive 数；
- `Cb`：边界 composite 需重建的 leaf 总数。

| 编辑阶段 | 复杂度 |
|---|---|
| spantree 原生编辑 | `O(log R + touched runs)` |
| `spA_touch` 边界修正 | `O(log R + Cb)` |
| AC 更新 | `O(logA + qA)` |
| start 树 seam 重排 | `O(log m·logA + qS log m)`，seam 组批量操作可优化 |
| sensitive `clearleaf` | `qClear · O(log R + F·C)` |
| 死亡对象 metadata 删除 | `qD · O(logA + log m)` |

共享 AC 使内部插入/删除的 metadata 成本保持 `O(logA + 局部处理)`，
与覆盖该区间的对象数量解耦。

## 10. spantree touch 扩展契约

- `spA_touch` 支持返回新 id 并写回。
- `db > 0` 且返回原 id：run 整体变长，composite 保持。
- `db > 0` 且返回新 id：新增区切为独立 run，旧区保持原 id。
- `db < 0`：若 surviving run 的 composite 需要变化，remaining run
  写入新 id。
- 编辑路径向 compositor arbiter 提供 seam 上下文：
  - 增长/收缩方向；
  - 内部/左边界/右边界；
  - 边界插入时的默认继承侧 composite 与另一侧 peer composite。
- compositor 在 `cp_edit_begin/end` 上下文中展开 composite，按
  object gravity/sensitive 重建新 run 的 leaf 集合。
- sensitive object 若被编辑触碰但仍有其他碎片存活，由 `cp_edit_end`
  统一执行整对象 `clearleaf`。

## 11. 测试计划

- 以朴素区间列表为 reference，随机对拍 mark/unmark/clear、随机
  remove/append/insert、随机 gravity/sensitive/重叠。
- 覆盖边界：`p<s`、`p==s`、`s<p<e`、`p==e`、`p>e`；删除前缀/内部/
  后缀/整对象/边界缝。
- 覆盖同一 seam 大量对象混合 gravity，以及删除后多个 start 塌缩到
  同一 seam。
- 双后端对拍：spanvec 后端与 spantree 后端验证 object API 与 styled
  输出一致。
- 每次操作后检查 `cp_range`、`cp_seek/next` 顺序、`cp_clear` head-in
  集合、sensitive 存活规则、spantree 结构不变式与 styled 输出。
- OOM 注入覆盖 `cp_edit_begin` 预检、原生编辑失败、
  `cp_edit_end` sensitive clearleaf 失败重试。
- 专项测试 attr=0 的 object 位置/遍历/head-in 删除，以及 attr 非 0
  的 spantree 同步。

## 12. 待定问题清单

> 本节汇总当前文档中尚未收敛、后续必须讨论清楚的项目。它们不是
> “历史遗留”，而是当前设计的真实开放点。

### 12.1 总体结构

- [ ] §1.5：object 位置索引最终采用 A / B / C 中的哪一种。
- [ ] §1.7：object ns 的结构取向与 styled 来源关系（object 专题，
      尚未讨论）。
- [ ] object ns 相关结构是否写 spantree 与 styled 合并的最终关系。

### 12.2 `cp_State` 内部组织

- [ ] ns 注册表使用 dense 数组 + free list，还是稀疏表/哈希。
- [ ] store 注册表的结构；`CP_STORE_MAX` 是否保留；同一个 backend
      实例被多个 ns 共享时 `cp_Store` 如何表示和索引。
- [ ] `cp_IdInfo / cp_Cons / vector` 的容量增长、空闲链与 refcount
      回收细节。
- [ ] 所有 `cp_Id` 可回收后，旧句柄复用语义如何对外说明；token/object
      的公开 id 生命周期是否还需要额外代际保护。
- [ ] priority 表与 sensitive 队列的表示。
- [ ] `cp_State` 是否直接持有 object 位置索引；若是，最终采用哪一种
      索引结构。
- [ ] `cp_State` 中是否区分普通 ns 与 eph ns 两套注册空间，以及如何
      支持高 id 的 eph ns。

### 12.3 API 与 cursor

- [ ] `cp_MarkCursor` 的完整字段布局与 C++ 兼容表示。
- [ ] `cp_StyledCursor` 是否维持“每次重建组”的模型；若不维持，缓存
      放在哪里。
- [ ] `cp_clear(MC, len)` 的内部删除顺序与后端调用算法。
- [ ] `cp_seek / cp_next` 对 token ns 的具体迭代算法与复杂度。

### 12.4 文档编辑与事件

- [ ] `cp_edit_begin/end` 是否还需要 abort/失败离开接口；原生编辑失败
      后编辑上下文如何收尾。
- [ ] “非原子 remove+insert”与“后端原子 splice”等价的精确验证方法。
- [ ] `cp_edit_end` 中 AC/start-order 树、sensitive 清理、eph 清理的
      失败恢复顺序与可重试语义。
- [ ] 编辑期间 sensitive 判定所需的信息来源：是事件入队，还是编辑后
      统一比较内容。

### 12.5 位置索引算法

- [ ] AC 中同 seam 锚点的稳定排序规则。
- [ ] AC 插入/删除时锚点位于删除区间内的完整运动规则。
- [ ] start-order 树的具体平衡树类型、节点字段、子树 `best` 维护。
- [ ] start-order 树中两个 start 锚点的比较成本与实现方式。
- [ ] seam 组批量重排（gravity 分区、删除塌缩）的具体算法与复杂度。
- [ ] 若采用单棵 marktree：per-ns 遍历的过滤/剪枝元数据如何设计。

### 12.6 combine id

- [x] combine id 的物理表示：2 成分用 1 个 cons；3 成分用 2 个 cons；
      >=4 成分用 vector。
- [ ] combine 展开/重建时的排序、去重与 `mergewrite / mergeclear /
      mergecut` 的具体集合算法。
- [ ] direct id 与 combine id 的 refcount 精确维护顺序。
- [ ] 事件回调“精确预留、零分配”的预检算法与容量估算。

### 12.7 StoreOps / spantree 扩展

- [ ] `write(st, ns, 0, r)` 在 spantree 后端的具体映射（特殊 clear
      token、新增原语、还是逐段 merge）。
- [ ] `cp_touch / spA_touch` 写回契约：返回值、seam 上下文、写回后
      合并/再 touch 的规则。
- [ ] `spA_touch` 写回扩展与 object ns 编辑语义的关系（object 专题）。
- [ ] StoreOps 是否应该包含文档编辑原语，还是继续由绑定层直接调用
      后端原生编辑。

### 12.8 多来源 styled

- [ ] token 后端流与 object 区间索引合并的具体算法与数据结构。
- [ ] styled 组构建是每次全量收集，还是增量/缓存；OOM 时如何保持
      cursor 状态。

### 12.9 OOM 与原子性

- [ ] `cp_mark / cp_unmark / cp_clear / cp_nsclear` 的多步 mutator
      如何做到全有或全无。
- [ ] `cp_clear` head-in 删除多个 object 时中途失败的回滚/重试策略。
- [ ] `cp_edit_begin` 预检的精确容量估算与 `cp_edit_end` 部分失败
      后的状态定义。

### 12.10 其它

- [ ] §8 中普通 ns 与 eph ns 的 id 池划分是否继续保留。
- [ ] spanvec 后端是否仍需要支持 object ns，还是只承担 token/eph
      语义。
