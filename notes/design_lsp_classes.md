# LSP 类设计定案（lsp.lua：Protocol / Client + hint text Ed 接口）

> 状态：设计定案（2026-08-12）。前置：notes/design_lsp.md（协议层孵化记录，
> §二/§四 仍有效）；本文档固化**类结构重构 + hint text Ed 接口 + tmux 显示测试**。
> 类结构非锁死——用户明示后续可能修改，本文档作为演进起点。
> **实现回更（2026-08-12）**：重构已落地（lsp.lua 四类 + Ed vtext + editor 接线
> 收敛 + 测试清理 + tmux 显示测试），逐节对照实现修正签名/行为/用例数偏差，
> 偏差点标注"（实现回更）"；设计意图未变的段落保持原文。
> **变更（2026-08-12）**：Render 类撤回——暂只 Protocol + Client 两分类；
> Render 定位为摩擦层，hint 显示逻辑先进 Client，从业务摩擦中提炼
> **Ed 注入文本接口**（见 §3.3），为孵化 C 库做准备。

## 一、背景与动机（失控证据）

近 7 天 12 个 hint 相关 fix，典型打地鼠：cursor 偏移三改（596177c→ab35c6f→
1f8748f）、stale 响应守卫（421d8c2）、idle 调度新机制（51fbb2d）、行尾崩溃
（394ddfc）、墙钟修复（9735e64）、null 重试（9049234）、编辑位移（4b6914d）
……每次修复都往 `Ed` 上加状态，最终 **13 个 lsp_\* 字段**散落 editor.lua。

根因两条：

1. **职责边界错误**：editor.lua 直接 require `yyjson`（仅配置回答 L1388）、
   `luv`（仅绝对路径 + 墙钟），LSP 领域逻辑（hint 解码/位移/调度、semantic、
   diag 解析）约 400 行在 editor.lua 内，与光标/渲染核心纠缠。
2. **坐标体系分裂**：hint 是注入文本，但"显示列"计算散落三处——move_vert
   存真实文本列（L370）、render_cursor 加 hint 偏移（L1751-1756）、motion
   字节级移动不感知——每次只补一处，下一处漏。Neovim 语义：**垂直移动按
   屏幕列**（含虚拟文本）记忆。

测试同步失控：editor_test 断言内部状态（`e.lsp_hints[0][1].dcol`、
`e.lsp_hint_retry`），与实现耦合；ANSI 字节流断言难读（`\27[1;9H\27[?25h`
+ 手算列注释）。

## 二、文件与类结构

```
lua/lsp.lua（一个文件，四个类；require 时返回 {RPC=, IO=, Protocol=, Client=}）
│
├── LspRPC 类 = RPC      —— 帧编解码（现 lua/jsonrpc.lua 全量，零改动语义）
│    Content-Length 切帧 / 持久解码器 / enc_request·enc_notify·enc_result·enc_error
│
├── LspIO 类 = IO        —— 进程桥（现 lua/lspio.lua 全量，零改动语义）
│    luv spawn / pipe / nowait 泵 / 写队列（EAGAIN 回退）
│
├── LspProto 类 = Protocol —— 协议核心（现 lua/lspclient.lua 全量，零改动语义）
│    状态机 / pending / 通知分发 / didOpen·didChange / UTF-16 换算 /
│    默认 workspace/configuration 响应（内置，json.null 使用点收拢到本类）
│
└── LspClient 类 = Client —— 接入层（editor 唯一接触面，"lspclient 用来接入"）
     协议编排 + LSP 数据获取/调度 + 数据写入 Ed vtext 槽与查询层
```

**（实现回更）类注解名**：LuaLS 注解统一为 `lsp.RPC`/`lsp.IO`/`lsp.Protocol`/
`lsp.Client`，与 `require "lsp"` 返回值一致（原 lspio.Handle/lspclient.Client
残留名指向已删模块，产生 47 个 LuaLS warning，已修）。RPC 为纯函数表
（enc_*/decoder），无 `@class` 注解；IO/Protocol/Client 各带 `@class`。

**Render 的定位（撤回建类）**：hint 显示逻辑作为 Client 内部方法先跑业务。
集成摩擦点（渲染注入、光标列、编辑位移、清场）即提炼源——抽象结果
**长在 Ed 上**（§3.3），不建独立类。semantic/diag 是纯样式层（不改布局），
Client 持缓存、editor 查询；唯 hint（注入文本）有布局+光标语义，属
编辑器核心接口。

**并入/保留的边界**：
- jsonrpc、lspio **并入 lsp.lua**（用户裁定：lsp.lua 包含所有功能，RPC/IO
  作内部类）——并入为机械平移，语义零改动；其独立测试随迁（§7.1）
- lsp_span 的 decode/clip 两个纯函数并入 lsp.lua 内部函数（模块私有，
  不建类；C 化时独立平移）
- json（C 绑定，原 yyjson 改名，2026-08-12 合并 starwing 版类型标记
  机制）保持外部 .so——RPC 的 encode/decode 依赖它；lspio 的进程桥
  依赖 luv（外部）。C 化时 RPC/IO 相继替换这些依赖

## 三、接口签名（C 化接口候选，逐条标记）

### 3.1 Protocol（= 现 lspclient.lua + 默认配置）

```lua
Protocol.new(opts)                 -- get_text/get_line/offset_pos/on_status
                                   -- + config?: 已知 section → 配置表（默认
                                   --   {Lua={hint={enable=true,setType=true}}}）
:on(method, fn)                    -- 通知处理器（publishDiagnostics 等）
:on_server(method, fn)             -- server→client 请求（可覆盖默认配置响应）
:start(argv, uri, langid, root)    -- spawn + initialize 握手
:request(method, params, cb)       -- 队列请求
:notify(method, params)
:poll()                            -- 泵 + 分发
:notify_edit(off, del, s)          -- 即时增量同步（编辑前状态换算）
:sync_full()                       -- undo/redo 整篇 didChange
:stop()
state / uri / version / capabilities   -- 公开只读字段
```

- **配置回答内移**：默认配置响应内置本类（`json.null` 使用点收拢），
  editor 的 `require "yyjson"` 消失。`on_server` 仍可整体覆盖。
- C 化候选：状态机 + pending 表（纯 C 结构）；UTF-16 换算（cellgrid 家族）。

### 3.2 Client（接入层）

```lua
Client.new(opts)
  -- opts: 协议四件套 get_text/get_line/offset_pos/on_status（转发 Protocol）
  --      + dcol_fn(line, bytecol)（UTF-16 位置→显示列，editor 配 tabstop）
  --      + viewport_fn() → {top, rows}（editor: scroll_line + term:size）
  --      + now_fn() → 墙钟秒（默认已实现: luv.hrtime()/1e9；测试: 假钟）
  --      + attrmap（semantic tokenType 名 → attr 表，editor 的 LSP_ATTRS；
  --        含可选 diag 键——回更：diag 底色 attr 由 editor 注入）
  --      + vtext（Ed 注入接口，见 §3.3）: { set = fn(line, list),
  --        clear = fn() } —— editor 传 ed 绑定闭包，Client 不直接碰 Ed
  --      + hint_idle?（秒，默认 tonumber(os.getenv("PT_HINT_IDLE")) or 1.0）
  --      + proto?（预建 Protocol，测试注入 fake）——回更：原设计由 Client
  --        自建 proto，实现补了注入点，TestClient 全用假注入
:start(argv, uri, langid, root)  -- 无 silent 参数（回更：silent 走 on_status 闭包）
:stop()                          -- 联动清场：清 hint/semantic/diag + vtext 槽
:on_edit(off, del, s)            -- Protocol.notify_edit + dirty 标记（位移归 Ed）
:resync()                        -- undo/redo：sync_full + 缓存全清 + vtext 槽全清
:tick()                          -- 主循环空闲：idle/视口变更/null 重试 → 重拉
:post_render()                   -- render 末尾：semantic dirty 且非 pending → 重拉
:query_spans(s, e) → {sem=…, diag=…}   -- 拉模式裁剪（editor 喂 merge_layers）
:diag_at(off) → span?            -- 状态栏 diag 消息
:status() → string               -- 状态栏右段（"running"/"starting"/…）
```

- **hint 业务（调度 + 解析）在 Client**：idle/重试/版本守卫/视口变更 →
  `proto:request(inlayHint)` → 响应解码 → `opts.vtext.set(line, list)`
  写入 Ed vtext 槽。**Client 不持有注入文本数据**（数据归属 Ed，位移归 Ed）——
  只持调度状态（dirty/pending/retry 预算）。
- **hint 编辑位移不在此**：Ed 核心职责（§3.3），docedit 漏斗自动执行。
- semantic/diag 缓存本类私有，editor 只读查询。
- C 化候选：span 解码/裁剪（lsp_span 平移）；调度状态机（pending/重试）。

**（实现回更）实现细节**：
- 调度字段经 `h_state(opts)` helper 初始化（`Client.new` 与 `Client:start`
  共用；重启时复位，不带 stale 行/重试预算）。`last_edit_t` 初值 -1e6
  ——启动即视为 idle，首帧即刷新。
- diag 解析（publishDiagnostics → UTF-16 范围解码 → 字节 span）handler 注册
  在 `Client:start`（经 `proto:on`），版本守卫丢乱序旧快照；attr 取
  `attrmap.diag or {underline=true}` 兜底。
- 缓存形态：`self.sem = {spans={}, dirty=true, pending=false}`、
  `self.diag = nil`（{version, spans}）。decode/clip 为 lsp.lua 模块私有函数
  `span_decode`/`span_clip`（原 lsp_span.lua 并入，文件已删）。
- `status()` 返回 `proto.state`（无 proto → "exited"）；editor 拼
  `"lsp:on"`（running）/ `"lsp:" .. status` / `"lsp: off"`。
- `tick` 视口 end 行 = `top + rows - 1`，**未 clamp 到 doc breaks**（Client
  无 breaks accessor，phantom 行请求无害——server 对越界行返回空，取舍）。
- `:start(argv, uri, langid, root)` 实际无 `silent` 参数（silent 走
  on_status 闭包，见 §五）；`Client:poll()` 转发 `proto:poll()`。

### 3.3 hint text 的 Ed 接口（编辑器核心注入文本抽象）

**概念**：注入文本（virt text，Neovim extmark 语义）是编辑器核心能力——
任何消费者（LSP hint、diag 内联、diff 标记…）写同一接口；渲染/光标/
编辑位移由核心统一处理。当前只 LSP hint 一个消费者，接口取最小形态，
**随摩擦演进**（用户裁定：从具体业务摩擦中提炼，不预设计）。

```lua
-- 数据：ed.vtexts = { [line] = { {dcol=, text=, style=}, ... } }（升序）
--   nil/空 = 无注入。dcol 为显示列（tabstop 展开后），注入不占文本字节。
Ed:set_vtext(line, list)       -- 整行原子替换（LSP 响应到达 / 清行）
Ed:clear_vtexts()               -- 全清（lsp off / 换文件）
-- docedit 漏斗内部自动执行（核心职责，C 化候选）：
Ed:shift_vtexts(off, del, s)    -- 编辑位移：同线位移/删区丢弃/跨线全清
-- 光标/渲染查询（唯一权威，替代现分散补偿）：
Ed:vtext_dcol(line, bytecol, at_start)  -- 显示列（含注入偏移；at_start=true
                                         --   插入间隙语义，同现 hint_offset）
Ed:screen_to_text_dcol(line, scol)      -- 文本列（减该行注入偏移，clamp）
```

**（实现回更）行为细节**：
- `set_vtext(line, list)`：list 为 nil 或空表 → 该行置 nil（实现：
  `if list and #list > 0 then 存 else nil end`）——"空即清"与设计同义。
- `vtext_dcol(line, bytecol, at_start)`：bytecol 为**行内字节列**（非 doc
  光标偏移）——实现读行文本 + `text_byte_to_dcol(text, bytecol, tabstop)`
  算出文本 dcol，再叠加"dcol 之前"的注入宽度（at_start 时 `>=` 即停，
  即 hint 首字符处插入落于 hint 前）。
- `shift_vtexts`：**跨线（off 与 off+del 不同行）或插入串含 `\n` → 全清**
  `self.vtexts = {}`；同线时用 `text_byte_to_dcol` 算编辑点 edcol，hint
  dcol < edcol 保留、落在删区 [edcol, edcol+del) 丢弃、其后整体平移 delta；
  结果空表存 nil。**插入语义：先 seek 到 off+del 读文本再还原 offset**，
  与 doc:edit 之前的状态一致。
- **条目含 `style` 字段**：render_line 前置设 `h.style = self.styles.dim`
  （render 时写入，非 Client 写入）。

- **渲染**：render_line 的 hints 参数来自 `ed.vtexts[line]`（render_line 保
  持纯函数，参数传入，不依赖 ed）。
- **光标**：render_cursor / move_vert 全走 `vtext_dcol` + `screen_to_text_dcol`。
- **C 化候选**：注入点表（per-line 排序数组）+ 位移（编辑漏斗）+ 列换算
  （显示列族，与 text_byte_to_dcol 同族）。

## 四、坐标模型（当前 bug 根因修复）

**原则**：注入偏移的计算只有 `vtext_dcol` / `screen_to_text_dcol` 两个函数，
全在 Ed（§3.3）；editor 任何光标路径都不得自行补偿。

```
垂直移动（jk，Neovim 语义）：
  dcol = ed:vtext_dcol(cur_line, byte_col, at_start)  -- 屏幕列（含注入）
  doc:seek("line", nlnum)
  text_dcol = ed:screen_to_text_dcol(nlnum, dcol)      -- 减目标行注入
  doc:seek("cur", dcol_to_byte(doc, nlnum, text_dcol))
  -- at_start = (mode == "INSERT")：插入间隙语义（现 hint_offset 第三参）

水平移动（h/l，字节级）不动：注入不占字节，光标字节即文本位置，
显示列由 render_cursor 调 vtext_dcol 统一算出。
```

- `screen_to_text_dcol`：目标屏幕列 → 按该行注入列表（升序 dcol，逐个减
  宽度）反查文本列；越过行尾 clamp。
- **消灭 13 个 lsp_\* 字段**：editor 只留 `self.lsp`（Client 对象）+
  `self.vtexts`（Ed vtext 槽，编辑器核心数据）。

**（实现回更）**：坐标模型与设计一致——move_vert 走 `vtext_dcol` +
`screen_to_text_dcol`（editor.lua L332-334），render_cursor 走 `vtext_dcol`。
另：`byte_to_dcol` 死代码已删（lsp 时代遗留，无调用点）。

## 五、数据流

```
编辑：docedit(off,del,s)
  → Ed:shift_vtexts(off,del,s)（vtext 槽位移，核心职责）
  → self.lsp:on_edit → Protocol.notify_edit（即时 didChange，编辑前状态）
                    → Client 内部 dirty 标记（semantic/hint 重拉）
主循环空闲：self.lsp:tick()
  → Client 内部：idle（now_fn 墙钟）/视口变更/null 重试 → proto:request(inlayHint)
  → 响应回调：版本守卫（proto.version 对比）→ 解码 → ed:set_vtext(line, list)
渲染：self.lsp:query_spans(s,e)（sem/diag 层，editor 合并自己的 hl/piece/visual）
     + ed.vtexts[line]（render_line 注入）
     + ed:vtext_dcol（render_cursor）
  render 末尾：self.lsp:post_render()（semantic 重拉）
状态栏：self.lsp:status()（右段）+ self.lsp:diag_at(cur_off)（中间段）
undo/redo：self.lsp:resync()（sync_full + Client 缓存全清 + vtext.clear() 清 vtext 槽）
```

**（实现回更）**：数据流与设计一致。确认细节：
- docedit 漏斗 = `shift_vtexts`（先于 `doc:edit`）+ `lsp:on_edit` +
  `hl:notify_edit`（editor.lua docedit，单漏斗）。
- undo/redo（u / <C-r>）均调 `lsp:resync()`；resync 内 `vtext.clear()` 经
  `opts.vtext` 注入闭包调 `Ed:clear_vtexts`。
- tick → `lsp:tick` 转发；render 末尾 → `lsp:post_render()`。
- 状态栏拼接：`"lsp:on"`（running）/ `"lsp:" .. status` / `"lsp: off"`；
  非静默启动的异常退出（exited）以瞬态 msg 报出（on_status 闭包）。

## 六、editor.lua 收敛清单

**删**：
- `require yyjson`、`require lsp_span`（lsp.lua 内部用）
- lsp_hint_decode / edit_hints / lsp_diag_at / hint_offset / resync_lsp /
  lsp_sem·lsp_diag·lsp_hints·lsp_hint_\*·last_edit_t·hint_idle 字段
- :lsp on/off/status 命令体（改调 Client）——命令保留，体量缩减
- lsp_start 内 workspace/configuration 回答 + diag 解析 + semantic 请求块

**（实现回更）`require luv` 与 `LSP_ATTRS` 保留**：设计原列删 luv，实际
保留——editor.lua 仍用 `luv.cwd()` 解析相对文件路径（L1232），属文件系统
职责非 LSP 侵入；LSP 侧（spawn/pipe/墙钟）的 luv 在 lsp.lua 内部。`LSP_ATTRS`
亦保留（semantic tokenType 名 → attr 表，Client 的 attrmap 注入源，加
`diag = ATTR_DIAG` 键，见 §3.2）。editor 侧 `require "luv"` 仅剩 cwd 一处。

**改**：
- `require "lsp"` → `self.lsp = lsp.Client.new(...)`（lsp_start 装配）+ 注入
  接口闭包（§3.3）
- **新增**：`self.vtexts = {}`（Ed vtext 槽）+ `Ed:set_vtext` /
  `Ed:clear_vtexts` / `Ed:shift_vtexts` / `Ed:vtext_dcol` /
  `Ed:screen_to_text_dcol`（§3.3，Shift/查询为内部或公开按摩擦定）
- docedit：`self:shift_vtexts(off, del, s)`（核心职责，先于 doc:edit）+
  `if self.lsp then self.lsp:on_edit(...) end`（单漏斗）
- u / <C-r>：`self.lsp:resync()`（替换 resync_lsp；vtext 槽清理由 Client 经
  vtext.clear 或 editor 直接 clear——按摩擦定）
- tick：`self.lsp:tick()`（替换现 tick 主体）
- render：层列表 sem/diag 来自 `self.lsp:query_spans`；hints 来自
  `self.vtexts[ld.line]`；末尾 `self.lsp:post_render()`
- render_cursor：`self:vtext_dcol(...)` 替换本地 display_col 计算
- move_vert：屏幕列语义（§四）
- render_status：`self.lsp:status()` / `self.lsp:diag_at(cur_off)`

**保留**：ext_lang / lsp_cmd（文件名→server argv，UI 决策）、main 的自动
lsp_start 调用、show_pieces/sel_start（editor 自有层）。

## 七、测试

### 7.1 lua/tests/lsp_test.lua（替代 lspclient_test.lua；jsonrpc_test/lspio_test
随迁——RPC/IO 用例改为 require "lsp" 取子类，断言不变）

四组，按类划分：

| 组 | 对象 | 手段 | 用例（平移 + 新增） |
|---|---|---|---|
| TestRPC | RPC（原 jsonrpc） | 纯函数/持久解码器（现 jsonrpc_test 全套） | 帧切分/半帧/enc 四形态/坏帧弃帧 |
| TestIO | IO（原 lspio） | luv 进程（现 lspio_test 全套） | spawn/泵/写队列回退/EOF |
| TestProto | Protocol | fake server 进程（现 lspclient_test 全套） | 握手/$hello 忽略/didChange 回执/UTF-16/诊断/停机 + **新增**：默认配置回答（Lua→hint，其余 null） |
| TestClient | Client | 假注入（fake proto 记录 request + 假 doc/视口/墙钟/假 vtext 记录 set 调用） | hint 调度：idle 触发/视口变更/null 重试预算/stale 版本守卫；响应→vtext.set 落槽；semantic 重拉（post_render）；diag_at；on_edit 双通道；resync 全清；start 失败清场 |

**（实现回更）用例数**：TestRPC 11 / TestIO 4 / TestProto 10 / TestClient 14
共 39（`just lua/lsp` 全绿）。TestClient 比设计表多出：testTickSkipsWhenTyping
（打字中跳过）、testTickWithoutNowFn（默认墙钟）、testRestartResetsHints
（h_state 重启复位）。jsonrpc_test.lua/lspio_test.lua 文件已删，用例随迁
lsp_test.lua 相应组。

### 7.2 editor_test.lua：TestVtext（Ed 注入接口，纯核心无 LSP）

hint text Ed 接口是编辑器核心能力，独立于 LSP 测试（fake term + 直接调用）：

| 用例 | 断言 |
|---|---|
| set/clear 生命周期 | vtext 槽写入/清空 |
| vtext_dcol（normal/insert 间隙语义） | 含注入的显示列 |
| screen_to_text_dcol | 反向换算 + 越过注入/行尾 clamp |
| **垂直移动保持屏幕列**（核心 bug） | 行 0 有注入、行 1 无：j 后光标字节对应屏幕列（等价 tmux 断言，单元层先行） |
| shift_vtexts | 同线位移/删区丢弃/跨线全清 |
| render_line 注入 | grid cell 断言（现有 TestHint:testInjectShift 平移） |

**（实现回更）实际 8 用例**（`just lua/ed` 全绿）：split 为
testSetAndClear / testVtextDcolNormalSkipsHint / testVtextDcolInsertGap /
testScreenToTextDcol / testJKKeepsScreenCol / testShiftVtextsSameLine /
testShiftVtextsCrossLineClears / testRenderLineInjectsVtext。

### 7.3 lua/tests/tmux_test.lua（显示行为集成测试，新增）

**动机**：显示行为（屏幕列、注入、滚动）的最终裁判是真实终端；手写
PTY+CSI 解析器 = 重造 tmux。tmux 提供：会话 PTY（new-session -d）、按键注入
（send-keys，转义序列用 `-H` 十六进制喂原始字节）、**解析好的屏幕**（
capture-pane -p）、光标坐标（display -p `#{cursor_x} #{cursor_y}`）。

```lua
-- helper 抽为 lua/tests/tmux.lua 模块（fake PTY：tmux 管终端/解析 CSI/吐屏幕）
tmux.new({cmd=, rows=, cols=, files={{path=,content=}}}) -- 写临时文件 + new-session
tmux:feed(...)                  -- send-keys（命名键 "Enter"/"Escape"）
tmux:raw(hex)                   -- 转义原始字节（-H 十六进制）
tmux:capture() → string[]       -- capture-pane -p（行数组，去尾空白）
tmux:cursor() → {x=,y=}         -- display -p `#{cursor_x} #{cursor_y}`（0 基）
tmux:wait(pred, timeout?)       -- poll 等稳定（默认 300×20ms）
tmux:gone() → bool              -- 会话已消失（进程退出）
tmux:kill()                     -- kill-session + 删临时文件
```

**fake LSP server**：`lsp_cmd` 增加环境变量覆盖 `PT_LSP_CMD`（lsp_cmd 内
`os.getenv("PT_LSP_CMD")` 优先），测试生成临时 fakelsp.lua（从 editor_test
的 fake_server 模板提炼，只处理 initialize/inlayHint/shutdown/didOpen），
会话命令：
`PT_LSP_CMD='lua /tmp/fakelsp.lua' lua editor.lua f.lua`
（tmux_test 另加 `PT_HINT_IDLE=0` 免等 idle 即刷，见 §3.2）

用例（断言即读屏幕，行为直白；**实现回更：实际 7 用例**，hint 渲染注入
断言并入 jk 用例——"int:hello" 文本）：

| 用例 | 行为断言 |
|---|---|
| testBasicRender | 行号/内容/状态栏文本（NORMAL） |
| testJKKeepsScreenCol | 行 0 字节 0（hint "int:" 注入）→ j 到行 1：光标 x 保持；屏幕行 1 文本 `int:hello` |
| testInsertGapCursor | insert：光标在 hint 首字符（x==4） |
| testEditNoTear | i 插入 X → hint 位移 → 屏幕 `Xint:hello` 无撕裂 |
| testScroll | G/gg：视口跟随 + 行号正确 |
| testStatusDiag | fakelsp 推 publishDiagnostics → 状态栏 `diag: boom` |
| testQuit | :q → 会话结束（gone） |

**时序注意**：send-keys 后 editor 主循环 getkey(100ms) 超时才 render——
`tmux:wait` 轮询光标/文本直到稳定（默认 300×20ms）。

### 7.4 editor_test.lua 清理

- 删 `require lspio` / `require yyjson`；TestHint 系列显示行为用例迁
  tmux_test 或 TestVtext（§7.2）；只留装配冒烟（lsp_start 注入假 argv
  失败路径、:lsp 命令语意）
- 剩余 LSP 断言改为行为断言（不改内部字段）

**（实现回更）**：TestHint 组名保留，仅剩 2 个**无进程**用例
（testSpawnFailSilentAndLoud / testNoServerMsg）；进程用例全删，显示行为
迁 tmux_test 或 TestVtext。editor_test 全量绿（98 用例）。

### 7.5 CI

- 新 job `lua-tmux`（ubuntu + macos，test.yml 已落地）：apt/brew 装 tmux；
  luarocks 装 luautf8 + luv（editor.lua 硬依赖，原 CI 未装）；tree-sitter
  不装——**editor.lua 顶层宽容**（pcall require，无则降级无高亮，hl.new
  判 nil）；跑 `just lua/tmux`（先 build 各 .so）
- windows：跳过（tmux 无）
- 覆盖报告：tmux_test 不参与 lcov（进程外）；lua 侧 cov 仍按 per-module
  （C 绑定 yyjson 等），lsp_test 跑功能测试（`just lua/lsp`）

## 八、C 化接口候选（重构完成后再评估）

| 候选 | 位置 | 形态 |
|---|---|---|
| JSON-RPC 帧 + 状态机 + pending | RPC / Protocol | C 结构 + 回调表 |
| UTF-16 换算 | Protocol | cellgrid 家族纯函数 |
| span 解码/裁剪 | lsp.lua 内部函数（原 lsp_span） | 纯 C 数组操作 |
| vtext 位移 + 屏幕列↔文本列 | Ed vtext 接口（§3.3） | 显示列族纯函数（现 lua 版在 editor.lua §2 已有 C 化注释） |
| 进程桥 | IO | luv 替换为 C spawn/pipe（平台适配层） |

## 九、决策记录

| 议题 | 决策 | 理由 |
|---|---|---|
| 类文件 | 单文件 lua/lsp.lua 四分类（RPC/IO/Protocol/Client） | 用户裁定：一个整体 Lua 文件负责 LSP，包含所有功能，类间接口良好 |
| 类名 | RPC / IO / Protocol / Client（无 lsp 前缀） | 用户裁定；名字非锁定 |
| jsonrpc/lspio 并入 | 并入 lsp.lua 作内部类 RPC/IO（机械平移，零语义改动） | 用户裁定：lsp.lua 包含所有功能 |
| lsp_span 并入 | decode/clip 作 lsp.lua 内部纯函数（不建类） | 极小；C 化时独立平移 |
| vtext 命名 | **vtext**（VSCode 同款）——ed.vtexts + set_vtext/clear_vtexts/shift_vtexts/vtext_dcol/screen_to_text_dcol | 用户裁定；虚拟文本抽象（virt text 语义），非 span（span=样式区间，不混淆）；popup/tooltip 将来若出现再提炼控件模型，不预设计 |
| Render 类 | **撤回**——暂不建类，hint 逻辑先进 Client | 用户裁定：Render 是摩擦层，从业务摩擦提炼结果长在 Ed 上 |
| hint text 接口 | **Ed 接口**（vtext 槽 + 位移 + 列换算），非 LSP 模块接口 | 注入文本是编辑器核心能力（virt text 语义），任何消费者复用；C 化候选 |
| 接入面 | Client，editor 只持一个对象（self.lsp） | 装配薄、生命周期联动、editor 不碰协议细节 |
| 数据归属 | 注入文本数据归 Ed（vtexts），Client 只持调度状态 | 位移/渲染/光标是核心职责；Client 是生产者 |
| 调度归属 | Client（tick/post_render） | 与协议生命周期联动；假注入可测 |
| 配置回答 | Protocol 内置默认（yyjson 收拢） | editor 的 yyjson require 消失 |
| 坐标 | vtext_dcol / screen_to_text_dcol 唯一权威（Ed） | 消灭补偿散落；Neovim 屏幕列语义 |
| 显示测试 | tmux（非手写 PTY+CSI） | tmux 现成 PTY + 屏幕解析；断言可读 |
| editor.lua treesitter | 顶层 pcall require 宽容 | CI 免装 tree-sitter；hl.new 已处理 nil |
| CI | tmux job（ubuntu/macos），windows 跳过 | tmux 无 windows |

**（实现回更）**：决策表与实现一致；vtext 命名行已含全部五个接口。补充实现
确认：`@class` 注解名随 §二 统一为 `lsp.*`；`require luv` 保留为 editor
侧文件系统职责（§六）；treesitter 宽容实现为
`local ok_ts, ts = pcall(require, "treesitter")` + `if not ok_ts then ts = nil end`
——`select(2, pcall)` 返回错误串（truthy），不能直接当 ts 用，需显式判 ok_ts。

## 十、风险与后续

- 类结构未锁死（用户明示）——本文档为演进起点，接口以实现为准
- Ed vtext 接口 §3.3 为初稿，**以摩擦为准演进**（用户裁定：业务摩擦中提炼，
  不预设计）——实现中如摩擦点与初稿冲突，以实际需要为准并回更本文档
- popup/tooltip 类"控件"暂不抽象（无需求，预设计即违背摩擦原则）；将来
  出现时若摩擦证明条目模型（{dcol,text,style}）不够，再升级内容模型
- semantic/diag 若出现"内联显示"需求（不进 merge_layers 纯样式），
  可能成为 vtext 接口的第二个消费者——届时验证接口通用性
- tmux 测试时序（轮询等待）引入偶发 flake——超时/重试参数需调优
- PT_LSP_CMD 是测试 hook，语义上属 UI 决策，C 化前保留 editor 侧
- jsonrpc_test/lspio_test 随迁是机械平移——实现时以原文件为基准逐测试迁移，
  不得顺手改语义（守住"并入零改动"承诺）

**（实现回更）**：
- `PT_LSP_CMD` hook 已落地（lsp_cmd 内 `os.getenv` 优先）；另加
  `PT_HINT_IDLE`（hint_idle 默认值 env，§3.2）——tmux_test 用它置 0 免等
  idle 即刷，测试 hook 均保留 editor 侧。
- jsonrpc_test/lspio_test 随迁完成（用例入 lsp_test.lua，原文件删除）。
- tmux 时序 flake 为真实现状：`wait` 默认 300×20ms，偶发下 `wait_screen`
  超时——参数可再调（已知取舍）。
