# editor.lua 演进设计（class 化 + 注册表 + 可测试性）

> 状态：讨论已定案，待实施。背景：editor.lua 从简单 demo 膨胀至 866 行
> 单文件——term/ed 单例名字空间 + 全局私有变量（tk_instance），命令靠
> `normal_cmds` 表 + if/elseif 链。目标：正规化 class 写法、LuaLS 标注
> 完全、方便接入 spantree/highlighter 等外围库、任何显示/行为 bug 可在
> editor_test.lua 重现。

## 一、决策（与用户讨论确认）

| 议题 | 决策 |
|---|---|
| 模块组织 | **单文件内部正规化**（保持 demo 属性，不拆多文件） |
| 实例化 | **多实例 + 依赖注入**：`Ed.new(content, term, grid)` / `Ed.open(filename, term, grid)` |
| 构造参数 | **位置参数，不用 opts 表**（防 setmetatable 混入多余字段） |
| 按键/命令 | **注册表 API**：`e:keymap(mode, key, fn)` / `e:command(name, fn)`，内置同样走注册表 |
| 高亮模块 | 现状 + 类型标注，spantree 落地时再重构（YAGNI） |
| 类型定义 | 新建 `lua/lua-utf8.d.lua`（luautf8 仓库无官方定义，按 README v0.2.x 公开 API 写，可回推上游）；`.luarc.json` workspace.library 修正为四个 `.d.lua` 实际路径 |
| 测试 | luaunit（与 cg/tf/pt 测试一致），`just lua-ed` 不变 |

## 二、类结构

```lua
--- @class editor.Term          -- 文件内 class，不导出（内部实现）
--- @class editor.Ed            -- 模块唯一导出（"整个文件都是 editor"）
```

模块 `return Ed`。Term 是内部实现细节——后面计划用 unibilium 替代物
换掉 termfeed 时，只改文件内部 + `Ed.newterm` 工厂，模块导出面不动。

### Term（封装 termfeed + 终端 I/O，不导出）

- `Term.new(opts)`：**纯构造无终端副作用**——创建 termfeed 实例
  （`tf.new()` + `FLAG_DELBS`，安全）、存 out/size。opts 字段：
  `out`（默认 io，须有 write/flush 方法）、`size`（默认 `cg.winsize(1)`）
- `term:enter()`：alt screen（`?1049h`）+ 隐藏光标（`?25l`）+ `tf:raw(0)`，
  只在 main 调用（raw 会改真终端，测试环境禁用）
- `term:leave()`：`tf:cooked()`/`tf:delete()` + 恢复光标/清屏/退 alt screen
- `term:write(s)` / `term:flush()`：统一输出口（render 全部 io.write 改此）
- `term:getkey()`：`waitkey(0,-1)` → `format()`，nil 当超时/失败
- `term:size()` → rows, cols（注入函数）
- `term:move(row, col)`、style 常量 REVERSE/DIM/RESET

原 `tk_instance`（全局）→ `Term` 实例字段 `tf`，LuaLS 标
`--- @type termfeed.State`。

### Ed

字段（全部实例化）：`doc/filename/mode/cmdline/msg/saved_vid/pending_key/
scroll_line/grid/term/tabstop(=4)/log/done`。

- `Ed.new(content?, term?, grid?)`：初始内容（缺省空文档），term 缺省
  `Ed.newterm()`（鸭子类型，测试传 fake 表），grid 缺省 `cg.new()`
- `Ed.open(filename, term?, grid?)`：读文件
- `Ed.newterm()`：默认终端工厂——未来换 unibilium 替代物的唯一替换点
- 可配项（tabstop、log）为公开实例字段，测试 `e.log = nil` 禁日志
- main 不单持 term 变量：`e.term:enter()` / `e.term:getkey()` /
  `e.term:leave()` 全走实例字段

## 三、按键/命令注册表

```lua
--- @alias editor.Mode "normal"|"insert"|"command"
--- @alias editor.Key string          -- termfeed format，如 "j"、"<C-r>"、"<Up>"
--- @alias editor.KeymapFn fun(self: editor.Ed, key: editor.Key)
--- @alias editor.CommandFn fun(self: editor.Ed, arg: string, bang: boolean)

e.keymaps  = { normal = {}, insert = {}, command = {} }  -- key -> KeymapFn
e.commands = {}                                          -- name -> CommandFn

e:keymap(mode, key, fn)   -- 注册按键，返回 self
e:command(name, fn)       -- 注册 :命令（不含冒号），返回 self
```

内置按键/命令全部走同一机制注册。分发：

- `e:dispatch(key)`：按 `e.mode` 查 `mode_dispatch[mode](self, key)`
- 每模式 = **查表 + fallback**：
  - normal：查 keymaps.normal → 未命中无操作；两键组合（gg/dd）
    泛化：**key 是某组合键前缀且自身无绑定 → 等下一键**（新增 zz 等
    组合键零改动）
  - insert：查 keymaps.insert → 未命中 fallback 插入文本字符
    （`<...>` 控制键过滤）
  - command：查 keymaps.command → 未命中 fallback 追加 cmdline
    （单可打印字符）

**:命令解析（顺带修死代码 bug）**：现 `cmd:match("^(%a+)(.*)")` 使
`cmdname == "q!"` 永不命中（q! 被拆成 q+!，行为碰巧正确但语义错乱）。
新解析：

```lua
local name, bang, arg = self.cmdline:match("^(%a+)(!?)(.*)")
arg = arg:match("^%s*(.*)")
-- commands[name](self, arg, bang ~= "")；未命中 msg = "Unknown: :"..cmdline
```

## 四、渲染与主流程

- `e:render()`：主体流程不变（scroll clamp → lines_data → grid 绘制 →
  diff 输出），所有 `io.write` 改 `term:write`
- `e:render_status()`：status bar（现 render 尾部 if/else）
- `e:render_cursor()`：光标计算与定位
- **退出路径**：`e:quit()` 置 `e.done = true`（替换 `:q` 内 `os.exit`，
  原实现绕过 pcall 清理链）

```lua
local function main(argv)
  local e = argv[1] and Ed.open(argv[1]) or Ed.new()
  e.term:enter()
  local ok, err = pcall(function()
    while not e.done do
      e:render()
      e:dispatch(e.term:getkey())
    end
  end)
  e.term:leave()
  -- 错误处理不变（RESET + stderr + exit 1）
end
```

## 五、测试（luaunit）

harness（editor_test.lua 文件内 helper，不再写 tmp 文件/hack io.output）：
fake term 只需 render 用到的三个方法（鸭子类型，无需 Term 实例）：

```lua
local ROWS, COLS = 6, 40
local function make_ed(content)
  local term = { s = "", write = function(t, x) t.s = t.s .. x end,
                 flush = function() end,
                 size = function() return ROWS, COLS end }
  local e = Ed.new(content, term)
  e.log = nil
  return e
end
```

断言三维度：
- `e.grid:cell(r, c)` → cp, style（cg 自带读取，屏幕矩阵断言）
- `out.s` 字节流（CSI/status bar 断言）
- `e.doc` / `e.mode` / `e.scroll_line` 状态断言

既有 7 个测试（scroll 回归 4 + 基础 ops 3）原样改写保留，作为行为
回归网。新增：每个内置按键/命令一个断言组（keymap 注册、命令解析含
q! 修复、模式切换、pending 组合键）。

## 六、遗留（本期不做）

- hl 模块：现状 + 标注；spantree 落地时重构为可替换回调
  （render 通过 ed.highlighter 取每行 segments）
- highlighter：不建接口，等引擎选型
- 多文件拆分：维持单文件，膨胀到 ~900 行再议
- 文档：README editor 小节更新（Ed.new/open 用法 + 外围库挂载示例）

## 六b、C 模块孵化候选（demo 定位：C 库孵化平台）

editor.lua 是 C 模块库的 demo——cellgrid/termfeed 均由 editor demo 需求
孵化。当前仍留在 Lua 侧的计算，凡属"库级算法"均打 `TODO(C)` 标记，
按需孵化：

| 候选 | 位置 | 说明 |
|---|---|---|
| 字符移动原语 | `cursor_move_char` + `utf8_char_len`/`utf8_prev_start` | UTF-8 字符边界 walk；可入 pt 或独立 C 模块（配合 undo 对齐） |
| 显示列换算 | `text_byte_to_dcol`/`text_dcol_to_byte`（tab 展开 + 宽字符） | cellgrid 家族候选；render_line 的 tab 逻辑（Task 5 迁移）同属此候选 |
| 单词移动 | `move_word_forward/backward` + `word_class` | 暂留 Lua（vim 语义属编辑器逻辑）；若入 kana/多语言分词再 C 化 |
| 渲染管线 | render_line 的 style 批量 putline | spantree 落地后与 highlighter 合并进 C 渲染路径 |

`word_class` 为纯查表小函数，永久留 Lua 亦可，不标记。

## 七、实施顺序

1. `lua-utf8.d.lua` + `.luarc.json`（已完成）
2. 重写 editor.lua（Term → Ed 结构 → 注册表 → render → main）
3. 改写 editor_test.lua（既有 7 测试先通过）
4. 新增按键/命令测试组
5. `just lua-ed` 全绿 + README 更新
