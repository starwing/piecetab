# 讨论：spantree 的身份 / 搜索高亮失效（discuss_spantree_id）

> 状态：**设计讨论中，不是最终方案**。
> 本会话目标是把设计问题讨论清楚，最后写 handoff 文档交给另一个会话实现。
> 本文件记录当前讨论状态、已确认方向、开放问题和已知约束。

## 0. 动机：为什么讨论这个

讨论这个是为了支持 **editor.lua 的搜索功能开发**：

- `/`、`?`、`n`、`N` 搜索要接入 `textmatch.State`；
- 遵守约束：**不在 render 里搜索**；搜索结果**只存在 spantree 的 `search` 层**，
  不建 Lua 侧搜索列表；
- `n` / `N` 跳转并把所有命中写进 `search` 层；`:nohl` 清树；
- **编辑了高亮文本后，必须删除对应的搜索 mark**。例如搜 `abc` 后在 `b` 后插入
  `x` 变成 `abxc`，原来 `abc` 的高亮必须消失，其他高亮保留。

search 给 spantree 的需求文档是：

- **`notes/design_spantree_search_invalidate.md`**（Status: proposed，给 spantree C
  开发的需求规格）
- 另可参考 **`notes/plans/plan_textmatch_editor_search.md`**（搜索接入 plan，含
  当前 API 能力核查与新增 API 建议）

### search 需求要点（来自 `notes/design_spantree_search_invalidate.md`）

1. **每个 mark 有独立身份（mark handle）**
   - 新增 `Tree:add_mark(ns, attr, off, len) -> mark_id`；
   - `mark_id` 在树生命周期内稳定；与 attr id 解耦；同一样式可以有多个不同
     `mark_id`。
   - 新增 `Tree:remove_mark(mark_id) -> count`，精确删除单个 mark，不影响同
     attr 的其他 mark。

2. **编辑失效原语**
   - 新增 `Tree:invalidate(ns, off, del) -> removed`，必须在 `tree:splice` 之前
     调用（基于编辑前坐标）。
   - 语义（建议保守策略）：删除与 `[off, off+del)` 相交、或边界正好贴住删除
     起/终点的完整 mark；纯插入按“严格包含插入点或末尾正好在插入点”处理。
   - 删除的是**完整 mark**，不是裁剪区间。

3. **（可选）`splice` 返回受影响 mark**，或提供 `splice_ex`；第一版若已有
   `invalidate` 可后续增强。

4. **修复 `unmark` 死循环**：单 namespace 单 mark 时 `t:unmark(id)` 死循环。

5. **非目标**：不在 Lua 侧维护搜索结果列表；不写 Lua 垫片绕过 C API。

## 1. 背景与问题

- `spantree.h` 是 B+ run 树：把 bytes 范围划分成 span，每个 span 存一个 `sp_Id`。
- `spantree.h` 不关心 id 的“含义”；id 是什么由 arbiter / 绑定层决定。
- 当前 `lua/spantree.c` 绑定层假设 **id == attr**：同一个 intern attr 返回同一个表现 id。
- 搜索高亮需要 **“同样式、不同身份”**：每个搜索匹配是一个独立 mark，但共享同一个视觉 attr table。
- 现有绑定层表达不了这种“同样式不同身份”的情况。

已确认的方向（仅方向，不是设计）：

- **某些 namespace 需要声明自己需要“身份 id”这个功能**；不是所有 ns 都需要。
  目前只有 search 需要。
- 对声明了身份 id 的 ns，同一个视觉 attr table 下可以有多个不同身份的 mark；
  即“同样式、不同身份”是必须支持的。
- 身份必须存在于 C 核心，不能做成 Lua 侧 mark 注册表。
- `spantree.h` 确实需要修改：目前只有 born/died 通知，缺少 **“改”（id 存活但长度/位置变化）** 通知。

**API 形式没有确定。** search 需求文档
（`notes/design_spantree_search_invalidate.md`）里提出的
`add_mark` / `remove_mark` / `invalidate` 只是需求侧的例子/候选，
不是已确认的设计。最终 API 是什么、叫什么、是否新增方法，还是 ns 声明模式后
复用现有 `mark` / `clear` / `splice`，都还没有定。

## 2. 用户明确纠正过的事项

- **不接受 Lua-side mark 注册表**。身份必须由 C 核心持有。
- **“设计分叉不存在”**：`spantree.h` 一直存 id；问题只是绑定层把 id 默认成 attr。需要让绑定层支持“一个 attr table 对应多个身份 id”这件事，但具体 API 未定。
- **`design_marktree` 是过时/否定的文档**，已移到 archive，不再作为设计依据。
- **remove_mark 即使有 mark 数组，也要通过 ns 扫树**：树是位置索引，不是 id 索引；id->span 数组只是缓存，且必须靠编辑通知保持准确。数组不是 remove 的充分条件。
- **不要急着写代码**。当前最多是方向，不是方案；本会话目的是讨论清楚后写文档交给其他会话。
- 用户还指出：讨论要有章法，不要给不可推敲的结论，也不要问一堆不需要展开的问题。

## 3. 当前设计分层（讨论框架）

建议按以下四层分开讨论：

- **A. 身份与 id 分配**（`lua/spantree.c` 内部）
- **B. 树变更通知**（`spantree.h`）
- **C. 删除 / 失效操作**（基于当前树扫描）
- **D. API 形状**（暴露给 Lua 的接口）

## 4. 各层现状与开放问题

### A. 身份与 id 分配

现状：

- `lua/spantree.c` 的 plain id 域是 `[0, CP_COMP_START)`，`CP_COMP_START = 1 << 24`（约 16.7M）。
- `cp_State.next` 是 `unsigned`，attr id 和 op id 共享这一个递增计数器。
- `attrs` 和 `ops` 是 dense vector，长度跟着 `next` 走。
- plain id 目前**没有回收机制**（composite 区有 `freehead`，但普通区没有）。
- `spantree.h` 的 `sp_Id` 是 `size_t`（64 位），树本身不在乎 id 数量。

开放问题：

- mark id 是否应该有独立 id 域 / 独立分配器，而不是和 attr/op 共用 `next`？
- mark id 是否需要回收（free list）？搜索高亮是高频增删，不回收会泄漏到撞上 `CP_COMP_START`。
- 每个 mark id 的 C 记录最小形态是什么？（例如 `mark_id -> { ns, canonical_attr_id }`）
- 是否可以为每个 mark 不建 Lua table，只建一个小 C 记录？
- per-ns opt-in 怎么表达：新 API `add_mark` 还是 ns 声明“identity mode”？

### B. 树变更通知（`spantree.h`）

现状：

- 已有 `sp_Arbiterf`：`sp_Id sp_Arbiterf(void *ud, sp_Id id, sp_Id old, sp_Mask *mask)`。
- 已有通知：`arb(id, 0)` = born，`arb(0, id)` = died，`arb(in, old)` = 合并/变换。
- **没有**“id 存活但长度/位置变化”的通知。

开放问题（通知形式，`Editf` 不是唯一解）：

1. 单一 Editf 回调 + event enum？
2. 多个 typed 回调（born / died / resized）？
3. 直接复用 arbiter：
   - 3a. 把 resize 模拟成 died + born（不新增 API，但语义过载，composite refcount 风险高）；
   - 3b. 扩展 arbiter 签名携带长度/位置信息（breaking change，且把通知和变换混在一起）。
4. 变更日志（journal）：mutation 后绑定层读取受影响 runs 列表。

需要权衡：API 面大小、语义清晰度、重入约束、composite id refcount 安全、对现有 `lst_arb` 的影响。

### C. 删除 / 失效操作

基于“树是唯一事实来源”的候选操作：

```text
remove_mark(mark_id):
    扫该 ns 的 span，找到含该 mark_id 的段
    对该段执行 clear(ns)（或清除该 id）

invalidate(ns, off, del):
    splice 前扫 [off, off+del) 相交的 mark
    对每个相交 mark 整段删除
    纯插入（del=0）不失效，只平移
```

开放问题：

- 单次 `remove_mark` 是否接受 O(n) 扫该 ns？是否需要 id 索引作为后续优化？
- mark 数组 / id->span 缓存是否必要？目前倾向：**不是语义必需**，只是性能优化。

### D. API 形状

当前约束：

- 普通 ns：`mark()` 行为不变（同 attr 同 id）。
- identity ns：新增 `add_mark()`，每次返回新 mark id。
- `span` 对 identity ns 返回 mark id。
- `unmark(mark_id)` 删除单个 mark。
- `clear(ns)` 批量清空。

具体签名和是否引入 ns 模式，等 A/B/C 定后再定。

## 5. 已知的当前绑定层 bug（研究过程中复现）

- 单 ns 单 mark 时 `t:unmark(id)` 会卡死/超时，根因在 `lua/spantree.c` 的 `lst_unmark_apply`（约 line 1506）清掉唯一 layer 后的处理。
- `span("s",1,1)` 对 mark `[0,3)` 返回 `[0,2)`，不是完整 mark。
- `clear("s",6,1)` 对 `[5,8)` 会切成 `[5,6)+[7,8)`。
- `splice(3,0,2)` 对 `[0,3)` 会延长成 `[0,5)`。
- 两个同 attr mark `[0,3)`、`[5,8)`，`splice(3,2,0)` 删掉 gap 后会合并成 `[0,6)`——这是“同样式同身份”导致的合并；如果每个 mark 有独立 id 就不会合并。

## 6. 关键代码位置

- `spantree.h:58`：`typedef size_t sp_Id;`
- `spantree.h:62`：`typedef sp_Id sp_Arbiterf(void *ud, sp_Id id, sp_Id old, sp_Mask *mask);`
- `spantree.h` 中 `spA_died` 在合并/删除时调用：`spD_mergeleaf`、`spD_seambound`、`spF_appendspan`、`spF_fillrt` 等。
- `lua/spantree.c:71`：`#define CP_COMP_START ((sp_Id)1 << 24)`
- `lua/spantree.c:136`：`unsigned next;`（plain id 计数器，无回收）
- `lua/spantree.c:412`：`cp_internattr`（attr id 分配）
- `lua/spantree.c:495`：`cp_op`（op id 分配）
- `lua/spantree.c:1506` 附近：`lst_unmark_apply`（unmark 死循环根因）

## 7. 下一步

- 换一个 **scope 更小**的场景来聚焦设计讨论（当前讨论范围太大，需要缩小）。
- 在讨论清楚后，把最终设计写成 handoff 文档交给其他会话实现。
