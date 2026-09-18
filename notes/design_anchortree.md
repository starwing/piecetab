# anchortree 设计（左边缘 span + EOF 零长 edge 模型）

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
  位置 `T` 的 EOF 零长 edge，且它必须是最后一个叶子槽。
- EOF 零长 edge 不是 sentinel：它不标记结束位置，也不防越界写；它只在位置 `T`
  有端点项时创建，唯一职责是承载 EOF 字节缝上的 point list。它的两个桶都空后
  立即删除；删除后若 `T > 0`，尾坐标由最后一条正长 span 的 `poff == bytes`
  表达，`T == 0` 时树回到无 edge 空树。
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
- 若树非空，首 edge 的 `bytes` 可以为任意非负长度；除 EOF 零长 edge 外其余 edge 的
  `bytes > 0`。
- 不存在两个 edge 位于同一位置。

示例：

- 只有 `edge@0`、span 100：`bytes == 100`，`T == 100`，表示坐标 0 有缝且内容
  长度 100 内无其他缝。
- `edge@0`、`edge@10`、`edge@50`：三者 `bytes` 分别为 10、40、50，`T == 100`。
- 上面的树再加 EOF 零长 edge `edge@100`：该槽 `bytes == 0`，是最后叶子槽；删除它后
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
    at_Node   rt[AT_MAX_LEVEL]; /* scratch roots for cutrange/stitch */
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
    at_Edge   *seam;    /* purge link-move target; NULL means free */
};
```

- `at_State.rt[]` 是删除/stitch 的暂存根区，仅 `atD_rmrange` 使用；
  `at_reset`/`at_deltree` 不清它，它不作为持久状态。
- `at_Tree.seam` 是本次删除/切割的幸存目标 edge；非 `NULL` 时 `atN_purge`
  把被回收叶子 edge 的两个桶逐槽搬到它上面，`NULL` 时直接回收。
- 叶子层把 `children[i]` 按 `at_Edge *` 解释；内层按 `at_Node *` 解释。
- 叶子槽 `bytes[i]` 是该 edge 到下一个 edge（或 `T`）的 span；除 EOF 零长 edge 外必须
  `> 0`，EOF 零长 edge 恒为 0 且只允许出现在最后一个叶子槽。
- 内层 `bytes[i]` 是子树内所有叶子 `bytes` 的和；`T->bytes` 是所有叶子之和。
- 删除/插入通过 `atD_*` 的 stitch/递归平衡与 `atI_makeroom` 完成；
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
  全空也永久保留，作为 0 边界与前导 span。EOF 零长 edge 在桶空后删除。
- 任何新 edge 在创建后必须立即获得端点槽；因为创建前 reserve 已保证后续不失败，
  `at_newanchor` 在 `atI_ensure` 两次返回 start/end edge 后无失败地链接两槽。
- 删除/切割不创建带槽临时 edge：幸存目标由 `T->seam` 指定，被回收叶子 edge
  的桶由 `atN_purge` 逐槽搬进 `seam`，随后 edge 槽位由 `atD_cutrange`/stitch
  重新接回左子树；插入和 `at_delanchor` 产生的空 edge 由 `atE_purge` 回收。

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
  有 EOF 零长 edge 时从 0 起）为虚拟距离。
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
  Cursor 以 `poff = bytes` 落到 `T`。`at_next` 的“跳过当前 span”分支因此
  不会把耗尽 Cursor 的坐标推过 `T`，后续 `at_splice` 仍作用于原位置。
- 耗尽不写任何 flag：扫完最后一个 edge 时 `at_next` 留下的状态就是
  “Cursor 坐标 + `side == 2`”；后续 `at_next` 幂等返回 `AT_NONE`。
  `at_locate`/`at_advance` 重新播种桶游标后迭代自然恢复。
- 端点迭代不保存槽指针：保存 `side`、`next_id`、`next_end`。从槽 `p` 前进统一
  用函数宏 `atE_next(p, a)` 取 `p.is_end ? a->enext : a->snext`；先扫 left 桶，
  `next_id == 0` 后切到 right；只产出 `is_end == 0` 的 START 槽，END 槽只用于
  推进链表。
- anchor 数组在 `at_newanchor` 触发扩容时会搬移，因此 Cursor 保存 id 与标签而
  不是 `at_Point *`。
- 修改树结构的 API（`at_newanchor`/`at_remove`/`at_insert`/`at_append`/
  `at_splice`/`at_delanchor`）可能搬移叶子槽、删除 edge 或改变桶链，旧 Cursor
  的 `paths` 可能悬空。`at_remove`/`at_insert`/`at_append`/`at_splice` 在操作中
  维护同一个 Cursor 并刷新 epoch；`at_delanchor` 不维护 Cursor，调用后旧
  Cursor 失效，调用方必须重新 `at_seek`/`at_locate`。
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
  `at_remove`/`at_insert`/`at_append`/`at_splice` 返回 `AT_ESTALE`，`at_next`
  返回 `AT_NONE` 且写 `*plen = 0`；`at_locate` 不拒绝过期 Cursor，直接重定位
  并刷新 epoch，是唯一恢复入口。
  epoch 是库内部机制，不提供 getter，也不在公共 API 文档中暴露数值；
  `AT_ESTALE` 即调用方唯一需要知道的“重新 seek/locate”信号。
- 内部定位统一以 Cursor 为中心：`atK_findleaf`/`atK_forwardoff`/
  `atK_backwardoff` 与 spantree 的同名函数同构，但 anchortree 允许虚拟尾：
  `atK_findleaf` 用 `>=` 前进，使 `poff == 0` 吸附到 edge 起点；
  `atK_forwardoff` 在 `d < in` 时停留、`d >= in` 时进入下一 edge，目标达到或
  越过 `T` 时直接 `atK_locend` 并补虚拟 `poff`；`atK_backwardoff` 在
  `d <= poff` 时停留，否则向左回退。`at_locate` 只在 `pos < T` 时调用
  `atK_findleaf`，因此不会遇到 EOF 零长 edge。
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
  之外建一个本地 `R`：`R = *C` 后直接 `atK_forwardoff(&R, del)`；到/越过 `T`
  时用 `atK_locend` 归一到尾 edge。`atD_diverlevel` 区分同 edge
  (`atD_rmleaf`) 与跨 edge (`atD_rmrange`)；跨 edge 时 `T->seam = atK_edge(&R)`，
  `atD_remove` 在 rmrange 返回后清回 `NULL`。删除路径的 Cursor 由 `atD_stitch`
  末尾的结构断言与 `atD_rmleaf` 各分支在 mutation 过程中维护，返回时已在删除
  起点；公共入口只调 `atK_rewind` 复位迭代状态，不调用 `at_locate`/`at_seek`
  事后归位。公共入口 `at_newanchor` 允许在传入 Cursor 上用
  `atK_forwardoff`/`atK_locend` 定位 end，这是“物化新 anchor 端点”的显式定位，
  不建立第二个 Cursor，也不是内部 helper 强制定位。

## 三、不变量

1. 空树 `root.child_count == 0`、没有 edge、`T->bytes == 0`。
2. 非空树首个叶子槽是 `edge@0`；除 EOF 零长 edge 外所有叶子槽 `bytes > 0`。
3. EOF 零长 edge 只在位置 `T` 有端点项时存在：`bytes == 0`，且必须是最后一个叶子槽。
4. `T->bytes` 等于所有叶子 `bytes` 之和；edge 位置严格递增。
5. `T > 0` 时首 edge 永久保留，即使它的两个桶都空。
6. 除首 edge 与 EOF 零长 edge 外，每个树内 edge 至少有一个端点槽；失去全部槽的 edge 按
   §四删除。EOF 零长 edge 在两个桶都空后删除。
7. 每个 live anchor 恰好有两个槽：START `{id, 0}` 与 END `{id, 1}`。
8. 槽位于它 role 对应的桶中：`start_right`/`end_right` 记录两个槽各自的桶
   （见 §6.1 表）；同一 anchor 的两个槽可以在同一桶内。
9. 每个桶内，从桶头出发按槽的 `is_end` 选择 anchor 的 `snext/enext`，链必须终
   止于 `{0,0}`。
10. 链中每个槽的 `id` 必须是 live anchor；该 anchor 的 `start/end` 必须等于当
    前 edge，且角色与 `is_end` 一致。
11. edge 的 `parent/idx` 与它所在叶子槽一致。
12. 不存在两个 edge 位于同一字节缝；0 长槽只允许是末位 EOF 零长 edge。
13. `T->levels < AT_MAX_LEVEL`；每次增加树高的操作之后立即断言。
    `AT_MAX_LEVEL` 是编译期契约：默认 16 只对 `AT_FANOUT=32` 安全；覆盖
    `AT_FANOUT` 时必须按 `docs/max_levels.md` 预定义足够的 `AT_MAX_LEVEL`，
    release 构建不会自动扩张 path 数组。
14. `T->idmax <= AT_ANCHOR_MAX`；freelist 上的 id 必须已释放
    （`end == NULL`）。

## 四、edge 删除

删除一个树内 edge@`p`（调用方保证它没有端点槽；首 edge/EOF 零长 edge 规则见下）：

- 首 edge：`T == 0` 且无槽时删除，树回到无 edge 空树；`T > 0` 时首 edge 永不
  删除（即使无槽），它承载 0 边界与前导 span。
- 其他 edge：把它的 `bytes` 加到前一个 edge 的 `bytes` 上，然后从树中删除该
  槽；`T` 不变，它之后的 edge 位置保持不变（前一个 span 吸收被删字节）。
- EOF 零长 edge `bytes == 0`，并入前一个 edge 不改变任何 span；删除后若 `T > 0`，尾
  坐标由最后一条正长 span 的 `poff == bytes` 表达。
- 删除后同步被搬移槽的 `parent/idx`，并沿路径更新内层 `bytes`。

## 五、创建/取得 edge

在位置 `p` 创建/取得树内 edge：

- `p == 0`：非空树返回首 edge；空树创建叶槽 0、`bytes = 0`（EOF 零长 edge），`T` 仍
  为 0。新 edge 必须随后获得端点槽或回滚删除。
- `0 < p < T`：定位到包含 `p` 的 span。若 `p` 已是 edge 位置则直接返回；否则
  在该 span 内拆分：
  - 原 edge 的 `bytes` 改为 `p - edge_pos`；
  - 新 edge 的 `bytes` 为原 `bytes - (p - edge_pos)`；
  - 新槽插在原 edge 之后，返回新 edge。
- `p == T`：已有 EOF 零长 edge 则返回；否则追加一个 EOF 零长 edge，返回它。新 edge 必须
  随后获得端点槽或回滚删除。
- `p > T`：非空时把最后一个 span 的 `bytes` 增加 `p - T`，再追加 0 长的
  `edge@p`，`T = p`；空树 `p > 0` 时先创建首 `edge@0`、其 `bytes = p`，再追加
  `edge@p`、`bytes = 0`，`T = p`。返回 `edge@p`。
- edge 分配先于任何度量/结构修改；公共入口在 mutation 前统一走
  `atU_reserve(C, edges, pass, extra)`，失败返回 `AT_ERRMEM` 且树不变。
  helper 位于 `/* object/edge helpers */`，只做一次 nodes 预留与一次 edges
  预留；reserve 成功后插入槽触发的 node split 与 edge 分配只用
  `atP_ralloc`/assert，不再返回 OOM，也不留下需要回滚的半成品。

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
  §四删除；EOF 零长 edge 桶空即删。
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

anchortree 提供四个编辑原语：`at_remove(C, len)`、`at_insert(C, ins)`、
`at_append(C, ins)` 与 `at_splice(C, del, ins)`。`at_remove` 删除 `C` 起 `len`
字节并停在删除起点；`at_insert` 在 `C` 处插入并停在原 offset；`at_append` 在
`C` 处插入并推进到 `offset + ins`；`at_splice` 先 remove 后 append，Cursor
停在 `offset + ins`。插入阶段遵循 **append semantics**：文本在当前 Cursor 处
插入后，按 gravity 应右移的端点移到新文本之后。`len == 0`/`ins == 0` 是
no-op，不递增 epoch；过期 Cursor 返回 `AT_ESTALE`；插入侧对
`max(pos, T->bytes) + ins` 检查 overflow；删除侧 `len` 可任意大，进入删除流程
后按树尾截断。

### 7.1 插入阶段（append semantics）

插入发生在 Cursor 当前坐标 `p`，长度为 `ins > 0`：

- `p` 是某个 edge 的位置（`poff == 0`）：
  - LEFT 桶中的槽留在 `p`；
  - RIGHT 桶非空时：原 edge 的 `bytes` 改为 `ins`；新建 `edge@p+ins`，其
    `bytes` 为原 edge 的旧 `bytes`，插在原 edge 之后；把 RIGHT 桶整体迁移到新
    edge（同步 anchor 的 `start/end`）；
  - RIGHT 桶为空时：原 edge 的 `bytes` 增加 `ins`。
  - 迁移后原 edge 若没有任何槽且不是首 edge，由 `atE_purge` 回收（其 `bytes`
    并入前一个 edge）；首 edge 在 `T > 0` 时保留。
- `p` 位于某个 span 内部（`0 < poff < bytes`）：没有端点在 `p`，把该 span 的
  `bytes` 增加 `ins`，之后所有 edge 自动右移，`T` 增加 `ins`。
- `p == T` 且没有 EOF 零长 edge（最后一条正长 span，位置 `T` 无端点）：把最后一条
  span 的 `bytes` 增加 `ins`，`T` 增加 `ins`。
- `p == T` 且有 EOF 零长 edge（`bytes == 0`）：等价于“`p` 是 edge 位置”的分支：
  - RIGHT 桶非空：EOF 零长 edge `bytes` 改为 `ins`，新建 `edge@T+ins`（EOF 零长 edge）并把
    RIGHT 桶迁移过去，`T` 增加 `ins`；
  - RIGHT 桶为空：EOF 零长 edge `bytes` 增加 `ins`，`T` 增加 `ins`；它变成最后一条
    正长 span，不再需要 EOF 零长 edge（新 `T` 无端点）。
- `p > T`（虚拟区）：把最后一条 span 的 `bytes` 增加 `pos - T + ins`，`T` 变为
  `pos + ins`；不创建新 edge，后续若需要承载端点由 §五扩展出前导 span/edge。
- 空树：创建首 `edge@0`，其 `bytes = pos + ins`，`T = pos + ins`；纯插入也会
  立即建立该前导 span。
- mutation 前的资源预留统一走 `atU_reserve(C, edges, pass, extra)`：
  `at_remove` 传 `(0, 1, 0)`；`at_insert`/`at_append` 经 `atI_edit` 传
  `(1, 1, 2)`；`at_splice` 传 `(1, rm + add, 2*add)`，其中
  `rm = del > 0 && pos < T->bytes`、`add = ins > 0`；`at_newanchor` 传
  `(3, 2, 5)`。`pass`/`extra` 由调用者精确计算，helper 不解释事务来源。
  预留成功后 `atD_*`/`atI_*` 只用 `atP_ralloc`/assert，不产生需要回滚的
  半成品；预留失败返回 `AT_ERRMEM` 且树、Cursor、epoch 均不变。

### 7.2 删除（stitch + seam）

删除 `[L, R)`（`del = R - L`），`atD_remove` 使用本地 `R = *C`：

- `pos = at_offset(C)`；`del` 按 `T->bytes - pos` 截断；`R` 经
  `atK_forwardoff(&R, del)` 到右边界，到/越过 `T` 时 `atK_locend` 归一到尾 edge。
- `fl = atD_diverlevel(C, &R)`。`fl > levels` 表示 `R` 仍在当前 edge：
  `atD_rmleaf(C, del)` 只处理当前槽。部分删除只改当前槽 bytes；整条 edge
  删除时由 `R` 的构造保证该 edge 必为叶子末槽：`atK_forwardoff` 在刚好到达
  下一 edge 边界时会前进到 successor，因此 `fl > levels` 的整槽删除只可能
  发生在 `C`/`R` 都被 `atK_locend` 归一到树尾 edge 的情形，断言
  `i + 1 == cc`。此时若 edge 空，断言 `i == 0` 后 `atN_remove`（唯一空
  edge 只能是首 edge）；否则保留该 edge 并把 `bytes[i] = 0`（EOF 零长
  edge 的由来）。删除后断言 `cc == 0 || i < cc`；不存在 successor 分支，
  也不存在 `i == cc` 尾修正分支。
- 跨 edge 时 `T->seam = atK_edge(&R)`，断言 `seam` 不是左边界 edge；走
  `atD_rmrange(C, &R, fl)`。rmrange 用 `at_State.rt[]` 暂存 R 侧子树，
  `atD_trimright`/`atD_trimleft` 修剪左右边界，`atD_cutrange` 把 R 侧槽拷入
  `rt[]` 并 purge L 侧；`atN_purge` 把被回收叶子 edge 的桶逐槽搬到 `T->seam`。
  `atD_stitch` 先按 `spD_stitch` 同形的通用清理删除 L 叶的零长边界槽，再重接
  `rt[]`；该清理不区分 EOF 零长 edge 与普通槽位，不是零 span 特判，被删槽的
  端点由 purge 搬进 `T->seam`。
- `atD_cutrange` 对 leaf 层 `rt[0]` 始终复制 R 当前槽（`i += (k != 0)`），
  不检查其 `bytes`。spantree 的 `k || bytes[i] == 0` 跳过条件在 anchortree 会
  把 R 边界槽留进 L 的待删区间；若该槽是 `T->seam`，随后的 `atN_remove` 会
  在 purge 时命中 `edge != seam` 断言。因此这里必须复制 R 当前槽；这不是为
  0 span 增加分支，而是去掉 spantree 的零长跳过特例。
  `atD_mergeleaf` 只把 Cursor 移到 append 位置；首 edge 即使桶空也保留。
- `atD_stitch` 末尾以结构断言收口：`assert(cc != 0 && i < cc)`。该断言由
  `atD_mergeleaf`/`atD_stitchnode`/`atD_rebalance` 维护：`rt[0]` 恒非空
  （`atD_cutrange` 对 leaf 层始终复制 R 当前槽），`atD_stitchnode` 至少在 L
  叶留下一个槽，因此 `cc != 0`；`atD_mergeleaf` 把 Cursor
  放到 append 位置后，`atD_stitchnode` 要么在旧 `cc` 处追加（新 `cc` 更大），
  要么在叶子满时 `atD_findroom` 重新锚定到新节点首个槽；foldnode/rebalance
  只合并不变量允许的节点，不产生 `i == cc`。因此不需要 spD_stitch 的 inline
  tail 修正，只保留断言。
- 删除后 `T->seam` 清回 `NULL`。删除 mutation 的 rewind 统一由
  `atD_remove` 出口负责；`at_remove`/`at_splice` 不再各自调用 `atK_rewind`，
  del+ins 路径最后由 `atK_forwardoff` 出口重新播种。`at_delanchor` 与
  insertion 路径产生的空 edge 由 `atE_purge` 回收，它不参与 D 类删除。

### 7.3 `at_splice(C, del, ins)` 与公共 API

- `at_splice` 语义为“删除 `[C, C+del)` 后再在 C 处插入 `ins`”，Cursor 结束于
  `offset + ins`。
- reserve 统一走 `atU_reserve(C, edges, pass, extra)`，一次预留
  `pass*L + extra` 个 nodes 与 edges；公共入口不再直接调用 `atP_reserve`。
  `pass` 与 `extra` 由调用者按事务内容精确计算；helper 不解释 pass 的
  remove/insert 来源，避免抽象泄露。
- 调用值：
  - `at_remove`：`(0, 1, 0)`；remove 侧总分配 ≤ L。
  - `at_insert`/`at_append` 经 `atI_edit`：`(1, 1, 2)`；一次
    `atI_makeroom` 最坏 `L+2`。
  - `at_splice`：`(1, rm + add, 2*add)`，其中
    `rm = del > 0 && pos < T->bytes`、`add = ins > 0`；一次 remove 加至多
    一次 insert；纯删除时多预留 1 条 edge，属允许的常数余量。
  - `at_newanchor`：`(3, 2, 5)`；两次 `atI_ensure`，edges 最大 3，nodes 为
    `2L + (L+2 + L+3 - 2L) = 2L+5`。
- `atP_reserve` 是“至少 n 个空闲对象”的下界保证，不是累加额度；`atU_reserve`
  对 nodes 只调用一次，edges 只调用一次，不存在同 pool 二次 reserve 的 `max`
  语义问题。
- remove 的 `L` 依据：`atD_stitchnode` 的延迟分段修复 `kl -> l` 各段不重叠，
  `atD_makechain` 只在第一次建链时分配；最坏左树满时链高为 `L`。
- insert 的 `L+2` 依据：`atI_makeroom` 最坏根分裂新增 2 个节点、之后每层
  `atI_splitchild` 新增 1 个；第 j 个 insert pass 时树高最多 `L+j`，上界
  `L+j+2`。因此 n 个 insert pass 的 `extra` 为 `2*n + n*(n-1)/2`。
- 删除阶段按 §7.2 处理端点塌缩与 edge 删除；插入阶段按 §7.1 处理端点迁移与
  edge 扩展。
- 预留成功后、第一次树 mutation 前递增 `T->epoch` 并刷新传入 Cursor；删除与
  插入都不再调用 `at_seek`/`at_locate` 事后归位：删除由 stitch/rmleaf 自维护，
  插入由 `atI_insert` 维护；删除 mutation 的 rewind 集中在 `atD_remove` 出口。

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
AT_API int at_remove(at_Cursor *C, size_t len);
AT_API int at_insert(at_Cursor *C, size_t ins);
AT_API int at_append(at_Cursor *C, size_t ins);
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
- `at_remove` 过期返回 `AT_ESTALE`；`len == 0` 或 `at_offset(C) >= T->bytes`
  为纯 no-op，不 reserve、不递增 epoch；`len` 进入删除流程后按树尾截断；
  成功后 Cursor 停在删除起点。
- `at_insert` 在 `C` 处插入 `ins` 字节，成功后 Cursor 停在原 `offset`；
  `at_append` 在 `C` 处插入并停在 `offset + ins`；`ins == 0` 为纯 no-op。
  两者在 mutation 前分别预留 nodes/edge，并检查
  `max(pos, T->bytes) + ins` 溢出；失败返回 `AT_ERRMEM`/`AT_ERROVERFLOW`，
  树、Cursor、epoch 均不变。
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
  一个 EOF 零长 edge。
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
  `atK_forwardoff(&R, del)`；`atD_diverlevel` 比较 `C`/`R` 后直接选
  `atD_rmleaf` 或 `atD_rmrange`，不再有 successor 扫描。公共入口
  `at_newanchor` 在传入 Cursor 上用 `atK_forwardoff`/`atK_locend` 物化 end edge，
  不属于内部 helper 强制定位，也不建立第二个 Cursor。
- 桶迁移使用 `atE_movelink`：逐槽按 `is_end` 读出后继，更新
  `anchor->start/end` 后推入目标桶；不合并、不去重、不预留额外内存。
- 所有 reserve 统一走 `atU_reserve(C, edges, pass, extra)`；内部 helper 不再
  自行 reserve，只用 `atP_ralloc`/assert。`atU_reserve` 只做
  `atP_reserve(nodes, pass*L + extra)` 与 `atP_reserve(edges, edges)`；
  `pass`/`extra` 由调用者精确计算，helper 不解释 pass 来源。
  调用值：`at_remove` `(0,1,0)`；`at_insert`/`at_append` `(1,1,2)`；
  `at_splice` `(1, rm+add, 2*add)`；`at_newanchor` `(3,2,5)`。依据：
  - remove pass：`atD_stitchnode` 的延迟分段修复 `kl -> l` 不重叠，
    `atD_makechain` 只在第一次建链时分配，最坏左树满时链高为 `L`；
  - insert pass：一次 `atI_makeroom` 最坏分配 `L+2`（根分裂新增 2 个节点，
    之后每层 `atI_splitchild` 新增 1 个）；第 j 个 insert pass 时树高最多
    `L+j`，上界 `L+j+2`，n 个 insert pass 的 `extra = 2n + n(n-1)/2`。
  `atP_reserve` 只扩容 pool，失败不产生任何新 edge/节点，也不改变树。
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
- 内部前置条件用 `assert` 表达，不写防御分支：`atI_splitchild` 断言两侧子节点
  均不少于 `AT_FANOUT / 2`；`atI_splitat` 在 `atI_makeroom` 前断言
  `C->poff > 0`、之后断言 `C->poff < p->bytes[i]`；`atI_pad` 断言当前 edge
  是叶尾槽且 `C->poff == p->bytes[i]`；`atI_insertsplit` 断言
  `C->poff == 0` 且当前 edge 的 RIGHT 桶非空。
- `atI_makeroom` 自底向上找可分裂祖先并分裂到叶子，不预检当前叶子，与
  `spI_makeroom` 同形。`atI_splitat`/`atI_pad`/`atI_insertsplit` 因此都先无条件
  调用 `atI_makeroom(C)`，再计算一次当前 `p/i`（`atI_insertsplit` 再取 `e`），
  不在调用方重复 `if (cc >= AT_FANOUT) { makeroom; 重算; }`。
- `at_next` 是单函数结构：入口校验 → 当前 edge 虚拟/越界检查 → 当前 edge 桶
  扫描 → `atK_nextedge` 前进并 `atK_rewind` → 用 `at_range` 填充 `plen`。
  不保留独立 `atK_step` 包装层。`atE_nextstart` 对应 `spN_findslot`，
  `atK_nextedge` 对应 `sp_next` 的 ascend/descend 段；`atK_nextedge` 不内联进
  `at_next`，因为内联后会超过函数 30 行硬限。找到 id 后计算 `plen`，耗尽不写
  flag，Cursor 保持在坐标 + `side == 2` 的自然耗尽态。`atE_nextstart` 直接返回
  `at_Id`，不再有 `out` 参数；`id` 在声明时初始化由函数返回值负责。
- `atE_nextstart` 只沿当前 edge 的两个桶线性推进，续扫位置保存在
  `C->side`/`next_id`/`next_end` 中；单次 `at_next` 在过滤条件下最坏需要
  跨过其后所有无命中 edge，代价 O(剩余 edge 数 × 树高 + 桶槽数)，整轮
  迭代与扫描总量同阶，不会在同一位置反复从头扫描。
- `atN_purge` 的叶节点早退分支保留 `if (k == 0) ... return;` 单行返回；
  `atI_ensure`/`atI_splitat`/`atI_pad`/`atI_emptyedge` 已改为单返回值，
  root 空、`poff == 0`、span 内、tail/virtual 各分支直接 return edge，不再有
  out 参数。
- `epoch` 是 `unsigned` 的相等性失效计数：`at_remove`/`at_splice` 在 reserve
  成功后、mutation 前递增并刷新传入 Cursor；`at_insert`/`at_append` 在
  `atI_edit` 内做同样的事；`at_newanchor` 在 reserve 与 start/end
  edge 物化、链接完成后、成功返回时递增，但不刷新传入 `C->epoch`，使旧
  Cursor 过期；`at_delanchor` 在取得 live anchor 后、摘槽前递增；`at_reset`
  清空前递增。`at_seek`/`at_locate` 只捕获/刷新，不递增。reserve 失败或参数
  错误等未 mutation 的失败不递增，Cursor 保持有效。只做相等比较，允许自然
  回绕，不为了“避免回绕”升宽。
- 公共 API 在解引用 `paths` 前检查 `C->epoch == C->tree->epoch`；过期时
  `at_advance`/`at_remove`/`at_insert`/`at_append`/`at_newanchor`/`at_splice`
  返回 `AT_ESTALE`，`at_next` 返回 `AT_NONE` 且写 `*plen = 0`。不提供
  `at_epoch` getter。
- 可优化项：`atE_unlink`/`atE_movelink` 目前从桶头线性扫描同侧链；引入
  `sprev/eprev` 双向链后单槽删除可 O(1)，代价是每 anchor +8B，暂不实施。
- debug checker 与共享模型位于 `tests/at_tests.h`（`att_checktree`/
  `att_checkchains`/`att_checkcursor`/`att_checkdepth`/`att_seq`/`att_Model`），
  单测与 fuzz 共享同一份；不搬入 `anchortree.h`。`att_checkchains` 做全量链表
  不变量检查，`att_checktree` 做结构检查：每个桶的槽都属于 live anchor、side/
  角色正确、每个 live anchor 恰出现一次 START/END；空树额外要求 `levels == 0`；
  叶子层首 edge 位置为 0、除末位 EOF 零长 edge 外无 0 长槽、中间 edge 必须有槽、首 edge
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

- 删除流程采用 spantree 的双 Cursor 形态：`atD_remove` 以传入 `C` 为左边界，
  本地 `R = *C` 后 `atK_forwardoff(&R, del)`，到/越过 `T` 时 `atK_locend` 归一
  到尾 edge。`atD_diverlevel` 区分同 edge 的 `atD_rmleaf` 与跨 edge 的
  `atD_rmrange`；跨 edge 时 `T->seam = atK_edge(&R)`，`atD_cutrange` 借助
  `at_State.rt[]` 暂存 R 侧子树，`atN_purge` 把被回收叶子 edge 的桶搬到 seam，
  `atD_stitch` 按 spD_stitch 的通用零长边界槽清理后重新接回 `rt[]`，末尾以
  `assert(cc != 0 && i < cc)` 收口。
- `atD_rmleaf(C, del)` 只处理当前槽：部分删除只改当前槽 bytes；整条 edge
  删除必在叶子末槽（`assert(i + 1 == cc)`），空 edge 只能是首 edge
  （`assert(i == 0)`），否则保留 EOF 零长 edge；删除后
  `assert(cc == 0 || i < cc)`。
- `atD_mergeleaf` 不做字节回收：没有可合并的 id/mask，只需把 Cursor 移到
  append 位置。spantree/piecetab 的 id/mask 合并分支在 anchor 领域不存在。
- `atD_makechain` 不创建新根：`atD_findroom` 先断言待插入层没有右兄弟，直接
  在现有链上补节点；spantree 的 new-root 与“搬右兄弟”段没有公共 witness。
- `atD_stitch` 用末尾结构断言替代 spD_stitch 的 inline tail 修正；该分支在
  anchortree 删除调用图中不可达（证明见 §7.2 与 coverage exemptions）。
- `atE_purge(C)` 只服务 insertion 与 `at_delanchor` 的空 edge 回收，按首 edge
  规则把 bytes 并入前驱并复用 `atD_rebalance`；`atD_rmrange`/`atD_rmleaf`
  不调用它。
- `atD_balancenode` 保留 `int d + at_Delta *ds` 的多输出形态，与
  `spD_balancenode` 同步；跨库统一（调用方计算 `d`、helper 单返回 `ds`）列入
  TODO.md，不在此单独改签名。
- 单槽删除加 `atD_foldnode`/`atD_rebalance` 足以维持不变量；
  根占用不变量（`levels > 0` 时根孩子数 `>= 2`）由 `atD_rebalance` 末尾的
  collapse 循环在单孩子根出现时立即折叠保证。该不变量由共享 `att_checktree`
  固定为测试断言。
- 插入流程统一计算 grow delta：span 内部或末尾、RIGHT 桶为空、虚拟尾都走
  `atE_resize`，空树分支直接内联在 `atI_insert`（旧 `atI_insertempty` 已删除）；
  只有 edge 位置且 RIGHT 桶非空时才进 `atI_insertsplit`，它先 `atI_makeroom` 再
  重新计算 `p/i/e`，然后 `atE_resize` + `atN_makespace` 在 `p+ins` 建新 edge 并
  `atE_movelink`。`atI_splitchild` 内联节点对半拆分逻辑（与 `spI_splitchild`
  同形），不再抽 `atI_splitnode`；右节点数与左右分界都在调用方就地计算。删空的
  内部 edge 统一由 `atE_purge` 收尾，并保持 Cursor 在原来的绝对坐标
  （`atE_purge` 删除后把 `poff` 调回删除前的坐标）。
- `atK_locend` 带空树保护：空树写 `paths[0] = root.children`、
  `off = poff = 0`，不假设存在叶子槽；非空树以 `off = T->bytes - poff` 收尾。
  其 0/1 返回值保留：`at_locate` 的早退分支用
  `return atK_locend(C), ..., AT_OK;` 做 C89 提交语义（§12.2）。

### 块顺序与 sp 映射

spantree 的物理顺序是 `/* remove */` → `/* insertion / split */` → `/* fill */`。
anchortree 采用同一顺序，并在前面保留 metric/path helpers 与 cursor navigation：

1. `/* metric/path helpers */`
2. `/* cursor navigation */`
3. `/* remove */`（含末尾的 `atE_purge`）
4. `/* insertion / split */`
5. `/* insertion */`
6. `/* APIs */`

remove 块只依赖 `atN_*`/`atP_*`/`atE_*`/`atK_*`/`atM_*`，不调用 `atI_*`；
`atE_purge` 位于 remove 块末尾，insertion 与 `at_delanchor` 在它定义之后调用。
`atU_reserve` 位于 `/* object/edge helpers */` 段末，只依赖 `atP_reserve` 与
`at_Tree.levels`，不调用 `atI_*`/`atD_*`；它是 mutation 前的统一资源预留
helper，spantree 没有对应 helper（各入口直接 `spP_reserve`），差异登记见 §12.4。

| spantree 块 | anchortree 块 / 顺序 | 说明 |
| --- | --- | --- |
| `/* remove */` | `/* remove */`，在 `/* insertion / split */` 之前 | `atD_trimright` 到 `atD_remove` 与 spantree 顺序一致；`atE_purge` 为领域空 edge 回收 |
| `/* insertion / split */` | `/* insertion / split */` + `/* insertion */` | 前段：`splitroot`/`splitchild`/`makeroom`/`splitat`/`pad`；后段：`atI_emptyedge`/`ensure`/`insertsplit`/`insert`/`atI_edit` |
| `/* fill */` | 无独立块 | anchortree 无 ns mask；`at_State.rt[]` 只供删除/stitch 使用，object 级遍历由 `at_next` 承担 |

`atA_*`/`atE_*`/`atK_*`/`atD_*`/`atI_*` 与 sp 的对应关系：

| anchortree | sp 对应 | 差异边界 |
| --- | --- | --- |
| `atP_*`/`atS_*`/`atN_*`/`atM_up` | `spP_*`/`spS_*`/`spN_*`/`spM_up` | 基本同构 |
| `atA_*`（`slot`/`alloc`/`free`/`fromid`/`clear`） | spantree 无 anchor 池 | 稠密 id 池与 freelist 属 anchortree 领域 |
| `atE_*`（`empty`/`new`/`link`/`unlink`/`movelink`/`next`/`nextstart`/`resize`/`purge`） | `spA_*`（`born`/`died`/`merge`/`touch`）、`spL_*` 读路径 | `atE_resize` 对应 `spA_touch`；`atE_movelink` 是删除期端点搬迁目标；`atE_purge` 是 insertion/delanchor 的空 edge 回收 |
| `atK_*`（`locend`/`findleaf`/`forwardoff`/`backwardoff`/`nextedge`/`rewind`/`edge`/`edgepos`/`fromedge`/`prevedge`/`none`）+ `at_next` | `spK_*` + `sp_next` | `atK_forwardoff` 额外处理到达/越过 `T` 时的 `atK_locend` + 虚拟 `poff`；`atK_fromedge`/`atK_edgepos`/`atK_prevedge`/`atK_nextedge` 是 edge/span 现场路径 helper，spantree 以 mask 与叶顺序代替；`at_next` 单函数结构对齐 `sp_next`，`atE_nextstart` 对应 `spN_findslot` |
| `atD_*` | `spD_*` | 同构的 trimright/trimleft/cutrange/mergeleaf/balancenode/foldnode/rebalance/backwardnode/makechain/findroom/stitchnode/stitch/rmrange/rmleaf/diverlevel/remove；`at_State.rt[]` 暂存 R 侧子树，`T->seam` 是 purge 的端点搬迁目标。领域差异：`makechain` 无 new-root、`findroom` 禁止右兄弟、`mergeleaf` 无 id/mask 合并、`rmleaf` 只处理当前槽且整条 edge 删除必在末槽（assert）、`atD_stitch` 末尾以 `assert(cc != 0 && i < cc)` 收口（sp inline tail 修正不可达） |
| `atI_*` | `spI_*` + `sp_append`/`sp_insert` | `atI_emptyedge`/`ensure` 为 start/end edge 物化；`insert` 内联空树初始化；`ensure`/`emptyedge`/`splitat`/`pad` 单返回值，无 out 参数；`atI_splitchild` 内联节点拆分；`atI_edit` 共享 `at_insert`/`at_append` 的 reserve/overflow/epoch/`atI_insert` 流程，公共 wrapper 只加 rewind/forward |

## 十二、与其他树库的差异

> 本节登记 anchortree 与 spantree.h / piecetab.h / linecache.h 的差异台账。
> 基准关系：K/D/I 主干以 spantree.h 为参考；piecetab.h / linecache.h 只用于
> 跨库形态对照，不因它们的领域结构改变 anchortree 主干。
> 新增差异必须先在本节登记，并给出领域原因或“无法同构 / 同构代价过高”的证明；
> 未登记的差异一律视为实现偏差。

### 12.1 reserve 契约

- 所有 reserve 统一通过 `atU_reserve(C, edges, pass, extra)` 完成；public API
  不再直接调用 `atP_reserve`。helper 位于 `/* object/edge helpers */`，只做：
  `atP_reserve(nodes, pass*L + extra)`、`atP_reserve(edges, edges)`。
- `pass` 与 `extra` 由调用者精确计算，helper 不解释 pass 是 remove 还是
  insert，也不接受 rm/ins 分类参数。允许非 L 级的常数余量（例如 `edges`
  固定传 1、`at_newanchor` 传 3；`extra` 是常数）；禁止任何随 `L` 增长的
  余量。`nodes` 计算保持一行 `pass*L + extra`，不为消除常数余量增加代码。
  `at_splice` 一次调用传
  `(1, rm + add, 2*add)`，其中 `rm = del > 0 && pos < T->bytes`、
  `add = ins > 0`；禁止拆成 remove reserve + append reserve 两次调用。
- `atP_reserve` 仍是“至少 n 个空闲对象”的下界保证，不是累加额度；因为
  `atU_reserve` 对同一 pool 只调用一次，`max` 语义不再参与求和。

### 12.2 返回值契约

- 内部函数允许有返回值仅限两类：调用方消费该值，或用于 C89 提交语义
  （`return helper(...), 0/1;`）。
- 无外部消费者、但调用方用其 0/1 做提交语义的 helper，保留 `int`：
  - `atK_forwardoff` / `atK_backwardoff`：`at_advance` 以
    `return helper(...), AT_OK;` 提交；
  - `atI_insert`：`atI_edit` 以
    `return atI_insert(C, ins), assert(...), AT_OK;` 提交；改 `void` 会在
    空树分支产生孤立 `return;`；
  - `atK_locend`：`at_locate` 的早退分支以
    `return atK_locend(C), ..., AT_OK;` 提交。
- 当前既无消费者也无提交语义的返回只有 `atE_unlink`：改 `void`，并以
  `if`/`else` 收尾，避免孤立 `return;`。
- `atK_nextedge` / `atE_empty` / `atI_ensure` 等返回值有消费者，保持非 void。
- 参考库中同名函数仍可能返回 `int`；跨库统一列入 TODO，不在 anchortree 侧保留
  死返回。

### 12.3 结构同构项（不得保留实现偏差）

- `at_locate` 与 `sp_locate` / `pt_locate` 同形：首分支 early-return，尾部直接
  return，不用 `else`。
- `at_next` 是单函数结构：入口校验 → 当前 edge 虚拟/越界检查 → 当前 edge 桶
  扫描 → `atK_nextedge` 前进并 `atK_rewind` → 填充 `plen`。不再保留独立
  `atK_step` 包装层。
  - `atE_nextstart` 对应 `spN_findslot`：在“当前槽”内寻找下一个匹配项。
  - `atK_nextedge` 对应 `sp_next` 内的 ascend/descend 段；它不并入 `at_next`
    的原因是内联后会超过函数 30 行硬限。
  - `plen` 由 `at_range` 计算是领域差异：同一 edge 可含多个 anchor，长度不是
    单个叶子槽的 `bytes`。
- `atI_splitchild` 内联节点对半拆分逻辑，与 `spI_splitchild` 同形；不保留
  `atI_splitnode` 单调用 helper。
- `atI_makeroom` 与 `spI_makeroom` 同形，只保留自底向上的查找/分裂循环，不预检
  当前叶子。
- `at_append` 在 `atK_forwardoff` 之后不再重复 `atK_rewind`；`atK_forwardoff`
  所有出口已统一 rewind。
- `at_splice` 不保留简单分支在 `else` 的形态。删除 mutation 的 rewind 集中在
  `atD_remove` 出口：`at_remove` 与 `at_splice` 都不再各自调用 `atK_rewind`；
  del+ins 路径中 `atI_insert` 只依赖 `paths`/`off`/`poff`、不读桶游标，
  因此 `atD_remove` 的 rewind 只是统一播种，最后由 `atK_forwardoff` 出口再次
  重新播种。
- `at_remove` 不重复 clamp；`len` 进入删除流程后由 `atD_remove` 统一按树尾截断。
- `atD_remove` 在 `atK_forwardoff` 后不再重复 `atK_locend`；`atK_forwardoff` 的
  契约已包含到达/越过 `T` 时的归一化。
- `atE_purge` 删除当前 edge 后，先把 `C->paths[l]` 指到同一叶内的有效幸存槽
  （`i < cc ? i : cc - 1`），`cc == 0` 时指向 `p->children`，再调用
  `atD_rebalance`；删除完成后的最终 rebase 用 `atK_fromedge(prev)`，它是从 edge
  现场（`parent`/`idx`）构造 Cursor，不是 `locate`/`seek` 重新搜索。

### 12.4 领域差异（允许保留）

| 差异 | 领域原因 | 同步策略 |
| --- | --- | --- |
| run id/mask → anchor 池 + 桶链表 | 一个 edge 可挂多个 START/END；ns 是 29bit 精确值，不是位掩码 | 保留；D/I 主干只删去 run 合并分支 |
| `atD_mergeleaf` 不合并 id/mask | anchor 域无 run id 可合并 | 保留；等价结构由桶链表承担 |
| `atD_makechain` / `atD_findroom` 不建新根、不搬右兄弟 | 删除 invariant 保证 divergence 及以下 L 右侧已清空，根必有空位 | 保留；由结构断言与 fuzzer 守 |
| `atD_stitch` 去掉 inline tail 修正 | `rt[0]` 恒非空 + `atD_mergeleaf` 定位不变量使 `i == cc` 不可达 | 保留；以末尾断言守 |
| EOF 零长 edge、首 edge 永久保留 | 左边缘 span 模型承载 T 位置端点与 0 边界 | 保留 |
| `atE_purge` / `T->seam` 端点搬迁 | 回收 edge 前需把桶槽搬到幸存 edge | 保留 |
| `atK_forwardoff` 支持虚拟尾 | 删除后插入、`at_newanchor` end 物化都可能跨 T | 保留；同族仅 spantree 有虚拟区，引入只在 spantree.h 走跨库设计 |
| `at_next` 的桶游标状态 `side`/`next_id`/`next_end` | 同一 edge 多个 anchor，必须保存桶内续扫位置 | 保留；结构对应关系见 §12.3 |
| `atI_ensure` / `atI_emptyedge` / `atI_splitat` | anchor 端点物化需要在指定坐标创建/取得 edge | 保留；对应 sp 的 pad/grow 分支拆分 |
| `at_insert` 与 `at_append` 共用 mutation | 无 run id，不需要 sp 的 `growleft` 语义 | 保留；与 piecetab/linecache 的 append+advance 模式一致 |
| epoch / `AT_ESTALE` | Cursor 可能因 mutation 悬空 | 保留；跨库推广见 TODO |
| 节点池 + edge 池分离 | edge 是独立于节点的领域对象 | 保留 |
| `atU_reserve` 统一预留 | anchortree 必须在 mutation 前把 remove+insert 的 nodes/edges 一次算清（`at_splice` 只允许一次 helper 调用）；spantree 各入口直接 `spP_reserve`，没有跨 pass 合并的 helper | 保留；helper 只做池预留，不解释事务来源 |

### 12.5 跨库后续项

- `spantree.h` 可单独引入 `atK_forwardoff` 的虚拟尾语义，但它属于跨库设计变更：
  先更新 spantree 的 design/TODO 与测试，再动实现；piecetab/linecache 无虚拟区
  语义，不参与。
- 返回值去掉死 `int`、`sp_splice` 事务化、epoch 推广等，按 TODO.md 跨库推进；
  anchortree 侧不因为参考库同名而保留无消费者的返回值。
