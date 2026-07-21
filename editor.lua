#!/usr/bin/env lua
-- editor.lua -- 基于 piecetab 的终端文本编辑器（spantree 验证平台）
-- 用法: lua editor.lua [file]

package.cpath = package.cpath ..
    ";./build/lua55/?.so;./build/luajit/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
local pt = require("piecetab")
local utf8 = require("lua-utf8")

-- ================================================================
-- Logging (writes to editor.log for debugging)
-- ================================================================

local logfile = nil
local function edlog(fmt, ...)
  if not logfile then
    local f = io.open("editor.log", "w")
    if f then
      f:setvbuf("line")
      logfile = f
    end
  end
  if logfile then
    logfile:write(string.format(fmt, ...) .. "\n")
  end
end

-- ================================================================
-- Section 1: Terminal control
-- ================================================================

local term = {}

function term.init()
  io.write("\27[?1049h") -- alt screen
  io.write("\27[?25l")   -- hide cursor
  io.flush()
  os.execute("stty raw -echo min 1 time 1")
end

function term.shutdown()
  os.execute("stty -raw echo 2>/dev/null")
  io.write("\27[?25h")   -- show cursor
  io.write("\27[2J")     -- clear alt screen
  io.write("\27[?1049l") -- exit alt screen
  io.flush()
end

--- @return integer row, integer cols
function term.size()
  local f = io.popen("stty size 2>/dev/null")
  if f then
    local s = f:read("*a")
    f:close()
    local r, c = s:match("^(%d+) (%d+)")
    if r then return tonumber(r)|0, tonumber(c)|0 end
  end
  return 24, 80
end

function term.move(row, col)
  io.write(string.format("\27[%d;%dH", row, col))
end

function term.clear()
  io.write("\27[2J")
end

function term.clearline()
  io.write("\27[K")
end

-- style codes
term.REVERSE  = "\27[7m"
term.DIM      = "\27[2m"
term.RESET    = "\27[0m"

local CSI_MAP = {
  A = "up",
  B = "down",
  C = "right",
  D = "left",
  H = "home",
  F = "end",
  ["1~"] = "home",
  ["4~"] = "end",
  ["5~"] = "pageup",
  ["6~"] = "pagedown",
  ["3~"] = "delete",
}

local function csi_read()
  local seq = ""
  while true do
    local b = io.stdin:read(1)
    if not b then return nil end
    seq = seq .. b
    local term = seq:match("[A-Za-z~]$")
    if term then
      local key = seq:match("^%[(.*)" .. term) or ""
      local full = key .. term
      return CSI_MAP[full] or ("csi:" .. full)
    end
  end
end

local pushback = nil

function term.getkey()
  if pushback then
    local k = pushback
    pushback = nil
    return k
  end
  local c = io.stdin:read(1)
  if not c then return nil end
  local b = c:byte()
  -- ESC
  if b == 0x1b then
    local next = io.stdin:read(1)
    if not next then return "escape" end
    if next:byte() == 0x5b then
      local csi = csi_read()
      return csi or "escape"
    end
    pushback = next:byte() == 0x1b and "escape" or next
    return "escape"
  end
  -- control characters
  if b == 0x0d or b == 0x0a then return "enter" end
  if b == 0x08 or b == 0x7f then return "backspace" end
  if b == 0x09 then return "tab" end
  if b == 0x0c then return "ctrl-l" end
  if b == 0x12 then return "ctrl-r" end
  if b == 0x03 then return "ctrl-c" end
  -- UTF-8 multibyte: read continuation bytes
  if b >= 0xc0 and b <= 0xfd then
    local n = 0
    if b < 0xe0 then
      n = 1
    elseif b < 0xf0 then
      n = 2
    else
      n = 3
    end
    for _ = 1, n do
      local cb = io.stdin:read(1)
      if not cb then break end
      c = c .. cb
    end
  end
  return c
end

-- ================================================================
-- Section 2: Editor engine
-- ================================================================

local ed = {}

function ed.init(filename)
  local content = ""
  if filename then
    local f = io.open(filename, "r")
    if f then
      content = f:read("*a"); f:close()
    end
  end
  ed.doc = content ~= "" and pt.doc(content) or pt.doc(nil)
  ed.filename = filename
  ed.mode = "NORMAL"
  ed.cmdline = ""    -- command-line buffer for ":" mode
  ed.msg = ""        -- status message (transient)
  ed.dirty = false   -- unsaved changes since last save
  ed.scroll_line = 0 -- first visible line (0-based)
  edlog("init: file=%s lines=%d bytes=%d",
    filename or "(new)", ed.doc:breaks(), #ed.doc)
end

-- Text manipulation helpers

local function word_class(byte)
  if byte >= 48 and byte <= 57 then return 1 end  -- digit
  if byte >= 65 and byte <= 90 then return 1 end  -- upper
  if byte >= 97 and byte <= 122 then return 1 end -- lower
  if byte == 95 then return 1 end                 -- underscore
  return 0
end

local function move_word_forward(doc)
  local saved = doc:offset()
  local lnum = doc:line()
  doc:seek("line", lnum)
  local line = doc:read("l") or ""
  doc:seek("set", saved)
  local col = doc:column()
  local len = #line
  local i = col + 0
  -- skip current word or space
  if i < len then
    local cls = word_class(line:byte(i + 1))
    while i < len and word_class(line:byte(i + 1)) == cls do i = i + 1 end
    -- skip whitespace
    while i < len and word_class(line:byte(i + 1)) == 0 and line:byte(i + 1) == 32 do i = i + 1 end
  end
  doc:seek("cur", i - col)
end

local function move_word_backward(doc)
  local saved = doc:offset()
  local lnum = doc:line()
  doc:seek("line", lnum)
  local line = doc:read("l") or ""
  doc:seek("set", saved)
  local col = doc:column()
  local i = col - 1
  -- skip whitespace
  while i > 0 and line:byte(i + 1) == 32 do i = i - 1 end
  if i >= 0 then
    local cls = word_class(line:byte(i + 1))
    while i >= 0 and word_class(line:byte(i + 1)) == cls do i = i - 1 end
  end
  doc:seek("cur", (i + 1) - col)
end

-- Rendering

function ed.render()
  io.write("\27[?25l")
  local saved_off = ed.doc:offset()
  local rows, cols = term.size()
  local visrows = rows - 1
  local total_lines = ed.doc:breaks()
  local lnum_width = math.max(3, tostring(total_lines):len())
  local text_width = cols - lnum_width - 2
  if text_width < 1 then text_width = 1 end
  edlog("render: size=%dx%d lnumw=%d textw=%d scroll=%d cur=%d,%d total=%d",
    rows, cols, lnum_width, text_width, ed.scroll_line,
    ed.doc:line(), ed.doc:column(), total_lines)

  -- clamp scroll
  local cur_line = ed.doc:line()
  if cur_line < ed.scroll_line then
    ed.scroll_line = cur_line
  elseif cur_line >= ed.scroll_line + visrows then
    ed.scroll_line = cur_line - visrows + 1
  end
  if ed.scroll_line < 0 then ed.scroll_line = 0 end

  -- === text area (rows 1..rows-1) ===
  ed.doc:seek("line", ed.scroll_line)
  local line_idx = ed.scroll_line - 1
  for line in ed.doc:lines() do
    line_idx = line_idx + 1
    if line_idx >= ed.scroll_line + visrows then break end
    local screen_row = line_idx - ed.scroll_line + 1

    term.move(screen_row, 1)
    term.clearline()

    -- line number
    io.write(term.DIM)
    io.write(string.format("%" .. lnum_width .. "d ", line_idx + 1))
    io.write(term.RESET)

    -- line content
    local display = line:gsub("\t", "    ")
    if #display > text_width then
      display = display:sub(1, text_width)
    end
    if line_idx == 5 then
      edlog("  render line %d: len=%d [%s]", line_idx, #display, display:sub(1, 60))
    end
    io.write(display)
    if #display < text_width then
      io.write(string.rep(" ", text_width - #display))
    end
  end

  -- clear remaining text rows (beyond visible lines)
  local drawn_rows = math.min(total_lines, ed.scroll_line + visrows)
  drawn_rows = drawn_rows - ed.scroll_line
  for row = drawn_rows + 1, visrows do
    term.move(row, 1)
    term.clearline()
  end

  -- === status bar (last row) ===
  term.move(rows, 1)
  if ed.mode == "COMMAND" then
    io.write(term.REVERSE)
    io.write(":" .. ed.cmdline)
    local pad = cols - utf8.width(":" .. ed.cmdline) - 1
    if pad > 0 then io.write(string.rep(" ", pad)) end
    io.write(term.RESET)
  else
    local dirty_mark = ed.dirty and "[+] " or ""
    local linestr = string.format("L%d,%d", cur_line + 1, ed.doc:column() + 1)
    local left = string.format(" %s%s %s  %s ", dirty_mark,
      ed.filename or "[No Name]", ed.mode, linestr)
    local msg_part = ""
    if #ed.msg > 0 then msg_part = " " .. ed.msg end
    local pad = cols - utf8.width(left) - utf8.width(msg_part) - 1
    if pad < 0 then pad = 0 end
    io.write(term.REVERSE)
    io.write(left .. string.rep(" ", pad) .. msg_part .. " ")
    io.write(term.RESET)
  end

  -- === cursor position ===
  ed.doc:seek("set", saved_off)
  cur_line = ed.doc:line()
  local cur_screen_row = cur_line - ed.scroll_line + 1
  if cur_screen_row < 1 then cur_screen_row = 1 end
  if cur_screen_row > rows - 1 then cur_screen_row = rows - 1 end

  -- Compute display column for cursor
  local saved = ed.doc:offset()
  ed.doc:seek("line", cur_line)
  local cur_line_text = ed.doc:read("l") or ""
  ed.doc:seek("set", saved)
  local byte_col = ed.doc:column()
  local pre_text = cur_line_text:sub(1, byte_col)
  pre_text = pre_text:gsub("\t", "    ")
  local display_col = utf8.width(pre_text)

  local cur_screen_col = display_col + lnum_width + 2
  if cur_screen_col > cols then cur_screen_col = cols end

  term.move(cur_screen_row, cur_screen_col)
  io.write("\27[?25h")
  io.flush()
end

-- Normal mode commands

local normal_cmds = {}

-- helper: end-of-text column for line (excludes trailing \n)
local function line_endcol(ed, lnum)
  local llen = ed.doc:linelen(lnum)
  if llen > 0 and lnum < ed.doc:breaks() - 1 then llen = llen - 1 end
  return llen
end

function normal_cmds.h(ed) ed.doc:seek("cur", -1) end

function normal_cmds.l(ed) ed.doc:seek("cur", 1) end

function normal_cmds.j(ed)
  local lnum = ed.doc:line()
  if lnum >= ed.doc:breaks() - 1 then return end
  local col = ed.doc:column()
  ed.doc:seek("line", lnum + 1)
  local nlen = ed.doc:linelen(lnum + 1)
  if nlen > 0 and lnum + 1 < ed.doc:breaks() - 1 then nlen = nlen - 1 end
  ed.doc:seek("cur", math.min(col, nlen))
end

function normal_cmds.k(ed)
  local lnum = ed.doc:line()
  if lnum <= 0 then return end
  local col = ed.doc:column()
  ed.doc:seek("line", lnum - 1)
  local plen = ed.doc:linelen(lnum - 1)
  if plen > 0 and lnum - 1 < ed.doc:breaks() - 1 then plen = plen - 1 end
  ed.doc:seek("cur", math.min(col, plen))
end

function normal_cmds.w(ed) move_word_forward(ed.doc) end

function normal_cmds.b(ed) move_word_backward(ed.doc) end

normal_cmds["0"] = function(ed)
  ed.doc:seek("line", ed.doc:line())
end

normal_cmds["$"] = function(ed)
  local lnum = ed.doc:line()
  ed.doc:seek("line", lnum)
  ed.doc:seek("cur", line_endcol(ed, lnum))
end

function normal_cmds.gg(ed)
  ed.doc:seek("line", 0)
end

function normal_cmds.G(ed)
  ed.doc:seek("line", ed.doc:breaks() - 1)
end

function normal_cmds.x(ed)
  ed.doc:splice(1, "")
end

function normal_cmds.dd(ed)
  local lnum = ed.doc:line()
  local llen = ed.doc:linelen(lnum)
  ed.doc:seek("line", lnum)
  ed.doc:remove(llen)
end

function normal_cmds.i(ed) ed.mode = "INSERT" end

function normal_cmds.a(ed)
  ed.doc:seek("cur", 1); ed.mode = "INSERT"
end

function normal_cmds.o(ed)
  local lnum = ed.doc:line()
  ed.doc:seek("line", lnum)
  ed.doc:seek("cur", line_endcol(ed, lnum))
  ed.doc:append("\n")
  ed.mode = "INSERT"
end

function normal_cmds.O(ed)
  ed.doc:seek("line", ed.doc:line())
  ed.doc:insert("\n")
  ed.mode = "INSERT"
end

function normal_cmds.u(ed)
  ed.doc:undo()
  edlog("undo: offset=%d", ed.doc:offset())
  ed.msg = ""
end

normal_cmds["ctrl-r"] = function(ed)
  ed.doc:redo()
  edlog("redo: offset=%d", ed.doc:offset())
  ed.msg = ""
end

normal_cmds[":"] = function(ed)
  ed.mode = "COMMAND"
  ed.cmdline = ""
end

normal_cmds["up"] = function(ed) normal_cmds.k(ed) end
normal_cmds["down"] = function(ed) normal_cmds.j(ed) end
normal_cmds["left"] = function(ed) normal_cmds.h(ed) end
normal_cmds["right"] = function(ed) normal_cmds.l(ed) end

-- Command-line execution

local function cmd_save(ed)
  if not ed.filename then
    ed.msg = "No filename"
    edlog("save: no filename")
    return
  end
  local f = io.open(ed.filename, "w")
  if not f then
    ed.msg = "Cannot write: " .. ed.filename
    edlog("save: FAIL %s", ed.filename)
    return
  end
  local data = ed.doc:dump()
  f:write(data)
  f:close()
  ed.dirty = false
  ed.msg = '"' .. ed.filename .. '" written'
  edlog("save: %s %d bytes", ed.filename, #data)
end

local function cmd_quit(ed)
  term.shutdown()
  os.exit(0)
end

local function cmd_wq(ed)
  cmd_save(ed)
  cmd_quit(ed)
end

local function cmd_edit(ed, arg)
  if not arg or arg == "" then
    ed.msg = "No filename"
    edlog("edit: no filename")
    return
  end
  local f = io.open(arg, "r")
  local content = ""
  if f then
    content = f:read("*a"); f:close()
  end
  ed.doc = content ~= "" and pt.doc(content) or pt.doc(nil)
  ed.filename = arg
  ed.dirty = false
  ed.scroll_line = 0
  ed.msg = '"' .. arg .. '" loaded, ' .. ed.doc:breaks() .. " lines"
  edlog("edit: %s lines=%d bytes=%d", arg, ed.doc:breaks(), #ed.doc)
end

local function exec_command(ed)
  local cmd = ed.cmdline
  edlog("exec: :%s", cmd)
  ed.mode = "NORMAL"
  ed.cmdline = ""
  local cmdname, arg = cmd:match("^(%a+)(.*)")
  arg = arg and arg:match("^%s+(.*)") or ""
  if cmdname == "w" then
    cmd_save(ed)
  elseif cmdname == "q" then
    cmd_quit(ed)
  elseif cmdname == "wq" then
    cmd_wq(ed)
  elseif cmdname == "q!" then
    cmd_quit(ed)
  elseif cmdname == "e" then
    cmd_edit(ed, arg)
  else
    ed.msg = "Unknown: :" .. cmd
  end
end

-- Insert mode handlers

local function insert_key(ed, key)
  if key == "escape" then
    ed.mode = "NORMAL"
    ed.doc:commit()
    ed.doc:seek("cur", -1)
    ed.dirty = true
    edlog("insert: ESC -> NORMAL, commit")
    ed.msg = ""
  elseif key == "backspace" then
    local off = ed.doc:offset()
    if off > 0 then
      ed.doc:seek("cur", -1)
      ed.doc:splice(1, "")
    end
  elseif key == "delete" then
    ed.doc:splice(1, "")
  elseif key == "enter" then
    ed.doc:append("\n")
  elseif key == "tab" then
    ed.doc:append("\t")
  elseif key == "ctrl-c" then
    ed.mode = "NORMAL"
    ed.msg = ""
  elseif type(key) == "string" and #key > 0 then
    -- printable character
    local b = key:byte(1)
    if b >= 32 and b < 127 then
      ed.doc:append(key)
    elseif b >= 0xc0 then
      -- UTF-8 multibyte
      ed.doc:append(key)
    end
  end
end

-- Normal mode dispatch

local function normal_key(ed, key)
  local handler = normal_cmds[key]
  if handler then
    handler(ed)
    ed.msg = ""
  elseif key == "escape" or key == "ctrl-c" then
    ed.msg = ""
  end
end

-- Command mode

local function command_key(ed, key)
  if key == "escape" or key == "ctrl-c" then
    ed.mode = "NORMAL"
    ed.cmdline = ""
  elseif key == "enter" then
    exec_command(ed)
  elseif key == "backspace" then
    ed.cmdline = ed.cmdline:sub(1, -2)
  elseif type(key) == "string" and utf8.len(key) == 1 then
    local b = key:byte(1)
    if b >= 32 and b <= 126 then
      ed.cmdline = ed.cmdline .. key
    end
  end
end

-- Top-level dispatch

function ed.dispatch(key)
  if not key then return end
  edlog("key: mode=%s key=%s off=%d line=%d col=%d",
    ed.mode, key, ed.doc:offset(), ed.doc:line(), ed.doc:column())
  if ed.mode == "INSERT" then
    insert_key(ed, key)
  elseif ed.mode == "NORMAL" then
    normal_key(ed, key)
  elseif ed.mode == "COMMAND" then
    command_key(ed, key)
  end
end

-- ================================================================
-- Section 3: Main
-- ================================================================

local function main(argv)
  local filename = argv[1]
  ed.init(filename)
  term.init()

  local function cleanup()
    term.shutdown()
  end

  -- Catch exit signals (raw mode: no signals, but just in case)
  local ok, err = pcall(function()
    while true do
      ed.render()
      local key = term.getkey()
      ed.dispatch(key)
    end
  end)

  cleanup()
  if not ok and err then
    io.write(term.RESET)
    io.stderr:write("Error: " .. tostring(err) .. "\n")
    os.exit(1)
  end
end

main(arg)
