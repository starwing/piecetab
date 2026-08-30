# Roadmap

> 当前状态（2026-08-30 更新）：
> - 核心 C 库已落地：linecache / piecetab / undotree / cellgrid / termfeed /
>   spantree / **textmatch**，均为 C89 单头 + 测试/CI/覆盖率。
> - editor.lua 已完成多层高亮、spantree/vtext、LSP 集成、光标同步与渲染整理。
> - 行覆盖 100%、分支覆盖目标 95%（覆盖构建用 -DNDEBUG，assert 分支不纳入）。
> - textmatch.h 独立库 + Lua 绑定（`tm.find/match/gmatch`）已落地；
>   尚未接入 editor.lua 搜索。

## 已完成（2026-08）

### textmatch.h（新落地）

- [x] **独立库**：stb-style 单头 C89、无堆分配、前缀 `tm_`；`tm_Reader`
      可 seek 的 piece 源抽象，可直接跑在 piecetab 的 piece 表上。
- [x] **Lua pattern**：按 Unicode codepoint 匹配；`^ $ * + - ?`、字符类 /
      `[...]` / `%b` / `%f` / `%1..%9` / `()` captures；`TM_LITERAL`、
      `TM_LINEANCHOR`。
- [x] **API**：`tm_reset/seek/pattern/match/find/offset/matchend/capture`；
      source 游标集中在 `tm_State`，匹配层只传 `tm_Match *M`。
- [x] **测试与集成**：`tests/textmatch_test.c` 全覆盖（split piece、跨 piece
      回溯、literal 跨块搜索、mid-character 边界等）；`just tm` /
      `just tm-cov` / CI 已接入；`misc/check_textmatch.lua` 做匹配层结构检查。
- [x] **文档**：`notes/design_textmatch.md` 与 refine / global-state 计划、
      audit 已同步。
- [x] **Lua 绑定**：新增 `lua/textmatch.c`，提供 `tm.find` / `tm.match` /
      `tm.gmatch`，接受 string 与 `piecetab.Buffer`，索引统一 Lua 1-based；
      `just lua/tm` / `just lua/tm-cov` 已接入。

边界：尚未接入 editor.lua 搜索（见下一步候选）。

### 多层高亮 / spantree / vtext

- [x] **sc（style compositor）**：attr 字段化 table → 32bit handle，逆查 +
      动态 CSI。
- [x] **cellgrid 绑定优化**：`lua_geti`、代理表、style 0 默认。
- [x] **spantree Lua 绑定组件化**（Compositor / Tree / Cursor），editor 多层
      消费替代手写 merge_layers。
- [x] **vtext / hint text** 作为 spantree 服务层，LSP inlay hints 经 vtext
      注入。
- [x] **渲染接入**：`_render_line` 单次 styled 遍历 + 屏幕列 tab。

### LSP 集成

- [x] RPC framing + luv IO、client 状态机、增量文本同步。
- [x] semantic tokens / diagnostics / inlay hints 写者。
- [x] undo/redo 增量同步（`doc:undo(f)` hunk 回调）。
- [x] UTF-16 换算族 + vendored luautf8，按行 `utf16_map` 批量映射。

### bench / 调优

- [x] bench 骨架（C89 harness + JSON + 绘图）。
- [x] PT/LC/SP FANOUT 调优 + README 性能表。

### editor 与基础库整理

- [x] 光标坐标同步：dry_run + lazy sync、单次 styled 遍历、tab 按屏幕列。
- [x] editor.lua 整理：ATTR_* 直接挂 Ed、LSP 可选 attach。
- [x] 坐标族 / 字符移动 C 化（cellgrid slice 坐标族、piecetab doc 字符原语）。
- [x] 摩擦项已确认/解决：`styled()` 混 vtext 是设计信号；`Grid:put/putstring`
      `st=nil` 保留原样式。
- [x] piecetab `pt_close`/`pt_reset` 全量回收 arena：state 维护全局 arena
      block 链表，close/reset 时释放仍存活树的 arena，不再要求调用方先
      `pt_release` 所有 buffer。

## 下一步候选（未完成）

- **textmatch 接入 editor.lua 搜索**
  - 基于已落地的 `lua/textmatch.c`，把 `/`、`?` 搜索接到 editor.lua。
  - 反向搜索按设计用 forward 反复找；空匹配推进由调用方处理。
  - 顺手补 README 的库清单 / API 概览。
- **editor 读行不要整行读**：渲染路径已按 run 按需读；剩余 word motion 等
  整行读取可改为按需/前缀读，避免大行全量拷贝。
- **C 化评估报告**：从 editor demo 的使用逻辑评估剩余 C 化候选（哪个 API
  让 Lua 层写得更顺手；Lua C 模块 vs stb header），负载只作参考。
- **LSP 扩展**：在能促进展示/孵化 C 模块时做（如 UTF-16 换算、span_decode、
  semantic/diag 写者的 C 化展示）；否则保持 Demo 够用即可。

## 低优先级

- **textmatch `^` 在查找边界 `off == endoff` 的 0 长度匹配**：当前
  `tm_find` 对 `S->off >= endoff` 直接返回 `TM_OK`，因此 `^` 在空输入或
  范围终点不匹配；这是已接受的行为。若以后要支持，需要只放行 `^` 分支并
  校验 `tm_matchend(S) == S->off`（或引入 bounded probe），属于低优先级增强。
- **popup window / cmd window**：横向新特性，无设计支撑，需先澄清孵什么 C
  库。
- **lsp.lua UTF-16 非法 UTF-8 显式处理**：piecetab 是字节级库，编辑若在多
  字节码点中间切开会产生非法 UTF-8，`Protocol.text_byte_to_utf16` 会抛
  `invalid UTF-8 code`。报错本身是对的；后续在 Lua 侧处理成带上下文的明确
  错误，或保证编辑不产生这种状态。低优先级：当前编辑（charlen 约束）不会
  产生这种文本。

## 历史归档

- linecache / piecetab Lua 绑定 + doc 组合层 + undotree.h 独立库
  （design_luabind.md 定案）
- termfeed / cellgrid C 库孵化
- tree-sitter 0.26 绑定（5.1/5.5 兼容，grammar .so 加载）
- editor.lua class 化重写 + 注册表 + luaunit 测试框架
- editor.lua 真实 tree-sitter 语法高亮（增量，C/Lua）
- spantree 落地：C 库（arbiter 单层）+ Lua 绑定 + 搜索 demo 曾接入后退场；
  现由 editor.lua 真实消费
- CI（GitHub Actions：build + test + coverage）+ README badges
