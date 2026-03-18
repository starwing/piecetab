# TODO：piecetab 实施看板（阶段同步）

> 说明：本文件按“当前代码状态”同步，不再只写理想路线。

## 当前阶段结论（2026-02-24）

- 已完成：基础内存池、快照基础、定位/导航主体框架。
- 已完成：`pt_Chars` / `pt_Lines` 回调接口已接入 `pt_State`。
- 进行中：编辑第一阶段——“将 `s,len` 填充进空 `pt_Node`（最多 FANOUT 槽）”。
- 未完成：`pt_insert` / `pt_remove` / `pt_replace` 正式实现。

## 阶段任务（按优先级）

1. 回调与分片契约收敛（进行中）
   - 维持公开回调参数以 `pt_PieceSize` 为上限单位。
   - 保持 80 列签名可读性（必要时通过结构体参数收口）。
   - 明确 `pt_Lines` 的输出契约：`consumed/chars/breaks/ends` 一致。

2. 空节点填充（进行中）
   - 完成 `ptI_fill` 的稳态逻辑：
     - 逐 piece 分片（`PT_PIECE_MAXSIZE` 上限）；
     - 填充 `children/bytes/chars/breaks`；
     - 直接写 `piece->ends`（避免二次拷贝）。
   - 失败路径要求：分配失败可中止并返回已填充数量。

3. `pt_insert` 第一版（待开始）
   - 基于空节点填充接入叶层插入。
   - 先实现“同层插入 + 超限分裂 + 向上增加子树”。
   - root 超限时增高（无 key 提升，仅 child 扩展）。

4. `pt_remove` / `pt_replace`（待开始）
   - `pt_replace` 先按 remove+insert 语义闭环。
   - 后续再做独立路径优化。

5. 事务语义闭环（待开始）
   - `pt_commit` / `pt_rollback` 与 dirty 快照一致。
   - 回滚后 cursor/snapshot 行为需可预测。

## 空间优化专项（新增）

- `pt_Node` 中 `chars[] + breaks[]` 双数组冗余明显，且访问路径相同。
   - 目标：减少 `pt_Node` 中 `chars[] + breaks[]` 双数组冗余。
   - 方案：合并为单一 `metric[]` 数组，并用 bitset 标识该槽位语义（chars 或 breaks）。
- `pt_Piece` 在行内 piece 中过重
  - 目标：减少 `pt_Piece` 在行内 piece 中的空间占用。
  - 方案：如果一个 `pt_Node` 的 `breaks[]` 为 0，则该槽位必定为行内 piece。
         则直接存储 char*，不分配 `pt_Piece` 结构体。

## 验证清单（当前阶段）














