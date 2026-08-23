# editor.lua 现状设计

> 状态：目标设计（2026-08 修订中）。当前未提交 `editor.lua` 经审计判定
> 不保留，将回滚 `HEAD` 后按本文目标重写；本文随 `editor.lua` 同步更新。
> 旧规划与历史决策见文末「历史」章节。

## 〇、接口阅读约定

- `editor.lua` 只依赖各 Lua 绑定的 **`.d.lua` 接口文档**
  （`lua/cellgrid.d.lua`、`lua/spantree.d.lua`、`lua/piecetab.d.lua` 等），
  不直接依赖 `.h` 或 C 实现细节。
- 对接口有疑问时，**先读对应 `.d.lua`**；仍不清晰才读 `.h` 或
  `notes/design_*.md`。
- 一旦通过 `.h`/设计案解答了疑问，**把答案整理回 `.d.lua`**，
  避免下次再翻底层文件。

## 一、定位与模块结构

`editor.lua` 是 piecetab 系 C 库的终端编辑器 demo，也是 C 模块孵化平台。
单文件实现，`return Ed`；测试通过 `require("editor")` 直接驱动 `Ed`。

依赖：

- `piecetab`（Doc：piece tree + undo）
- `cellgrid`（屏幕网格、坐标/宽度/写入）
- `termfeed`（终端输入解析）
- `spantree`（样式合成器 + span 树：hl/sem/diag/piece/visual/vtext）
- `treesitter`（可选，缺失时 `hl` 关闭）
- `lsp`（`lua/lsp.lua`，可选 LSP client）

## 二、Ed 类与依赖注入

```lua
Ed.new(content?, term?, grid?)     -- content 缺省/空 => 空文档
Ed.open(filename, term?, grid?)    -- 读文件
```

- `term` 是鸭子类型：`{ write(s), flush(), size() -> rows, cols }`；
  测试传 fake term，不依赖真终端。
- `grid` 缺省 `cg.new()`。
- `tf` 恒为 `termfeed.State`，`esc_timeout` 控制转义序列超时。

主要字段：

| 字段 | 含义 |
|---|---|
| `doc` | `piecetab.Doc`，文本/光标/undo 的权威状态 |
| `mode` | `"NORMAL"` / `"INSERT"` / `"COMMAND"` / `"VISUAL"` |
| `goal` | 纵向目标列（Neovim curswant），**不是当前屏幕列**；空行/短行会 clamp |
| `scroll_line` | 视口首行 |
| `grid` / `term` / `tf` | 屏幕、终端输出、输入解析 |
| `comp` / `tree` | `spantree.Compositor` / `Tree`，样式与 span 层 |
| `styles` | 预 intern 的 style handle 表 |
| `hl` | tree-sitter 高亮器（可空） |
| `lsp` | `lsp.Client`（可空） |
| `show_pieces` | piece 边界可视化（debug） |
| `sel_start` | visual 模式锚点 |
| `clip` | 无名寄存器 |
| `keymaps` / `commands` | 注册表：`normal/insert/command/visual` + `:命令` |

## 三、按键/命令注册表

- `Ed:keymap(mode, key, fn)`、`Ed:command(name, fn)` 注册，返回 `self`。
- 内置按键/命令全部走同一注册表（`install_normal_keys` 等）。
- `Ed:dispatch(key)` 按 `mode_dispatch[mode:lower()]` 分发。
- 两键组合（`gg`/`dd`）通过 `pending_key` 实现：某键是组合前缀且自身无
  绑定时等待下一键。
- `:命令` 解析：`line:match("^(%a+)(!?)(.*)")`，`q!`/`wq!` 语义正确。

**keymap/cmdmap 是实现细节，工具函数不外露**：

- `keymaps` / `commands` 及其 install 函数只是编辑器内部实现细节，对
  “Demo 暴露 C 函数摩擦 / 孵化 C 库”没有作用；工具函数不要放在文件顶部
  全局可见。
- 所有命令/按键专用工具函数藏在对应的 install 函数内部：
  `install_normal_keys`、`install_insert_keys`、`install_visual_keys`、
  `install_command_keys`、`install_builtin_commands`。
- 例如 `word_class`、`move_word_forward/backward`、`move_vert`、`open_line`
  放到 normal keys 使用处；`ins_escape` / `ins_backspace` / `ins_delete`
  放到 `install_insert_keys` 内；`exec_command` 放到 `install_command_keys`
  内；builtin command handlers 全部在 `install_builtin_commands` 内定义。
- `sel_range` 不保留模块级 helper：内联到 visual `y`/`d` 与 `render()` 的
  visual mark 两处；是否新增 C `Doc:sel_range` / `Doc:char_span` 另议
  （见 audit 第 11 节，`charlen` 本身够用，剩余判断是 Lua 选择语义）。

## 四、样式与 span 层（spantree）

`Ed.new` 建立 compositor 并注册 namespace：

| namespace | 优先级 | 用途 |
|---|---|---|
| `vtext` | 0 | 注入显示文本（LSP inlay hint），`attr = {vtext=string, vstyle?=style}` |
| `hl` | 1（eph） | tree-sitter 语法高亮，每帧清空重填 |
| `sem` | 2 | LSP semantic tokens |
| `diag` | 3 | LSP diagnostics |
| `piece` | 4 | piece 边界可视化（快速层，每帧重填） |
| `visual` | 5 | 视觉选择反色（快速层，每帧重填） |

`tree:styled(s_off, e_off)` 返回已按 namespace/优先级折叠好的非重叠 run，
Lua 侧不再有 `merge_layers`。

## 五、渲染管线

`Ed:render()` 主流程：

1. 隐藏光标，取 `rows/cols`，clamp `scroll_line`。
2. 若启用 `hl`：清 `hl` 层，`hl:query_region(self.tree, "hl", s_off, e_off)` 直接写
   spantree（不再返回 span 表）。
3. 填 `piece`/`visual` 快速层：piece 直接内联遍历
   `doc:buffer():pieces()` 并 `tree:mark("piece", ...)`，不建中间 table
   （不保留 `piece_spans` 函数）。
4. `tree:styled(s_off, e_off)` 收集 viewport 级 spans（仅供非行内用途；
   `_render_line` 不再依赖 `self._spans`）。
5. `g:begin` + 行号列。
6. 逐行调用 `_render_line(line_idx, col, dry_run, target_scol, ...)`：
   - 行内样式与 vtext 全部来自 `tree:styled(lo, lo + linelen + 1)`；
   - 不再需要 `hl.line_segments` 或单独 `tree:span("vtext")`。
7. `g:diff` 生成 CSI 输出。
8. `render_status` + `render_cursor`，`g:freeze`。
9. `lsp:post_render()` 异步刷新。

### `Ed:_render_line`（目标算法，2026-08 用户澄清后修订）

`_render_line(line_idx, row, col, dry_run, target_scol, ...)` 是唯一行走；
dry_run 与真实渲染共用同一流程，区别只有：

1. dry_run 不写 cell；
2. dry_run 一旦发生 Doc 更新就立即 return。

非 dry_run 在 `text_dirty` 为真时**同样更新 Doc**，只是更新后继续渲染。

入口：

- 若 `dry_run and not text_dirty`：直接 return（没有要同步的东西）。

状态变量：

- `text_start = 0`：尚未写入 grid 的文本起始 byte。
- `style_start = 0`：该段积压文本在屏幕上的起始列。
- `dc = 0`：当前屏幕列。
- `cursor_col`：权威屏幕列。
- `text_dirty`：Doc 光标是否已落后于 `cursor_row/col`。

对每个 run：

1. **没有 `vtext`**
   - 若 `not dry_run`：**只写样式不写文本**——`g:span(row, col+dc, col+endc, id)`，
     其中 `endc = g:cols(dc, run_text)`。
   - 推进 `dc`（dry_run 只推进，不写）。
   - 不更新 `text_start` / `style_start`。

2. **有 `vtext`**
   a. 若 `off > text_start`，处理积压文本 `pending = doc:readat(text_start, off)`：
      - 若 `text_dirty` 且 `cursor_col` 在
        `[style_start, style_start + width(pending))` 内：
        用 `g:byte(style_start, cursor_col - style_start, pending)` 得到
        byte，更新 Doc，置 `text_dirty = false`；**若 dry_run 立即 return**，
        非 dry_run 继续。
      - 若 `cursor_col` 在 hint 区间 `[dc, dc + hint_w)` 内 →
        `cursor_col = dc + hint_w`（跳过 hint），**不更新 Doc、不 return**；
        此行为 dry_run 与真实渲染都执行。
      - 若 `not dry_run`：`g:putstring(row, col+style_start, nil, pending)`
        （nil 即 `CG_TRANSPARENT`，只填字符、保留已铺 style）。
   b. 写 hint：`hint_w = g:cols(dc, attr.vtext) - dc`；
      - 若 `not dry_run`：`g:putstring(row, col+dc, attr.vstyle or styles.hint, attr.vtext)`；
      - `dc = dc + hint_w`。
   c. 写锚点文本 `anchor = doc:readat(off, len)`：
      - 若 `text_dirty` 且 `cursor_col` 落在锚点内：用
        `g:byte(dc, cursor_col - dc, anchor)` 映射并更新 Doc，置
        `text_dirty = false`；**若 dry_run 立即 return**，非 dry_run 继续。
      - 若 `not dry_run`：`g:putstring(row, col+dc, id, anchor)`（直接带 style）。
      - `dc = g:cols(dc, anchor)`。
   d. `text_start = off + len`；`style_start = dc`。

3. 循环结束后：
   - 若 `text_dirty` 仍为真：在 `remaining` 内映射到 EOL/clamp，更新 Doc，
     置 `text_dirty = false`；**若 dry_run 立即 return**。
   - 若 `not dry_run`：`g:putstring(row, col+style_start, nil, remaining)`。

**EOL hint 特殊规则（Neovim 模型，2026-08 用户实测确认）**：
- hint 挂在换行符（`rel_off >= text_end`）时，EOL 光标停在最后一个字符上，
  **不**在 hint 后；渲染记录 `cursor_col = eol_cursor_dc`（文本末尾、hint 前）。
- dry_run 若 `cursor_col` 落在 EOL hint 内或之后：更新 Doc 到 EOL，并把
  `cursor_col` 置为 `eol_cursor_dc`；dry_run 更新后立即 return。
- `a` 在最后一个字符上按下时，插入发生在 hint 前；hint 随换行符右移。

> dry_run 与真实渲染共用同一 run 流。dry_run 不写 cell，且一旦发生 Doc
> 更新就提前 return；非 dry_run 在 `text_dirty` 时同样更新 Doc，只是更新后
> 继续渲染。hint 跳过只调整 `cursor_col`，不触发 return。`styled()` 已覆盖
> 整行（含无 style run），`vtext` 字段是 hint 锚点信号。真实渲染中
> “无 vtext run 先 span 染色、vtext 时 transparent 冲刷积压文本”是
> `CG_TRANSPARENT` 的核心用途。

### 坐标模型

- **文本列**：Doc 存的光标列，`g:next`/`g:cols`/`g:byte` 的输入输出，
  用于编辑与 hint 锚定。
- **屏幕列**：实际显示列，用于画光标和 j/k 目标列。
- `Ed.cursor_row/col`：屏幕坐标；`doc:line/column/offset`：文本坐标。
- `Ed:_render_line(dry_run=true)`：屏幕列 → 文本列（只对光标行）。
- `Ed:_render_line(dry_run=false)` 渲染光标行时记录 `cursor_col`：
  文本列 → 屏幕列。
- `move_vert` 用 `goal` 保存屏幕目标列；`goal == nil` 时用 `cursor_col`
  作为当前屏幕列；目标行由 dry_run 换算回文本列。
- **注意**：`goal` 是目标列（curswant），不是当前屏幕列；当前屏幕列
  在短行/空行会被 clamp 到行尾（如空行屏幕列 0，但 goal 仍保留原值）。

## 六、LSP 集成

`lsp` 模块可选（`pcall(require,"lsp")`，缺省时 LSP 关闭）。`Ed:lsp_start`
是薄入口，只调 `lsp.attach(self, {silent=, argv=})`；Client 装配、样式映射
与回调全部在 `lua/lsp.lua` 内：

- `get_text` / `get_line` / `offset_pos`：文本快照与位置换算。
- `vtext.set/clear`：inlay hint → `set_vtext` / `clear_vtexts`。
- `sem.set/clear`：semantic tokens → `set_sem` / `tree:clear("sem")`。
- `diag.set/clear`：diagnostics → `set_diag` / `tree:clear("diag")`。
- `on_status`：LSP 进程状态进 `msg`。

`set_vtext` 把 hint 写成 `tree:mark("vtext", {vtext=..., vstyle=?}, off, n)`，
编辑移位由 spantree splice 承担。

## 七、C 模块孵化边界

editor demo 是 C 库孵化平台：cellgrid 坐标族、spantree 样式折叠均由此
孵化。判断标准是【让 Lua 层写得更顺手】，负载只作参考。

**C 孵化独立铁律**：

> 任何 C 库/模块都不得接受其他 C 库对象作为输入：C 层禁止做胶水。
> 跨库组合只能在 Lua 层完成，或通过独立纯数据接口衔接；若某个 C 库
> 需要另一个 C 库的数据，应重新设计边界，而不是直接传对方对象。

**Demo 暴露摩擦原则（禁 C 函数垫片）**：

> 不允许为 C 绑定函数写 Lua 垫片（shim）。Demo 必须直接调用 C 模块 API，
> 以便真实暴露“这里用起来不顺”的摩擦点，驱动 C 库孵化。像 `line_text`
> 这种把 `doc:readat(doc:lineoffset(...), doc:linelen(...))` 包一层的
> oneliner 应删除，调用处直接写 C API。

已 C 化：

- cellgrid 坐标族：`cg_next` / `cg_cols` / `cg_byte` / `cg_putslice`
  （slice 形态，见 design_cellgrid.md）。
- spantree：`styled()` 区间折叠、`span()` 行内迭代、vtext 带宽度节点。

仍在 Lua 侧、可能继续孵化的候选见 `TODO.md`（render_line 批量
putstring、坐标 bridge 等）。

## 八、开放问题

- tab 展开基数：目标设计统一用屏幕列基数——宽度计算与 dry_run 映射都用
  `g:cols(dc, ...)` / `g:byte(dc, ...)`，`putstring` 负责 tab 展开；不再
  按文本列手拆 tab。
- ~~`screen_to_text_dcol` 的摩擦~~：已由 `Ed:_render_line(dry_run=true)`
  吸收（只对光标行，不再单独维护反向换算方法）。

## 九、光标坐标同步（目标设计，修订中）

> 本节是目标设计；审计判定当前未提交实现不保留，回滚后由
> `Ed:_render_line` 的 dry_run 统一承担，并移除 `vtext_dcol` /
> `screen_to_text_dcol` / `_sync_text_from_screen`。

### 坐标模型

- **文本坐标**：`doc:line()` / `doc:column()` / `doc:offset()`，编辑的
  权威坐标。
- **屏幕坐标**：`Ed.cursor_row` / `Ed.cursor_col`，画光标和纵向运动的
  权威坐标。两个坐标系同等重要，没有“谁只是缓存”的说法。

### dry_run

- `Ed:_render_line(line_idx, row, col, dry_run, target_scol, ...)` 是 Ed
  私有方法，也是唯一行走：
  - 行内样式与 vtext 来自 `tree:styled(lo, lo + linelen + 1)`，不依赖
    `self._spans`；
  - `dry_run=true` 与真实渲染**同一 run 流**，区别只有两点：
    1. 不写 cell；
    2. 一旦发生 Doc 更新就立即 return。
  - `dry_run=true and not text_dirty`：直接 return（没有要同步的东西）。
  - `dry_run=false` 且 `text_dirty` 为真时：**同样在行走中更新 Doc**，
    更新后继续渲染，并在光标行顺带记录 `cursor_col`（text→screen）。
  - `cursor_col` 落在 hint 内则推到 hint 后，不更新 Doc、不 return，
    继续行走（dry_run 与真实渲染都执行）。
- 仅需要同步（访问器场景）时，对 `screen_line` 调用
  `Ed:_render_line(..., true, cursor_col)`；不需要整屏 `render()`。

### lazy sync

- `j/k`（以及未来 wrap 类移动）只更新 `cursor_row/col`，置
  `text_dirty = true`，不立即改 `doc`。
- `render()` **不单独 pre-sync**：逐行调用 `_render_line` 时，若该行是
  光标行且 `text_dirty`，行走中同步 Doc 并继续绘制；同步后
  `text_dirty = false`。
- Ed 提供访问器：
  ```lua
  Ed:text_line()
  Ed:text_col()
  Ed:text_offset()
  ```
  若 `text_dirty` 为真，访问器直接调用
  `Ed:_render_line(line, 0, true, cursor_col)` 同步后再返回。
- 外部代码不得直接读 `doc:line()/column()/offset()` 作为光标状态；
  一律走 Ed 访问器。
- 内部路径同样不得在 `text_dirty` 时直接读/写 `doc`：key handler、
  `docedit`、`set_vtext`、LSP `tick/poll` 回调等都要先同步或改走访问器；
  不能依赖“每键后 render”来掩盖。

### 测试覆盖

- `j/k` 无 hint：屏幕列保持、文本列正确。
- `j/k` 有 hint：跳过 hint，光标不落在 hint 内部。
- INSERT 临时状态：在 hint 前 `a` 后向下，光标落在 hint 首字符。
- 行尾/空行 clamp：目标屏幕列超过行宽时文本列 clamp 到行尾。
- 读访问器强制 sync：`dispatch("j")` 后不 render，直接 `e:text_col()`
  应返回正确新文本列。
- 连按 `j` 不 render：第二次 `j` 基于 `cursor_row/col` 而非旧 doc。
- `w/b/h/l` 跳过 hint 文本（左右两个方向都要测）。

## 历史

### 早期规划（class 化 + 注册表 + 可测试性）

- 最初 editor.lua 是简单 demo（约 866 行），全局单例 `tk_instance`，
  命令靠 if/elseif 链。
- 规划：正规化 class 写法、LuaLS 标注、注册表 API、多实例依赖注入、
  Term 类封装 termfeed。
- 已实施并演进：`Ed` class + 注册表已落地；`Term` 类未单独建，改用
  鸭子类型 `term` 表 + 实例字段 `tf`。
- spantree/highlighter/LSP 是后续新增，早期规划未覆盖。

### 渲染/坐标演进

- 早期 `text_byte_to_dcol` / `text_dcol_to_byte` 在 Lua 侧；cellgrid
  坐标族 C 化后改为 `g:cols` / `g:byte`。
- 早期 vtext 拼接循环在 editor 层；spantree 落地后 vtext 入树，
  `merge_layers` 由 `styled()` 取代。
