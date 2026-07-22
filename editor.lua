#!/usr/bin/env lua
-- editor.lua -- 基于 piecetab 的终端文本编辑器（spantree 验证平台）
-- 用法: lua editor.lua [file]

package.cpath = package.cpath ..
    ";./build/lua55/?.so;./build/luajit/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
local pt = require("piecetab")
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
local tk_instance = nil

function term.init()
  io.write("\27[?1049h") -- alt screen
  io.write("\27[?25l")   -- hide cursor
  io.flush()
  tk_instance = termkey.new(0)
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

-- Key name mapping: termkey KEYSYM name -> editor key string
local KEYSYM_MAP = {
  Enter     = "enter",
  Escape    = "escape",
  Backspace = "backspace",
  Tab       = "tab",
  Space     = " ",
  Delete    = "delete",
  Home      = "home",
  End       = "end",
  PageUp    = "pageup",
  PageDown  = "pagedown",
  Up        = "up",
  Down      = "down",
  Left      = "left",
  Right     = "right",
}

--- Read one key (blocking). Returns key string for dispatch.
function term.getkey()
  local tk = tk_instance
  local r = tk:waitkey()  -- blocks until complete key or ESC timeout
  if r ~= "KEY" then return nil end
  local ktype = tk:key()
  if ktype == "UNICODE" then
    local str, cp = tk:data()
    if cp == 13 then return "enter" end
    if cp == 9  then return "tab" end
    if cp == 8 or cp == 127 then return "backspace" end
    if cp == 27 then return "escape" end
    if tk:mod("c") and cp >= 1 and cp <= 26 then
      if cp == 12 then return "ctrl-l" end
      if cp == 18 then return "ctrl-r" end
      if cp == 3  then return "ctrl-c" end
      return "ctrl-" .. string.char(cp + 96)
    end
    return str
  elseif ktype == "KEYSYM" then
    local name = tk:data()
    if KEYSYM_MAP[name] then return KEYSYM_MAP[name] end
    return "keysym:" .. name
  elseif ktype == "FUNCTION" then
    return "F" .. tk:data()
  end
  return nil
end


-- ================================================================
-- Section 2: Cell grid (frame buffer with scroll-aware diff)
-- ================================================================

-- Cell char stored as Unicode codepoint (integer). 0 = continuation (skip).
-- Style IDs: 0=normal, 1=dim, 3=gray bg

local function char_to_cp(ch)
  if #ch == 0 then return 0x20 end
  local b = ch:byte()
  if b < 0x80 then return b end
  if b < 0xe0 then return ((b & 0x1f) << 6) | (ch:byte(2) & 0x3f) end
  if b < 0xf0 then return ((b & 0x0f) << 12) | ((ch:byte(2) & 0x3f) << 6) | (ch:byte(3) & 0x3f) end
  return ((b & 0x07) << 18) | ((ch:byte(2) & 0x3f) << 12) | ((ch:byte(3) & 0x3f) << 6) | (ch:byte(4) & 0x3f)
end

local function cp_to_utf8(cp)
  if cp <= 0 then return "" end
  if cp <= 0x7f then return string.char(cp) end
  if cp <= 0x7ff then
    return string.char(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f))
  end
  if cp <= 0xffff then
    return string.char(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
  end
  return string.char(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
end

local function cp_width(cp)
  if cp <= 0 then return 0 end
  if cp < 0x100 then return 1 end
  local ch = cp_to_utf8(cp)
  return utf8.width(ch) or 1
end

local Grid = {}
Grid.__index = Grid

local function row_fill(cols, cp, style)
  local ct, st = {}, {}
  for c = 1, cols do ct[c] = cp; st[c] = style end
  return ct, st
end

function Grid:new()
  return setmetatable({
    C = {}, S = {}, Bc = {}, Bs = {}, dirty = {},
    B2D = {}, T = {}, TSC = {},
    rows = 0, cols = 0, topline = 0, old_topline = 0,
    all_dirty = true,
  }, Grid)
end

function Grid:begin_frame(topline, visrows, cols)
  local resized = (self.rows ~= visrows or self.cols ~= cols)
  self.rows = visrows
  self.cols = cols
  self.old_topline = self.topline
  self.topline = topline
  local delta = topline - self.old_topline

  if resized then
    local ct, bs = row_fill(cols, 0x20, 0)
    for r = 1, visrows do
      self.C[r] = {table.unpack(ct)}
      self.S[r] = {table.unpack(bs)}
      self.Bc[r] = {table.unpack(ct)}
      self.Bs[r] = {table.unpack(bs)}
      self.B2D[r] = nil
      self.T[r] = nil
      self.TSC[r] = nil
    end
  end

  if self.all_dirty then
    for r = 1, visrows do
      self.dirty[r] = {}
      for c = 1, cols do self.dirty[r][c] = true end
    end
    return
  end

  if math.abs(delta) < 1 or math.abs(delta) >= visrows then
    for r = 1, visrows do self.dirty[r] = {} end
    return
  end

  -- scroll: shift current from back, then sync back = current
  -- this ensures put() compares against correct post-scroll state
  if delta > 0 then
    for r = 1, visrows - delta do
      local ct = self.Bc[r + delta]
      self.C[r] = ct and {table.unpack(ct)} or (row_fill(cols, 0x20, 0))
      self.S[r] = {}
      for c = 1, cols do self.S[r][c] = (self.Bs[r + delta] or {})[c] or 0 end
    end
    for r = visrows - delta + 1, visrows do
      self.C[r], self.S[r] = row_fill(cols, 0x20, 0)
    end
  else
    local n = -delta
    for r = 1, visrows - n do
      local ct = self.Bc[r]
      self.C[r + n] = ct and {table.unpack(ct)} or (row_fill(cols, 0x20, 0))
      self.S[r + n] = {}
      for c = 1, cols do self.S[r + n][c] = (self.Bs[r] or {})[c] or 0 end
    end
    for r = 1, n do
      self.C[r], self.S[r] = row_fill(cols, 0x20, 0)
    end
  end

  -- sync back = current (post-scroll baseline for put comparisons)
  local n = delta > 0 and delta or -delta
  for r = 1, visrows do
    self.Bc[r] = {table.unpack(self.C[r])}
    self.Bs[r] = {table.unpack(self.S[r])}
  end

  -- exposed rows dirty, shifted rows clean
  for r = 1, visrows do self.dirty[r] = {} end
  local first = delta > 0 and (visrows - n + 1) or 1
  local last  = delta > 0 and visrows or n
  for r = first, last do
    for c = 1, cols do self.dirty[r][c] = true end
  end
end

function Grid:clear()
  self.all_dirty = true
  for r = 1, self.rows do
    for c = 1, self.cols do self.dirty[r][c] = true end
    self.B2D[r] = nil
    self.T[r] = nil
    self.TSC[r] = nil
  end
end

function Grid:clear_row(r)
  local ct, bs = row_fill(self.cols, 0x20, 0)
  self.C[r] = ct
  self.S[r] = bs
  for c = 1, self.cols do self.dirty[r][c] = true end
  self.B2D[r] = nil
  self.T[r] = nil
  self.TSC[r] = nil
end

function Grid:put(r, c, ch, style)
  if c < 1 or c > self.cols then return end
  local cp = char_to_cp(ch)
  local new_w = cp_width(cp)
  local old_cp = self.Bc[r][c] or 0x20
  local old_w = cp_width(old_cp)
  self.C[r][c] = cp
  self.S[r][c] = style
  self.dirty[r][c] = (cp ~= old_cp) or (style ~= (self.Bs[r][c] or 0))
  -- continuation columns
  for j = 1, new_w - 1 do
    local cc = c + j
    if cc > self.cols then break end
    self.C[r][cc] = 0
    self.S[r][cc] = style
    self.dirty[r][cc] = true
  end
  -- clear orphan columns (old wide char replaced by narrow)
  if new_w < old_w then
    for j = new_w, old_w - 1 do
      local cc = c + j
      if cc > self.cols then break end
      self.C[r][cc] = 0x20
      self.S[r][cc] = 0
      self.dirty[r][cc] = true
    end
  end
end

function Grid:puts(r, c, text, style)
  local col = c
  local i = 1
  while i <= #text do
    local b = text:byte(i)
    local ch, clen
    if b >= 0xc0 then
      clen = (b < 0xe0 and 2) or (b < 0xf0 and 3) or 4
      ch = text:sub(i, i + clen - 1)
    else
      clen = 1
      ch = text:sub(i, i)
    end
    local cp = char_to_cp(ch)
    local w = cp_width(cp)
    self:put(r, col, ch, style)
    col = col + w
    i = i + clen
  end
end

function Grid:span(r, c, n, style)
  for col = c, c + n - 1 do
    if col > self.cols then break end
    self.S[r][col] = style
    self.dirty[r][col] = (style ~= (self.Bs[r][col] or 0))
  end
end

function Grid:diff()
  local d = { rows = {} }
  if not self.all_dirty then
    local delta = self.topline - self.old_topline
    if math.abs(delta) > 0 and math.abs(delta) < self.rows then
      d.scroll = { top = 1, bot = self.rows, n = delta }
    end
  end
  for r = 1, self.rows do
    for c = 1, self.cols do
      if self.dirty[r][c] then d.rows[r] = true; break end
    end
  end
  return d
end

function Grid:snap()
  for r = 1, self.rows do
    self.Bc[r] = {table.unpack(self.C[r])}
    self.Bs[r] = {table.unpack(self.S[r])}
    self.dirty[r] = {}
  end
  self.all_dirty = false
end

-- High-level put: raw text with tabs, UTF-8/width handled internally.
-- Returns absolute column after text.
function Grid:puts_raw(r, startcol, text, style, tabstop, pad_to)
  self.B2D[r] = {}
  self.T[r] = text
  self.TSC[r] = startcol
  local dc = 0
  local i = 1
  self.B2D[r][1] = 0
  while i <= #text do
    local b = text:byte(i)
    if b == 9 then
      local spaces = tabstop - (dc % tabstop)
      for s = 1, spaces do
        self:put(r, startcol + dc + s - 1, " ", style)
      end
      dc = dc + spaces
      i = i + 1
    elseif b >= 0xc0 then
      local clen = (b < 0xe0 and 2) or (b < 0xf0 and 3) or 4
      local ch = text:sub(i, i + clen - 1)
      local w = cp_width(char_to_cp(ch))
      self:put(r, startcol + dc, ch, style)
      for j = 0, clen - 1 do self.B2D[r][i + j] = dc end
      dc = dc + w
      i = i + clen
    else
      self:put(r, startcol + dc, text:sub(i, i), style)
      dc = dc + 1
      i = i + 1
    end
    self.B2D[r][i] = dc
  end
  if pad_to then
    while startcol + dc < pad_to do
      self:put(r, startcol + dc, " ", 0)
      dc = dc + 1
    end
  end
  return startcol + dc
end

-- Highlight byte range [bytestart, byteend) (1-based, exclusive) with style.
function Grid:highlight(r, bytestart, byteend, style)
  local map = self.B2D[r]
  local startcol = self.TSC[r]
  if not map or not startcol then return end
  local ds = map[bytestart]
  local de = map[byteend]
  if not ds or not de then return end
  for col = startcol + ds, startcol + de - 1 do
    self.S[r][col] = style
    self.dirty[r][col] = true
  end
end

-- 0-based byte offset -> 0-based display column within text (via B2D).
function Grid:byte_to_dcol(r, byte)
  local map = self.B2D[r]
  return map and map[byte + 1]
end

-- 0-based display column within text -> 0-based byte offset (via B2D).
function Grid:dcol_to_byte(r, dcol)
  local map = self.B2D[r]
  local texts = self.T[r]
  if not map or not texts then return nil end
  local bc = #texts
  for i = 1, #texts + 1 do
    local dc = map[i]
    if dc == nil then break end
    if dc > dcol then bc = i - 2; break end
    if dc == dcol then bc = i - 1; break end
  end
  while bc > 0 do
    local b = texts:byte(bc)
    if not b or b < 0x80 or b >= 0xc0 then break end
    bc = bc - 1
  end
  return bc
end

-- Pad row from col to targetcol with char and style.
function Grid:pad_to(r, col, targetcol, ch, style)
  while col < targetcol do
    self:put(r, col, ch, style)
    col = col + 1
  end
end

-- ================================================================
-- Section 3: Highlight module (piece-based span coloring)
-- ================================================================

local hl = {}

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
    regions[#regions + 1] = {offset = off, length = len, kind = i % 2}
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
        segs[#segs + 1] = {start = s, len = e - s + 1, kind = r.kind}
      end
    end
  end
  return segs
end

-- ================================================================
-- Section 4: Editor engine
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
  ed.pending_key = nil  -- for multi-key sequences (gg, dd)
  ed.scroll_line = 0 -- first visible line (0-based)
  ed.grid = Grid:new()  -- cell grid for diff-based rendering
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

local BG_GRAY = "\27[48;5;237m"

local function style_ansi(s)
  if s == 1 then return term.DIM end
  if s == 3 then return BG_GRAY end
  return term.RESET  -- style 0 = normal, must clear any stale style
end

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

local function diff_flush(diff, g, cols)
  if diff.scroll then
    local s = diff.scroll
    io.write(string.format("\27[%d;%dr", s.top, s.bot))
    if s.n > 0 then
      io.write(string.format("\27[%dS", s.n))
    else
      io.write(string.format("\27[%dT", -s.n))
    end
    io.write("\27[r")
  end
  for r = 1, g.rows do
    if diff.rows[r] then
      term.move(r, 1)
      local cps, styles = g.C[r], g.S[r]
      local out_chars, out_st = {}, {}
      for col = 1, cols do
        local cp = cps[col] or 0x20
        if cp ~= 0 then
          out_chars[#out_chars + 1] = cp_to_utf8(cp)
          out_st[#out_st + 1] = styles[col] or 0
        end
      end
      local chars = table.concat(out_chars)
      -- byte offset for each character index (1-based char → 1-based byte)
      local boff = { 1 }
      for ci = 1, #out_chars do
        boff[ci + 1] = boff[ci] + #out_chars[ci]
      end
      local i = 1
      while i <= #out_st do
        local s = out_st[i]
        io.write(style_ansi(s))
        local j = i
        while j <= #out_st and out_st[j] == s do j = j + 1 end
        io.write(chars:sub(boff[i], boff[j] - 1))
        i = j
      end
      io.write(term.RESET)
    end
  end
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
  g:begin_frame(ed.scroll_line, visrows, cols)

  -- Pass 1: line numbers from breaks()
  local lnum_fmt = "%" .. lnum_width .. "d "
  for row = 1, visrows do
    local line_idx = ed.scroll_line + row - 1
    if line_idx < total_lines then
      local s = string.format(lnum_fmt, line_idx + 1)
      g:puts(row, 1, s, 1)
      for c = #s + 1, cols do g:put(row, c, " ", 0) end
    else
      for c = 1, lnum_width + 1 do g:put(row, c, " ", 0) end
      g:put(row, lnum_width + 2, "~", 1)
      for c = lnum_width + 3, cols do g:put(row, c, " ", 0) end
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

  -- Pass 2a: text content
  for _, ld in ipairs(lines_data) do
    g:puts_raw(ld.row, col_start, ld.text, 0, 4, col_pad)
    if ld.row == cur_line - ed.scroll_line + 1 then
      edlog("  pass2 row=%d text_len=%d", ld.row, #ld.text)
    end
  end

  -- Pass 2b: highlights
  for _, ld in ipairs(lines_data) do
    local segs = hl.line_segments(regions, ld.start, ld.start + #ld.text)
    for _, seg in ipairs(segs) do
      if seg.kind == 1 then
        g:highlight(ld.row, seg.start, seg.start + seg.len, 3)
      end
    end
  end

  -- flush grid diff
  -- flush grid diff
  local diff = g:diff()
  local dirty_count, dirty_rows = 0, {}
  for r = 1, visrows do if diff.rows[r] then dirty_count = dirty_count + 1; dirty_rows[#dirty_rows+1] = r end end
  edlog("  diff: scroll=%s dirty=%d rows=%s",
    diff.scroll and string.format("%d,%d,%d", diff.scroll.top, diff.scroll.bot, diff.scroll.n) or "nil",
    dirty_count, table.concat(dirty_rows, ","))
  -- dump grid cells for cursor line
  if diff.rows[cur_line - ed.scroll_line + 1] then
    local gc = g.C[cur_line - ed.scroll_line + 1] or {}
    local gs = g.S[cur_line - ed.scroll_line + 1] or {}
    local cps, ss = {}, {}
    for c = 1, math.min(cols, 40) do cps[#cps+1] = string.format("%04X", gc[c] or 0); ss[#ss+1] = gs[c] or 0 end
    edlog("  grid[%d]: cps=%s", cur_line - ed.scroll_line + 1, table.concat(cps, " "))
    edlog("  grid[%d]: sts=%s", cur_line - ed.scroll_line + 1, table.concat(ss, " "))
  end
  diff_flush(diff, g, cols)

  -- status bar
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
    saved_off, cur_line, cur_line_text:gsub("\n","\\n"), #cur_line_text, byte_col)
  local display_col = text_byte_to_dcol(cur_line_text, byte_col, 4)

  local cur_screen_col = display_col + lnum_width + 2
  if cur_screen_col > cols then cur_screen_col = cols end

  term.move(cur_screen_row, cur_screen_col)
  io.write("\27[?25h")
  io.flush()

  g:snap()
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
end

function normal_cmds.dd(ed)
  local lnum = ed.doc:line()
  local llen = ed.doc:linelen(lnum)
  ed.doc:seek("line", lnum)
  ed.doc:remove(llen)
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

normal_cmds["ctrl-r"] = function(ed)
  ed.doc:redo()
  edlog("redo: offset=%d", ed.doc:offset())
  ed.msg = ""
end

normal_cmds["ctrl-l"] = function(ed)
  ed.grid:clear()
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
    if ed.doc:offset() > 0 then
      cursor_move_char(ed.doc, -1)
    end
    ed.dirty = true
    edlog("insert: ESC -> NORMAL, commit off=%d", ed.doc:offset())
    ed.msg = ""
  elseif key == "backspace" then
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
  elseif key == "delete" then
    local off = ed.doc:offset()
    if off < #ed.doc:buffer() then
      local buf = ed.doc:buffer()
      local clen = utf8_char_len(buf:read(off, 1):byte())
      ed.doc:edit(clen, "")
    end
  elseif key == "enter" then
    ed.doc:edit(0, "\n")
  elseif key == "tab" then
    ed.doc:edit(0, "\t")
  elseif key == "ctrl-c" then
    ed.mode = "NORMAL"
    ed.msg = ""
  elseif key == "up" then
    normal_cmds.k(ed)
  elseif key == "down" then
    normal_cmds.j(ed)
  elseif key == "left" then
    cursor_move_char(ed.doc, -1)
  elseif key == "right" then
    cursor_move_char(ed.doc, 1)
  elseif key == "home" then
    ed.doc:seek("line", ed.doc:line())
  elseif key == "end" then
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum)
    ed.doc:seek("cur", line_endcol(ed, lnum))
  elseif key == "pageup" then
    local jump = (term.size() - 2)
    for _ = 1, jump do normal_cmds.k(ed) end
  elseif key == "pagedown" then
    local jump = (term.size() - 2)
    for _ = 1, jump do normal_cmds.j(ed) end
  elseif type(key) == "string" and #key > 0 then
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
