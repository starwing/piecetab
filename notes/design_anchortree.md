# anchortree 设计（edge 模型）

> 状态：实现设计。
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
- 文档真实总长度：anchortree 不保存“EOF 之后”的字节，只保存已标记的字节缝。

### 坐标语义

- 坐标原点 `0` 由 `at_Tree` 内嵌的 `origin` edge 表达。
- 树内只保存位置 `> 0` 的 edge。
- 树内禁止 `bytes == 0` 的 edge。
- 不存在独立 EOF 哨兵；最后一个树内 edge（如果有）就是当前最后一个已知字节缝。
- seek/locate/advance 超过最后一个 edge 的位置时，用 `poff` 表达虚拟位置。
- 可以把这套结构理解为在“无限长”坐标线上标记字节缝：只在有端点项的位置产生 edge。

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

- 每个树内 edge 对应一个字节缝位置 `p > 0`。
- edge 的 `bytes` 定义为：**从上一个 edge（或 origin）到本 edge 越过的字节数**。
- 等价地：本 edge 位于它 `bytes` 所覆盖区间 `[prev, p)` 的**右侧字节缝**。
- 若文档中没有任何树内 edge，则 `T->bytes == 0`；`origin` 仍然存在。
- 树内 edge 的 `bytes > 0`；不存在两个 edge 位于同一字节缝。

示例：

- 只有 origin 和一个 edge@100：edge 的 `bytes == 100`，表示在 100 个字节后有一个字节缝。
- 若该 edge 没有任何端点项，则删除它，树变空树，`T->bytes == 0`。
- edge@10、edge@50、edge@100：三者 `bytes` 分别为 10、40、50。

### 2.2 树结构

```c
#define AT_FANOUT    34
#define AT_MAX_LEVEL 16
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
} at_Node;

struct at_State {
    void     *alloc_ud;
    at_Alloc *allocf;
    at_Pool   nodes;
    at_Pool   edges;
    at_Node   rt[AT_MAX_LEVEL];
};

struct at_Tree {
    at_Node   root;
    at_State *S;
    size_t    bytes;   /* 最后一个树内 edge 的位置；无树内 edge 时为 0 */
    int       levels;
    at_Edge   origin;  /* pos=0 的 edge，不在 B+ 树中 */
    at_Anchor *ids;    /* dense 对象池，0 号保留 */
    size_t    idcap;
    at_Id     idfree;
};
```

- 叶子层把 `children[i]` 按 `at_Edge *` 解释；内层按 `at_Node *` 解释。
- 叶子槽 `bytes[i]` 是正长度，含义见 §2.1。
- 内层 `bytes[i]` 是子树内所有叶子 `bytes` 的和。
- 树内 `bytes` 总和不等于“文档长度”，而是最后一个树内 edge 的位置。
- `at_State.rt[AT_MAX_LEVEL]` 是删除/stitch 的暂存根区；删除路径必须按 spantree.h 的 stitch 流程使用它。
- `at_Pool` 在 `AT_POOL_STATS` 编译配置下额外含 `size_t live_obj`，用于测试泄漏统计；该模式与 spantree.h 的 `SP_POOL_STATS` 相同，细节参考 spantree.h 的 pool 实现。

### 2.3 edge 对象

```c
typedef struct at_Point {
    at_Id    id;
    unsigned ns : 31;
    unsigned is_end : 1;
} at_Point;

typedef struct at_Edge {
    at_Node   *parent;   /* 所在叶子容器；origin 为 NULL */
    at_Index   idx;      /* 在 parent->children/bytes 中的下标；origin 不使用 */
    at_Point   first_left;
    at_Point   first_right;
    at_Point  *left_tail;
    at_Point  *right_tail;
} at_Edge;
```

- `first_left.id == 0` 表示左侧为空；`first_right.id == 0` 表示右侧为空。
- left/right 列表不排序；查找按 id 线性扫。
- `left_tail` / `right_tail` 指向小向量 header 后的首元素。
- 小向量私有 header 格式与项目其他 stb 库一致：
  ```c
  typedef struct atV_Header {
      unsigned len, cap;
  } atV_Header;
  ```
  - `tail` 指向 `atV_Header` 后的首个元素，即 `atV_hdr(tail) == (atV_Header *)tail - 1`。
  - 容量/长度用 `unsigned`，不使用 `size_t`。
  - 配套操作以 `atV_` 前缀实现：`atV_hdr`、`atV_len`、`atV_cap`、`atV_push`、`atV_reserve`、`atV_free` 等。
- 每个树内 edge 必须至少有一个端点项；失去全部端点项后从树中删除。
- `origin` 可以没有端点项；它始终存在。

### 2.4 object 池

```c
typedef struct at_Anchor {
    at_Edge *start;
    at_Edge *end;
} at_Anchor;
```

- `at_Id` 从 1 开始，0 保留为“空/未使用”。
- `at_Id` 直接索引 `T->ids`。
- 空闲 id 通过 freelist 回收；freelist 用空闲 `at_Anchor` 的 `start` 字段串接下一个空闲 id，`NULL`/0 表示结束。
- `anchor->start/end` 指向 `origin` 或树内 edge。
- `start == end` 表示零长 object；两个端点项在同一 edge 内。
- 允许 `start` 位置大于 `end` 位置（交叉零长），`at_range` 原样返回。

### 2.5 Cursor

```c
typedef struct at_Cursor {
    at_Tree   *T;
    size_t     off;       /* 当前 edge 的 gap 左端绝对位置 */
    size_t     poff;      /* 在 gap 内偏移；等于当前 edge->bytes 表示恰在该 edge */
    size_t     left_idx;  /* left 列表当前下标/已消费计数 */
    size_t     right_idx; /* right 列表当前下标/已消费计数 */
    at_Id      cur;
    at_Node  **paths[AT_MAX_LEVEL]; /* 根到叶子槽的路径 */
} at_Cursor;
```

- `cur` 不是最终设计的一部分；当前实现临时复用作 `at_next` 迭代状态，后续会替换为正确的迭代表示，可能修改。
- `at_seek` 初始化 Cursor；它没有“返回位置”的语义。
- `off + poff` 是 Cursor 的绝对坐标。
- 树内当前 edge 就是 `*C->paths[atK_levels(C)]` 所指向的叶子槽内容（按 `at_Edge *` 解释）。
- 指向树内 edge@`p` 时，`off + poff == p`，且 `poff == edge->bytes`。
- 位于两个树内 edge 之间的 gap 内时，当前槽是右侧下一个树内 edge，`poff` 小于其 `bytes`。
- 超过最后一个树内 edge 时，仍停留在最后一个树内 edge 槽，`poff` 超过其 `bytes`，超出部分为虚拟距离。
- 空树/指向 origin 的状态由 Cursor 的路径与 `poff` 约定表达，不单独保存 `at_Edge *edge` 字段。
- 端点迭代不保存 `at_Point *` 指针：tail 向量可能在操作中 resize，指针会失效。用 `left_idx/right_idx` 表达 left/right 两个逻辑列表中的当前位置：
  - `0` 表示 first_left/first_right；
  - `>= 1` 表示 `left_tail[left_idx-1]` / `right_tail[right_idx-1]`；
  - 当前所在侧由“left 是否已遍历完”决定：先 left 后 right，left 耗尽后切到 right。

## 三、不变量

1. 树内每个叶子槽 `bytes > 0`。
2. 树内 edge 的位置严格递增；`T->bytes` 等于最后一个树内 edge 的位置。
3. `origin` 不在树中，不计入 `T->bytes`。
4. 每个树内 edge 至少有一个端点项；没有端点项的树内 edge 必须被删除。
5. 端点项只存在于它所在字节缝对应的 edge 中：
   - 位置 0 的端点在 `origin`；
   - 位置 `p > 0` 的端点在树内 edge@`p`。
6. `anchor->start/end` 指向的 edge 确实包含对应角色的端点项。
7. edge 的 `parent/idx` 与它所在叶子槽一致。
8. 树内不存在 0 长 edge，因此不存在“同一字节缝有两个 edge”的情况。

## 四、edge 删除与 gap 合并

删除一个树内 edge@`p` 时：

- 它已经没有端点项（调用方应先删除其上所有端点）。
- 若存在下一个树内 edge@`q`：把 `edge@p.bytes` 加到 `edge@q.bytes` 上，然后从树中删除 `edge@p`。
  - 效果：`edge@q` 的 gap 从“`p` 到 `q`”变为“`p` 的前一个位置到 `q`”。
- 若不存在下一个树内 edge：直接从树中删除 `edge@p`，`T->bytes` 减少 `edge@p.bytes`。
- `origin` 永不删除。

这条规则使“空 edge 直接删、树可空、`T->bytes == 0`”成立。

## 五、创建 edge

在位置 `p` 创建/取得树内 edge：

- `p == 0`：返回 `&T->origin`。
- 已存在 `edge@p`：直接使用。
- `p` 位于某个 gap 内：设 gap 左端为 `a`（`a` 是前一 tree edge 或 origin），右端为 `b`（下一个 tree edge；若不存在则说明 `p > T->bytes`）。
  - 新建 edge@`p`，`bytes = p - a`。
  - 若存在 `b`：原 `edge@b.bytes` 变为 `b - p`；新 edge 插到 `edge@b` 前。
  - 若不存在 `b`：新 edge 作为最后一个 edge 追加，`T->bytes` 变为 `p`。
- 新建的 edge 必须先获得端点项，或在同一操作中被删除；不能以空 edge 状态留在树中。

## 六、对象操作

### 6.1 `at_newanchor(C, len, ns, gravity, *out)`

- `C` 已初始化，`start = at_offset(C)`。
- `end = start + len`；允许 `end` 超过当前最后一个 edge（虚拟区），此时会创建新的最后 edge。
- 确保 start edge：`start == 0` 用 origin，否则创建/取得 `edge@start`。
- 确保 end edge：`end == 0` 用 origin，否则创建/取得 `edge@end`。
- 从 freelist 分配 `at_Id`，初始化 `anchor->start/end`。
- `ns` 是调用方给的 namespace 标签，写入两个端点项的 `at_Point.ns`；anchortree 不解释其含义。
- 按 gravity 插入端点项：

| gravity | start 端点 | end 端点 |
|---|---|---|
| AT_GROW  | LEFT  | RIGHT |
| AT_FIXED | RIGHT | LEFT  |
| AT_SHIFT | RIGHT | RIGHT |
| AT_LOCK  | LEFT  | LEFT  |

- START 项插入 start edge 的对应 left/right 列表；END 项插入 end edge 的对应 left/right 列表。
- `len == 0` 时 start/end 是同一个 edge，仍插入两个端点项。

### 6.2 `at_del(T, id)`

- 通过 `at_Id` 直索引得到 `at_Anchor`。
- 从 `anchor->start` 删除角色 START 的项；从 `anchor->end` 删除角色 END 的项。
- 对涉及的树内 edge，若删除后没有剩余端点项，按 §四 删除该 edge。
- 把 `at_Id` 归还 freelist。

### 6.3 `at_range(T, id, *start, *end)`

- 若 `anchor->start` 是 origin，start 位置为 0；否则沿 edge 的 `parent/idx` 累加得到 edge 位置。
- end 同理。
- 交叉零长时原样返回 `*start > *end`。

### 6.4 `at_gravity(T, id)`

- 在 start/end edge 的 left/right 列表中查找对应端点项。
- 由 start 所在侧与 end 所在侧按上表反推 GROW/FIXED/SHIFT/LOCK。
- 错误时返回负值。

## 七、文本编辑

anchortree 不提供独立的 `insert` 与 `append` 编辑原语。`at_splice` 是唯一编辑原语，其插入阶段遵循 **append semantics**：文本在当前 Cursor 处插入后，插入内容位于该坐标的左侧端点与右侧端点之间；按 gravity 应右移的端点移到新文本之后。若调用方需要“在当前 Cursor 之前插入”的语义，应先把 Cursor 后退所需长度（`at_advance(-len)`），再调用 `at_splice`。

### 7.1 插入阶段（append semantics）

- 插入发生在 Cursor 当前坐标 `p`。
- 若 `p` 不是树内 edge 位置，并且存在后续树内 edge：
  - 插入不影响任何端点本身；
  - 把后续第一个 edge 的 `bytes` 增加 `ins`，使之后所有 edge 自动右移。
- 若 `p` 是树内 edge 位置（或 origin）：
  - LEFT 端点项留在 `p`；
  - RIGHT 端点项移到 `p + ins`；
  - 若 `p + ins` 处没有 edge，创建新 edge；
  - 被移动的端点项按角色同步 `anchor->start/end`。
- 若 `p` 超过最后一个 edge：
  - 若没有可移动的端点，则不产生树内变化；
  - 若需要承载端点或后续操作，则通过创建新的最后 edge 扩展。
- 任何因插入而变空的树内 edge 按 §四 删除。

### 7.2 删除

- 删除 `[L, R)`。
- 受影响端点塌缩到同一个幸存 `edge@L`；不设单独的 R 幸存 edge。端点项移动时保留其 LEFT/RIGHT 侧。
- 位置 `p < L` 的端点不动；位置 `p > R` 的端点随度量更新自动左移 `del`。
- 同一 object 两端落到同一 edge 时保留 START/END 两个端点项，表示零长或交叉。
- 删除后所有变空的树内 edge 按 §四 删除。
- 删除到最后一个 edge 之后没有已知字节缝时，树自然收缩；`T->bytes` 由度量更新自动变小。

### 7.3 `at_splice(C, del, ins)`

- 语义为“删除 `[C, C+del)` 后再在 C 处插入 `ins`”。
- 删除阶段按 §7.2 处理端点塌缩与 edge 删除。
- 插入阶段按 §7.1 处理端点迁移与 edge 扩展。
- 若 `del == 0 && ins == 0`，直接成功。

## 八、对象级迭代

- 迭代按树内 edge 坐标序进行。
- 对每个 edge，只产出角色为 START 的端点项；END 项跳过。
- 正长 object 的 END 在其 end edge 被跳过。
- 零长 object 的 START 产出一次，END 跳过。
- 同一 edge 内多个对象不排序；稳定序由上层按 `(start, id)` 自行稳定。
- `at_next` 返回下一个 object 的 START id；到达末尾或参数错误时返回 `AT_NONE`。
- `at_next` 支持 ns 过滤：`ns == 0` 不过滤；`ns > 0` 只返回 `at_Point.ns == ns` 的 START。
- `at_next` 的 `plen` 为可选输出：仅当非 NULL 时，查找该 object 的 range 末端并计算长度写入 `*plen`；若 `start > end`（交叉区间），写入 0。

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
AT_API int   at_advance(at_Cursor *C, at_Delta d);
AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen);
```

```c
AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out);
AT_API int at_del(at_Tree *T, at_Id id);

AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end);
AT_API int at_gravity(at_Tree *T, at_Id id);
```

```c
AT_API int at_splice(at_Cursor *C, size_t del, size_t ins);
```

- `at_seek` 初始化 Cursor，不返回位置。
- `at_advance` 在坐标线上前后移动；越过最后一个 edge 时进入虚拟 `poff`。
- `at_gravity` 返回 `AT_GROW..AT_LOCK`，错误时返回负值。
- `at_next` 返回下一个 object 的 START id；`ns` 为过滤条件，`plen` 为可选长度输出；到达末尾或参数错误时返回 `AT_NONE`。

## 十、内存估算

- `at_Anchor`：start/end 两个指针，约 16B。
- `at_Edge`：parent/idx + first_left/right + tail 指针；实际以 `sizeof` 为准。
- `at_Point` 存在 edge 的 first/tail 小向量中；单端点侧只占 first 字段。
- 树内 edge 数量与“有端点的字节缝”数量同阶。
- `at_Id` 稠密数组 + freelist，索引 O(1)。

## 十一、实现注意

- 所有叶子槽搬移（copy/move/remove/makespace/split/merge）后，要同步被搬移 edge 的 `parent/idx`。
- 度量更新沿路径累加；删除最后一个 edge 时 `T->bytes` 自动收缩。
- 端点 tail vector 的扩容必须在任何端点 mutation 前通过 `atO_reserve`/`atV_reserve` 完成；reserve 成功后的 push 不得失败，失败表示 reserve 计算错误，应 assert。
- debug 构建可维护反查断言：`anchor->start/end` 指向的 edge 必须包含对应角色端点项。
- 树内不引入 0 长叶子槽；`origin` 是唯一的“位置 0”表达，且不在 B+ 树中。

## 十二、当前实现偏差（专题，待修正）

以下条目是当前 `anchortree.h` 实现与本文设计的已知偏差。它们不是最终设计；后续会按专题调整实现，并在修正后从本节移除。

- **`at_next` ns/plen**：当前实现仍为 `at_Id at_next(at_Cursor *C)`，不支持 ns 过滤与 `plen` 输出；本文已更新为 `at_next(C, ns, &plen)`，实现需随后对齐。
- **`at_Cursor.cur`**：当前实现临时复用作迭代状态；该字段不是最终设计，后续应设计正确的迭代表示并移除。
