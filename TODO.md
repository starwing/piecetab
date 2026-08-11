# Roadmap

> 当前状态：linecache/piecetab/undotree Lua 绑定完成，termfeed/cellgrid
> 孵化完成，tree-sitter 绑定集成，editor.lua 重写 + 测试框架 + 真实
> 语法高亮。行覆盖 100%、分支覆盖 ~90%。以下为未来方向。

## 下一步（2026-08 决议）：多层高亮孵化

> 原则：**Lua 原型先行（v4 方法论）**——在 editor.lua 中孵化新组件，
> 接口随演进固化，之后酌情 C 化（Lua 特有 → Lua C 模块，通用 → stb
> header）。**不空对空设计**：sc 与多层合成在 editor 真实消费场景中
> 开发，独立设计风险 = 返工。
>
> 大目标（远期）：LSP 集成（semantic tokens / inlay hints / diagnostics）。
> hint text 与多层高亮**解耦**（渲染注入独立通道，不进 sc intern）。

### 本期清单

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

## 后续候选（多层高亮落地后评估）

- **hint text**（虚拟文本）：渲染注入能力，基于多层基础；LSP
  inlay hints 前置
- **C 化评估**：整个 Lua 栈通盘考虑（sc/合成器/渲染管线），
  哪些值得 C 化、以何形态（Lua C 模块 vs stb header）
- **spantree**：技术储备齐备（design_spantree.md 核心定案），
  **需要才上**——层存储 + 混合器 + 身份层（extmark）；virt_text
  "渲染注入不承诺"决议是否推翻待 hint text 设计时裁定
- **LSP 集成**（大目标）：semantic tokens → 层写者、
  inlay hints → hint text、diagnostics → 身份层
- **LSP undo 增量同步**（接口摩擦专题，2026-08 标记）：undo/redo
  现走全量 `sync_full()`（无 range didChange）——与"一切走增量
  即时同步"定案摩擦。undotree 增量在手边：`lpt_switch` 的
  `ut_diff` hunks `{pa, pdel, cins}` 坐标系与 `notify_edit(off,
  del, s)` 天然一致，只差 C 侧暴露 `doc:switchdiff()` API；
  边缘场景 fresh undo（未 commit 编辑被 u 丢弃）需另行定案
  （编辑栈逆序 vs 全量兜底）。开工前读 design_lsp.md §4.4

## 低优先级

- **bench 骨架 + FANOUT 调优**：多层高亮后的 editor 真实负载
  可作调优场景（原 TODO #2/#5 合并）
- **popup window / cmd window**：横向新特性，无设计支撑，
  需先澄清孵什么 C 库
- **TODO(C) 小项**（editor.lua 标注）：字符移动原语（pt 侧
  seek/clamp）、显示列换算（cellgrid 家族）、渲染管线（style
  批量 putline，spantree 落地后合并）

## 已完成（2026-07/08 归档）

- linecache/piecetab Lua 绑定 + doc 组合层 + undotree.h 独立库
  （design_luabind.md 定案）
- termfeed / cellgrid C 库孵化（editor demo 驱动）
- tree-sitter 0.26 绑定（兼容 5.1/5.5，grammar .so 加载）
- editor.lua class 化重写 + 注册表 + luaunit 测试框架
- editor.lua 真实 tree-sitter 语法高亮（增量，C/Lua）
- CI（GitHub Actions：build + test + coverage）+ README badges
