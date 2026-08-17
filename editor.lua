#!/usr/bin/env lua
-- editor.lua -- piecetab-based terminal text editor (class skeleton)
-- usage: lua editor.lua [file]

package.cpath = package.cpath ..
    ";./lua/?.so;./lua/luajit/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
package.path = package.path .. ";./lua/?.lua"
local pt = require("piecetab")
local cg = require("cellgrid")

local utf8 = require("lua-utf8")
local tf = require("termfeed")
local sp = require("spantree")
local ok_ts, ts = pcall(require, "treesitter")
if not ok_ts then ts = nil end -- absent: hl off (pcall err msg is a string, not nil)
local lsp = require("lsp")
local luv = require("luv")

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
  if logfile then logfile:write(string.format(fmt, ...) .. "\n") end
end

-- ================================================================
-- Section 1: Term class (terminal I/O via termfeed, not exported)
-- ================================================================

---@alias editor.Mode "normal"|"insert"|"command"|"visual"
---@alias editor.Key string
---@alias editor.KeymapFn fun(self: editor.Ed, key: editor.Key)
---@alias editor.CommandFn fun(self: editor.Ed, arg?: string, bang?: boolean)

--- @class editor.Term
---@field out {write: fun(o: table, s: string), flush: fun(o: table)}
---@field size_fn fun(): integer, integer
---@field tf termfeed.State
---@field esc_timeout integer
---@field s? string  captured output (fake term in tests)
local Term = {}

-- method-style wrapper so Term:write and duck-typed outs (fake terms)
-- both work via self.out.write(self.out, s)
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
    -- bare ESC: waitkey polls esc_timeout ms for a prefix before flushing
    -- it as a standalone key (vim timeoutlen; -1 blocks forever)
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

  --- Read one key, waiting up to `timeout` ms (nil = timed out, caller
  --- runs idle work). Defaults to the ESC prefix window.
  --- @return string?
  function Term:getkey(timeout)
    if self.tf:waitkey(0, timeout or self.esc_timeout) ~= "KEY" then
      return nil
    end
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

-- SGR attribute codes (booleans -> code; csi generation lives here,
-- the spantree tree has zero format knowledge)
local SGR_ATTR = { bold = 1, dim = 2, italic = 3, underline = 4,
                   reverse = 7 }

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

-- ================================================================
-- Section 2: Text/cursor pure functions
-- Char motion and column math here are C-module incubation
-- candidates (see notes/design_editor.md); keep them marked.
-- ================================================================

-- Line text at lnum (current state incl. uncommitted edits; doc:read
-- moves the cursor, so save/restore — buffer:read is committed-only).
---@param doc piecetab.Doc
---@param lnum integer
---@return string
local function line_text(doc, lnum)
  local saved = doc:offset()
  doc:seek("line", lnum)
  local t = doc:read("l") or ""
  doc:seek("set", saved)
  return t
end

---@param byte integer
local function word_class(byte)
  if byte >= 48 and byte <= 57 then return 1 end  -- digit
  if byte >= 65 and byte <= 90 then return 1 end  -- upper
  if byte >= 97 and byte <= 122 then return 1 end -- lower
  if byte == 95 then return 1 end                 -- underscore
  return 0
end

-- Move cursor by n characters (-1 = left, +1 = right). A successful
-- horizontal motion re-samples the vertical goal column (Neovim curswant).
---@param ed editor.Ed
---@param n integer
local function cursor_move_char(ed, n)
  -- TODO(C): promote char motion to C (pt or new module)
  local doc = ed.doc
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
  if doc:offset() ~= saved then ed.goal = nil end
end

---@param ed editor.Ed
local function move_word_forward(ed)
  local doc = ed.doc
  local saved = doc:offset()
  local lnum = doc:line()
  local line = line_text(doc, lnum)
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
  -- skip whitespace
  while i > 0 and line:byte(i + 1) == 32 do i = i - 1 end
  if i >= 0 then
    local cls = word_class(line:byte(i + 1))
    while i >= 0 and word_class(line:byte(i + 1)) == cls do i = i - 1 end
  end
  doc:seek("cur", (i + 1) - col)
  if doc:offset() ~= saved then ed.goal = nil end
end

-- Rendering helpers

-- helper: end-of-text column for line (excludes trailing \n)
---@param ed editor.Ed
---@param lnum integer
local function line_endcol(ed, lnum)
  local llen = ed.doc:linelen(lnum)
  if llen > 0 and lnum < ed.doc:breaks() - 1 then llen = llen - 1 end
  return llen
end

-- display column -> byte offset within given line (clamp to char boundary)
---@param doc piecetab.Doc
---@param lnum integer
---@param dcol integer
---@param grid cellgrid.Grid
local function dcol_to_byte(doc, lnum, dcol, grid)
  return grid:byte(line_text(doc, lnum), dcol)
end

-- Truncate text to fit a display width budget (UTF-8 aware, whole chars).
---@param text string
---@param maxw integer
---@return string
local function text_trunc(text, maxw)
  if utf8.width(text) <= maxw then return text end
  local w, i = 0, 1
  while i <= #text do
    local nxt = utf8.next(text, i) or #text + 1
    local cw = utf8.width(text, i, nxt - 1) or 1
    if w + cw > maxw then break end
    w, i = w + cw, nxt
  end
  return text:sub(1, i - 1)
end

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
  doc:seek("line", nlnum)
  doc:seek("cur", dcol_to_byte(doc, nlnum,
    ed:screen_to_text_dcol(nlnum, scol), ed.grid))
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
-- Section 3: Highlight module (tree-sitter highlighter)
-- ================================================================

local hl = {}

-- file extension -> language name (nil = no highlighting)
---@param filename string?
---@return string?
local function ext_lang(filename)
  if not filename then return nil end
  local ext = filename:match("%.([%w_]+)$")
  if ext == "c" or ext == "h" then return "c" end
  if ext == "lua" then return "lua" end
  return nil
end

-- LSP server command for a file (nil = no server available). PT_LSP_CMD
-- overrides the built-in mapping (space-split argv).
---@param filename string?
---@return string[]?
local function lsp_cmd(filename)
  local over = os.getenv("PT_LSP_CMD")
  if over and #over > 0 then
    local argv = {}
    for w in over:gmatch("%S+") do argv[#argv + 1] = w end
    return argv
  end
  if ext_lang(filename) == "c" then return { "clangd" } end
  if ext_lang(filename) == "lua" then return { "lua-language-server" } end
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
    "if" @keyword
    "end" @keyword
    "function" @keyword
    "local" @keyword
    "return" @keyword
    (function_call
      name: (identifier) @function)
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
    local doc = self.ed.doc
    local function pos(off)
      local line, col = doc:linecol(off)
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

--- Parse if dirty. Content via doc:dump() (uncommitted edits incl.;
-- doc:buffer() is committed-version only).
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
  doc:seek("set", 0)
  local len = doc:piece("len")
  while len > 0 do
    local off = doc:offset()
    if odd and off + len > start and off < endoff then
      spans[#spans + 1] = {
        offset = math.max(off, start),
        length = math.min(off + len, endoff) - math.max(off, start),
        attr = ATTR_GRAY_BG,
      }
    end
    odd = not odd
    len = doc:piece("next")
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
  local tail = doc:buffer():read(e, 4)
  local nxt = #tail > 0 and utf8.next(tail, 1)
  return s, nxt and e + nxt - 1 or e
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

-- Overlay a quick layer (piece bg, visual reverse) onto tree segments:
-- split at the overlay boundary, patch the covered part's attr (copy +
-- merge + re-intern); adjacent same-style segments merge back.
--- @param comp spantree.Tree
--- @param segs table  array of {offset, length, style}
--- @param lo integer  overlay start (byte offset)
--- @param hi integer  overlay end (exclusive)
--- @param patch_fn fun(attr: table)
--- @return table
local function overlay_spans(comp, segs, lo, hi, patch_fn)
  local out = {}
  local function push(seg)
    local last = out[#out]
    if last and last.style == seg.style
        and last.offset + last.length == seg.offset then
      last.length = last.length + seg.length
    else
      out[#out + 1] = seg
    end
  end
  for _, s in ipairs(segs) do
    local off, e = s.offset, s.offset + s.length
    local a = math.max(off, lo)
    local b = math.min(e, hi)
    if b <= a then
      push(s)
    else
      if a > off then push({ offset = off, length = a - off, style = s.style }) end
      local attr = {}
      for k, v in pairs(comp:attr(s.style) or {}) do attr[k] = v end
      patch_fn(attr)
      push({ offset = a, length = b - a, style = comp:intern(attr) })
      if b < e then push({ offset = b, length = e - b, style = s.style }) end
    end
  end
  return out
end

-- ================================================================
-- Section 4: Ed class
-- ================================================================

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
---@field term editor.Term
---@field grid cellgrid.Grid
---@field keymaps table<string, table<string, editor.KeymapFn>>
---@field commands table<string, editor.CommandFn>
---@field comp spantree.Tree  span tree (attr -> handle intern)
---@field styles table<string, integer>  pre-interned style handles
---@field show_pieces boolean  piece-boundary visualization layer
---@field sel_start integer?  visual-mode selection anchor
---@field clip string?  unnamed register (yank buffer)
---@field vtexts table<integer, table<integer, {dcol: integer, text: string, style?: integer}>?>  injected display text per line
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
  n.h = function(ed) cursor_move_char(ed, -1) end
  n.l = function(ed) cursor_move_char(ed, 1) end
  n.j = function(ed) move_vert(ed, 1) end
  n.k = function(ed) move_vert(ed, -1) end
  n.w = function(ed) move_word_forward(ed) end
  n.b = function(ed) move_word_backward(ed) end
  n["0"] = function(ed) ed.goal = nil; ed.doc:seek("line", ed.doc:line()) end
  n["$"] = function(ed)
    ed.goal = nil
    local text = line_text(ed.doc, ed.doc:line())
    if #text > 0 then
      -- vim: stop on the last char, not after it (multi-byte aware)
      ed.doc:seek("cur", utf8.offset(text, 0, #text) - 1) -- last char start
    end
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
    cursor_move_char(ed, 1); ed.mode = "INSERT"
  end
  n.o = function(ed) open_line(ed, 1) end
  n.O = function(ed) open_line(ed, -1) end
  -- jump doc versions (undo/redo), feed the change hunks to the LSP
  -- as sequential edits (incremental sync)
  local function switch_sync(ed, name)
    local changes = ed.lsp and {} or nil
    local function apply(off, del, text)
      if changes then
        changes[#changes + 1] = { off = off, del = del, text = text }
      end
    end
    ed.doc[name](ed.doc, apply)
    if ed.hl then ed.hl:reset() end
    if ed.lsp then ed.lsp:on_switch(changes) end
  end
  n.u = function(ed) switch_sync(ed, "undo") end
  n["<C-r>"] = function(ed) switch_sync(ed, "redo") end
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
  local off = self.doc:offset()
  if off > 0 and self.doc:buffer():read(off - 1, 1) ~= "\n" then
    cursor_move_char(self, -1)
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
  i["<Home>"] = function(ed) ed.goal = nil
    ed.doc:seek("line", ed.doc:line()) end
  i["<End>"] = function(ed)
    ed.goal = nil
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum)
    ed.doc:seek("cur", line_endcol(ed, lnum))
  end
  local function page(ed, dl)
    local rows = ed.term:size()
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
  v["<Escape>"] = function(ed) ed.mode = "NORMAL"; ed.sel_start = nil end
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
    if not ed.filename then ed.msg = "No filename"; return end
    local f = io.open(ed.filename, "w")
    if not f then ed.msg = "Cannot write: " .. ed.filename; return end
    f:write(ed.doc:dump()); f:close()
    ed.saved_vid = ed.doc:version()
    ed.msg = '"' .. ed.filename .. '" written'
  end
  c.q = function(ed, arg, bang) ed:quit() end
  c.wq = function(ed, arg, bang) c.w(ed); c.q(ed) end
  c.pieces = function(ed, arg, bang) ed.show_pieces = not ed.show_pieces end
  c.lsp = function(ed, arg, bang)
    if arg == "on" then
      if ed.lsp then ed.msg = "lsp already on"; return end
      local cmd = lsp_cmd(ed.filename)
      if not cmd then ed.msg = "lsp: no server for this file"; return end
      ed:lsp_start(cmd)
    elseif arg == "off" then
      if ed.lsp then ed.lsp:stop(); ed.lsp = nil end
      ed.msg = "lsp off"
    elseif arg == "status" then
      ed.msg = ed.lsp and ("lsp: " .. ed.lsp:status()) or "lsp: off"
    else
      ed.msg = "usage: :lsp on|off|status"
    end
  end
  c.e = function(ed, arg, bang)
    if not arg or arg == "" then ed.msg = "No filename"; return end
    ed:load_file(arg)
  end
end

do
  Ed.__index = Ed

  --- @return editor.Term
  function Ed.newterm(opts)
    return Term.new(opts)
  end

  --- @return spantree.Tree
  function Ed.newcompositor()
    return sp.new()
  end

  --- @param content? string
  --- @param term? editor.Term  duck-typed, default Ed.newterm()
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
    self.goal = nil -- vertical goal column (Neovim curswant), screen cols
    self.scroll_line = 0
    self.log = edlog
    self.done = false
    self.term = term or Ed.newterm()
    self.grid = grid or cg.new()
    self.comp = sp.new()
    self.styles = { dim = self.comp:intern(ATTR_DIM) }
    self.vtexts = {}
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
  --- @param term? editor.Term
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
    self.doc = content ~= "" and pt.doc(content) or pt.doc(nil)
    self.filename = filename
    self:open_language(ext_lang(filename))
    self:clear_vtexts()
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
    self.hl = lang and hl.new(self, lang) or nil
  end

  --- Start an LSP server process (document access wired to the live
  -- doc; edits funnel via docedit -> on_edit). silent: fail quietly
  -- (automatic start); loud: report via on_status.
  --- @param argv string[]
  --- @param silent? boolean
  --- @return boolean  false when the server could not be started
  function Ed:lsp_start(argv, silent)
    if self.lsp then return true end
    local ed = self
    -- LSP needs absolute file URIs: relative paths (lua editor.lua foo)
    -- break workspace indexing, so require() types never resolve
    local fname = self.filename or ""
    if #fname > 0 and fname:sub(1, 1) ~= "/" then
      fname = luv.cwd() .. "/" .. fname
    end
    self.lsp = lsp.Client.new({
      get_text = function() return ed.doc:dump() end,
      get_line = function(lnum) return line_text(ed.doc, lnum) end,
      offset_pos = function(off)
        return ed.doc:linecol(off)
      end,
      on_status = function(state, why)
        -- steady states render in the status bar segments; only report
        -- abnormal exits as a transient message (silent start: no)
        if state == "exited" and not silent then
          ed.msg = "lsp: " .. state .. (why and " (" .. why .. ")" or "")
        end
      end,
      dcol_fn = function(line, bytecol)
        return ed.grid:cols(line_text(ed.doc, line), bytecol)
      end,
      viewport_fn = function()
        local rows = ed.term:size()
        return { top = ed.scroll_line, rows = rows - 1 }
      end,
      attrmap = LSP_ATTRS,
      vtext = {
        set = function(line, list) ed:set_vtext(line, list) end,
        clear = function() ed:clear_vtexts() end,
      },
    })
    local ok = self.lsp:start(argv, "file://" .. fname,
      ext_lang(fname) or "plaintext", "file://" .. (fname:match("^(.*)/") or "."))
    if not ok then self.lsp = nil end
    return ok
  end

  -- vtext: injected display text (virt text). Data lives on Ed: rendering,
  -- cursor columns and edit shifting are core responsibilities; consumers
  -- (LSP inlay hints today) write via set_vtext/clear_vtexts.
  --- @param line integer
  --- @param list table<integer, {dcol: integer, text: string, style?: integer}>?  nil/empty clears
  function Ed:set_vtext(line, list)
    if list and #list > 0 then self.vtexts[line] = list
    else self.vtexts[line] = nil end
  end

  function Ed:clear_vtexts()
    self.vtexts = {}
  end

  -- Display column at (line, bytecol) shifted past injected text; at_start
  -- = insert-gap: the hint-start byte maps onto the hint's first char.
  --- @param line integer
  --- @param bytecol integer  byte offset within the line
  --- @param at_start boolean
  --- @return integer
  function Ed:vtext_dcol(line, bytecol, at_start)
    local lst = self.vtexts[line]
    local dcol = self.grid:cols(line_text(self.doc, line), bytecol)
    local w = 0
    for _, h in ipairs(lst or {}) do
      if h.dcol > dcol or at_start and h.dcol == dcol then break end
      w = w + utf8.width(h.text)
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
    local lst = self.vtexts[line]
    local w = 0
    for _, h in ipairs(lst or {}) do
      local hs = h.dcol + w
      if scol < hs then break end
      local hw = utf8.width(h.text)
      if scol < hs + hw then return h.dcol end
      w = w + hw
    end
    return scol - w
  end

  -- Shift vtext entries after an edit: same-line hints past the edit
  -- point shift by the byte delta; hints in the deleted range drop;
  -- multi-line edits clear the slot.
  --- @param off integer
  --- @param del integer
  --- @param s string
  function Ed:shift_vtexts(off, del, s)
    local line = self.doc:linecol(off)
    local eline = self.doc:linecol(off + del)
    if line ~= eline or s:find("\n", 1, true) then
      self.vtexts = {}
      return
    end
    local lst = self.vtexts[line]
    if not lst then return end
    local base = self.doc:lineoffset(line)
    local edcol = self.grid:cols(line_text(self.doc, line), off - base)
    local delta = #s - del
    local out = {}
    for _, h in ipairs(lst) do
      if h.dcol < edcol then
        out[#out + 1] = h
      elseif h.dcol >= edcol + del then
        h.dcol = h.dcol + delta
        out[#out + 1] = h
      end
    end
    self.vtexts[line] = #out > 0 and out or nil
  end

  --- Edit at cursor with highlight notification (single edit funnel):
  -- shift vtexts, notify the LSP, then the doc.
  function Ed:docedit(del, s)
    local off = self.doc:offset()
    self:shift_vtexts(off, del, s)
    if self.lsp then self.lsp:on_edit(off, del, s) end
    self.doc:edit(del, s)
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
      if v then
        if k == "fg" or k == "bg" then
          local pre = k == "fg" and "38" or "48"
          if type(v) == "table" then
            codes[#codes + 1] = pre .. ";2;" .. v.r .. ";" .. v.g .. ";" .. v.b
          else
            codes[#codes + 1] = pre .. ";5;" .. tostring(v)
          end
        elseif SGR_ATTR[k] then
          codes[#codes + 1] = tostring(SGR_ATTR[k])
        end
      end
    end
    table.sort(codes)
    if #codes == 0 then return "\27[0m" end
    return "\27[0m\27[" .. table.concat(codes, ";") .. "m"
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
    local s_off = self.doc:lineoffset(self.scroll_line)
    local e_off = self.doc:lineoffset(
      math.min(self.scroll_line + visrows, total_lines))
    -- assemble spans bottom-up: plain base, then hl < lsp-sem < lsp-diag
    -- (overlay patch covers lower-layer keys), then quick layers
    local spans = { { offset = s_off, length = e_off - s_off, style = 0 } }
    local function overlay(attr)
      return function(a)
        for k, v in pairs(attr) do a[k] = v end
      end
    end
    if self.hl then
      for _, s in ipairs(self.hl:query_region(s_off, e_off)) do
        spans = overlay_spans(self.comp, spans, s.offset, s.offset + s.length,
          overlay(s.attr))
      end
    end
    if self.lsp then
      local q = self.lsp:query_spans(s_off, e_off)
      for _, s in ipairs(q.sem) do
        spans = overlay_spans(self.comp, spans, s.offset, s.offset + s.length,
          overlay(s.attr))
      end
      for _, s in ipairs(q.diag) do
        spans = overlay_spans(self.comp, spans, s.offset, s.offset + s.length,
          overlay(s.attr))
      end
    end
    -- quick layers: piece bg, then visual reverse
    if self.show_pieces then
      for _, s in ipairs(piece_spans(self.doc, s_off, e_off)) do
        spans = overlay_spans(self.comp, spans, s.offset, s.offset + s.length,
          function(a) a.bg = ATTR_GRAY_BG.bg end)
      end
    end
    if self.sel_start then
      local lo, hi = sel_range(self.doc, self.sel_start, saved_off)
      spans = overlay_spans(self.comp, spans, lo, hi,
        function(a) a.reverse = true end)
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
        g:putslice(r0, 0, self.styles.dim, s)
        g:clearrow(r0, #s, cols)
      else
        g:clearrow(r0, 0, cols)
        g:put(r0, lnum_width + 1, 0x7e, self.styles.dim)
      end
    end

    -- content + highlights (single pass over lines(), highlight-driven)
    local col_start = lnum_width + 2
    local col_pad = col_start + text_width
    self.doc:seek("line", self.scroll_line)
    local cur_off = self.doc:offset()
    local line_idx = self.scroll_line - 1

    for line_text in self.doc:lines() do
      line_idx = line_idx + 1
      if line_idx >= self.scroll_line + visrows then break end
      local row = line_idx - self.scroll_line
      local segs = hl.line_segments(spans or {}, cur_off, cur_off + #line_text)
      local hints = self.vtexts[line_idx]
      if hints then
        for _, h in ipairs(hints) do h.style = self.styles.dim end
      end
      local endcol = render_line(g, row, col_start - 1, line_text, segs, hints)
      if endcol < col_pad - 1 then
        g:clearrow(row, endcol, col_pad - 1)
      end
      cur_off = cur_off + #line_text + 1
    end

    -- flush grid diff (diff queries option keys by string first, so
    -- only number style ids reach csi; unknown ids -> nil -> defaults)
    local csi = g:diff(setmetatable({}, {
      __index = function(_, id)
        if type(id) == "number" then return self:csi(id) end
      end,
    }))
    self.log("  diff: csi_len=%d", #csi)
    self.term:write(csi)

    self:render_status(rows, cols, cur_line, cur_col, saved_off)
    self:render_cursor(saved_off, lnum_width, rows, cols)
    self.term:flush()
    g:freeze()

    -- after render: delegate refresh work (semantic tokens) to the client
    if self.lsp then self.lsp:post_render() end
  end

  function Ed:render_status(rows, cols, cur_line, cur_col, cur_off)
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
      local avail = cols - utf8.width(left) - utf8.width(right) - 1
      msg_part = text_trunc(msg_part, math.max(0, avail))
      local pad = math.max(0, avail - utf8.width(msg_part))
      self.term:write(Term.REVERSE)
      self.term:write(left .. msg_part .. string.rep(" ", pad) .. right .. " ")
      self.term:write(Term.RESET)
    end
  end

  function Ed:render_cursor(saved_off, lnum_width, rows, cols)
    self.doc:seek("set", saved_off)
    local cur_line = self.doc:line()
    local cur_screen_row = cur_line - self.scroll_line + 1
    if cur_screen_row < 1 then cur_screen_row = 1 end
    if cur_screen_row > rows - 1 then cur_screen_row = rows - 1 end

    local cur_line_text = line_text(self.doc, cur_line)
    local byte_col = self.doc:column()
    -- cursor skips hints on motion; at the byte gap (insert) it may sit
    -- on the hint's first char (append semantics, input lands before it)
    local display_col = self:vtext_dcol(cur_line, byte_col,
      self.mode == "INSERT")

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
  if key == "<Escape>" or key == "<C-c>" then ed.msg = ""; return end
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
  if fn then fn(self, key); self.msg = "" end
end
mode_dispatch.visual = visual_key
mode_dispatch.command = command_key

-- ================================================================
-- Section 6: Main
-- ================================================================

local function main(argv)
  local e = argv[1] and Ed.open(argv[1]) or Ed.new()
  e.term:enter()

  -- automatic LSP: silently enable when a server exists for the file
  local cmd = lsp_cmd(e.filename)
  if cmd then e:lsp_start(cmd, true) end

  -- Catch exit signals (raw mode: no signals, but just in case)
  local ok, err = pcall(function()
    while not e.done do
      if e.lsp then e.lsp:poll() end
      e:render()
      local key = e.term:getkey(100) -- 100ms idle slice for tick()
      if key then e:dispatch(key) end
      e:tick()
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
