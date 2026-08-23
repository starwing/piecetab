-- editor.lua class skeleton tests (luaunit harness).
-- run: just lua/ed
-- Requires the lua-utf8 rock (same cpath as editor.lua itself).
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = root .. "/?.lua;" .. dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/lua/luajit/?.so;"
      or root .. "/lua/?.so;")
    .. package.cpath
    .. ";./lua/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"

local lu = require "luaunit"
local Ed = require "editor"

-- tree-sitter is optional: editor.lua disables highlighting when the binding
-- (or the compiled grammar) is absent. Keep these tests green after a clean
-- build by skipping only the syntax-highlighting cases.
local has_treesitter = pcall(function()
  local ts = require "treesitter"
  ts.require("c")
end)

local function skip_without_treesitter()
  lu.skipIf(not has_treesitter, "tree-sitter not available")
end

local ROWS, COLS = 6, 40

local function make_ed(content)
  local term = {
    s = "",
    write = function(t, x) t.s = t.s .. x end,
    flush = function() end,
    move = function(t, row, col)
      t.s = t.s .. string.format("\27[%d;%dH", row, col)
    end,
    size = function() return ROWS, COLS end
  }
  local e = Ed.new(content, term)
  e.log = function() end
  return e
end

local LINES = 20 -- document lines for scroll tests

-- Compare a cell style handle against an attr table at key level: tree
-- fill segments carry merged (arbiter-composed) ids, so a plain intern
-- handle never equals them — assert the folded attr instead.
local function assert_style(e, st, attr)
  local a = e.comp:attr(st)
  lu.assertNotIsNil(a, "style id " .. tostring(st) .. " has no attr")
  assert(a)
  for k, v in pairs(attr) do
    lu.assertEquals(a[k], v, "style key " .. tostring(k))
  end
end

-- Opposite: every set key in attr must be absent/different on the cell.
local function assert_not_style(e, st, attr)
  local a = e.comp:attr(st)
  for k, v in pairs(attr) do
    lu.assertNotEquals(a and a[k], v, "style key " .. tostring(k))
  end
end

local function make_doc()
  local t = {}
  for i = 1, LINES do t[i] = "line " .. i end
  return table.concat(t, "\n") .. "\n"
end

-- Render one frame, return captured byte stream (accumulates in e.term.s).
local function frame(e)
  e:render()
  return e.term.s
end

-- Drive one key: dispatch, then render frame, return frame bytes.
local function keystroke(e, k)
  e:dispatch(k)
  return frame(e)
end

TestSkeleton = {}

function TestSkeleton:testEscTimeoutFinite()
  -- bare ESC must return after esc_timeout (regression: waitkey(-1)
  -- blocks forever after ESC in real tty, breaking :mode exit).
  -- Real-tty behavior verified manually; here we lock the config.
  local e = Ed.new()
  lu.assertIsTrue(e.esc_timeout > 0 and e.esc_timeout <= 1000)
  local e2 = Ed.new(nil, {
    esc_timeout = 7,
    write = function() end,
    flush = function() end,
    size = function() return 24, 80 end
  })
  lu.assertEquals(e2.esc_timeout, 7)
end

function TestSkeleton:testConstruct()
  local e = make_ed("a\nb")
  lu.assertEquals(e.mode, "NORMAL")
  lu.assertNotIsNil(e.doc)
  lu.assertEquals(e.doc:breaks(), 2)
end

function TestSkeleton:testNewEmptyDoc()
  local e = make_ed()
  lu.assertEquals(e.doc:breaks(), 0) -- empty doc: 0 lines (see piecetab_test testLineCountEmpty)
end

function TestSkeleton:testUndoSwitchSyncsLsp()
  -- u gathers the change hunks and pushes them to the LSP as sequential
  -- edits (incremental sync); <C-r> does the same for redo
  local e = make_ed("hello\n")
  local switch_edits
  --- @type any
  e.lsp = {
    on_edit = function() end,
    undo_switch = function(_, doc_undo)
      local changes = {}
      doc_undo(function(off, del, text)
        changes[#changes + 1] = { off = off, del = del, text = text }
      end)
      switch_edits = changes
    end,
  }
  e.doc:commit()
  e.doc:seek("set", 0)
  e:doc_edit(0, "X")
  e.doc:commit()
  e:dispatch("u")
  lu.assertEquals(switch_edits[1], { off = 0, del = 1, text = "" })
  e:dispatch("<C-r>")
  lu.assertEquals(switch_edits[1], { off = 0, del = 0, text = "X" })
end

function TestSkeleton:testOpenReadsFile()
  local path = os.tmpname()
  local f = assert(io.open(path, "w"))
  f:write("x\ny\n")
  f:close()
  local e = Ed.open(path)
  e.log = function() end
  lu.assertEquals(e.doc:breaks(), 2)
  os.remove(path)
end

function TestSkeleton:testTermWriteNoCrash()
  -- default Term.new() must not crash: out defaults to io wrapped
  -- method-style (io.write(io, s) would error before the fix)
  local e = Ed.new() -- real Term.new() path
  e.log = function() end
  e.term:write("")
end

function TestSkeleton:testKeymapRegisterAndDispatch()
  local e = make_ed("")
  local called = 0
  e:keymap("normal", "K", function(self) called = called + 1 end)
  e:dispatch("K")
  lu.assertEquals(called, 1)
  e:dispatch("X") -- unbound: no-op
  lu.assertEquals(called, 1)
end

function TestSkeleton:testCommandRegister()
  local e = make_ed("")
  local got
  e:command("w", function(self, arg, bang) got = { arg, bang } end)
  e:dispatch(":")
  e:dispatch("w")
  e:dispatch(" ")
  e:dispatch("f")
  e:dispatch("i")
  e:dispatch("l")
  e:dispatch("e")
  e:dispatch("<Enter>")
  lu.assertNotIsNil(got)
end

function TestSkeleton:testQuitSetsDone()
  local e = make_ed("")
  e:quit()
  lu.assertIsTrue(e.done)
end

function TestSkeleton:testKeymapReturnsSelf()
  -- keymap/command return self for chaining
  local e = make_ed("")
  lu.assertEquals(e:keymap("normal", "q", function() end), e)
  lu.assertEquals(e:command("qq", function() end), e)
end

function TestSkeleton:testDispatchAfterQuit()
  -- dispatch is a single-key processor; done is consulted by the main loop
  -- only, so a stray dispatch after quit must not crash and must leave
  -- done set. Documented: the key handler still runs (x below deletes 'a').
  local e = make_ed("ab\n")
  e:quit()
  e:dispatch("x")
  lu.assertIsTrue(e.done)
end

TestNormal = {}

function TestNormal:setUp()
  self.e = make_ed("line one\nline two\nline three\n")
end

function TestNormal:testHAndL()
  self.e:dispatch("l")
  lu.assertEquals(self.e.doc:column(), 1)
  self.e:dispatch("h")
  lu.assertEquals(self.e.doc:column(), 0)
end

function TestNormal:testJAndK()
  self.e:dispatch("j")
  lu.assertEquals(self.e:text_line(), 1)
  self.e:dispatch("k")
  lu.assertEquals(self.e:text_line(), 0)
end

function TestNormal:testWordMotions()
  self.e:dispatch("w")
  lu.assertEquals(self.e.doc:column(), 5) -- skips "line"+space, at 'o'
  self.e:dispatch("b")
  lu.assertEquals(self.e.doc:column(), 0)
end

function TestNormal:testZeroAndDollar()
  self.e:dispatch("$")
  lu.assertEquals(self.e.doc:column(), 7) -- on 'e', last char of "line one"
  self.e:dispatch("0")
  lu.assertEquals(self.e.doc:column(), 0)
end

function TestNormal:testDollarOnUtf8Line()
  local e = make_ed("你好\n")
  e:dispatch("$")
  lu.assertEquals(e.doc:column(), 3) -- start of "好"
end

function TestNormal:testDollarXKeepsLine()
  self.e:dispatch("$")
  self.e:dispatch("x") -- deletes 'e', not the newline
  lu.assertEquals(self.e.doc:breaks(), 3)
end

function TestNormal:testDollarAInsertsAtEnd()
  self.e:dispatch("$")
  self.e:dispatch("a") -- insert after last char
  self.e:dispatch("X")
  self.e:dispatch("<Escape>")
  self.e.doc:seek("set", 0)
  lu.assertEquals(self.e.doc:read("*a"), "line oneX\nline two\nline three\n")
end

function TestNormal:testGgG()
  self.e:dispatch("G")
  lu.assertEquals(self.e.doc:line(), 2)
  self.e:dispatch("gg")
  lu.assertEquals(self.e.doc:line(), 0)
end

function TestNormal:testXDeletesChar()
  self.e:dispatch("x")
  lu.assertEquals(self.e.doc:read("l"):sub(1, 1), "i")
end

function TestNormal:testDdDeletesLine()
  self.e:dispatch("dd")
  lu.assertEquals(self.e.doc:breaks(), 2)
end

function TestNormal:testPendingGeneric()
  -- custom combo "zz" verifies generic pending (not builtin-dependent)
  local e2 = make_ed("")
  local hit = 0
  e2:keymap("normal", "zz", function() hit = hit + 1 end)
  e2:dispatch("z")
  lu.assertEquals(hit, 0) -- waits for second key
  e2:dispatch("z")
  lu.assertEquals(hit, 1)
end

function TestNormal:testPendingMissFallsThrough()
  self.e:dispatch("g")
  self.e:dispatch("x") -- gx unbound -> x acts
  lu.assertEquals(self.e.doc:read("l"):sub(1, 1), "i")
end

function TestNormal:testMsgClearedOnNormalKey()
  self.e.msg = "written"
  self.e:dispatch("j")
  lu.assertEquals(self.e.msg, "")
end

function TestNormal:testUndoRedo()
  self.e:dispatch("i")
  self.e:dispatch("X")
  self.e:dispatch("<Escape>")
  self.e:dispatch("u")
  self.e.doc:seek("set", 0) -- cursor home; assert content, not cursor rest
  lu.assertNotStrContains(self.e.doc:read("l"), "X")
  self.e:dispatch("<C-r>")
  self.e.doc:seek("set", 0)
  lu.assertStrContains(self.e.doc:read("l"), "X")
end

function TestNormal:testOOpensLineBelow()
  self.e:dispatch("o")
  lu.assertEquals(self.e.mode, "INSERT")
  lu.assertEquals(self.e.doc:breaks(), 4)
  lu.assertEquals(self.e.doc:line(), 1) -- new empty line below, cursor at its start
  lu.assertEquals(self.e.doc:column(), 0)
end

function TestNormal:testOOpensLineAbove()
  self.e:dispatch("O")
  lu.assertEquals(self.e.mode, "INSERT")
  lu.assertEquals(self.e.doc:breaks(), 4)
  lu.assertEquals(self.e.doc:line(), 0) -- new empty line above, cursor at its start
  lu.assertEquals(self.e.doc:column(), 0)
end

TestInsert = {}

function TestInsert:setUp()
  self.e = make_ed("ab\n")
end

local function esc(e) e:dispatch("<Escape>") end

function TestInsert:testTypeText()
  self.e:dispatch("a") -- a: cursor right one char, enter insert
  self.e:dispatch("X")
  self.e:dispatch("Y")
  esc(self.e)
  self.e.doc:seek("set", 0)
  lu.assertEquals(self.e.doc:read("l"), "aXYb") -- brief said "abXY": 'a' appends after cursor (vim semantics), corrected
  lu.assertEquals(self.e.mode, "NORMAL")
end

function TestInsert:testOThenTextEscCursorOnLastChar()
  -- regression: o + "abcdef" + ESC must land on 'f' (L2,6 display).
  -- Root cause is in pt (commit after partial linecache sync corrupts
  -- the line view); see piecetab_test testCommitAfterPartialSync.
  local e = make_ed("#!/usr/bin/env lua\nx\n")
  e:dispatch("o")
  for c in ("abcdef"):gmatch(".") do e:dispatch(c) end
  e:dispatch("<Escape>")
  lu.assertEquals(e.doc:line(), 1)
  lu.assertEquals(e.doc:column(), 5)
end

function TestInsert:testBackspace()
  self.e:dispatch("i")
  self.e:dispatch("X")
  self.e:dispatch("<Backspace>")
  esc(self.e)
  lu.assertEquals(self.e.doc:read("l"), "ab")
end

function TestInsert:testBackspaceUtf8()
  local e = make_ed("你好\n")
  e.doc:seek("set", 6)      -- end of line, after the 3-byte "好"
  e:dispatch("i")
  e:dispatch("<Backspace>") -- deletes whole "好" (3 bytes), cursor trails to its start
  e:dispatch("<Escape>")
  e.doc:seek("set", 0)
  lu.assertEquals(e.doc:read("*a"), "你\n")
end

function TestInsert:testEnterSplitsLine()
  self.e:dispatch("i")
  self.e:dispatch("<Enter>")
  esc(self.e)
  lu.assertEquals(self.e.doc:breaks(), 2)
end

function TestInsert:testEscapeMovesLeft()
  self.e:dispatch("i")
  self.e:dispatch("<Escape>")
  lu.assertEquals(self.e.doc:column(), 0) -- ESC moves cursor one char left
end

function TestInsert:testEscapeAtLineStartStays()
  local e = make_ed("ab\ncd\n")
  e.doc:seek("line", 1) -- start of "cd"
  e:dispatch("i")
  e:dispatch("<Escape>")
  lu.assertEquals(e.doc:line(), 1) -- vim: line start ESC stays, no wrap
  lu.assertEquals(e.doc:column(), 0)
end

function TestInsert:testEscapeAtEofMovesLeft()
  local e = make_ed("ab") -- no trailing newline
  e.doc:seek("set", 2)    -- EOF
  e:dispatch("i")
  e:dispatch("X")
  e:dispatch("<Escape>")
  lu.assertEquals(e.doc:offset(), 2) -- back before the X
end

function TestInsert:testControlKeyFiltered()
  self.e:dispatch("i")
  self.e:dispatch("<F5>") -- unbound control key -> not inserted
  esc(self.e)
  lu.assertEquals(self.e.doc:read("l"), "ab")
end

TestCommand = {}

function TestCommand:setUp()
  self.e = make_ed("ab\n")
end

function TestCommand:testColonEntersCommandMode()
  self.e:dispatch(":")
  lu.assertEquals(self.e.mode, "COMMAND")
end

function TestCommand:testTypingAppendsCmdline()
  self.e:dispatch(":")
  self.e:dispatch("w")
  self.e:dispatch("q")
  lu.assertEquals(self.e.cmdline, "wq")
end

function TestCommand:testEscapeAborts()
  self.e:dispatch(":")
  self.e:dispatch("abc")
  self.e:dispatch("<Escape>")
  lu.assertEquals(self.e.mode, "NORMAL")
  lu.assertEquals(self.e.cmdline, "")
end

function TestCommand:testBackspace()
  self.e:dispatch(":")
  self.e:dispatch("wq")
  self.e:dispatch("<Backspace>")
  lu.assertEquals(self.e.cmdline, "w")
end

function TestCommand:testUnknownCommand()
  self.e:dispatch(":")
  self.e:dispatch("zz")
  self.e:dispatch("<Enter>")
  lu.assertEquals(self.e.mode, "NORMAL")
  lu.assertStrContains(self.e.msg, "Unknown")
end

function TestCommand:testBangParsed() -- regression: q! dead-code branch
  local e2 = make_ed("")
  local got
  e2:command("q", function(self, arg, bang) got = bang end)
  e2:dispatch(":")
  e2:dispatch("q")
  e2:dispatch("!")
  e2:dispatch("<Enter>")
  lu.assertIsTrue(got)
end

function TestCommand:testSaveWritesFile()
  local path = os.tmpname()
  local f = assert(io.open(path, "w")); f:write("ab\n"); f:close()
  local e = Ed.open(path)
  e.log = function() end
  e:dispatch("i")
  e:dispatch("X")
  e:dispatch("<Escape>")
  e:dispatch(":")
  e:dispatch("w")
  e:dispatch("<Enter>")
  local out = assert(io.open(path, "r"))
  lu.assertEquals(out:read("*a"), "Xab\n")
  out:close()
  os.remove(path)
end

function TestCommand:testQuitSetsDone()
  local e = make_ed("")
  e:dispatch(":")
  e:dispatch("q")
  e:dispatch("<Enter>")
  lu.assertIsTrue(e.done)
end

function TestCommand:testWqSavesAndQuits()
  local path = os.tmpname()
  local f = assert(io.open(path, "w")); f:write("ab\n"); f:close()
  local e = Ed.open(path)
  e.log = function() end
  e:dispatch("i")
  e:dispatch("X")
  e:dispatch("<Escape>")
  e:dispatch(":")
  e:dispatch("wq")
  e:dispatch("<Enter>")
  lu.assertIsTrue(e.done)
  local out = assert(io.open(path, "r"))
  lu.assertEquals(out:read("*a"), "Xab\n")
  out:close()
  os.remove(path)
end

function TestCommand:testEreloadsFile()
  local path = os.tmpname()
  local f = assert(io.open(path, "w")); f:write("one\n"); f:close()
  local e = Ed.open(path)
  e.log = function() end
  f = assert(io.open(path, "w")); f:write("two\nthree\n"); f:close()
  e:dispatch(":")
  e:dispatch("e ")
  e:dispatch(path)
  e:dispatch("<Enter>")
  lu.assertEquals(e.filename, path)
  lu.assertEquals(e.doc:breaks(), 2)
  e.doc:seek("line", 0)
  lu.assertEquals(e.doc:read("l"), "two")
  os.remove(path)
end

TestUtf8 = {}

function TestUtf8:setUp()
  self.e = make_ed("你好\nworld\n")
end

function TestUtf8:testHLeftAcrossUtf8()
  self.e.doc:seek("set", 6)               -- end of "你好" line (byte 6)
  self.e:dispatch("h")
  lu.assertEquals(self.e.doc:column(), 3) -- start of "好"
  self.e:dispatch("h")
  lu.assertEquals(self.e.doc:column(), 0)
end

function TestUtf8:testLRightAcrossUtf8()
  self.e.doc:seek("set", 0)
  self.e:dispatch("l")
  lu.assertEquals(self.e.doc:column(), 3) -- start of "好"
  self.e:dispatch("l")
  lu.assertEquals(self.e.doc:column(), 6) -- end of line
end

function TestUtf8:testDeleteUtf8()
  self.e.doc:seek("set", 3) -- at "好"
  self.e:dispatch("i")
  self.e:dispatch("<Delete>")
  self.e:dispatch("<Escape>")
  self.e.doc:seek("set", 0)
  lu.assertEquals(self.e.doc:read("*a"), "你\nworld\n")
end

function TestUtf8:testDeleteLastChar()
  -- cursor at "好" (last char of buffer): utf8.next returns nil past it
  local e = make_ed("你好")
  e.doc:seek("set", 3)
  e:dispatch("i")
  e:dispatch("<Delete>")
  e:dispatch("<Escape>")
  e.doc:seek("set", 0)
  lu.assertEquals(e.doc:read("*a"), "你")
end

function TestUtf8:testJKeepsDisplayCol()
  -- "你好" is 4 display columns wide; line end at byte col 6; j to "world"
  -- keeps display col 4 -> byte col 4
  self.e.doc:seek("set", 6)
  self.e.cursor_col = 4 -- display col for the "你好" end
  self.e:dispatch("j")
  lu.assertEquals(self.e:text_col(), 4)
  self.e:dispatch("k")
  lu.assertEquals(self.e:text_col(), 6)
end

function TestUtf8:testHLongLineWindow()
  -- 100 ASCII + CJK: left-move window [off-4, off+1) must land exactly
  local line = string.rep("a", 100) .. "你好b"
  local e = make_ed(line .. "\n")
  e.doc:seek("set", #line)
  e:dispatch("h")
  lu.assertEquals(e.doc:column(), #line - 1) -- at 'b'
  e:dispatch("h")
  lu.assertEquals(e.doc:column(), #line - 4) -- start of "好"
  e:dispatch("h")
  lu.assertEquals(e.doc:column(), #line - 7) -- start of "你"
  e:dispatch("h")
  lu.assertEquals(e.doc:column(), #line - 8) -- last 'a'
end

-- ======== Scroll regression: viewport down must emit SU, not SD ========
-- (grid diff emits region + SU/SD; captured via accumulating e.term.s)

TestScroll = {}

function TestScroll:setUp()
  self.e = make_ed(make_doc())
end

function TestScroll:testDownScrollEmitsSu()
  -- j to the last visible row (row 4 of 5), then one more j scrolls
  for _ = 1, 4 do
    keystroke(self.e, "j")
  end
  local s = keystroke(self.e, "j")      -- 5th j: scroll_line 0 -> 1
  lu.assertStrContains(s, "\27[1;5r")   -- scroll region rows 1..5
  lu.assertStrContains(s, "\27[1S")     -- SU 1: content scrolls UP
  lu.assertNotStrContains(s, "\27%[1T") -- no SD
  lu.assertEquals(self.e.scroll_line, 1)
end

function TestScroll:testDownScrollMany()
  for _ = 1, 4 do
    keystroke(self.e, "j")
  end
  for i = 1, 10 do
    local s = keystroke(self.e, "j")
    lu.assertNotStrContains(s, "\27%[1T", "frame " .. i)
    lu.assertEquals(self.e.scroll_line, i)
  end
  lu.assertEquals(self.e.scroll_line, 10)
end

function TestScroll:testUpScrollEmitsSd()
  for _ = 1, 14 do -- deep scroll down (viewport 10..14)
    keystroke(self.e, "j")
  end
  self.e.term.s = "" -- observe k-phase only (earlier frames hold SU)
  local s
  for _ = 1, 20 do
    s = keystroke(self.e, "k")
    if self.e.scroll_line < 10 then break end
  end
  lu.assertStrContains(s, "\27[1;5r")
  lu.assertStrContains(s, "\27[1T")     -- SD 1: content scrolls DOWN
  lu.assertNotStrContains(s, "\27%[1S") -- no SU
  lu.assertEquals(self.e.scroll_line, 9)
end

function TestScroll:testScrollThenIdleFrame()
  -- after a scroll frame, an unchanged frame must not redraw grid
  -- content: the ring offset must persist across delta=0 frames
  for _ = 1, 6 do keystroke(self.e, "j") end -- cursor 5 -> scroll_line 1
  self.e.term.s = ""
  frame(self.e)
  -- grid diff must be empty: only the ?25l marker, the diff finish
  -- reset, and the status bar (row 6) may be written
  lu.assertStrContains(self.e.term.s, "\27[?25l\27[0m\27[6;1H", false)
end

-- ======== Status bar ========

TestOps = {}

TestStyleTable = {}

function TestStyleTable:testFullStateReset()
  -- SGR codes are cumulative: each style CSI must fully reset prior
  -- state, or a DIM->color transition (line number -> keyword) leaks
  -- the DIM attribute onto the colored cell
  local e = make_ed("")
  local attrs = { { fg = 207 }, { bg = 237, dim = true },
    { bold = true, underline = true } }
  for i, a in ipairs(attrs) do
    lu.assertStrContains(e:csi(e.comp:intern(a)), "\27[0m", false, "attr " .. i)
  end
end

-- ======== Syntax highlighting ========
TestSyntax = {}

function TestSyntax:testKeywordC()
  skip_without_treesitter()
  -- "int main(void) { return 0; }": i@0, main@4, return@15
  local e = make_ed("int main(void) { return 0; }\n")
  e:open_language("c")
  frame(e)
  local _, st = e.grid:cell(0, 4)  -- content col 0 ('i' of int)
  assert_style(e, st, Ed.ATTR_KEYWORD)
  local _, st2 = e.grid:cell(0, 8) -- content col 4 ('m' of main)
  assert_not_style(e, st2, Ed.ATTR_KEYWORD)
  assert_style(e, st2, Ed.ATTR_FUNCTION)
  local _, st3 = e.grid:cell(0, 21) -- content col 17 ('r' of return)
  assert_style(e, st3, Ed.ATTR_KEYWORD)
end

function TestSyntax:testCommentStringC()
  skip_without_treesitter()
  -- "int x; /* note */ char *s = \"hi\";": /@7, quote@29
  local e = make_ed("int x; /* note */ char *s = \"hi\";\n")
  e:open_language("c")
  frame(e)
  local _, st = e.grid:cell(0, 11)  -- content col 7 ('/' of comment)
  assert_style(e, st, Ed.ATTR_COMMENT)
  local _, st2 = e.grid:cell(0, 32) -- content col 28 ('"' of string)
  assert_style(e, st2, Ed.ATTR_STRING)
end

function TestSyntax:testEditUpdatesHighlight()
  skip_without_treesitter()
  local e = make_ed("int main(void) { return 0; }\n")
  e:open_language("c")
  frame(e)
  e.doc:seek("set", 17) -- 'r' of "return"
  e:doc_edit(1, "")

  frame(e)
  local _, st = e.grid:cell(0, 21) -- content col 17 ("eturn" start)
  assert_not_style(e, st, Ed.ATTR_KEYWORD)
end

function TestSyntax:testNoLanguageNoHighlight()
  local e = make_ed("int main(void) { return 0; }\n")
  frame(e)
  local _, st = e.grid:cell(0, 4) -- content col 0
  lu.assertEquals(st, 0)          -- default handle
end

TestSc = {}

function TestSc:testInternReuse()
  local s = Ed.newcompositor()
  lu.assertEquals(s:intern({ fg = 207 }), s:intern({ fg = 207 }))
  lu.assertEquals(s:intern({}), 0) -- default attr pre-interned as style 0
end

function TestSc:testInternDistinct()
  local s = Ed.newcompositor()
  lu.assertNotEquals(s:intern({ fg = 1 }), s:intern({ bg = 1 }))
  lu.assertNotEquals(s:intern({ fg = 1 }), s:intern({ fg = 2 }))
  lu.assertNotEquals(s:intern({ bold = true }), s:intern({ underline = true }))
end

function TestSc:testInternFieldOrderIrrelevant()
  local s = Ed.newcompositor()
  lu.assertEquals(s:intern({ fg = 1, bold = true }),
    s:intern({ bold = true, fg = 1 }))
end

function TestSc:testInternUnsetSkipped()
  local s = Ed.newcompositor()
  lu.assertEquals(s:intern({ bold = true }),
    s:intern({ bold = true, dim = nil }))
  lu.assertNotEquals(s:intern({ bold = true }),
    s:intern({ bold = true, underline = false }))
end

function TestSc:testInverseLookup()
  local s = Ed.newcompositor()
  local id = s:intern({ fg = 207, bg = "#010203", bold = true })
  local a = assert(s:attr(id))
  lu.assertEquals(a.fg, 207)
  lu.assertEquals(a.bg, "#010203")
  lu.assertIsTrue(a.bold)
  lu.assertIsNil(s:attr(id + 999))
end

function TestSc:testCsiGeneration()
  -- csi lives on Ed (the spantree tree has zero format knowledge)
  local e = make_ed("")
  lu.assertEquals(e:csi(0), "\27[0m")
  lu.assertEquals(e:csi(e.comp:intern({})), "\27[0m")
  lu.assertEquals(e:csi(e.comp:intern({ fg = 207 })), "\27[0m\27[38;5;207m")
  lu.assertEquals(e:csi(e.comp:intern({ bold = true, bg = 237 })),
    "\27[0m\27[1;48;5;237m")
  lu.assertEquals(e:csi(e.comp:intern({ fg = "#010203" })),
    "\27[0m\27[38;2;1;2;3m")
  lu.assertIsNil(e:csi(999))
end

TestLayers = {}

-- 3 pieces: "aaaa" + "XY" + " bbbb" via two mid-doc edits
local function make_pieces(content)
  local e = make_ed(content)
  e.doc:seek("set", 4)
  e:doc_edit(0, "XY") -- split into 2 pieces
  e.doc:seek("set", 8)
  e:doc_edit(0, "Z")  -- split into 3 pieces
  e.show_pieces = true
  return e
end

-- "aaaaXY bZbbb\n": pieces [0,4) plain, [4,6) gray, [6,8) plain,
-- [8,9) gray, [9,13) plain (inserts create hole pieces)
function TestLayers:testPiecesAlternate()
  local e = make_pieces("aaaa bbbb\n")
  frame(e)
  local _, st = e.grid:cell(0, 4)   -- 'a' (piece 1, plain)
  lu.assertEquals(st, 0)
  local _, st2 = e.grid:cell(0, 8)  -- 'X' (piece 2, gray)
  assert_style(e, st2, Ed.ATTR_GRAY_BG)
  local _, st3 = e.grid:cell(0, 12) -- 'Z' (piece 4, gray)
  assert_style(e, st3, Ed.ATTR_GRAY_BG)
  local _, st4 = e.grid:cell(0, 13) -- 'b' (piece 5, plain)
  lu.assertEquals(st4, 0)
end

function TestLayers:testPiecesToggleCommand()
  local e = make_pieces("aaaa bbbb\n")
  lu.assertIsTrue(e.show_pieces)
  e:dispatch(":") -- not needed; command path below
  e.commands.pieces(e, "", false)
  lu.assertIsFalse(e.show_pieces)
end

function TestLayers:testPieceGrayOnlyOnEvenPieces()
  -- odd piece must NOT carry gray (piece 1 = plain)
  local e = make_pieces("aaaa bbbb\n")
  frame(e)
  local _, st = e.grid:cell(0, 9)  -- 'Y' (piece 2, gray)
  assert_style(e, st, Ed.ATTR_GRAY_BG)
  local _, st2 = e.grid:cell(0, 4) -- 'a' (piece 1, plain)
  lu.assertEquals(st2, 0)
end

-- "int Qx\n": pieces [0,4) "int " plain, [4,5) "Q" gray, [5,7) "x\n" plain
function TestLayers:testLayeredCompose()
  skip_without_treesitter()
  -- syntax fg on plain piece; piece bg alone on gray piece
  local e = make_ed("int x\n")
  e:open_language("c")
  e.doc:seek("set", 4)
  e:doc_edit(0, "Q")
  e.show_pieces = true
  frame(e)
  local bg = e.comp:intern(Ed.ATTR_GRAY_BG)
  local _, st = e.grid:cell(0, 4)  -- 'i' (piece 1, plain): keyword only
  assert_style(e, st, Ed.ATTR_KEYWORD)
  local _, st2 = e.grid:cell(0, 8) -- 'Q' (piece 2, gray): no syntax
  assert_style(e, st2, Ed.ATTR_GRAY_BG)
  local _, st3 = e.grid:cell(0, 9) -- 'x' (piece 3, plain): nothing
  lu.assertEquals(st3, 0)
  lu.assertNotEquals(bg, 0)
end

-- "aaaXYa\nbbbb\n": pieces [0,3) plain, [3,5) gray, [5,12) plain —
-- the plain piece crosses the line break
function TestLayers:testPieceAcrossLineBoundary()
  local e = make_ed("aaaa\nbbbb\n")
  e.doc:seek("set", 3)
  e:doc_edit(0, "XY")
  e.show_pieces = true
  frame(e)
  local _, st = e.grid:cell(0, 4)  -- 'a' col 0 (piece 1, plain)
  lu.assertEquals(st, 0)
  local _, st2 = e.grid:cell(0, 8) -- 'Y' col 4 (piece 2, gray, same row)
  assert_style(e, st2, Ed.ATTR_GRAY_BG)
  local _, st3 = e.grid:cell(1, 4) -- row 1 col 0 (piece 3, plain)
  lu.assertEquals(st3, 0)
end

-- "char *s = \"XYaaaa\";\n": pieces [0,12) plain, [12,14) gray,
-- [14,20) plain; the string literal "XYaaaa" spans the piece boundary —
-- string+bg on the gray piece, fg-only on the plain pieces
function TestLayers:testMergeLayersUnsetPassesThrough()
  skip_without_treesitter()
  local e = make_ed('char *s = "aaaa";\n')
  e:open_language("c")
  e.doc:seek("set", 12) -- at the opening quote
  e:doc_edit(0, "XY")
  e.show_pieces = true
  frame(e)
  local _, st = e.grid:cell(0, 4)   -- 'c' of char (piece 1, plain): keyword
  assert_style(e, st, Ed.ATTR_KEYWORD)
  local _, st2 = e.grid:cell(0, 16) -- '"' (piece 2, gray): string + gray
  lu.assertEquals(st2, e.comp:intern({ fg = 114, bg = 237 }))
  local _, st3 = e.grid:cell(0, 18) -- 'a' (piece 3, plain): string only
  assert_style(e, st3, Ed.ATTR_STRING)
end

TestVisual = {}

function TestVisual:testEnterAndExtend()
  local e = make_ed("abcdef\n")
  e:dispatch("v")
  lu.assertEquals(e.mode, "VISUAL")
  lu.assertEquals(e.sel_start, 0)
  local s = frame(e)
  lu.assertStrContains(s, "VISUAL") -- status bar mode
  -- cursor char is inside the selection immediately (vim charwise)
  local _, st = e.grid:cell(0, 4)   -- 'a'
  assert_style(e, st, Ed.ATTR_REVERSE)
  e:dispatch("l")                   -- cursor 1, selection [0,2) = "ab"
  frame(e)
  local _, st2 = e.grid:cell(0, 5)  -- 'b'
  assert_style(e, st2, Ed.ATTR_REVERSE)
  local _, st3 = e.grid:cell(0, 6)  -- 'c' (outside selection)
  lu.assertEquals(st3, 0)
end

function TestVisual:testReverseSelectionExtendsBackward()
  -- l past sel_start then h back: selection is [min,max), anchor lost
  local e = make_ed("abcdef\n")
  e:dispatch("v")
  e:dispatch("l")
  e:dispatch("l")                  -- cursor 2, selection [0,3) = "abc"
  e:dispatch("h")                  -- cursor 1, selection [0,2) = "ab"
  frame(e)
  local _, st = e.grid:cell(0, 5)  -- 'b' (cursor char)
  assert_style(e, st, Ed.ATTR_REVERSE)
  local _, st2 = e.grid:cell(0, 6) -- 'c' (outside selection)
  lu.assertEquals(st2, 0)
end

-- '"XY' at bytes 10..16: 'X' (byte 12) carries string fg (syntax) +
-- gray bg (piece 2) + reverse (selection) in one cell — 3-layer merge
function TestVisual:testThreeLayerCompose()
  skip_without_treesitter()
  local e = make_ed('char *s = "aaaa";\n')
  e:open_language("c")
  e.doc:seek("set", 12)
  e:doc_edit(0, "XY")
  e.doc:seek("set", 12) -- on 'X' (piece 2, inside string literal)
  e:dispatch("v")
  e:dispatch("l")
  frame(e)
  local _, st = e.grid:cell(0, 16) -- content col 12
  lu.assertEquals(st, e.comp:intern({ fg = 114, reverse = true, bg = 237 }))
end

function TestVisual:testEscapeClears()
  local e = make_ed("ab\n")
  e:dispatch("v")
  e:dispatch("l")
  e:dispatch("<Escape>")
  lu.assertEquals(e.mode, "NORMAL")
  lu.assertIsNil(e.sel_start)
end

function TestVisual:testYankPaste()
  local e = make_ed("ab\n")
  e:dispatch("v")
  e:dispatch("y") -- selection = "a" (cursor char)
  lu.assertEquals(e.mode, "NORMAL")
  lu.assertEquals(e.clip, "a")
  lu.assertEquals(e.doc:offset(), 0) -- cursor back to selection start
  e:dispatch("p")
  e.doc:seek("set", 0)
  lu.assertEquals(e.doc:read("l"), "aab")
  local e2 = make_ed("ab\n")
  e2:dispatch("v")
  e2:dispatch("l") -- cursor 1, selection "ab"
  e2:dispatch("y")
  lu.assertEquals(e2.clip, "ab")
end

function TestVisual:testYankUtf8Char()
  -- cursor char selection covers the whole multibyte char, not 1 byte
  local e = make_ed("你好\n")
  e.doc:seek("set", 0)
  e:dispatch("v")
  e:dispatch("y")
  lu.assertEquals(e.clip, "你")
  e:dispatch("p")
  e.doc:seek("set", 0)
  lu.assertEquals(e.doc:read("l"), "你你好")
end

function TestVisual:testDeleteUndo()
  local e = make_ed("ab\n")
  e:dispatch("v")
  e:dispatch("l") -- selection "ab"
  e:dispatch("d")
  lu.assertEquals(e.mode, "NORMAL")
  lu.assertEquals(e.doc:offset(), 0)
  lu.assertEquals(e.doc:buffer():read(0, 1), "\n")
  e:dispatch("u")
  lu.assertEquals(e.doc:buffer():read(0, 3), "ab\n")
end

function TestVisual:testMultilineSelection()
  local e = make_ed("ab\ncd\n")
  e:dispatch("v")                  -- sel_start 0
  e:dispatch("j")                  -- cursor line 1 col 0, selection "ab\nc"
  frame(e)
  local _, st = e.grid:cell(0, 4)  -- 'a'
  assert_style(e, st, Ed.ATTR_REVERSE)
  local _, st2 = e.grid:cell(0, 5) -- 'b'
  assert_style(e, st2, Ed.ATTR_REVERSE)
  local _, st3 = e.grid:cell(1, 4) -- 'c' (cursor char, selected)
  assert_style(e, st3, Ed.ATTR_REVERSE)
  local _, st4 = e.grid:cell(1, 5) -- 'd' (outside selection)
  lu.assertEquals(st4, 0)
  e:dispatch("d")                  -- deletes "ab\nc"
  lu.assertEquals(e.doc:buffer():read(0, 2), "d\n")
end

function TestVisual:testWordMotionExtend()
  local e = make_ed("foo bar\n")
  e:dispatch("v")
  e:dispatch("w") -- cursor 4, selection "foo b"
  e:dispatch("y")
  lu.assertEquals(e.clip, "foo b")
  local e2 = make_ed("foo bar\n")
  e2:dispatch("v")
  e2:dispatch("w")
  e2:dispatch("b") -- cursor 0 = anchor, selection "f"
  e2:dispatch("y")
  lu.assertEquals(e2.clip, "f")
end

-- ======== LSP start semantics (no server process) ========

TestHint = {}

function TestHint:testSpawnFailSilentAndLoud()
  -- automatic start (silent): missing server quietly disables lsp;
  -- manual :lsp on (loud): reports the failure in msg
  local e = make_ed("x\n")
  e.filename = "foo.lua"
  lu.assertIsFalse(e:lsp_start(true, { "/nonexistent/lsp-server" }))
  lu.assertIsNil(e.lsp)
  lu.assertEquals(e.msg, "", "silent start: no message")
  local e2 = make_ed("x\n")
  e2.filename = "foo.lua"
  lu.assertIsFalse(e2:lsp_start(false, { "/nonexistent/lsp-server" }))
  lu.assertIsNil(e2.lsp)
  lu.assertStrContains(e2.msg, "exited", "loud start: reports failure")
end

function TestHint:testNoServerMsg()
  -- :lsp on for a file with no matching server
  local e = make_ed("x\n")
  e.filename = "foo.txt"
  e.commands.lsp(e, "on")
  lu.assertStrContains(e.msg, "no server")
  lu.assertIsNil(e.lsp)
end

-- vtext: injected display text on the Ed core, stored in the spantree
-- "vtext" layer (no LSP involved)
TestVtext = {}

-- Collect the tree's vtext spans for a line (offsets relative to it).
local function vtext_spans(e, line)
  local lo = e.doc:lineoffset(line)
  local ll = e.doc:linelen(line, true)
  local out = {}
  for off, len, attr in e.tree:span("vtext", lo, ll + 1) do
    out[#out + 1] = { off = off - lo, len = len, text = attr.vtext }
  end
  return out
end

function TestVtext:setUp()
  self.e = make_ed("hello\nworld\n")
  self.e:set_vtext(0, { { off = 0, text = "int:" } })
end

function TestVtext:testSetAndClear()
  lu.assertEquals(#vtext_spans(self.e, 0), 1)
  self.e:set_vtext(0, nil)
  lu.assertEquals(#vtext_spans(self.e, 0), 0)
  self.e:set_vtext(0, { { off = 1, text = "a" } })
  self.e:clear_vtexts()
  lu.assertEquals(#vtext_spans(self.e, 0), 0)
end

function TestVtext:testHintDefaultStyleHasBackground()
  frame(self.e)
  local col = math.max(3, tostring(self.e.doc:breaks()):len()) + 1
  local _, st = self.e.grid:cell(0, col)
  assert_style(self.e, st, Ed.ATTR_HINT)
end

function TestVtext:testRenderRecordsCursorCol()
  -- non-dry render_line records text→screen col for the cursor line
  self.e:set_vtext(0, { { off = 0, text = "int:" } })
  self.e.doc:seek("set", 0)
  frame(self.e)
  lu.assertEquals(self.e.cursor_col, 4)
  self.e.doc:seek("set", 1)
  frame(self.e)
  lu.assertEquals(self.e.cursor_col, 5)
end

function TestVtext:testRenderRecordsInsertGapCol()
  -- insert gap: hint-start byte maps onto the hint's first char
  self.e.doc:seek("set", 0)
  self.e.mode = "INSERT"
  frame(self.e)
  lu.assertEquals(self.e.cursor_col, 0)
end

function TestVtext:testTextColDryRunSkipsHint()
  -- dry_run via accessor: screen col inside/after a hint maps past it
  self.e:set_vtext(0, { { off = 2, text = "ZZ" } })
  self.e.cursor_row = 0
  self.e.cursor_col = 4                 -- first text cell after the hint
  self.e.text_dirty = true
  lu.assertEquals(self.e:text_col(), 2) -- byte 3 ('l'), 0-based col 2
  lu.assertEquals(self.e.text_dirty, false)
end

function TestVtext:testTextColAccessorSyncsDirty()
  -- lazy accessor contract: reading text coords materializes doc cursor
  self.e.cursor_row = 1
  self.e.cursor_col = 2
  self.e.text_dirty = true
  lu.assertEquals(self.e:text_col(), 2) -- line 1 "world", no hints
  lu.assertEquals(self.e.doc:line(), 1)
  lu.assertEquals(self.e.text_dirty, false)
end

function TestVtext:testJKKeepsScreenCol()
  -- line 0 injected, line 1 not: j keeps screen col 4 (Neovim semantics)
  self.e:set_vtext(0, { { off = 0, text = "int:" } })
  self.e.doc:seek("set", 0)
  self.e.cursor_col = 4                 -- screen col for text col 0 (hint at start)
  self.e:dispatch("j")
  lu.assertEquals(self.e:text_col(), 4) -- line 1 text col 4 = screen col 4
  self.e:dispatch("k")
  lu.assertEquals(self.e:text_col(), 0) -- back to line 0 byte 0 (screen col 4)
end

function TestVtext:testLeftRightSkipHint()
  -- hint at start: l/h move by text chars, skipping injected cells
  self.e:set_vtext(0, { { off = 0, text = "int:" } })
  self.e.doc:seek("set", 0)
  self.e.cursor_col = 4
  keystroke(self.e, "l")
  lu.assertEquals(self.e.doc:column(), 1)
  lu.assertEquals(self.e.cursor_col, 5)
  keystroke(self.e, "h")
  lu.assertEquals(self.e.doc:column(), 0)
  lu.assertEquals(self.e.cursor_col, 4)
end

function TestVtext:testWordSkipHint()
  -- w/b move by text words, skipping injected cells
  local e = make_ed("hello world\n")
  e:set_vtext(0, { { off = 0, text = "int:" } })
  e.doc:seek("set", 0)
  e.cursor_col = 4
  keystroke(e, "w")
  lu.assertEquals(e.doc:column(), 6) -- 'w'
  lu.assertEquals(e.cursor_col, 10)
  keystroke(e, "b")
  lu.assertEquals(e.doc:column(), 0)
  lu.assertEquals(e.cursor_col, 4)
end

function TestVtext:testInsertDownKeepsGapColumn()
  -- insert gap at the hint start: <Down> keeps screen col 0 (insert
  -- semantics), not the text col past the hint
  self.e.doc:seek("set", 0)
  self.e:dispatch("i")
  self.e:dispatch("<Down>")
  lu.assertEquals(self.e.doc:column(), 0) -- line 1 byte 0 = screen col 0
end

function TestVtext:testEditShiftsHints()
  -- edits through the docedit funnel splice the tree: the hint stays
  -- bound to its char and shifts with it
  self.e.doc:seek("set", 0)
  self.e:doc_edit(0, "x") -- insert before 'h': hint stays on 'h'
  lu.assertEquals(vtext_spans(self.e, 0)[1].off, 1)
  self.e.doc:seek("set", 0)
  self.e:doc_edit(1, "") -- delete 'x' back
  lu.assertEquals(vtext_spans(self.e, 0)[1].off, 0)
end

function TestVtext:testDeleteBoundCharDrops()
  -- deleting the bound char removes the hint (the segment dies with it)
  self.e.doc:seek("set", 0)
  self.e:doc_edit(1, "") -- delete 'h', range [0,1) covers the bound char
  lu.assertEquals(#vtext_spans(self.e, 0), 0)
end

function TestVtext:testCrossLineEditShifts()
  -- a cross-line edit splices the tree: the vtext layer survives (the
  -- old implementation cleared every slot; the tree shifts segments)
  self.e.doc:seek("set", 0)
  self.e:doc_edit(0, "a\nb")
  local spans = vtext_spans(self.e, 1)
  lu.assertEquals(#spans, 1)
  lu.assertEquals(spans[1].off, 1) -- still bound to 'h' on line 1
end

function TestVtext:testUndoShifts()
  -- undo splices the change hunks into the tree: the hint shifts back
  self.e.doc:seek("set", 0)
  self.e:doc_edit(0, "x")
  self.e.doc:commit()
  self.e:dispatch("u")
  lu.assertEquals(vtext_spans(self.e, 0)[1].off, 0)
end

function TestVtext:testRenderSyncsDocAfterJK()
  -- a rendered j must leave doc/cursor_row on the same line (no stale
  -- sync from a non-cursor line during render)
  local e = make_ed("one\ntwo\nthree\n")
  keystroke(e, "j")
  lu.assertEquals(e.doc:line(), 1)
  lu.assertEquals(e.cursor_row, 1)
  lu.assertIsFalse(e.text_dirty)
end

function TestVtext:testTabHintMapping()
  -- tab expansion uses the screen column base; hint after a tab maps
  -- dry_run inside the hint to the anchor byte
  local e = make_ed("a\tb\n")
  e:set_vtext(0, { { off = 2, text = "HH" } })
  e.doc:seek("set", 2)
  e.cursor_col = 6 -- screen col of 'b' (after HH)
  frame(e)
  lu.assertEquals(e.cursor_col, 6)
  e.cursor_row = 0
  e.cursor_col = 5 -- inside HH
  e.text_dirty = true
  lu.assertEquals(e:text_col(), 2)
  lu.assertEquals(e.cursor_col, 6)
end

function TestVtext:testMultipleHints()
  -- multiple hints on one line: each stays bound to its anchor and the
  -- cursor skips every hint
  local e = make_ed("abcdef\n")
  e:set_vtext(0, { { off = 1, text = "AA" }, { off = 4, text = "BB" } })
  e.doc:seek("set", 1)
  e.cursor_col = 3 -- screen col of 'b' (after AA)
  frame(e)
  lu.assertEquals(e.cursor_col, 3)
  e.doc:seek("set", 4)
  e.cursor_col = 8 -- screen col of 'e' (after BB)
  frame(e)
  lu.assertEquals(e.cursor_col, 8)
  -- dry run inside the first hint maps to its anchor 'b'
  e.cursor_row = 0
  e.cursor_col = 2
  e.text_dirty = true
  lu.assertEquals(e:text_col(), 1)
  lu.assertEquals(e.cursor_col, 3)
end

function TestVtext:testEolHintCursorBeforeHint()
  -- Neovim model: a hint attached to the newline keeps the EOL cursor on
  -- the last character (before the hint), not after it.
  local e = make_ed("hello\nworld\n")
  e:set_vtext(0, { { off = 5, text = "<EOL>" } })
  e.doc:seek("set", 5) -- EOL
  e.cursor_col = 10
  frame(e)
  lu.assertEquals(e.cursor_col, 5) -- before <EOL>
  -- dry run inside/after the hint also clamps to EOL and moves to before it
  for _, c in ipairs({ 5, 7, 10, 12 }) do
    e.cursor_row = 0
    e.cursor_col = c
    e.text_dirty = true
    lu.assertEquals(e:text_col(), 5)
    lu.assertEquals(e.cursor_col, 5)
  end
end

function TestVtext:testEolHintInsertBeforeHint()
  -- Neovim model: pressing a at the last char inserts before the EOL
  -- hint; the hint stays attached to the (shifted) newline.
  local e = make_ed("hello\nworld\n")
  e:set_vtext(0, { { off = 5, text = "<EOL>" } })
  e.doc:seek("set", 4) -- last char 'o'
  e:dispatch("a")
  e:dispatch("X")
  e:dispatch("<Escape>")
  e.doc:seek("set", 0)
  lu.assertEquals(e.doc:read("*a"), "helloX\nworld\n")
  lu.assertEquals(vtext_spans(e, 0)[1].off, 6) -- still on the newline
end

-- sem/diag full-snapshot layers: rendered through the tree's styled
-- stream (priority fold: sem 2 < diag 3, severity is csi-invisible)
TestTreeLayers = {}

function TestTreeLayers:testSemDiagStyled()
  local e = make_ed("int x;\n")
  e:set_sem({ { offset = 0, length = 3, attr = { fg = 207 } } })
  e:set_diag({ {
    offset = 1,
    length = 2,
    attr = { underline = true, severity = 1 }
  } })
  e:render()
  local _, st = e.grid:cell(0, 4)  -- content col 0 ('i'): sem only
  assert_style(e, st, { fg = 207 })
  local _, st2 = e.grid:cell(0, 5) -- content col 1 ('n'): both fold
  assert_style(e, st2, { fg = 207, underline = true })
end

function TestTreeLayers:testSemEditShift()
  -- a splice shifts the sem layer (async gap coverage until refetch)
  local e = make_ed("int x;\n")
  e:set_sem({ { offset = 0, length = 3, attr = { fg = 207 } } })
  e.doc:seek("set", 0)
  e:doc_edit(0, "ab")
  local spans = {}
  for off, len, _, id in e.tree:styled(0, e.tree:bytes()) do
    spans[#spans + 1] = { off, len, id }
  end
  lu.assertEquals(spans[1][1], 0) -- id0 first
  lu.assertEquals(spans[2][1], 2) -- sem segment shifted to byte 2
  lu.assertEquals(spans[2][2], 3)
end

-- goal column (Neovim curswant): j/k keep the theoretical screen column;
-- a short/empty line clamps the cursor, the goal survives and re-lands
-- once a long enough line is reached again
TestGoal = {}

local function goal_ed()
  return make_ed("long line here\nworld\nlong line here\n")
end

function TestGoal:testJKKeepsGoalAcrossShortLine()
  local e = goal_ed()
  e.doc:seek("set", 0)
  e.doc:seek("cur", 10)
  e.cursor_col = 10
  e:dispatch("j")
  lu.assertEquals(e:text_col(), 5)  -- "world" is short: clamp to its end
  e:dispatch("j")
  lu.assertEquals(e:text_col(), 10) -- goal restored on the long line
end

function TestGoal:testJKKeepsGoalAcrossEmptyLine()
  local e = make_ed("long line here\n\nlong line here\n")
  e.doc:seek("set", 0)
  e.doc:seek("cur", 10)
  e.cursor_col = 10
  e:dispatch("j")
  lu.assertEquals(e:text_col(), 0)  -- empty line: clamp to line start
  e:dispatch("j")
  lu.assertEquals(e:text_col(), 10) -- goal restored
end

function TestGoal:testHorizontalMotionResetsGoal()
  -- h re-samples the goal from the current column (Neovim updates
  -- curswant on horizontal motion): j after h goes from the h column
  local e = goal_ed()
  e.doc:seek("set", 0)
  e.doc:seek("cur", 10)
  e.cursor_col = 10
  e:dispatch("j")
  lu.assertEquals(e:text_col(), 5)
  keystroke(e, "h") -- renders, so cursor_col becomes 4
  keystroke(e, "j") -- from col 4, not the stale goal 10
  lu.assertEquals(e.doc:column(), 4)
end

os.exit(lu.LuaUnit.run(), true)
