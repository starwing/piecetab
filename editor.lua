#!/usr/bin/env lua
-- editor.lua -- 基于 piecetab 的终端文本编辑器（spantree 验证平台）
-- 用法: lua editor.lua [file]

package.cpath = package.cpath ..
    ";./build/lua55/?.so;./build/luajit/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
local pt = require("piecetab")
local cg = require("cellgrid")

local utf8 = require("lua-utf8")
local termkey = require("termkey")

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
-- Section 1: Terminal control (powered by termkey)
-- ================================================================

local term = {}
--- @type termkey.Termkey
local tk_instance = nil

function term.init()
  io.write("\27[?1049h") -- alt screen
  io.write("\27[?25l")   -- hide cursor
  io.flush()
  tk_instance = assert(termkey.new(0))
  tk_instance:setcanonflags("delbs")
  tk_instance:start()
end

function term.shutdown()
  if tk_instance then
    tk_instance:stop()
    tk_instance:delete()
  end
  io.write("\27[?25h")   -- show cursor
  io.write("\27[2J")     -- clear alt screen
  io.write("\27[?1049l") -- exit alt screen
  io.flush()
end

--- @return integer row, integer cols
function term.size()
  local r, c = cg.winsize(1) -- stdout fd
  if r and c then return r, c end
  return 24, 80
end

function term.move(row, col)
  io.write(string.format("\27[%d;%dH", row, col))
end

-- style codes
term.REVERSE = "\27[7m"
term.DIM     = "\27[2m"
term.RESET   = "\27[0m"

--- Read one key (blocking). Returns termkey-formatted key string for dispatch.
function term.getkey()
  local tk = tk_instance
  local r = tk:waitkey()
  if r ~= "KEY" then return nil end
  return tk:format(termkey.FORMAT_VIM)
end

-- ================================================================
-- Section 2: Cell grid (frame buffer with scroll-aware diff)
-- ================================================================

-- style ID constants
local STYLE_NORMAL = 0
local STYLE_DIM    = 1
local STYLE_GRAY   = 3

-- diff style table
local DIFF_STYLE   = {
  [0] = "\27[0m",        -- RESET
  [1] = "\27[2m",        -- DIM
  [3] = "\27[48;5;237m", -- gray bg
}

-- ================================================================
-- Section 3: Highlight module (piece-based span coloring)
-- ================================================================

local hl           = {}

--- Build array of {offset, length, kind} from piece boundaries.
--- Uses doc cursor (includes uncommitted edits), restores position.
function hl.build_regions(doc)
  local saved = doc:offset()
  local regions = {}
  local off = 0
  local i = 0
  local doclen = #doc
  while off < doclen do
    doc:seek("set", off)
    local len = doc:piece("len")
    if len <= 0 then break end
    regions[#regions + 1] = { offset = off, length = len, kind = i % 2 }
    off = off + len
    i = i + 1
  end
  doc:seek("set", saved)
  return regions
end

--- Compute colored segments for one line.
--- @param regions table array of {offset, length, kind}
--- @param line_start integer byte offset of line start
--- @param line_end   integer byte offset of line end (exclusive)
--- @return table array of {start=1-based byte, len, kind}
function hl.line_segments(regions, line_start, line_end)
  local segs = {}
  for _, r in ipairs(regions) do
    local r_end = r.offset + r.length
    if r.offset < line_end and r_end > line_start then
      local s = math.max(r.offset, line_start) - line_start + 1
      local e = math.min(r_end, line_end) - line_start
      if e >= s then
        segs[#segs + 1] = { start = s, len = e - s + 1, kind = r.kind }
      end
    end
  end
  return segs
end

-- ================================================================
-- Section 4: Editor engine
-- ================================================================

--- @class editor
--- @field doc piecetab.Doc
local ed = {}

local function dirty(ed)
  return ed.doc:version() ~= ed.saved_vid
end

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
  ed.cmdline = ""      -- command-line buffer for ":" mode
  ed.msg = ""          -- status message (transient)
  ed.saved_vid = ed.doc:version()
  ed.pending_key = nil -- for multi-key sequences (gg, dd)
  ed.scroll_line = 0   -- first visible line (0-based)
  ed.grid = cg.new()   -- cell grid for diff-based rendering
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

-- UTF-8 helpers
local function utf8_char_len(byte)
  if byte < 0x80 then return 1 end
  if byte < 0xc0 then return 0 end
  if byte < 0xe0 then return 2 end
  if byte < 0xf0 then return 3 end
  return 4
end

-- Move cursor by n characters (-1 = left, +1 = right)
local function cursor_move_char(doc, n)
  local off = doc:offset()
  if n < 0 and off <= 0 then return end
  local buf, lnum = doc:buffer(), doc:line()
  local saved = off
  if n < 0 then
    local p = off - 1
    while p > 0 do
      local b = buf:read(p, 1):byte()
      if b >= 0xc0 or b < 0x80 then break end
      p = p - 1
    end
    doc:seek("set", p)
  elseif n > 0 then
    if off >= #buf then return end
    local clen = utf8_char_len(buf:read(off, 1):byte())
    doc:seek("set", off + clen)
  end
  -- restore if seek didn't move (boundary clamp)
  if doc:offset() == saved and n > 0 and off < #buf then
    doc:seek("set", off + 1)
  end
end

local function move_word_forward(doc)
  local saved = doc:offset()
  local lnum = doc:line()
  doc:seek("line", lnum)
  local line = doc:read("l") or ""
  doc:seek("set", saved)
  local col = doc:column()
  edlog("w: saved=%d lnum=%d line=[%s](%d) col=%d",
    saved, lnum, line, #line, col)
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
  edlog("w result: seek cur %d", i - col)
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

-- Rendering helpers

-- 0-based display column within text (before byte offset 'byte').
local function text_byte_to_dcol(text, byte, tabstop)
  if byte <= 0 then return 0 end
  local col = 0
  local i = 1
  local blen = math.min(byte, #text)
  while i <= blen do
    local b = text:byte(i)
    if b == 9 then
      col = col + tabstop - (col % tabstop)
      i = i + 1
    elseif b >= 0xc0 then
      local clen = (b < 0xe0 and 2) or (b < 0xf0 and 3) or 4
      if i + clen - 1 <= blen then
        local ch = text:sub(i, i + clen - 1)
        col = col + (utf8.width(ch) or 1)
      end
      i = i + clen
    else
      col = col + 1
      i = i + 1
    end
  end
  return col
end

-- 0-based byte offset for display column 'dcol' within text.
local function text_dcol_to_byte(text, dcol, tabstop)
  if dcol <= 0 then return 0 end
  local col = 0
  local i = 1
  while i <= #text do
    local b = text:byte(i)
    local nextcol
    if b == 9 then
      nextcol = col + tabstop - (col % tabstop)
    elseif b >= 0xc0 then
      local clen = (b < 0xe0 and 2) or (b < 0xf0 and 3) or 4
      local ch = text:sub(i, i + clen - 1)
      nextcol = col + (utf8.width(ch) or 1)
    else
      nextcol = col + 1
    end
    if nextcol > dcol then return i - 1 end
    col = nextcol
    if b >= 0xc0 then
      i = i + ((b < 0xe0 and 2) or (b < 0xf0 and 3) or 4)
    else
      i = i + 1
    end
  end
  return #text
end

-- Single-pass line render: walk text byte-by-byte, switch style at segment
-- boundaries, batch same-style text into g:putline. Tabs expanded inline.
-- Returns absolute column (0-based) after rendered text.
local function render_line(g, row, col, text, segs, tabstop)
  local byte = 1
  local batch_start = 1
  local cur_style = STYLE_NORMAL
  local seg_idx = 1
  local dc = 0

  local function style_at(b)
    while seg_idx <= #segs do
      local s = segs[seg_idx]
      if b >= s.start and b < s.start + s.len then
        return s.kind == 1 and STYLE_GRAY or STYLE_NORMAL
      end
      if b < s.start then break end
      seg_idx = seg_idx + 1
    end
    return STYLE_NORMAL
  end

  local function flush()
    if batch_start < byte then
      local s = text:sub(batch_start, byte - 1)
      dc = g:putline(row, col + dc, s, cur_style) - col
      batch_start = byte
    end
  end

  while byte <= #text do
    local b = text:byte(byte)
    if b == 9 then
      flush()
      local n = tabstop - (dc % tabstop)
      dc = g:putline(row, col + dc, string.rep(" ", n), cur_style) - col
      byte = byte + 1
      batch_start = byte
    else
      local clen = 1
      if b >= 0xc0 then
        clen = (b < 0xe0 and 2) or (b < 0xf0 and 3) or 4
      end
      local st = style_at(byte)
      if st ~= cur_style then
        flush(); cur_style = st
      end
      byte = byte + clen
    end
  end
  flush()
  return col + dc
end

function ed.render()
  io.write("\27[?25l")
  local rows, cols = term.size()
  local visrows = rows - 1
  local total_lines = ed.doc:breaks()
  local cur_line = ed.doc:line()
  local lnum_width = math.max(3, tostring(total_lines):len())
  local text_width = cols - lnum_width - 2

  -- clamp scroll
  if cur_line < ed.scroll_line then
    ed.scroll_line = cur_line
  elseif cur_line >= ed.scroll_line + visrows then
    ed.scroll_line = cur_line - visrows + 1
  end
  if ed.scroll_line < 0 then ed.scroll_line = 0 end

  edlog("render: size=%dx%d scroll=%d cur=%d,%d total=%d",
    rows, cols, ed.scroll_line, cur_line, ed.doc:column(), total_lines)

  local regions = hl.build_regions(ed.doc)
  local saved_off = ed.doc:offset()
  local g = ed.grid
  g:begin(ed.scroll_line, visrows, cols)

  -- Pass 1: line numbers from breaks()
  local lnum_fmt = "%" .. lnum_width .. "d "
  for row = 1, visrows do
    local r0 = row - 1
    local line_idx = ed.scroll_line + row - 1
    if line_idx < total_lines then
      local s = string.format(lnum_fmt, line_idx + 1)
      g:putline(r0, 0, s, STYLE_DIM)
      g:clearrow(r0, #s, cols)
    else
      g:clearrow(r0, 0, cols)
      g:put(r0, lnum_width + 1, 0x7e, STYLE_DIM)
    end
  end

  -- Pass 2: content and highlights
  ed.doc:seek("line", ed.scroll_line)
  local lines_data = {}
  local cur_off = ed.doc:offset()
  local line_idx = ed.scroll_line - 1

  for line_text in ed.doc:lines() do
    line_idx = line_idx + 1
    if line_idx >= ed.scroll_line + visrows then break end
    local line_start = cur_off
    cur_off = cur_off + #line_text + 1
    local row = line_idx - ed.scroll_line + 1
    lines_data[#lines_data + 1] = {
      row = row, text = line_text, start = line_start
    }
  end

  local col_start = lnum_width + 2
  local col_pad = col_start + text_width

  -- Pass 2: content + highlights (single pass, highlight-driven)
  for _, ld in ipairs(lines_data) do
    local r0 = ld.row - 1
    local segs = hl.line_segments(regions, ld.start, ld.start + #ld.text)
    local endcol = render_line(g, r0, col_start - 1, ld.text, segs, 4)
    if endcol < col_pad - 1 then
      g:clearrow(r0, endcol, col_pad - 1)
    end
  end

  -- flush grid diff
  local csi = g:diff(DIFF_STYLE)
  edlog("  diff: csi_len=%d", #csi)
  io.write(csi)

  -- status bar
  term.move(rows, 1)
  if ed.mode == "COMMAND" then
    io.write(term.REVERSE)
    io.write(":" .. ed.cmdline)
    local pad = cols - utf8.width(":" .. ed.cmdline) - 1
    if pad > 0 then io.write(string.rep(" ", pad)) end
    io.write(term.RESET)
  else
    local dirty_mark = dirty(ed) and "[+] " or ""
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

  -- cursor
  ed.doc:seek("set", saved_off)
  cur_line = ed.doc:line()
  local cur_screen_row = cur_line - ed.scroll_line + 1
  if cur_screen_row < 1 then cur_screen_row = 1 end
  if cur_screen_row > rows - 1 then cur_screen_row = rows - 1 end

  local saved = ed.doc:offset()
  ed.doc:seek("line", cur_line)
  local cur_line_text = ed.doc:read("l") or ""
  ed.doc:seek("set", saved)
  local byte_col = ed.doc:column()
  edlog("cursor: saved_off=%d cur_line=%d line_text=[%s](%d) byte_col=%d",
    saved_off, cur_line, cur_line_text:gsub("\n", "\\n"), #cur_line_text, byte_col)
  local display_col = text_byte_to_dcol(cur_line_text, byte_col, 4)

  local cur_screen_col = display_col + lnum_width + 2
  if cur_screen_col > cols then cur_screen_col = cols end

  term.move(cur_screen_row, cur_screen_col)
  io.write("\27[?25h")
  io.flush()

  g:freeze()
end

-- Normal mode commands

local normal_cmds = {}

-- helper: end-of-text column for line (excludes trailing \n)
local function line_endcol(ed, lnum)
  local llen = ed.doc:linelen(lnum)
  if llen > 0 and lnum < ed.doc:breaks() - 1 then llen = llen - 1 end
  return llen
end

-- byte offset -> display column within current line
local function byte_to_dcol(doc)
  local saved = doc:offset()
  local lnum = doc:line()
  doc:seek("line", lnum)
  local text = doc:read("l") or ""
  doc:seek("set", saved)
  return text_byte_to_dcol(text, doc:column(), 4)
end

-- display column -> byte offset within given line (clamp to char boundary)
local function dcol_to_byte(doc, lnum, dcol)
  local saved = doc:offset()
  doc:seek("line", lnum)
  local text = doc:read("l") or ""
  doc:seek("set", saved)
  return text_dcol_to_byte(text, dcol, 4)
end

function normal_cmds.h(ed) cursor_move_char(ed.doc, -1) end

function normal_cmds.l(ed) cursor_move_char(ed.doc, 1) end

function normal_cmds.j(ed)
  local lnum = ed.doc:line()
  if lnum >= ed.doc:breaks() - 1 then return end
  local dcol = byte_to_dcol(ed.doc)
  ed.doc:seek("line", lnum + 1)
  local bc = dcol_to_byte(ed.doc, lnum + 1, dcol)
  edlog("j: from L%d dcol=%d -> L%d bc=%d", lnum, dcol, lnum + 1, bc)
  ed.doc:seek("cur", bc)
end

function normal_cmds.k(ed)
  local lnum = ed.doc:line()
  if lnum <= 0 then return end
  local dcol = byte_to_dcol(ed.doc)
  ed.doc:seek("line", lnum - 1)
  local bc = dcol_to_byte(ed.doc, lnum - 1, dcol)
  ed.doc:seek("cur", bc)
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
  ed.doc:edit(1, "")
  ed.doc:commit()
end

function normal_cmds.dd(ed)
  local lnum = ed.doc:line()
  local llen = ed.doc:linelen(lnum)
  ed.doc:seek("line", lnum)
  ed.doc:remove(llen)
  ed.doc:commit()
end

function normal_cmds.i(ed) ed.mode = "INSERT" end

function normal_cmds.a(ed)
  cursor_move_char(ed.doc, 1); ed.mode = "INSERT"
end

function normal_cmds.o(ed)
  local lnum = ed.doc:line()
  ed.doc:seek("line", lnum)
  ed.doc:seek("cur", line_endcol(ed, lnum))
  ed.doc:edit(0, "\n")
  ed.mode = "INSERT"
end

function normal_cmds.O(ed)
  ed.doc:seek("line", ed.doc:line())
  ed.doc:edit(0, "\n")
  ed.doc:seek("cur", -1)
  ed.mode = "INSERT"
end

function normal_cmds.u(ed)
  ed.doc:undo()
  edlog("undo: offset=%d", ed.doc:offset())
  ed.msg = ""
end

normal_cmds["<C-r>"] = function(ed)
  ed.doc:redo()
  edlog("redo: offset=%d", ed.doc:offset())
  ed.msg = ""
end

normal_cmds["<C-l>"] = function(ed)
  ed.grid:clear()
  ed.msg = ""
end

normal_cmds[":"] = function(ed)
  ed.mode = "COMMAND"
  ed.cmdline = ""
end

normal_cmds["<Up>"] = function(ed) normal_cmds.k(ed) end
normal_cmds["<Down>"] = function(ed) normal_cmds.j(ed) end
normal_cmds["<Left>"] = function(ed) normal_cmds.h(ed) end
normal_cmds["<Right>"] = function(ed) normal_cmds.l(ed) end

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
  ed.saved_vid = ed.doc:version()
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
  ed.saved_vid = ed.doc:version()
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
  if key == "<Escape>" then
    ed.mode = "NORMAL"
    ed.doc:commit()
    if ed.doc:offset() > 0 then
      cursor_move_char(ed.doc, -1)
    end
    edlog("insert: ESC -> NORMAL, commit off=%d", ed.doc:offset())
    ed.msg = ""
  elseif key == "<Backspace>" then
    local off = ed.doc:offset()
    if off > 0 then
      local buf = ed.doc:buffer()
      local p = off - 1
      while p > 0 do
        local b = buf:read(p, 1):byte()
        if b >= 0xc0 or b < 0x80 then break end
        p = p - 1
      end
      local char_len = off - p
      ed.doc:seek("set", p)
      ed.doc:edit(char_len, "")
    end
  elseif key == "<Delete>" then
    local off = ed.doc:offset()
    if off < #ed.doc:buffer() then
      local buf = ed.doc:buffer()
      local clen = utf8_char_len(buf:read(off, 1):byte())
      ed.doc:edit(clen, "")
    end
  elseif key == "<Enter>" then
    ed.doc:edit(0, "\n")
  elseif key == "<Tab>" then
    ed.doc:edit(0, "\t")
  elseif key == "<C-c>" then
    ed.mode = "NORMAL"
    ed.msg = ""
  elseif key == "<Up>" then
    normal_cmds.k(ed)
  elseif key == "<Down>" then
    normal_cmds.j(ed)
  elseif key == "<Left>" then
    cursor_move_char(ed.doc, -1)
  elseif key == "<Right>" then
    cursor_move_char(ed.doc, 1)
  elseif key == "<Home>" then
    ed.doc:seek("line", ed.doc:line())
  elseif key == "<End>" then
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum)
    ed.doc:seek("cur", line_endcol(ed, lnum))
  elseif key == "<PageUp>" then
    local jump = (term.size() - 2)
    for _ = 1, jump do normal_cmds.k(ed) end
  elseif key == "<PageDown>" then
    local jump = (term.size() - 2)
    for _ = 1, jump do normal_cmds.j(ed) end
  elseif type(key) == "string" and #key > 0 then
    if key:sub(1, 1) == "<" and key:sub(-1) == ">" then return end
    local b = key:byte(1)
    if b >= 32 and b < 127 or b >= 0xc0 then
      ed.doc:edit(0, key)
    end
  end
end

-- Normal mode dispatch

local function normal_key(ed, key)
  if ed.pending_key then
    local combo = ed.pending_key .. key
    ed.pending_key = nil
    local handler = normal_cmds[combo]
    if handler then
      handler(ed)
      ed.msg = ""
      return
    end
  end
  if key == "g" or key == "d" then
    ed.pending_key = key
    return
  end
  ed.pending_key = nil
  local handler = normal_cmds[key]
  if handler then
    handler(ed)
    ed.msg = ""
  elseif key == "<Escape>" or key == "<C-c>" then
    ed.msg = ""
  end
end

-- Command mode

local function command_key(ed, key)
  if key == "<Escape>" or key == "<C-c>" then
    ed.mode = "NORMAL"
    ed.cmdline = ""
  elseif key == "<Enter>" then
    exec_command(ed)
  elseif key == "<Backspace>" then
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
-- Section 4: Main
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
      ed.dispatch(term.getkey())
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
