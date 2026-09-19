# anchortree API 重塑调研（research_anchortree_reshape.md）

> 日期：2026-09-16。
> 范围：只讨论 API 模型与归属，不改 `anchortree.h` / `notes/design_anchortree.md`
> （另一 agent 正在改工作树）。本文件是调研与方案对比，不是设计定案；推荐意见
> 经用户确认后才写进 `design_anchortree.md`。
>
> 基线：工作树 `anchortree.h`（1306 行）、`design_anchortree.md` §八/§九、
> `design_compositor.md` §1.3/§7/§12.5、`design_spantree.md`（结果 id 模型）、
> `notes/archive/research_marktree.md`、`notes/plans/plan_anchortree_refine.md`
> 的行数目标。未运行测试。

## 0. 结论摘要（待确认）

1. `at_next` 从 150 看不到 `[100,200)` 的 anchor，根因是它被定义成
   **按 edge 坐标序的 START 迭代**，不是区间查询。渲染/敏感需要的是
   overlap（区间相交）或 stab（点覆盖）；补 `at_prev` 不能解决这个问题。
2. “当前 edge 是否被覆盖”不是一个布尔量：同一 gap 可被多个 anchor 覆盖。
   即使维护了覆盖状态，向前遇到的第一个 END 也未必属于覆盖当前点的
   anchor（反例见 §2）。要拿到 id，必须有 active 集合或区间索引。
3. 逐 edge 物化覆盖（spantree fill/clear 式）确实 O(k) 且代码复杂，应拒绝；
   但“维护覆盖”不等于只有物化一条路：**按 start 排序 + 子树 max-end**
   的区间索引可以 O(log n) 更新、O(log n + k) 查询。这正是
   `design_compositor.md` §1.3 候选 A 的 per-ns start-order 树 + 子树 `best`。
4. 推荐分工：`anchortree.h` 保持“位置真值 + 端点 + gravity + splice”；
   overlap/cover 与 sensitive 判定放 compositor 的 per-ns 索引。
   anchortree 本轮只补：
   - 文档明确 `at_next` 是 start 序迭代，不承诺 overlap；
   - 端点级迭代（返回 id / START-END 角色 / 端点位置），供
     compositor 维护索引、做 active-list sweep、调试；
   - gravity 边界下的“touch 语义表”，作为 sensitive 的公共契约；
   - 评估 cursor epoch、`at_prev`、批量删除的取舍。
5. 若坚持 anchortree 原生 sensitive，就等于要求 anchortree 自己拥有区间
   索引；或退化为 O(#sensitive) 每次 splice 扫描。两条路都要用户明确
   复杂度契约，不建议默认走。

## 1. 问题定义：三类需求不能混为一谈

设 anchor 的当前有效区间为 `[s, e)`（`at_range` 返回值；零长 `s == e`，
交叉 `s > e`）。对区间 `[a, b)`：

- **overlap**：`s < b && e > a`（渲染一个 viewport 需要它）；
- **stab / cover**：`s <= p && e > p`（从某一点开始找“谁覆盖我”）；
- **inside / containment**：`a <= s && e <= b`（对象完全在查询范围内）；

外加 fourth：

- **touch**：一次 `splice(L, del, ins)` 是否改变了 anchor 覆盖的字节集合
  （sensitive 语义）。

当前 `at_next(C, ns, plen)` 是 **START 槽的 edge 坐标序迭代**：
对一个 edge，先扫 left 桶再扫 right 桶；只产出 `is_end == 0` 的槽，
END 槽仅用于推进链表；扫完当前 edge 后 `atK_nextedge`。因此：

- START 在 cursor 之前的 anchor 永远不会被产出；
- 从差距 gap 内的位置 `p` 调用，当前 edge 是 `>= p` 的第一个 edge；
- 例：`[100,200)` 的 START 在 edge 100；从 `p=150` seek 时当前 edge 是
  200，`at_next` 只会从 200 的 START 桶开始，`[100,200)` 完全不可见。

`at_prev` 缺失是另一个问题（反向遍历的对称性），不是 overlap 问题的根因；
一个“上一 START”接口同样看不到 `[100,200)`。

### 1.1 查询语义的开放点

- 零长 anchor：不覆盖任何字节；stab/overlap 是否仍要在其精确位置命中？
- 交叉 anchor（`s > e`）：`at_range` 原样返回，`at_next` 的 `plen` 当 0。
  overlap/stab 应视为空集，还是按某种 normalize 处理？
- 边界：`s == p`、`e == p` 在插入/删除时受 gravity 左/右侧影响；查询语义
  必须和 splice 的移动规则一致，否则 compositor 的索引会漂移。

这些语义必须在 reshape 目标里先定，否则无论索引放哪都会错。

## 2. 为什么“当前 edge 已覆盖 + 向后找 END”不充分

方案 1 的直觉是：若当前 gap 被某个 anchor 覆盖，则从当前位置向后遇到的
第一个 END 就是该 anchor 的 END，拿到这个 end 的 id 就能拿到 start。
这对**互不重叠**的 anchor 成立，但 anchortree 允许重叠（compositor 的
object ns 也明确支持重叠、嵌套、部分相交）。反例：

```
A = [0, 100)  覆盖 p=50
B = [60, 70)  在 A 内部，start > p
```

从 50 向后，第一个 END 是 B@70；B 的 start=60 > 50，B 不覆盖 p。
A 的 END 在 100，但它不是第一个 END。仅凭“第一个 END + 它的 start”，
会把 B 误判成覆盖 p 的 anchor。要正确回答，必须：

- 维护“p 处的 active anchor 集合”，或
- 用区间索引按 `start <= p && end > p` 做 stab。

### 2.1 覆盖计数也不够

可以只在 edge 上维护一个差分量（START +1 / END −1），B+ 树节点聚合
前缀和；这样“p 是否被覆盖”是 O(h·fanout) 的，插入/删除 anchor 也只需
改两个边并上行 O(h·fanout)——**不是 O(k)**。但它不能给出 id：

- `count(p) > 0` 只说明有覆盖者；
- 向前扫 END 时，仍要把 start 在 p 之后的 END 逐个排除；
- 上例中若 A 很长、内部有 n 个短 B_i，向后要扫过 n 个 B_i 的 END 才能
  到达 A 的 END，复杂度 O(n)。

结论：要 id 就要区间索引；计数只能做布尔优化。

### 2.2 物化覆盖 vs 区间索引

- **物化覆盖**：每个 edge 挂 active list，或按 byte 写 run。更新范围跨越
  k 个 edge 就是 O(k)，这就是 spantree fill/clear 的开销来源。拒绝。
- **区间索引**：每个 anchor 只在它的 start 处登记一次；子树维护
  “本子树 start 的最大 end 指针/序键”。uniform shift 不改变相对序，
  不需要重写；只有端点真正迁移/塌缩的 anchor（以及 seam 组重排）
  需要更新。查询 O(log n + k)，更新 O(log n + 输出内 anchor 数)。
  这是唯一能同时满足 overlap 查询和复杂度承诺的方向。

## 3. 现状盘点与归属

### 3.1 anchortree 现状

- `atO_scan` 可同时看到 START/END 槽，但公共 `at_next` 只吐 START；
- `at_range` 可拿单个 anchor 的当前 start/end，但 O(层级·fanout)；
- 没有任何子树聚合（max end / min start / 计数）；
- `at_splice` 是 delete + insert，删除把 `(L, R]` 内的端点塌缩到 `L`；
- 设计边界明确写“不负责与 compositor / spantree 的同步”；
- `notes/plans/plan_anchortree_refine.md` 的目标是把 1306 行压到
  ≤1375（理想 ≤1240），并重写大量结构 helper。此时往主树里塞新的聚合
  维护，会和该重构正面冲突，行数与 bug 面都不可控。

### 3.2 compositor / spantree 现状

- `design_compositor.md` §1.3 候选 A 已经设计了 object ns 的位置索引：
  共享 AC（anchortree 的位置真值）+ **per-ns start-order 树**，节点保存
  子树 `best`（当前 end 最大的 object），供 `cp_seek / cp_next` 做
  “下一个与区间相交的对象”。§12.5 把平衡树类型、比较成本、`best`
  维护列为待定。
- 也就是说：**overlap 查询的索引在 compositor 的设计里已经存在**，
  只是还没实现、还没和 anchortree 对齐。
- `spantree.h` 是“结果 id 的字节密铺”，不解释对象、不表达重叠集合；
  `sp_style`/`sp_clear` 的路径是 O(段数) 的。让它承担通用 anchor
  coverage，会把 fill/clear 的 O(k) 和对象语义重新耦合进来，不是
  合适的宿主。它适合承担“已有 attr 的结果合成”，不是区间索引。

### 3.3 marktree 调研的教训

- `notes/archive/research_marktree.md` 明确：pair 区间 + 每节点
  `intersect` 集合是 marktree 复杂度最高、bug 最多的部分，明确建议
  “除非明确需要 O(log n) stab query，否则不建议引入”。
- 同时，Neovim extmark 的渲染不是把覆盖写进树：窗口顶部做一次 stab，
  然后按 `itr_next` 扫 START 事件，在渲染层维护 active list 逐列合成。
- 这说明：**树提供有序端点 + stab，渲染层维护 active 集合**是成熟分工。
  anchortree 当前缺的正是“端点级有序迭代 + stab”；stab 可以由 compositor
  的 `best` 树提供，端点迭代应由 anchortree 提供。

## 4. 方案对比

| 方案 | 覆盖索引在哪 | 更新成本 | 查询成本 | 代码/风险 | 评价 |
|---|---|---|---|---|---|
| A. 物化 per-edge active list | anchortree 或 spantree | O(k)（k=范围内 edge/段） | O(1) 局部 | 高；同 fill/clear | 拒绝：复杂度承诺与状态爆炸 |
| B. compositor per-ns start-order + `best` | compositor | O(touched·log m_ns) | O(log m_ns + k) | 已在候选 A 设计内 | **推荐** |
| C. anchortree 内建区间索引 | anchortree 主树 | O(log n + 迁移端点数) | O(log n + k) | 高；聚合要贯穿 split/merge/stitch | 备选，不建议本轮 |
| D. spantree composite id 承担 | spantree run | O(段数) fill/clear | O(段数) | 已存在但耦合样式 | 只适合有 attr 的已有路径 |

方案 C 若要落地，粗略形态是：

- 每个 START edge / 内部 child 槽保存“本子树 start 的最大 end edge 指针”；
- 插入/删除/迁移 anchor 时沿路径重算 `best`；
- `at_cover(C, ns, id)` / `at_nextover` 用 `best` 剪枝向左/右找；
- 所有结构 helper（split/merge/stitch/rotate）都要携带并重算该聚合；
- 还要定义“多个覆盖者如何迭代输出、ns 如何过滤、零长/交叉如何处理”；
- 预计新增 150–300 行、多层聚合维护和大量新测试，且与当前重构冲突。

结论：除非目标变成“anchortree 必须独立支持渲染/overlap”，否则 C 不是
当前性价比最高的方案。

## 5. sensitive 语义：原生支持意味着什么

sensitive 的定义是“任何导致该 object 内容改变的编辑都触发失效”。它和
overlap/touch 查询同源：要找到“被这次编辑触碰的 anchor”，必须能回答：

- 删除 `[L, R)`：anchor 是否覆盖了 `[L, R)` 中至少一个字节；
- 插入 `(L, ins)`：anchor 是否覆盖了新增的字节；
- 替换 = 删除 + 插入，两阶段分别判定。

仅靠“端点是否在 `[L, R)` 内”不够：

- 删除时 anchor 的端点可能被塌缩到 `L`，但内容没有改变。例：anchor
  `[R, e)` 的 start 在 `R`，删除 `[L, R)` 后它平移到 `[L, e−del)`；
  端点移动了，但覆盖的原文档字节集合不变，不应算 touch。
- 真正被删除内容触碰的 anchor 是 `[s, e)` 与 `[L, R)` 相交的那些，
  包括 start < L < end 的跨越 anchor；要枚举它们必须有 overlap 索引。
- 插入在边界 `p` 时，start/end 的 LEFT/RIGHT 桶决定插入字节是否被包含：
  START 在 LEFT 桶（stay）→ 插入进入 anchor；START 在 RIGHT 桶（迁移到
  `p+ins`）→ 不进入。END 同理但方向相反。这必须和 gravity 表完全一致。

### 5.1 三种落地选择

- **S1（推荐）**：compositor 用 per-ns `best` 树在编辑前/编辑后判定 touch；
  `anchortree` 不新增 sensitive flag。删除动作由 compositor 调
  `at_delanchor` / spantree clear。anchortree 只承诺“位置、端点迁移、
  `at_range` 结果与 touch 判定表一致”。这保留 anchortree 的简单与复杂度。
- **S2**：anchortree 原生 `AT_SENSITIVE` + 内建区间索引。删除可以在
  `at_splice` 内完成，但需要把被删 id 交给调用方（删除 spantree 的
  composite），因此还需要 out-list/callback；索引本身是方案 C 的成本。
- **S3**：anchortree 维护敏感 anchor 列表，每次 splice 对每个 sensitive
  做一次 touch 判定。实现简单，但复杂度是 O(#sensitive · range_cost)，
  且需要避免“已删除列表项”的安全迭代。只有在敏感对象数量有明确上界
  时才可接受。

**不建议 S2/S3 在没有明确复杂度契约时默认采用。** 如果“双方无法对接”
指的是“compositor 无法知道哪些 anchor 被触碰”，S1 的接口是：
compositor 拥有 `best` 树，它自己就是 touch 判定的执行者；anchortree 只
需要给出准确的 `at_range` 和端点迁移语义。

### 5.2 建议写进契约的 touch 判定（草案）

对有效区间 `[s, e)`（正长，`s < e`；交叉视为空，零长视为空）：

- 删除 `[L, R)`：touch ⟺ `s < R && e > L`（标准区间相交）。端点被
  塌缩但覆盖字节未变的 anchor 不算（如 `s == R`）。
- 插入 `(p, ins)`：先按 gravity 计算两端迁移后的有效区间 `[s', e')`；
  touch ⟺ 新增区间 `[p, p+ins)` 与 `[s', e')` 有交。
  等价于逐个边界判断：`p` 在 anchor 内部（`s < p < e`）→ touch；
  `p == s` 当且仅当 START 在 LEFT 桶 → touch；`p == e` 当且仅当 END
  在 RIGHT 桶 → touch。
- `at_splice(L, del, ins)` = 先删除后插入，依次判定；两阶段任一 touch
  即 touch。
- 零长：覆盖空集，删除永不 touch；插入只有在迁移后区间变为包含新增
  字节时才 touch（例如 START LEFT + END RIGHT 会把零长撑成 `[p, p+ins)`）。
- 交叉：按空集处理；迁移后若变为正长并覆盖新增字节，再按插入规则判定。

这张表需要和“删除把 `(L, R]` 端点塌缩到 L”的精确实现逐条对照；
建议以表驱动测试固定，而不是靠实现自然正确。

## 6. API reshape 建议（按优先级）

### 6.1 明确 `at_next` 的边界，不把 overlap 塞进它

- 保留 `at_next(C, ns, plen)` 作为 START 槽的坐标序迭代；在 design 与
  README 中明确：不承诺返回 START 在 cursor 之前的 anchor，不是
  viewport/overlap 查询。
- 渲染与 `cp_seek/cp_next` 走 compositor 的 per-ns `best` 树；不要在
  anchortree 层承诺“从 150 能看到 100..200”。
- 如果未来出现“没有 compositor、直接用 anchortree 渲染”的消费者，
  再评估方案 C 的 `at_cover/at_nextover`，而不是现在默认加入。

### 6.2 新增端点级迭代（本轮最有价值的增量）

建议增加一个公共原语（名字待定）：

```c
AT_API at_Id at_nextpoint(at_Cursor *C, int ns, int *is_end, size_t *ppos);
```

语义草案：

- 与 `at_next` 共用 Cursor 的迭代状态；按 edge 坐标序，每个 edge 先
  left 桶后 right 桶；
- 返回下一个端点槽（START 或 END），`*is_end` 给出角色，
  `*ppos` 给出该端点的 edge 绝对位置；
- `ns` 过滤与 `at_next` 相同：0 = 不过滤，>0 相等，<0 = 参数错误；
- 正常耗尽/参数错误返回 `AT_NONE`；成功后 Cursor 状态已推进；
- `at_next` 可重构为“只收 START 槽”的薄包装（内部 `atO_scan` 增加
  role mask 参数），避免两份扫描逻辑。

为什么需要它：

- compositor 在编辑范围 `[L, R]` 内枚举被迁移的端点，拿到 id 和精确
  位置；`ppos` 是必要的，因为 cursor 在 gap 内时 `at_offset(C)` 是
  seek 位置（例 150），不是当前 edge 位置（例 200）。
- 渲染层若自己维护 active list，也必须按 START/END 事件扫描；
  `at_next` 目前把 END 全吞掉，接不上。
- 调试/校验 active 集合、验证 gravity 边界也需要它。

它**不能**替代 overlap 索引：从 150 开始 `at_nextpoint` 仍不会主动
产出 100 的 START；它只能向前扫端点。跨越 anchor 要靠 compositor 的
stab/`best` 树先找到。

成本：约 20–40 行（若只加枚举逻辑）；但会与另一 agent 正在做的
bucket 结构重写（single-list 等）相互影响，需在其重构落定后基于新
结构实现。

### 6.3 `at_prev` 的取舍

`at_prev` 与本次 overlap 问题不同源，但确实是 API 对称性缺口。当前桶是
单链表，反向步进有三条路：

1. `at_Anchor` 增加 `sprev/eprev`，`at_Edge` 增加桶尾指针；
   维护点：`atO_link/unlink/moveside/clean` 都要同步。内存增加
   （anchor +2 个 `at_Point`，edge +2 个 `at_Point`），代码中等。
2. 不做反向链接，`at_prev` 每次从头扫当前桶找前驱：O(k_bucket) 每步，
   最坏 O(k²) 扫一个 edge；不建议。
3. 暂不提供，等出现真实反向消费者（滚动回看、双向对象迭代）再做。

建议：本轮把 `at_prev` 列入“待评估”，不要因为“没有 at_prev”而把它
和 overlap 查询绑在一起做；先确认消费者和桶结构重构方向。

### 6.4 端点比较/排序原语（供 compositor 索引）

compositor 的 per-ns start-order 树需要比较两个 object 的 start 先后。
当前可用的公开信息只有 `at_range` 的位置数值；动态比较 `(start, id)` 在
uniform shift 下相对序不变，树形仍然有效，但每次比较要 O(h·fanout)。
如果实测成为瓶颈，可考虑增加：

- `at_cmpstart(T, a, b)`（或返回一个 opaque order key）：按 edge 序比较
  start，位置相同按 id 稳定；O(h) 而非位置求和；
- 对应 `at_cmpend` 只在 `best` 维护需要 tie-break 时才有意义。

本轮宜先记录为 API 候选，等 compositor 平衡树选型后再定。

### 6.5 Cursor epoch / 失效语义

当前编辑结构后旧 Cursor 的 `paths` 可能悬空，`at_newanchor`/`at_delanchor`
不维护 Cursor；`at_splice` 只维护传入的 Cursor。compositor 会有多个
迭代器和索引，这是实际风险。建议把 epoch 纳入 reshape 目标：

- `at_Tree` 增加结构版本号；`at_Cursor` 记录取得时的版本；
- 公共导航 API 在版本不匹配时返回明确错误（`AT_ESTALE` 或复用
  `AT_ERRPARAM`），不把悬空路径当有效状态；
- 编辑者自己的 Cursor 由 `at_splice` 在返回前 re-seek。

这不是 overlap 问题的必要条件，但会让 compositor 的索引维护安全得多。

### 6.6 批量删除与 sensitive 清理

如果 S1 落地，compositor 可能一次删除多个 sensitive object。
`at_delanchor` 每次 unlink 要扫桶，逐个删同 edge 的 k 个 anchor 是
O(k²) 最坏。候选：

- `at_delanchors(T, const at_Id *ids, size_t n)` 批量删除，按 edge 分组
  后单趟 unlink；
- 或先收集 ids，再逐个 `at_delanchor`，接受当前成本，等性能数据。

本轮先记录；若 compositor 的 sensitive 队列可能有较大 qClear，建议
给批量版本留位。

### 6.7 `at_splice` 的变更通知（可选，暂不推荐）

另一种消除“端点 id 拿不到”的办法是：`at_splice` 通过 caller 提供的
callback 报告被迁移的 `(id, role, old_pos, new_pos)`。优点是 compositor
不用回扫 `[L, R]`；缺点是：

- 回调发生在结构变更中，重入和失败语义复杂；
- 跨边界 anchor 的 touch 仍需 compositor 自己的索引补充；
- 与 `at_nextpoint` 回扫的复杂度同阶（删除路径本来就要走这些 edge）。

因此建议先不做；若未来测出回扫成本显著，再评估 event/callback 变体。

## 7. 推荐的 reshape 目标（供讨论定案）

1. **职责不变**：anchortree = 坐标/端点/gravity 引擎；compositor 的
   per-ns start-order + `best` = 区间索引与 overlap 查询；spantree =
   styled 结果密铺。
2. **补齐端点视图**：新增 `at_nextpoint`（id/role/pos）；`at_next`
   明确为 START-only 包装；`at_prev` 单独评估。
3. **补齐语义契约**：`design_anchortree.md` 增加 gravity 边界 touch
   表；零长/交叉在 overlap/stab/touch 中的定义；编辑后 `at_range`
   与端点迁移的精确规则。
4. **compositor 侧对接**：
   - `cp_seek/cp_next` 用 per-ns `best` 树回答 overlap/stab，渲染不再
     依赖 `at_next`；
   - 编辑流程：编辑前用 `best` 树找 touch 集合 → 执行 splice → 对受
     影响 object 调 `at_range` 更新 start-order/`best` → 删除 sensitive
     （`at_delanchor` + spantree clear）；
   - 需要时用 `at_nextpoint` 回扫 `[L, R]` 校验/重建端点迁移集合。
5. **暂不加入**：anchortree 内建 coverage、每-edge active list、
   spantree 承担通用覆盖索引。
6. **可选项**：若用户决定 anchortree 必须原生 sensitive，则先做方案 C
   的 `best` 聚合 + `AT_SENSITIVE` 删除协议，再做 S2；不要在 S1 的
   API 上偷偷加半套敏感语义。

## 8. 交付/验收候选（不运行，仅记录）

- `at_nextpoint`：
  - 从 gap/edge/origin/空树 seek 后，START/END 事件顺序与 left/right
    桶一致；
  - `ppos` 等于端点 edge 位置，不是 seek 位置；
  - ns 过滤、耗尽幂等、参数错误不消耗状态；
  - 与 `at_next` 混用时槽恰好产出一次。
- touch 表驱动测试：每种 gravity × {start/end 在 L/R/内部/边界} ×
  {del>0/ins>0} 一条。
- compositor `best` 树：stab/overlap 找到 `[100,200)`（p=150 反例）、
  嵌套重叠、零长/交叉、删除塌缩后重排、sensitive 删除。
- 直线复杂度回归：`at_splice` / `at_nextpoint` 不因 range 内无端点
  的 edge 数产生额外数量级开销（或明确记录同等阶）。

## 9. 待用户决策的问题

1. **索引归属**：接受“compositor 的 per-ns `best` 树负责 overlap/stab，
   anchortree 不做查询索引”吗？还是要求 anchortree 原生支持
   `at_cover/at_nextover`（方案 C）？
2. **sensitive 归属**：接受 S1（compositor 判定 + anchortree 只管
   删除/位置）吗？若坚持 S2/S3，复杂度契约取
   O(log n + touched) 还是 O(#sensitive)？
3. **API 增量**：`at_nextpoint`（返回 id/is_end/ppos）是否确定加入？
   `at_prev` 本轮做还是延后？端点比较 `at_cmpstart` 是否要预留？
4. **语义**：零长/交叉 anchor 在 overlap/stab/touch 中按空集处理，
   是否有反例需求？
5. **范围**：cursor epoch / 批量删除是否纳入本轮 reshape，还是作为
   下一阶段？
