# anchortree 设计（左边缘 span + 尾哨兵模型）

> 状态：实现设计（左边缘 L2–L4 迁移中）。
> 术语：`edge`，不用 `seam`。
> 本文件描述 `anchortree.h` 的坐标模型、结构与操作约定。

## 一、定位与边界

### 负责

- 维护一维坐标上的字节缝（byte seam）与 object range。
- 每个 object 一个 `at_Id`，含 start/end 两个端点。
- 文本 splice 后端点按 gravity 移动。
- 提供 object 级查询、删除、按坐标序的对象级迭代；迭代支持按 ns 过滤。
- `at_gravity` 返回调用方创建时给的 GROW/FIXED/SHIFT/LOCK。

### 不负责

- namespace 的 attr 解释、styled 合成。
- 与 compositor / spantree 的同步。
- Lua 绑定。
- 文档真实总长度：anchortree 不保存“EOF 之后”的字节，只保存已标记字节缝所
  需的跨度；没有端点项影响的纯插入可以不动树。

### 坐标语义

- 非空树必有 `edge@0`；空树没有 edge，`T->bytes == 0`。
- `T == T->bytes == 所有叶子 span 之和`，是当前已知的最大字节缝位置。
- edge 坐标 = 它前面所有 span 之和；`bytes` 是从本 edge 到下一个 edge（或到
  `T`）的 span 长度。因此每个 edge 表达一段左闭右开的 `[p, q)`。
- 中间 span（任何非末尾 edge 的 `bytes`）必须 `> 0`；唯一允许的 0 长叶子槽是
  位置 `T` 的尾哨兵，且它必须是最后一个叶子槽。
- 尾哨兵按需存在：位置 `T` 有端点项时才创建；其两个桶都空后立即删除。删除后
  若 `T > 0`，尾坐标由最后一条正长 span 的 `poff == bytes` 表达；`T == 0` 时
  树回到无 edge 空树。
- 首 edge 可以没有端点项：只要 `T > 0`，它作为 0 边界与前导 span 永久保留。
- seek/locate/advance 超过 `T` 的位置用 `poff` 表达虚拟偏移。

## 二、数据模型

### 2.1 edge 与长度定义

```c
typedef struct at_State  at_State;
typedef struct at_Tree   at_Tree;
typedef struct at_Node   at_Node;
typedef struct at_Edge   at_Edge;
typedef struct at_Cursor at_Cursor;
typedef struct at_Anchor at_Anchor;
typedef struct at_Point  at_Point;

typedef unsigned       at_Id;
typedef ptrdiff_t      at_Delta;
typedef unsigned short at_Index;
```

- 每个树内 edge 对应一个字节缝位置 `p >= 0`；`p == 0` 由首个 edge 表达。
- `edge.bytes` 是从本 edge 位置到下一个 edge 位置（最后一个 edge 则到 `T`）之
  间的字节数。
- 若树非空，首 edge 的 `bytes` 可以为任意非负长度；除尾哨兵外其余 edge 的
  `bytes > 0`。
- 不存在两个 edge 位于同一位置。

示例：

- 只有 `edge@0`、span 100：`bytes == 100`，`T == 100`，表示坐标 0 有缝且内容
  长度 100 内无其他缝。
- `edge@0`、`edge@10`、`edge@50`：三者 `bytes` 分别为 10、40、50，`T == 100`。
- 上面的树再加尾哨兵 `edge@100`：该槽 `bytes == 0`，是最后叶子槽；删除它后
  `T` 仍为 100，尾坐标由 `edge@50` 的 `poff == bytes` 表达。

### 2.2 树结构

```c
#define AT_FANOUT    32
#define AT_MAX_LEVEL 16 /* safe pair with AT_FANOUT=32; see docs/max_levels.md */
#define AT_PAGE_SIZE 65536

typedef struct at_Pool {
    size_t obj_size;
    void  *freed;
    void  *pages;
    size_t freed_obj;
} at_Pool;

typedef struct at_Node {
    at_Node   *children[AT_FANOUT];
    size_t     bytes[AT_FANOUT];
    at_Index   child_count;
    at_Node   *parent;    /* parent node; NULL for the root */
    at_Index   idx;       /* child index in the parent */
} at_Node;

struct at_State {
    void     *alloc_ud;
    at_Alloc *allocf;
    at_Pool   nodes;
    at_Pool   edges;
};

struct at_Tree {
    at_Node   root;
    at_State *S;
    size_t    bytes;   /* sum of all leaf spans; 0 for an empty tree */
    int       levels;
    at_Anchor *ids;    /* dense object pool, slot 0 reserved */
    size_t    idcap;
    at_Id     idfree;
};
```

- 叶子层把 `children[i]` 按 `at_Edge *` 解释；内层按 `at_Node *` 解释。
- 叶子槽 `bytes[i]` 是该 edge 到下一个 edge（或 `T`）的 span；除尾哨兵外必须
  `> 0`，尾哨兵恒为 0 且只允许出现在最后一个叶子槽。
- 内层 `bytes[i]` 是子树内所有叶子 `bytes` 的和；`T->bytes` 是所有叶子之和。
- 删除/插入通过 `atD_delsuccessor`/`atI_makeroom` 的递归平衡完成，不使用
  spantree.h 的 stitch 暂存根区；`AT_MAX_LEVEL` 只约束 `Cursor.paths` 的深度。
- `at_Pool` 在 `AT_POOL_STATS` 编译配置下额外含 `size_t live_obj`，用于测试
  泄漏统计；该模式与 spantree.h 的 `SP_POOL_STATS` 相同，细节参考 spantree.h
  的 pool 实现。
- `AT_MAX_LEVEL` 与 `AT_FANOUT` 必须成对选择：默认 `AT_FANOUT=32` /
  `AT_MAX_LEVEL=16` 安全；自定义更小 fanout 时按 `docs/max_levels.md` 的表加
  大（如 `AT_FANOUT=4` 需 `AT_MAX_LEVEL=64`）。树高增加处有
  `assert(levels < AT_MAX_LEVEL)` 兜底；release 构建不自动扩张 path 数组。

### 2.3 端点槽与 edge

```c
typedef struct at_Point {
    unsigned id     : 31;
    unsigned is_end : 1;
} at_Point;

typedef struct at_Edge {
    at_Node  *parent; /* leaf containing this edge; never NULL */
    at_Index  idx;    /* index in parent->children/bytes */
    at_Point  left;   /* left bucket head; empty is {0,0} */
    at_Point  right;  /* right bucket head; empty is {0,0} */
} at_Edge;
```

- `at_Point` 只是 4 字节侵入式链接槽：`id` 指向 anchor，`is_end` 区分
  START/END 角色。
- 每个 `at_Edge` 有 `left/right` 两个桶头；`id == 0` 表示空桶。
- 端点槽不单独分配：每个 live anchor 的两个槽内嵌在 anchor 中，见 §2.4。
- 同一 anchor 的 START/END 落到同一桶时，按 `is_end` 标签串在同一链上；不合
  并、不去重。
- 树内 edge 必须至少有一个端点槽，但首 edge 例外：`T > 0` 时首 edge 即使两桶
  全空也永久保留，作为 0 边界与前导 span。尾哨兵在桶空后删除。
- 任何新 edge 在创建后必须立即获得端点槽，或在同一操作的失败回滚中被删除。

### 2.4 anchor 池

```c
typedef struct at_Anchor {
    at_Edge *start;
    at_Edge *end;
    at_Point snext;
    at_Point enext;
    unsigned ns      : 29;
    unsigned gravity : 2;
} at_Anchor;
```

- `at_Anchor` 是桶链表的节点：`start/end` 指向端点所在 edge；`snext/enext` 分
  别是 START/END 槽的后继槽。
- `T->ids` 是稠密数组；`at_Id` 从 1 开始，0 保留为“空/未使用”。
- 空闲 id 用 `snext.id` 串成 freelist，`T->idfree == 0` 表示 freelist 空。
- `anchor->end == NULL` 表示 id 已释放；live anchor 的 `start/end` 均非 NULL。
- `ns` 与 `gravity` 打包在 anchor 内；`ns` 占 29 bit，`gravity` 占 2 bit。
- 容量上限 `AT_ANCHOR_MAX == (1u << 31)`；写入时 `ns & 0x1fffffffu`。
- `at_Id` 直接索引 `T->ids`，查询 O(1)；删除需要从桶头线性扫描同侧链（O(k)，
  k 为该桶槽数），可优化为 `at_Point` 双向链（sprev/eprev），暂不引入额外
  8B/anchor。
- 允许 start 位置大于 end 位置的交叉零长，`at_range` 原样返回。

### 2.5 Cursor

```c
#define AT_ITER_TREE 0u /* iteration active */
#define AT_ITER_DONE 1u /* iteration exhausted */

typedef struct at_Cursor {
    at_Tree   *tree;   /* tree being navigated */
    size_t     off;    /* start coordinate of the current edge */
    size_t     poff;   /* offset inside the current span */
    unsigned   side;     /* current bucket: 0 = left, 1 = right, 2 = done */
    unsigned   next_id;  /* next slot anchor id; 0 = current bucket done */
    unsigned   next_end; /* is_end tag of the next slot */
    unsigned   iter;     /* AT_ITER_TREE / AT_ITER_DONE */
    at_Node  **paths[AT_MAX_LEVEL]; /* root-to-leaf path */
} at_Cursor;
```

- `off + poff` 是 Cursor 的绝对坐标。
- 当前 edge 是 `*paths[levels]` 指向的叶子槽，按 `at_Edge *` 解释。
- 指向树内 edge@`p` 时 `off + poff == p` 且 `poff == 0`。
- 位于一个 span 内部时，当前 edge 是该 span 的起点 edge，`0 < poff < bytes`。
- 超过最后一个 edge 时仍停留在最后一个叶子槽：`poff >= bytes`，超出部分（当
  有尾哨兵时从 0 起）为虚拟距离。
- 空树由 `paths[0] == root.children`、`off == 0`、`poff == pos` 表达；没有 edge
  可指向，`at_next` 直接耗尽。
- `at_seek` 是构造器（`memset` + 设置 tree 后调用 `at_locate`）；`at_locate` 是
  公开的重定位原语，重置 `iter = AT_ITER_TREE` 并把桶游标播种到当前 edge 的
  left 桶头。
- `at_advance(C, d)`：`d == 0` 保留 `iter/side/next_id/next_end` 与坐标不变；
  `d != 0` 移动坐标并把桶游标重新播种到新 edge 的 left 桶头（`iter` 置
  `AT_ITER_TREE`）；空树无 edge 时跳过播种。
- 端点迭代不保存槽指针：保存 `side`、`next_id`、`next_end`。从槽 `p` 前进时用
  `p.is_end ? ids[p.id].enext : ids[p.id].snext`；先扫 left 桶，`next_id == 0`
  后切到 right；只产出 `is_end == 0` 的 START 槽，END 槽只用于推进链表。
- anchor 数组在 `at_newanchor` 触发扩容时会搬移，因此 Cursor 保存 id 与标签而
  不是 `at_Point *`。
- 修改树结构的 API（`at_newanchor`/`at_splice`/`at_delanchor`）可能搬移叶子槽、
  删除 edge 或改变桶链，旧 Cursor 的 `paths` 可能悬空。`at_splice` 在返回前重
  新 `at_seek` 维护传入的 Cursor；`at_newanchor`/`at_delanchor` 不维护 Cursor，
  调用后旧 Cursor 失效，调用方必须重新 `at_seek`。
- 后续可在 Cursor 中加 epoch：与 tree 不匹配的 epoch 视为过期 Cursor，相关 API
  返回 `AT_ERRPARAM`，而不是解引用悬空 `paths`。
- 内部定位统一以 Cursor 为中心：`atK_findleaf`/`atK_forwardoff`/
  `atK_backwardoff` 与 spantree 的同名函数同构，但采用左边缘约定：
  `atK_findleaf` 用 `>=` 前进，使 `poff == 0` 吸附到 edge 起点；
  `atK_forwardoff` 在 `d < in` 时停留、`d >= in` 时进入下一 edge；
  `atK_backwardoff` 在 `d <= poff` 时停留，否则向左回退。`at_locate` 只在
  `pos < T` 时调用 `atK_findleaf`，因此不会遇到 0 长尾哨兵；`at_advance` 在
  目标 `>= T` 时走 `atK_locend` + 虚拟偏移，避免 `forwardoff` 越过尾哨兵。
- `at_locate`/`at_advance` 都是增量的自底向上降/升定位，不调用 `at_seek` 构造
  函数。

## 三、不变量

1. 空树 `root.child_count == 0`、没有 edge、`T->bytes == 0`。
2. 非空树首个叶子槽是 `edge@0`；除尾哨兵外所有叶子槽 `bytes > 0`。
3. 尾哨兵只在位置 `T` 有端点项时存在：`bytes == 0`，且必须是最后一个叶子槽。
4. `T->bytes` 等于所有叶子 `bytes` 之和；edge 位置严格递增。
5. `T > 0` 时首 edge 永久保留，即使它的两个桶都空。
6. 除首 edge 与尾哨兵外，每个树内 edge 至少有一个端点槽；失去全部槽的 edge 按
   §四删除。尾哨兵在两个桶都空后删除。
7. 每个 live anchor 恰好有两个槽：START `{id, 0}` 与 END `{id, 1}`。
8. 槽位于它 role 对应的桶中：side 由 `gravity` 与 `is_end` 决定（见 §6.1
   表）；同一 anchor 的两个槽可以在同一桶内。
9. 每个桶内，从桶头出发按槽的 `is_end` 选择 anchor 的 `snext/enext`，链必须终
   止于 `{0,0}`。
10. 链中每个槽的 `id` 必须是 live anchor；该 anchor 的 `start/end` 必须等于当
    前 edge，且角色与 `is_end` 一致。
11. edge 的 `parent/idx` 与它所在叶子槽一致。
12. 不存在两个 edge 位于同一字节缝；0 长槽只允许是末位尾哨兵。
13. `T->levels < AT_MAX_LEVEL`；每次增加树高的操作之后立即断言。
    `AT_MAX_LEVEL` 是编译期契约：默认 16 只对 `AT_FANOUT=32` 安全；覆盖
    `AT_FANOUT` 时必须按 `docs/max_levels.md` 预定义足够的 `AT_MAX_LEVEL`，
    release 构建不会自动扩张 path 数组。
14. `T->idcap <= AT_ANCHOR_MAX`；freelist 上的 id 必须已释放
    （`end == NULL`）。

## 四、edge 删除

删除一个树内 edge@`p`（调用方保证它没有端点槽；首 edge/尾哨兵规则见下）：

- 首 edge：`T == 0` 且无槽时删除，树回到无 edge 空树；`T > 0` 时首 edge 永不
  删除（即使无槽），它承载 0 边界与前导 span。
- 其他 edge：把它的 `bytes` 加到前一个 edge 的 `bytes` 上，然后从树中删除该
  槽；`T` 不变，它之后的 edge 位置保持不变（前一个 span 吸收被删字节）。
- 尾哨兵 `bytes == 0`，并入前一个 edge 不改变任何 span；删除后若 `T > 0`，尾
  坐标由最后一条正长 span 的 `poff == bytes` 表达。
- 删除后同步被搬移槽的 `parent/idx`，并沿路径更新内层 `bytes`。

## 五、创建/取得 edge

在位置 `p` 创建/取得树内 edge：

- `p == 0`：非空树返回首 edge；空树创建叶槽 0、`bytes = 0`（尾哨兵），`T` 仍
  为 0。新 edge 必须随后获得端点槽或回滚删除。
- `0 < p < T`：定位到包含 `p` 的 span。若 `p` 已是 edge 位置则直接返回；否则
  在该 span 内拆分：
  - 原 edge 的 `bytes` 改为 `p - edge_pos`；
  - 新 edge 的 `bytes` 为原 `bytes - (p - edge_pos)`；
  - 新槽插在原 edge 之后，返回新 edge。
- `p == T`：已有尾哨兵则返回；否则追加一个 0 长尾哨兵，返回它。新 edge 必须
  随后获得端点槽或回滚删除。
- `p > T`：非空时把最后一个 span 的 `bytes` 增加 `p - T`，再追加 0 长的
  `edge@p`，`T = p`；空树 `p > 0` 时先创建首 `edge@0`、其 `bytes = p`，再追加
  `edge@p`、`bytes = 0`，`T = p`。返回 `edge@p`。
- edge 分配先于任何度量/结构修改；分配失败返回 `AT_ERRMEM` 且树不变。插入槽
  可能触发节点 split，split 失败返回 `AT_ERRMEM`：已完成的 split 可以保留，
  但坐标与 `T` 不变，操作开始时分配且仍无槽的 edge 被释放。

## 六、对象操作

### 6.1 `at_newanchor(C, len, ns, gravity, *out)`

- `C` 已初始化，`start = at_offset(C)`。
- `end = start + len`；允许 `end` 超过 `T`（虚拟区），此时会创建新的最后 edge。
- 从 freelist 分配 `at_Id`（可能扩容 `T->ids`，失败返回 `AT_ERRMEM` 且不变更
  树）；初始化 anchor 的 `start/end/ns/gravity`。
- 确保 start edge 与 end edge（`atO_edges`）：`start` 处不存在 edge 时按 §五
  创建/拆分；`end` 同理。
- `atO_edges` 在任何插入前把所需 nodes/edges 预留到池中；预留失败直接返回
  `AT_ERRMEM`，不产生任何新 edge，失败后树与操作前一致。
- `ns` 写入 `anchor->ns`；`gravity` 写入 `anchor->gravity`；anchortree 不解释
  ns 含义。
- 按 gravity 把两个槽链接到对应桶：

| gravity | START 槽桶 | END 槽桶 |
|---|---|---|
| AT_GROW  | start edge LEFT  | end edge RIGHT |
| AT_FIXED | start edge RIGHT | end edge LEFT  |
| AT_SHIFT | start edge RIGHT | end edge RIGHT |
| AT_LOCK  | start edge LEFT  | end edge LEFT  |

- 桶 side 由 `atO_side(gravity, is_end)` 给出；START 槽 `{id, 0}` 链接到 start
  edge 的对应桶，END 槽 `{id, 1}` 链接到 end edge 的对应桶。
- `len == 0` 时 start/end 是同一个 edge，仍然链接两个槽；两者可能落在同一个
  桶。
- 链接操作只改桶头与 anchor 的 `snext/enext`，不分配内存。
- `at_newanchor` 不维护传入 Cursor；调用后旧 Cursor 失效，调用方必须重新
  `at_seek`。

### 6.2 `at_delanchor(T, id)`

- 通过 `at_Id` 直索引得到 live `at_Anchor`（`end != NULL`）；否则返回
  `AT_ERRPARAM`。
- 由 `anchor->gravity` 与 `is_end` 计算两侧桶，从 start/end edge 的桶中摘下对
  应的 `at_Point`。
- 对涉及的树内 edge：首 edge 在 `T > 0` 时保留；其他 edge 若两个桶都空，按
  §四删除；尾哨兵桶空即删。
- 把 `at_Id` 归还 freelist：`snext.id = idfree`，`end = NULL`。
- `at_delanchor` 全程零分配；OOM 下也必须成功。
- 删除只改变 anchor 元数据与可能空掉的 edge，不改变文本坐标；但它可能删除
  edge、搬移叶子槽，且不接受 Cursor，因此调用后所有旧 Cursor 失效，必须重新
  `at_seek`（epoch 检测见 §2.5）。

### 6.3 `at_range(T, id, *start, *end)`

- edge 位置从 `parent/idx` 沿路径把之前所有叶子 span 相加得到（`atO_edgepos`）；
  首 edge 位置为 0。
- start/end 分别取 anchor 的 `start/end` edge 位置。
- 交叉零长时原样返回 `*start > *end`。

### 6.4 `at_gravity(T, id)`

- 返回 `anchor->gravity` 的 `AT_GROW..AT_LOCK`；无效 id 返回负值。
- 因 gravity 已打包在 anchor 内，查询为 O(1)，不需要反查桶。

## 七、文本编辑

anchortree 不提供独立的 `insert` 与 `append` 编辑原语。`at_splice` 是唯一编辑
原语，其插入阶段遵循 **append semantics**：文本在当前 Cursor 处插入后，插入
内容位于该坐标的左侧端点与右侧端点之间；按 gravity 应右移的端点移到新文本之
后。若调用方需要“在当前 Cursor 之前插入”的语义，应先把 Cursor 后退所需长度
（`at_advance(-len)`），再调用 `at_splice`。

### 7.1 插入阶段（append semantics）

插入发生在 Cursor 当前坐标 `p`，长度为 `ins > 0`：

- `p` 是某个 edge 的位置（`poff == 0`）：
  - LEFT 桶中的槽留在 `p`；
  - RIGHT 桶非空时：原 edge 的 `bytes` 改为 `ins`；新建 `edge@p+ins`，其
    `bytes` 为原 edge 的旧 `bytes`，插在原 edge 之后；把 RIGHT 桶整体迁移到新
    edge（同步 anchor 的 `start/end`）；
  - RIGHT 桶为空时：原 edge 的 `bytes` 增加 `ins`。
  - 迁移后原 edge 若没有任何槽且不是首 edge，按 §四删除（其 `bytes = ins` 并
    入前一个 edge）；首 edge 在 `T > 0` 时保留。
- `p` 位于某个 span 内部（`0 < poff < bytes`）：没有端点在 `p`，把该 span 的
  `bytes` 增加 `ins`，之后所有 edge 自动右移，`T` 增加 `ins`。
- `p == T` 且没有尾哨兵（最后一条正长 span，位置 `T` 无端点）：把最后一条
  span 的 `bytes` 增加 `ins`，`T` 增加 `ins`。
- `p == T` 且有尾哨兵（`bytes == 0`）：等价于“`p` 是 edge 位置”的分支：
  - RIGHT 桶非空：尾哨兵 `bytes` 改为 `ins`，新建 `edge@T+ins`（尾哨兵）并把
    RIGHT 桶迁移过去，`T` 增加 `ins`；
  - RIGHT 桶为空：尾哨兵 `bytes` 增加 `ins`，`T` 增加 `ins`；它变成最后一条
    正长 span，不再需要哨兵（新 `T` 无端点）。
- `p > T`（虚拟区）：把最后一条 span 的 `bytes` 增加 `pos - T + ins`，`T` 变为
  `pos + ins`；不创建新 edge，后续若需要承载端点由 §五扩展出前导 span/edge。
- 空树：创建首 `edge@0`，其 `bytes = pos + ins`，`T = pos + ins`；纯插入也会
  立即建立该前导 span。
- 插入前 `at_splice` 预留 nodes/edges；单条 `atI_insertedge` 的 edge 分配失败会
  释放该 edge，节点 split 失败可能留下已完成 split 但不改变坐标与 `T`。

### 7.2 删除

删除 `[L, R)`（`del = R - L`）：

- 确保 `edge@L`：`L` 位于某 span 内部时按 §五拆分；`edge@L` 是幸存 edge。
- 若 `R < T`，确保 `edge@R` 并按 §五拆分/取得；若 `R == T`，不需要创建
  `edge@R`（已有尾哨兵时它就是 `edge@R`，`bytes == 0`）。
- 记 `bR`：`R < T` 时为 `edge@R` 的 `bytes`；`R == T` 时为 0。
- 把位置在 `(L, R]` 的所有 edge 上的桶整体迁移到 `edge@L`，槽保留原来的
  LEFT/RIGHT 桶与 `is_end` 角色。
- 位置 `p > R` 的端点槽随度量更新自动左移 `del`。
- 删除位置在 `(L, R]` 的 edge 槽：每个按 §四把 `bytes` 并入前一个 edge；随后
  把 `edge@L.bytes` 设为 `bR`，`T->bytes -= del`。
- `R == T` 时 `bR == 0`，`edge@L` 成为新的尾哨兵；若它随后也没有任何槽且不是
  首 edge，按 §四删除，树自然收缩到 `L`。
- 同一 object 两端落到同一 edge 时保留 START/END 两个槽，表示零长或交叉；即
  使同桶也不合并。
- 删除后所有变空的树内 edge 按 §四删除；首 edge 在 `T > 0` 时保留。

### 7.3 `at_splice(C, del, ins)`

- 语义为“删除 `[C, C+del)` 后再在 C 处插入 `ins`”。
- 删除阶段按 §7.2 处理端点塌缩与 edge 删除。
- 插入阶段按 §7.1 处理端点迁移与 edge 扩展。
- 在任何 mutation 前按最坏情况预留 nodes/edges；预留失败返回 `AT_ERRMEM` 且
  树不变。
- 若 `del == 0 && ins == 0`，直接成功。
- 返回前把传入 Cursor 重新定位到 `L`（等价于 `at_seek(C, C->tree, L)`），因此
  调用后 Cursor 有效且位于 `L`。

## 八、对象级迭代

- 迭代按 edge 坐标序进行；对每个 edge 先扫 left 桶再扫 right 桶，只产出
  `is_end == 0` 的 START 槽；END 槽只用于推进链表。
- 正长 object 的 END 在其 end edge 被跳过；零长 object 的 START 产出一次，
  END 跳过。
- 同一 edge 内多个对象不排序；稳定序由上层按 `(start, id)` 自行稳定。
- `at_next` 从 Cursor 当前位置开始：
  - 空树直接耗尽；
  - `poff > bytes[current]`（虚拟尾）直接耗尽；
  - `poff > 0` 时先移到下一个 edge（跳过当前 span 的桶），再扫描；
  - `poff == 0` 时扫描当前 edge 的桶；
  - 当前 edge 扫完后移到下一个 edge 继续；没有下一个 edge 时耗尽。
- 耗尽写 `iter = AT_ITER_DONE`；之后重复调用幂等返回 `AT_NONE`。
- `at_next` 支持 ns 过滤：`ns == 0` 不过滤；`ns > 0` 只返回
  `anchor->ns == (unsigned)ns` 的 START；`ns < 0` 参数错误，返回 `AT_NONE`
  且不把迭代置为耗尽。
- `at_next` 的 `plen` 为可选输出：仅当非 NULL 时写 `*plen`。找到 id 后用
  `at_range` 计算该 object 的 range：`start <= end` 写 `end - start`；
  `start > end`（交叉区间）写 0；零长写 0。
- 正常耗尽或参数错误（包括 `C == NULL`、`C->tree == NULL`、`ns < 0`）返回
  `AT_NONE`，且 `plen != NULL` 时写 0。

## 九、API 草案

```c
#define AT_OK        (0)
#define AT_ERRPARAM (-1)
#define AT_ERRMEM   (-2)
#define AT_NONE     (~(at_Id)0)

typedef enum at_Gravity { AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK } at_Gravity;

typedef void *at_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
```

```c
AT_API at_State *at_open(at_Alloc *allocf, void *ud);
AT_API void      at_close(at_State *S);

AT_API at_Tree  *at_newtree(at_State *S);
AT_API void      at_deltree(at_Tree *T);
AT_API void      at_reset(at_Tree *T);
```

```c
AT_API int   at_seek(at_Cursor *C, at_Tree *T, size_t pos);
AT_API int   at_locate(at_Cursor *C, size_t pos);
AT_API int   at_advance(at_Cursor *C, at_Delta d);
AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen);
```

```c
AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out);
AT_API int at_delanchor(at_Tree *T, at_Id id);

AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end);
AT_API int at_gravity(at_Tree *T, at_Id id);
```

```c
AT_API int at_splice(at_Cursor *C, size_t del, size_t ins);
```

- `at_seek` 初始化 Cursor 并定位到坐标；不返回位置。
- `at_locate` 在保持 `C->tree` 不变的前提下把 Cursor 重定位到给定坐标；空树、
  虚拟尾与迭代相位规则同 `at_seek`。
- `at_advance` 在坐标线上前后移动；越过最后一个 edge 时进入虚拟 `poff`；
  `d == 0` 保留迭代状态，`d != 0` 重置到当前 edge 的桶头。
- `at_gravity` 返回 `AT_GROW..AT_LOCK`，错误时返回负值；打包存储，O(1)。
- `at_next` 返回下一个 object 的 START id；`ns == 0` 不过滤，`ns > 0` 按
  `anchor->ns` 过滤，`ns < 0` 为参数错误；`plen` 为可选长度输出，耗尽/参数
  错误写 0，交叉区间写 0；到达末尾或参数错误时返回 `AT_NONE`。
- `at_splice` 成功后传入 Cursor 重新定位在 `L`。

## 十、内存估算

- `at_Point`：4B（`id:31` + `is_end:1`）。
- `at_Anchor`：`start/end` 两个指针 + `snext/enext` 两个 4B 槽 +
  `ns:29`/`gravity:2`；以 `sizeof` 为准。
- `at_Edge`：`parent/idx` + 两个 4B 桶头；无 tail vector 指针。
- `at_Tree` 不再内嵌 `origin`，位置 0 由首个叶子槽表达。
- 每个 live anchor 恰好占两个槽，不按端点数分配额外内存；没有 point pool，也
  没有 endpoint tail vector。
- 树内 edge 数量与“有端点项的字节缝”数量同阶，外加最多一个前导首 edge 与一
  个尾哨兵。
- `at_Id` 稠密数组通过 `atO_growids` 倍增扩容，freelist 复用已删除 id；查询
  O(1)。

## 十一、实现注意

- 所有叶子槽搬移（copy/move/remove/makespace/split/merge）后，要同步被搬移
  edge 的 `parent/idx`。
- 度量更新沿路径累加；`T->bytes` 等于所有叶子 span 之和，删除/插入后由 `del`/
  `ins` 直接更新。
- 删除/增长 edge 的度量更新以 Cursor 为工作结构：`atD_deleteedge`/`atI_shift`
  先建立 `paths`，再用 `atK_parent`/`atK_idx` 沿路径修改 `bytes`；不使用
  `anc/idx` 手搓数组，也不在内部 helper 中强制定位 Cursor。
- 桶迁移使用 `atO_moveside`：逐槽按 `is_end` 读出后继，更新
  `anchor->start/end` 后推入目标桶；不合并、不去重、不预留额外内存。
- `at_splice` 在任何 mutation 前通过 `atP_reserve` 预留 nodes/edges；
  `at_newanchor` 所需的 edge 也由 `atO_edges` 预留在池中，预留失败不产生新 edge。
- `at_delanchor` 零分配：只做桶摘除、空 edge 删除与 freelist 归还。
- 可优化项：`atO_unlink`/`atO_moveside` 目前从桶头线性扫描同侧链；引入
  `sprev/eprev` 双向链后单槽删除可 O(1)，代价是每 anchor +8B，暂不实施。
- debug 构建由 `at_checkchains` 做全量链表不变量检查，由 `at_checktree` 做结
  构检查：每个桶的槽都属于 live anchor、side/角色正确、每个 live anchor 恰出
  现一次 START/END；叶子层首 edge 位置为 0、除末位尾哨兵外无 0 长槽、中间
  edge 必须有槽、首 edge 在 `T > 0` 时可空、`T == sum(bytes)`。
- `T->ids` 在 `at_newanchor` 扩容时可能搬移；Cursor 与桶链都存 id + `is_end`，
  不存 `at_Point *`。
