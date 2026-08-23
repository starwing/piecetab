# LSP 集成孵化记录（design）

> 状态：孵化中（2026-08-12）。Task 2/3（传输层）已实现，本文档固化
> 其终态 + Task 4-6 设计定案。开发中同步更新（对齐 design_sc.md 模式）。
> 前置资料：notes/research_lsp.md（能力实测，**§五 UTF-16 说法有误，
> 以本文档 §五 为准**）。

## 一、定位

editor.lua 接入 LSP server（stdio/JSON-RPC），semantic tokens /
diagnostics / inlay hints 渐进落地，为多层高亮栈产生第 4/5 层真实负载
（C 化评估触发条件，Task 8）。Lua 原型先行，接口随演进固化。

**架构原则**（用户裁定）：lspclient 为**独立模块**（lua/lspclient.lua，
editor.lua require），非 editor 内嵌类；传输层两个纯模块（jsonrpc/lspio）
离线可测，不依赖编辑器环境。

## 二、模块形态

```
editor.lua 主循环（termfeed getkey 超时轮询）
  └─ Ed 实例持有 lspclient 状态（nil = 未启动）
      └─ lua/lspclient.lua —— 状态机 + pending + 通知分发 + 文本同步 + 请求队列
          ├─ lua/jsonrpc.lua —— 帧解码器（持久状态，Content-Length 切帧）
          │   └─ lua/yyjson.so —— decode/encode（yyjson 绑定，lyy_ 前缀）
          └─ lua/lspio.lua —— luv 进程桥（spawn/pipe/nowait 泵/写队列）
  └─ 数据面：semantic → 第 4 层写者（lsp_spans，拉模式裁剪缓存）
             diag     → 第 5 层写者（diag_spans，拉模式裁剪快照）
             hints    → Task 7（独立注入通道，不进 sc intern）
```

主循环每帧：`Ed:lsp_poll()` → `lspio.pump(h)`（luv nowait）+ 解码分发。
请求是异步的（pending 表 id→回调），写者数据全是**缓存+拉模式**——
render 不发起任何 IO。

**lspclient 依赖注入**（editor 无关，可测）：`get_text()`（didOpen 全
文）、`get_line(lnum)`（UTF-16 换算行文本）、`offset_pos(off)`（字节
offset → line, bytecol）、`on_status(state, why)`。editor 闭包接
piecetab doc；测试接行表。**自足**：UTF-16 换算优先用 vendored
lua-utf8，缺失时回退手写 charlen（双运行时可测）。

## 三、传输层定案（Task 2/3 终态，勿翻案）

| 层 | 形态 | 要点 |
|---|---|---|
| jsonrpc.decoder | 持久状态对象 | `{data, body_start, body_len}` 跨调用保半帧；reader 协议 `chunk`/`""`(pause)/`nil`(EOF)；返回 `msg` 或 `nil, err`（"eof"/"again"/"bad JSON: ..."） |
| jsonrpc.enc_* | 纯函数 | enc_request/enc_notify/enc_result/enc_error 四形态 + frame() 头 |
| lspio.spawn | luv 薄封装 | 无重造 spawn；read_start 累积 buf + reader 索引游标（pause 存活） |
| lspio 写队列 | try_write 回退 | EAGAIN 留队重试；send 只入队，pump 时 drain |
| 帧边界 | **Content-Length 权威** | 字节计数切帧，body 完整才 decode |

**JSON 边界判断结论**（用户遗留问题）：**不需要 yyjson 新功能**。
帧边界由 Content-Length 头决定，与 JSON 内容无关；流式/增量解析场景
不存在（yyjson 要求完整串，decoder 的 data 缓冲已拼分片）；decode 失败
即弃帧继续（坏帧无法恢复，知道"实际边界"也无用）。

## 四、client 架构（Task 4 定案）

### 4.1 状态机

```
idle → initializing（已 spawn，等 initialize 响应）
     → running（握手完成，可发请求/收通知）
     → shuttingdown（发 shutdown 请求，等响应后 exit 通知）
     → exited（进程回收）
启动失败（spawn 错误/initialize 错误响应）→ idle，状态栏报因
```

### 4.2 pending 表

`id → {method, cb}`；响应到达按 id 查表调 cb（cb 收 result 或 error）。
**无超时兜底**（YAGNI——LSP server 同机响应快，卡死时状态栏可见）。

### 4.3 通知分发

有 method 无 id = 通知。查 method 表：publishDiagnostics →
diag 回调；未知（`$/hello`、`$/progress` 等）**一律忽略**（sumneko
连接首条即 `$/hello`）。

### 4.4 文本同步（change=2 增量）

- **didOpen**：全量文本 + version=1（打开文件/启动 client 时）
- **didChange**：version 递增 + edits 数组（升序、互不重叠）
- **即时同步**（设计变更：弃"每帧 flush 合并"）：每条 docedit 在编辑
  **前**换算 range（当时状态可定位）→ 立即可发。坐标相对 server 已
  同步状态，**天然正确，无合并/重算**（合并需跨状态重算坐标，复杂度
  不值）。server 增量处理轻量，同点连续插入 = 多条 didChange 无碍
- **漏斗**：`docedit` 钩子在 `doc:edit` **之前**调用 `notify_edit(off,
  del, s)`——换算用的是编辑前状态（offset_pos 注入）
- **version 追踪**：client 持自增 version（从 1 起），didChange 带上；
  server 侧诊断回推 version 即源自此
- **start 参数**：`lspclient:start(argv, uri, langid, root)`——root 为
  workspace 根 uri（editor 传文件所在目录，测试默认 = uri）
- **undo/redo 同步**（已落地 2026-08，定案见 design_luabind.md
  §十二）：undo/redo 经 `doc:undo(f)` 回调拿 hunk 顺序链（f 逐
  hunk 收 off/del/text）→ `notify_edits` 单条 didChange 多 edit
  （顺序应用语义）；fresh 逆段与 switch 段天然衔接（fresh 应用完
  恰 = committed = switch 的 pa 基准），f 无段感知。废弃
  `sync_full` 全量重传。

### 4.5 initialize 必须带 workspaceFolders（smoke 实测教训）

lua-language-server 只传 `rootUri` 时落入 **fallback workspace**
（server 日志 `<fallback>`），**打开文档不推诊断**。必须传
`workspaceFolders: [{uri=root, name="lsp"}]`（research 原版 probe 一直
传——复制调研脚本时丢失该字段，调试多轮才定位）。clangd 无此问题
（不推诊断），但两 server 同走此路径，统一带上。

### 4.6 method 全名约定

LSP 通知/请求 method 一律**全名**（`textDocument/publishDiagnostics`、
`textDocument/didOpen`）。`lsp:on(method, fn)` 注册用全名；测试 fake
server 回推通知也用全名。调试教训：`publishDiagnostics` 短名注册
handler 永不触发，且收到消息时 method 显示全名——**勿截断比较**。

### 4.7 server 配置链路（workspace/configuration，Task 7 实测教训）

LuaLS 的 inlay hint 等特性由配置开关控制（`Lua.hint.enable` 默认
**false**）。配置传递 = **server 发 `workspace/configuration` 请求**
（section "Lua"），前提 client 在 initialize 声明
`capabilities.workspace.configuration = true`。裸连接（capabilities
空表）→ server 永远不请求配置 → hint 恒 null——**不是"声明与行为
不符"，是配置没开**（VSCode 扩展声明 capability 并响应配置，所以
能看到 hint）。

lspclient 侧：`on_server(method, fn)` 注册 server→client 请求
（fn 返回 result / result+err；nil result 编码为 JSON null——
**Lua 表尾放字面 nil 不构成元素**，须用 `yyjson.null` 哨兵）。
editor 响应：`Lua` section → `{ hint = { enable = true } }`，其余
section → null。

排障教训：server 特性"声明支持却无响应"时，先查**配置开关 +
client 是否参与配置协商**（workspace/configuration），再怀疑
server 行为。

## 五、UTF-16 换算（Section 2 纯函数，纠正 research §五）

**事实**（research §五 有误）：UTF-16 中 BMP 内字符（含 CJK U+4E00–
U+9FFF）= **1 code unit**；仅补充平面（U+10000+，emoji 等）= 2 units
（代理对）。research 说 "CJK 每字 2 units" 错误——显示列 2 列 ≠
UTF-16 2 units。

**换算 = 数 UTF-8 首字节，无需解码码点**：

| 字符 | UTF-8 首字节 | UTF-16 units | 显示列 |
|---|---|---|---|
| ASCII | < 0xC0 | 1 | 1 |
| 2-3 字节（含 CJK） | 0xC0–0xEF | 1 | 1 或 2（宽度查表） |
| 4 字节（emoji 等） | 0xF0+ | 2 | 2 |

```lua
-- editor.lua Section 2（C 化候选，同 cellgrid 家族）
-- 字节 offset → 行内 UTF-16 code unit 列
local function text_byte_to_utf16(text, byte)  -- 每字: 首字节>=0xF0 → +2, 否则 +1
-- UTF-16 unit 列 → 字节 offset（反向累计，clamp 到字符边界）
local function text_utf16_to_byte(text, units)
-- 字节 offset → LSP position {line, character}
local function doc_byte_to_utf16(doc, off)      -- 行定位 via doc:seek("line")
-- LSP position → 字节 offset（diag range / hint 解码用）
local function doc_utf16_to_byte(doc, line, unitcol)
```

三个坐标互不换算（UTF-8 字节 / UTF-16 units / 显示列各管各的）：
显示列 ≠ UTF-16 列（CJK 1 unit vs 2 列）——两函数不共用 dcol 逻辑。

## 六、写者数据流（Task 5/6 定案）

### 6.1 semantic tokens（第 4 层）

```
响应（full + 全量）→ span_decode: 扁平 int 数组按 legend 解码
  → {offset, length, attr}[] 全量缓存（文档坐标系）
编辑 → dirty 标记（缓存保留）→ 新响应到达原子替换 + 清 dirty
渲染 → lsp_spans() 拉模式：缓存二分裁剪 [s_off, e_off] → merge_layers
层序（低→高）: syntax < piece < visual < semantic < diag
```

- **空窗策略**（用户裁定）：编辑后到新响应间**保留旧缓存继续渲染**
  （dirty 但不清空）——无闪烁；漂移窗口 ~50-100ms，VSCode 同款行为
- 解码细节：tokenType 索引 → legend 名 → ATTR_* 映射表，未知忽略；
  clangd 重复名按名字查表无碍（同名同 attr）；tokenModifiers 忽略（先
  不映射，YAGNI——要映射时加位运算叠加）
- 重拉触发：dirty 且 render 结束时（非每帧请求；编辑后一次即可）
- resultId 不传（D3）

### 6.2 diagnostics（第 5 层）

```
publishDiagnostics 快照（per-URI 全量）→ uri → {version, diags} 缓存
  version < 已应用 version → 丢弃（防乱序）
渲染 → diag_spans() 拉模式：range UTF-16 → 字节 span
  → {offset, length, attr=underline}（range 内下划线，非整行）
msg 显示首个错误行（状态栏）
```

- **空窗策略**：同 6.1 保留旧快照（LSP 语义：新快照前旧快照有效；
  编辑后 server 重推自然覆盖）
- **不做位置推断**（D4）：编辑后 server 主动重推，client 零推断；
  无身份层需求（research §四）

### 6.3 编辑后重拉时序（Task 5 起）

```
docedit → 漏斗记 edits → 渲染前 flush didChange(version++)
  → server 重推 diag（自动） / client 发 semanticTokens/full（dirty 且 running）
  → 响应解码 → 原子替换缓存 → 下帧 render 生效
```

## 七、决策记录

| 议题 | 决策 | 理由 |
|---|---|---|
| JSON 边界 | Content-Length 计数，yyjson 零新功能 | 头即权威边界；流式解析无场景；坏帧弃帧即恢复 |
| lspclient 形态 | 独立 lua/lspclient.lua | 状态机代码量大；独立可测（fake server 无需编辑器） |
| 空窗策略 | 保留旧缓存（dirty + 原子替换） | 无闪烁；漂移窗口短；VSCode 同款 |
| 层序 | syntax < piece < visual < semantic < diag | semantic 盖 syntax（LSP 比 tree-sitter 准）；diag 最高（下划线压语义色仍可见） |
| semantic 重拉 | dirty 且 render 后一次 | 非每帧；节流天然（编辑频率 < 帧率） |
| 诊断渲染 | range 内下划线，非整行 | 精确指示错误位置 |
| tokenModifiers | 先忽略 | YAGNI；要映射时加位运算 |

## 八、演进点

- **inlay hints**（Task 7）：独立注入通道（design_sc.md §5 解耦）；
  viewport 内请求 + render_line 注入点；sumneko 返回 null = 静默
- **C 化评估点**（Task 8）：UTF-16 换算（cellgrid 家族）；span_decode
  解码循环；merge_layers 区间折叠（与 spantree 同族）
- **hint text 独立孵化**：Task 7 是唯一入口，之前不独立做
- 请求节流：semantic 重拉若发现 server 忙（pending 中）则跳过本次
- 多 URI（多文件）：lspclient 按当前文件单 URI 起步，多文件后补
  （didOpen/didChange 按活跃文件路由）

## 九、测试

- Task 4：lspclient_test.lua 6 测试（fake server 进程对答：initialize
  握手、$/hello 忽略、didOpen/didChange 回执、UTF-16 换算断言
  （CJK/emoji）、publishDiagnostics 收到、shutdown/exit 序列）
- Task 5/6：editor_test.lua（TestLspSemantic/TestLspDiag：fake server 喂
  token 数组/diag 推送到 grid cell style 断言，含与 syntax 共存 +
  UTF-16 换算断言 + 版本丢弃断言）
- UTF-16 换算：单测矩阵（ASCII/CJK/emoji/tab 混合行的双向一致性）
- 验证：`just lua/lspclient`、`just lua/ed`、LuaLS 零诊断
- **真实 server smoke 教训**（Task 4 已跑通）：lua-language-server 握手
  + 诊断推送实测（3 条 Undefined global）需 workspaceFolders（§4.5）；
  smoke 脚本一次性不保留——回归靠 lspclient_test 的 fake server
