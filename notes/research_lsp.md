# LSP 集成调研记录（Task 1 产出，前置资料）

> 日期：2026-08-11。实测两 server（本机）capabilities + 边界行为 + spantree
> 依赖评估。**本文件是调研/前置资料，非设计**——LSP 集成的具体结构
> （模块接口、数据流）遵循 v4 方法论在开发中同步定案（孵化记录，
> 对齐 design_sc.md 模式），落地后写 `notes/design_lsp.md`。
> 产出决策表 → plan_lsp_integration.md 的 Task 5/6/7 依据。
> 工具：/tmp/lsp_probe.py（stdio JSON-RPC 对答，一次性脚本）。

## 一、capabilities 实测（initialize 响应）

| 能力 | lua-language-server 3.19.0 | clangd 21.0.0 (Apple) |
|---|---|---|
| semanticTokens | `full: true` + `range: true` | `full: {delta: true}`，**range: false** |
| semanticTokens resultId | **无**（full 响应无 resultId，永不增量） | 有（full/delta 响应带 resultId） |
| 编辑后 full 响应 | 全量重发（100→105 tokens） | 全量重发（传 previousResultId 也全量） |
| tokenTypes | 23 项（无重复） | 25 项**有重复名**（variable×2、function×3、type×3） |
| tokenModifiers | 12 项 | 18 项 |
| textDocumentSync | change=2 增量 ✓ openClose ✓ | change=2 增量 ✓ openClose ✓ |
| diagnosticProvider（pull） | **无**（null） | **无**（null） |
| 诊断推送 | push publishDiagnostics（didOpen 后 2+4 条） | push（didOpen 后 0 条，无编译错误时静默） |
| inlayHintProvider | `{resolveProvider: true}` | `true` |
| inlayHint 实测 | **配好配置后返回 hint 数组**（kind=2 参数名形态 `"name:"`）。裸连接（不响应 workspace/configuration）返回 null——**非"声明与行为不符"**：`Lua.hint.enable` 默认 false，且 client 须声明 `workspace.configuration` capability 并响应 `Lua` section 配置（VSCode 扩展即如此） | 返回 hint 数组（kind=2 Type 形态 `"a:"`） |

## 二、协议边界行为（实测）

1. **`$/hello` 通知**：lua-language-server 在 initialize 响应**之前**发
   `$/hello: ["world"]`（stdio/TCP 模式检测）。client 必须忽略未知通知
   （含 `$/` 前缀的进度等）——jsonrpc 分发器按"有 method 无 id = 通知"
   处理，未知一律忽略。
2. **publishDiagnostics = per-URI 全量快照**：每次推送覆盖该 URI 全部诊断
   （实测 10 条→11 条全量）。非增量补丁 → client 端无合并逻辑，直接替换。
3. **semantic tokens 编码**：扁平 int 数组，5 int = 1 token
   `[deltaLine, deltaChar, length, tokenType, tokenModifiers]`；deltaLine 跨
   行累计（每行 token 的 deltaLine 相对上一 token 行），deltaChar 同行相对
   上一 token 字符。**两 server 实测均全量重发** → client 不做 delta 应用。
4. **诊断 range 与 offset**：LSP position.character = **UTF-16 code unit**
   索引（规范默认；两 server 均未声明 offsetEncoding 变更）。editor.lua
   内部为 UTF-8 字节 + 显示列 → **必须做 UTF-16 ↔ 字节换算**（见 §五）。

## 三、决策（plan_lsp_integration.md Task 0 落定）

| 决策 | 裁定 | 依据 |
|---|---|---|
| D3 semantic 策略 | **`semanticTokens/full` + 全量替换缓存 + viewport 裁剪渲染** | clangd 不支持 range；两 server 实际都全量重发 → delta 应用是无用功；client 缓存全量数组，渲染时按 viewport 行裁剪（写者协议天然兼容）。resultId 不传 |
| D4 diagnostics 形态 | **push + 按版本丢弃 + 下划线 attr**，**不引入 spantree** | per-URI 全量快照 + server 主动重推 → client 零位置推断，身份层无需求（证据链见 §四） |
| inlay hints | 通道照做，sumneko 场景无 hints 静默（不报错） | clangd 实测可用；sumneko 声明支持实测 null——hint 渲染通道按"空 = 正常"处理 |

## 四、spantree 依赖评估（D4 调研结论）

**结论：LSP 接入不强依赖 spantree（身份层），spantree 继续推迟。**

证据链：
1. **semantic tokens**：全量快照 + 全量替换，token 位置不跨编辑存活（下次
   编辑后全量重拉）——无身份需求。
2. **diagnostics**：server 主动重推全量（编辑 → server 重算 → 新快照覆盖），
   client 不推断旧诊断位置——无身份需求。版本丢弃仅防乱序（记录
   uri→version，响应 version 落后即弃）。
3. **渲染侧**：cellgrid 已有**单元格级 diff**（每帧 diff 旧帧），span 变化
   自动重染差异格——不重染整行/整帧，身份层减负价值不存在。
4. 触发 spantree 的真实场景（仍有效）：undo 跨服务器重算间隙的视觉
   连续性、virt_text 身份、渲染 FANOUT 调优——均非 LSP 接入前提。

## 五、UTF-16 换算坑（集成时必处理）

- LSP 全部 position/range 用 UTF-16 code unit（BMP 内 CJK 每字 = 2 units，
  ASCII = 1；代理对 4 units）。
- editor.lua 侧：doc 字节 offset（UTF-8）+ 显示列（text_byte_to_dcol）。
- 需要的换算：`byte_off ↔ (line, utf16_col)`。实现：行文本按 UTF-8 解码
  累计 width 得 UTF-16 列（`utf8.next` 逐字 + 2 per CJK，或委托 lua-utf8
  `utf8.offset` 逆算——**用 lua-utf8 的 `utf8.width`/`utf8.next` 组合，
  不引新依赖**）。双向换算函数放 Section 2（TODO(C) 候选——列换算本
  就是 cellgrid 家族候选）。
- 陷阱：UTF-16 列与显示列（宽字符 2 列）**不同**（CJK = UTF-16 2 units
  但显示 2 列 vs ASCII 1/1）——但正好同值（CJK 2/2、ASCII 1/1）？不：
  组合字符、emoji（4 units / 2 列）不同。editor 中文场景为主，先按
  BMP 假设实现，代理对标记 TODO。

## 六、其余要点

- **clangd tokenTypes 重复名**：legend 索引 → 名字映射，重复名字指向
  同一 attr 无碍（查表按名字，名字相同即同 attr）。
- **semantic tokens 性能**：100 行 Lua ≈ 100 个 token = 500 int ≈ 4KB
  JSON，全量拉取开销可忽略；大文件（万行）才需评估（bench 时测，
  Task 8）。
- **didChange 增量编码**：server 支持 change=2（增量 range 补丁）——client
  按编辑的字节差生成 UTF-16 range 补丁（Task 4 实现）。
- **clangd 启动**：`--background-index=false` 快；默认后台索引大工程慢
  （风险表已有）。
- **sumneko `$/hello`**：连接后第一条消息即此通知，client 首帧测试须
  容忍（fake server 也要模拟！）。
