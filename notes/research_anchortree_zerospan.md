# anchortree 零长尾 span / 左边缘坐标模型调研

> 范围：只分析“树中段尾”问题的根因与三个候选改法，不改实现。
> 基线：当前 `anchortree.h`（左边缘模型之前）、`notes/design_anchortree.md`
> 的 edge/游标/删除/插入契约。结论是调研意见，不是设计定案。

## 0. 结论摘要

1. 当前“树中段尾”不是 bug，是 edge 定义为 **span 右边缘** 的直接后果：
   指向 middle edge@`p` 时 `poff == edge->bytes`，与树尾同形，因此
   `poff >= bytes[i]` 不能用来判断树尾。
2. 把 edge 改成 **span 左边缘**、只允许 **最后一个 span 为 0 长** 的模型
   （方案 A）在语义上可行：
   - 中间 span 全部正长，精确边界表示为下一条 edge 的 `poff == 0`；
   - 0 长只出现在尾哨兵，`poff >= bytes[i]` 恒等价于“在尾哨兵/树尾”；
   - `atK_findleaf`/`atK_forwardoff`/`atK_backwardoff` 可以完全回到
     spantree 的 `>=`/`>` 比较，不再需要当前的 `>` 特例；
   - `origin` 与 `AT_ITER_ORIGIN` 可以删除：空树/位置 0 由树内尾哨兵
     统一表达。
3. 但方案 A 是**坐标模型级重写**，不是局部补丁。风险集中在：
   - 空树/`at_newtree`/`at_reset` 必须有尾哨兵，涉及生命周期与 OOM 契约；
   - B+ 删除/再平衡必须把“尾槽 0 长、首槽 0 边界、尾槽不可删”作为
     显式豁免，并证明 split/merge/underflow 不会把尾哨兵移到非最后位置；
   - 插入/删除的“合并方向”相对当前实现整体反转；
   - 大量测试与 `at_checktree` 断言需要升级。
4. 方案 B（把 0 长哨兵放到树外，类似当前 origin 的“end 哨兵”）保留
   全正长 B+ 树，风险较低，但重新引入第二个 phase 边界，游标状态比 A 多。
5. 方案 C（0 长改 1 长，用 `poff > T->bytes` 判尾）不干净：尾哨兵起点
   `poff == 0 < bytes == 1`，反而无法用 `poff >= bytes` 判尾；不建议。
6. 方案 D（保持现状）继续携带 middle-tail 特例，且与 spantree 的
   `findleaf/advance` 永久存在 `>`/`>=` 分歧。

**推荐**：若确定要消除 middle-tail，选 A 并作为独立重构推进；若当前
风险预算不足，选 B 作为低风险过渡；不建议 C。

## 1. 问题定义：middle-tail 与快速判尾

spantree/piecetab 的共同不变式是“树中不得处于段尾态”：
`paths` 指向段 `i`、`poff == bytes[i]` 时，段 `i` 必须是整树最后一段。
这样 `poff >= bytes[i]`（或 `poff > bytes[i]`）就是树尾判定，树中任意
位置都走统一算术。

anchortree 当前模型：

- edge@`p` 是它左侧 gap 的**右边缘**；
- gap 长度为 `edge->bytes`，`p = 前一个 edge/origin 的坐标 + bytes`；
- 指向 edge@`p` 的精确表示是 `off + poff == p` 且 `poff == bytes`；
- 因此 `p < T->bytes` 的中间 edge 也会出现 `poff == bytes` —— 这就是
  **树中段尾**。

它有两类可观察影响：

- `atK_findleaf` 不能照抄 spantree 的 `>=`，必须用 `>` 把精确位置留在
  左侧 edge，否则 `at_next` 会漏掉/错扫该坐标上的 anchor；
- `atK_forwardoff` 必须用 `d <= in`、`d > bytes[i]` 等适配比较；
- 只有把坐标绝对值与 `T->bytes` 比较才能判尾，不能只看当前槽。

## 2. 方案 A：左边缘 span + 只允许尾 0 长

### 2.1 模型定义

树是覆盖 `[0, T->bytes]` 的 span 序列：

- 每条 edge 是它右侧 span 的**左边缘**（起点）；
- edge `i` 的起点 `s_i` = 它前面所有 slot 的 `bytes` 之和；
- `bytes[i] = s_{i+1} - s_i`（最后一条 = `T->bytes - s_last`）；
- 不变量：只有最后一条 edge 允许 `bytes == 0`（尾哨兵），其余 `> 0`；
- 尾哨兵位置恒为 `T->bytes`；它可以是空的（没有 anchor），且永不删除。

### 2.2 游标表示

给定坐标 `q`：

- `q < T->bytes`：找 edge `i` 使 `s_i <= q < s_i + bytes[i]`，
  `off = s_i`、`poff = q - s_i`；精确边界 `q == s_i` 落在 edge `i`
  且 `poff == 0`。
- `q >= T->bytes`：落在尾哨兵，`off = T->bytes`，
  `poff = q - T->bytes`（`q == T` 时 poff 0）。

于是：

- 中间 edge 永远有 `0 <= poff < bytes[i]`，不可能段尾；
- 尾哨兵 `bytes == 0`，`poff >= 0` 恒真，天然就是“树尾”；
- 不再需要 `origin` 与 `AT_ITER_ORIGIN`；空树/位置 0 由首条 edge
  （即尾哨兵）表达。

### 2.3 导航：可以完全回到 spantree 形状

关键的隔离规则是：

> `at_locate`/`at_advance` 遇到 `pos >= T->bytes` 一律走尾哨兵路径
> （`atK_locend` 或等价逻辑），不把 tail slot 交给 `atK_findleaf`。

这样：

- `atK_findleaf(C, l, &poff)` 可以对 `*poff >= bytes[i]` 使用 spantree
  的 `>=`；对 `pos < T->bytes`，因为前面正长 span 的总和 > pos，
  循环不可能消费到 tail slot，必然停在某个正长槽。
- `atK_forwardoff` 可以回到 spantree 的 `d >= bytes[i]`；目标
  `pos < T->bytes` 保证不会越过 tail slot。
- `atK_backwardoff` 可以回到 spantree 的 `d > bytes[i]`；从尾哨兵
  `poff == 0` 出发时，第一次迭代的候选就是它前面的正长槽，tail 本身
  不参与算术；移动到坐标 0 会自然停在首 edge（`poff == 0`），不需要
  当前 `l < 0` 的 origin 特判。
- `atK_locend` 对尾哨兵：`off = T - bytes == T`、`poff = 0`；
  虚拟尾继续 `poff += pos - T`。
- 精确边界扫描：`at_next` 只需在入口加一条规则：
  - `poff > bytes[i]` → 虚拟尾，done；
  - `poff > 0`（在正长 span 内部）→ 先 `atK_nextedge` 到下一条 edge
    再扫；
  - `poff == 0` → 扫当前 edge（anchor 在该边界上）。
  这比当前的“origin phase + 当前 edge 两种含义”更单一。

### 2.4 anchor / at_range

- anchor 仍按 gravity 链接到 start edge / end edge 的 left/right 桶；
- `atO_edgepos` 从“累加 `i <= idx`”（右边缘坐标）改为“累加
  `i < idx`”（左边缘起点坐标）；尾哨兵特殊返回 `T->bytes`；
- `at_newanchor` 的 `start/end` 边界创建改用左边缘 `atI_insertedge`：
  - `pos == 0` 返回首 edge；
  - `pos == T->bytes` 返回尾哨兵；
  - `pos` 在 span 内：拆分 span，在 `pos` 插入新 edge；
  - `pos > T->bytes`：延长尾哨兵到 `pos`，再追加新的 0 长尾哨兵。
- 删除 anchor 后空掉的**中间** edge 仍应删除（合并到前一条 span，见
  2.5）；首 edge 与尾哨兵是不可删哨兵。

### 2.5 插入 / 删除：方向反转，但语义可能更短

左边缘模型下，span 的“合并方向”与当前实现相反：

- 插入 `ins` 在 `p`：
  - 先确保 `edge@p` 存在（span 内拆分时，旧 span 变
    `[s, p)`，新 edge@p 接管 `[p, s+L)`）；
  - `edge@p.bytes += ins`（新文本属于从 `p` 开始的 span）；
  - 在 `p + ins` 建新 edge，把 RIGHT 桶迁过去；
  - `p == T` 时就是“尾哨兵变正长 + 追加新尾哨兵”，与当前
    `atI_insertempty` 同构，可取消 origin 特例。
- 删除 `[L, R)`：
  - 确保 `edge@L`、`edge@R` 存在（`R == T` 时 `edge@R` 就是尾哨兵）；
  - 把 `(L, R]` 上所有 edge 的桶迁移到 `edge@L`（每个 anchor 端点
    恰好迁移一次）；
  - 删除 `(L, R]` 这些 edge slot；
  - `edge@L.bytes = 旧 edge@R.bytes`：保留下标 R 之后的后缀 span，
    `T->bytes -= del`；`R == T` 时 `edge@L.bytes = 0`，它自动成为
    新尾哨兵；
  - 严格位置 `> R` 的 edge 不变，坐标因前缀和减少而自然左移 `del`。
  这一语义比当前 `atD_prepwindow + movechains + cutrange + stitch`
  更接近“移动一段桶 + 改一个 span 长度 + 删若干 slot”，有减码空间。
- anchor 的 gravity 语义保持不变：LEFT 槽留在原边界，RIGHT 槽迁到
  `p + ins`；删除时 `(L, R]` 全部塌缩到 `L`。

### 2.6 B+ 结构：0 长尾槽的豁免清单

真正需要逐点审计的不是搜索，而是结构维护：

1. **尾哨兵位置**：任何 split/merge/rotate 后，0 长槽必须仍是最右
   叶子槽；它可能随右节点搬移，但不得出现在中间。
2. **首 edge / 尾哨兵不可删**：`atD_deleteedge` 对 `pos == 0` 与
   `pos == T->bytes` 必须保留边界；中间空 boundary 删除时把 `bytes`
   并入**前一条** edge（当前实现是并入后一条）。
3. **underflow/merge**：空树只有尾哨兵时，root/leaf 可能只有一个
   0 长子槽；`atN_min`、rebalance、stitch 不能把它删掉或触发合并死循环。
4. **split**：`atI_splitnode`/`atI_splitroot` 按 child count 切分，0 长
   尾槽只会落在右侧节点最后；`atN_sumbytes` 加 0 不破坏度量。
5. **插入断言**：`atI_insertat` 当前 `assert(rem < n->bytes[i])` 对
   0 长尾槽不成立；必须确保只有 `pos < T->bytes` 才走该路径。
6. **删除范围**：`atD_rmrange`/`atD_cutrange` 不能把尾哨兵计入待删
   slot；`R == T` 的处理应显式走“merge tail into L”。
7. **生命周期**：`at_newtree`/`at_reset` 必须保证尾哨兵存在：
   - 方案 A1：`at_newtree` 分配一个 edge 作为哨兵，失败返回 NULL；
   - 方案 A2：惰性创建，空树先保持 `cc == 0`，首次 anchor/insert 时
     创建；这会让“空树”与“只有哨兵的空树”两个状态并存；
   - `at_reset` 可先 detach 哨兵，清空后再挂回，避免 void API 里
     OOM。
8. **测试/检查器**：`at_checktree`/`at_checkchains` 需增加
   “tail slot may be empty, must be last, exactly one, never removed”、
   “no other empty/middle edge”断言；现有大量按 `T->bytes==0 => cc==0`
   的测试需要同步。

## 3. 方案对比

| 方案 | 树内 0 长 | middle-tail | 游标 phase | B+ 正长不变式 | 改动量 | 主要风险 |
|---|---|---|---|---|---|---|
| A 左边缘 + 尾哨兵 | 仅尾 1 个 | 无 | 无（可删 origin） | 破坏（尾豁免） | 大 | sentinel 生命周期、B+ 豁免、测试面 |
| B 左边缘 + 树外 end 哨兵 | 无 | 无 | HEAD/TREE/END | 保持 | 中大 | 第二哨兵/phase、end 随 T 移动 |
| C 0 长改 1 长 | 尾 1 个正长伪哨兵 | 无 | 需特判 | 保持 | 中 | `poff>=bytes` 判尾失效，语义不干净 |
| D 保持现状 | 无 | 存在 | ORIGIN/TREE | 保持 | 0 | 与 spantree 永久分歧、特例 |

方案 B 的简述：树仍用左边缘正长 span；位置 0 用 origin/head 哨兵，
位置 `T->bytes` 用树外 end 哨兵；`at_next` 依次扫 head、树、end。
它不用 0 长树内槽，但要维护“end 哨兵随 T 增长/收缩时转成树内 edge、
旧 edge 转成新 end”的迁移，且空树时 head/end 同为位置 0 的哨兵，
状态数不比 A 少。用户提出的“把 0 长放到 origin 位置并改名 end”若指
这个方向，落地时需先定义 head/end 的重合/分离规则。

## 4. 验证计划与 kill criteria

若启动方案 A 原型，建议顺序：

1. 先改模型层：edge 起点坐标、`atO_edgepos`、`atK_findleaf` 回 `>=`、
   `at_locate`/`at_advance` 尾哨兵路径；用现有 `at_checkcursor`/fuzz
   校验游标与模型位置。
2. 再改 `at_next`：删除 origin phase，加入 `poff > 0` 跳 edge 规则；
   用 `iter_*` 测试与 fuzz 的模型迭代对比。
3. 再改 insert：左边缘拆分/尾扩展；用 split/OOM 测试。
4. 最后改 delete：桶迁移 + span 继承 + 中间 slot 删除；这是风险最高
   的一步，必须用 `at_checkchains`、fanout4/8、OOM 注入全量跑。
5. 全量闸门同当前：`just at`、`just at-cov` 行 100%、`just f::at`
   1–10 + `at-dbg`、clang-format、C89/C++03。

出现以下任一情况就应停止方案 A，回到 B 或 D：

- 空树哨兵的 OOM/生命周期无法在不破坏 API 的前提下成立；
- B+ underflow/split 需要把“尾槽特殊”扩散到超过 2–3 个结构 helper；
- 删除 span 继承无法保证后缀位置不变（`at_range` 回归）；
- `at_next` 的 phase 规则比现在的 origin phase 更复杂；
- 测试改动量超过实现改动量。

## 5. 用户决策（2026-09-16）

1. **空树不设尾哨兵**：尾哨兵的唯一职责是承载位置 `T->bytes` 上的
   point；有 point 就不是空树，所以空树没有 edge，`at_newtree`/
   `at_reset` 不分配哨兵。
2. **尾哨兵按需存在、可删除**：位置 `T` 出现 point 时创建 0 长尾
   edge；该 edge 的桶为空且没有 point 需要承载时删除它；删除后
   若 `T > 0`，尾坐标由最后一条正长 span 的 `poff == bytes` 表达，
   若 `T == 0` 则回到无 edge 的空树。
3. **选 A，不选 B/C**：既然尾 0 可实现，直接实现左边缘 + 按需尾 0
   哨兵模型。
4. 由此细化的不变式：
   - 空树：`root.cc == 0`，无 edge；`T->bytes == 0`。
   - 非空树：位置 `0` 有首 edge；首 edge 可以没有 point，但携带
     到下一个边界的正长 span（它是 0 边界的表达，不在桶空时删除）。
   - 中间 edge 必须 `bytes > 0`；只有尾哨兵 `bytes == 0`。
   - 尾哨兵若存在则必须是最后一个叶子槽；桶空时可删除，删除后
     尾坐标改用最后正长 span 的 `poff == bytes` 表达。
   - 树中段尾不存在：中间 span 的 `poff < bytes`；`poff >= bytes`
     仅在尾哨兵（`bytes == 0, poff == 0`）或最后正长 span 的树尾
     （`poff == bytes`）成立。
   - 首 edge / 中间 edge 的空 boundary 删除走“并入前一条 span”；
     首 edge 不因桶空而删除；尾哨兵只在桶空时删除。

