#!/usr/bin/env lua
-- editor.lua -- piecetab-based terminal text editor (class skeleton)
-- usage: lua editor.lua [file]

package.cpath = package.cpath ..
    ";./lua/?.so;./lua/luajit/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
local pt = require("piecetab")
local cg = require("cellgrid")

local utf8 = require("lua-utf8")
local tf = require("termfeed")
local ts = require("treesitter")

-- ================================================================
-- Section 0: Logging (writes to editor.log for debugging)
-- ================================================================

local logfile = nil
---@param fmt string
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

---@alias editor.Mode "normal"|"insert"|"command"
---@alias editor.Key string
---@alias editor.KeymapFn fun(self: editor.Ed, key: editor.Key)
---@alias editor.CommandFn fun(self: editor.Ed, arg: string, bang: boolean)

--- @class editor.Term
---@field out {write: fun(o: table, s: string), flush: fun(o: table)}
---@field size_fn fun(): integer, integer
---@field tf termfeed.State
---@field esc_timeout integer
---@field s? string  captured output (fake term in tests)
local Term = {}

-- method-style wrapper so Term:write/self.out.write(self.out, s) works
-- for both this and duck-typed outs (fake term in tests)
local IO = {
  write = function(_, s) io.write(s) end,
  flush = function() io.flush() end
}

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
    -- bare ESC: waitkey polls esc_timeout ms for a sequence prefix before
    -- flushing it as a standalone ESC key (vim timeoutlen; -1 blocks forever)
    self.esc_timeout = opts.esc_timeout or 50
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
    if self.tf:waitkey(0, self.esc_timeout) ~= "KEY" then return nil end
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
Term.REVERSE         = "\27[7m"
Term.DIM             = "\27[2m"
Term.RESET           = "\27[0m"

-- grid cell style IDs (see DIFF_STYLE for CSI mapping)
local STYLE_NORMAL   = 0
local STYLE_DIM      = 1
local STYLE_GRAY     = 3
local STYLE_KEYWORD  = 10
local STYLE_STRING   = 11
local STYLE_COMMENT  = 12
local STYLE_FUNCTION = 13

-- diff style table: cell style ID -> CSI
local DIFF_STYLE     = {
  [0]  = "\27[0m",        -- RESET
  [1]  = "\27[2m",        -- DIM
  [3]  = "\27[48;5;237m", -- gray bg
  [10] = "\27[38;5;207m", -- keyword (pink)
  [11] = "\27[38;5;114m", -- string (green)
  [12] = "\27[38;5;245m", -- comment (gray fg)
  [13] = "\27[38;5;81m",  -- function (blue)
}

-- ================================================================
-- Section 2: Text/cursor pure functions
-- Char motion and column math here are C-module incubation
-- candidates (see notes/design_editor.md); keep them marked.
-- ================================================================

---@param byte integer
local function word_class(byte)
  if byte >= 48 and byte <= 57 then return 1 end  -- digit
  if byte >= 65 and byte <= 90 then return 1 end  -- upper
  if byte >= 97 and byte <= 122 then return 1 end -- lower
  if byte == 95 then return 1 end                 -- underscore
  return 0
end

-- Move cursor by n characters (-1 = left, +1 = right)
---@param doc piecetab.Doc
---@param n integer
local function cursor_move_char(doc, n)
  -- TODO(C): promote char motion to C (pt or new module)
  local off = doc:offset()
  if n < 0 and off <= 0 then return end
  local buf = doc:buffer()
  local saved = off
  if n < 0 then
    -- tail window [off-4, off+1): prev char lead + current lead
    local s0 = math.max(off - 4, 0)
    local p = utf8.offset(buf:read(s0, off - s0 + 1), -1, off - s0 + 1)
    doc:seek("set", p - 1 + s0)
  elseif n > 0 then
    if off >= #buf then return end
    -- 5 bytes cover a 4-byte char plus its successor's lead byte
    local nxt = utf8.next(buf:read(off, 5), 1)
    doc:seek("set", nxt and off + nxt - 1 or #buf)
  end
  -- restore if seek didn't move (boundary clamp)
  if doc:offset() == saved and n > 0 and off < #buf then
    doc:seek("set", off + 1)
  end
end

---@param doc piecetab.Doc
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

---@param doc piecetab.Doc
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
---@param text string
---@param byte integer
---@param tabstop integer
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
      local nxt = utf8.next(text, i) or #text + 1
      if nxt - 1 <= blen then
        col = col + (utf8.width(text, i, nxt - 1) or 1)
      end
      i = nxt
    else
      col = col + 1
      i = i + 1
    end
  end
  return col
end

-- 0-based byte offset for display column 'dcol' within text.
---@param text string
---@param dcol integer
---@param tabstop integer
local function text_dcol_to_byte(text, dcol, tabstop)
  if dcol <= 0 then return 0 end
  local col = 0
  local i = 1
  while i <= #text do
    local b = text:byte(i)
    local nxt = i + 1
    local nextcol
    if b == 9 then
      nextcol = col + tabstop - (col % tabstop)
    elseif b >= 0xc0 then
      nxt = utf8.next(text, i) or #text + 1
      nextcol = col + (utf8.width(text, i, nxt - 1) or 1)
    else
      nextcol = col + 1
    end
    if nextcol > dcol then return i - 1 end
    col = nextcol
    i = nxt
  end
  return #text
end

-- helper: end-of-text column for line (excludes trailing \n)
---@param ed editor.Ed
---@param lnum integer
local function line_endcol(ed, lnum)
  local llen = ed.doc:linelen(lnum)
  if llen > 0 and lnum < ed.doc:breaks() - 1 then llen = llen - 1 end
  return llen
end

-- byte offset -> display column within current line
---@param doc piecetab.Doc
local function byte_to_dcol(doc)
  local saved = doc:offset()
  local lnum = doc:line()
  doc:seek("line", lnum)
  local text = doc:read("l") or ""
  doc:seek("set", saved)
  return text_byte_to_dcol(text, doc:column(), 4)
end

-- display column -> byte offset within given line (clamp to char boundary)
---@param doc piecetab.Doc
---@param lnum integer
---@param dcol integer
local function dcol_to_byte(doc, lnum, dcol)
  local saved = doc:offset()
  doc:seek("line", lnum)
  local text = doc:read("l") or ""
  doc:seek("set", saved)
  return text_dcol_to_byte(text, dcol, 4)
end

-- Move cursor vertically by dl lines, preserving display column
---@param doc piecetab.Doc
---@param dl integer
local function move_vert(doc, dl)
  local lnum = doc:line()
  local nlnum = lnum + dl
  if nlnum < 0 or nlnum >= doc:breaks() then return end
  local dcol = byte_to_dcol(doc)
  doc:seek("line", nlnum)
  doc:seek("cur", dcol_to_byte(doc, nlnum, dcol))
end

-- Open a new line: dir > 0 below (o), dir < 0 above (O); enter INSERT
---@param self editor.Ed
---@param dir integer
local function open_line(self, dir)
  self.doc:seek("line", self.doc:line())
  if dir > 0 then
    self.doc:seek("cur", line_endcol(self, self.doc:line()))
  end
  self:docedit(0, "\n")
  if dir < 0 then self.doc:seek("cur", -1) end
  self.mode = "INSERT"
end

-- ================================================================
-- Section 3: Highlight module (tree-sitter syntax highlighting)
-- ================================================================

local hl = {}

-- file extension -> language name (nil = no highlighting)
---@param filename string
---@return string?
local function ext_lang(filename)
  local ext = filename:match("%.([%w_]+)$")
  if ext == "c" or ext == "h" then return "c" end
  if ext == "lua" then return "lua" end
  return nil
end

-- Minimal highlights subsets (keyword/string/comment/function).
-- NB: primitive types (int/char/void) are internal tokens of primitive_type,
-- not matchable as string literals; match the node type instead.
local HL_QUERIES = {
  c = [[
    (comment) @comment
    (string_literal) @string
    (primitive_type) @keyword
    "break" @keyword
    "case" @keyword
    "const" @keyword
    "continue" @keyword
    "default" @keyword
    "do" @keyword
    "else" @keyword
    "enum" @keyword
    "extern" @keyword
    "for" @keyword
    "goto" @keyword
    "if" @keyword
    "inline" @keyword
    "register" @keyword
    "restrict" @keyword
    "return" @keyword
    "sizeof" @keyword
    "static" @keyword
    "struct" @keyword
    "switch" @keyword
    "typedef" @keyword
    "union" @keyword
    "volatile" @keyword
    "while" @keyword
    (function_definition
      declarator: (function_declarator
        declarator: (identifier) @function))
  ]],
  lua = [[
    (comment) @comment
    (string) @string
    "and" @keyword
    (break_statement) @keyword
    "do" @keyword
    "else" @keyword
    "elseif" @keyword
    "end" @keyword
    (false) @keyword
    "for" @keyword
    "function" @keyword
    "goto" @keyword
    "if" @keyword
    "in" @keyword
    "local" @keyword
    (nil) @keyword
    "not" @keyword
    "or" @keyword
    "repeat" @keyword
    "return" @keyword
    "then" @keyword
    (true) @keyword
    "until" @keyword
    "while" @keyword
    (function_call
      name: (identifier) @function)
  ]],
}

local HL_STYLES = {
  comment      = STYLE_COMMENT,
  string       = STYLE_STRING,
  keyword      = STYLE_KEYWORD,
  ["function"] = STYLE_FUNCTION,
}

--- Create a highlighter for a language ("c"/"lua"), nil if unsupported.
function hl.new(ed, lang)
  local qsrc = HL_QUERIES[lang]
  if not qsrc then return nil end
  local langobj = ts.require(lang)
  if not langobj then return nil end
  local parser = ts.parser.new()
  parser.language = langobj
  local self = {
    ed = ed,
    parser = parser,
    query = langobj:query(qsrc),
    tree = nil,
    dirty = true
  }
  return setmetatable(self, { __index = hl })
end

--- Reset tree (whole-reparse on next request). Used after undo/redo/:e.
function hl:reset()
  self.tree = nil
  self.dirty = true
end

--- Notify an edit: translate the tree, defer parse to next request.
function hl:notify_edit(start, old_len, new_len)
  if self.tree then
    local doc = self.ed.doc
    local function pos(off)
      doc:seek("set", off)
      return doc:line() + 1, doc:column() + 1 -- 1-based
    end
    local srow, scol = pos(start)
    local orow, ocol = pos(start + old_len)
    local nrow, ncol = pos(start + new_len)
    self.tree:edit(start + 1, start + old_len + 1, start + new_len + 1,
      srow, scol, orow, ocol, nrow, ncol)
  end
  self.dirty = true
end

--- Parse if dirty. Content via doc:dump() (includes uncommitted edits;
-- doc:buffer() is committed-version only).
function hl:ensure()
  if not self.dirty then return end
  self.tree = self.parser:parse(self.tree, self.ed.doc:dump())
  self.dirty = false
end

--- Query spans for a byte range: {offset, length, style} (0-based offsets).
function hl:query_region(start, endoff)
  self:ensure()
  if not self.tree then return {} end
  local c = self.query:exec(self.tree.root)
  c:set_byte_range(start + 1, endoff)
  local stylemap, spans = HL_STYLES, {}
  while true do
    local ci = c:next_capture()
    if not ci then break end
    local node = c[ci]
    local _, cid = c:captures(ci)
    local style = stylemap[self.query:capture_name_for_id(cid)]
    if style then
      spans[#spans + 1] = {
        offset = node.start_byte - 1,
        length = node.end_byte - node.start_byte,
        style = style
      }
    end
  end
  return spans
end

--- Compute colored segments for one line.
--- @param spans table array of {offset, length, style}
--- @param line_start integer byte offset of line start
--- @param line_end   integer byte offset of line end (exclusive)
--- @return table array of {start=1-based byte, len, style}
function hl.line_segments(spans, line_start, line_end)
  local segs = {}
  for _, sp in ipairs(spans) do
    local r_end = sp.offset + sp.length
    if sp.offset < line_end and r_end > line_start then
      local s = math.max(sp.offset, line_start) - line_start + 1
      local e = math.min(r_end, line_end) - line_start
      if e >= s then
        segs[#segs + 1] = { start = s, len = e - s + 1, style = sp.style }
      end
    end
  end
  return segs
end

-- Single-pass line render: walk text byte-by-byte, switch style at segment
-- boundaries, batch same-style text into g:putline. Tabs expanded inline.
-- Returns absolute column (0-based) after rendered text.
-- TODO(C): tab expand + display-col math to C (cellgrid family);
-- see notes/design_editor.md 六b
---@param g cellgrid.Grid
---@param row integer
---@param col integer
---@param text string
---@param segs table
---@param tabstop integer
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
        return s.style or STYLE_NORMAL
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
      local nxt = utf8.next(text, byte) or #text + 1
      local st = style_at(byte)
      if st ~= cur_style then
        flush(); cur_style = st
      end
      byte = nxt
    end
  end
  flush()
  return col + dc
end

-- ================================================================
-- Section 4: Ed class
-- ================================================================

--- @class editor.Ed
---@field doc piecetab.Doc  document buffer (undo history + linecache)
---@field hl table?  syntax highlighter (tree-sitter), nil = no highlight
---@field filename string?
---@field mode string  "NORMAL"|"INSERT"|"COMMAND"
---@field cmdline string
---@field msg string
---@field saved_vid integer
---@field pending_key string?
---@field scroll_line integer
---@field tabstop integer
---@field log fun(fmt: string, ...: any)
---@field done boolean
---@field term editor.Term
---@field grid cellgrid.Grid
---@field keymaps table<string, table<string, fun(self: editor.Ed, key: string)>>
---@field commands table<string, fun(self: editor.Ed, arg: string, bang: boolean)>
local Ed = {}

-- forward declaration: filled in Section 5 (dispatch reads it via upvalue)
local mode_dispatch = {}

-- Execute pending ":" cmdline: parse name/bang/arg, dispatch registered
-- :command; unregistered name sets msg "Unknown: :" .. line
local function exec_command(self)
  self.mode = "NORMAL"
  local line = self.cmdline
  self.cmdline = ""
  local name, bang, arg = line:match("^(%a+)(!?)(.*)")
  local fn = name and self.commands[name]
  if fn then
    fn(self, arg:match("^%s*(.*)"), bang == "!")
  else
    self.msg = "Unknown: :" .. line
  end
end

-- built-in normal keymaps (per-instance, called from Ed.new)
local function install_normal_keys(self)
  local n = self.keymaps.normal
  n.h = function(ed) cursor_move_char(ed.doc, -1) end
  n.l = function(ed) cursor_move_char(ed.doc, 1) end
  n.j = function(ed) move_vert(ed.doc, 1) end
  n.k = function(ed) move_vert(ed.doc, -1) end
  n.w = function(ed) move_word_forward(ed.doc) end
  n.b = function(ed) move_word_backward(ed.doc) end
  n["0"] = function(ed) ed.doc:seek("line", ed.doc:line()) end
  n["$"] = function(ed)
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum)
    local text = ed.doc:read("l") or ""
    ed.doc:seek("line", lnum) -- read advanced past the line; rewind
    if #text > 0 then
      -- vim: stop on the last char, not after it (multi-byte aware)
      ed.doc:seek("cur", utf8.offset(text, 0, #text) - 1) -- last char start
    end
  end
  n.gg = function(ed) ed.doc:seek("line", 0) end
  n.G = function(ed) ed.doc:seek("line", ed.doc:breaks() - 1) end
  n.x = function(ed)
    ed:docedit(1, ""); ed.doc:commit()
  end
  n.dd = function(ed)
    local lnum = ed.doc:line()
    local llen = ed.doc:linelen(lnum)
    ed.doc:seek("line", lnum)
    ed:docedit(llen, "")
    ed.doc:commit()
  end
  n.i = function(ed) ed.mode = "INSERT" end
  n.a = function(ed)
    cursor_move_char(ed.doc, 1); ed.mode = "INSERT"
  end
  n.o = function(ed) open_line(ed, 1) end
  n.O = function(ed) open_line(ed, -1) end
  n.u = function(ed)
    ed.doc:undo()
    if ed.hl then ed.hl:reset() end
  end
  n["<C-r>"] = function(ed)
    ed.doc:redo()
    if ed.hl then ed.hl:reset() end
  end
  n["<C-l>"] = function(ed) ed.grid:clear() end
  n[":"] = function(ed)
    ed.mode = "COMMAND"; ed.cmdline = ""
  end
  n["<Up>"] = n.k
  n["<Down>"] = n.j
  n["<Left>"] = n.h
  n["<Right>"] = n.l
end

-- built-in insert handlers (registered via install_insert_keys)
local function ins_escape(self)
  self.mode = "NORMAL"
  self.doc:commit()
  local off = self.doc:offset()
  if off > 0 and self.doc:buffer():read(off - 1, 1) ~= "\n" then
    cursor_move_char(self.doc, -1)
  end
  self.msg = ""
end

local function ins_backspace(self)
  local off = self.doc:offset()
  if off > 0 then
    -- tail window [off-4, off+1): prev char lead + current lead
    local buf = self.doc:buffer()
    local s0 = math.max(off - 4, 0)
    local p = utf8.offset(buf:read(s0, off - s0 + 1), -1, off - s0 + 1)
    local prev = p - 1 + s0
    self.doc:seek("set", prev)
    self:docedit(off - prev, "")
  end
end

local function ins_delete(self)
  local off = self.doc:offset()
  local buf = self.doc:buffer()
  if off < #buf then
    -- 5 bytes cover a 4-byte char plus its successor's lead byte
    local nxt = utf8.next(buf:read(off, 5), 1)
    self:docedit(nxt and nxt - 1 or #buf - off, "")
  end
end

-- built-in insert keymaps (per-instance, called from Ed.new)
local function install_insert_keys(self)
  local i = self.keymaps.insert
  i["<Escape>"] = ins_escape
  i["<Backspace>"] = ins_backspace
  i["<Delete>"] = ins_delete
  i["<Enter>"] = function(ed) ed:docedit(0, "\n") end
  i["<Tab>"] = function(ed) ed:docedit(0, "\t") end
  i["<C-c>"] = function(ed)
    ed.mode = "NORMAL"
    ed.msg = ""
  end
  i["<Up>"] = function(ed) move_vert(ed.doc, -1) end
  i["<Down>"] = function(ed) move_vert(ed.doc, 1) end
  i["<Left>"] = function(ed) cursor_move_char(ed.doc, -1) end
  i["<Right>"] = function(ed) cursor_move_char(ed.doc, 1) end
  i["<Home>"] = function(ed) ed.doc:seek("line", ed.doc:line()) end
  i["<End>"] = function(ed)
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum)
    ed.doc:seek("cur", line_endcol(ed, lnum))
  end
  i["<PageUp>"] = function(ed)
    local rows = ed.term:size()
    for _ = 1, rows - 2 do move_vert(ed.doc, -1) end
  end
  i["<PageDown>"] = function(ed)
    local rows = ed.term:size()
    for _ = 1, rows - 2 do move_vert(ed.doc, 1) end
  end
end

-- built-in command keymaps (per-instance, called from Ed.new)
local function install_command_keys(self)
  local c = self.keymaps.command
  c["<Escape>"] = function(ed)
    ed.mode = "NORMAL"; ed.cmdline = ""
  end
  c["<C-c>"] = function(ed)
    ed.mode = "NORMAL"; ed.cmdline = ""
  end
  c["<Enter>"] = exec_command
  c["<Backspace>"] = function(ed) ed.cmdline = ed.cmdline:sub(1, -2) end
end

-- built-in :commands (per-instance, called from Ed.new)
local function install_builtin_commands(self)
  local c = self.commands
  c.w = function(ed, arg, bang)
    if not ed.filename then
      ed.msg = "No filename"; return
    end
    local f = io.open(ed.filename, "w")
    if not f then
      ed.msg = "Cannot write: " .. ed.filename; return
    end
    local data = ed.doc:dump()
    f:write(data); f:close()
    ed.saved_vid = ed.doc:version()
    ed.msg = '"' .. ed.filename .. '" written'
  end
  c.q = function(ed, arg, bang) ed:quit() end
  c.wq = function(ed, arg, bang)
    c.w(ed); c.q(ed)
  end
  c.e = function(ed, arg, bang)
    if not arg or arg == "" then
      ed.msg = "No filename"; return
    end
    local f = io.open(arg, "r")
    local content = ""
    if f then
      content = f:read("*a"); f:close()
    end
    ed.doc = content ~= "" and pt.doc(content) or pt.doc(nil)
    ed.filename = arg
    ed:open_language(ext_lang(arg))
    ed.saved_vid = ed.doc:version()
    ed.scroll_line = 0
    ed.msg = '"' .. arg .. '" loaded, ' .. ed.doc:breaks() .. " lines"
  end
end

do
  Ed.__index = Ed

  --- @return editor.Term
  function Ed.newterm(opts)
    return Term.new(opts)
  end

  --- @param content? string
  --- @param term? table  duck-typed, default Ed.newterm()
  --- @param grid? cellgrid.Grid
  --- @return editor.Ed
  function Ed.new(content, term, grid)
    local self = setmetatable({}, Ed)
    self.doc = content and content ~= "" and pt.doc(content) or pt.doc(nil)
    self.filename = nil
    self.hl = nil
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
    install_command_keys(self)
    install_builtin_commands(self)
    return self
  end

  --- @param filename string
  --- @param term? table
  --- @param grid? cellgrid.Grid
  --- @return editor.Ed
  function Ed.open(filename, term, grid)
    local content = ""
    local f = io.open(filename, "r")
    if f then
      content = f:read("*a"); f:close()
    end
    local self = Ed.new(content, term, grid)
    self.filename = filename
    self:open_language(ext_lang(filename))
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

  --- Enable syntax highlighting for a language ("c"/"lua"/nil to disable).
  function Ed:open_language(lang)
    self.hl = lang and hl.new(self, lang) or nil
  end

  --- Edit at cursor with highlight notification (single edit funnel).
  function Ed:docedit(del, s)
    local off = self.doc:offset()
    self.doc:edit(del, s)
    if self.hl then self.hl:notify_edit(off, del, #s) end
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

  function Ed:render()
    self.term:write("\27[?25l")
    local rows, cols = self.term:size()
    local visrows = rows - 1
    local total_lines = self.doc:breaks()
    local cur_line = self.doc:line()
    local cur_col = self.doc:column()
    local lnum_width = math.max(3, tostring(total_lines):len())
    local text_width = cols - lnum_width - 2

    -- clamp scroll
    if cur_line < self.scroll_line then
      self.scroll_line = cur_line
    elseif cur_line >= self.scroll_line + visrows then
      self.scroll_line = cur_line - visrows + 1
    end
    if self.scroll_line < 0 then self.scroll_line = 0 end

    self.log("render: size=%dx%d scroll=%d cur=%d,%d total=%d",
      rows, cols, self.scroll_line, cur_line, cur_col, total_lines)

    local saved_off = self.doc:offset()
    local spans
    if self.hl then
      self.doc:seek("line", self.scroll_line)
      local s_off = self.doc:offset()
      self.doc:seek("line", math.min(self.scroll_line + visrows, total_lines))
      local e_off = self.doc:offset()
      self.doc:seek("set", saved_off)
      spans = self.hl:query_region(s_off, e_off)
    end
    local g = self.grid
    g:begin(self.scroll_line, visrows, cols)

    -- Pass 1: line numbers from breaks()
    local lnum_fmt = "%" .. lnum_width .. "d "
    for row = 1, visrows do
      local r0 = row - 1
      local line_idx = self.scroll_line + row - 1
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
    self.doc:seek("line", self.scroll_line)
    local lines_data = {}
    local cur_off = self.doc:offset()
    local line_idx = self.scroll_line - 1

    for line_text in self.doc:lines() do
      line_idx = line_idx + 1
      if line_idx >= self.scroll_line + visrows then break end
      local line_start = cur_off
      cur_off = cur_off + #line_text + 1
      local row = line_idx - self.scroll_line + 1
      lines_data[#lines_data + 1] = {
        row = row, text = line_text, start = line_start
      }
    end

    local col_start = lnum_width + 2
    local col_pad = col_start + text_width

    -- content + highlights (single pass, highlight-driven)
    for _, ld in ipairs(lines_data) do
      local r0 = ld.row - 1
      local segs = hl.line_segments(spans or {}, ld.start, ld.start + #ld.text)
      local endcol = render_line(g, r0, col_start - 1, ld.text, segs, self.tabstop)
      if endcol < col_pad - 1 then
        g:clearrow(r0, endcol, col_pad - 1)
      end
    end

    -- flush grid diff
    local csi = g:diff(DIFF_STYLE)
    self.log("  diff: csi_len=%d", #csi)
    self.term:write(csi)

    self:render_status(rows, cols, cur_line, cur_col)
    self:render_cursor(saved_off, lnum_width, rows, cols)
    self.term:flush()
    g:freeze()
  end

  function Ed:render_status(rows, cols, cur_line, cur_col)
    self.term:move(rows, 1)
    if self.mode == "COMMAND" then
      self.term:write(Term.REVERSE)
      self.term:write(":" .. self.cmdline)
      local pad = cols - utf8.width(":" .. self.cmdline) - 1
      if pad > 0 then self.term:write(string.rep(" ", pad)) end
      self.term:write(Term.RESET)
    else
      local dirty_mark = (self.doc:version() ~= self.saved_vid) and "[+] " or ""
      local linestr = string.format("L%d,%d", cur_line + 1, cur_col + 1)
      local left = string.format(" %s%s %s  %s ", dirty_mark,
        self.filename or "[No Name]", self.mode, linestr)
      local msg_part = ""
      if #self.msg > 0 then msg_part = " " .. self.msg end
      local pad = cols - utf8.width(left) - utf8.width(msg_part) - 1
      if pad < 0 then pad = 0 end
      self.term:write(Term.REVERSE)
      self.term:write(left .. string.rep(" ", pad) .. msg_part .. " ")
      self.term:write(Term.RESET)
    end
  end

  function Ed:render_cursor(saved_off, lnum_width, rows, cols)
    self.doc:seek("set", saved_off)
    local cur_line = self.doc:line()
    local cur_screen_row = cur_line - self.scroll_line + 1
    if cur_screen_row < 1 then cur_screen_row = 1 end
    if cur_screen_row > rows - 1 then cur_screen_row = rows - 1 end

    local saved = self.doc:offset()
    self.doc:seek("line", cur_line)
    local cur_line_text = self.doc:read("l") or ""
    self.doc:seek("set", saved)
    local byte_col = self.doc:column()
    self.log("cursor: saved_off=%d cur_line=%d line_text=[%s](%d) byte_col=%d",
      saved_off, cur_line, cur_line_text:gsub("\n", "\\n"), #cur_line_text, byte_col)
    local display_col = text_byte_to_dcol(cur_line_text, byte_col, self.tabstop)

    local cur_screen_col = display_col + lnum_width + 2
    if cur_screen_col > cols then cur_screen_col = cols end

    self.term:move(cur_screen_row, cur_screen_col)
    self.term:write("\27[?25h")
  end
end

-- ================================================================
-- Section 5: mode_dispatch skeleton (filled by Tasks 2/3/4)
-- ================================================================

mode_dispatch.normal = function(ed, key)
  if ed.pending_key then
    local combo = ed.pending_key .. key
    local fn = ed.keymaps.normal[combo]
    ed.pending_key = nil
    if fn then
      fn(ed, combo); ed.msg = ""; return
    end
  end
  local fn = ed.keymaps.normal[key]
  if fn then
    fn(ed, key); ed.msg = ""; return
  end
  if key == "<Escape>" or key == "<C-c>" then
    ed.msg = ""
    return
  end
  for combo in pairs(ed.keymaps.normal) do
    if #combo > 1 and combo:sub(1, 1) == key then
      ed.pending_key = key
      return
    end
  end
end
-- insert dispatch: keymap hit -> fn, else printable char fallback
local function insert_key(self, key)
  local fn = self.keymaps.insert[key]
  if fn then
    fn(self, key); return
  end
  if type(key) == "string" and #key > 0 then
    if key:sub(1, 1) == "<" and key:sub(-1) == ">" then return end
    local b = key:byte(1)
    if b >= 32 and b < 127 or b >= 0xc0 then
      self:docedit(0, key)
    end
  end
end
mode_dispatch.insert = insert_key
-- command dispatch: keymap hit -> fn, else printable chars append cmdline
local function command_key(self, key)
  local fn = self.keymaps.command[key]
  if fn then
    fn(self, key); return
  end
  if type(key) == "string" and #key > 0 then
    if key:sub(1, 1) == "<" and key:sub(-1) == ">" then return end
    local i = 1
    while i <= #key do
      local b = key:byte(i)
      if b >= 32 and b <= 126 then self.cmdline = self.cmdline .. key:sub(i, i) end
      i = i + 1
    end
  end
end
mode_dispatch.command = command_key

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

Ed.STYLE_NORMAL   = STYLE_NORMAL
Ed.STYLE_DIM      = STYLE_DIM
Ed.STYLE_GRAY     = STYLE_GRAY
Ed.STYLE_KEYWORD  = STYLE_KEYWORD
Ed.STYLE_STRING   = STYLE_STRING
Ed.STYLE_COMMENT  = STYLE_COMMENT
Ed.STYLE_FUNCTION = STYLE_FUNCTION

return Ed
