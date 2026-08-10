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

local ROWS, COLS = 6, 40

local function make_ed(content)
  local term = { s = "", write = function(t, x) t.s = t.s .. x end,
                 flush = function() end,
                 move = function(t, row, col)
                   t.s = t.s .. string.format("\27[%d;%dH", row, col)
                 end,
                 size = function() return ROWS, COLS end }
  local e = Ed.new(content, term)
  e.log = function() end
  return e
end

local LINES = 20 -- document lines for scroll tests

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
  local t = Ed.newterm()
  lu.assertTrue(t.esc_timeout > 0 and t.esc_timeout <= 1000)
  local t2 = Ed.newterm({ esc_timeout = 7 })
  lu.assertEquals(t2.esc_timeout, 7)
end

function TestSkeleton:testConstruct()
  local e = make_ed("a\nb")
  lu.assertEquals(e.mode, "NORMAL")
  lu.assertEquals(e.tabstop, 4)
  lu.assertNotNil(e.doc)
  lu.assertEquals(e.doc:breaks(), 2)
end

function TestSkeleton:testNewEmptyDoc()
  local e = make_ed()
  lu.assertEquals(e.doc:breaks(), 0) -- empty doc: 0 lines (see pt_test testLineCountEmpty)
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
  lu.assertNotNil(got)
end

function TestSkeleton:testQuitSetsDone()
  local e = make_ed("")
  e:quit()
  lu.assertTrue(e.done)
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
  lu.assertTrue(e.done)
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
  lu.assertEquals(self.e.doc:line(), 1)
  self.e:dispatch("k")
  lu.assertEquals(self.e.doc:line(), 0)
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
  -- the line view); see pt_test testCommitAfterPartialSync.
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
  e.doc:seek("set", 6) -- end of line, after the 3-byte "好"
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
  e.doc:seek("set", 2) -- EOF
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
  lu.assertTrue(got)
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
  lu.assertTrue(e.done)
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
  lu.assertTrue(e.done)
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
  self.e.doc:seek("set", 6) -- end of "你好" line (byte 6)
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
  self.e:dispatch("j")
  lu.assertEquals(self.e.doc:column(), 4)
  self.e:dispatch("k")
  lu.assertEquals(self.e.doc:column(), 6)
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

function TestScroll:testContentRendered()
  -- document content bytes must appear in frame output (regression:
  -- render content path was only covered via lnum/status/scroll before).
  -- cellgrid diff emits per-cell CUP + char (no contiguous text runs), so
  -- assert content chars land at the expected screen columns.
  local s = frame(self.e)
  lu.assertStrContains(s, "\27[1;5H\27[0ml") -- "line 1": 'l' at col 5
  lu.assertStrContains(s, "\27[1;10H1")      -- "line 1": '1' at col 10
  lu.assertStrContains(s, "\27[5;10H5")      -- "line 5": '5' at col 10
end

function TestScroll:testDownScrollEmitsSu()
  -- j to the last visible row (row 4 of 5), then one more j scrolls
  for _ = 1, 4 do
    keystroke(self.e, "j")
  end
  local s = keystroke(self.e, "j") -- 5th j: scroll_line 0 -> 1
  lu.assertStrContains(s, "\27[1;5r") -- scroll region rows 1..5
  lu.assertStrContains(s, "\27[1S")   -- SU 1: content scrolls UP
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
  lu.assertStrContains(s, "\27[1T")    -- SD 1: content scrolls DOWN
  lu.assertNotStrContains(s, "\27%[1S") -- no SU
  lu.assertEquals(self.e.scroll_line, 9)
end

function TestScroll:testUpScrollRedrawsLineNumbers()
  -- exposed top row after SD is physically blank: the full line number
  -- must be redrawn (regression: only differing digits were emitted,
  -- leaving "54" instead of "854")
  for _ = 1, 14 do -- viewport 10..14, cursor at 14
    keystroke(self.e, "j")
  end
  local s
  for _ = 1, 20 do
    s = keystroke(self.e, "k")
    if self.e.scroll_line < 10 then break end
  end
  -- row 0 line number "10" (1-based) fully redrawn: digits 1 and 0
  lu.assertStrContains(s, "\27[1;2H1") -- col 1: digit '1'
  lu.assertStrContains(s, "\27[1;3H0") -- col 2: digit '0'
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

function TestOps:setUp()
  self.e = make_ed("")
end

function TestOps:testLineNumbersTrackScroll()
  -- bottom line number must stay correct while scrolling
  local e = make_ed(make_doc())
  for _ = 1, 19 do
    keystroke(e, "j")
  end
  local s = frame(e)
  lu.assertStrContains(s, "\27[5;2H2\27[5;3H0") -- lnum "20": tens at col 2, units at col 3
  lu.assertEquals(e.doc:line(), 19)
end

function TestOps:testInsertAndRender()
  self.e:dispatch("i")
  local s = keystroke(self.e, "x")
  lu.assertStrContains(s, "\27[1;5H\27[0mx") -- 'x' rendered at col 5 (content start)
  self.e:dispatch("<Escape>")
end

function TestOps:testStatusBarShowsMode()
  local e = make_ed("")
  local s = frame(e)
  lu.assertStrContains(s, "NORMAL")
  e:dispatch("i")
  s = frame(e)
  lu.assertStrContains(s, "INSERT")
end

function TestOps:testStatusBarShowsCommandLine()
  local e = make_ed("")
  e:dispatch(":")
  local s = frame(e)
  lu.assertStrContains(s, "\27[7m:") -- REVERSE + ":" + cmdline
end

function TestOps:testCursorClampedToScreen()
  -- long line: display col 100 -> screen col clamped to cols (40)
  local e = make_ed(string.rep("x", 100) .. "\n")
  e.doc:seek("set", 100)
  local s = frame(e)
  lu.assertStrContains(s, "\27[1;40H")
end

-- ======== Diff style table ========

TestStyleTable = {}

function TestStyleTable:testFullStateReset()
  -- SGR codes are cumulative: each style CSI must fully reset prior
  -- state, or a DIM->color transition (line number -> keyword) leaks
  -- the DIM attribute onto the colored cell
  for st, csi in pairs(Ed.DIFF_STYLE) do
    if st ~= 0 then
      lu.assertStrContains(csi, "\27[0m", false, "style " .. tostring(st))
    end
  end
end

-- ======== Syntax highlighting ========
TestSyntax = {}

function TestSyntax:testKeywordC()
  -- "int main(void) { return 0; }": i@0, main@4, return@15
  local e = make_ed("int main(void) { return 0; }\n")
  e:open_language("c")
  frame(e)
  local _, st = e.grid:cell(0, 4) -- content col 0 ('i' of int)
  local cp2, st2 = e.grid:cell(0, 4)
  lu.assertEquals(st, Ed.STYLE_KEYWORD)
  local _, st2 = e.grid:cell(0, 8) -- content col 4 ('m' of main)
  assert(st2 ~= Ed.STYLE_KEYWORD, "main should not be keyword: " .. tostring(st2))
  assert(st2 == Ed.STYLE_FUNCTION, "main should be function: " .. tostring(st2))
  local _, st3 = e.grid:cell(0, 21) -- content col 17 ('r' of return)
  lu.assertEquals(st3, Ed.STYLE_KEYWORD)
end

function TestSyntax:testCommentStringC()
  -- "int x; /* note */ char *s = \"hi\";": /@7, quote@29
  local e = make_ed("int x; /* note */ char *s = \"hi\";\n")
  e:open_language("c")
  frame(e)
  local _, st = e.grid:cell(0, 11) -- content col 7 ('/' of comment)
  lu.assertEquals(st, Ed.STYLE_COMMENT)
  local _, st2 = e.grid:cell(0, 32) -- content col 28 ('"' of string)
  lu.assertEquals(st2, Ed.STYLE_STRING)
end

function TestSyntax:testEditUpdatesHighlight()
  local e = make_ed("int main(void) { return 0; }\n")
  e:open_language("c")
  frame(e)
  e.doc:seek("set", 17) -- 'r' of "return"
  e:docedit(1, "")

  frame(e)
  local _, st = e.grid:cell(0, 21) -- content col 17 ("eturn" start)
  lu.assertNotEquals(st, Ed.STYLE_KEYWORD) -- "eturn" is not a keyword
end

function TestSyntax:testNoLanguageNoHighlight()
  local e = make_ed("int main(void) { return 0; }\n")
  frame(e)
  local _, st = e.grid:cell(0, 4) -- content col 0
  lu.assertEquals(st, 0) -- STYLE_NORMAL
end

os.exit(lu.LuaUnit.run(), true)

TestSc = {}

function TestSc:testInternReuse()
  local s = Ed.newsc()
  lu.assertEquals(s:intern({ fg = 207 }), s:intern({ fg = 207 }))
  lu.assertEquals(s:intern({}), 0) -- default attr pre-interned as style 0
end

function TestSc:testInternDistinct()
  local s = Ed.newsc()
  lu.assertNotEquals(s:intern({ fg = 1 }), s:intern({ bg = 1 }))
  lu.assertNotEquals(s:intern({ fg = 1 }), s:intern({ fg = 2 }))
  lu.assertNotEquals(s:intern({ bold = true }), s:intern({ underline = true }))
end

function TestSc:testInternFieldOrderIrrelevant()
  local s = Ed.newsc()
  lu.assertEquals(s:intern({ fg = 1, bold = true }),
                  s:intern({ bold = true, fg = 1 }))
end

function TestSc:testInternUnsetSkipped()
  local s = Ed.newsc()
  lu.assertEquals(s:intern({ bold = true }),
                  s:intern({ bold = true, underline = false, dim = nil }))
end

function TestSc:testInverseLookup()
  local s = Ed.newsc()
  local id = s:intern({ fg = 207, bg = { r = 1, g = 2, b = 3 }, bold = true })
  local a = s:attr(id)
  lu.assertEquals(a.fg, 207)
  lu.assertEquals(a.bg.r, 1)
  lu.assertTrue(a.bold)
  lu.assertNil(s:attr(id + 999))
end

function TestSc:testCsiGeneration()
  local s = Ed.newsc()
  lu.assertEquals(s:csi(0), "\27[0m")
  lu.assertEquals(s:csi(s:intern({})), "\27[0m")
  lu.assertEquals(s:csi(s:intern({ fg = 207 })), "\27[0m\27[38;5;207m")
  lu.assertEquals(s:csi(s:intern({ bold = true, bg = 237 })),
                  "\27[0m\27[1;48;5;237m")
  lu.assertEquals(s:csi(s:intern({ fg = { r = 1, g = 2, b = 3 } })),
                  "\27[0m\27[38;2;1;2;3m")
  lu.assertNil(s:csi(999))
end
