# Neovim marktree 调研报告

> 调研对象: `neovim/src/nvim/marktree.{c,h}` + `marktree_defs.h`（约 2590 + 139 + 100 行）
> 调研目的: 为 piecetab 开发 marktree 类似功能提供参考。
> 后续设计决议见 `notes/design_spantree.md`（spantree：字节覆盖属性模型）。
> 结论摘要: marktree 是 kbtree（B-tree）改造的 (row,col) 位置树，mark 为带 id
> 的实体对象；**无区间批量删除 API**（上层逐个 `marktree_del_itr`）；文本编辑
> 用 `marktree_splice` 把区域内 mark **挤压到边界而非删除**；gravity 决定
> mark 在插入/删除点的粘附方向，且**参与排序 key**。

## 一、总体架构

### 1.1 数据结构

```c
// marktree_defs.h
enum { MT_MAX_DEPTH = 20, MT_BRANCH_FACTOR = 10 };  // 实际最大分支 2*10

typedef struct { int32_t row, col; } MTPos;

typedef struct {              // mark 本体（存于节点 key 数组）
  MTPos pos;                  // 相对位置（见 1.2）
  uint32_t ns, id;            // namespace + id → 64 位唯一 lookup key
  uint16_t flags;             // gravity/end/paired/decor 等
  DecorInlineData decor_data; // 装饰 payload
} MTKey;

struct mtnode_s {             // B-tree 节点（叶/内共用）
  int32_t n;                  // key 数（T-1 .. 2T-1）
  int16_t level;              // 0 = 叶
  int16_t p_idx;              // 在父节点中的下标
  Intersection intersect;     // 覆盖本子树的 pair-id 集合（kvec）
  MTNode *parent;             // ← 有 parent 指针（linecache 无）
  MTKey key[2*T - 1];
  struct mtnode_inner_s s[];  // 仅内部节点分配: ptr[2T] + meta[2T][5]
};

typedef struct {
  MTNode *root;
  uint32_t meta_root[kMTMetaCount]; // 全树各类装饰计数
  size_t n_keys, n_nodes;
  PMap(uint64_t) id2node[1];        // id → 所在节点，O(1) 反查
} MarkTree;
```

要点：

- **B-tree 而非 B+ 树**：key 存于所有层级（内部节点也存实 mark），
  一个 mark 可能在内部节点上。遍历需上下穿梭（`itr_next` 处理
  "internal key" 情形）。
- **`id2node` 哈希表**：`mt_lookup_id(ns,id,end)` 组合出 64 位 key，
  映射到 mark 所在节点。所有搬动 key 的操作（split/merge/pivot/swap）
  必须调 `refkey()` 维护。这是 O(1) 按 id 定位的代价。
- **meta 子树计数**：`meta[i][kMTMetaCount]` 记各子树内 5 类装饰 mark
  数，供 filtered 遍历剪枝（`marktree_itr_get_filter`），思路与
  linecache 的 bytes/breaks 度量聚合同构。

### 1.2 相对坐标（核心设计）

base 是**按节点定的，不是按单个 key**。精确规则：

1. 每个节点 x 有唯一 base B(x) = **x 子树在全树 key 中序序列中的前驱
   key**（进入 x 子树时最后跨过的 separator）的绝对位置。根 base=(0,0)。
2. **节点内所有 key 共享这一个 base**——`key[j]` 不相对 `key[j-1]`，
   全部直接相对 B(x)，因此节点内可直接 key_cmp 二分。
3. 子节点 base 推导：`ptr[0]` 继承 B(x)；`ptr[i], i>0` 的 base =
   `key[i-1]` 的绝对位置。

```
            x: [k1      k2]        B(x) = 上层传下
              /    |      \
            c0    c1       c2
B(c0)=B(x)    B(c1)=k1绝对   B(c2)=k2绝对
```

k1、k2 都相对 B(x)（k2 不相对 k1）。c0 一路是第 0 子时 base 上溯到更
高层甚至原点——操作性定义：从 x 上行找第一个 `p_idx > 0` 的祖先，取其
parent 的 `key[p_idx-1]`。

绝对位置 = 从根下行沿路 `compose` 所有跨过的 separator 的相对 pos
（迭代器 `itr->pos` 即此累积值）：

```c
static void relative(MTPos base, MTPos *val);   // 绝对→相对
static void unrelative(MTPos base, MTPos *val); // 相对→绝对
static void compose(MTPos *base, MTPos val);    // 迭代器下行累积
```

**行列混合语义**（unrelative, marktree.c:92-100）：累加不是向量加法——

- `val.row == 0`（与 base 同行）: 绝对 = `(base.row, base.col + val.col)`
- `val.row > 0`（跨行）: 绝对 = `(base.row + val.row, val.col)`，
  **col 本身就是绝对列**

原因：col 是行内偏移，行首天然是绝对参照；跨行后列偏移不可加。col 仅当
整条 base 链同行时才是相对的。收益：行内编辑（same_line splice）只调
该行内 mark 的 col，后续行 mark 跨行、col 绝对、row 未变，完全不用碰
（splice 第三循环 same_line 提前 break 的基础, marktree.c:2051-2054）。

源码佐证：下行换 base（marktree.c:1430-1434，i==0 则继承）；同节点共享
base（`marktree_itr_pos` marktree.c:1705-1710，itr->pos 仅下行时累积）；
separator 下沉时同 base 直接减（`merge_node` marktree.c:1035-1037）。

好处：**平移一整棵子树只需修改一个 separator key 的 pos**——splice 每层
只改 O(2T) 个 key 的根基。linecache 用"度量和"隐式表达相对位置，殊途
同归（深入对比见第七节）。

### 1.3 排序 key（flags 参与排序！）

```c
static int key_cmp(MTKey a, MTKey b) {
  // 1. row  2. col  3. flags & cmp_mask
  const uint16_t cmp_mask =
    MT_FLAG_RIGHT_GRAVITY | MT_FLAG_END | MT_FLAG_REAL | MT_FLAG_LAST;
}
```

同一 (row,col) 上的次序: `left-gravity marks < right-gravity marks`
（RIGHT_GRAVITY 是 bit14，头文件注明 "These _must_ be last to preserve
ordering"）。伪 key 技巧：查询时构造无 `MT_FLAG_REAL` 的 pseudo-key，
必然排在同位置真实 key 之前/后（配合 `MT_FLAG_LAST` bit15 可排最后），
用于把迭代器精确插到 left 组与 right 组之间。

## 二、接口清单

### 2.1 写入

| 接口 | 功能 |
|---|---|
| `marktree_put(b, key, end_row, end_col, end_right)` | 插入 mark；`end_row>=0` 时成对插入 start/end 两个 key 并建立 intersection |
| `marktree_put_key(b, k)` | 底层单 key 插入（根满则 split 加深） |
| `marktree_del_itr(b, itr, rev)` | 删除迭代器当前 mark，迭代器**保持有效并指向下一 mark**；返回配对另一端的 id（置 ORPHANED，须由调用方接着删） |
| `marktree_move(b, itr, row, col)` | 移动 mark：同叶内小移动仅 memmove；否则 del+put |
| `marktree_revise_meta(b, itr, old_key)` | 原地改 flags 后修正祖先 meta 计数 |
| `marktree_clear(b)` | 释放全树 |

### 2.2 文本编辑适配（位置更新）

| 接口 | 功能 |
|---|---|
| `marktree_splice(b, start_l, start_c, old_l, old_c, new_l, new_c)` | 文本 [start, start+old_extent) 被 new_extent 大小的内容替换；全树 mark 位置随之更新（详见第三节） |
| `marktree_move_region(b, start, extent, new_pos)` | 区域搬迁：暂存区域内 mark → splice 删旧 → splice 插新 → 重新 put |

### 2.3 查询/遍历

| 接口 | 功能 |
|---|---|
| `marktree_itr_get(b, row, col, itr)` | 定位 ≥ (row,col) 的第一个 mark |
| `marktree_itr_get_ext(b, pos, itr, last, gravity, oldbase, filter)` | 扩展版：`gravity` 控制落在同位置 left/right 组之间；`last` 取 < pos 的最后一个；`filter` 按 meta 剪枝 |
| `marktree_itr_first / itr_last` | 首/末 mark |
| `marktree_itr_next / itr_prev` | 前后步进（叶内 O(1)，摊还 O(1)） |
| `marktree_itr_current(itr)` | 取当前 mark（换算绝对 pos） |
| `marktree_itr_pos(itr)` | 仅取绝对位置 |
| `marktree_lookup(b, id, itr?)` / `marktree_lookup_ns(...)` | 按 id O(1) 反查（哈希 + `marktree_itr_set_node` 重建迭代器路径） |
| `marktree_get_alt / get_altpos` | 取配对 mark 的另一端 |
| `marktree_itr_get_filter / itr_next_filter / itr_step_out_filter` | 用 meta 计数跳过无关子树的过滤遍历 |
| `marktree_itr_get_overlap(b, row, col, itr)` + `marktree_itr_step_overlap(b, itr, pair)` | **stab query**：枚举所有覆盖 (row,col) 的 pair 区间（利用节点 intersect 集合，O(log n + k)）；结束后 itr 退化为普通迭代器可继续 `itr_next` 扫区间 |

### 2.4 pair 区间与 intersection

成对 mark（区间高亮等）的 start/end 各是一个独立 key。为支持 stab query，
维护不变式：pair 区间完全覆盖某子树时，其 id 记入该子树根的
`node->intersect` 集合（且不再重复记入子孙）。split/merge/pivot 时用
`intersect_common/add/sub/mov/merge` 五个有序集合运算修补。这是 marktree
中复杂度最高、bug 最多的部分（`unintersect_node` 里有条注释承认 assert
在用户端失败过）。

## 三、批量删除如何处理

**关键结论：marktree 本体没有"区间批量删除"接口。** 两个相关场景分别是：

### 3.1 真删除：上层逐个删（extmark_clear 模式）

`extmark.c:extmark_clear(buf, ns, l_row, l_col, u_row, u_col)`:

```c
marktree_itr_get(tree, l_row, l_col, itr);          // 定位区间起点
while (true) {
  MTKey mark = marktree_itr_current(itr);
  if (越过 (u_row,u_col)) break;
  if (mark.ns 匹配) extmark_del(...);   // 内部 marktree_del_itr
  else              marktree_itr_next(tree, itr);
}
```

可行性全靠 `marktree_del_itr` 的两条保证：

1. **删除后迭代器仍有效**，指向被删 mark 的下一个（文档明言推荐只正向
   迭代删除，`rev=true` 路径直接 `abort()` 未实现）；
2. 删除内部 key 时先 `itr_prev` 偷前驱叶 key 顶替（经典 B-tree 删除），
   然后 steal-left / steal-right / merge 自底向上修树，全程同步修正迭代
   器的 `s[lvl].i` 路径。

即批量删除 = O(k·log n)，k 为区间内 mark 数。没有像 linecache
`lc_remove` 那样的整棵子树摘除 + stitch 缝合优化。原因推测：mark 密度
远低于行密度，且每个 mark 都要从 `id2node` 哈希表删除、pair 要解除
intersection，逐个删是自然选择。

### 3.2 假删除：splice 把区域内 mark 挤到边界

文本删除时 mark **不删**。`marktree_splice` 语义：`[start, start+old_extent)`
的文本被 `new_extent` 大小的新文本替换。区域内 mark 按 gravity 收敛：

- **left-gravity mark → 收敛到 start**（第一个循环，`rawkey(itr).pos = loc_start`）
- **right-gravity mark → 收敛到新区域末端 start+new_extent**（第二个循环，`rawkey(itr).pos = loc_new`）

难点：收敛后同一位置上必须维持 "left 组在前、right 组在后" 的排序不变式，
但区域内原本 left/right mark 是按位置交错的。做法（marktree.c:1957-2007）：

```
itr    从 start 正向扫（gravity=true 定位，跳过 start 处 left 组）
enditr 从 old_extent 末端反向找最后一个 left-gravity mark
扫描中遇到 right-gravity mark 时:
    enditr 左移至指向 left-gravity mark
    swap_keys(itr, enditr)      ← 物理交换两个 key（pos 不换）
    → right mark 被换到区域尾部，left mark 换到当前位置
itr 与 enditr 相遇 → 进入第二循环处理剩余 right 组
```

`swap_keys` 需同步维护 `id2node`、meta 计数（两节点到 LCA 的路径修正）、
pair 的 intersection（记入 damage map，splice 尾部统一重算
`marktree_intersect_pair` 删旧加新）。

第三个循环处理区域之后的 mark：仅对 `old_extent.row` 同行的 mark 调 col，
之后各行加 `delta.row`；纯列内编辑（`same_line`）提前退出。得益于相对
坐标，**每层只需改动 O(2T) 个 key**，整体 O(T·log n + 区域内 mark 数)。

### 3.3 区域搬移 marktree_move_region

删除 + 重放：正向扫区域逐个 `marktree_del_itr` 并把 key（相对化后）存入
kvec → `splice` 缩掉旧区域 → `splice` 撑开新区域 → 逐个 `put_key` 回放，
pair 用 `marktree_restore_pair` 重建 intersection。印证 3.1：区间成批
操作的标准姿势就是"迭代器 + 逐个删"。

## 四、gravity：定义与用法

### 4.1 定义

```c
#define MT_FLAG_RIGHT_GRAVITY (((uint16_t)1) << 14)
static inline bool mt_right(MTKey key) { return key.flags & MT_FLAG_RIGHT_GRAVITY; }
```

语义：**在 mark 的精确位置发生插入时，mark 粘向哪边**。

- left-gravity（默认）：留在原位（插入文本在 mark 之后）
- right-gravity：随文本右移（mark 粘在插入文本之后）

pair mark 的两端 gravity 独立（`marktree_put` 的 `end_right` 参数）。
典型用法：区间高亮 start=right, end=left → 在边界打字不扩张区间；
`right_gravity=true, end_right=true` → 在末尾打字扩张区间。

### 4.2 三处实际作用

**(a) 排序**（key_cmp, 见 1.3）：同位置 left < right。这不是实现巧合而是
必要条件——splice 收敛时 left 组归 start、right 组归区域尾，二者最终
位置有序，排序不变式才能通过 swap 修复。

**(b) splice 收敛方向**（见 3.2）：`mt_right(rawkey(itr))` 决定 mark 被
挤向 start 还是 new_extent 末端。这正是编辑器语义：删除包含 mark 的区域
后，left-gravity mark 落在删除点前沿，right-gravity 落在替换文本之后。

**(c) 查询定界**：

- `marktree_itr_get_ext(..., gravity, ...)` 构造 pseudo-key
  `flags = gravity ? MT_FLAG_RIGHT_GRAVITY : 0`，把迭代器插到同位置
  left 组之后 / right 组之前。splice 用 `gravity=true` 定位 start，
  使 start 处 left-gravity mark 不被收敛（其位置不在删除区域内）；
  同理 `enditr` 用 `last=true, gravity=true` 取 old_extent 处最后一个
  left mark（该处 right mark 不属于删除区域）。
- 区间端点归属判定（extmark.c:382-387）：

```c
if (mark.pos.col - !mt_right(mark) < l_col) ...  // left mark 在左边界视作界外
else if (mark.pos.col + mt_right(mark) > u_col)  // right mark 在右边界视作界外
```

即半开语义：位置相同处，left-gravity mark 属于左侧区间，right-gravity
属于右侧区间。

### 4.3 对 piecetab 的启示

gravity 本质是"位置相等时的次序位"。若用字节缝点模型，可把 gravity 编入
比较最低位（offset*2+gravity）完整继承排序/定界/收敛三处逻辑。

**但 spantree 设计（notes/design_spantree.md）已决定不用缝模型**：标记
作为字节覆盖属性，gravity 由编辑操作方向取代（段内插入继承段属性；
append 继承左段、insert 继承右段），决策从数据搬到操作，与 vim `a`/`i`
和 sam 语义同构。本节分析保留作缝模型参考。

## 五、与 linecache 对比

| 维度 | neovim marktree | piecetab linecache |
|---|---|---|
| 树型 | B-tree（kbtree 改），key 在所有层 | B+ 计量树，数据只在叶 |
| 元素 | 实体 mark（id/ns/flags/decor payload） | 匿名行断点（只是计数，无身份） |
| 坐标 | 二维 (row,col)，显式相对坐标 | 一维字节偏移，度量和隐式表达 |
| 定位 | 按 pos O(log n)；按 id O(1)（哈希 id2node） | 按 offset/行号 O(log n)；无 id 概念 |
| parent 指针 | 有（+p_idx），迭代器可上行 | 无，游标存根→叶路径槽位 |
| 节点分配 | xcalloc 逐个（xmalloc 失败即 abort） | 页池 + freelist，reserve 预分配保事务 |
| 区间删除 | 无 API；上层逐 mark `del_itr`，O(k·log n) | `lc_remove` 子树摘除 + stitch 缝合，O(log n) |
| 文本编辑 | `splice`：mark 不删，按 gravity 挤压到边界 | `lc_splice`：区域内 break 物理删除 |
| gravity | 有，参与排序 + 收敛方向 + 查询定界 | 无此概念（break 无粘附语义） |
| 区间对象 | pair mark + 节点 intersect 集合，支持 stab query | 无 |
| 子树聚合 | meta[5] 装饰计数（剪枝用） | bytes+breaks 双度量（导航用） |
| 迭代器有效性 | del_itr 后仍有效（核心卖点） | 编辑后游标失效，需重新 seek |
| 平衡修复 | 经典删除：steal 左/右 → merge，自底向上 | trim + fold + balance + stitch |
| OOM 策略 | 无（xmalloc abort） | reserve 预分配，LC_ERRMEM 安全返回 |

**互补关系**：linecache 解决"位置→行号"度量查询；marktree 解决"位置上
挂对象且随编辑漂移"。

> **[已决策]** 后续讨论确定 piecetab 走 **spantree**（字节覆盖属性 +
> linecache 骨架）+ 独立身份层的分层方案，见
> `notes/design_spantree.md`。下列早期建议 1、2、4、5 仍成立并已吸收进
> spantree 设计；建议 3（gravity 编入排序 key）被"操作方向决定继承"
> 取代。

1. **B+ 树 + 池分配 + reserve 事务**沿用 linecache 骨架（避开 marktree
   的 parent 指针与 per-node malloc）；
2. mark 需要身份（id→node 映射）时才引入哈希表；若只需匿名 mark
   （如折叠点、诊断位置），可省掉 id2node 与 refkey 全套维护。
   注意：**id2node 的前提是 node 有 parent 指针**（反查后需
   `marktree_itr_set_node` 上行重建迭代器路径），linecache 骨架无
   parent，仅加哈希表无法工作；
3. ~~gravity 位编入排序 key~~（spantree 以操作方向取代，见 4.3）；
4. 批量删除二选一：mark 稀疏 → 学 marktree 迭代器逐删（要求 del 后
   迭代器有效）；mark 稠密 → 学 lc_remove 子树摘除（但 mark 带 payload
   时须先枚举回收，逐删可能反而简单）；
5. pair 区间 + intersection 集合复杂度极高（neovim 自己都有 assert 翻车
   注释），除非明确需要 O(log n) stab query，否则**不建议引入**；区间可
   用两个独立 mark + 上层配对表达。

## 六、B+ 度量和 vs B 树相对偏移：定位方式对比

两者都是"位置不存绝对值，存增量"，差别在增量锚在哪：

- **度量和**（linecache）：增量锚在**每个元素间隙**——位置 = 路径前缀
  和，元素只贡献"长度/间距"
- **相对 key**（marktree）：增量锚在**子树 separator**——同节点 key
  共享 base，各存 delta-to-base

| 维度 | B+ 树 + 度量和 | B 树 + key 相对偏移 |
|---|---|---|
| 编辑平移成本 | **近零**：改编辑点一处度量 + 上行聚合 O(log n)，后缀位置隐式全平移 | O(T·log n)：splice 需改右邻每层 O(2T) 个 key 的 delta |
| 节点内定位 | 线性扫描累减（度量是各子树总量，无法二分） | **可二分**：同 base 直接 key_cmp（getp_aux） |
| 元素身份 | 无。位置即身份 | 有。id2node O(1) 反查；代价是搬 key 必 refkey |
| 同位置多元素 tie-break | 别扭：0 度量元素不占位 | 天然：flags 参与排序（gravity/END 位） |
| 不变式维护难度 | **低**：加减聚合 | 高：处处 relative/unrelative 修正，bug 重灾区 |
| 区间批量删除 | **整子树摘除** O(log n)（stitch） | 逐个 O(k·log n)（id2node/intersection 逼逐 key 善后） |
| 顺序扫描 | B+ 叶层连续，缓存友好 | key 散布各层，itr_next 上下穿梭 |
| 表达"区段长度" | **天然**（度量即长度） | 别扭（长度编码成坐标差） |
| 表达"稀疏点对象" | 别扭（无身份/tie-break） | **天然** |

**为何 marktree 平移更贵**：key 相对**节点 base** 而非前驱元素，base
右移时同节点后续 key 逐个要改。若改成相对前驱（纯差分），平移只改一处，
但节点内二分即废（定位需线性累加）。这是根本 trade-off：**想平移近零，
右侧兄弟存的量必须与左侧无关**（即"总量"型度量，必然线性扫）；**想节点
内二分，必须存相对同一 base 的单调序列**（平移必改 O(T)/层）。不可兼得：

- lazy shift 标记（线段树思路）不可行——平移影响"某点之后的后缀"而非
  整子树，路径每层只有部分 key 要动，lazy 打不上
- 节点内前缀和数组可二分，但任何更新 O(T) 重建，等于没省
- T 缩到 1 = 二叉 marker tree（Atom splay 做法），更新局部性最好但丢
  缓存局部性

且 T ≤ 几十时**节点内线性扫比二分快**（SIMD/预取友好、无分支预测失败）：
linecache FANOUT=62 线性扫、marktree T=10（key 32+ 字节大对象、splice
每层 O(T) 写成本压 T）都在线性收益区间，`getp_aux` 的二分 n≤19 优势可
忽略。**度量树"线性扫、平移近零"这笔交易更划算**——前提是不需要
key_cmp 式复杂排序。

**负载特征佐证**（neovim 实际）：读远多于写——每次重绘扫屏幕范围 mark
（每可见行 itr_get + itr_next，窗口顶 stab query），滚动/光标移动都触发；
写是每键 splice + 插件阵发 put/clear。meta 剪枝、filter 遍历、
intersection 全为读优化；splice 复杂但只求正确。

## 七、extmark 标记模型（点/缝/重叠/合成）

**模型：点，不是片段**。extmark 本体是单点 (row,col)；区间 = 两个独立
点 key（start/end，`MT_FLAG_PAIRED` 关联）。无片段实体：文本不因 mark
分段，mark 不拥有文本，只是两枚图钉。与 piece table 正交。

**位置语义：字节缝**。col = "第 col 个字节之前的缝"。证据：gravity 只在
缝模型下有意义（插入发生在缝上）；端点归属 extmark.c:382-387 同一缝按
gravity 劈左右半；高亮区间 = [start缝, end缝) 半开。mark 不附着字符：
文本删除只收敛位置不删 mark（除非 `MT_FLAG_INVALIDATE` 置 INVALID）。

**重叠：任意且互相独立**。可嵌套、可部分交叉（`[A [B A) B)` 合法），树
里就是按位置排序的独立 key。marktree 不管重叠语义（intersection 仅加速
stab query）。

**渲染：存储不分段，逐列合成**。[AAABBBBAAA] 永远存 2 个标记（4 个
key），绝不分裂三段。合成在重绘管线 `decor_redraw_col`
（decoration.c:730-792）：mark start 处按 `(priority, ordering)` 二分插
入 active 列表，每列 `hl_combine_attr` 逐个叠加。**非"栈取最上"**：属性
级合并，高优先级只覆盖其设置的字段，`hl_mode` 可选
replace/combine/blend（decoration_defs.h:50-52）。

**含义**：点模型下重叠合成是渲染端职责，树只维护"有序点集 + pair 关联"。
spantree 反其道：把合成结果直接存进树（染色即最终值），渲染端零合成，
重叠合成上移到染色写入方——见 `notes/design_spantree.md`。

## 八、参考源码索引

| 主题 | 位置 |
|---|---|
| 排序与 pseudo-key | marktree.c:117-163 (`key_cmp`, `marktree_getp_aux`) |
| 插入与分裂 | marktree.c:181-320 (`split_node`, `marktree_putp_aux`, `marktree_put`) |
| 删除协议（六步注释） | marktree.c:515-773 (`marktree_del_itr`) |
| splice 挤压收敛 | marktree.c:1913-2107 (`marktree_splice`, `swap_keys`) |
| 区域搬移 | marktree.c:2109-2145 (`marktree_move_region`) |
| stab query | marktree.c:1743-1852 (`itr_get_overlap`, `itr_step_overlap`) |
| intersection 集合运算 | marktree.c:800-1022 |
| 上层批量清除 | extmark.c:200-253 (`extmark_clear`) |
| gravity 端点归属 | extmark.c:382-387 |
| 混合坐标换算 | marktree.c:81-110 (`relative`/`unrelative`/`compose`) |
| 渲染合成 | decoration.c:730-792 (`decor_redraw_col`), decoration_defs.h:50-52 (`hl_mode`) |
