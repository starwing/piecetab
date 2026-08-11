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
local ts = require("treesitter")
local lspclient = require("lspclient")
local lsp_span = require("lsp_span")
local yyjson = require("yyjson")
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
  if logfile then
    logfile:write(string.format(fmt, ...) .. "\n")
  end
end

-- ================================================================
-- Section 1: Term class (terminal I/O via termfeed, not exported)
-- ================================================================

---@alias editor.Mode "normal"|"insert"|"command"|"visual"
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

  --- Read one key, waiting up to `timeout` ms (nil = timed out, the
  --- caller can run idle work). Defaults to the ESC prefix window.
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

-- UTF-16 unit column -> byte offset within a UTF-8 line. BMP chars are
-- 1 unit; only 4-byte supplementary chars (emoji) are 2 (clamp to char
-- boundary, LSP positions never split chars).
---@param text string
---@param units integer
local function text_utf16_to_byte(text, units)
  local i = 1
  while i <= #text and units > 0 do
    local nxt = utf8.next(text, i) or #text + 1
    units = units - (nxt - i == 4 and 2 or 1)
    i = nxt
  end
  return i - 1
end

-- LSP position {line, character=UTF-16 units} -> byte offset.
---@param doc piecetab.Doc
---@param line integer
---@param unitcol integer
local function doc_utf16_to_byte(doc, line, unitcol)
  local saved = doc:offset()
  doc:seek("line", line)
  local base = doc:offset()
  local text = doc:read("l") or ""
  doc:seek("set", saved)
  return base + text_utf16_to_byte(text, unitcol)
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
-- Section 3: Highlight module (style compositor + tree-sitter)
-- ================================================================

-- Style compositor: interns attr-field tables to unique handles. Cellgrid
-- treats style ids as opaque handles; the compositor owns the attr ->
-- handle -> CSI conversion. Field values: fg/bg = 256-color index or
-- {r,g,b} table; boolean keys (bold/underline/...) = set attribute.

--- @class editor.Sc
--- @field by_attr table<string, integer>
--- @field attr_by table<integer, table>
--- @field next_id integer
local sc = {}

local SGR_ATTR = {
  bold = 1, dim = 2, italic = 3, underline = 4, reverse = 7,
}

do
  sc.__index = sc

  --- @return editor.Sc
  function sc.new()
    local self = setmetatable({ by_attr = {}, attr_by = {}, next_id = 0 }, sc)
    self:intern({}) -- style 0 = default (empty) attr
    return self
  end

  -- Canonical key: sorted "k:v" parts; booleans as bare "k"; {r,g,b} as
  -- "k:rgb(r,g,b)". Nil/false fields are unset and skipped.
  --- @param attr table
  --- @return string
  local function canon(attr)
    local parts = {}
    for k, v in pairs(attr) do
      if v then
        if type(v) == "table" then
          parts[#parts + 1] = k .. ":rgb(" .. v.r .. "," .. v.g .. "," .. v.b .. ")"
        elseif v ~= true then
          parts[#parts + 1] = k .. ":" .. tostring(v)
        else
          parts[#parts + 1] = k
        end
      end
    end
    table.sort(parts)
    return table.concat(parts, ",")
  end

  -- Intern an attr table to a unique handle; identical attrs share one.
  --- @param attr table
  --- @return integer
  function sc:intern(attr)
    local key = canon(attr)
    local id = self.by_attr[key]
    if id then return id end
    id = self.next_id
    self.next_id = id + 1
    self.by_attr[key] = id
    self.attr_by[id] = attr
    return id
  end

  -- Inverse lookup: handle -> attr table (compositor-owned, do not mutate).
  --- @param id integer
  --- @return table
  function sc:attr(id)
    return self.attr_by[id]
  end

  -- SGR escape for a handle: reset + attribute codes (diff emits this on
  -- style change, so each entry must be a full state).
  --- @param id integer
  --- @return string?
  function sc:csi(id)
    local a = self.attr_by[id]
    if not a then return nil end
    local codes = {}
    for k, v in pairs(a) do
      if v then
        if k == "fg" or k == "bg" then
          local pre = (k == "fg") and "38" or "48"
          if type(v) == "table" then
            codes[#codes + 1] = pre .. ";2;" .. v.r .. ";" .. v.g .. ";" .. v.b
          else
            codes[#codes + 1] = pre .. ";5;" .. v
          end
        else
          local n = SGR_ATTR[k]
          if n then codes[#codes + 1] = tostring(n) end
        end
      end
    end
    if #codes == 0 then return "\27[0m" end
    table.sort(codes)
    return "\27[0m\27[" .. table.concat(codes, ";") .. "m"
  end
end

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

-- LSP server command for a file (nil = no server available).
---@param filename string?
---@return string[]?
local function lsp_cmd(filename)
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

local HL_ATTRS = {
  comment      = ATTR_COMMENT,
  string       = ATTR_STRING,
  keyword      = ATTR_KEYWORD,
  ["function"] = ATTR_FUNCTION,
}

-- LSP semantic tokenType names -> attrs (unknown names ignored; clangd
-- duplicate legend names map by name, same name -> same attr).
local LSP_ATTRS = {
  comment      = ATTR_COMMENT,
  string       = ATTR_STRING,
  keyword      = ATTR_KEYWORD,
  number       = ATTR_NUMBER,
  ["function"] = ATTR_FUNCTION,
  method       = ATTR_FUNCTION,
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

-- Fold layered writer spans into one attr span list. Higher layers
-- override keys set on lower ones (key-level partial merge); unset keys
-- pass through. Input/output: {offset, length, attr} (0-based).
--- @param layers table array of span arrays, low to high
--- @param start integer
--- @param endoff integer (exclusive)
--- @return table
local function merge_layers(layers, start, endoff)
  local bounds = {}
  for _, spans in ipairs(layers) do
    for _, sp in ipairs(spans) do
      bounds[#bounds + 1] = sp.offset
      bounds[#bounds + 1] = sp.offset + sp.length
    end
  end
  table.sort(bounds)
  local out, lo = {}, start
  local function fold(hi)
    local attr = {}
    for _, spans in ipairs(layers) do
      for _, sp in ipairs(spans) do
        if sp.offset <= lo and lo < sp.offset + sp.length then
          for k, v in pairs(sp.attr) do
            if v then attr[k] = v end
          end
        end
      end
    end
    out[#out + 1] = { offset = lo, length = hi - lo, attr = attr }
  end
  for _, hi in ipairs(bounds) do
    if hi <= lo then
      -- duplicate boundary, skip
    elseif hi >= endoff then
      fold(endoff)
      lo = endoff
      break
    else
      fold(hi)
      lo = hi
    end
  end
  if lo < endoff then fold(endoff) end
  return out
end

-- Diag span containing byte offset `off`; highest severity wins
-- (LSP: 1 = error, higher numbers are weaker).
--- @param spans table array of {offset, length, msg, severity}
--- @param off integer
--- @return table?
local function lsp_diag_at(spans, off)
  local best
  for _, sp in ipairs(spans) do
    if sp.offset <= off and off < sp.offset + sp.length then
      if not best or sp.severity < best.severity then best = sp end
    end
  end
  return best
end

-- Display-column shift from inlay hints relative to the cursor column.
-- Normal motion (h/l) never rests on a hint: bytes at or past a hint's
-- start shift past it. In insert mode the cursor sits at the byte gap
-- (append semantics): the hint-start byte maps onto the hint's first
-- char — i/append input lands before the hint, no cursor/text tearing.
--- @param hints table?
--- @param dcol integer
--- @param at_start boolean  insert-mode (cursor at the byte gap)
--- @return integer
local function hint_offset(hints, dcol, at_start)
  if not hints then return 0 end
  local w = 0
  for _, h in ipairs(hints) do
    if at_start then
      if h.dcol >= dcol then break end
    elseif h.dcol > dcol then
      break
    end
    w = w + utf8.width(h.text)
  end
  return w
end

-- Decode inlayHint response items into per-line hint lists
-- {[line] = {{dcol, text}, ...}} sorted by display column. Position is
-- the insertion point (UTF-16), converted to the display column the
-- text is injected at; label is a string or an array of parts.
--- @param ed editor.Ed
--- @param hints table  inlayHint response (array or null)
--- @return table
local function lsp_hint_decode(ed, hints)
  local out = {}
  for _, h in ipairs(hints or {}) do
    local pos = h.position
    if pos then
      local saved = ed.doc:offset()
      ed.doc:seek("line", pos.line)
      local base = ed.doc:offset()
      local t = ed.doc:read("l") or ""
      ed.doc:seek("set", saved)
      local label = h.label
      if type(label) == "table" then
        local parts = {}
        for _, p in ipairs(label) do parts[#parts + 1] = p.value end
        label = table.concat(parts)
      end
      if type(label) == "string" and #label > 0 then
        local bcol = text_utf16_to_byte(t, pos.character)
        local lst = out[pos.line]
        if not lst then lst = {}; out[pos.line] = lst end
        lst[#lst + 1] = { dcol = text_byte_to_dcol(t, bcol, ed.tabstop),
          text = label }
      end
    end
  end
  for _, lst in pairs(out) do
    table.sort(lst, function(a, b) return a.dcol < b.dcol end)
  end
  return out
end

-- Piece-boundary writer (pull mode, no cache): scan all pieces, alternate
-- gray background on even pieces (first piece plain) to visualize
-- piecetab layout on screen.
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

-- Charwise visual selection [s, e): inclusive of both anchor and cursor
-- characters (vim semantics) — e = cursor char end, s = min char start.
-- Cursor always sits on a char start, so s is a plain min of the two.
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

-- Visual-selection writer (pull mode, no cache): charwise selection
-- [s, e) reverse-highlighted where it intersects the visible region.
-- Top layer in render. Cursor passed in: render moves the doc cursor
-- around (piece scan), so doc:offset() is unreliable here.
--- @param doc piecetab.Doc
--- @param sel integer?  selection anchor (byte offset)
--- @param cur integer  cursor byte offset
--- @param start integer
--- @param endoff integer (exclusive)
--- @return table
local function visual_spans(doc, sel, cur, start, endoff)
  local s, e = sel_range(doc, sel, cur)
  local lo = math.max(s, start)
  local hi = math.min(e, endoff)
  if hi <= lo then return {} end
  return { { offset = lo, length = hi - lo, attr = ATTR_REVERSE } }
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
---@param hints table?  sorted by dcol: {dcol, text, style} — injected
---  into the render stream (virt_text, never interned)
local function render_line(g, row, col, text, segs, tabstop, hints)
  local byte = 1
  local batch_start = 1
  local cur_style = 0
  local seg_idx = 1
  local hint_idx = 1
  local dc = 0

  -- display column before each character start (batch-flush independent,
  -- so hint injection can target the exact char position)
  local dcols = {}
  do
    local d, i = 0, 1
    while i <= #text do
      dcols[i] = d
      local b = text:byte(i)
      if b == 9 then
        d = d + tabstop - (d % tabstop)
        i = i + 1
      else
        local nxt = utf8.next(text, i) or #text + 1
        d = d + (utf8.width(text, i, nxt - 1) or 1)
        i = nxt
      end
    end
  end

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

  local function flush()
    if batch_start < byte then
      local s = text:sub(batch_start, byte - 1)
      dc = g:putline(row, col + dc, s, cur_style) - col
      batch_start = byte
    end
  end

  local function flush_hints()
    while hints and hint_idx <= #hints and hints[hint_idx].dcol <= dcols[byte] do
      flush() -- write the text before the hint position first
      local h = hints[hint_idx]
      dc = g:putline(row, col + dc, h.text, h.style) - col
      hint_idx = hint_idx + 1
    end
  end

  while byte <= #text do
    flush_hints()
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
  flush_hints()
  return col + dc
end

-- ================================================================
-- Section 4: Ed class
-- ================================================================

--- @class editor.Ed
---@field doc piecetab.Doc  document buffer (undo history + linecache)
---@field hl table?  syntax highlighter (tree-sitter), nil = no highlight
---@field filename string?
---@field mode string  "NORMAL"|"INSERT"|"COMMAND"|"VISUAL"
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
---@field commands table<string, fun(self: editor.Ed, arg?: string, bang?: boolean)>
---@field sc editor.Sc  style compositor (attr -> handle intern)
---@field styles table<string, integer>  pre-interned style handles
---@field show_pieces boolean  piece-boundary visualization layer
---@field sel_start integer?  visual-mode selection anchor (byte offset)
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
-- Full resync after undo/redo: the doc jump can't be localized, so the
-- server gets a whole-document didChange and caches refresh.
local function resync_lsp(ed)
  if ed.lsp then
    ed.lsp:sync_full()
    if ed.lsp_sem then ed.lsp_sem.dirty = true end
    ed.lsp_hint_dirty = true
  end
end

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
    resync_lsp(ed)
  end
  n.p = function(ed)
    if not ed.clip then return end
    ed:docedit(0, ed.clip)
    ed.doc:commit()
  end
  n.v = function(ed)
    ed.sel_start = ed.doc:offset()
    ed.mode = "VISUAL"
  end
  n["<C-r>"] = function(ed)
    ed.doc:redo()
    if ed.hl then ed.hl:reset() end
    resync_lsp(ed)
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
    ed.mode = "NORMAL"
    ed.sel_start = nil
  end
  v.d = function(ed)
    local s, e = sel_range(ed.doc, ed.sel_start, ed.doc:offset())
    ed.doc:seek("set", s)
    ed:docedit(e - s, "")
    ed.doc:commit()
    ed.mode = "NORMAL"
    ed.sel_start = nil
  end
  v["<Escape>"] = function(ed)
    ed.mode = "NORMAL"; ed.sel_start = nil
  end
  v["<C-c>"] = v["<Escape>"]
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
  c.pieces = function(ed, arg, bang)
    ed.show_pieces = not ed.show_pieces
  end
  c.lsp = function(ed, arg, bang)
    if arg == "on" then
      if ed.lsp then
        ed.msg = "lsp already on"; return
      end
      local cmd = lsp_cmd(ed.filename)
      if not cmd then
        ed.msg = "lsp: no server for this file"; return
      end
      ed:lsp_start(cmd)
    elseif arg == "off" then
      if ed.lsp then
        ed.lsp:stop()
        ed.lsp, ed.lsp_sem, ed.lsp_diag = nil, nil, nil
        ed.lsp_hints, ed.lsp_hint_view = nil, nil
      end
      ed.msg = "lsp off"
    elseif arg == "status" then
      ed.msg = ed.lsp and ("lsp: " .. ed.lsp.state) or "lsp: off"
    else
      ed.msg = "usage: :lsp on|off|status"
    end
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

  --- @return editor.Sc
  function Ed.newsc()
    return sc.new()
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
    self.scroll_line = 0
    self.tabstop = 4
    self.log = edlog
    self.done = false
    self.term = term or Ed.newterm()
    self.grid = grid or cg.new()
    self.sc = sc.new()
    self.styles = { dim = self.sc:intern(ATTR_DIM) }
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

  --- Start an LSP server process for the current buffer (document access
  -- wired to the live doc; edits funnel via docedit -> notify_edit).
  -- silent: fail quietly (automatic start); loud: report via on_status.
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
    self.lsp = lspclient.new({
      get_text = function() return ed.doc:dump() end,
      get_line = function(lnum)
        local saved = ed.doc:offset()
        ed.doc:seek("line", lnum)
        local t = ed.doc:read("l") or ""
        ed.doc:seek("set", saved)
        return t
      end,
      offset_pos = function(off)
        local saved = ed.doc:offset()
        ed.doc:seek("set", off)
        local line, col = ed.doc:line(), ed.doc:column()
        ed.doc:seek("set", saved)
        return line, col
      end,
      on_status = function(state, why)
        -- steady states render in the status bar segments; only report
        -- abnormal exits as a transient message (silent start: no)
        if state == "exited" and not silent then
          ed.msg = "lsp: " .. state .. (why and " (" .. why .. ")" or "")
        end
      end,
    })
    -- semantic tokens: full-snapshot cache, replaced atomically per
    -- response; dirty marks edits, kept rendering until the next snapshot
    self.lsp_sem = { spans = {}, dirty = true, pending = false }
    self.lsp_diag = nil
    -- inlay hints: per-viewport cache, refreshed on scroll or idle
    self.lsp_hints = nil
    self.lsp_hint_view = nil
    self.lsp_hint_pending = false
    self.lsp_hint_dirty = true
    self.last_edit_t = 0
    self.hint_idle = 1.0 -- seconds of no typing before a hint refresh
    -- answer LuaLS config requests: hints on (VSCode-default behavior),
    -- rest of the Lua section left unset so defaults apply
    self.lsp:on_server("workspace/configuration", function(params)
      local out = {}
      for _, item in ipairs(params and params.items or {}) do
        if item.section == "Lua" then
          out[#out + 1] = { hint = { enable = true, setType = true } }
        else
          out[#out + 1] = yyjson.null
        end
      end
      return out
    end)
    self.lsp:on("textDocument/publishDiagnostics", function(p)
      if p.uri ~= self.lsp.uri then return end
      -- drop stale snapshots (out-of-order pushes)
      local cur = self.lsp_diag and self.lsp_diag.version or -1
      local v = p.version
      if v and v < cur then return end
      local spans = {}
      for _, d in ipairs(p.diagnostics or {}) do
        local r = d.range
        local s = doc_utf16_to_byte(ed.doc, r.start.line, r.start.character)
        local e = doc_utf16_to_byte(ed.doc, r["end"].line, r["end"].character)
        if e > s then
          spans[#spans + 1] = {
            offset = s, length = e - s, attr = ATTR_DIAG,
            msg = d.message, severity = d.severity or 2,
          }
        end
      end
      ed.lsp_diag = { version = v or cur, spans = spans }
      for _, d in ipairs(p.diagnostics or {}) do
        if d.severity == 1 then
          ed.msg = "diag: " .. d.message
          break
        end
      end
    end)
    local ok = self.lsp:start(argv, "file://" .. fname,
      ext_lang(fname) or "plaintext",
      "file://" .. (fname:match("^(.*)/") or "."))
    if not ok then
      self.lsp, self.lsp_sem, self.lsp_diag = nil, nil, nil
      self.lsp_hints, self.lsp_hint_view = nil, nil
    end
    return ok
  end

  --- Shift cached hints after an edit (they are injected text: stale
  -- positions would squeeze/relocate new chars). Same-line hints past
  -- the edit point shift by the byte delta (approx: tab/wide-char
  -- columns self-heal on the next response); hints inside the deleted
  -- range are dropped; multi-line edits clear the cache.
  --- @param off integer
  --- @param del integer
  --- @param s string
  function Ed:edit_hints(off, del, s)
    local hints = self.lsp_hints
    if not hints then return end
    local saved = self.doc:offset()
    self.doc:seek("set", off)
    local line = self.doc:line()
    self.doc:seek("set", off + del)
    local eline = self.doc:line()
    self.doc:seek("set", saved)
    if line ~= eline or s:find("\n", 1, true) then
      self.lsp_hints = nil
      return
    end
    local lst = hints[line]
    if not lst then return end
    self.doc:seek("line", line)
    local base = self.doc:offset()
    local text = self.doc:read("l") or ""
    self.doc:seek("set", saved)
    local edcol = text_byte_to_dcol(text, off - base, self.tabstop)
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
    hints[line] = out
  end

  --- Edit at cursor with highlight notification (single edit funnel).
  function Ed:docedit(del, s)
    local off = self.doc:offset()
    if self.lsp then
      self.lsp:notify_edit(off, del, s)
      if self.lsp_sem then self.lsp_sem.dirty = true end
      self:edit_hints(off, del, s)
      self.lsp_hint_dirty = true
      self.last_edit_t = os.clock()
    end
    self.doc:edit(del, s)
    if self.hl then self.hl:notify_edit(off, del, #s) end
  end

  --- Idle work (called on main-loop timeouts, ~100ms): refresh inlay
  -- hints once typing has stopped (debounce) or the viewport moved.
  -- Continuous typing never requests; the stale-response guard (doc
  -- version) stays as a belt-and-braces against races.
  function Ed:tick()
    if not (self.lsp and self.lsp.state == "running") then return end
    if not (self.lsp.capabilities.inlayHintProvider
        and not self.lsp_hint_pending) then return end
    local idle = os.clock() - (self.last_edit_t or 0) >= self.hint_idle
    if not (self.lsp_hint_dirty and idle
        or self.lsp_hint_view ~= self.scroll_line) then return end
    local rows = self.term:size()
    local visend = math.min(self.scroll_line + rows - 1, self.doc:breaks())
    self.lsp_hint_pending = true
    self.lsp_hint_reqver = self.lsp.version
    self.lsp:request("textDocument/inlayHint", {
      textDocument = { uri = self.lsp.uri },
      range = { start = { line = self.scroll_line, character = 0 },
        ["end"] = { line = visend, character = 0x7fffffff } },
    }, function(result, err)
        self.lsp_hint_pending = false
        if err or not result then
          -- answered (null = no hints / unsupported): settle, don't
          -- refetch until the viewport moves or the doc is edited
          self.lsp_hint_dirty = false
          self.lsp_hint_view = self.scroll_line
        elseif self.lsp.version ~= self.lsp_hint_reqver then
          -- edited while in flight: keep the shifted cache, refetch
        else
          self.lsp_hints = lsp_hint_decode(self, result)
          self.lsp_hint_view = self.scroll_line
          self.lsp_hint_dirty = false
        end
      end)
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
    if self.hl or self.show_pieces or self.sel_start
        or (self.lsp_sem and #self.lsp_sem.spans > 0)
        or (self.lsp_diag and #self.lsp_diag.spans > 0) then
      self.doc:seek("line", self.scroll_line)
      local s_off = self.doc:offset()
      self.doc:seek("line", math.min(self.scroll_line + visrows, total_lines))
      local e_off = self.doc:offset()
      self.doc:seek("set", saved_off)
      local layers = {}
      if self.hl then
        layers[#layers + 1] = self.hl:query_region(s_off, e_off)
      end
      if self.show_pieces then
        layers[#layers + 1] = piece_spans(self.doc, s_off, e_off)
      end
      if self.sel_start then
        layers[#layers + 1] = visual_spans(self.doc, self.sel_start,
                                           saved_off, s_off, e_off)
      end
      if self.lsp_sem then
        layers[#layers + 1] = lsp_span.clip(self.lsp_sem.spans, s_off, e_off)
      end
      if self.lsp_diag then
        layers[#layers + 1] = lsp_span.clip(self.lsp_diag.spans, s_off, e_off)
      end
      spans = merge_layers(layers, s_off, e_off)
      for _, sp in ipairs(spans) do
        sp.style = self.sc:intern(sp.attr)
      end
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
        g:putline(r0, 0, s, self.styles.dim)
        g:clearrow(r0, #s, cols)
      else
        g:clearrow(r0, 0, cols)
        g:put(r0, lnum_width + 1, 0x7e, self.styles.dim)
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
        row = row, text = line_text, start = line_start, line = line_idx
      }
    end

    local col_start = lnum_width + 2
    local col_pad = col_start + text_width

    -- content + highlights (single pass, highlight-driven)
    for _, ld in ipairs(lines_data) do
      local r0 = ld.row - 1
      local segs = hl.line_segments(spans or {}, ld.start, ld.start + #ld.text)
      local hints = self.lsp_hints and self.lsp_hints[ld.line]
      if hints then
        for _, h in ipairs(hints) do h.style = self.styles.dim end
      end
      local endcol = render_line(g, r0, col_start - 1, ld.text, segs,
        self.tabstop, hints)
      if endcol < col_pad - 1 then
        g:clearrow(r0, endcol, col_pad - 1)
      end
    end

    -- flush grid diff
    local sc = self.sc
    local csi = g:diff(setmetatable({}, {
      __index = function(_, id) return sc:csi(id) end,
    }))
    self.log("  diff: csi_len=%d", #csi)
    self.term:write(csi)

    self:render_status(rows, cols, cur_line, cur_col, saved_off)
    self:render_cursor(saved_off, lnum_width, rows, cols)
    self.term:flush()
    g:freeze()

    -- after render: refresh semantic tokens when dirty (edit once ->
    -- one request; skip while a request is already in flight)
    if self.lsp and self.lsp.state == "running" and self.lsp_sem
        and self.lsp_sem.dirty and not self.lsp_sem.pending then
      local cap = self.lsp.capabilities.semanticTokensProvider
      if not (cap and cap.full) then
        self.lsp_sem.dirty = false
      else
        local sem = self.lsp_sem
        sem.pending = true
        self.lsp:request("textDocument/semanticTokens/full", {
          textDocument = { uri = self.lsp.uri },
        }, function(result, err)
            sem.pending = false
            if err then
              sem.dirty = false
            elseif result and result.data then
              sem.spans = lsp_span.decode(result.data, cap.legend,
                LSP_ATTRS, function(line, unit)
                  return doc_utf16_to_byte(self.doc, line, unit)
                end)
              sem.dirty = false
            end
          end)
      end
    end
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
      -- wins (recomputed every frame, gone once the cursor leaves);
      -- otherwise the event message (command feedback, lsp events)
      local at = self.lsp_diag and lsp_diag_at(self.lsp_diag.spans, cur_off)
      local msg_part = ""
      if at then
        msg_part = " diag: " .. at.msg
      elseif #self.msg > 0 then
        msg_part = " " .. self.msg
      end
      -- right: persistent server state, short form
      local right = self.lsp
          and (self.lsp.state == "running" and " lsp:on"
            or (" lsp:" .. self.lsp.state)) or ""
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

    local saved = self.doc:offset()
    self.doc:seek("line", cur_line)
    local cur_line_text = self.doc:read("l") or ""
    self.doc:seek("set", saved)
    local byte_col = self.doc:column()
    self.log("cursor: saved_off=%d cur_line=%d line_text=[%s](%d) byte_col=%d",
      saved_off, cur_line, cur_line_text:gsub("\n", "\\n"), #cur_line_text, byte_col)
    local display_col = text_byte_to_dcol(cur_line_text, byte_col, self.tabstop)
    -- cursor skips hints on motion; at the byte gap (insert) it may sit
    -- on the hint's first char (append semantics, input lands before it)
    display_col = display_col
      + hint_offset(self.lsp_hints and self.lsp_hints[cur_line], display_col,
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
-- visual dispatch: keymap hit -> fn, else ignore (motions live in keymap)
local function visual_key(self, key)
  local fn = self.keymaps.visual[key]
  if fn then
    fn(self, key); self.msg = ""
  end
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

-- attr tables for tests (intern via e.sc:intern(Ed.ATTR_*) to get handles)
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
