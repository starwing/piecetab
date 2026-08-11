# tmux 显示测试速查（brief）

本文件记录 tmux 显示行为测试的使用约定。测试文件：`lua/tests/editor_tmux_test.lua`；
框架模块：`lua/tests/tmux.lua`。运行：`just lua/ed-tmux`（无 tmux 时自动跳过）。

## 定位（两处测试的分工）

| 文件 | 测什么 | 断言对象 |
|---|---|---|
| `editor_test.lua` | 编辑器**逻辑**（光标/编辑/undo/模式/命令/坐标/位移/转义形态） | fake term + cellgrid cell |
| `editor_tmux_test.lua` | 编辑器**显示行为**（屏幕文本/光标坐标/滚动视口/状态栏/注入渲染） | tmux capture-pane + cursor |

**迁移原则**：断言"最终屏幕状态"的用例迁 tmux（可读性高、覆盖真实渲染管线）；
断言"转义序列/网格 diff 形态"（如 `\27[1S` SU、scroll region、帧稳定）留
editor_test——tmux 已消费转义序列，这类断言在 tmux 层不可见。

## tmux.lua 接口

```lua
local tmux = require "tmux"
local s = tmux.new({ cmd = "命令", rows = 24, cols = 80,
                     files = { { path = "f", content = "..." } } })
s:feed("j", "i", "Enter")   -- tmux 语义键（字符/键名）
s:raw("1b5b41")             -- 原始转义字节（hex 对）；feed 不做 hex 猜测
s:capture() → string[]      -- 屏幕行数组（尾随空格已 trim）
s:cursor() → { x, y }       -- 光标列/行（0-based pane 内）
s:wait(pred, timeout)       -- 轮询 pred(s)，默认 300×20ms
s:gone() → boolean          -- 会话已退出
s:kill()                    -- 杀会话 + 删临时文件（tearDown 兜底）
```

- `feed` 与 `raw` 分离：`"a"~"f"` 是合法 hex 字符，feed 里做 hex 猜测会误判
- files 在 spawn 前写入、kill 时删除；会话名 `pttest<N>` 自增唯一

## 用例模式（editor_tmux_test.lua 内）

- **spawn_ed(content, hints, opts)**：临时文件 + fakelsp 脚本 + `PT_LSP_CMD`/
  `PT_HINT_IDLE=0` 启动真实 editor.lua；等待状态栏 `lsp:on`（握手往返）
- **fakelsp**：用 `lsp.RPC.decoder` + `json` 帧编解码的假 LSP server 脚本
  （initialize/inlayHint/shutdown；`diag` 参数推 publishDiagnostics）
- **tearDown**：所有 session 统一 kill（失败也执行，防泄漏）
- 临时文件名要短（长 `/var/folders/...` 路径会截断状态栏）

## 时序注意

- editor 主循环 `getkey(100ms)` 超时才 render——按键后必须 `s:wait(...)` 轮询
  到目标状态（不要立即 capture）
- hint 请求经 idle 调度：测试用 `PT_HINT_IDLE=0` 关闭 debounce
- wait 条件用屏幕状态（文本出现/光标到位），不用 sleep

## 断言技巧

- capture 行含行号前缀：`"  1 line 1"`（3 宽右对齐 + 空格）——匹配用
  `^%s*%d+`
- 状态栏在最后一行（rows-1，0-based `[24]`）
- 光标 x/y 是 0-based pane 坐标；长行截断到 cols-1
- tab 展开：默认 tabstop 4，`"a\tb"` 显示 `"a   b"`
- 行尾截断：pane 宽度内填满（`#row == cols`）

## 约定

- 用例文件命名 `editor_tmux_test.lua`（被测模块 + tmux 驱动方式）；
  justfile target `ed-tmux`；`just lua/test` 全量含它
- 新增显示行为用例优先放 tmux 层；editor_test 保持纯逻辑
