# spantree 搜索编辑失效需求（供 C 开发）

Status: proposed — 待 C 开发。

Date: 2026-08-30

## 背景

`editor.lua` 要把 `/`、`?`、`n`、`N` 搜索接到 `textmatch.State` 上，并遵守：

- **不在 render 里搜索**；搜索只在命令触发时发生。
- 搜索结果**只存在 spantree 的 `search` 层**，不建 Lua 侧搜索列表。
- `/`、`?` 直接搜索跳转、不写高亮；`n` / `N` 跳转并把所有命中写进
  `search` 层；`:nohl` 清树。
- 编辑了高亮文本后，必须删除**对应的**搜索 mark。例如搜 `abc` 后在 `b`
  后插入 `x` 变成 `abxc`，原来 `abc` 的高亮必须消失，其他高亮保留。

当前 `spantree` API 不足以干净完成“编辑后删除对应 mark”，需要新增能力。
本文档是给 spantree C 开发的**需求规格**。

## 现状 API 与已实测缺口

相关现有 API：

```lua
t:mark(ns, attr_or_id, off, len) -> attr_id
t:span(ns?, off, len) -> iter(off, len, attr, id)
t:clear(ns?, off?, len?)
t:unmark(attr_id) -> count
t:splice(off, del, ins)
```

已实测结论（最小复现，2026-08-30）：

1. **`span` 子区间返回段前缀裁剪，不是完整 mark 区间**
   - mark `[0,3)`，`span("search", 1, 1)` 返回 `[0,2)`；
   - 要拿完整 mark 必须查询到文档尾再过滤。

2. **`unmark(id)` 在“目标 mark 是所在段唯一层”时死循环**
   - 最小用例：一个 namespace、一个 mark、`unmark` 不返回；
   - 当有其他 namespace 的 mark 重叠在同一段时不会触发。

3. **`clear(ns, off, len)` 只裁剪区间，不删除完整 mark**
   - mark `[5,8)`，`clear("search", 6, 1)` 后变成 `[5,7)` + `[7,8)`。

4. **`splice` 边界会腐蚀 mark**
   - mark `[0,3)` 末尾插入：`splice(3, 0, 2)` 后 mark 变 `[0,5)`；
   - mark `[0,3)` 末尾删除：`splice(3, 1, 0)` 后 mark 变 `[0,5)`；
   - 删除紧邻 mark 之前的区间会把 mark 合并/腐蚀（如 `[0,3)` 与 `[5,8)`，
     `splice(3, 2, 0)` 后只剩 `[0,6)`）。

5. **没有编辑回调 / 受影响 mark 返回**
   - `splice` 不告诉调用方哪些 mark 被位移/删除。

## 需求

### 1. 每个 mark 有独立身份（mark handle）

当前 `mark` 返回 attr id，attr id 是样式 intern 结果，不能唯一标识一个
mark。需要新增：

```lua
--- 新增一个带身份的 mark；返回独立、稳定的 mark_id。
--- mark_id 与 attr id 解耦：同一样式可以有多个不同 mark_id。
---@param ns string?             namespace 名（nil = 无归属）
---@param attr table|integer     attr 表或 attr id
---@param off integer            0-based byte offset
---@param len integer            字节长度
---@return integer mark_id
function Tree:add_mark(ns, attr, off, len) end

--- 精确删除一个 mark；mark_id 不存在时返回 0。
---@param mark_id integer
---@return integer 删除的 mark 数（0 或 1）
function Tree:remove_mark(mark_id) end
```

约束：

- `mark_id` 在树的生命周期内稳定；`splice` 位移 mark 时 `mark_id` 不变。
- `remove_mark` 只删指定 mark，不影响同 attr id 的其他 mark。
- 不要求 `mark_id` 可跨树复用。
- 建议 `mark_id` 为树内递增整数；具体存储（如每树一张 id->span 映射）由
  C 实现决定，但要避免全树扫描 O(n) 删除。

### 2. 编辑失效原语

新增一个“删除与编辑区间相关的完整 mark”的原语，供 editor 在
`tree:splice` **之前**调用：

```lua
--- 删除 ns 层中受编辑影响的完整 mark，返回删除数量。
--- 必须在 splice 前调用（基于编辑前坐标）。
---@param ns string?   namespace 名（nil = 所有层）
---@param off integer  0-based 编辑起点
---@param del integer  删除字节数（0 = 纯插入）
---@return integer removed
function Tree:invalidate(ns, off, del) end
```

语义（建议默认保守策略，避免 splice 边界腐蚀）：

- `del > 0`（删除/替换区间 `[off, off+del)`）：
  删除满足以下任一条件的 mark `[s, e)`：
  - `s < off + del and e > off`（与删除区间相交）；
  - `e == off`（mark 末尾正好在删除起点，避免末尾删除腐蚀）；
  - `s == off + del`（mark 起点正好在删除终点，避免删除前腐蚀）。
- `del == 0`（纯插入 at `off`）：
  删除满足 `s < off and e >= off` 的 mark。
  - 即“严格包含插入点”或“末尾正好在插入点”的 mark；
  - 不删除“起点正好在插入点”的 mark（实测起点插入只会整体移位，不腐蚀）。

说明：

- `invalidate` 删除的是**完整 mark**，不是裁剪区间。
- 若一个 mark 同时被多个条件命中，只删一次。
- 若不需要保守边界策略，可后续再加 `mode` 参数；第一版按上述保守语义实现。

### 3. （可选）`splice` 返回受影响 mark

更彻底的方案是让编辑本身回报影响：

```lua
--- 返回本次编辑受影响（位移/删除/裁剪）的 mark_id 列表。
---@return integer[] mark_ids
function Tree:splice(off, del, ins) end
```

这会改变现有返回 `self` 的契约；若不想破坏兼容，可新增
`splice_ex` / `splice_with_affected`。第一版若已有 `invalidate`，
此条可作为后续增强。

### 4. 修复 `unmark` 死循环

即使有了 `add_mark` / `remove_mark`，现有 `unmark(id)` 也应修复：

- 复现：单 namespace、单 mark、`t:unmark(id)` 死循环。
- 根因方向：`lst_unmark_apply` 清空“段内唯一层”后的推进逻辑。
- 验收：上述最小用例返回 `1`，且 `span` 不再输出该 mark。

### 5. 迭代中安全删除（可选）

若保留 `span` 迭代删除场景，可提供：

```lua
--- 在迭代回调中允许删除当前 mark。
---@param ns string?
---@param off integer
---@param len integer
---@param fn fun(off:integer, len:integer, attr:table, mark_id:integer)
function Tree:each(ns, off, len, fn) end
```

或允许 `span` 迭代期间调用 `remove_mark` 而不抛错。非必须，第一版可用
“先收集 mark_id 再逐个 remove”的方式。

## 使用示例（editor.lua 预期）

```lua
-- n/N 写高亮：每个命中一个独立 mark_id
local mid = self.tree:add_mark("search", search_attr, p, e - p)

-- 编辑前失效（doc_edit 漏斗内、tree:splice 之前）
self.tree:invalidate("search", off, del)
self.tree:splice(off, del, #s)
```

## 验收用例

### spantree 单测

1. `add_mark` 返回的 mark_id 与 `comp:intern(attr)` 不同；同 attr 两个
   mark 的 mark_id 不同。
2. `remove_mark(mid)` 只删该 mark，另一个同 attr mark 仍在。
3. `invalidate("search", off, del)` 删除完整 mark，不残留半截。
4. 边界用例：
   - mark `[0,3)`，`invalidate("search", 3, 0)`（末尾插入）删除该 mark；
   - mark `[0,3)`，`invalidate("search", 0, 0)`（起点插入）**不**删除；
   - mark `[0,3)`，`invalidate("search", 3, 1)`（末尾删除）删除；
   - mark `[5,8)`，`invalidate("search", 3, 2)`（删除到起点）删除。
5. 编辑无关区域：`invalidate` 不影响远处 mark。
6. `unmark` 单层死循环回归：单 namespace 单 mark 的 `unmark` 正常返回。

### editor 集成

- 搜 `abc` 高亮两处；在第一个 `abc` 中间插入字符，只有第一个高亮消失，
  第二个保留。
- 在匹配末尾插入字符，保守策略下该高亮消失（可接受，避免腐蚀）。
- `:nohl` 清空后编辑不报错。

## 非目标 / 约束

- 不在 Lua 侧维护搜索结果列表；不写 Lua 垫片绕过 C API。
- 本需求只针对“编辑后删除对应 mark”；不包含搜索历史、替换、增量搜索。
- 若实现 `invalidate` 成本过高，允许先实现 `add_mark` / `remove_mark` +
  在 editor 里“收集受影响 mark_id 再逐个 remove”（这是当前 API 下的最小
  可行路径），但最终仍应提供 `invalidate` 作为正式原语。
