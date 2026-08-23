# Roadmap

> 当前状态（2026-08-23 更新）：linecache/piecetab/undotree Lua 绑定完成，
> termfeed/cellgrid 孵化完成，tree-sitter 绑定集成，editor.lua 重写 +
> 测试框架 + 真实语法高亮；**多层高亮已全部落地**，**spantree 已从储备
> 转为 editor.lua 的样式/层/vtext 基础设施**，**LSP 集成（semantic /
> diag / inlay hints / undo 增量同步）已完成**，**bench 骨架 + PT/LC/SP
> FANOUT 调优完成**。行覆盖 100%、分支覆盖目标 95%（覆盖构建用
> -DNDEBUG，assert 分支不纳入）。以下为未来方向。

## 已完成（2026-08 归档）

### 多层高亮孵化（2026-08 决议，本期清单全部完成）

- [x] **sc（style_compositor）**：editor.lua 内嵌类（Term 式）——
      attr 字段化 table（fg/bg + attr 键）→ 32bit handle
      （规范化 + hash 复用，递增分配）+ 逆查 + 动态 CSI 生成
      （替代静态 DIFF_STYLE；色值形态决定 256/真彩色 CSI）
- [x] **cellgrid.c 绑定**：`lcg_styleof` 的 `lua_rawgeti` → `lua_geti`
      （`lua53_geti` shim，5.1/5.2/LuaJIT 兼容，`lua53_` 前缀惯例）；
      diff 接受带元表代理表（`__index` 动态查 sc）——帧内表免构建；
      style 0 恒为默认样式（`lcg_finish` 依赖）
- [x] **合成器**（editor 内）：键级 partial——高层设的键覆盖、低层
      透传（CSS 覆盖式）；任意计算混合（如背景色混合）走自定义
      fold，API 先不定（演进点）
- [x] **ts 写者改造**：hl 模块 query_region 产 attr 字段化 spans
      （capture 名 → attr，不再产 STYLE ID）
- [x] **piece 写者**：pt piece 序列扫描 → 交替 gray bg attr spans
      （拉模式，无缓存，每帧现场扫描）
- [x] **渲染接入**：query_region → 各写者 → 合成 → sc:intern →
      handle spans → line_segments/render_line（style 变 handle，
      逻辑不动）；`STYLE_*` 常量 → attr 表常量；status bar 直写
      CSI 不经 grid，不动
- [x] **测试**（editor_test）：intern 复用/逆查一致/CSI 生成、
      双写者共存（piece 背景 + 语法前景同格）、键级 partial 合成、
      piece 边界与行边界交错
- [x] **边界记录**入 notes：`notes/design_sc.md`——sc 接口形态、
      C 化评估点（通用 → stb，Lua 特有 → C 模块）、hint text 解耦

### spantree 消费 + vtext/hint text（2026-08 落地）

- [x] **spantree Lua 绑定组件化**：Compositor / Tree / Cursor 三组件
      （`sp.compositor()` + `sp.new(comp)`），样式服务与树解耦；
      定案见 `notes/design_spantree_lua.md`；`spantree.h` API 零改动
- [x] **editor.lua 多层消费**：hl（eph）、sem、diag、piece、visual、
      vtext 各层注册到 spantree tree；渲染经 tree 的 span/styled 流
      合成，替代 Lua 侧 merge_layers 手工叠加
- [x] **vtext / hint text**：vtext 作为 spantree 服务层（绑定后一字符、
      attr 含 `vtext`/`vstyle`），编辑位移由 tree:splice 承担；
      LSP inlay hints 经 vtext 注入；坐标换算由 `Ed:_render_line`
      dry_run / `cursor_col` 记录承担（旧 `vtext_dcol` /
      `screen_to_text_dcol` 已移除）

### LSP 集成（2026-08 完成）

- [x] **传输层**：`lsp.lua` 内 RPC framing + luv IO（spawn/pump/写队列），
      Content-Length 切帧；离线可测
- [x] **client 核心**：状态机、pending、通知分发、增量文本同步、
      workspace/configuration 应答、按扩展名自动启动
- [x] **semantic tokens → 第 4 层写者**：`semanticTokens/full` 解码、
      全量缓存 + 原子替换、编辑后 dirty 重拉、层序
      syntax < piece < visual < semantic < diag
- [x] **diagnostics → 第 5 层写者**：publishDiagnostics 快照缓存、
      version 防乱序、UTF-16 range → 字节 span、range 内下划线；
      **不做身份层**（编辑后 server 重推，client 零推断）
- [x] **inlay hints → vtext 注入**：viewport 内请求、空闲 debounce、
      null 有界重试、stale 响应丢弃
- [x] **undo/redo 增量同步**：`doc:undo(f)` 回调式 hunk 暴露
      （off/del/text 顺序链，fresh+switch 单条 didChange），
      废弃 `sync_full`/`resync`。定案见 design_luabind.md §十二

### bench / FANOUT 调优（2026-08 完成）

- [x] **bench 骨架**：C89 harness（bench.h）+ piecetab / spantree /
      linecache 三套 benchmark suite + JSON 输出 + 绘图脚本
- [x] **FANOUT 调优**：PT_FANOUT=31、LC_FANOUT=16/LC_LEAF_FANOUT=34、
      SP_FANOUT=34；MAX_LEVEL 可配置并有安全守卫
- [x] **README 性能表**：seek/locate/advance/splice/next/edit/scan/fill
      汇总

### 早期归档

- linecache/piecetab Lua 绑定 + doc 组合层 + undotree.h 独立库
  （design_luabind.md 定案）
- termfeed / cellgrid C 库孵化（editor demo 驱动）
- tree-sitter 0.26 绑定（兼容 5.1/5.5，grammar .so 加载）
- editor.lua class 化重写 + 注册表 + luaunit 测试框架
- editor.lua 真实 tree-sitter 语法高亮（增量，C/Lua）
- **spantree 落地**（2026-08）：C 库（arbiter 单层）+ Lua 绑定
  （compositor C 化 + __hash + epoch 守卫，定案
  notes/design_spantree_lua.md；API 摩擦检查结论：spantree.h 零改动）
  + 搜索高亮 demo 曾接入后按第一性退场。compositor 与 vtext/多层
  染色服务现由 editor.lua 真实消费
- **坐标族 / 字符移动 C 化**：cellgrid slice 坐标族
  （cg_next/cg_cols/cg_byte/cg_putslice）与 piecetab doc 字符原语
  （advancechars/charlen/readat/linelen noeol）已落地
- CI（GitHub Actions：build + test + coverage）+ README badges

### 光标坐标同步（2026-08 完成）

- [x] **dry_run + lazy sync**：`j/k` 只更新屏幕坐标并标脏；render 或
      `text_line/text_col/text_offset` 访问器只对光标行调用
      `Ed:_render_line(dry_run=true)` 同步 Doc；`render()` 渲染到光标行
      时先同步再绘制并记录 `cursor_col`。已移除 `vtext_dcol` /
      `screen_to_text_dcol` / `_sync_text_from_screen`。见
      `notes/design_editor.md` §九 + `notes/plans/plan_editor_cursor_sync.md`

### 摩擦列表（2026-08，render_line 重构已确认）
- ~~`tree:styled()` 混入 vtext~~ **已确认是设计信号，不是摩擦**：
      `styled()` 在 hint 锚点的 text run 上带 `vtext` 字段，正是用来同时拿到
      “锚点 text + hint”并一次写出的信号；不需要跳过/忽略。剩余边界：
      同一字节多个 vtext mark 时 attr 是否能保留全部 hint、vtext mark 长度
      是否总是一个字符（当前 `set_vtext` 用 `charlen`，通常为 1）
- ~~**cellgrid 染色接口没有 `st=nil` 保留原样式**~~ **已落地**：
      `Grid:put` / `Grid:putstring` 的 `st` 省略/nil 时映射为 `CG_TRANSPARENT`，
      只写文本、保留原 style；`Grid:span` 仍要求 `st` 为 integer（纯 style
      更新不接受 nil）。见 `notes/design_cellgrid.md` §3.4

## 后续候选（2026-08 评估后剩余）

- **C 化评估（剩余部分）**：sc 已 C 化进 spantree，cellgrid 坐标族 /
  字符移动已 C 化，merge_layers 区间折叠已由 spantree `styled()` 完成；
  剩余渲染管线（render_line 简化、vtext 坐标换算按屏幕列 tab）与
  lsp.lua UTF-16 换算族仍待评估；评估以“让 Lua 层写得更顺手”为主要
  标准，负载只作参考
- **TODO(C) 剩余小项**：`Ed:_render_line` 直接走 `cg_putslice` 的屏幕列
  tab 展开，去掉手拆 tab/fill；坐标换算已由 dry_run / `cursor_col` 记录
  承担；lsp.lua UTF-16 换算族与 cellgrid 家族合并评估（原 plan_cols
  Step 4 暂缓）
- **spantree per-line styled cursor 候选**：用 Cursor / 行裁剪迭代替代
  viewport spans 表 + hl.line_segments，逐行直接产出折叠 run，渲染循环
  不再需要中间 spans 表
- **editor 读行不要整行读**：渲染/坐标换算只需屏幕宽附近；按需读
  `text_width * 3`（或屏幕宽），遇 UTF-8 截断再续读；避免大行全量拷贝

## 低优先级

- **C 化评估报告**：当前最自然的下一步，从 editor demo 的【使用逻辑】
  评估剩余 C 化候选（哪个 API 让 Lua 层写得更顺手；Lua C 模块 vs stb
  header），负载只作参考
- **LSP 扩展**：在能促进展示/孵化 C 模块的情况下做（如 UTF-16 换算、
  span_decode、semantic/diag 写者的 C 化展示）；否则保持 Demo 够用即可
- **popup window / cmd window**：横向新特性，无设计支撑，
  需先澄清孵什么 C 库
