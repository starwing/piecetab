#!/usr/bin/env lua
-- editor.lua -- piecetab-based terminal text editor (class skeleton)
-- usage: lua editor.lua [file]

package.cpath = package.cpath
    .. (_G["jit"] and ";./lua/luajit/?.so" or ";./lua/?.so")
    .. ";./lua/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
package.path = package.path .. ";./lua/?.lua"
local pt = require("piecetab")
local cg = require("cellgrid")

local tf = require("termfeed")
local sp = require("spantree")
local ok, ts = pcall(require, "treesitter")
if not ok then ts = nil end
local ok, lsp = pcall(require, "lsp")
if not ok then lsp = nil end

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
local SGR_ATTR   = { bold = 1, dim = 2, italic = 3, underline = 4, reverse = 7 }

-- Section 2: Highlight module (tree-sitter highlighter)
-- The highlighter is a tree-sitter adapter: it depends only on a
-- piecetab.Doc (text source) and the Ed class ATTR_* tables, never
-- on an Ed instance.

local hl         = {}

-- Minimal highlights subsets (keyword/string/comment/function).
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

-- file extension -> tree-sitter language id (plaintext when unknown)
local FILE_LANG  = { c = "c", h = "c", lua = "lua" }
local function file_langid(filename)
  local ext = filename and filename:match("%.([%w_]+)$")
  return ext and FILE_LANG[ext] or "plaintext"
end

--- Create a highlighter for a language ("c"/"lua"), nil if unsupported.
---@param doc piecetab.Doc
---@param lang string
---@param attrs editor.Ed  Ed class (only ATTR_* tables are read)
function hl.new(doc, lang, attrs)
  if not ts then return nil end
  local qsrc = HL_QUERIES[lang]
  if not qsrc then return nil end
  local langobj = ts.require(lang)
  if not langobj then return nil end
  local parser = ts.parser.new()
  parser.language = langobj
  local self = {
    doc = doc,
    attrs = {
      comment      = attrs.ATTR_COMMENT,
      string       = attrs.ATTR_STRING,
      keyword      = attrs.ATTR_KEYWORD,
      ["function"] = attrs.ATTR_FUNCTION,
    },
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
function hl:on_edit(start, old_len, new_len)
  if self.tree then
    local function pos(off)
      local line, col = self.doc:linecol(off)
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
  self.tree = self.parser:parse(self.tree, self.doc:dump())
  self.dirty = false
end

--- Query a byte range and write captures directly into a spantree
--- namespace: hl:query_region(sp, ns, start, endoff).
function hl:query_region(sp, ns, start, endoff)
  self:ensure()
  if not self.tree then return end
  local c = self.query:exec(self.tree.root)
  c:set_byte_range(start + 1, endoff)
  local attrmap = self.attrs
  while true do
    local ci = c:next_capture()
    if not ci then break end
    local node = c[ci]
    local _, cid = c:captures(ci)
    local attr = attrmap[self.query:capture_name_for_id(cid)]
    if attr then
      sp:mark(ns, attr, node.start_byte - 1, node.end_byte - node.start_byte)
    end
  end
end

-- Section 3: Ed class

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
---@field repaint boolean  whether a repaint is needed
local Ed            = {}

-- attribute field tables (interned into grid style handles by sc)
Ed.ATTR_DIM         = { dim = true }
Ed.ATTR_HINT        = { bg = 236, fg = 252 } -- Neovim-ish inlay hint bg
Ed.ATTR_GRAY_BG     = { bg = 237 }
Ed.ATTR_KEYWORD     = { fg = 207 }
Ed.ATTR_STRING      = { fg = 114 }
Ed.ATTR_COMMENT     = { fg = 245 }
Ed.ATTR_FUNCTION    = { fg = 81 }
Ed.ATTR_REVERSE     = { reverse = true }

-- forward declaration: filled in Section 5 (dispatch reads it via upvalue)
local mode_dispatch = {}

-- built-in normal keymaps (per-instance, called from Ed.new)
local function install_normal_keys(self)
  local n = self.keymaps.normal

  local function word_class(byte)
    if byte >= 48 and byte <= 57 then return 1 end  -- digit
    if byte >= 65 and byte <= 90 then return 1 end  -- upper
    if byte >= 97 and byte <= 122 then return 1 end -- lower
    if byte == 95 then return 1 end                 -- underscore
    return 0
  end

  local function move_word_forward(ed)
    local doc = ed.doc
    local saved = doc:offset()
    local lnum = doc:line()
    local line = doc:readat(doc:lineoffset(lnum), doc:linelen(lnum, true))
    local col = doc:column()
    local len = #line
    local i = col
    if i < len then
      local cls = word_class(line:byte(i + 1))
      while i < len and word_class(line:byte(i + 1)) == cls do i = i + 1 end
      while i < len and line:byte(i + 1) == 32 do i = i + 1 end
    end
    doc:seek("cur", i - col)
    if doc:offset() ~= saved then ed.goal = nil end
  end

  local function move_word_backward(ed)
    local doc = ed.doc
    local saved = doc:offset()
    local lnum = doc:line()
    local line = doc:readat(doc:lineoffset(lnum), doc:linelen(lnum, true))
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

  -- Move cursor vertically by dl lines, preserving the screen column
  -- (injected text counts; Neovim curswant survives the EOL clamp).
  local function move_vert(ed, dl)
    local doc = ed.doc
    local lnum = (not ed.text_dirty) and doc:line() or ed.cursor_row
    local nlnum = lnum + dl
    if nlnum < 0 or nlnum >= doc:breaks() then return end
    local scol = ed.goal or ed.cursor_col
    ed.goal = scol
    ed.cursor_row = nlnum
    ed.cursor_col = scol
    ed.text_dirty = true
  end

  local function open_line(self, dir)
    self.doc:seek("line", self.doc:line())
    if dir > 0 then
      local lnum = self.doc:line()
      self.doc:seek("line", lnum, self.doc:linelen(lnum, true))
    end
    self:doc_edit(0, "\n")
    if dir < 0 then self.doc:seek("cur", -1) end
    self.mode = "INSERT"
  end

  function n.h(ed)
    if ed.doc:offset() > 0 then
      ed.goal = nil; ed.doc:advancechars(-1)
    end
  end

  function n.l(ed)
    if ed.doc:offset() < #ed.doc then
      ed.goal = nil; ed.doc:advancechars(1)
    end
  end

  function n.j(ed) move_vert(ed, 1) end

  function n.k(ed) move_vert(ed, -1) end

  function n.w(ed) move_word_forward(ed) end

  function n.b(ed) move_word_backward(ed) end

  n["0"] = function(ed)
    ed.goal = nil; ed.doc:seek("line", ed.doc:line())
  end
  n["$"] = function(ed)
    ed.goal = nil
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum, ed.doc:linelen(lnum, true))
    if ed.doc:column() > 0 then ed.doc:advancechars(-1) end
  end
  function n.gg(ed)
    ed.goal = nil; ed.doc:seek("line", 0)
  end

  function n.G(ed)
    ed.goal = nil
    ed.doc:seek("line", ed.doc:breaks() - 1)
  end

  function n.x(ed) ed:on_edit(1, "") end

  function n.dd(ed)
    local lnum = ed.doc:line()
    ed.doc:seek("line", lnum)
    ed:on_edit(ed.doc:linelen(lnum), "")
  end

  function n.i(ed) ed.mode = "INSERT" end

  function n.a(ed)
    if ed.doc:offset() < #ed.doc then
      ed.goal = nil; ed.doc:advancechars(1)
    end
    ed.mode = "INSERT"
  end

  function n.o(ed) open_line(ed, 1) end

  function n.O(ed) open_line(ed, -1) end

  function n.u(ed) ed:switch_version("undo") end

  n["<C-r>"] = function(ed) ed:switch_version("redo") end
  function n.p(ed)
    if not ed.clip then return end
    ed:on_edit(0, ed.clip)
  end

  function n.v(ed)
    ed.sel_start = ed.doc:offset(); ed.mode = "VISUAL"
  end

  n["<C-l>"] = function(ed) ed.grid:clear() end
  n[":"] = function(ed)
    ed.mode = "COMMAND"; ed.cmdline = ""
  end
  n["<Up>"], n["<Down>"], n["<Left>"], n["<Right>"] = n.k, n.j, n.h, n.l
end

-- built-in insert keymaps (per-instance, called from Ed.new)
local function install_insert_keys(self)
  local i, n = self.keymaps.insert, self.keymaps.normal

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
      self:doc_edit(off - self.doc:offset(), "")
    end
  end

  local function ins_delete(self)
    local n = self.doc:charlen()
    if n > 0 then self:doc_edit(n, "") end
  end

  i["<Escape>"] = ins_escape
  i["<Backspace>"] = ins_backspace
  i["<Delete>"] = ins_delete
  i["<Enter>"] = function(ed) ed:doc_edit(0, "\n") end
  i["<Tab>"] = function(ed) ed:doc_edit(0, "\t") end
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
  local function page(ed, dl, key)
    local rows = ed:size()
    for _ = 1, rows - 2 do
      if dl > 0 then n.j(ed, key) else n.k(ed, key) end
    end
  end
  i["<PageUp>"] = function(ed) page(ed, -1, "<PageUp>") end
  i["<PageDown>"] = function(ed) page(ed, 1, "<PageDown>") end
end

-- built-in visual keymaps (per-instance, called from Ed.new)
local function install_visual_keys(self)
  local n, v = self.keymaps.normal, self.keymaps.visual
  -- motions reuse normal handlers; cursor moves extend the selection
  for _, k in ipairs({ "h", "l", "j", "k", "w", "b", "0", "$",
    "<Up>", "<Down>", "<Left>", "<Right>" }) do
    v[k] = n[k]
  end
  function v.y(ed)
    local sel = ed.sel_start
    local cur = ed.doc:offset()
    local s, e = sel or cur, cur
    if s > e then s, e = e, s end
    local n = ed.doc:charlen(e)
    ed.doc:seek("set", s)
    ed.clip = ed.doc:buffer():read(s, e - s + n)
    ed.mode = "NORMAL"; ed.sel_start = nil
  end

  function v.d(ed)
    local sel = ed.sel_start
    local cur = ed.doc:offset()
    local s, e = sel or cur, cur
    if s > e then s, e = e, s end
    local n = ed.doc:charlen(e)
    ed.doc:seek("set", s)
    ed:on_edit(e - s + n, "")
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
  function c.w(ed, arg, bang)
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

  function c.q(ed, arg, bang) ed:quit() end

  function c.wq(ed, arg, bang)
    c.w(ed); c.q(ed)
  end

  function c.pieces(ed, arg, bang)
    ed.show_pieces = not ed.show_pieces
  end

  function c.lsp(ed, arg, bang)
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

  function c.e(ed, arg, bang)
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
    self.cursor_row = self.doc:line()
    self.cursor_col = self.doc:column()
    self.text_dirty = false
    self.log = edlog
    self.done = false
    self.repaint = false
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
    self.styles = {
      dim = self.comp:intern(Ed.ATTR_DIM),
      hint = self.comp:intern(Ed.ATTR_HINT),
    }
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
    self:open_language(file_langid(filename))
    self.saved_vid = self.doc:version()
    self.scroll_line = 0
    self.cursor_row = self.doc:line()
    self.cursor_col = self.doc:column()
    self.text_dirty = false
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
    self.hl = lang and hl.new(self.doc, lang, Ed)
  end

  --- Start an LSP server via lsp.attach. The lsp module is optional:
  -- absent -> no server (nil), same as an unsupported file.
  --- @param silent? boolean
  --- @param argv? string[]  override the configured server command
  --- @return boolean  false when lsp is absent or no server is configured
  function Ed:lsp_start(silent, argv)
    if self.lsp then return true end
    if not lsp then return false end
    return lsp.attach(self, { silent = silent, argv = argv })
  end

  -- vtext: injected display text (virt text). Data lives in the spantree
  -- "vtext" layer (a service on top of the span tree); consumers (LSP
  -- inlay hints) write per-line lists, edit shifting is the tree's splice.
  --- @param line integer
  --- @param list table<integer, {off: integer, text: string, style?: integer}>?  nil/empty clears
  function Ed:set_vtext(line, list)
    self:_render_line(self.cursor_row, true)
    local lo = self.doc:lineoffset(line)
    local ll = self.doc:linelen(line, true)
    self.tree:clear("vtext", lo, ll + (line < self.doc:breaks() and 1 or 0))
    for _, h in ipairs(list or {}) do
      local n = self.doc:charlen(lo + h.off)
      if n > 0 then
        self.tree:mark("vtext", { vtext = h.text, vstyle = h.style },
          lo + h.off, n)
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
    self:_render_line(self.cursor_row, true)
    self.tree:clear("sem")
    for _, s in ipairs(spans) do
      self.tree:mark("sem", s.attr, s.offset, s.length)
    end
  end

  --- @param spans table  array of {offset, length, attr}
  function Ed:set_diag(spans)
    self:_render_line(self.cursor_row, true)
    self.tree:clear("diag")
    for _, s in ipairs(spans) do
      self.tree:mark("diag", s.attr, s.offset, s.length)
    end
  end

  -- Jump doc versions (undo/redo): splice change hunks into the span
  -- tree and feed them to the LSP as sequential edits.
  function Ed:switch_version(name)
    self:_render_line(self.cursor_row, true)
    if self.hl then self.hl:reset() end
    local function sync(f)
      self.doc[name](self.doc, function(off, del, text)
        self.tree:splice(off, del, #text)
        if f then f(off, del, text) end
      end)
    end
    if self.lsp then self.lsp:undo_switch(sync) else sync() end
  end

  -- Edit at cursor with highlight notification (single edit funnel):
  -- notify the LSP, edit the doc, sync the span tree (the vtext layer
  -- shifts with the splice), then the highlighter.
  function Ed:doc_edit(del, s)
    self:_render_line(self.cursor_row, true)
    local off = self.doc:offset()
    if self.lsp then self.lsp:on_edit(off, del, s) end
    self.doc:edit(del, s)
    self.tree:splice(off, del, #s)
    if self.hl then self.hl:on_edit(off, del, #s) end
  end

  -- Edit + commit (normal-mode ops: x/dd/p, visual d)
  function Ed:on_edit(del, s)
    self:doc_edit(del, s)
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

  function Ed:quit() self.done = true end

  function Ed:write(s) self.term.write(self.term, s) end

  function Ed:flush() self.term.flush(self.term) end

  function Ed:size() return self.term.size() end

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

  --- Text-coordinate accessors (lazy screen-to-doc sync). Reading any of
  -- these after a j/k motion materializes the Doc cursor from the
  -- screen coordinates via render_line(dry_run=true).
  function Ed:text_line()
    self:_render_line(self.cursor_row, true)
    return self.doc:line()
  end

  function Ed:text_col()
    self:_render_line(self.cursor_row, true)
    return self.doc:column()
  end

  function Ed:text_offset()
    self:_render_line(self.cursor_row, true)
    return self.doc:offset()
  end

  function Ed:dispatch(key)
    if not key then return end
    self:_render_line(self.cursor_row, true)
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
    if #codes == 0 then return "\27[0m" end                -- RESET
    return "\27[0m\27[" .. table.concat(codes, ";") .. "m" -- SGR
  end

  -- Render one line (or dry-run its cursor mapping). Single styled-run
  -- walk shared by real render and dry_run; the two only differ in cell writes.
  --- @overload fun(self: editor.Ed, line_idx: integer, dry_run: true): integer?
  --- @overload fun(self: editor.Ed, line_idx: integer, dry_run: nil, row: integer, col: integer): integer
  --- @param line_idx integer  line to render
  --- @param dry_run? boolean
  --- @param row? integer
  --- @param col? integer  0-based render column for the text area
  --- @return integer?  absolute end column; dry_run may return nil only when not dirty
  function Ed:_render_line(line_idx, dry_run, row, col)
    if dry_run then
      if not self.text_dirty then return end
      row, col = 0, 0
    end
    row, col = assert(row), assert(col)

    local g = self.grid
    local lo = self.doc:lineoffset(line_idx)
    local text_end = self.doc:linelen(line_idx, true)
    local eol_cursor_dc
    local cursor_line = line_idx == self.cursor_row
    local cursor_byte = self.doc:column() + 1
    local cand_byte, cand_col -- deferred dirty mapping
    local text_start, style_start, dc = 0, 0, 0

    -- doc byte -> screen column: only before the cursor byte; later
    -- spans overwrite with their own start, C clamps k to the span.
    local function record(seg_start, seg_text, screen_base)
      if cursor_line and not self.text_dirty and seg_start < cursor_byte then
        self.cursor_col = g:cols(screen_base, seg_text, 1,
          cursor_byte - seg_start - 1)
      end
    end

    -- screen column -> doc byte: remember the last span whose screen
    -- start is not past target; g:byte clamps past-end to span end+1.
    local function consider(seg_start, seg_text, screen_base)
      if cursor_line and self.text_dirty and self.cursor_col >= screen_base then
        local rel = g:byte(screen_base, self.cursor_col - screen_base, seg_text)
        cand_byte, cand_col = seg_start + rel - 1, self.cursor_col
      end
    end

    local function commit_dirty()
      if not (cursor_line and self.text_dirty) then return end
      local byte = cand_byte or text_end
      self.doc:seek("line", line_idx, byte)
      self.text_dirty = false
      self.cursor_col = byte == text_end and (eol_cursor_dc or dc) or cand_col
    end

    for off, len, attr, id in self.tree:styled(lo, text_end + 1) do
      local rel_off = off - lo
      local is_vtext = attr.vtext ~= nil
      if rel_off < text_end or is_vtext then
        if not is_vtext then
          local run_end = math.min(rel_off + len, text_end)
          if run_end > rel_off then
            local run_text = self.doc:readat(lo + rel_off, run_end - rel_off)
            record(rel_off, run_text, dc)
            local newdc = g:cols(dc, run_text)
            if not dry_run then
              g:span(row, col + dc, col + newdc, id)
            end
            dc = newdc
          end
        else
          if rel_off > text_start then
            local pending = self.doc:readat(lo + text_start, rel_off - text_start)
            record(text_start, pending, style_start)
            consider(text_start, pending, style_start)
            if not dry_run then
              g:putstring(row, col + style_start, nil, pending)
            end
          end
          local hint = attr.vtext
          local is_eol = rel_off >= text_end
          if is_eol then eol_cursor_dc = eol_cursor_dc or dc end
          local pre_hint_dc = dc
          local hint_w = g:cols(dc, hint) - dc
          if cursor_line and self.text_dirty and self.cursor_col >= dc
              and self.cursor_col < dc + hint_w then
            if is_eol then
              cand_byte, cand_col = text_end, eol_cursor_dc
            else
              self.cursor_col = dc + hint_w
            end
          end
          if not dry_run then
            g:putstring(row, col + dc, attr.vstyle or self.styles.hint, hint)
          end
          dc = dc + hint_w
          local anchor = rel_off < text_end
              and self.doc:readat(lo + rel_off, math.min(len, text_end - rel_off))
              or ""
          if #anchor > 0 then
            if self.mode == "INSERT" and not self.text_dirty and cursor_line
                and cursor_byte == rel_off + 1 then
              self.cursor_col = pre_hint_dc
            else
              record(rel_off, anchor, dc)
            end
            consider(rel_off, anchor, dc)
            local newdc = g:cols(dc, anchor)
            if not dry_run then
              g:putstring(row, col + dc, id, anchor)
            end
            dc = newdc
          end
          text_start = rel_off + len
          style_start = dc
        end
      end
    end

    if text_start < text_end then
      local remaining = self.doc:readat(lo + text_start, text_end - text_start)
      record(text_start, remaining, style_start)
      consider(text_start, remaining, style_start)
      if not dry_run then
        g:putstring(row, col + style_start, nil, remaining)
      end
    end

    if cursor_line and not self.text_dirty and cursor_byte == text_end + 1 then
      self.cursor_col = eol_cursor_dc or dc
    end

    commit_dirty()
    return col + dc
  end

  --- @param force? boolean
  function Ed:render(force)
    if not force and not self.repaint then return end
    self:write("\27[?25l") -- hide cursor
    local rows, cols = self:size()
    local visrows = rows - 1
    if not self.text_dirty then self.cursor_row = self.doc:line() end
    local cur_line = self.cursor_row
    local total_lines = self.doc:breaks()
    local lnum_width = math.max(3, tostring(total_lines):len())
    local text_width = cols - lnum_width - 2

    if cur_line < self.scroll_line then
      self.scroll_line = cur_line
    elseif cur_line >= self.scroll_line + visrows then
      self.scroll_line = cur_line - visrows + 1
    end
    if self.scroll_line < 0 then self.scroll_line = 0 end

    self.log("render: size=%dx%d scroll=%d cur=%d,%d total=%d",
      rows, cols, self.scroll_line, cur_line, self.cursor_col, total_lines)

    local s_off = self.doc:lineoffset(self.scroll_line)
    local e_off = self.doc:lineoffset(
      math.min(self.scroll_line + visrows, total_lines))
    -- tree-sitter spans into the tree's "hl" eph layer (below the
    -- persistent ns layers): every tree edit clears it, the next frame
    -- refills from a fresh query
    if self.hl then
      self.tree:clear("hl", s_off, e_off - s_off)
      self.hl:query_region(self.tree, "hl", s_off, e_off)
    end
    -- quick layers: piece bg, then visual reverse (tree folds them
    -- above hl/sem/diag)
    self.tree:clear("piece")
    self.tree:clear("visual")
    if self.show_pieces then
      local odd = false
      for off, len, _ in self.doc:buffer():pieces() do
        if odd and off + len > s_off and off < e_off then
          self.tree:mark("piece", Ed.ATTR_GRAY_BG,
            math.max(off, s_off),
            math.min(off + len, e_off) - math.max(off, s_off))
        end
        odd = not odd
      end
    end
    local sel = self.sel_start
    if sel then
      local cur = self:text_offset()
      local lo, hi = sel, cur
      if sel > cur then lo, hi = cur, sel end
      local n = self.doc:charlen(hi)
      self.tree:mark("visual", Ed.ATTR_REVERSE, lo, hi - lo + n)
    end
    local g = self.grid
    g:begin(self.scroll_line, visrows, cols)

    local lnum_fmt = "%" .. lnum_width .. "d "
    for r0 = 0, visrows - 1 do
      local line_idx = self.scroll_line + r0
      if line_idx < total_lines then
        local s = string.format(lnum_fmt, line_idx + 1)
        g:putstring(r0, 0, self.styles.dim, s)
        g:clearrow(r0, #s, cols)
      else
        g:clearrow(r0, 0, cols)
        g:put(r0, lnum_width + 1, 0x7e, self.styles.dim)
      end
    end

    -- content + highlights (read-only line iteration; doc cursor stays put)
    local col_start = lnum_width + 2
    local col_pad = col_start + text_width
    for r0 = 0, visrows - 1 do
      local line_idx = self.scroll_line + r0
      if line_idx >= total_lines then break end
      local endcol = self:_render_line(line_idx, nil, r0,
        col_start - 1) --[[@as integer]]
      if endcol < col_pad - 1 then
        g:clearrow(r0, endcol, col_pad - 1)
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

    local cur_off = self.doc:offset()
    local cur_line2 = self.doc:line()
    local cur_col2 = self.doc:column()
    self:render_status(rows, cols, cur_line2, cur_col2, cur_off)
    self:render_cursor(lnum_width, rows, cols)
    self:flush()
    g:freeze()

    -- after render: delegate refresh work (semantic tokens) to the client
    if self.lsp then self.lsp:post_render() end
    self.repaint = false
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
      -- center: transient messages: the diag message under the cursor
      -- wins (gone once the cursor leaves); else the event message
      local at = self.lsp and self.lsp:diag_at(cur_off)
      local msg_part = at and (" diag: " .. at.msg)
          or (#self.msg > 0 and " " .. self.msg or "")
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
    local cur_screen_row = math.max(1,
      math.min(cur_line - self.scroll_line + 1, rows - 1))

    local display_col = self.cursor_col or 0
    -- cursor_col is recorded by _render_line (text-to-screen, hints skipped)

    local cur_screen_col = math.min(display_col + lnum_width + 2, cols)

    self:move(cur_screen_row, cur_screen_col)
    self:write("\27[?25h") -- show cursor
  end
end

-- Section 4: mode_dispatch skeleton

function mode_dispatch.normal(ed, key)
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
  if fn then return fn(self, key) end
  if key_printable(key) then self:doc_edit(0, key) end
end
mode_dispatch.insert = insert_key
-- command dispatch: keymap hit -> fn, else printable chars append cmdline
local function command_key(self, key)
  local fn = self.keymaps.command[key]
  if fn then return fn(self, key) end
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

-- Section 5: Main

local function main(argv)
  local e = argv[1] and Ed.open(argv[1]) or Ed.new()
  e:enter()

  -- automatic LSP: silently enable when a server exists for the file
  e:lsp_start(true)

  -- Catch exit signals (raw mode: no signals, but just in case)
  local ok, err = pcall(function()
    e:render(true)              -- initial frame (also lets LSP post_render kick off)
    while not e.done do
      local key = e:getkey(100) -- idle slice for LSP ticks
      if key then
        e:dispatch(key)
        e.repaint = true -- always repaint after a key (cursor moves, edits, etc)
      end
      e:tick()
      e:render()
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

return Ed
