# anchortree remove/stitch 专题报告

> 状态：待解决专题。
> 关联设计：`notes/design_anchortree.md`。
> 参考实现：`spantree.h` 的 `spD_remove/spD_rmrange/spD_stitch/spD_rebalance`。

## 1. 问题背景

当前 `anchortree.h` 的 `at_splice` 删除部分没有使用 spantree.h 的 stitch 删除算法，而是实现了一套独立的“范围枚举 + 端点搬迁 + 整槽删除”算法。

design 的“当前实现偏差”中已记录：

- 删除路径必须按 spantree.h 的 stitch 流程实现；
- `at_State.rt[]` 是 stitch 暂存根区，当前没有使用；
- 当前也没有实现非空节点下溢时的 borrow/merge rebalance。

本报告用于专题推进该问题的修复。

## 2. 当前实现摘要

### 2.1 调用链

```
at_splice(C, del, ins)
  -> atD_delete(T, L, del)
       -> 确保 L edge / R boundary
       -> atD_remove_range(T, L, R, le)
            -> 收集 (L, R] 内所有 edge 位置
            -> 收集这些 edge 上的 left/right 端点数量
            -> atO_reserve 预扩 le 的 left/right tail
            -> atD_relocate_node：把端点全部搬到 le
            -> 按位置倒序 atE_remove_slot 删除整槽
       -> 若 L 是临时空 edge，再 atE_delete 清理
```

### 2.2 关键假设

- `(L, R]` 中的每个 tree edge 都表示“整个 gap 被删除”，因此该 edge 槽应整体消失。
- 删除终点 `R` 若落在 gap 中，会先创建一个临时 `edge@R`，使部分删除变成可整槽删除。
- 删除起点 `L` 若不存在，则创建临时 `edge@L` 作为端点搬迁的“幸存 seam”。
- 删除后若 `L` 仍为空，再以“空 edge 删除”语义清理，即把 bytes 并入后继或收缩 `T->bytes`。

### 2.3 当前算法的本质

它不是标准的 B+ 范围删除，而是：

1. 多次遍历树收集受影响位置/端点统计；
2. 用一个动态分配的 `pos[]` 数组记录待删槽；
3. 先把端点搬到目标 seam；
4. 再逐槽删除。

这种方式能通过当前测试，但没有沿用 spantree 的路径裁剪、临时 root、stitch、rebalance 框架。

## 3. 为什么需要改为 stitch 算法

### 3.1 与设计/项目惯例不一致

design 明确要求删除走 spantree.h 的 stitch 流程，`at_State.rt[]` 也是为此存在。当前实现属于设计偏差，必须修正。

### 3.2 结构健康度

当前删除只处理“空节点被清空后移除”和“root 只有一个 child 时收缩”，没有实现非空节点的下溢合并/借位 rebalance。这会导致删除后树出现稀疏节点，B+ 树的 fanout/高度保证不成立。

### 3.3 算法路径一致性

spantree 的 `spD_remove` 是从左右 Cursor 出发的路径级操作，复杂度由树高和受影响节点数决定，并且删除过程中直接维护 Cursor/path、临时 root 和 stitch。anchortree 的 edge 模型虽然叶子载荷不同（edge 指针而非 span id），但树骨架可以复用同样的路径处理框架。

### 3.4 OOM 原子性

当前删除为了搬迁端点引入了动态 `pos[]` 分配和多次 reserve。改用 stitch 后，可以在操作前统一估算并 reserve 节点/edge/vector 容量，减少中间失败分支；这也符合“OOM 由 reserve 保证，不用防御分支”的原则。

## 4. 目标算法：edge 模型上的 spantree-style remove

### 4.1 整体流程（预期）

```
at_splice(C, del, ins)
  -> reserve nodes/edges/vector capacity
  -> remove(C, del)
       L = C; R = advance(C, del)
       若 L/R 在同一叶内：做局部 trim/remove
       否则：
         spD_trimright(L)
         spD_trimleft(R)
         spD_cutrange(L, R, rt[], diverge_level)
         spD_stitch(L, rt[])
         spD_rebalance(...)
  -> append/insert 阶段
```

与 spantree 的差异在于：

- leaf 槽存的是 `at_Edge *`，不是 span id/mask；
- 删除 text 时，某些 edge 的整个 gap 会消失，需要移除槽并减 bytes；
- 若 edge 上仍有端点需要保留，应先把端点搬到幸存 seam；
- 需要同步维护 `at_Edge.parent/idx` 和 `at_Node.parent/idx`。

### 4.2 要移植的 spantree 组件

| spantree 函数 | 作用 | anchortree 对应/改造 |
|---|---|---|
| `spD_remove` | 入口：按 len 删除 | `atD_remove` |
| `spD_rmleaf` | 同一叶内删除 | 保留并改造 |
| `spD_rmrange` | 跨叶删除，调用 cut+stitch | 改造 |
| `spD_trimright/trimleft` | 裁剪左右边界部分段 | 裁剪 gap/edge 槽 |
| `spD_cutrange` | 从 L/R 路径切出中间到 `rt[]` | 切出整槽/段 |
| `spD_stitch` | 把 `rt[]` 接回原树 | 保留思路 |
| `spD_stitchnode` | 分层把临时 root 内容接回 | 保留思路 |
| `spD_rebalance` | 下溢借位/合并/root 收缩 | 必须补上 |
| `spD_foldnode` | 合并/平衡相邻节点 | 必须补上 |
| `spD_makechain/findroom` | 构造/寻找可容纳空间 | 保留思路 |

### 4.3 端点搬迁时机

删除过程中不能直接把带端点的 edge 槽“扔掉”。需要：

1. 先识别哪些 edge 槽会因文本删除而消失；
2. 把这些 edge 上的 START/END 端点搬到幸存 seam（删除点 `L` 对应 edge）；
3. 更新 `at_Anchor.start/end`；
4. 再删除空槽。

在 stitch 框架下，端点搬迁应发生在槽被从树中摘除之前；搬迁所需的 vector 容量应在删除前一次性 reserve。

### 4.4 `rt[]` 的用途

`at_State.rt[]` 应像 `sp_State.rt[]` 一样作为跨叶删除时从路径上切下来的临时 root 序列容器：

- `rt[0]` 放叶层切出的内容；
- 更高层放对应内部层切出的内容；
- stitch 时逐层合并/插入回原树。

## 5. 必须保持的语义

- 删除 `[L, R)` 后，受影响端点塌缩到同一个幸存 `edge@L`；
- LEFT/RIGHT 侧保持不变；
- 同一 object 两端落到同一 edge 时仍保留 START/END 两项；
- 空 tree edge 不允许在 API 返回后残留；
- `anchor->start/end` 必须始终指向真实存在的、包含对应端点项的 edge；
- `T->bytes` 必须等于最后一个 tree edge 的位置；
- 删除到最后一个 edge 之后时树自然收缩；
- 所有公开 API 返回后树满足 `at_checktree`（测试 helper）与“无空 edge”不变量。

## 6. 需要解决的难点

1. **leaf 槽不是简单 span**：edge 槽的删除语义有两种：
   - “端点没了但文本还在” → `atE_delete`（bytes 并入后继）；
   - “整个 gap 被文本删除” → `atE_remove_slot`（bytes 减去）。
   stitch 流程必须能区分这两种情况，并且不能在第一种情况下误减总长。
2. **端点搬迁与树删除的顺序**：需要保证搬迁不失败；若搬迁成功但槽删除中途失败，树必须仍一致（当前实现用 reserve 保证 vector 不失败，但树节点/edge pool 的容量仍需统一估算）。
3. **内部 node parent/idx 同步**：切出、拼接、合并、借位都会搬动 child，需要像 spantree 一样在每次 slot move 后同步。
4. **下溢 rebalance**：需要实现非空节点的左右兄弟借位/合并，而不仅是空节点清理和 root 收缩。
5. **Cursor/path 维护**：spantree 的删除强依赖 Cursor 的路径；anchortree 的 `at_Cursor` 有路径字段，但当前删除没有使用它。改造后应从 L/R Cursor 路径出发，而不是从 root 反复收集。

## 7. 验证计划

- 移植完成后，现有 `just at`、`just fuzz/at 1`、`just at-cov` 必须通过。
- 增加针对以下场景的测试：
  - 单叶内删除；
  - 跨叶/多层删除；
  - 删除终点落在 gap 中间；
  - 删除整段多个 edge；
  - 删除后 L/R 临时 edge 清理；
  - 非空节点下溢触发 borrow/merge；
  - root 层数收缩；
  - 随机 splice + anchor 模型对拍；
  - OOM：删除前 reserve 失败必须保持树一致；reserve 成功后不应出现防御性失败分支。
- 覆盖率目标：行 100%、函数 100%、分支尽量 ≥95%。

## 8. 开放问题

- 是否把“端点搬迁”也纳入 `rt[]` 切出过程，还是在 `cutrange` 前先统一搬到 L edge？
- 是否将 `atE_delete` 与 `atE_remove_slot` 统一成带 mode 的槽删除原语，避免两个语义混用？
- 若 `L` 是 gap 中间，最终幸存 seam 是 `edge@L` 还是后继 edge 被 `atE_delete` 清理后的形态？当前语义是后者，需要在新算法中保持。
