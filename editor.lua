#!/usr/bin/env lua
-- editor.lua -- piecetab-based terminal text editor (class skeleton)
-- usage: lua editor.lua [file]

package.cpath = package.cpath ..
    ";./lua/?.so;./lua/luajit/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
package.path = package.path .. ";./lua/?.lua"
local pt = require("piecetab")
local cg = require("cellgrid")

local tf = require("termfeed")
local sp = require("spantree")
local ok_ts, ts = pcall(require, "treesitter")
if not ok_ts then ts = nil end -- absent: hl off (pcall err msg is a string, not nil)
local lsp = require("lsp")

-- Section 0: Logging (writes to editor.log for debugging)

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
  if logfile then logfile:write(string.format(fmt, ...) .. "\n") end
end

-- Section 1: Terminal I/O (termfeed + output sink; no Term class)

---@alias editor.Mode "normal"|"insert"|"command"|"visual"
---@alias editor.Key string
---@alias editor.KeymapFn fun(self: editor.Ed, key: editor.Key)
---@alias editor.CommandFn fun(self: editor.Ed, arg?: string, bang?: boolean)

-- SGR attribute codes (booleans -> code; csi generation lives here,
-- the spantree tree has zero format knowledge)
local SGR_ATTR      = { bold = 1, dim = 2, italic = 3, underline = 4, reverse = 7 }

-- attribute field tables (interned into grid style handles by sc)
local ATTR_DIM      = { dim = true }
local ATTR_GRAY_BG  = { bg = 237 }
local ATTR_KEYWORD  = { fg = 207 }
local ATTR_STRING   = { fg = 114 }
local ATTR_COMMENT  = { fg = 245 }
local ATTR_FUNCTION = { fg = 81 }
local ATTR_NUMBER   = { fg = 215 }
local ATTR_DIAG     = { underline = true }
local ATTR_REVERSE  = { reverse = true }

-- Section 2: Text/cursor pure functions
-- Char motion and column math here are C-module incubation
-- candidates (see notes/design_editor.md); keep them marked.

---@param doc piecetab.Doc
---@param lnum integer
---@return string
local function line_text(doc, lnum)
  return doc:readat(doc:lineoffset(lnum), doc:linelen(lnum, true))
end

---@param byte integer
local function word_class(byte)
  if byte >= 48 and byte <= 57 then return 1 end  -- digit
  if byte >= 65 and byte <= 90 then return 1 end  -- upper
  if byte >= 97 and byte <= 122 then return 1 end -- lower
  if byte == 95 then return 1 end                 -- underscore
  return 0
end

---@param ed editor.Ed
local function move_word_forward(ed)
  local doc = ed.doc
  local saved = doc:offset()
  local lnum = doc:line()
  local line = line_text(doc, lnum)
  local col = doc:column()
  local len = #line
  local i = col
  if i < len then
    local cls = word_class(line:byte(i + 1))
    while i < len and word_class(line:byte(i + 1)) == cls do i = i + 1 end
    while i < len and word_class(line:byte(i + 1)) == 0 and line:byte(i + 1) == 32 do i = i + 1 end
  end
  doc:seek("cur", i - col)
  if doc:offset() ~= saved then ed.goal = nil end
end

---@param ed editor.Ed
local function move_word_backward(ed)
  local doc = ed.doc
  local saved = doc:offset()
  local lnum = doc:line()
  local line = line_text(doc, lnum)
  local col = doc:column()
  local i = col - 1
  while i > 0 and line:byte(i + 1) == 32 do i = i - 1 end
  if i >= 0 then
    local cls = word_class(line:byte(i + 1))
    while i >= 0 and word_class(line:byte(i + 1)) == cls do i = i - 1 end
  end
  doc:seek("cur", (i + 1) - col)
  if doc:offset() ~= saved then ed.goal = nil end
end

-- Rendering helpers

-- Move cursor vertically by dl lines, preserving the screen column
-- (injected text counts; Neovim curswant survives the EOL clamp).
---@param ed editor.Ed
---@param dl integer
local function move_vert(ed, dl)
  local doc = ed.doc
  local lnum = doc:line()
  local nlnum = lnum + dl
  if nlnum < 0 or nlnum >= doc:breaks() then return end
  local scol = ed.goal or ed:vtext_dcol(lnum, doc:column(),
    ed.mode == "INSERT")
  ed.goal = scol
  local line = line_text(doc, nlnum)
  local bytecol = ed.grid:byte(ed:screen_to_text_dcol(nlnum, scol),
    line, 1, #line) - 1
  doc:seek("line", nlnum, bytecol)
end

---@param self editor.Ed
---@param dir integer
local function open_line(self, dir)
  self.doc:seek("line", self.doc:line())
  if dir > 0 then
    local lnum = self.doc:line()
    self.doc:seek("line", lnum, self.doc:linelen(lnum, true))
  end
  self:docedit(0, "\n")
  if dir < 0 then self.doc:seek("cur", -1) end
  self.mode = "INSERT"
end

-- Section 3: Highlight module (tree-sitter highlighter)

local hl = {}

-- Minimal highlights subsets (keyword/string/comment/function).
-- NB: primitive types (int/char/void) are internal tokens of primitive_type,
-- not matchable as string literals; match the node type instead.
local HL_QUERIES = {
  c = [[
    (comment) @comment
    (string_literal) @string
    (primitive_type) @keyword
    "break" @keyword "case" @keyword "const" @keyword "continue" @keyword
    "default" @keyword "do" @keyword "else" @keyword "enum" @keyword
    "extern" @keyword "for" @keyword "goto" @keyword "if" @keyword
    "inline" @keyword "register" @keyword "restrict" @keyword "return" @keyword
    "sizeof" @keyword "static" @keyword "struct" @keyword "switch" @keyword
    "typedef" @keyword "union" @keyword "volatile" @keyword "while" @keyword
    (function_definition declarator: (function_declarator
      declarator: (identifier) @function))
  ]],
  lua = [[
    (comment) @comment
    (string) @string
    "if" @keyword "end" @keyword "function" @keyword "local" @keyword
    "return" @keyword
    (function_call name: (identifier) @function)
  ]],
}

local HL_ATTRS = {
  comment      = ATTR_COMMENT,
  string       = ATTR_STRING,
  keyword      = ATTR_KEYWORD,
  ["function"] = ATTR_FUNCTION,
}

-- LSP semantic tokenType names -> attrs (unknown ignored; same name
-- -> same attr). diag is the fallback underline attr (via attrmap.diag).
local LSP_ATTRS = {
  comment      = ATTR_COMMENT,
  string       = ATTR_STRING,
  keyword      = ATTR_KEYWORD,
  number       = ATTR_NUMBER,
  ["function"] = ATTR_FUNCTION,
  method       = ATTR_FUNCTION,
  diag         = ATTR_DIAG,
}

--- Create a highlighter for a language ("c"/"lua"), nil if unsupported.
function hl.new(ed, lang)
  if not ts then return nil end
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
    local function pos(off)
      local line, col = self.ed.doc:linecol(off)
      return line + 1, col + 1 -- 1-based
    end
    local srow, scol = pos(start)
    local orow, ocol = pos(start + old_len)
    local nrow, ncol = pos(start + new_len)
    self.tree:edit(start + 1, start + old_len + 1, start + new_len + 1,
      srow, scol, orow, ocol, nrow, ncol)
  end
  self.dirty = true
end

--- Parse if dirty. Content via doc:dump() (live buffer incl. uncommitted).
function hl:ensure()
  if not self.dirty then return end
  self.tree = self.parser:parse(self.tree, self.ed.doc:dump())
  self.dirty = false
end

--- Query spans for a byte range: {offset, length, attr} (0-based offsets).
function hl:query_region(start, endoff)
  self:ensure()
  if not self.tree then return {} end
  local c = self.query:exec(self.tree.root)
  c:set_byte_range(start + 1, endoff)
  local attrmap, spans = HL_ATTRS, {}
  while true do
    local ci = c:next_capture()
    if not ci then break end
    local node = c[ci]
    local _, cid = c:captures(ci)
    local attr = attrmap[self.query:capture_name_for_id(cid)]
    if attr then
      spans[#spans + 1] = {
        offset = node.start_byte - 1,
        length = node.end_byte - node.start_byte,
        attr = attr
      }
    end
  end
  return spans
end

-- Piece-boundary writer (pull mode, no cache): scan all pieces, alternate
-- gray background on even pieces (first piece plain) to visualize layout.
--- @param doc piecetab.Doc
--- @param start integer
--- @param endoff integer (exclusive)
--- @return table
local function piece_spans(doc, start, endoff)
  local spans, odd = {}, false
  for off, len, _ in doc:buffer():pieces() do
    if odd and off + len > start and off < endoff then
      spans[#spans + 1] = {
        offset = math.max(off, start),
        length = math.min(off + len, endoff) - math.max(off, start),
        attr = ATTR_GRAY_BG,
      }
    end
    odd = not odd
  end
  return spans
end

-- Charwise visual selection [s, e): includes both anchor and cursor
-- chars (vim); cursor sits on a char start, so s is a plain min.
--- @param doc piecetab.Doc
--- @param sel integer?  selection anchor (byte offset)
--- @param cur integer  cursor byte offset
--- @return integer, integer
local function sel_range(doc, sel, cur)
  if not sel then return cur, cur end
  local s, e = sel, cur
  if sel > cur then s, e = cur, sel end
  local n = doc:charlen(e)
  return s, e + n
end

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

-- Single-pass line render: walk clusters via g:next, switch style at
-- segment boundaries, batch same-style text into g:putslice (tabs are
-- expanded inside the grid). Returns absolute column (0-based) after
-- the rendered text.
---@param g cellgrid.Grid
---@param row integer
---@param col integer
---@param text string
---@param segs table
---@param hints table?  sorted by dcol: {dcol, text, style} — injected
---  into the render stream (virt_text, never interned)
local function render_line(g, row, col, text, segs, hints)
  local batch_start = 1
  local cur_byte = 1
  local cur_style = 0
  local seg_idx = 1
  local hint_idx = 1
  local dc = 0
  local hint_w = 0 -- injected hint width (overlay: not part of text col)
  local ts = g:tabstop()

  local function style_at(b)
    while seg_idx <= #segs do
      local s = segs[seg_idx]
      if b >= s.start and b < s.start + s.len then
        return s.style or 0
      end
      if b < s.start then break end
      seg_idx = seg_idx + 1
    end
    return 0
  end

  -- flush [batch_start, cur_byte) as same-style putslice spans. Tabs
  -- expand on the TEXT column (cursor math is text based): putslice
  -- would align them to the render column, incl. injected hints.
  local function flush()
    if batch_start < cur_byte then
      local pos = batch_start
      while pos < cur_byte do
        local t = text:find("\t", pos, true)
        if not t or t >= cur_byte then
          dc = g:putslice(row, col + dc, cur_style, text,
            pos, cur_byte - 1) - col
          break
        end
        if t > pos then
          dc = g:putslice(row, col + dc, cur_style, text,
            pos, t - 1) - col
        end
        local ts = g:tabstop()
        local n = ts - (dc - hint_w) % ts
        local sc = col + dc
        g:fill(row, sc, sc + n, 32, cur_style)
        dc = dc + n
        pos = t + 1
      end
      batch_start = cur_byte
    end
  end

  local function flush_hints(dcol)
    while hints and hint_idx <= #hints and hints[hint_idx].dcol <= dcol do
      flush() -- write the text before the hint position first
      local h = hints[hint_idx]
      dc = g:putslice(row, col + dc, h.style, h.text) - col
      hint_w = hint_w + g:cols(h.text)
      hint_idx = hint_idx + 1
    end
  end

  for byte, dcol in g:next(text) do
    cur_byte = byte
    flush_hints(dcol)
    local st = style_at(byte)
    if st ~= cur_style then
      flush(); cur_style = st
    end
  end
  cur_byte = #text + 1
  flush()
  flush_hints(g:cols(text)) -- end-of-line column
  return col + dc
end

-- Section 4: Ed class

--- @class editor.Ed
---@field doc piecetab.Doc  document buffer (undo history + linecache)
---@field hl table?  syntax highlighter, nil = off
---@field filename string?
---@field mode string  "NORMAL"|"INSERT"|"COMMAND"|"VISUAL"
---@field cmdline string
---@field msg string
---@field saved_vid integer
---@field pending_key string?
---@field goal integer?  vertical goal column (Neovim curswant)
---@field scroll_line integer
---@field log fun(fmt: string, ...: any)
---@field done boolean
---@field lsp lsp.Client?  LSP client (nil = off)
---@field term table  {write, flush, size}
---@field tf termfeed.State
---@field esc_timeout integer
---@field grid cellgrid.Grid
---@field keymaps table<string, table<string, editor.KeymapFn>>
---@field commands table<string, editor.CommandFn>
---@field comp spantree.Compositor  style compositor (attr -> handle intern)
---@field tree spantree.Tree  span tree bound to comp (extmark/identity layer)
---@field styles table<string, integer>  pre-interned style handles
---@field show_pieces boolean  piece-boundary visualization layer
---@field sel_start integer?  visual-mode selection anchor
---@field clip string?  unnamed register (yank buffer)
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
  n.h = function(ed)
    if ed.doc:offset() > 0 then ed.goal = nil; ed.doc:advancechars(-1) end
  end
  n.l = function(ed)
    if ed.doc:offset() < #ed.doc then ed.goal = nil; ed.doc:advancechars(1) end
  end
  n.j = function(ed) move_vert(ed, 1) end
  n.k = function(ed) move_vert(ed, -1) end
  n.w = function(ed) move_word_forward(ed) end
  n.b = function(ed) move_word_backward(ed) end
  n["0"] = function(ed) ed.goal = nil; ed.doc:seek("line", ed.doc:line()) end
  n["$"] = function(ed)
    ed.goal = nil
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum, ed.doc:linelen(lnum, true))
    if ed.doc:column() > 0 then ed.doc:advancechars(-1) end
  end
  n.gg = function(ed) ed.goal = nil; ed.doc:seek("line", 0) end
  n.G = function(ed)
    ed.goal = nil
    ed.doc:seek("line", ed.doc:breaks() - 1)
  end
  n.x = function(ed) ed:onedit(1, "") end
  n.dd = function(ed)
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum)
    ed:onedit(ed.doc:linelen(lnum), "")
  end
  n.i = function(ed) ed.mode = "INSERT" end
  n.a = function(ed)
    if ed.doc:offset() < #ed.doc then ed.goal = nil; ed.doc:advancechars(1) end
    ed.mode = "INSERT"
  end
  n.o = function(ed) open_line(ed, 1) end
  n.O = function(ed) open_line(ed, -1) end
  n.u = function(ed) ed:switch_version("undo") end
  n["<C-r>"] = function(ed) ed:switch_version("redo") end
  n.p = function(ed)
    if not ed.clip then return end
    ed:onedit(0, ed.clip)
  end
  n.v = function(ed) ed.sel_start = ed.doc:offset(); ed.mode = "VISUAL" end
  n["<C-l>"] = function(ed) ed.grid:clear() end
  n[":"] = function(ed) ed.mode = "COMMAND"; ed.cmdline = "" end
  n["<Up>"], n["<Down>"], n["<Left>"], n["<Right>"] = n.k, n.j, n.h, n.l
end

-- built-in insert handlers (registered via install_insert_keys)
local function ins_escape(self)
  self.mode = "NORMAL"
  self.doc:commit()
  if self.doc:offset() > 0 and self.doc:column() > 0 then
    self.goal = nil
    self.doc:advancechars(-1)
  end
  self.msg = ""
end

local function ins_backspace(self)
  local off = self.doc:offset()
  if off > 0 then
    self.doc:advancechars(-1)
    self:docedit(off - self.doc:offset(), "")
  end
end

local function ins_delete(self)
  local n = self.doc:charlen()
  if n > 0 then self:docedit(n, "") end
end

-- built-in insert keymaps (per-instance, called from Ed.new)
local function install_insert_keys(self)
  local i, n = self.keymaps.insert, self.keymaps.normal
  i["<Escape>"] = ins_escape
  i["<Backspace>"] = ins_backspace
  i["<Delete>"] = ins_delete
  i["<Enter>"] = function(ed) ed:docedit(0, "\n") end
  i["<Tab>"] = function(ed) ed:docedit(0, "\t") end
  i["<C-c>"] = function(ed)
    ed.mode = "NORMAL"
    ed.msg = ""
  end
  -- arrows share normal handlers (motion only, no mode change)
  i["<Up>"], i["<Down>"], i["<Left>"], i["<Right>"] = n.k, n.j, n.h, n.l
  i["<Home>"] = function(ed)
    ed.goal = nil
    ed.doc:seek("line", ed.doc:line())
  end
  i["<End>"] = function(ed)
    ed.goal = nil
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum, ed.doc:linelen(lnum, true))
  end
  local function page(ed, dl)
    local rows = ed:size()
    for _ = 1, rows - 2 do move_vert(ed, dl) end
  end
  i["<PageUp>"] = function(ed) page(ed, -1) end
  i["<PageDown>"] = function(ed) page(ed, 1) end
end

-- built-in visual keymaps (per-instance, called from Ed.new)
local function install_visual_keys(self)
  local n, v = self.keymaps.normal, self.keymaps.visual
  -- motions reuse normal handlers; cursor moves extend the selection
  for _, k in ipairs({ "h", "l", "j", "k", "w", "b", "0", "$",
    "<Up>", "<Down>", "<Left>", "<Right>" }) do
    v[k] = n[k]
  end
  v.y = function(ed)
    local s, e = sel_range(ed.doc, ed.sel_start, ed.doc:offset())
    ed.doc:seek("set", s)
    ed.clip = ed.doc:buffer():read(s, e - s)
    ed.mode = "NORMAL"; ed.sel_start = nil
  end
  v.d = function(ed)
    local s, e = sel_range(ed.doc, ed.sel_start, ed.doc:offset())
    ed.doc:seek("set", s)
    ed:onedit(e - s, "")
    ed.mode = "NORMAL"; ed.sel_start = nil
  end
  v["<Escape>"] = function(ed)
    ed.mode = "NORMAL"; ed.sel_start = nil
  end
  v["<C-c>"] = v["<Escape>"]
end

-- built-in command keymaps (per-instance, called from Ed.new)
local function install_command_keys(self)
  local c = self.keymaps.command
  local function cancel(ed)
    ed.mode = "NORMAL"; ed.cmdline = ""
  end
  c["<Escape>"] = cancel
  c["<C-c>"] = cancel
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
    f:write(ed.doc:dump()); f:close()
    ed.saved_vid = ed.doc:version()
    ed.msg = '"' .. ed.filename .. '" written'
  end
  c.q = function(ed, arg, bang) ed:quit() end
  c.wq = function(ed, arg, bang)
    c.w(ed); c.q(ed)
  end
  c.pieces = function(ed, arg, bang) ed.show_pieces = not ed.show_pieces end
  c.lsp = function(ed, arg, bang)
    if arg == "on" then
      if ed.lsp then
        ed.msg = "lsp already on"; return
      end
      if not ed:lsp_start() then ed.msg = "lsp: no server for this file" end
    elseif arg == "off" then
      if ed.lsp then
        ed.lsp:stop(); ed.lsp = nil
      end
      ed.msg = "lsp off"
    elseif arg == "status" then
      ed.msg = ed.lsp and ("lsp: " .. ed.lsp:status()) or "lsp: off"
    else
      ed.msg = "usage: :lsp on|off|status"
    end
  end
  c.e = function(ed, arg, bang)
    if not arg or arg == "" then
      ed.msg = "No filename"; return
    end
    ed:load_file(arg)
  end
end

do
  Ed.__index = Ed

  --- @return spantree.Compositor
  function Ed.newcompositor()
    return sp.compositor()
  end

  --- @param content? string
  --- @param term? table  duck-typed {write, flush, size}
  --- @param grid? cellgrid.Grid
  --- @return editor.Ed
  function Ed.new(content, term, grid)
    local self = setmetatable({}, Ed)
    self.doc = pt.doc(content ~= "" and content or nil)
    self.filename = nil
    self.hl = nil
    self.mode = "NORMAL"
    self.cmdline = ""
    self.msg = ""
    self.saved_vid = self.doc:version()
    self.pending_key = nil
    self.goal = nil -- vertical goal column (Neovim curswant), screen cols
    self.scroll_line = 0
    self.log = edlog
    self.done = false
    self.tf = assert(tf.new())
    self.tf:setflag(tf.FLAG_DELBS)
    self.esc_timeout = (term and term.esc_timeout) or 50
    self.term = term or {
      write = function(_, s) io.write(s) end,
      flush = function() io.flush() end,
      size = function()
        local r, c = cg.winsize(1)
        if r and c then return r, c end
        return 24, 80
      end,
    }
    self.grid = grid or cg.new()
    self.comp = sp.compositor()
    self.tree = sp.new(self.comp)
    self.styles = { dim = self.comp:intern(ATTR_DIM) }
    self.comp:namespace("vtext", 0)
    self.comp:namespace("hl", 1, "e")
    self.comp:namespace("sem", 2)
    self.comp:namespace("diag", 3)
    self.comp:namespace("piece", 4)
    self.comp:namespace("visual", 5)
    self.comp:fields("add", { "severity" })
    self.tree:splice(0, 0, #self.doc)
    self.show_pieces = true -- piece-boundary visualization (debug aid)
    self.keymaps = { normal = {}, insert = {}, command = {}, visual = {} }
    self.commands = {}
    install_normal_keys(self)
    install_insert_keys(self)
    install_visual_keys(self)
    install_command_keys(self)
    install_builtin_commands(self)
    return self
  end

  --- @param filename string
  --- @param term? table  {write, flush, size}
  --- @param grid? cellgrid.Grid
  --- @return editor.Ed
  function Ed.open(filename, term, grid)
    local self = Ed.new(nil, term, grid)
    self:load_file(filename)
    return self
  end

  --- Load/reload a file: rebuild doc, highlight, vtexts, scroll.
  --- @param filename string
  function Ed:load_file(filename)
    local f = io.open(filename, "r")
    local content = f and f:read("*a") or ""
    if f then f:close() end
    self.doc = pt.doc(content ~= "" and content or nil)
    self.filename = filename
    self.tree:clear()
    self.tree:splice(0, 0, #self.doc)
    self:open_language(lsp.Client.langid(filename))
    self.saved_vid = self.doc:version()
    self.scroll_line = 0
    self.msg = '"' .. filename .. '" loaded, ' .. self.doc:breaks() .. " lines"
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
    self.hl = lang and hl.new(self, lang)
  end

  --- Start an LSP server process for the current file (server command,
  -- URI and root resolved by lsp.Client:start_file). silent: fail
  -- quietly (automatic start); loud: report via on_status.
  --- @param silent? boolean
  --- @param argv? string[]  override the configured server command
  --- @return boolean  false when no server is configured
  function Ed:lsp_start(silent, argv)
    if self.lsp then return true end
    local ed = self
    self.lsp = lsp.Client.new({
      get_text = function() return ed.doc:dump() end,
      get_line = function(lnum) return line_text(ed.doc, lnum) end,
      offset_pos = function(off) return ed.doc:linecol(off) end,
      on_status = function(state, why)
        if state == "exited" and not silent then
          ed.msg = "lsp: " .. state .. (why and " (" .. why .. ")" or "")
        end
      end,
      attrmap = LSP_ATTRS,
      vtext = {
        set = function(line, list) ed:set_vtext(line, list) end,
        clear = function() ed:clear_vtexts() end,
      },
      sem = {
        set = function(spans) ed:set_sem(spans) end,
        clear = function() ed.tree:clear("sem") end,
      },
      diag = {
        set = function(spans) ed:set_diag(spans) end,
        clear = function() ed.tree:clear("diag") end,
      },
    })
    local ok = self.lsp:start_file(self.filename, argv)
    if not ok then self.lsp = nil end
    return ok
  end

  -- vtext: injected display text (virt text). Data lives in the spantree
  -- "vtext" layer (a service on top of the span tree); consumers (LSP
  -- inlay hints) write per-line lists, edit shifting is the tree's splice.
  --- @param line integer
  --- @param list table<integer, {off: integer, text: string, style?: integer}>?  nil/empty clears
  function Ed:set_vtext(line, list)
    local lo = self.doc:lineoffset(line)
    local ll = self.doc:linelen(line, true)
    self.tree:clear("vtext", lo, ll + (line < self.doc:breaks() and 1 or 0))
    for _, h in ipairs(list or {}) do
      local n = self.doc:charlen(lo + h.off)
      if n > 0 then
        local attr = { vtext = h.text }
        if h.style then attr.vstyle = h.style end
        self.tree:mark("vtext", attr, lo + h.off, n)
      end
    end
  end

  function Ed:clear_vtexts()
    self.tree:clear("vtext")
  end

  -- Full-snapshot layers (LSP semantic tokens / diagnostics): replace
  -- the layer wholesale, the tree shifts it across edits until the LSP
  -- refetches (async gap coverage).
  --- @param spans table  array of {offset, length, attr}
  function Ed:set_sem(spans)
    self.tree:clear("sem")
    for _, s in ipairs(spans) do
      self.tree:mark("sem", s.attr, s.offset, s.length)
    end
  end

  --- @param spans table  array of {offset, length, attr}
  function Ed:set_diag(spans)
    self.tree:clear("diag")
    for _, s in ipairs(spans) do
      self.tree:mark("diag", s.attr, s.offset, s.length)
    end
  end

  -- Display column at (line, bytecol) shifted past injected text; at_start
  -- = insert-gap: the hint-start byte maps onto the hint's first char.
  -- Hints live in the tree's "vtext" layer, bound to the char after them.
  --- @param line integer
  --- @param bytecol integer  byte offset within the line
  --- @param at_start boolean
  --- @return integer
  function Ed:vtext_dcol(line, bytecol, at_start)
    local lo = self.doc:lineoffset(line)
    local text = line_text(self.doc, line)
    local dcol = self.grid:cols(text, 1, bytecol)
    local w = 0
    for off, _, attr in self.tree:span("vtext", lo, #text + 1) do
      local hdcol = self.grid:cols(text, 1, off - lo)
      if hdcol > dcol or at_start and hdcol == dcol then break end
      w = w + self.grid:cols(attr.vtext)
    end
    return dcol + w
  end

  -- Text column for a screen column: subtract every vtext block wholly
  -- before it; a screen col inside a hint maps to the hint's first text
  -- col after it (Neovim coladvance: cursor skips the hint).
  --- @param line integer
  --- @param scol integer  display column
  --- @return integer
  function Ed:screen_to_text_dcol(line, scol)
    local lo = self.doc:lineoffset(line)
    local text = line_text(self.doc, line)
    local w = 0
    for off, _, attr in self.tree:span("vtext", lo, #text + 1) do
      local hs = self.grid:cols(text, 1, off - lo) + w
      if scol < hs then break end
      local hw = self.grid:cols(attr.vtext)
      if scol < hs + hw then return self.grid:cols(text, 1, off - lo) end
      w = w + hw
    end
    return scol - w
  end

  -- Jump doc versions (undo/redo): splice change hunks into the span
  -- tree and feed them to the LSP as sequential edits.
  function Ed:switch_version(name)
    if self.hl then self.hl:reset() end
    local function sync(f)
      self.doc[name](self.doc, function(off, del, text)
        self.tree:splice(off, del, #text)
        if f then f(off, del, text) end
      end)
    end
    if self.lsp then self.lsp:undo_switch(sync) else sync() end
  end

  --- Edit at cursor with highlight notification (single edit funnel):
  -- notify the LSP, edit the doc, sync the span tree (the vtext layer
  -- shifts with the splice), then the highlighter.
  function Ed:docedit(del, s)
    local off = self.doc:offset()
    if self.lsp then self.lsp:on_edit(off, del, s) end
    self.doc:edit(del, s)
    self.tree:splice(off, del, #s)
    if self.hl then self.hl:notify_edit(off, del, #s) end
  end

  -- Edit + commit (normal-mode ops: x/dd/p, visual d)
  function Ed:onedit(del, s)
    self:docedit(del, s)
    self.doc:commit()
  end

  --- Idle work (called on main-loop timeouts): delegate to the LSP
  -- client (hint refresh scheduling lives there).
  function Ed:tick()
    if self.lsp then self.lsp:tick() end
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

  function Ed:write(s)
    self.term.write(self.term, s)
  end

  function Ed:flush()
    self.term.flush(self.term)
  end

  function Ed:size()
    return self.term.size()
  end

  function Ed:move(row, col)
    self:write(string.format("\27[%d;%dH", row, col)) -- CUP
  end

  function Ed:getkey(timeout)
    if self.tf:waitkey(0, timeout or self.esc_timeout) ~= "KEY" then
      return nil
    end
    return self.tf:format()
  end

  function Ed:enter()
    self:write("\27[?1049h\27[?25l") -- enter alt screen + hide cursor
    self:flush()
    self.tf:raw(0)
  end

  function Ed:leave()
    self.tf:cooked()
    self.tf:delete()
    self:write("\27[?25h\27[2J\27[?1049l") -- show cursor, clear, leave alt screen
    self:flush()
  end

  function Ed:dispatch(key)
    if not key then return end
    local fn = mode_dispatch[self.mode:lower()]
    assert(fn, "unknown mode")
    fn(self, key)
  end

  -- SGR escape for a style id: full reset + lexicographically sorted
  -- codes; fg/bg map to 38/48;5;n or 38/48;2;r;g;b; SGR_ATTR booleans
  -- to their codes; nil for operator/unknown ids.
  --- @param id integer
  --- @return string?
  function Ed:csi(id)
    local attr = self.comp:attr(id)
    if not attr then return nil end
    local codes = {}
    for k, v in pairs(attr) do
      if v and (k == "fg" or k == "bg") then
        local pre = k == "fg" and "38" or "48"
        if type(v) == "string" then
          local r, g, b = v:match("^#(%x%x)(%x%x)(%x%x)$")
          if r then
            codes[#codes + 1] = pre .. ";2;" .. tonumber(r, 16)
                .. ";" .. tonumber(g, 16) .. ";" .. tonumber(b, 16)
          end
        elseif type(v) == "number" then
          codes[#codes + 1] = pre .. ";5;" .. tostring(v)
        end
      elseif v and SGR_ATTR[k] then
        codes[#codes + 1] = tostring(SGR_ATTR[k])
      end
    end
    table.sort(codes)
    if #codes == 0 then return "\27[0m" end -- RESET
    return "\27[0m\27[" .. table.concat(codes, ";") .. "m" -- SGR
  end

  function Ed:render()
    self:write("\27[?25l") -- hide cursor
    local rows, cols = self:size()
    local visrows = rows - 1
    local total_lines = self.doc:breaks()
    local cur_line = self.doc:line()
    local cur_col = self.doc:column()
    local lnum_width = math.max(3, tostring(total_lines):len())
    local text_width = cols - lnum_width - 2

    if cur_line < self.scroll_line then
      self.scroll_line = cur_line
    elseif cur_line >= self.scroll_line + visrows then
      self.scroll_line = cur_line - visrows + 1
    end
    if self.scroll_line < 0 then self.scroll_line = 0 end

    self.log("render: size=%dx%d scroll=%d cur=%d,%d total=%d",
      rows, cols, self.scroll_line, cur_line, cur_col, total_lines)

    local cur_off = self.doc:offset()
    local s_off = self.doc:lineoffset(self.scroll_line)
    local e_off = self.doc:lineoffset(
      math.min(self.scroll_line + visrows, total_lines))
    -- tree-sitter spans into the tree's "hl" eph layer (below the
    -- persistent ns layers): every tree edit clears it, the next frame
    -- refills from a fresh query
    if self.hl then
      self.tree:clear("hl", s_off, e_off - s_off)
      for _, s in ipairs(self.hl:query_region(s_off, e_off)) do
        self.tree:mark("hl", s.attr, s.offset, s.length)
      end
    end
    -- quick layers: piece bg, then visual reverse (tree folds them
    -- above hl/sem/diag)
    self.tree:clear("piece")
    self.tree:clear("visual")
    if self.show_pieces then
      for _, s in ipairs(piece_spans(self.doc, s_off, e_off)) do
        self.tree:mark("piece", ATTR_GRAY_BG, s.offset, s.length)
      end
    end
    if self.sel_start then
      local lo, hi = sel_range(self.doc, self.sel_start, cur_off)
      self.tree:mark("visual", ATTR_REVERSE, lo, hi - lo)
    end
    local spans = {}
    for off, len, _, id in self.tree:styled(s_off, e_off) do
      spans[#spans + 1] = { offset = off, length = len, style = id }
    end
    local g = self.grid
    g:begin(self.scroll_line, visrows, cols)

    local lnum_fmt = "%" .. lnum_width .. "d "
    for row = 1, visrows do
      local r0 = row - 1
      local line_idx = self.scroll_line + row - 1
      if line_idx < total_lines then
        local s = string.format(lnum_fmt, line_idx + 1)
        g:putslice(r0, 0, self.styles.dim, s)
        g:clearrow(r0, #s, cols)
      else
        g:clearrow(r0, 0, cols)
        g:put(r0, lnum_width + 1, 0x7e, self.styles.dim)
      end
    end

    -- content + highlights (read-only line iteration; doc cursor stays put)
    local col_start = lnum_width + 2
    local col_pad = col_start + text_width
    local last_line = math.min(self.scroll_line + visrows, total_lines) - 1
    for line_idx = self.scroll_line, last_line do
      local line_start = self.doc:lineoffset(line_idx)
      local line_text = self.doc:readat(line_start,
        self.doc:linelen(line_idx, true))
      local row = line_idx - self.scroll_line
      local segs = hl.line_segments(spans, line_start, line_start + #line_text)
      local hints = {}
      for off, _, attr in self.tree:span("vtext", line_start, #line_text + 1) do
        hints[#hints + 1] = {
          dcol = g:cols(line_text, 1, off - line_start),
          text = attr.vtext,
          style = attr.vstyle or self.styles.dim
        }
      end
      local endcol = render_line(g, row, col_start - 1, line_text, segs, hints)
      if endcol < col_pad - 1 then
        g:clearrow(row, endcol, col_pad - 1)
      end
    end

    -- flush grid diff (diff queries option keys by string first, so
    -- only number style ids reach csi; unknown ids -> nil -> defaults)
    local csi = g:diff(setmetatable({}, {
      __index = function(_, id)
        if type(id) == "number" then return self:csi(id) end
      end,
    }))
    self.log("  diff: csi_len=%d", #csi)
    self:write(csi)

    self:render_status(rows, cols, cur_line, cur_col, cur_off)
    self:render_cursor(lnum_width, rows, cols)
    self:flush()
    g:freeze()

    -- after render: delegate refresh work (semantic tokens) to the client
    if self.lsp then self.lsp:post_render() end
  end

  function Ed:render_status(rows, cols, cur_line, cur_col, cur_off)
    self:move(rows, 1)
    if self.mode == "COMMAND" then
      self:write("\27[7m") -- REVERSE
      self:write(":" .. self.cmdline)
      local pad = cols - self.grid:cols(":" .. self.cmdline) - 1
      if pad > 0 then self:write(string.rep(" ", pad)) end
      self:write("\27[0m") -- RESET
    else
      local dirty_mark = (self.doc:version() ~= self.saved_vid) and "[+] " or ""
      local linestr = string.format("L%d,%d", cur_line + 1, cur_col + 1)
      local left = string.format(" %s%s %s  %s ", dirty_mark,
        self.filename or "[No Name]", self.mode, linestr)
      -- center: transient messages — the diag message under the cursor
      -- wins (gone once the cursor leaves); else the event message
      local at = self.lsp and self.lsp:diag_at(cur_off)
      local msg_part = ""
      if at then
        msg_part = " diag: " .. at.msg
      elseif #self.msg > 0 then
        msg_part = " " .. self.msg
      end
      -- right: persistent server state, short form
      local right = self.lsp and (self.lsp:status() == "running"
        and " lsp:on" or (" lsp:" .. self.lsp:status())) or ""
      local avail = cols - self.grid:cols(left) - self.grid:cols(right) - 1
      local n = self.grid:byte(math.max(0, avail), msg_part, 1, #msg_part)
      msg_part = msg_part:sub(1, n - 1)
      local pad = math.max(0, avail - self.grid:cols(msg_part))
      self:write("\27[7m") -- REVERSE
      self:write(left .. msg_part .. string.rep(" ", pad) .. right .. " ")
      self:write("\27[0m") -- RESET
    end
  end

  function Ed:render_cursor(lnum_width, rows, cols)
    local cur_line = self.doc:line()
    local cur_screen_row = cur_line - self.scroll_line + 1
    if cur_screen_row < 1 then cur_screen_row = 1 end
    if cur_screen_row > rows - 1 then cur_screen_row = rows - 1 end

    local byte_col = self.doc:column()
    -- cursor skips hints on motion; at the byte gap (insert) it may sit
    -- on the hint's first char (append semantics, input lands before it)
    local display_col = self:vtext_dcol(cur_line, byte_col,
      self.mode == "INSERT")

    local cur_screen_col = display_col + lnum_width + 2
    if cur_screen_col > cols then cur_screen_col = cols end

    self:move(cur_screen_row, cur_screen_col)
    self:write("\27[?25h") -- show cursor
  end
end

-- Section 5: mode_dispatch skeleton (filled by Tasks 2/3/4)

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
    ed.msg = ""; return
  end
  for combo in pairs(ed.keymaps.normal) do
    if #combo > 1 and combo:sub(1, 1) == key then
      ed.pending_key = key
      return
    end
  end
end
-- printable single key: not a <...> sequence, ASCII graphic or a UTF-8
-- lead byte (whole multibyte char is printable)
---@param key string
---@return boolean
local function key_printable(key)
  if #key == 0 then return false end
  if key:sub(1, 1) == "<" and key:sub(-1) == ">" then return false end
  local b = key:byte(1)
  return b >= 32 and b < 127 or b >= 0xc0
end

-- insert dispatch: keymap hit -> fn, else printable char fallback
local function insert_key(self, key)
  local fn = self.keymaps.insert[key]
  if fn then
    fn(self, key); return
  end
  if key_printable(key) then self:docedit(0, key) end
end
mode_dispatch.insert = insert_key
-- command dispatch: keymap hit -> fn, else printable chars append cmdline
local function command_key(self, key)
  local fn = self.keymaps.command[key]
  if fn then
    fn(self, key); return
  end
  if key_printable(key) then self.cmdline = self.cmdline .. key end
end
-- visual dispatch: keymap hit -> fn, else ignore (motions live in keymap)
local function visual_key(self, key)
  local fn = self.keymaps.visual[key]
  if fn then
    fn(self, key); self.msg = ""
  end
end
mode_dispatch.visual = visual_key
mode_dispatch.command = command_key

-- Section 6: Main

local function main(argv)
  local e = argv[1] and Ed.open(argv[1]) or Ed.new()
  e:enter()

  -- automatic LSP: silently enable when a server exists for the file
  e:lsp_start(true)

  -- Catch exit signals (raw mode: no signals, but just in case)
  local ok, err = pcall(function()
    while not e.done do
      if e.lsp then e.lsp:poll() end
      e:render()
      local key = e:getkey(100) -- 100ms idle slice for tick()
      if key then e:dispatch(key) end
      e:tick()
    end
  end)

  e:leave()
  if not ok and err then
    io.write("\27[0m") -- RESET
    io.stderr:write("Error: " .. tostring(err) .. "\n")
    os.exit(1)
  end
end

-- run as a script only; tests require this file and drive Ed directly
if arg and arg[0] and arg[0]:match("editor%.lua$") then
  main(arg)
end

-- attr tables for tests (intern via e.comp:intern(Ed.ATTR_*) to get handles)
Ed.ATTR_DIM      = ATTR_DIM
Ed.ATTR_GRAY_BG  = ATTR_GRAY_BG
Ed.ATTR_KEYWORD  = ATTR_KEYWORD
Ed.ATTR_STRING   = ATTR_STRING
Ed.ATTR_COMMENT  = ATTR_COMMENT
Ed.ATTR_FUNCTION = ATTR_FUNCTION
Ed.ATTR_NUMBER   = ATTR_NUMBER
Ed.ATTR_DIAG     = ATTR_DIAG
Ed.ATTR_REVERSE  = ATTR_REVERSE

return Ed
