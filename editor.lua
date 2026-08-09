#!/usr/bin/env lua
-- editor.lua -- piecetab-based terminal text editor (class skeleton)
-- usage: lua editor.lua [file]

package.cpath = package.cpath ..
    ";./lua/?.so;./lua/luajit/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
local pt = require("piecetab")
local cg = require("cellgrid")

local utf8 = require("lua-utf8")
local tf = require("termfeed")

-- ================================================================
-- Section 0: Logging (writes to editor.log for debugging)
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
-- Section 1: Term class (terminal I/O via termfeed, not exported)
-- ================================================================

--- @class editor.Term
local Term = {}

-- method-style wrapper so Term:write/self.out.write(self.out, s) works
-- for both this and duck-typed outs (fake term in tests)
local IO = { write = function(_, s) io.write(s) end,
             flush = function() io.flush() end }

do
  Term.__index = Term

  --- @param opts? table  {out?, size?}
  --- @return editor.Term
  function Term.new(opts)
    opts = opts or {}
    local self = setmetatable({}, Term)
    --- @type termfeed.State
    self.tf = assert(tf.new())
    self.tf:setflag(tf.FLAG_DELBS)
    self.out = opts.out or IO
    self.size_fn = opts.size or function()
      local r, c = cg.winsize(1)
      if r and c then return r, c end
      return 24, 80
    end
    return self
  end

  --- @param s string
  function Term:write(s)
    self.out.write(self.out, s)
  end

  function Term:flush()
    self.out.flush(self.out)
  end

  --- @return integer rows, integer cols
  function Term:size()
    return self.size_fn()
  end

  --- @param row integer
  --- @param col integer
  function Term:move(row, col)
    self:write(string.format("\27[%d;%dH", row, col))
  end

  --- Read one key (blocking). Returns termfeed-formatted key string.
  --- @return string?
  function Term:getkey()
    if self.tf:waitkey(0, -1) ~= "KEY" then return nil end
    return self.tf:format()
  end

  --- Enter alt screen + raw mode (main only; tests use fake term).
  function Term:enter()
    self:write("\27[?1049h\27[?25l")
    self:flush()
    self.tf:raw(0)
  end

  --- Leave raw mode, restore terminal.
  function Term:leave()
    self.tf:cooked()
    self.tf:delete()
    self:write("\27[?25h\27[2J\27[?1049l")
    self:flush()
  end
end

-- style codes
Term.REVERSE = "\27[7m"
Term.DIM     = "\27[2m"
Term.RESET   = "\27[0m"

-- ================================================================
-- Section 2: Text/cursor pure functions
-- Char motion and column math here are C-module incubation
-- candidates (see notes/design_editor.md); keep them marked.
-- ================================================================

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

-- byte offset of the start of the UTF-8 character ending at off (off-1..)
local function utf8_prev_start(buf, off)
  local p = off - 1
  while p > 0 do
    local b = buf:read(p, 1):byte()
    if b >= 0xc0 or b < 0x80 then break end
    p = p - 1
  end
  return p
end

-- Move cursor by n characters (-1 = left, +1 = right)
local function cursor_move_char(doc, n)
  -- TODO(C): promote char motion to C (pt or new module)
  local off = doc:offset()
  if n < 0 and off <= 0 then return end
  local buf = doc:buffer()
  local saved = off
  if n < 0 then
    doc:seek("set", utf8_prev_start(buf, off))
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
  -- TODO(C): promote column math to C (cellgrid family)
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

-- Move cursor vertically by dl lines, preserving display column
local function move_vert(doc, dl)
  local lnum = doc:line()
  local nlnum = lnum + dl
  if nlnum < 0 or nlnum >= doc:breaks() then return end
  local dcol = byte_to_dcol(doc)
  doc:seek("line", nlnum)
  doc:seek("cur", dcol_to_byte(doc, nlnum, dcol))
end

-- Open a new line: dir > 0 below (o), dir < 0 above (O); enter INSERT
local function open_line(self, dir)
  self.doc:seek("line", self.doc:line())
  if dir > 0 then
    self.doc:seek("cur", line_endcol(self, self.doc:line()))
  end
  self.doc:edit(0, "\n")
  if dir < 0 then self.doc:seek("cur", -1) end
  self.mode = "INSERT"
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
-- Section 4: Ed class
-- ================================================================

--- @class editor.Ed
local Ed = {}

-- forward declaration: filled in Section 5 (dispatch reads it via upvalue)
local mode_dispatch = {}

-- built-in normal keymaps (per-instance, called from Ed.new)
local function install_normal_keys(self)
  local n = self.keymaps.normal
  n.h = function(self) cursor_move_char(self.doc, -1) end
  n.l = function(self) cursor_move_char(self.doc, 1) end
  n.j = function(self) move_vert(self.doc, 1) end
  n.k = function(self) move_vert(self.doc, -1) end
  n.w = function(self) move_word_forward(self.doc) end
  n.b = function(self) move_word_backward(self.doc) end
  n["0"] = function(self) self.doc:seek("line", self.doc:line()) end
  n["$"] = function(self)
    local lnum = self.doc:line()
    self.doc:seek("line", lnum)
    self.doc:seek("cur", line_endcol(self, lnum))
  end
  n.gg = function(self) self.doc:seek("line", 0) end
  n.G = function(self) self.doc:seek("line", self.doc:breaks() - 1) end
  n.x = function(self) self.doc:edit(1, ""); self.doc:commit() end
  n.dd = function(self)
    local lnum = self.doc:line()
    local llen = self.doc:linelen(lnum)
    self.doc:seek("line", lnum)
    self.doc:remove(llen)
    self.doc:commit()
  end
  n.i = function(self) self.mode = "INSERT" end
  n.a = function(self) cursor_move_char(self.doc, 1); self.mode = "INSERT" end
  n.o = function(self) open_line(self, 1) end
  n.O = function(self) open_line(self, -1) end
  n.u = function(self) self.doc:undo() end
  n["<C-r>"] = function(self) self.doc:redo() end
  n["<C-l>"] = function(self) self.grid:clear() end
  n[":"] = function(self) self.mode = "COMMAND"; self.cmdline = "" end
  n["<Up>"] = n.k
  n["<Down>"] = n.j
  n["<Left>"] = n.h
  n["<Right>"] = n.l
end

-- built-in insert handlers (registered via install_insert_keys)
local function ins_escape(self)
  self.mode = "NORMAL"
  self.doc:commit()
  if self.doc:offset() > 0 then cursor_move_char(self.doc, -1) end
  self.msg = ""
end

local function ins_backspace(self)
  local off = self.doc:offset()
  if off > 0 then
    local p = utf8_prev_start(self.doc:buffer(), off)
    self.doc:seek("set", p)
    self.doc:edit(off - p, "")
  end
end

local function ins_delete(self)
  local off = self.doc:offset()
  local buf = self.doc:buffer()
  if off < #buf then
    self.doc:edit(utf8_char_len(buf:read(off, 1):byte()), "")
  end
end

-- built-in insert keymaps (per-instance, called from Ed.new)
local function install_insert_keys(self)
  local i = self.keymaps.insert
  i["<Escape>"] = ins_escape
  i["<Backspace>"] = ins_backspace
  i["<Delete>"] = ins_delete
  i["<Enter>"] = function(self) self.doc:edit(0, "\n") end
  i["<Tab>"] = function(self) self.doc:edit(0, "\t") end
  i["<C-c>"] = function(self)
    self.mode = "NORMAL"
    self.msg = ""
  end
  i["<Up>"] = function(self) move_vert(self.doc, -1) end
  i["<Down>"] = function(self) move_vert(self.doc, 1) end
  i["<Left>"] = function(self) cursor_move_char(self.doc, -1) end
  i["<Right>"] = function(self) cursor_move_char(self.doc, 1) end
  i["<Home>"] = function(self) self.doc:seek("line", self.doc:line()) end
  i["<End>"] = function(self)
    local lnum = self.doc:line()
    self.doc:seek("line", lnum)
    self.doc:seek("cur", line_endcol(self, lnum))
  end
  i["<PageUp>"] = function(self)
    local rows = self.term:size()
    for _ = 1, rows - 2 do move_vert(self.doc, -1) end
  end
  i["<PageDown>"] = function(self)
    local rows = self.term:size()
    for _ = 1, rows - 2 do move_vert(self.doc, 1) end
  end
end

do
  Ed.__index = Ed

  --- @return editor.Term
  function Ed.newterm()
    return Term.new()
  end

  --- @param content? string
  --- @param term? table  duck-typed, default Ed.newterm()
  --- @param grid? cellgrid.Grid
  --- @return editor.Ed
  function Ed.new(content, term, grid)
    local self = setmetatable({}, Ed)
    self.doc = content and content ~= "" and pt.doc(content) or pt.doc(nil)
    self.filename = nil
    self.mode = "NORMAL"
    self.cmdline = ""
    self.msg = ""
    self.saved_vid = self.doc:version()
    self.pending_key = nil
    self.scroll_line = 0
    self.tabstop = 4
    self.log = edlog
    self.done = false
    self.term = term or Ed.newterm()
    self.grid = grid or cg.new()
    self.keymaps = { normal = {}, insert = {}, command = {} }
    self.commands = {}
    install_normal_keys(self)
    install_insert_keys(self)
    return self
  end

  --- @param filename string
  --- @param term? table
  --- @param grid? cellgrid.Grid
  --- @return editor.Ed
  function Ed.open(filename, term, grid)
    local content = ""
    local f = io.open(filename, "r")
    if f then content = f:read("*a"); f:close() end
    local self = Ed.new(content, term, grid)
    self.filename = filename
    return self
  end

  --- @param mode editor.Mode
  --- @param key editor.Key
  --- @param fn editor.KeymapFn
  --- @return editor.Ed
  function Ed:keymap(mode, key, fn)
    self.keymaps[mode][key] = fn
    return self
  end

  --- @param name string
  --- @param fn editor.CommandFn
  --- @return editor.Ed
  function Ed:command(name, fn)
    self.commands[name] = fn
    return self
  end

  function Ed:quit()
    self.done = true
  end

  function Ed:dispatch(key)
    if not key then return end
    local fn = mode_dispatch[self.mode:lower()]
    assert(fn, "unknown mode")
    fn(self, key)
  end

  function Ed:render() -- Task 5: full implementation
    self.term:write("\27[?25l")
    self.grid:freeze()
  end
end

-- ================================================================
-- Section 5: mode_dispatch skeleton (filled by Tasks 2/3/4)
-- ================================================================

mode_dispatch.normal = function(self, key)
  if self.pending_key then
    local combo = self.pending_key .. key
    local fn = self.keymaps.normal[combo]
    self.pending_key = nil
    if fn then fn(self, combo); self.msg = ""; return end
  end
  local fn = self.keymaps.normal[key]
  if fn then fn(self, key); self.msg = ""; return end
  if key == "<Escape>" or key == "<C-c>" then
    self.msg = ""
    return
  end
  for combo in pairs(self.keymaps.normal) do
    if #combo > 1 and combo:sub(1, 1) == key then
      self.pending_key = key
      return
    end
  end
end
-- insert dispatch: keymap hit -> fn, else printable char fallback
local function insert_key(self, key)
  local fn = self.keymaps.insert[key]
  if fn then fn(self, key); return end
  if type(key) == "string" and #key > 0 then
    if key:sub(1, 1) == "<" and key:sub(-1) == ">" then return end
    local b = key:byte(1)
    if b >= 32 and b < 127 or b >= 0xc0 then
      self.doc:edit(0, key)
    end
  end
end
mode_dispatch.insert = insert_key
mode_dispatch.command = function() end

-- ================================================================
-- Section 6: Main
-- ================================================================

local function main(argv)
  local e = argv[1] and Ed.open(argv[1]) or Ed.new()
  e.term:enter()

  -- Catch exit signals (raw mode: no signals, but just in case)
  local ok, err = pcall(function()
    while not e.done do
      e:render()
      e:dispatch(e.term:getkey())
    end
  end)

  e.term:leave()
  if not ok and err then
    io.write(Term.RESET)
    io.stderr:write("Error: " .. tostring(err) .. "\n")
    os.exit(1)
  end
end

-- run as a script only; tests require this file and drive Ed directly
if arg and arg[0] and arg[0]:match("editor%.lua$") then
  main(arg)
end

return Ed
