# anchortree 设计（左边缘 span + EOF 零长 edge 模型）

> 状态：实现设计（左边缘 L2–L4 迁移中）。
> 术语：坐标结构统一用 `edge`；`at_Point` 是 anchor 端点的 occurrence（point）。
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
- `at_Point` 的完整定义位于公开 `/* structure */` 段、在
  `struct at_Cursor` 之前；`at_Cursor` 直接内嵌 `at_Point cur`，因此
  `at_Point` 是公开布局的一部分。
- 每个 `at_Edge` 有 `left/right` 两个桶头；`id == 0` 表示空桶。
- 端点槽不单独分配：每个 live anchor 的两个槽内嵌在 anchor 中，见 §2.4。
- 同一 anchor 的 START/END 落到同一桶时，按 `is_end` 标签串在同一链上；不合
  并、不去重。
- 树内 edge 必须至少有一个端点槽，但首 edge 例外：`T > 0` 时首 edge 即使两桶
  全空也永久保留，作为 0 边界与前导 span。EOF 零长 edge 在桶空后删除。
- 任何新 edge 在创建后必须立即获得端点槽；因为创建前 reserve 已保证后续不失败，
  `at_newanchor` 通过两次 `atI_addpoint` 无失败地完成 edge 物化与端点链入。
- 删除/切割不创建带槽临时 edge：幸存目标由 `T->seam` 指定，被回收叶子 edge
  的桶由 `atN_purge` 逐槽搬进 `seam`，随后 edge 槽位由 `atD_cutrange`/stitch
  重新接回左子树；`at_delanchor` 摘槽后空掉的 edge 由 `atE_purge` 回收，
  插入不再产生空 edge（见 §7.1）。

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
- `T->anchors` 是稠密数组；`at_Id` 从 1 开始，0 保留为“空/未使用”；
  `AT_NONE == 0` 即由此而来。
- 空闲 id 用槽内前 4 字节串成 freelist，`T->idfree == 0` 表示 freelist 空。
- `anchor->end == NULL` 即非 live；live anchor 的 `start/end` 均非 NULL。
- 空闲槽（从未分配或已释放）都满足 `end == NULL`：释放路径由 `atA_free`
  写入；扩容路径在串 freelist 时对该段每个新槽（含链尾）写入。`atA_alloc`
  返回的槽由 `at_newanchor` 整体清零后再初始化，因此 `atA_fromid` 的
  live 判定对任意 `id <= T->idmax` 成立。
- `at_Id` 复用顺序不是公共契约：`at_newanchor` 返回的 id 保证有效；已释放且
  未被复用的 id 在 `at_range`/`at_gravity`/`at_delanchor` 上继续报错。调用方
  只能依赖“新 id 有效、其他已释放 id 失效”，不得断言 LIFO/FIFO 复用顺序。
- `ns` 占 29 bit，写入前由 `at_newanchor` 校验 `0 <= ns <= AT_MAX_NS`；两个
  side 位各占 1 bit。
- 容量上限 `AT_ANCHOR_MAX == (1u << 31) - 1`。
- `at_Id` 直接索引 `T->anchors`，查询 O(1)；删除需要从桶头线性扫描同侧链
  （O(k)，k 为该桶槽数）。
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
    at_Point   cur;     /* last consumed slot */
    unsigned   epoch;   /* tree generation captured by seek/locate */
    at_Node  **paths[AT_MAX_LEVEL]; /* root-to-leaf path */
} at_Cursor;
```

- `off + poff` 是 Cursor 的绝对坐标；当前 edge 是 `*paths[levels]` 指向的叶子槽，按 `at_Edge *` 解释。
- **树中段尾禁令**：当前 edge 不是最后一条时恒有 `C->poff < p->bytes[i]`（`p` 为当前叶子节点）；
  只有当前 edge 是最后一条时才允许 `C->poff >= p->bytes[i]`。该条件是快速判断
  “Cursor 是否位于树末”的依据，多个 API 依赖它。
- 指向树内 edge@`p` 时 `off + poff == p` 且 `poff == 0`；位于 span 内部时 `0 < poff < bytes`；超过最后一个 edge 时停留在最后一个叶子槽，`poff >= bytes` 为虚拟距离；空树由 `paths[0] == root.children`、`off == 0`、`poff == pos` 表达，没有 edge 可指。
- `cur` 是“最后已消费/续扫槽位”：有效槽为 `cur.id != 0`，下一次从
  `atE_next(cur, anchor)` 开始，不是“下一个待消费槽”。4 字节 `at_Point`
  直接嵌入 Cursor，不再拆成 `cur_id`/`cur_end` 两个字段（8 字节→4 字节）。
  - fresh（`cur.id == 0 && !cur.is_end`）：当前 edge 尚未产出 anchor；
    `at_next` 在 `poff == 0` 时从当前 edge 的 `atE_first` 桶头开始。
  - done（`cur.id == 0 && cur.is_end`）：当前扫描范围已耗尽，`at_next`
    幂等返回 `AT_NONE`。anchor id 0 从不分配，`AT_NONE == 0` 既是“无
    id”也是 Cursor 的 fresh/done 基线，由 `is_end` 区分。
  - `cur.id != 0`：最后已消费槽；`cur.is_end` 决定取
    `anchor->end`/`end_right` 还是 `anchor->start`/`start_right`；
    `at_next` 只产出 START 槽（`is_end == 0`）。
- `at_next` 由两个层次组成：
  1. `at_next`：公共入口校验；空树或 done 直接耗尽；`cur` 为 fresh 且
     `poff == 0` 时把 `C->cur` 置为当前 edge 的 `atE_first`；随后进入
     `while (C->cur.id || atK_nextedge(C).id)` 循环：body 先测试当前
     candidate，匹配 `ns` 的 START 用 `at_anchor` 填充 `plen` 并返回；
     不匹配时以 `p` 保存候选，用 `atE_next(p, anchor)` 沿桶链推进；桶内
     耗尽且 `p` 在 left 桶时跨到 `e->right`；`C->cur` 清 0 后由下一次
     循环条件的 `atK_nextedge` 转入下一 edge。
  2. `atK_nextedge`：提交 cursor 到下一 edge：当前 leaf 的
     `poff >= bytes` 直接写 done；有后继时 `off += bytes[current]`、
     `poff = 0`，把 `C->cur` 置为下一 edge 的 `atE_first` 并返回它；无后继
     时由 `l < 0` 分支直接把 `C->poff` 设为当前 edge 末尾并写 done。
  - `at_next` 循环的 `p` 只保存“上一个已消费/待推进槽位”；`C->cur` 是当前
    candidate，匹配时用 `at_anchor` 填充 `plen`。
- 正常耗尽：`atK_nextedge` 的两个无后继出口都把 `C->cur` 写为 done
  `{0,1}`，`at_next` 循环因此退出并返回 `AT_NONE`。参数错误、过期 Cursor
  等非正常耗尽不改 `cur`、不改坐标。
- `atK_nextedge` 的提交规则：当前 leaf 的 `poff >= bytes`（已在/越过 `T`）
  直接写 done，不移动；有后继时 `off += bytes[current]`、`poff = 0`，落到下一
  edge 并把 `C->cur` 置为 `atE_first(next edge)`；`l < 0` 无后继时
  `C->poff = bytes[current]`、`C->cur = {0,1}`：此时当前 edge 已是最后一条，
  `paths/off` 保持现状，由 `bc` 直接提交尾坐标。
- 末尾 0 长 edge 的 START 按正常 edge 序遍历：cursor 在它之前时
  `atK_nextedge` 把它当作后继 edge 落脚，`at_next` 下一轮扫描它；只有
  cursor 已在 `T` 之后（虚尾）时 `poff >= bytes` 直接耗尽。
- `atK_rewind`：非空树写 `C->cur = atU_point(0, 0)`，空树写
  `C->cur = atU_point(0, 1)`。
- `atK_fromedge`：写 `C->cur = atU_point(0, 0)`（不是 done），并在建立 paths
  时把 `C->off` 置为当前 edge 起点，返回该位置；它的语义是“当前 edge 尚未
  开始”。`at_range`/`atE_purge` 消费它。
- `at_advance(C, 0)` 保留 `cur` 与坐标；`d != 0` 经
  `atK_forwardoff`/`atK_backwardoff` 后由 `atK_rewind` 复位；
  `at_seek`/`at_locate` 刷新 epoch 并把 `cur` 复位。
- 桶链扫描由 `at_next` 循环 body 直接完成：`atE_next(p, anchor)` 取单链
  后继，left→right 桶切换由 `C->cur.id == 0 && !atA_side(...)` 的
  `e->right` 分支完成；跨 edge 由循环条件的 `atK_nextedge` 完成。
  `atE_first` 是选择当前 edge 首个 occurrence 的宏（left 非空取 left，
  否则取 right）。
- insert 的耗尽态契约：`atI_insert` 不创建 anchor；它按 §7.1 的三路状态决定
  growspan/growleft/split，并按 `advance` 提交 Cursor/cur。空树由
  `atI_makefirst` 建首 edge 并保持 cur；当前 span 增长保持 cur；R10 由
  `atI_growleft(ins, advance)` 处理：叶内/跨叶共用一趟下降，按 `advance`
  在循环外选择两个变体——`advance=0` 的变体在 mutation 时同步写
  `C->paths` 并无条件写 fresh，`advance=ins` 的变体只 mutation 并保持端点
  为当前 edge 的 cur。`atI_split` 只保留 insertion 模式：`advance=0` 时 RIGHT
  桶 cur 写 fresh、LEFT 桶与 id0 保持；`advance=ins` 时 RIGHT 桶 cur 保持、
  LEFT 桶与 id0 写 fresh。`atI_splitpoint` 的 point 模式不改 cur，由
  `atI_addpoint`/`at_newanchor` 的 epoch 失效契约收口。
  `atE_purge` 只服务 `at_delanchor`，不再触碰任何 Cursor。`atD_rmrange` 是
  例外：仅当 `L->poff == 0`（L 零长末槽将被 purge、其桶前置到 seam）且旧
  `cur` 属于 L 桶时，才保留旧 `cur` 或改用 `left_end`；`L->poff > 0` 时一律
  fresh seam，避免越过 seam 新前插的 left 节点。
- 结构 mutation 后 Cursor 现场：`at_remove`/`at_insert`/`at_append`/`at_splice`
  在操作内维护同一个 Cursor 并刷新 epoch，不调用 `at_locate`/`at_seek`
  事后归位；插入由 `atI_insert` 按 §7.1 的 `advance` 规则维护
  paths/off/poff/cur；rmleaf 链不变；rmrange 按 §7.2 设置续扫槽。
  `at_delanchor` 不维护 Cursor，旧 Cursor 失效；`at_newanchor` 成功递增
  epoch 但不刷新传入 Cursor，调用方须重新 seek/locate。
- 空树归一：任何 mutation 出口若 `root.child_count == 0`，必须统一写
  `paths[0] = root.children; off = poff = 0; cur = atU_point(0, 1)`；
  删除路径同时保持 `T->seam == NULL` 断言。非空树不得残留 done 或
  `cur.id != 0` 指向已删除 edge。
- `at_next` 返回 anchor 时保证 `C->cur` 是当前 edge 内的 START，
  `C->off`/`C->poff` 以该 edge 为基准；body 用 `assert(e == atK_edge(C))`
  校验现场。若随后耗尽，`atK_nextedge` 直接把 Cursor 提交到 `T` 的末尾
  done 态（虚尾保持原位）。该语义由 `at_checkcursor` 覆盖。
- `epoch` 规则不变：`unsigned` 相等性失效计数，公共入口成功后统一递增并刷新
  Cursor 的 epoch；过期 Cursor 在解引用 paths 前被挡（`at_next` 返回
  `AT_NONE` 且 `*plen = 0`）。
- `atK_forwardoff` 的虚拟尾语义保留给 `at_advance`/`at_newanchor`；
  `atD_remove` 的 del 已截断，`atK_forwardoff` 最多走到
  `off + d == T->bytes`，其 end 分支 `atK_locend` 且虚拟 overshoot 为 0，
  因此不需要额外 clamp（与 `spD_remove` 的 `sp_advance + spK_locend` 结果一致）。
- 桶链只保存 id 与 is_end 标签，不保存槽指针；`atE_next(p, a)` 按
  `p.is_end ? a->enext : a->snext` 取后继。anchor 数组扩容搬移不影响 `cur`。
- fresh/done 由 `cur.id == 0` 加 `cur.is_end` 直接判定；
  `{0,0}`/`{0,1}` 只出现在 Cursor，不进入 edge 桶链（桶链空头恒为
  `{0,0}`）。

## 三、不变量

1. 空树 `root.child_count == 0`、没有 edge、`T->bytes == 0`。
2. 非空树首个叶子槽是 `edge@0`；除末位 EOF 零长 edge 外所有叶子槽 `bytes > 0`。
   末位槽 `bytes == 0` 仅表示 EOF 字节缝存在 point；其他叶子槽与内部节点
   聚合 `bytes` 均 > 0。
3. EOF 零长 edge 只在位置 `T` 有端点项时存在：`bytes == 0`，且必须是最后一个叶子槽。
4. `T->bytes` 等于所有叶子 `bytes` 之和；edge 位置严格递增。
5. `T > 0` 时首 edge 永久保留，即使它的两个桶都空。
6. 除 `edge@0` 与末位 EOF 零长 edge 外，每个树内 edge 至少有一个端点槽；
   edge 失去全部端点槽后必须删除并由 `atE_purge` 回收。EOF 零长 edge 在两个
   桶都空后删除。
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
15. `p` 为 Cursor 当前叶子节点：当前 edge 不是末 edge 时必须
    `C->poff < p->bytes[i]`；只有末 edge 才允许 `C->poff >= p->bytes[i]`
    （树中段尾禁令，详见 §2.5）。

## 四、edge 删除

删除一个树内 edge@`p`（调用方保证它没有端点槽；首 edge/EOF 零长 edge 规则见下）：

- 首 edge：`T == 0` 且无槽时删除，树回到无 edge 空树；`T > 0` 时首 edge 永不
  删除（即使无槽），它承载 0 边界与前导 span。
- 其他 edge：把它的 `bytes` 加到前一个 edge 的 `bytes` 上，然后从树中删除该
  槽；`T` 不变，它之后的 edge 位置保持不变（前一个 span 吸收被删字节）。
- EOF 零长 edge `bytes == 0`，并入前一个 edge 不改变任何 span；删除后若 `T > 0`，尾
  坐标由最后一条正长 span 的 `poff == bytes` 表达。
- 删除后同步被搬移槽的 `parent/idx`，并沿路径更新内层 `bytes`。

## 五、点物化（`atI_addpoint`）

树内 point 由 edge 承载；point 是 anchor 端点 occurrence（`at_Point`）。
`atI_addpoint(C, slot)` 在 Cursor 坐标上为 `slot.id` 的 anchor 添加
`slot.is_end` point：

1. root 为空或 `C->poff > 0` 时调用 `atI_insert(0, 0)`，把 span 长到
   cursor（空树建首 edge；虚拟尾补 gap）；
2. `C->poff > 0` 时直接调用 `atI_splitpoint(keep=C->poff)`，以返回的 `ne`
   作为 base edge；否则 base edge 为当前 edge；
3. 用 `atE_head` 按 `slot.is_end ? a->end_right : a->start_right` 选桶，把
   slot 链入并写回 anchor 的 `start/end` 指针。

后置条件：目标 edge 位于 `at_offset(C)`，Cursor 在该 edge 且 `poff == 0`；
point 已链入，anchor 端点指针已写。`atI_addpoint` 不返回值；调用方
`at_newanchor` 已按两次调用预留资源，post-reserve 无失败路径。

`atI_addpoint` 是第二个主入口：span 物化走 `atI_insert(0, 0)`，cursor 处的
edge 由 `atI_splitpoint` 直接提供。point 模式保持 RIGHT 桶不动：原 edge
的桶属于 `cursor` 之前的位置，新 edge 先取得 `b - keep` 的 span，随后由本次
`atI_addpoint` 立即获得端点槽。

### 5.1 edge 位置规则

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
  helper 位于 node operations 末尾（`atM_up` 之后），只做一次 nodes 预留与
  一次 edges 预留；reserve 成功后插入槽触发的 node split 与 edge 分配只用
  `atP_ralloc`/assert，不再返回 OOM，也不留下需要回滚的半成品。
  `atI_addpoint` 自身不 reserve；它由 `at_newanchor` 的一个事务预留覆盖。

## 六、对象操作

### 6.1 `at_newanchor(C, len, ns, gravity, *out)`

- `C` 已初始化，`start = at_offset(C)`；直接检查 `start + len < start`，溢出返回 `AT_ERROVERFLOW` 且树不变。
- `ns` 必须在 `0..AT_MAX_NS`（29 bit）内，否则返回 `AT_ERRPARAM`。
- `end = start + len`；允许 `end` 超过 `T`（虚拟区），此时会创建新的最后 edge。
- 从 freelist 分配 `at_Id`（可能扩容 `T->anchors`，失败返回 `AT_ERRMEM` 且树保持
  不变）；初始化 `ns` 与两个 side 位；`start`/`end` 指针由 `atI_addpoint` 写入。
- 用同一个传入 Cursor `C` 创建两个 point：
  - `atI_addpoint(C, atU_point(id, 0));`
  - `atK_forwardoff(C, len), atI_addpoint(C, atU_point(id, 1));`
  `atI_addpoint` 的 point 物化编排见 §五；`len == 0` 时第二次调用落在同一
  edge，不需要额外分支。
- `at_newanchor` 不做任何本地 Cursor 副本：它用同一个传入 `C` 完成两个
  point 的物化，成功返回时递增 `T->epoch` 但不刷新 `C->epoch`；结束时 C 的
  内部 paths 已被本次操作改写，epoch 仍为旧值，调用方必须重新
  `at_seek`/`at_locate`。这里不调用 `at_advance`、不建立第二个 Cursor。
- `at_newanchor` 在做任何树修改前预留两次 `atI_addpoint` 所需的 nodes/edges；
  预留失败直接返回 `AT_ERRMEM`，不产生任何新 edge，失败后树与操作前一致。
- `ns` 写入 `anchor->ns`；`start_right`/`end_right` 按 gravity 写入，anchortree
  不解释 ns 含义。
- 按 gravity 选择两个槽的桶：

| gravity  | START 槽桶       | END 槽桶       |
| -------- | ---------------- | -------------- |
| AT_GROW  | start edge LEFT  | end edge RIGHT |
| AT_FIXED | start edge RIGHT | end edge LEFT  |
| AT_SHIFT | start edge RIGHT | end edge RIGHT |
| AT_LOCK  | start edge LEFT  | end edge LEFT  |

- `atI_addpoint` 按 `slot.is_end` 选择 anchor 端点指针与 side 位：START 槽
  `{id, 0}` 链入 start edge 的 `start_right` 桶，END 槽 `{id, 1}` 链入 end edge
  的 `end_right` 桶；`at_gravity` 由两位反推调用方创建时给的 gravity。
- `len == 0` 时 start/end 是同一个 edge，仍然链入两个槽；两者可能落在同一个
  桶。
- 链入操作只改桶头与 anchor 的 `snext/enext`，不分配内存。
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
  同时在逐层上升时把各层当前槽之前的 span 相加，结果写入 `C->off` 并作为
  返回值；`at_range` 直接消费该返回值。首 edge 位置为 0，该过程不调用
  `at_locate`。
- start/end 分别取 anchor 的 `start/end` edge 位置。
- 交叉零长时原样返回 `*start > *end`。

### 6.4 `at_gravity(T, id)`

- 由 `anchor->start_right`/`end_right` 反推创建时的
  `AT_GROW..AT_LOCK`；无效 id 返回负值。
- 因两个 side 位已打包在 anchor 内，查询为 O(1)，不需要反查桶。

### 6.5 `at_anchor(C, pns, plen)`

- 只读查询：读取 `C->cur.id` 所对应 anchor 的信息；不推进桶游标、
  不移动 Cursor、不修改树。入口先做 `C`/`C->tree`/`epoch` 过期校验（与
  `at_next` 同规）；查询本身不递增 epoch。
- 成功时返回 `C->cur.id`，与 `at_next` 最后一次产出的 id 一致。
- `pns != NULL` 时写 `anchor->ns`；`pns == NULL` 时不写。
- `plen != NULL` 时用 `at_range` 取 `[start, end)` 长度：`start <= end` 写
  `end - start`（零长写 0），交叉区间（`start > end`）写 0；`plen == NULL`
  时不查 range。
- `C->cur.id == 0`（fresh 或 done）或 id 已释放/超出 `T->idmax` 时返回
  `AT_NONE`，且 `plen != NULL` 时写 0。
- `C == NULL`、`C->tree == NULL` 或 Cursor 过期（`epoch` 不匹配）返回
  `AT_NONE`，同样写 `*plen = 0`；只读查询不返回 `AT_ESTALE`。
- 成本：`cur.id` 直索引 anchor 为 O(1)；带 `plen` 时加一次 `at_range`
  （O(树高 × fanout)），无桶扫描、无内存分配。

### 6.6 `at_fromanchor(C, T, id)`

- 语义：按 `id` 构造 Cursor，落在该 anchor 的 START 槽上；现场与 `at_next`
  刚产出该 id 时一致（§2.5）：`cur = {id, 0}`、`off == at_range` 的 `start`、
  `poff == 0`、`epoch == T->epoch`。与 `at_anchor` 对偶：`at_fromanchor`
  由 id 取现场，`at_anchor` 由现场取 id 与元数据。
- 入口：`C`/`T` 非 NULL 且 `atA_fromid(T, id)` 命中 live anchor，否则返回
  `AT_ERRPARAM`；`AT_NONE`、已释放 id、`id > T->idmax`、空树同归此错误，
  失败时不改动 `C`。构造语义与 `at_seek` 一致：完全重建 Cursor 并捕获
  当前 `T->epoch`。
- 实现：`memset(C, 0, sizeof(at_Cursor))` 后调用 `atK_fromedge(C, T, a->start)`
  建立 paths/坐标/epoch，再写 `C->cur = atU_point(id, 0)`；零分配，
  复杂度 O(levels × fanout)（只读 `atN_sumbytes` 与建 paths），不修改树与 epoch。
- 派生契约：
  - `at_anchor(C, pns, plen)` 立即返回该 id 的 ns 与长度；
  - `at_next(C, ...)` 从该槽后继继续，不重复返回该 id；
  - 现场即 `at_next` 的规范产出态，§7.1/§7.2 的续扫槽与迁桶规则、epoch
    失效规则原样适用。
- 零长 / 交叉零长：`start == end` 时落在同一条 edge；位于 `T` 的 EOF 零长
  edge 上时 `poff == bytes == 0`，属 §2.5 的合法尾态；`start > end`（交叉
  零长）时位置取 `start`，与 `at_range` 的 `start` 一致。

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

插入发生在 Cursor 当前坐标 `p`，长度为 `ins > 0`。插入阶段只有一个内部主入口
`atI_insert(C, ins, advance)`：它完成事务检查、tree mutation 选择与 Cursor
提交。insert 与 append 的 tree mutation 完全相同，区别只在 Cursor 终态，由
`advance` 表达：

- `advance == 0`：Cursor 停在 `p`（insert semantics）；
- `advance == ins`：Cursor 到 `p + ins`（append semantics）。

`atI_addpoint` 是第二个主入口，负责 point 物化；它直接调用
`atI_splitpoint`，并复用 `atI_insert(0, 0)` 把 span 长到 cursor。tree mutation 由
“当前位置 + LEFT/RIGHT 桶状态 + `p` 是否为 0”直接决定；`advance` 只决定
Cursor/cur 提交，不参与 split/growleft 的选择。

#### 术语表

| 术语 | 含义 |
| --- | --- |
| `e` / `b` / `q` | 操作前的当前 edge、它的 `bytes`、它的起点坐标 `Cursor.off` |
| `p` | 插入坐标 `q + Cursor.poff` |
| `L空` / `L非空` | `e` 的 LEFT 桶是否为空 |
| `R空` / `R非空` | `e` 的 RIGHT 桶是否为空 |
| `ep` / `bp` | `p > 0` 时的前驱 edge、它操作前的 `bytes` |
| `advance` | 插入后 Cursor 需要前进的字节数：`0` 或 `ins` |
| `target` | 终态坐标 `p + advance`；insert 为 `p`，append 为 `p + ins`；实现由 `C->poff += advance` 或 `pos + advance` 直接得到 |
| `keep` | `atI_split` 后旧 edge 保留的 span 长度；insertion 模式等于 `ins`；`atI_splitpoint` 的 `keep` 等于 `Cursor.poff` |
| `growspan` | 不新建 edge 的 span 增长；空树建首 edge 由 `atI_makefirst` 实现，其余内联在 `atI_insert` 中 |
| `growleft` | 把 `ins` 长到前驱 `ep`，当前 `e` 整体右移；由 `atI_growleft` 实现，叶内/跨叶共用一趟下降，advance 在循环外选择两个提交变体 |
| `split` | insertion 模式在指定位置新建 edge，并把 RIGHT 桶迁到新 edge；由 `atI_split` 实现 |
| `splitpoint` | point 模式在 `Cursor.poff` 处新建 edge，RIGHT 桶不动；由 `atI_splitpoint` 实现 |
| insertion 模式 | `atI_split` 的 `Cursor.poff == 0` 形态，服务 `atI_insert` |
| point 模式 | `atI_splitpoint` 的 `Cursor.poff > 0` 形态，服务 `atI_addpoint` |
| fresh / done | Cursor 的 `{0,0}` / `{0,1}` 续扫槽状态 |
| keep（cur） | 保留 `cur` 原值，不改成 fresh |

#### 行为表

| Rx | 条件 | tree mutation | `advance = 0` 终态 | `advance = ins` 终态 |
| --- | --- | --- | --- | --- |
| R0 | 空树，`p == 0` | 建首 `edge@0`，`bytes = ins`，`T = ins` | 首 edge；`off=0`；`poff=0`；cur keep | 首 edge；`off=0`；`poff=ins`；cur keep |
| R1 | 空树，`p > 0` | 建首 `edge@0`，`bytes = p + ins`，`T = p + ins` | 首 edge；`off=0`；`poff=p`；cur keep | 首 edge；`off=0`；`poff=p+ins`；cur keep |
| R2 | 非空树，`poff > 0`；R/L 四种组合同 | 当前叶槽 bytes 增加 `ins`；`poff > b` 时再加 `poff-b` gap；`T` 同步 | `e`；`off=q`；`poff` 不变；cur keep | `e`；`off=q`；`poff+ins`；cur keep |
| R3 | 非空树，`poff==0`、R空、L空、`p==0` | 首 edge bytes 增加 `ins`；`T` 增加 `ins` | 首 edge；`off=0`；`poff=0`；cur keep | 首 edge；`off=0`；`poff=ins`；cur keep |
| R4 | 非空树，`poff==0`、R空、L空、`p>0` | 不变量排除；空非首 edge 已由 `atE_purge` 删除 | 非法格，不制定操作 | 非法格，不制定操作 |
| R5 | 非空树，`poff==0`、R空、L非空、`p==0` | 首 edge bytes 增加 `ins`；`T` 增加 `ins` | 首 edge；`off=0`；`poff=0`；cur keep | 首 edge；`off=0`；`poff=ins`；cur keep |
| R6 | 非空树，`poff==0`、R空、L非空、`p>0` | `e` bytes 增加 `ins`；`T` 增加 `ins` | `e`；`off=p`；`poff=0`；cur keep | `e`；`off=p`；`poff=ins`；cur keep |
| R7 | 非空树，`poff==0`、R非空、L非空、`p==0` | split：旧 edge bytes 改为 `ins`；在 `ins` 建 `ne`，bytes 为旧 `b`；RIGHT 桶迁 `ne`；`T` 增加 `ins` | 首 edge；`off=0`；`poff=0`；R桶 cur→FR；L桶 cur→keep；id0→keep | `ne`；`off=ins`；`poff=0`；R桶 cur→keep；L桶 cur→FR；id0→FR |
| R8 | 非空树，`poff==0`、R非空、L非空、`p>0` | 同 R7，起点为 `p`，`ne` 在 `p+ins` | `e`；`off=p`；`poff=0`；R桶 cur→FR；L桶 cur→keep；id0→keep | `ne`；`off=p+ins`；`poff=0`；R桶 cur→keep；L桶 cur→FR；id0→FR |
| R9 | 非空树，`poff==0`、R非空、L空、`p==0` | split（首 edge 例外）：首 edge bytes 改为 `ins`；在 `ins` 建 `ne`，bytes 为旧 `b`，RIGHT 桶迁入；`T` 增加 `ins` | 首 edge；`off=0`；`poff=0`；R桶 cur→FR；id0→keep | `ne`；`off=ins`；`poff=0`；R桶 cur→keep；id0→FR |
| R10 | 非空树，`poff==0`、R非空、L空、`p>0` | growleft：前驱 `ep` bytes 增加 `ins`；`e` 起点变为 `p+ins`；RIGHT 桶仍属 `e`；`T` 增加 `ins` | `ep` 最右叶槽；`off=p-bp`；`poff=bp`；cur→FR | `e`；`off=p+ins`；`poff=0`；端点为 `e` 的 cur→keep；id0→keep |

#### 分派表

| 公共操作 | 内部调用 |
| --- | --- |
| `at_insert` | `atI_insert(ins, advance=0)` |
| `at_append` | `atI_insert(ins, advance=ins)` |
| `at_splice` 插入阶段 | `atI_insert(ins, advance=ins)` |
| `at_newanchor` 的 point 物化 | `atI_addpoint` 直接调 `atI_splitpoint` |

Rx 在 `atI_insert` 内部的分派：

| Rx | `atI_insert` 内部操作 |
| --- | --- |
| R0、R1 | `atI_makefirst(ins, advance)` 建首 edge |
| R2、R3、R5、R6 | 当前 span 增长；`advance` 决定 `poff` |
| R7、R8、R9 | `atI_split(keep=ins, advance)` |
| R10 | `atI_growleft(ins, advance)` |

直接判据：

- `poff > 0`：growspan。
- `poff == 0` 且 R 空：growspan。
- `poff == 0` 且 R 非空、L 非空：split。
- `poff == 0` 且 R 非空、L 空、`p == 0`：split（首 edge 例外）。
- `poff == 0` 且 R 非空、L 空、`p > 0`：growleft。

`advance` 不参与以上判据；split/growleft 的选择只看 tree 状态。R10 由
`atI_growleft` 沿前驱子树最右路径完成叶内/跨叶统一的 bytes 增长；`advance`
在循环外选择两个变体：`advance == 0` 的变体在每层 mutation 时同步写
`C->paths`，`advance == ins` 的变体只写 bytes；两者的 Cursor/cur 提交见下。

#### helper 行为描述

- `atI_insert(ins, advance)`（插入主入口）：
  - 前置：`ins >= 0`，`advance == 0 || advance == ins`；`ins > 0` 时由
    public 插入入口调用，`ins == 0` 时只由 `atI_addpoint` 在
    `at_newanchor` 事务内调用。
  - 事务：`ins > 0` 时先做 `limit + ins < limit` overflow 检查，再
    `atU_reserve(C, 1, 1, 2)`；`ins == 0` 时资源由调用方事务覆盖，主入口不
    再 reserve。
  - 空树：转调 `atI_makefirst(ins, advance)`，cur keep。
  - 非空且 `poff > 0` 或 R 空：当前叶槽 bytes 增加 `ins + gap`
    （`gap = poff - b`，仅 `poff > b` 时存在）；`C->poff += advance`，
    cur keep。
  - 非空、`poff == 0`、R 非空、L 空、`p > 0`：转调
    `atI_growleft(ins, advance)`。
  - 其余：转调 `atI_split(ins, advance)`。
- `atI_makefirst(ins, advance)`（空树具体 case）：
  - 前置：root 为空；资源已由 `atI_insert` 的事务覆盖。
  - tree 后置：创建首 `edge@0`，`bytes = pos + ins`，`T = pos + ins`。
  - Cursor 后置：`paths[0]` 指向该槽，`off = 0`；`poff = pos + advance`；
    cur keep。
- `atI_growleft(ins, advance)`：
  - 前置：R10；前驱 `ep` 必存在（由调用方保证）。
  - tree 后置：叶内前驱直接命中时更新前驱叶槽；跨叶时升到首个有前驱兄弟的
    `base`，从前驱子树最右路径下降，逐个覆盖 `base..l` 的前驱槽 bytes；最后
    叶槽 `bytes += ins`；`e` bytes 不变、起点变为 `p+ins`；RIGHT 桶仍属 `e`；
    `T` 增加 `ins`；不产生空 edge。
  - Cursor 后置：叶内/跨叶共用一趟下降，循环内不做 `advance` 判断，按
    `advance` 在循环外选择两个变体：
    `advance == 0` 的变体在每层 mutation 时同步写
    `C->paths[d] = &p->children[i]`，下降结束后补 `C->paths[l]`，落在 `ep`
    最右叶槽，`off=p-bp`、`poff=bp`、cur 无条件 fresh；
    `advance == ins` 的变体只写 bytes，C paths 保持指向 `e`，`off=p+ins`、
    `poff=0`，端点为 `e` 的 cur keep，id0 且旧当前 edge 为 `e` 时 keep。
    函数返回 1/0 仅作 C89 提交语义，调用方写
    `return atI_growleft(...), AT_OK;`。
- `atI_split(keep, advance)`（insertion split）：
  - 前置：`Cursor.poff == 0`、R 非空、L 非空或 `p == 0`、`keep > 0`、
    `advance == 0 || advance == ins`；用于 R7/R8/R9。
  - tree 后置：旧 edge bytes 改为 `keep`；在起点 `q+keep` 建 `ne`，bytes 为
    旧 `b`；RIGHT 桶整体迁到 `ne` 并同步 anchor 端点指针；`T` 增加 `keep`；
    旧 `b == 0` 时 `ne` 是末位 EOF 零长 edge。
  - Cursor/cur：`advance == 0` 时落在旧 edge，`off=p`、`poff=0`；
    R桶 cur→FR，L桶 cur 与 id0 keep。`advance == ins` 时落在 `ne`，
    `off=p+keep`、`poff=0`；R桶 cur keep，L桶 cur 与 id0→FR。
- `atI_splitpoint(keep)`（point split）：
  - 前置：`Cursor.poff > 0`、`keep == Cursor.poff`、
    `keep <= 当前 edge bytes`；虚拟 gap 已由 `atI_insert(0, 0)` 吸收。
  - tree 后置：旧 edge bytes 改为 `keep`；在起点 `q+keep` 建 `ne`，bytes 为
    `b-keep`；RIGHT 桶不动；`T` 不变；`keep == b` 时 `ne` 是末位零长 edge。
    `ne` 返回给 `atI_addpoint` 立即链入 occurrence。
  - Cursor/cur：cursor 落在 `ne`，`off=q+keep`、`poff=0`；cur 不改，由
    `atI_addpoint` 与 `at_newanchor` 的 epoch 失效契约收口。
- `atI_addpoint(slot)`（第二个主入口）：
  - root 空或 `Cursor.poff > 0` 时先调 `atI_insert(0, 0)`，把 span 长到
    cursor（空树建首 edge；虚拟尾补 gap）。
  - `Cursor.poff > 0` 时直接调 `atI_splitpoint(keep=Cursor.poff)`，以返回的
    `ne` 作为 base edge；否则 base edge 为当前 edge。
  - 最后按 anchor 的 side 位选桶并链入 occurrence；不 reserve、不刷新
    epoch、不改 cur。

#### 资源与 epoch

- mutation 前的资源预留统一走 `atU_reserve(C, edges, pass, extra)`：
  `at_remove` 由 `atD_remove` 自保护 `(0, 1, 0)`；`at_insert`/`at_append`
  由 `atI_insert` 自保护 `(1, 1, 2)`；`at_splice` 传
  `(1, rm + extra, 2*extra)`，其中 `rm = del > 0 && pos < T->bytes`、
  `extra = ins > 0`；`at_newanchor` 传 `(3, 2, 5)`，覆盖 `atI_addpoint` 的
  `atI_insert(0, 0)` 与 point split。`pass`/`extra` 由调用者精确计算，
  helper 不解释事务来源。
- overflow 检查 `limit + ins < limit`（`limit = max(pos, T->bytes)`）在
  mutation 前完成；overflow 与 reserve 失败返回 `AT_ERROVERFLOW`/`AT_ERRMEM`，
  树、Cursor、epoch 均不变。
- 预留成功后 `atD_*`/`atI_*` 只用 `atP_ralloc`/assert，不产生需要回滚的
  半成品。
- `atI_insert` 不碰 epoch；公共入口在 mutation 成功后统一
  `T->epoch += 1, C->epoch = T->epoch`。`at_splice` 的插入阶段调用
  `atI_insert(ins, ins)`；`atI_addpoint` 依靠 `at_newanchor` 的旧 epoch 失效
  契约收口。


### 7.2 删除（stitch + seam）

删除 `[L, R)`（`del = R - L`），`atD_remove` 是 `static int`，使用本地 `R = *C`：

- `pos = at_offset(C)`；`pos >= T->bytes` 直接 `AT_OK`；`del` 按 `T->bytes - pos`
  截断；`R` 经 `atK_forwardoff(&R, del)` 到右边界。因为 del 已截断，
  `off + d <= T->bytes`；等于时 `atK_forwardoff` 内部 `atK_locend` 且虚拟
  overshoot 为 0，行为与 `spD_remove` 的 `sp_advance + spK_locend` 一致，
  **不需要额外 clamp**。
- 自保护 reserve：mutation 前 `atU_reserve(C, 0, 1, 0)`；失败返回 `AT_ERRMEM`，
  树/epoch/Cursor 不变。`atD_makechain` 入口断言
  `C->tree->S->nodes.freed_obj >= to - from`。
- `atD_stitch` 入口断言 `freed_obj >= levels`：anchortree 的 `atD_makechain`
  没有 spD_makechain 的 `from < 0` 新建 root 分支，stitch 内实际 node 分配
  总数 ≤ levels。
- `fl = atD_diverlevel(C, &R)`。`fl > levels` 表示同 edge 删除：
  `atD_rmleaf(C, del)` 只处理当前槽；桶链不变，Cursor 不清现场、不 rewind，
  `cur` 继续有效。部分删除只改 bytes；整条 edge 删除时按现有
  首 edge/EOF 零长规则处理，并且只在 `i + 1 == cc` 时发生。
- 跨 edge 时 `T->seam = atK_edge(&R)`，断言 seam 不是左边界 edge；
  `atD_rmrange(C, &R, fl)` 用 `rt[]` 暂存 R 侧、trim 左右边界、cutrange purge
  L 侧。`atN_purge` 通过 `atE_mvlink` 把被回收叶子 edge 的桶搬到 seam。
- `atE_mvlink` 采用“块倒序、块内正序”的整块前插：单次遍历源链更新
  `anchor->start/end` 并找尾，尾接 dst 旧头，dst 头设为源链头；单块 O(源链长)，
  全部被搬槽只搬一次，总 O(N)。
- 释放顺序保持 cutrange/stitch 原序（L右 → R左 → 中间；
  `atD_stitchnode`/fold/rebalance 删除的节点已先清空，不再触发搬链）。
  `atD_stitch` 对 L 零长末槽的清理是最后一个搬链点；块前插使 L 的左右链位于
  seam 每个桶最前，这是 rmrange 续扫不 skip 的前提。块间顺序除此之外不保证。
- L 零长末槽清理写成 `atN_purge(...) + atN_setcc(p, cc - 1)`：L 已是最后一个
  槽，只需把链搬进 seam。
- rmrange 不 rewind。`cur` 默认为 `{0,0}`；只有 `L->poff == 0` 且旧
  `cur.id != 0` 时才可能沿用旧现场：
  - `L->poff > 0`：L edge 存活，其桶原地不动；删除后 Cursor 落 seam，
    `cur = {0,0}`，从 seam left 桶头重扫。seam 可能刚被前插了 left 节点，
    因此不能保留旧 `cur` 或取 L.left 尾。
  - `L->poff == 0`：L 零长末槽在 stitch 开始时被 purge，其 left/right 桶作为
    最后一块整块前插到 seam 各桶最前，此时旧现场才可用：
    - 旧 `cur` 在 L 左桶：保留旧 `cur`，从其后继继续；
    - 旧 `cur` 在 L 右桶：取 L 左链尾 `left_end`（左链为空则 `{0,0}`），
      扫完 L.left 尾、L.right 头及更早搬入的块；
    - 旧 `cur.id == 0`（fresh/done）：保持 `{0,0}`，从 seam 头重扫。
- 以上策略保证不 skip；允许重复当前 id 附近。`left_end` 查找在 mutation 前
  完成，O(左链长)。
- 删除后 `T->seam` 清回 `NULL`；`atD_remove` 返回 `AT_OK`。`at_remove` 只在真正
  发生 mutation 时递增 epoch 并刷新 C；`atD_remove` 内部不碰 epoch。
- 空树归一：`atD_rmleaf`/`atD_rmrange`/`atD_remove` 任一出口若
  `root.child_count == 0`，统一写 `paths[0] = root.children; off = poff = 0;
  cur = atU_point(0, 1)`，并保持 `T->seam == NULL` 断言。
- rmleaf/rmrange 返回时的现场不变量：`at_offset(C) == pos`；`atK_edge(C)` 必须是
  有效叶子槽；`cur.id != 0` 时其 anchor 端点 edge 必须等于 `atK_edge(C)`。
  rmrange 成功返回时 `poff == 0`（`atD_stitch` 的 merge-leaf 步骤已把 poff 合进
  off）；
  rmleaf 保留原 `poff`。不再存在“允许 `cur.id` 与 `atK_edge(C)` 不同、
  由 `at_next` 恢复”的例外；`at_splice`/`at_remove` 直接接在 rmrange 后使用时
  依赖该不变量。
- `atD_stitch` 末尾结构断言 `assert(cc != 0 && i < cc)` 及
  `atD_stitch`/`atD_rmleaf` 的现场维护规则保持；不恢复 spD_stitch inline
  tail 修正。

### 7.3 `at_splice(C, del, ins)` 与公共 API

- `at_splice` 语义为“删除 `[C, C+del)` 后再在 C 处插入 `ins`”，Cursor 结束于
  `offset + ins`。
- 入口顺序固定：
  1. 参数/过期检查；
  2. no-op 早退：`del == 0 && ins == 0`、`ins == 0 && pos >= T->bytes`
     直接 `AT_OK`，不 reserve、不 bump epoch、不改 C；
  3. overflow 检查：`limit = max(pos, T->bytes)`，`limit + ins < limit` 返回
     `AT_ERROVERFLOW`；此检查必须在 reserve/remove 之前，避免 `del > 0` 时
     先删除再报 overflow；
  4. combined reserve（一次 `atU_reserve`）；
  5. `if (del > 0) atD_remove(...)`；`if (ins > 0) atI_insert(C, ins, ins)`；
     `del == 0` 不得调用 `atD_remove`，`ins == 0` 不得调用 `atI_insert`；
  6. 只有真实 mutation 成功后才 bump epoch/刷新 C。
- reserve 调用值：
  - `at_remove`：由 `atD_remove` 自保护 `(0, 1, 0)`；`atD_stitch` 入口断言同步改窄。
  - `at_insert`/`at_append`：由 `atI_insert` 自保护 `(1, 1, 2)`；一次
    `atI_makeroom` 最坏 `L+2`。
  - `at_splice`：一次 combined `(1, rm + extra, 2*extra)`，其中
    `rm = del > 0 && pos < T->bytes`、`extra = ins > 0`；`rm+extra >= 1`。
    纯删除时 `L+2` 覆盖 stitch 断言；纯插入时 `L+2` 覆盖 append；删除+插入时
    `2L+2` 覆盖 remove ≤L 与 append ≤L+2 的总和。
  - `at_newanchor`：`(3, 2, 5)`；两次 `atI_addpoint`，edges 最大 3（空树
    `atI_insert(0,0)` + point split 最多 2 条 + 后续 point 的 split 1 条），
    nodes 最坏 `2(L+2) = 2L+4`，`2L+5` 余 1。
- `atP_reserve` 是“至少 n 个空闲对象”的下界保证，不是累加额度；`atU_reserve`
  对 nodes/edges 各只调用一次。
- remove 的 L 依据：`atD_stitchnode` 延迟分段修复 `kl -> l` 各段不重叠，
  `atD_makechain` 只在第一次建链时分配；最坏左树满时链高为 L。insert 的 L+2
  依据：`atI_makeroom` 最坏根分裂新增 2、之后每层 `atI_splitchild` 新增 1；
  组合覆盖证明：remove 实际分配 ≤ L，insert 实际分配 ≤ levels_after + 2 ≤ L+2，
  `(rm+extra)L + 2*extra` 恰好覆盖；`atD_stitch` 入口断言同步改窄为 `>= levels`。
- `atI_insert` 返回 `AT_OK`/`AT_ERROVERFLOW`/`AT_ERRMEM`；overflow 与 reserve
  失败在 mutation 前。`at_splice` 的 remove/insert 内部自保护 reserve 在
  combined 覆盖下是 no-op 检查，再失败视为 internal invariant violation
  （实现用 assert 或 design 定义的传播），禁止 remove 已成功而 append 失败
  留下半成品。
- 删除阶段按 §7.2；插入阶段按 §7.1。`atI_insert`/`atD_remove` 等内部 helper
  不检查、不递增 epoch；公共入口统一 `T->epoch += 1, C->epoch = T->epoch`，
  `at_splice` 只 bump 一次。
- 删除与插入都不调用 `at_seek`/`at_locate` 事后归位：删除由 stitch/rmleaf
  自维护并按 §7.2 设置 cur；插入由 `atI_insert` 按 §7.1 的 `advance` 规则维护
  paths/off/poff/cur。

## 八、对象级迭代

- 迭代按 edge 坐标序进行；对每个 edge 先扫 left 桶再扫 right 桶，只产出
  `is_end == 0` 的 START 槽；END 槽只用于推进链表。
- 正长 object 的 END 在其 end edge 被跳过；零长 object 的 START 产出一次，
  END 跳过。
- 同一 edge 内多个对象不排序；稳定序由上层按 `(start, id)` 自行稳定。
- `at_next` 从 Cursor 当前位置开始，按 §2.5 的两层结构：
  - 空树或 done（`cur.id == 0 && cur.is_end`）直接耗尽；
  - `at_next` 循环条件 `while (C->cur.id || atK_nextedge(C).id)`：
    当前 edge 尚有 slot 时，body 先判断 candidate；不匹配则用
    `atE_next` + `e->right` 推进桶链；当前 edge 耗尽时由条件的
    `atK_nextedge` 转入下一 edge；
  - `atK_nextedge`：有后继时 `off += bytes[current]`、`poff = 0`，把
    `C->cur` 置为下一 edge 的 `atE_first` 并返回它；当前 leaf 的
    `poff >= bytes` 或 `l < 0` 无后继时直接把 `C->cur` 置 done；
  - `plen` 由 `at_next` 命中后经 `at_anchor`/`at_range` 填充，交叉/零长写 0。
- 耗尽用 done（`cur == {0,1}`）表达，不写额外 flag：正常耗尽后
  Cursor 已在 `T`（虚尾时保持原位）；之后重复调用幂等返回 `AT_NONE`；
  `at_locate`/`at_advance` 重新播种桶游标使迭代恢复。
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
- `at_anchor` 在同一 Cursor 上查询 `cur.id` 对应 anchor 的 ns 与长度，供调用方
  对 `at_next` 刚返回的 id 取元数据；它只读，不推进扫描现场，失败路径与
  `at_next` 的参数/过期检查同规（返回 `AT_NONE` 且写 `*plen = 0`）。

## 九、API 草案

公共 API 声明见 `anchortree.h` 公开段，分组如下（语义见对应章节）：

- 生命周期：`at_open` / `at_close` / `at_newtree` / `at_deltree` / `at_bytes` /
  `at_reset`；`at_bytes(T)` 返回缓存的 `T->bytes`，`T == NULL` 返回 0。
- Cursor：`at_seek` / `at_fromanchor`（构造函数）/ `at_locate` / `at_advance`
  （导航）与 `at_anchor` / `at_next`（只读迭代），语义见 §2.5、§6.5、§6.6、§八。
- anchor：`at_newanchor` / `at_delanchor` / `at_range` / `at_gravity`，语义见
  §6.1–§6.4。
- 文本编辑：`at_remove` / `at_insert` / `at_append` / `at_splice`，语义见 §七。
- 常量：`AT_OK`/`AT_ERRPARAM`/`AT_ERRMEM`/`AT_ESTALE`/`AT_ERROVERFLOW`、
  `AT_NONE == 0`（无 id 与 Cursor fresh/done 共用，由 `is_end` 区分）、
  `AT_MAX_NS`、`at_Gravity`。
- epoch 不对外暴露：`at_seek`/`at_locate` 只捕获/刷新；`at_newanchor`/
  `at_delanchor`/`at_reset` 与真实 mutation 递增；过期 Cursor 在解引用
  `paths` 前被挡（编辑入口返回 `AT_ESTALE`，`at_next` 返回 `AT_NONE` 且
  `*plen = 0`）。

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

- 所有叶子槽搬移（copy/move/remove/makespace/split/merge）后同步被搬移 edge 的
  `parent`/`idx`；度量沿 `paths` 由 `atM_up` 累加，`T->bytes` 为叶子 `bytes` 之和。
- 内部 helper 以 Cursor 为工作结构：禁止 `at_locate`/`at_seek` 重新建立位置，
  禁止用 `atK_locend`/`atK_backwardoff`/`at_advance` 丢弃现场；现场由算法自己
  维护（`atD_remove` 的本地 `R` 用 `atK_forwardoff` 从 `*C` 派生）。`at_newanchor`
  通过 `atI_addpoint` 在传入 Cursor 上物化 point，不属于内部 helper 强制定位。
- 内部前置条件用 `assert` 表达，不写防御分支：`atI_splitchild` 两侧子节点均不少
  于 `AT_FANOUT / 2`；`atI_insert` 断言 `advance == 0 || advance == ins` 与
  四路状态的前置；`atI_growleft` 断言 R10、`C->poff == 0`、`at_offset(C) > 0`、
  前驱下标 `i > 0` 与前驱叶槽 `bytes > 0`；`atI_split` 断言 `C->poff == 0`、
  RIGHT 桶非空、`keep > 0`、`e->left.id != 0 || p == 0`；
  `atI_splitpoint` 断言 `0 < C->poff`、`keep == C->poff <= 当前 edge bytes`。
- reserve 单点、`pass`/`extra` 由调用者精确计算、helper 不解释事务来源；reserve
  成功后的分配只用 `atP_ralloc`/assert，不产生需回滚的半成品（详见 §12.1）。
- 插入：`atI_insert` 空树转 `atI_makefirst`，内联当前 span 增长，R10 转
  `atI_growleft(ins, advance)`，其余转 `atI_split(keep=ins, advance)`；
  `atI_addpoint` 是第二个主入口，复用 `atI_insert(0, 0)` 物化 span，并直接
  调用 `atI_splitpoint`；插入不调用 `atE_purge`。
- 插入 helper 的实现顺序约束：
  - `atI_insert` 先做 `ins > 0` 时的 overflow/reserve；再判空树并转
    `atI_makefirst`；再判 `poff > 0 || R 空` 的当前 span 增长；再判 R10 并转
    `atI_growleft`；其余转 `atI_split`。`ins == 0` 只由 `atI_addpoint` 在
    `at_newanchor` 事务内使用。
  - `atI_growleft(ins, advance)` 叶内/跨叶共用一趟前驱子树最右路径的 bytes
    增长：循环内不做 `advance` 判断，按 `advance` 在循环外选择变体；
    `advance == 0` 的变体在 mutation 时同步写 `C->paths`，`advance == ins`
    的变体只写 bytes。两变体都不使用额外路径数组、不产生第二趟下降；叶槽提交
    后返回 1/0 仅作 C89 提交语义。
  - `atI_split` 先调用 `atI_makeroom`，再按 insertion 模式计算 `p/i/l/b`；
    point 模式由 `atI_addpoint` 在 `atI_insert(0, 0)` 之后直接调用
    `atI_splitpoint`。
- `atE_purge(T, e)` 只服务 `at_delanchor`：入口经 `atK_fromedge` 建立本地现场，
  删除无槽 edge 的槽位，把它的 `bytes` 并入前驱槽；叶内前驱直接命中，跨叶时
  沿原路径上升/下降就地调整内层 `bytes`（删除侧减 `b`、前驱侧加 `b`，公共
  祖先不变），必要时 `atD_rebalance`；返回 1 仅作 C89 提交语义，不再维护
  Cursor 契约。`atN_purge` + `T->seam` 服务删除路径。
- `at_next` 两层：入口校验 → 空树/done 耗尽 → `while (C->cur.id ||
  atK_nextedge(C).id)`；body 扫当前 edge 的 left→right 桶链，
  `atE_next` 是单链 helper；`atK_nextedge` 负责提交 Cursor 到下一 edge 或
  done。
- 桶迁移 `atE_mvlink`：块倒序、块内正序整块前插，单块 O(源链长)。
- `atK_forwardoff` 保留虚拟尾语义；`at_advance` 正向用 `off + d < off` 检测回绕；
  `atK_locend` 带空树保护。
- `at_delanchor` 全程零分配；`atA_fromid` 是三个公共入口共享的 live 校验。
- epoch：内部 helper 不检查、不递增；公共入口在 mutation 成功后
  `T->epoch += 1, C->epoch = T->epoch`（`at_splice` 只 bump 一次）；
  `at_newanchor` 链接完成后递增但不刷新 C；`at_delanchor` 摘槽前递增。
- debug checker 与共享模型在 `tests/at_tests.h`；fanout 无关的共享场景全部在
  `tests/anchortree_test_fanout4.c`，奇数/默认 fanout 专属用例分别在
  `tests/anchortree_test_fanout9.c` / `tests/anchortree_test_fanout32.c`；
  单测/fuzz 只走公共 API（结构断言经只读 checker）。

### 块顺序与 sp 映射

主体顺序沿用 spantree；块边界只用于组织职责，不作为硬约束：

1. metric/path helpers 与 `/* cursor navigation */` 置于前面；
2. `/* remove */`（含末尾 `atE_purge`）；
3. `/* insertion */`（`splitroot`/`splitchild`/`makeroom`/`makefirst`/
   `growleft`/`split`/`splitpoint`/`insert`/`addpoint`）；
4. `/* APIs */`。

无依赖的双行短函数按 `brief_writing.md` §3 聚入相邻的双行函数区，不单独开块
（如 `atK_rewind`）；只有一个函数的独立块不单开：metric/path helper（如
`atM_up`）可直接并入 node operations，共享 helper（如 `atU_reserve`）放在首个
使用块前。

| anchortree                                                 | sp 对应                                    | 差异边界                                                                                                                                                      |
| ---------------------------------------------------------- | ------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `atP_*`/`atS_*`/`atN_*`/`atM_up`                           | `spP_*`/`spS_*`/`spN_*`/`spM_up`           | 基本同构                                                                                                                                                      |
| `atA_*`（`slot`/`alloc`/`free`/`fromid`/`clear`/`side`）   | 无                                         | 稠密 id 池与 freelist 属 anchortree 领域                                                                                                                      |
| `atE_*`（`head`/`unlink`/`mvlink`/`next`/`first`/`purge`） | `spA_born`/`died`/`merge` + `spL_*` 读路径 | left/right 桶链表与空 edge 回收属领域                                                                                                                         |
| `atK_*` + `at_next`                                        | `spK_*` + `sp_next`/`spN_findslot`         | `atK_forwardoff` 支持虚拟尾；`atK_fromedge` 是 edge 现场 helper                                                                                              |
| `atD_*`                                                    | `spD_*`                                    | `makechain` 无 new-root、`findroom` 禁右兄弟、merge-leaf 步骤内联于 `stitch`（无 id/mask 合并）、`rmleaf` 整条 edge 删除必在末槽、`stitch` 以末尾结构断言收口 |
| `atI_*`                                                    | `spI_*` + `sp_append`/`sp_insert`          | `splitroot`/`splitchild`/`makeroom` 同形；`insert` 合并 sp_append/sp_insert 的事务检查与 spI_grow 的当前 span 增长，空树转 `makefirst`，`advance` 提交两种 Cursor 终态；`growleft` 与 spI_growleft 同形，叶内/跨叶共用一趟下降，advance 在循环外选择两个提交变体；`split` 只做 insertion split，point split 由 `splitpoint` 服务 `atI_addpoint`；无 span id/mask |

## 十二、与其他树库的差异

> 基准：K/D/I 主干以 `spantree.h` 为参考，`piecetab.h`/`linecache.h` 只做形态
> 对照；未登记的差异视为实现偏差。

### 12.1 reserve 契约

所有 reserve 经 `atU_reserve(C, edges, pass, extra)`，只做
`atP_reserve(nodes, pass*L + extra)` 与 `atP_reserve(edges, edges)`；`pass`/`extra`
由调用者精确计算，helper 不解释事务来源，失败不改变树/Cursor/epoch：

- `at_remove`（经 `atD_remove`）`(0,1,0)`：remove 实际分配 ≤ L。
- `at_insert`/`at_append`（经 `atI_insert`）`(1,1,2)`：append 最坏
  `L+2`（根分裂 +2、之后每层 +1）。
- `at_splice` 一次 combined `(1, rm+extra, 2*extra)`，`rm = del > 0 && pos < T->bytes`、
  `extra = ins > 0`，覆盖 remove ≤ L 与 append ≤ L+2 之和；禁止拆成两次调用。
- `at_newanchor` `(3,2,5)`。
- no-op 不 reserve；`at_splice` 的 overflow 检查先于 remove。

### 12.2 返回值契约

内部函数只在“调用方消费该值”或“C89 提交语义（`return helper(...), 0/1;`）”时
返回非 void，否则改 `void`。有消费者：`atI_insert`（`AT_OK`/
`AT_ERROVERFLOW`/`AT_ERRMEM`）、`atD_remove`（`AT_OK`/`AT_ERRMEM`）、
`atK_forwardoff`/`atK_backwardoff`/`atK_locend`/`atK_nextedge`
（返回下一 edge 首 occurrence 并同步写 `C->cur`）、`atE_empty`、`atK_fromedge`
（返回 edge 起点，被 `at_range`/`atE_purge` 消费）、`atI_split`（返回新
edge，由 `atI_insert` 消费）、`atI_splitpoint`（返回新 edge，由
`atI_addpoint` 消费）；`atI_growleft` 返回 0/1 仅作 C89 提交语义，调用方写
`return atI_growleft(...), AT_OK;`；`atI_makefirst`/
`atI_makeroom`/`atI_splitroot`/`atI_splitchild` 无外部消费者，返回 `void`；
`atE_purge` 返回 0/1 仅作 C89 提交语义、调用方丢弃，`atI_addpoint` 为
`void`。no-op 与失败路径不 bump epoch、不改 C。

### 12.3 结构同构项

- `at_locate`/`at_next`/`atK_nextedge`/`atI_splitroot`/`atI_splitchild`/
  `atI_makeroom` 与 sp 对应函数同形；`at_next` 的桶扫描与 `atK_nextedge`
  在 anchortree 内合并为“bucket 扫描 + edge 提交”两层，`sp_next` 则用
  `spN_findslot` + `spK_locend` 单函数完成；两者语义一致，形态差异来自
  edge bucket 表示。无后继时 `atK_nextedge` 直接写 done。
- 插入由一个内部主入口 `atI_insert(ins, advance)` 维护 paths/off/poff/cur：
  空树转 `atI_makefirst`，当前 span 增长内联，按 Rx 直接选择 `atI_growleft`
  或 `atI_split`。`atI_insert` 的当前 span 增长与 `spI_grow` 同形；`advance`
  只提交 Cursor 终态。R10 转 `atI_growleft(ins, advance)`：叶内/跨叶共用
  一趟前驱子树最右路径的 mutation，按 `advance` 在循环外选择两个变体；
  `advance=0` 的变体在 mutation 时同步写 `paths` 并 fresh，`advance=ins` 的
  变体保持当前 edge 现场并保留端点为其 cur；函数返回 0/1 仅作 C89 提交
  语义。`atI_split` 只做 insertion split，按 RIGHT 桶迁移写 cur；
  `atI_splitpoint` 把新 edge 返回给 `atI_addpoint`。`atI_addpoint` 是第二个
  主入口，直接调用 `atI_splitpoint`。
- 删除不集中 rewind：`atD_rmleaf` 链不变，`atD_rmrange` 按 §7.2 设置续扫槽；
  `atE_purge` 不再有 Cursor 契约（只服务 `at_delanchor`，现场自成一体）。
- remove 相关短 helper 按用户指定内联（`atD_trimright`/`atD_trimleft` →
  `atD_rmrange`、merge-leaf 步骤 → `atD_stitch`），仅在不超 25 行且逻辑等价时
  执行。

### 12.4 领域差异（允许保留）

| 差异                                        | 领域原因                                                                                                                   | 同步策略                                                                                               |
| ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| run id/mask → anchor 池 + left/right 桶链表 | 一个 edge 可挂多个 START/END；ns 是 29bit 精确值，非位掩码                                                                 | 保留；D/I 主干只删去 run 合并分支                                                                      |
| LEFT/RIGHT 桶语义                           | 插入发生在边界 `p` 时槽按 gravity 分两族：LEFT 留 `p`、RIGHT 右移 `ins`；因此边界可能需复制（split）或整体右移（growleft） | 保留；`atI_insert`/`atI_split` 以 `advance` 提交 insert/append 两种 Cursor 终态，`atI_splitpoint` 只服务 point；`spI_grow`/`spI_pad` 的 span-id 判据不可直接搬 |
| 无 per-node 匹配聚合                        | ns 挂在 anchor 上，node/edge 无聚合                                                                                        | 保留；`at_next` 逐 edge 扫桶链                                                                         |
| EOF 零长 edge、首 edge 永久保留             | 左边缘 span 模型承载 T 位置端点与 0 边界                                                                                   | 保留                                                                                                   |
| `atI_addpoint` 点物化                       | anchor 端点在 Cursor 坐标取得/创建 edge 并链入桶，只服务 `at_newanchor`                                                    | 保留；并入 `/* insertion */` 块；复用 `atI_insert(0, 0)` 与 `atI_splitpoint` |
| `atE_purge` / `T->seam` 端点搬迁            | 回收 edge 前需搬桶                                                                                                         | 保留（仅 `at_delanchor`；insert 侧由 `atI_growleft` 直接平移；`atE_purge(T, e)` 无 Cursor 契约，返回 1 仅作 C89 提交语义） |
| `atK_forwardoff` 虚拟尾                     | 删除与 point 物化可跨 `T`                                                                                                  | 保留                                                                                                   |
| Cursor 续扫槽 `at_Point cur`                | 同一 edge 多个 anchor                                                                                                      | 保留                                                                                                   |
| epoch / `AT_ESTALE`                         | mutation 后 Cursor 悬空                                                                                                    | 保留                                                                                                   |
| 节点池 + edge 池分离                        | edge 是独立领域对象                                                                                                        | 保留                                                                                                   |
| `atU_reserve`                               | 需在 mutation 前一次算清 remove+insert 的 nodes/edges                                                                      | 保留                                                                                                   |
