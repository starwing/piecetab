# anchortree 设计（左边缘 span + 尾哨兵模型）

> 状态：实现设计（左边缘 L2–L4 迁移中）。
> 术语：统一用 `edge`。
> 本文件描述 `anchortree.h` 的坐标模型、结构与操作约定。

## 一、定位与边界

### 负责

- 维护一维坐标上的字节边界（byte edge）与 object range。
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
- `T->bytes == 所有叶子 span 之和`，即当前已知的最大字节缝位置。
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
    size_t    bytes;   /* sum of all leaf spans; 0 for an empty tree */
    at_State *S;
    at_Anchor *anchors; /* dense object pool, slot 0 reserved */
    at_Id      idmax;   /* allocated anchor capacity */
    at_Id      idfree;
    unsigned   epoch;   /* structural generation; bumped on tree mutation */
    int        levels;
};
```

- 叶子层把 `children[i]` 按 `at_Edge *` 解释；内层按 `at_Node *` 解释。
- 叶子槽 `bytes[i]` 是该 edge 到下一个 edge（或 `T`）的 span；除尾哨兵外必须
  `> 0`，尾哨兵恒为 0 且只允许出现在最后一个叶子槽。
- 内层 `bytes[i]` 是子树内所有叶子 `bytes` 的和；`T->bytes` 是所有叶子之和。
- 删除/插入通过 `atD_rmleaf`/`atI_makeroom` 的递归平衡完成；
  `AT_MAX_LEVEL` 只约束 `Cursor.paths` 的深度。
- `at_Pool` 在 `AT_POOL_STATS` 编译配置下额外含 `size_t live_obj`，用于测试
  泄漏统计；该模式与 spantree.h 的 `SP_POOL_STATS` 相同，细节参考 spantree.h
  的 pool 实现。
- `AT_MAX_LEVEL` 与 `AT_FANOUT` 必须成对选择：默认 `AT_FANOUT=32` /
  `AT_MAX_LEVEL=16` 安全；自定义更小 fanout 时按 `docs/max_levels.md` 的表加
  大（如 `AT_FANOUT=4` 需 `AT_MAX_LEVEL=64`）。树高增加处有
  `assert(levels < AT_MAX_LEVEL)` 兜底；release 构建不自动扩张 path 数组。
- `at_Index` 是 `unsigned short`，`AT_FANOUT` 由
  `AT_STATIC_ASSERT(AT_FANOUT <= 0xffffu)` 在编译期约束；`at_Node`/`at_Edge`
  与页大小的关系由 `AT_STATIC_ASSERT(sizeof(...) < AT_PAGE_SIZE / 2)`
  编译期兜底，与 `atP_init` 的运行期断言一致。

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
- 任何新 edge 在创建后必须立即获得端点槽；因为创建前 reserve 已保证后续不失败，
  `at_newanchor` 在 `atI_ensure` 两次返回 start/end edge 后无失败地链接两槽。
  仅 `atD_rmbound`/`atD_remove` 为删除边界拆出的临时 edge 不带槽，并在同一次
  删除的 successor 循环中被删除。

### 2.4 anchor 池

```c
typedef struct at_Anchor {
    at_Edge *start;
    at_Edge *end;
    at_Point snext;
    at_Point enext;
    unsigned ns          : 29;
    unsigned start_right : 1;
    unsigned end_right   : 1;
} at_Anchor;
```

- `at_Anchor` 是桶链表的节点：`start/end` 指向端点所在 edge；`snext/enext` 分
  别是 START/END 槽的后继槽；`start_right`/`end_right` 记录两个槽各自落在
  start/end edge 的哪个桶。
- `T->anchors` 是稠密数组；`at_Id` 从 1 开始，0 保留为“空/未使用”。
- 空闲 id 用槽内前 4 字节串成 freelist，`T->idfree == 0` 表示 freelist 空。
- `anchor->end == NULL` 表示 id 已释放；live anchor 的 `start/end` 均非 NULL。
- `at_Id` 复用顺序不是公共契约：`at_newanchor` 返回的 id 保证有效；已释放且
  未被复用的 id 在 `at_range`/`at_gravity`/`at_delanchor` 上继续报错。调用方
  只能依赖“新 id 有效、其他已释放 id 失效”，不得断言 LIFO/FIFO 复用顺序。
- `ns` 占 29 bit，写入前由 `at_newanchor` 校验 `0 <= ns <= AT_MAX_NS`；两个
  side 位各占 1 bit。
- 容量上限 `AT_ANCHOR_MAX == (1u << 31) - 1`。
- `at_Id` 直接索引 `T->anchors`，查询 O(1)；删除需要从桶头线性扫描同侧链
  （O(k)，k 为该桶槽数），可优化为 `at_Point` 双向链（sprev/eprev），暂不引入
  额外 8B/anchor。
- 扩容由 `atA_alloc` 用 `allocf` 的 realloc 语义完成：传旧指针与旧字节数，
  只在成功后替换 `T->anchors`（失败时旧数组与树保持不变）；新容量
  `idmax + idmax / 2`（首次为 8），新槽前 4 字节从 `id + 1` 起串入 freelist。
  槽 0 保留不用，不参与 freelist。
- 允许 start 位置大于 end 位置的交叉零长，`at_range` 原样返回。

### 2.5 Cursor

```c
typedef struct at_Cursor {
    at_Tree   *tree;   /* tree being navigated */
    size_t     off;    /* start coordinate of the current edge */
    size_t     poff;   /* offset inside the current span */
    unsigned   side;     /* current bucket: 0 = left, 1 = right, 2 = done */
    unsigned   next_id;  /* next slot anchor id; 0 = current bucket done */
    unsigned   next_end; /* is_end tag of the next slot */
    unsigned   epoch;    /* tree generation captured by seek/locate */
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
  公开的重定位原语，刷新 epoch 并调用定位原语；`atK_findleaf`/`atK_locend`
  出口统一 `atK_rewind`，把桶游标播种到当前 edge 的 left 桶头。
- `at_advance(C, d)`：`d == 0` 保留 `side`/`next_id`/`next_end` 与坐标不变，使
  `at_next` 能从当前桶继续；`d != 0` 由 `atK_forwardoff`/`atK_backwardoff`
  移动并在各自出口 `atK_rewind`；空树时 `atK_rewind` 把 `side` 置 2 后直接
  返回。正向移动用 `off + d < off` 检测 `size_t` 回绕，回绕返回
  `AT_ERROVERFLOW` 且坐标不变，不做饱和；负数回退量用 `-(size_t)d`，不使用
  step 变量。
- `atK_nextedge` 的提交规则：只有存在后继 edge 时才把当前 span 的 `bytes`
  加到 `C->off` 并把 `poff` 清零；无后继时保持 `paths` 指向当前 edge，
  span 内部（`poff > 0`）的 Cursor 坐标不变，edge 边界（`poff == 0`）的
  Cursor 以 `poff = bytes` 落到 `T`。`atK_step` 的“跳过当前 span”分支因此
  不会把耗尽 Cursor 的坐标推过 `T`，后续 `at_splice` 仍作用于原位置。
- 耗尽不写任何 flag：扫完最后一个 edge 时 `atK_step` 留下的状态就是
  “Cursor 坐标 + `side == 2`”；后续 `at_next` 幂等返回 `AT_NONE`。
  `at_locate`/`at_advance` 重新播种桶游标后迭代自然恢复。
- 端点迭代不保存槽指针：保存 `side`、`next_id`、`next_end`。从槽 `p` 前进统一
  用函数宏 `atE_next(p, a)` 取 `p.is_end ? a->enext : a->snext`；先扫 left 桶，
  `next_id == 0` 后切到 right；只产出 `is_end == 0` 的 START 槽，END 槽只用于
  推进链表。
- anchor 数组在 `at_newanchor` 触发扩容时会搬移，因此 Cursor 保存 id 与标签而
  不是 `at_Point *`。
- 修改树结构的 API（`at_newanchor`/`at_splice`/`at_delanchor`）可能搬移叶子槽、
  删除 edge 或改变桶链，旧 Cursor 的 `paths` 可能悬空。`at_splice` 在操作中
  维护同一个 Cursor，返回时它位于 `L` 且 epoch 已刷新；`at_delanchor` 不维护
  Cursor，调用后旧 Cursor 失效，调用方必须重新 `at_seek`/`at_locate`。
  `at_newanchor` 直接使用传入的 Cursor 定位 start/end（不建立第二个 Cursor），
  成功后 `T->epoch += 1` 但不刷新传入 `C->epoch`；`C` 的 paths 已被本次操作改写，
  调用后该 Cursor 过期，调用方必须重新 `at_seek`/`at_locate`。Cursor 的生命周期
  必须被其 `at_Tree` 的生命周期覆盖：`at_deltree` 释放 tree 后旧 Cursor 立即成为
  悬空指针，epoch 机制不负责跨 tree 释放的检测。
- `epoch` 是 `unsigned` 的相等性失效计数：`at_newtree` 初始化为 1，Cursor 在
  `at_seek`/`at_locate` 时捕获当前 tree epoch；任何结构 mutation 递增 tree
  epoch，此后旧 Cursor 的 epoch 与 tree 不匹配，属于过期 Cursor。它只参与
  相等/不等比较，允许自然回绕；回绕后需连续 2^32 次 mutation 才可能出现旧
  Cursor 被误判为当前，接受该风险，不为了“避免回绕”升宽到 `size_t`。
  过期 Cursor 在解引用 `paths` 之前被挡住：`at_advance`/`at_newanchor`/
  `at_splice` 返回 `AT_ESTALE`，`at_next` 返回 `AT_NONE` 且写 `*plen = 0`；
  `at_locate` 不拒绝过期 Cursor，直接重定位并刷新 epoch，是唯一恢复入口。
  epoch 是库内部机制，不提供 getter，也不在公共 API 文档中暴露数值；
  `AT_ESTALE` 即调用方唯一需要知道的“重新 seek/locate”信号。
- 内部定位统一以 Cursor 为中心：`atK_findleaf`/`atK_forwardoff`/
  `atK_backwardoff` 与 spantree 的同名函数同构，但 anchortree 允许虚拟尾：
  `atK_findleaf` 用 `>=` 前进，使 `poff == 0` 吸附到 edge 起点；
  `atK_forwardoff` 在 `d < in` 时停留、`d >= in` 时进入下一 edge，目标达到或
  越过 `T` 时直接 `atK_locend` 并补虚拟 `poff`；`atK_backwardoff` 在
  `d <= poff` 时停留，否则向左回退。`at_locate` 只在 `pos < T` 时调用
  `atK_findleaf`，因此不会遇到 0 长尾哨兵。
- `at_locate`/`at_advance` 都是增量的自底向上降/升定位，不调用 `at_seek` 构造
  函数；`at_locate` 的 `>= T` 分支先 `atK_locend` 再补 `pos - T->bytes` 虚拟
  偏移，`at_advance` 与 `at_splice` 的正方向推进统一复用 `atK_forwardoff`，
  虚拟尾边界由它内部处理。
- 定位原语的 rewind 职责：`atK_findleaf` 落 leaf 后、`atK_locend` 收尾后、以及
  `atK_forwardoff`/`atK_backwardoff` 的早退分支都调用 `atK_rewind`；因此
  `at_locate`/`at_advance` 自身不再重复调用。`atK_rewind` 在 root 空时把
  `side` 置 2 后直接返回。
- `atK_locend` 返回 0/1（空树 0、非空 1），空树分支写
  `paths[0] = root.children` 并把 `off`/`poff` 清零；非空时沿最后子槽下降，
  以 `poff = 末尾 span`、`off = T->bytes - poff` 收尾，再 `atK_rewind`，与
  `spK_locend` 同构。
- 库内部 helper 一律以现场 Cursor (`paths`/`off`/`poff`) 为工作结构：禁止调用
  `at_locate`/`at_seek` 重新建立位置，禁止用 `atK_locend`/`atK_backwardoff`
  丢弃现场，也禁止内部 helper 调用 `at_advance`。删除流程只在 `C`（左边界）
  之外建一个本地 `R`：`R = *C` 后直接 `atK_forwardoff(&R, del)`（到/越过 `T`
  的 `atK_locend` + 虚拟偏移已收进 `atK_forwardoff`）；`atD_rmbound` 取到 `bR`
  后 `R` 复用为 successor 遍历（对应参考库 `spD_remove` 的 `R = *C`）。公共入口
  `at_newanchor` 允许在传入 Cursor 上用 `atK_forwardoff`/`atK_locend` 定位 end，
  这是“物化新 anchor 端点”的显式定位，不建立第二个 Cursor，也不是内部 helper
  强制定位。

## 三、不变量

1. 空树 `root.child_count == 0`、没有 edge、`T->bytes == 0`。
2. 非空树首个叶子槽是 `edge@0`；除尾哨兵外所有叶子槽 `bytes > 0`。
3. 尾哨兵只在位置 `T` 有端点项时存在：`bytes == 0`，且必须是最后一个叶子槽。
4. `T->bytes` 等于所有叶子 `bytes` 之和；edge 位置严格递增。
5. `T > 0` 时首 edge 永久保留，即使它的两个桶都空。
6. 除首 edge 与尾哨兵外，每个树内 edge 至少有一个端点槽；失去全部槽的 edge 按
   §四删除。尾哨兵在两个桶都空后删除。
7. 每个 live anchor 恰好有两个槽：START `{id, 0}` 与 END `{id, 1}`。
8. 槽位于它 role 对应的桶中：`start_right`/`end_right` 记录两个槽各自的桶
   （见 §6.1 表）；同一 anchor 的两个槽可以在同一桶内。
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
14. `T->idmax <= AT_ANCHOR_MAX`；freelist 上的 id 必须已释放
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
- edge 分配先于任何度量/结构修改；公共入口在 mutation 前用 `atI_reserve`
  预留 nodes/edges，失败返回 `AT_ERRMEM` 且树不变。reserve 成功后插入槽触发的
  node split 与 edge 分配只用 `atP_ralloc`/assert，不再返回 OOM，也不留下需要
  回滚的半成品。

## 六、对象操作

### 6.1 `at_newanchor(C, len, ns, gravity, *out)`

- `C` 已初始化，`start = at_offset(C)`；`start + len` 做 `atU_addoverflow`
  检查，溢出返回 `AT_ERROVERFLOW` 且树不变。
- `ns` 必须在 `0..AT_MAX_NS`（29 bit）内，否则返回 `AT_ERRPARAM`。
- `end = start + len`；允许 `end` 超过 `T`（虚拟区），此时会创建新的最后 edge。
- 从 freelist 分配 `at_Id`（可能扩容 `T->anchors`，失败返回 `AT_ERRMEM` 且树保持
  不变）；初始化 anchor 的 `start/end/ns` 与两个 side 位。
- 确保 start edge：`a->start = atI_ensure(C)` 直接取得当前位置 edge；`len == 0`
  时 `a->end = a->start`。否则用同一个传入 Cursor `C` 物化 end edge：
  - `len < atK_bytes(C) - start`（`atI_ensure` 已把树扩到 `start`，所以比较的
    是扩展后的树）：`atK_forwardoff(C, len)` 推进到 end，再
    `a->end = atI_ensure(C)`；
  - 否则（`end == T` 或虚拟尾）：`atK_locend(C)` 把 Cursor 落到当前末尾 span，
    `C->poff += start + len - atK_bytes(C)` 补足虚拟偏移，再
    `a->end = atI_ensure(C)`。
  `atI_ensure` 是单返回值 helper，按 Cursor 的 `poff` 分支：root 空→
  `atI_emptyedge`；`poff == 0`→`atK_edge`；`0 < poff < bytes`→`atI_splitat`；
  `poff >= bytes`→`atI_pad`。`atI_emptyedge`/`atI_splitat`/`atI_pad` 各自内部
  `atE_new` 并返回实际 edge，不再有 out 参数或 `atI_edges` 中间 helper。
- `at_newanchor` 不做任何本地 Cursor 副本：它用同一个传入 `C` 完成 start/end
  edge 物化与链接，成功返回时递增 `T->epoch` 但不刷新 `C->epoch`；结束时
  C 的内部 paths 已被本次操作改写，epoch 仍为旧值，调用方必须重新
  `at_seek`/`at_locate`。这里对新 anchor 端点的定位不调用 `at_advance`、不建立
  第二个 Cursor。
- `at_newanchor` 在做任何树修改前预留两次 `atI_ensure` 所需的 nodes/edges；预留
  失败直接返回 `AT_ERRMEM`，不产生任何新 edge，失败后树与操作前一致。
- `ns` 写入 `anchor->ns`；`start_right`/`end_right` 按 gravity 写入，anchortree
  不解释 ns 含义。
- 按 gravity 选择两个槽的桶：

| gravity | START 槽桶 | END 槽桶 |
|---|---|---|
| AT_GROW  | start edge LEFT  | end edge RIGHT |
| AT_FIXED | start edge RIGHT | end edge LEFT  |
| AT_SHIFT | start edge RIGHT | end edge RIGHT |
| AT_LOCK  | start edge LEFT  | end edge LEFT  |

- START 槽 `{id, 0}` 链接到 start edge 的 `start_right` 桶，END 槽 `{id, 1}`
  链接到 end edge 的 `end_right` 桶；桶 side 位由 `at_newanchor` 直接写入，
  `at_gravity` 由两位反推调用方创建时给的 gravity。
- `len == 0` 时 start/end 是同一个 edge，仍然链接两个槽；两者可能落在同一个
  桶。
- 链接操作只改桶头与 anchor 的 `snext/enext`，不分配内存。
- `at_newanchor` 成功返回前不刷新传入 Cursor 的 epoch；旧 Cursor 失效，调用方
  必须重新 `at_seek`/`at_locate`（epoch 失效契约见 §2.5）。

### 6.2 `at_delanchor(T, id)`

- 通过 `at_Id` 直索引得到 live `at_Anchor`（`end != NULL`）；否则返回
  `AT_ERRPARAM`。
- 由 `anchor->start_right`/`end_right` 选择两侧桶，从 start/end edge 的桶中摘下
  对应的 `at_Point`。
- 对涉及的树内 edge：首 edge 在 `T > 0` 时保留；其他 edge 若两个桶都空，按
  §四删除；尾哨兵桶空即删。
- 把 `at_Id` 归还 freelist：槽内前 4 字节写入 `idfree`，`end = NULL`。
- `at_delanchor` 全程零分配；OOM 下也必须成功。
- 删除只改变 anchor 元数据与可能空掉的 edge，不改变文本坐标；但它可能删除
  edge、搬移叶子槽，且不接受 Cursor，因此调用后所有旧 Cursor 失效，必须重新
  `at_seek`/`at_locate`（epoch 检测见 §2.5）。

### 6.3 `at_range(T, id, *start, *end)`

- edge 位置由 `atK_fromedge` 从 edge 的 `parent/idx` 现场建立 Cursor paths，
  再由 `atK_edgepos` 按 paths 把之前所有叶子 span 相加得到；首 edge 位置为 0。
  两者都不调用 `at_locate`。
- start/end 分别取 anchor 的 `start/end` edge 位置。
- 交叉零长时原样返回 `*start > *end`。

### 6.4 `at_gravity(T, id)`

- 由 `anchor->start_right`/`end_right` 反推创建时的
  `AT_GROW..AT_LOCK`；无效 id 返回负值。
- 因两个 side 位已打包在 anchor 内，查询为 O(1)，不需要反查桶。

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
- `at_splice` 在任何 mutation 前统一预留删除与插入所需的 nodes/edges；随后
  `atD_remove`/`atI_insert`/`atI_ensure` 只用 `atP_ralloc`/assert，不产生需要
  回滚的半成品；预留失败发生在任何 mutation 之前。

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
- 插入侧对 `max(pos, atK_bytes(C)) + ins` 做 `atU_addoverflow` 检查，溢出返回
  `AT_ERROVERFLOW` 且树不变；删除侧 `del` 仍可任意大，进入删除流程后先按
  `T` 截断。
- 过期 Cursor 在预留与 mutation 之前返回 `AT_ESTALE`；`del == 0 && ins == 0`
  也必须先通过过期检查，合法 Cursor 才直接成功。
- 预留成功后、第一次树 mutation 前递增 `T->epoch`；删除与插入都维护传入
  Cursor，插入完成后按 append 语义推进到 `pos + ins`，epoch 已刷新到新值，
  不再内部调用 `at_seek`。

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
  - 当前 edge 扫完后移到下一个 edge 继续；没有下一个 edge 时耗尽：
    后继存在才提交 `off`，否则 span 内部保持原坐标、edge 边界落到 `T`
    （见 §2.5 的 `atK_nextedge` 提交规则）。
- 耗尽不写 flag：扫完最后一个 edge 时 Cursor 停留在原坐标且 `side == 2`，
  之后重复调用幂等返回 `AT_NONE`；`at_locate`/`at_advance` 会重新播种桶游标，
  使迭代恢复。
- 过期 Cursor（与 tree epoch 不匹配）直接返回 `AT_NONE` 且写 `*plen = 0`，
  不解引用 `paths`、不修改 Cursor，也不返回 dead id；调用方需重新
  `at_seek`/`at_locate`。
- `at_next` 支持 ns 过滤：`ns == 0` 不过滤；`1 <= ns <= AT_MAX_NS` 只返回
  `anchor->ns == (unsigned)ns` 的 START；`ns < 0` 或 `ns > AT_MAX_NS` 为参数
  错误，返回 `AT_NONE` 且写 0。
- `at_next` 的 `plen` 为可选输出：仅当非 NULL 时写 `*plen`。找到 id 后用
  `at_range` 计算该 object 的 range：`start <= end` 写 `end - start`；
  `start > end`（交叉区间）写 0；零长写 0。
- 正常耗尽、过期或参数错误（包括 `C == NULL`、`C->tree == NULL`、`ns` 越界）
  返回 `AT_NONE`，且 `plen != NULL` 时写 0。

## 九、API 草案

```c
#define AT_OK          (0)
#define AT_ERRPARAM    (-1)
#define AT_ERRMEM      (-2)
#define AT_ESTALE      (-3) /* Cursor stale; seek again */
#define AT_ERROVERFLOW (-4) /* size_t coordinate/byte addition overflow */
#define AT_NONE   (~(at_Id)0)
#define AT_MAX_NS ((1u << 29) - 1) /* largest namespace filter value */

typedef enum at_Gravity { AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK } at_Gravity;

typedef void *at_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
```

```c
AT_API at_State *at_open(at_Alloc *allocf, void *ud);
AT_API void      at_close(at_State *S);

AT_API at_Tree  *at_newtree(at_State *S);
AT_API void      at_deltree(at_Tree *T);
AT_API size_t    at_bytes(const at_Tree *T);
AT_API void      at_reset(at_Tree *T);
```

- `at_bytes` 返回树的当前总 span（`T->bytes`）；`T == NULL` 时返回 0。

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
  虚拟尾与桶游标播种规则同 `at_seek`。它不拒绝过期 Cursor，成功后刷新 epoch，
  是过期 Cursor 的唯一恢复入口。
- `at_advance` 在坐标线上前后移动；越过最后一个 edge 时进入虚拟 `poff`；
  `d == 0` 保留桶扫描现场，`d != 0` 的移动原语出口统一 `atK_rewind`；
  正向移动用 `off + d < off` 检测回绕，回绕返回 `AT_ERROVERFLOW`；过期 Cursor
  返回 `AT_ESTALE`。
- `at_gravity` 返回 `AT_GROW..AT_LOCK`，错误时返回负值；打包存储，O(1)。
- `at_next` 返回下一个 object 的 START id；`ns == 0` 不过滤，
  `1 <= ns <= AT_MAX_NS` 按 `anchor->ns` 过滤，越界为参数错误，返回 `AT_NONE`
  且写 0；`plen` 为可选长度输出，耗尽/参数错误写 0，交叉区间写 0；到达末尾或
  参数错误时返回 `AT_NONE`；过期 Cursor 返回 `AT_NONE` 且写 0，不返回 dead id。
- `at_splice` 过期返回 `AT_ESTALE`；成功后传入 Cursor 位于 `pos + ins`，epoch
  刷新为新值。
- `at_splice(C, del, ins)` 在 `ins == 0` 且 `at_offset(C) >= atK_bytes(C)` 时是
  纯 no-op：不 reserve、不递增 epoch、不使其他 Cursor 失效。
- `at_newanchor` 与 `at_splice` 的插入若发生 `size_t` 加法溢出，返回
  `AT_ERROVERFLOW`；`del` 不设上限，进入删除流程后按 `T` 截断。
- `at_newanchor` 的 `ns` 越界返回 `AT_ERRPARAM`。
- 不提供 `at_epoch` getter，也不在公共 API 描述中暴露 epoch 数值。

## 十、内存估算

- `at_Point`：4B（`id:31` + `is_end:1`）。
- `at_Anchor`：`start/end` 两个指针 + `snext/enext` 两个 4B 槽 +
  `ns:29` + 两个 side 位；以 `sizeof` 为准。
- `at_Edge`：`parent/idx` + 两个 4B 桶头。
- `at_Tree` 的位置 0 由首个叶子槽表达。
- 每个 live anchor 恰好占两个槽，不按端点数分配额外内存。
- 树内 edge 数量与“有端点项的 edge 位置”数量同阶，外加最多一个前导首 edge 与
  一个尾哨兵。
- `at_Id` 稠密数组通过 `atA_alloc` 扩容（1.5x，首次 8），freelist 复用已删除
  id；查询 O(1)。

## 十一、实现注意

- 所有叶子槽搬移（copy/move/remove/makespace/split/merge）后，要同步被搬移
  edge 的 `parent/idx`。
- 度量更新沿路径累加；`T->bytes` 等于所有叶子 span 之和，删除/插入后由 `del`/
  `ins` 直接更新；公共查询 `at_bytes` 直接返回该缓存值，不遍历树。
- 删除/增长 edge 的度量更新以 Cursor 为工作结构：`atE_resize` 修改当前槽并用
  `atM_up` 沿 `paths` 更新祖先与 `T->bytes`；`atD`/`atI` 内部 helper 只接收
  Cursor/Edge，禁止调用 `at_locate`/`at_seek` 重新建立位置，禁止用
  `atK_locend`/`atK_backwardoff` 丢弃现场，也禁止内部 helper 调用 `at_advance`。
  `atD_remove` 的本地 `R` 同样不调用 `at_advance`：`R = *C` 后直接
  `atK_forwardoff(&R, del)`，`bR` 取到后 `R` 复用为 successor 遍历。公共入口
  `at_newanchor` 在传入 Cursor 上用 `atK_forwardoff`/`atK_locend` 物化 end edge，
  不属于内部 helper 强制定位，也不建立第二个 Cursor。
- 桶迁移使用 `atE_movelink`：逐槽按 `is_end` 读出后继，更新
  `anchor->start/end` 后推入目标桶；不合并、不去重、不预留额外内存。
- 只有公共入口 `at_splice`/`at_newanchor` 在任何 mutation 前通过
  `atI_reserve` 预留 nodes/edges；内部 helper 不再自行 reserve，只用
  `atP_ralloc`/assert。`atI_reserve` 的 nodes 上界按
  `passes * (4*(levels+2)+8)` 计算。依据：一次 `atI_makeroom` 的最坏节点数是
  `levels+2`（根分裂新增 2 个节点，之后每个下层 `atI_splitchild` 新增 1 个；
  公式给的单次配额 `4*(levels+2)+8` 已含余量）。`passes` 为本次操作可能发生的
  `atI_makeroom` 次数：`at_splice` 为 3（L 边界 split、R 边界 split、插入 split
  各至多一次），`at_newanchor` 为 2（两次 `atI_ensure` 各至多一次）。edges：
  `at_splice` 实际至多新增 3 条 edge（两个边界 split + 一个 insertion split），
  reserve 6 保留余量；`at_newanchor` 实际至多 3 条（首次 `atI_emptyedge` 至多 2
  条、第二次 ensure 至多 1 条），reserve 4 保留余量。`atP_reserve` 只扩容 pool，
  失败不产生任何新 edge/节点，也不改变树。
- `at_advance` 正向移动用 `off + d < off` 检测 `size_t` 回绕，回绕返回
  `AT_ERROVERFLOW` 且坐标不变；不再做饱和。
- `at_delanchor` 零分配：只做桶摘除、空 edge 删除与 freelist 归还。
- 公共入口 `at_delanchor`/`at_range`/`at_gravity` 统一调用 `atA_fromid`；它拦截
  `T == NULL`、`id == 0`、`id > T->idmax`，并做 live 查询（`end != NULL`），
  无效时返回 NULL，由入口转换成 `AT_ERRPARAM`。这是三个公共入口共享的入口校验
  实现，锚定 anchortree 的稠密 anchor 池；`atA_fromid` 的检查不构成内部 helper
  的兜底防御，而是公共入口共享实现的边界。
- `atN_fixlinks` 粒度：所有调用点传入的都是紧随其后的 `atN_copy`/`atN_move`
  的目标区间，并用搬移前的 `cc` 直接算出移动槽数（`cc - i`、`cc - i - k`、
  `cc - e`），不存在重扫整段。`atN_copy`/`atN_move` 是 memcpy/memmove 批量
  搬移，无法顺带逐槽修链；改为逐槽循环会把 leaf/内部节点的 `parent`/`idx`
  偏移判定复制进搬移函数，并使签名超过 80 列，因此保持“批量搬移 + 精确区间
  fixlinks”两段式。同节点搬移不改变 `parent`，fixlinks 重写它是幂等的。
- `atN_sumbytes` 与 `atN_setleaf`/`atN_set` 聚类在同一个
  `/* clang-format off */` 双行函数区，而不是按 spantree 的
  `spN_sumbytes`→`spN_copy` 顺序排在 `atN_copy` 之前：三者都是无依赖短
  helper，保护区按 brief_writing §3 只包一次；移到 `atN_copy` 之前会让该
  保护区只剩两个函数，或迫使单个函数再包一对 off/on，排版收益为负。定义
  与调用方均无顺序依赖，差异仅为段内聚类顺序，不构成跨库同步缺口。
- 内部前置条件用 `assert` 表达，不写防御分支：`atI_splitnode` 断言两侧
  子节点均不少于 `AT_FANOUT / 2`；`atI_splitat` 在 `atI_makeroom` 前断言
  `C->poff > 0`、之后断言 `C->poff < p->bytes[i]`；`atI_pad` 断言当前 edge
  是叶尾槽且 `C->poff == p->bytes[i]`；`atI_insertsplit` 断言
  `C->poff == 0` 且当前 edge 的 RIGHT 桶非空。
- `atI_makeroom` 先检查当前叶子：未满直接 no-op；已满才自底向上找可分裂祖先
  并分裂到叶子。`atI_splitat`/`atI_pad`/`atI_insertsplit` 因此都先无条件调用
  `atI_makeroom(C)`，再计算一次当前 `p/i`（`atI_insertsplit` 再取 `e`），不再
  在调用方重复 `if (cc >= AT_FANOUT) { makeroom; 重算; }`。
- `atK_step` 先按 `poff > bytes[current]` 处理虚拟尾直接耗尽，再按 `poff > 0`
  处理“先跳到下一 edge”的入口分支；随后用
  `for (; (id = atE_nextstart(C, atK_edge(C), ns)) == AT_NONE; atK_rewind(C))
  if (!atK_nextedge(C)) return atK_none(plen);`
  扫描与推进。找到 id 后用 `at_range` 计算 `plen`，耗尽不写 flag，Cursor 保持
  在坐标 + `side == 2` 的自然耗尽态。`atE_nextstart` 直接返回 `at_Id`，不再有
  `out` 参数；`id` 在声明时初始化由函数返回值负责。
- `atE_nextstart` 只沿当前 edge 的两个桶线性推进，续扫位置保存在
  `C->side`/`next_id`/`next_end` 中；单次 `at_next` 在过滤条件下最坏需要
  跨过其后所有无命中 edge，代价 O(剩余 edge 数 × 树高 + 桶槽数)，整轮
  迭代与扫描总量同阶，不会在同一位置反复从头扫描。
- `atN_purge` 的叶节点早退分支保留 `if (k == 0) ... return;` 单行返回；
  `atI_ensure`/`atI_splitat`/`atI_pad`/`atI_emptyedge` 已改为单返回值，
  root 空、`poff == 0`、span 内、tail/virtual 各分支直接 return edge，不再有
  out 参数。
- `epoch` 是 `unsigned` 的相等性失效计数：`at_splice` 在 reserve 成功后、
  删除/插入前递增并刷新传入 Cursor；`at_newanchor` 在 reserve 与 start/end
  edge 物化、链接完成后、成功返回时递增，但不刷新传入 `C->epoch`，使旧
  Cursor 过期；`at_delanchor` 在取得 live anchor 后、摘槽前递增；`at_reset`
  清空前递增。`at_seek`/`at_locate` 只捕获/刷新，不递增。reserve 失败或参数
  错误等未 mutation 的失败不递增，Cursor 保持有效。只做相等比较，允许自然
  回绕，不为了“避免回绕”升宽。
- 公共 API 在解引用 `paths` 前检查 `C->epoch == C->tree->epoch`；过期时
  `at_advance`/`at_newanchor`/`at_splice` 返回 `AT_ESTALE`，`at_next` 返回
  `AT_NONE` 且写 `*plen = 0`。不提供 `at_epoch` getter。
- 可优化项：`atE_unlink`/`atE_movelink` 目前从桶头线性扫描同侧链；引入
  `sprev/eprev` 双向链后单槽删除可 O(1)，代价是每 anchor +8B，暂不实施。
- debug checker 与共享模型位于 `tests/at_tests.h`（`att_checktree`/
  `att_checkchains`/`att_checkcursor`/`att_checkdepth`/`att_seq`/`att_Model`），
  单测与 fuzz 共享同一份；不搬入 `anchortree.h`。`att_checkchains` 做全量链表
  不变量检查，`att_checktree` 做结构检查：每个桶的槽都属于 live anchor、side/
  角色正确、每个 live anchor 恰出现一次 START/END；空树额外要求 `levels == 0`；
  叶子层首 edge 位置为 0、除末位尾哨兵外无 0 长槽、中间 edge 必须有槽、首 edge
  在 `T > 0` 时可空、`T == sum(bytes)`。
- 测试主体共享在 `tests/at_cases.h`，`tests/anchortree_test_fanout4.c` 与
  `tests/anchortree_test_fanout8.c` 只保留 `TEST(...)` 包装并注册同一组共享场景；
  仅 `insert_split_child` 标注 `/* fanout4-only */`、不在 fanout8 注册。
  `tests/anchortree_test_fanout5.c` 保留奇数 fanout 的 `odd_split_leaf`/
  `odd_random_ops`，并注册 `at_cases.h` 的共享 M1 回归 case
  `iter_exhaust_commit`；由 `just at7`/`just at9` 重编译到 fanout 7/9；它复用
  `at_tests.h` 的共享辅助（`att_lcg`/`att_checkall`/`att_randomsplice`）。默认
  配置 runner `tests/anchortree_test_fanout32.c` 同样 `#include "at_cases.h"`
  并注册共享 `iter_exhaust_commit`，以覆盖默认 fanout 下 `at_next` 耗尽后的
  坐标提交与幂等语义；其余共享场景依赖 fanout-4/5 布局，不在 fanout32 注册。四层覆盖声明：
  fanout-4/8 共享行为层、fanout-5 奇偶边界层、fanout-7/9 奇数重编译层、
  fanout-32 默认配置层。单测与 fuzz 不调用任何库内部函数：行为断言走公共 API；
  精确前置状态由 setup-only helper 直接读写树/池结构完成；共享 checker
  （`att_checktree`/`att_checkchains`/`att_checkcursor`）读内部不变量，不属于
  公共 API 黑盒；结构深度断言统一经 read-only 的 `att_checkdepth` 结构 checker，
  runner 不直接读 `T->levels`；checker 自测/破坏性测试可故意改内部态并隔离标注。
- 共享 checker 的序列容量 `AT_SEQ_CAP` 默认 4096（可编译期覆盖），深
  fanout-32 树超过 512 条 edge 时不再 abort。
- `T->anchors` 在 `at_newanchor` 扩容时可能搬移；Cursor 与桶链都存 id +
  `is_end`，不存 `at_Point *`。

- 删除流程采用 spantree 的双 Cursor 形态，但 anchortree 的 edge 只有 span 长度
  与两个桶、没有可合并的内容标识，因此不用 stitch/scratch root：`atD_remove`
  以传入 `C` 为左边界，再用本地 `R = *C` 直接 `atK_forwardoff(&R, del)`；
  `atD_rmbound` 单返回 `bR`（不再有 out 参数）。随后 `R` 复用为 successor
  Cursor 依次 `atE_movelink` + `atD_rmleaf`，最后 `atK_fromedge(C, T, le)`
  重建左边界，一次性把 `edge@L.bytes` 收成 `bR` 并 `atE_resize` 更新
  `T->bytes`。
- `atD_balancenode` 保留 `int d + at_Delta *ds` 的多输出形态，与
  `spD_balancenode` 同步；跨库统一（调用方计算 `d`、helper 单返回 `ds`）列入
  TODO.md，不在此单独改签名。
- 单槽删除加 `atD_foldnode`/`atD_rebalance` 足以维持不变量；
  根占用不变量（`levels > 0` 时根孩子数 `>= 2`）由 `atD_rebalance` 末尾的
  collapse 循环在单孩子根出现时立即折叠保证，`atD_rebalance` 因此总在根至少
  有两个孩子时进入。该不变量由共享 `att_checktree` 固定为测试断言。
- 插入流程统一计算 grow delta：span 内部或末尾、RIGHT 桶为空、虚拟尾都走
  `atE_resize`，空树分支直接内联在 `atI_insert`（旧 `atI_insertempty` 已删除）；
  只有 edge 位置且 RIGHT 桶非空时才进 `atI_insertsplit`，它先 `atI_makeroom` 再
  重新计算 `p/i/e`，然后 `atE_resize` + `atN_makespace` 在 `p+ins` 建新 edge 并
  `atE_movelink`。`atI_splitnode` 单返回新右节点，左节点数由调用方用
  `atN_cc(nd)` 取得。删空的内部 edge 统一由 `atD_rmempty` 收尾，并保持 Cursor 在
  原来的绝对坐标（`atD_rmempty` 删除后把 `poff` 调回删除前的坐标）。
- `atK_locend` 带空树保护并返回 0/1，与 `spK_locend` 同构：空树写
  `paths[0] = root.children`、`off = poff = 0` 后返回 0，不假设存在叶子槽；
  非空树以 `off = T->bytes - poff` 返回 1。

### 块顺序与 sp 映射

spantree 的物理顺序是 `/* remove */` → `/* insertion / split */` → `/* fill */`。
anchortree 的 D 与 I 存在双向依赖，无法在不引入未使用前向声明的前提下照搬：

- `atD_rmbound`/`atD_remove` 调 `atI_splitat`，以保证删除边界 `edge@L` 或
  `edge@R` 存在；
- `atI_insertsplit`/`atI_insert` 调 `atD_rmempty`，以清理插入后变空的内部 edge。

因此当前顺序是无需前向声明的拓扑序：`/* insertion / split */`（只含
`atI_splitat`/`atI_pad` 及其依赖）→ `/* remove */` → `/* insertion */`
（`atI_emptyedge`/`atI_ensure`/`atI_insertsplit`/`atI_insert`；空树初始化内联在
`atI_insert`）。后两段合并成完整的 insertion 语义；两段之间的 remove 依赖是
领域原因，块内 helper 职责与 spantree 对齐。

| spantree 块 | anchortree 块 / 顺序 | 说明 |
| --- | --- | --- |
| `/* remove */` | `/* remove */`，在 `/* insertion / split */` 之后 | `atD_remove`/`atD_rmleaf`/`atD_rmempty`/`atD_rmbound`/`atD_foldnode`/`atD_rebalance`/`atD_balancenode` |
| `/* insertion / split */` | 第一段 `/* insertion / split */` + 后段 `/* insertion */` | 第一段：`atI_splitnode`/`splitroot`/`splitchild`/`makeroom`/`splitat`/`pad`；后段：`atI_emptyedge`/`ensure`/`insertsplit`/`insert` |
| `/* fill */` | 无独立块 | anchortree 无 ns mask 与 `rt[]`；object 级遍历由 `atK_step`/`at_next` 承担，对应 `sp_next` 角色 |

`atA_*`/`atE_*`/`atK_*`/`atD_*`/`atI_*` 与 sp 的对应关系：

| anchortree | sp 对应 | 差异边界 |
| --- | --- | --- |
| `atP_*`/`atS_*`/`atN_*`/`atM_up` | `spP_*`/`spS_*`/`spN_*`/`spM_up` | 基本同构 |
| `atA_*`（`slot`/`alloc`/`free`/`fromid`/`clear`） | spantree 无 anchor 池 | 稠密 id 池与 freelist 属 anchortree 领域 |
| `atE_*`（`empty`/`new`/`link`/`unlink`/`movelink`/`next`/`nextstart`/`resize`） | `spA_*`（`born`/`died`/`merge`/`touch`）、`spL_*` 读路径 | `atE_resize` 对应 `spA_touch`；`atE_nextstart` 对应 `sp_next` 的扫描部分 |
| `atK_*`（`locend`/`findleaf`/`forwardoff`/`backwardoff`/`nextedge`/`rewind`/`edge`/`edgepos`/`fromedge`/`prevedge`/`none`/`step`） | `spK_*` + `sp_next` | `atK_forwardoff` 额外处理到达/越过 `T` 时的 `atK_locend` + 虚拟 `poff`；`atK_fromedge`/`atK_edgepos`/`atK_prevedge`/`atK_nextedge` 是 edge/span 现场路径 helper，spantree 以 mask 与叶顺序代替 |
| `atD_*` | `spD_*` | 无 stitch/scratch root/`rt[]`：用 `atD_rmbound` + 本地 `R`（先取右边界，后复用为 successor）+ `atD_rmempty` 完成边界保证与空 edge 清理 |
| `atI_*` | `spI_*` + `sp_append`/`sp_insert` | `atI_emptyedge`/`ensure` 为 start/end edge 物化；`insert` 内联空树初始化；`ensure`/`emptyedge`/`splitat`/`pad`/`splitnode` 单返回值，无 out 参数，无 `rt[]`/fill 阶段 |
