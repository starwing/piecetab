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
local lspio = require "lspio"
local yyjson = require "yyjson"

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
  lu.assertEquals(e.doc:breaks(), 0) -- empty doc: 0 lines (see piecetab_test testLineCountEmpty)
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
  local s = Ed.newsc()
  local attrs = { { fg = 207 }, { bg = 237, dim = true },
                  { bold = true, underline = true } }
  for i, a in ipairs(attrs) do
    lu.assertStrContains(s:csi(s:intern(a)), "\27[0m", false, "attr " .. i)
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
  lu.assertEquals(st, e.sc:intern(Ed.ATTR_KEYWORD))
  local _, st2 = e.grid:cell(0, 8) -- content col 4 ('m' of main)
  assert(st2 ~= e.sc:intern(Ed.ATTR_KEYWORD),
         "main should not be keyword: " .. tostring(st2))
  assert(st2 == e.sc:intern(Ed.ATTR_FUNCTION),
         "main should be function: " .. tostring(st2))
  local _, st3 = e.grid:cell(0, 21) -- content col 17 ('r' of return)
  lu.assertEquals(st3, e.sc:intern(Ed.ATTR_KEYWORD))
end

function TestSyntax:testCommentStringC()
  -- "int x; /* note */ char *s = \"hi\";": /@7, quote@29
  local e = make_ed("int x; /* note */ char *s = \"hi\";\n")
  e:open_language("c")
  frame(e)
  local _, st = e.grid:cell(0, 11) -- content col 7 ('/' of comment)
  lu.assertEquals(st, e.sc:intern(Ed.ATTR_COMMENT))
  local _, st2 = e.grid:cell(0, 32) -- content col 28 ('"' of string)
  lu.assertEquals(st2, e.sc:intern(Ed.ATTR_STRING))
end

function TestSyntax:testEditUpdatesHighlight()
  local e = make_ed("int main(void) { return 0; }\n")
  e:open_language("c")
  frame(e)
  e.doc:seek("set", 17) -- 'r' of "return"
  e:docedit(1, "")

  frame(e)
  local _, st = e.grid:cell(0, 21) -- content col 17 ("eturn" start)
  lu.assertNotEquals(st, e.sc:intern(Ed.ATTR_KEYWORD))
end

function TestSyntax:testNoLanguageNoHighlight()
  local e = make_ed("int main(void) { return 0; }\n")
  frame(e)
  local _, st = e.grid:cell(0, 4) -- content col 0
  lu.assertEquals(st, 0) -- default handle
end

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

TestLayers = {}

-- 3 pieces: "aaaa" + "XY" + " bbbb" via two mid-doc edits
local function make_pieces(content)
  local e = make_ed(content)
  e.doc:seek("set", 4)
  e:docedit(0, "XY") -- split into 2 pieces
  e.doc:seek("set", 8)
  e:docedit(0, "Z") -- split into 3 pieces
  e.show_pieces = true
  return e
end

-- "aaaaXY bZbbb\n": pieces [0,4) plain, [4,6) gray, [6,8) plain,
-- [8,9) gray, [9,13) plain (inserts create hole pieces)
function TestLayers:testPiecesAlternate()
  local e = make_pieces("aaaa bbbb\n")
  frame(e)
  local bg = e.sc:intern(Ed.ATTR_GRAY_BG)
  local _, st = e.grid:cell(0, 4) -- 'a' (piece 1, plain)
  lu.assertEquals(st, 0)
  local _, st2 = e.grid:cell(0, 8) -- 'X' (piece 2, gray)
  lu.assertEquals(st2, bg)
  local _, st3 = e.grid:cell(0, 12) -- 'Z' (piece 4, gray)
  lu.assertEquals(st3, bg)
  local _, st4 = e.grid:cell(0, 13) -- 'b' (piece 5, plain)
  lu.assertEquals(st4, 0)
end

function TestLayers:testPiecesToggleCommand()
  local e = make_pieces("aaaa bbbb\n")
  lu.assertTrue(e.show_pieces)
  e:dispatch(":") -- not needed; command path below
  e.commands.pieces(e, "", false)
  lu.assertFalse(e.show_pieces)
end

function TestLayers:testPieceGrayOnlyOnEvenPieces()
  -- odd piece must NOT carry gray (piece 1 = plain)
  local e = make_pieces("aaaa bbbb\n")
  frame(e)
  local _, st = e.grid:cell(0, 9) -- 'Y' (piece 2, gray)
  lu.assertEquals(st, e.sc:intern(Ed.ATTR_GRAY_BG))
  local _, st2 = e.grid:cell(0, 4) -- 'a' (piece 1, plain)
  lu.assertEquals(st2, 0)
end

-- "int Qx\n": pieces [0,4) "int " plain, [4,5) "Q" gray, [5,7) "x\n" plain
function TestLayers:testLayeredCompose()
  -- syntax fg on plain piece; piece bg alone on gray piece
  local e = make_ed("int x\n")
  e:open_language("c")
  e.doc:seek("set", 4)
  e:docedit(0, "Q")
  e.show_pieces = true
  frame(e)
  local bg = e.sc:intern(Ed.ATTR_GRAY_BG)
  local kw = e.sc:intern(Ed.ATTR_KEYWORD)
  local _, st = e.grid:cell(0, 4) -- 'i' (piece 1, plain): keyword only
  lu.assertEquals(st, kw)
  local _, st2 = e.grid:cell(0, 8) -- 'Q' (piece 2, gray): no syntax
  lu.assertEquals(st2, bg)
  local _, st3 = e.grid:cell(0, 9) -- 'x' (piece 3, plain): nothing
  lu.assertEquals(st3, 0)
  lu.assertNotEquals(kw, bg)
end

-- "aaaXYa\nbbbb\n": pieces [0,3) plain, [3,5) gray, [5,12) plain —
-- the plain piece crosses the line break
function TestLayers:testPieceAcrossLineBoundary()
  local e = make_ed("aaaa\nbbbb\n")
  e.doc:seek("set", 3)
  e:docedit(0, "XY")
  e.show_pieces = true
  frame(e)
  local _, st = e.grid:cell(0, 4) -- 'a' col 0 (piece 1, plain)
  lu.assertEquals(st, 0)
  local _, st2 = e.grid:cell(0, 8) -- 'Y' col 4 (piece 2, gray, same row)
  lu.assertEquals(st2, e.sc:intern(Ed.ATTR_GRAY_BG))
  local _, st3 = e.grid:cell(1, 4) -- row 1 col 0 (piece 3, plain)
  lu.assertEquals(st3, 0)
end

-- "char *s = \"XYaaaa\";\n": pieces [0,12) plain, [12,14) gray,
-- [14,20) plain; the string literal "XYaaaa" spans the piece boundary —
-- string+bg on the gray piece, fg-only on the plain pieces
function TestLayers:testMergeLayersUnsetPassesThrough()
  local e = make_ed('char *s = "aaaa";\n')
  e:open_language("c")
  e.doc:seek("set", 12) -- at the opening quote
  e:docedit(0, "XY")
  e.show_pieces = true
  frame(e)
  local _, st = e.grid:cell(0, 4) -- 'c' of char (piece 1, plain): keyword
  lu.assertEquals(st, e.sc:intern(Ed.ATTR_KEYWORD))
  local _, st2 = e.grid:cell(0, 16) -- '"' (piece 2, gray): string + gray
  lu.assertEquals(st2, e.sc:intern({ fg = 114, bg = 237 }))
  local _, st3 = e.grid:cell(0, 18) -- 'a' (piece 3, plain): string only
  lu.assertEquals(st3, e.sc:intern(Ed.ATTR_STRING))
end

TestVisual = {}

function TestVisual:testEnterAndExtend()
  local e = make_ed("abcdef\n")
  e:dispatch("v")
  lu.assertEquals(e.mode, "VISUAL")
  lu.assertEquals(e.sel_start, 0)
  local s = frame(e)
  lu.assertStrContains(s, "VISUAL") -- status bar mode
  local rev = e.sc:intern(Ed.ATTR_REVERSE)
  -- cursor char is inside the selection immediately (vim charwise)
  local _, st = e.grid:cell(0, 4) -- 'a'
  lu.assertEquals(st, rev)
  e:dispatch("l") -- cursor 1, selection [0,2) = "ab"
  frame(e)
  local _, st2 = e.grid:cell(0, 5) -- 'b'
  lu.assertEquals(st2, rev)
  local _, st3 = e.grid:cell(0, 6) -- 'c' (outside selection)
  lu.assertEquals(st3, 0)
end

function TestVisual:testReverseSelectionExtendsBackward()
  -- l past sel_start then h back: selection is [min,max), anchor lost
  local e = make_ed("abcdef\n")
  e:dispatch("v")
  e:dispatch("l")
  e:dispatch("l") -- cursor 2, selection [0,3) = "abc"
  e:dispatch("h") -- cursor 1, selection [0,2) = "ab"
  frame(e)
  local rev = e.sc:intern(Ed.ATTR_REVERSE)
  local _, st = e.grid:cell(0, 5) -- 'b' (cursor char)
  lu.assertEquals(st, rev)
  local _, st2 = e.grid:cell(0, 6) -- 'c' (outside selection)
  lu.assertEquals(st2, 0)
end

-- '"XY' at bytes 10..16: 'X' (byte 12) carries string fg (syntax) +
-- gray bg (piece 2) + reverse (selection) in one cell — 3-layer merge
function TestVisual:testThreeLayerCompose()
  local e = make_ed('char *s = "aaaa";\n')
  e:open_language("c")
  e.doc:seek("set", 12)
  e:docedit(0, "XY")
  e.doc:seek("set", 12) -- on 'X' (piece 2, inside string literal)
  e:dispatch("v")
  e:dispatch("l")
  frame(e)
  local _, st = e.grid:cell(0, 16) -- content col 12
  lu.assertEquals(st, e.sc:intern({ fg = 114, reverse = true, bg = 237 }))
end

function TestVisual:testEscapeClears()
  local e = make_ed("ab\n")
  e:dispatch("v")
  e:dispatch("l")
  e:dispatch("<Escape>")
  lu.assertEquals(e.mode, "NORMAL")
  lu.assertNil(e.sel_start)
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
  e:dispatch("v") -- sel_start 0
  e:dispatch("j") -- cursor line 1 col 0, selection "ab\nc"
  frame(e)
  local rev = e.sc:intern(Ed.ATTR_REVERSE)
  local _, st = e.grid:cell(0, 4) -- 'a'
  lu.assertEquals(st, rev)
  local _, st2 = e.grid:cell(0, 5) -- 'b'
  lu.assertEquals(st2, rev)
  local _, st3 = e.grid:cell(1, 4) -- 'c' (cursor char, selected)
  lu.assertEquals(st3, rev)
  local _, st4 = e.grid:cell(1, 5) -- 'd' (outside selection)
  lu.assertEquals(st4, 0)
  e:dispatch("d") -- deletes "ab\nc"
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

-- ======== LSP layers (Task 5/6) ========

-- shared preamble: frame helpers over stdio (fake server child process)
local function fake_preamble()
  return [[
local function readmsg()
  local head = io.read("*l")
  if not head then return nil end
  local len = tonumber(head:match("(%d+)"))
  io.read("*l") -- blank line
  local body = io.read(len)
  local y = require("yyjson")
  return y.decode(body)
end
local function sendmsg(t)
  local s = require("yyjson").encode(t)
  io.write("Content-Length: ", #s, "\r\n\r\n", s)
  io.flush()
end
local y = require("yyjson")
]]
end

local function fake_server(code)
  local path = os.tmpname()
  local f = assert(io.open(path, "w"))
  f:write(fake_preamble())
  f:write(code)
  f:close()
  return path
end

-- poll the lsp transport until pred() or the frame budget runs out
local function lsp_drive(e, pred, frames)
  frames = frames or 200
  for _ = 1, frames do
    if e.lsp then e.lsp:poll() end
    if pred() then return true end
    os.execute("sleep 0.01")
  end
  return false
end

TestLspSemantic = {}

-- server: handshake with a semantic legend, answer full requests
local function sem_code(data)
  return string.format([[
local legend = { tokenTypes = { "keyword", "string", "number",
  "comment", "function" }, tokenModifiers = {} }
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = { capabilities = {
        semanticTokensProvider = { legend = legend, full = true } } } })
    elseif m.method == "textDocument/semanticTokens/full" then
      sendmsg({ jsonrpc = "2.0", id = m.id,
        result = { data = { %s } } })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    end
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]], data)
end

function TestLspSemantic:testOverrideAndCoexist()
  -- semantic: int=keyword, main=function, return=number (overrides the
  -- syntax keyword); void stays syntax-only
  local path = fake_server(sem_code(
    "0,0,3,0,0, 0,4,4,4,0, 0,13,6,2,0"))
  local e = make_ed("int main(void) { return 0; }\n")
  e:open_language("c")
  e:lsp_start({ "lua", path })
  lu.assertTrue(lsp_drive(e, function() return e.lsp.state == "running" end))
  frame(e) -- render end issues the semanticTokens/full request
  lu.assertTrue(lsp_drive(e, function() return #e.lsp_sem.spans > 0 end),
    "spans decoded")
  frame(e)
  local kw = e.sc:intern(Ed.ATTR_KEYWORD)
  local fn = e.sc:intern(Ed.ATTR_FUNCTION)
  local nu = e.sc:intern(Ed.ATTR_NUMBER)
  local _, st = e.grid:cell(0, 4)  -- 'i' of int (semantic keyword)
  lu.assertEquals(st, kw)
  local _, st2 = e.grid:cell(0, 8) -- 'm' of main (semantic function)
  lu.assertEquals(st2, fn)
  local _, st3 = e.grid:cell(0, 13) -- 'v' of void (syntax only)
  lu.assertEquals(st3, kw)
  local _, st4 = e.grid:cell(0, 21) -- 'r' of return (semantic number wins)
  lu.assertEquals(st4, nu)
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestLspSemantic:testUtf16Decode()
  -- "你好" = 2 UTF-16 units / 6 bytes; "😀" = 2 units / 4 bytes
  local path = fake_server(sem_code("0,0,2,1,0"))
  local e = make_ed("你好世界\n")
  e:lsp_start({ "lua", path })
  lu.assertTrue(lsp_drive(e, function() return e.lsp.state == "running" end))
  frame(e)
  lu.assertTrue(lsp_drive(e, function() return #e.lsp_sem.spans > 0 end),
    "spans decoded")
  lu.assertEquals(e.lsp_sem.spans[1].offset, 0)
  lu.assertEquals(e.lsp_sem.spans[1].length, 6)
  frame(e)
  local st = e.sc:intern(Ed.ATTR_STRING)
  local _, c0 = e.grid:cell(0, 4) -- '你'
  local _, c1 = e.grid:cell(0, 5) -- '你' (width 2)
  local _, c2 = e.grid:cell(0, 6) -- '好'
  lu.assertEquals(c0, st)
  lu.assertEquals(c1, st)
  lu.assertEquals(c2, st)
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
  -- emoji: 4 UTF-8 bytes = 2 UTF-16 units -> span length 4
  local path2 = fake_server(sem_code("0,1,2,1,0"))
  local e2 = make_ed("a😀b\n")
  e2:lsp_start({ "lua", path2 })
  lu.assertTrue(lsp_drive(e2, function() return e2.lsp.state == "running" end))
  frame(e2)
  lu.assertTrue(lsp_drive(e2, function() return #e2.lsp_sem.spans > 0 end),
    "spans decoded")
  lu.assertEquals(e2.lsp_sem.spans[1].offset, 1)
  lu.assertEquals(e2.lsp_sem.spans[1].length, 4)
  e2.lsp:stop()
  lspio.close(e2.lsp.io)
  os.remove(path2)
end

TestLspDiag = {}

function TestLspDiag:testPushRenderAndVersionDrop()
  local path = fake_server([[
local function push(v, msg, s0, s1)
  sendmsg({ jsonrpc = "2.0", method = "textDocument/publishDiagnostics",
    params = { uri = "file://", version = v, diagnostics = {
      { range = { start = { line = 0, character = s0 },
        ["end"] = { line = 0, character = s1 } },
        message = msg, severity = 1 } } } })
end
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = { capabilities = {} } })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    end
  elseif m.method == "textDocument/didOpen" then
    push(1, "oops", 0, 5)
  elseif m.method == "textDocument/didChange" then
    local v = m.params.textDocument.version
    push(v, "later", 6, 11)
    push(1, "stale", 0, 5) -- out-of-order: must be dropped
    sendmsg({ jsonrpc = "2.0", method = "test/done" })
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]])
  local e = make_ed("hello world\n")
  e:lsp_start({ "lua", path })
  local done = false
  e.lsp:on("test/done", function() done = true end)
  lu.assertTrue(lsp_drive(e, function() return e.lsp_diag ~= nil end),
    "first push")
  frame(e)
  local und = e.sc:intern(Ed.ATTR_DIAG)
  local _, st = e.grid:cell(0, 4) -- 'h' of hello (v1 range 0-5)
  lu.assertEquals(st, und)
  e.doc:seek("set", 5)
  e:docedit(0, "!")
  lu.assertTrue(lsp_drive(e, function() return done end), "edit pushes")
  lu.assertEquals(e.lsp_diag.version, 2)
  lu.assertEquals(#e.lsp_diag.spans, 1) -- stale v1 dropped
  lu.assertStrContains(e.msg, "later")
  frame(e)
  local _, st2 = e.grid:cell(0, 10) -- ' ' (v2 range 6-11)
  lu.assertEquals(st2, und)
  local _, st3 = e.grid:cell(0, 14) -- 'l' of world
  lu.assertEquals(st3, und)
  local _, st4 = e.grid:cell(0, 15) -- 'd' outside range: not underlined
  lu.assertNotEquals(st4, und)
  local _, st5 = e.grid:cell(0, 4) -- 'h' no longer underlined
  lu.assertNotEquals(st5, und)
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestLspDiag:testUndoResyncs()
  -- edit -> diag; undo (full resync) -> server clears -> underline gone
  local path = fake_server([[
local function push(v, msg)
  sendmsg({ jsonrpc = "2.0", method = "textDocument/publishDiagnostics",
    params = { uri = "file://", version = v, diagnostics = {
      { range = { start = { line = 0, character = 0 },
        ["end"] = { line = 0, character = 5 } },
        message = msg, severity = 1 } } } })
end
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = { capabilities = {} } })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    end
  elseif m.method == "textDocument/didOpen" then
    push(1, "oops")
  elseif m.method == "textDocument/didChange" then
    local v = m.params.textDocument.version
    if m.params.contentChanges[1].range then
      push(v, "bad")
    else
      sendmsg({ jsonrpc = "2.0", method = "textDocument/publishDiagnostics",
        params = { uri = "file://", version = v, diagnostics = {} } })
      sendmsg({ jsonrpc = "2.0", method = "test/full" })
    end
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]])
  local e = make_ed("hello\n")
  e:lsp_start({ "lua", path })
  local full = false
  e.lsp:on("test/full", function() full = true end)
  lu.assertTrue(lsp_drive(e, function() return e.lsp_diag ~= nil end),
    "first push")
  frame(e)
  local und = e.sc:intern(Ed.ATTR_DIAG)
  local _, st = e.grid:cell(0, 4) -- 'h' (v1 range 0-5)
  lu.assertEquals(st, und)
  -- insert error, esc commits: v2 diag on "Xhello"
  e:dispatch("i")
  e:dispatch("X")
  e:dispatch("<Escape>")
  lu.assertTrue(lsp_drive(e, function()
    return e.lsp_diag and e.lsp_diag.version == 2 end), "v2 push")
  -- undo: full resync, server clears the diag
  e:dispatch("u")
  lu.assertTrue(lsp_drive(e, function() return full end), "full didChange")
  lu.assertTrue(lsp_drive(e, function()
    return e.lsp_diag and e.lsp_diag.version == 3 end), "v3 clear")
  lu.assertEquals(#e.lsp_diag.spans, 0)
  frame(e)
  local _, st2 = e.grid:cell(0, 4) -- 'h' back to plain
  lu.assertNotEquals(st2, und)
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestLspDiag:testCursorStatusMessage()
  -- cursor inside an underlined span -> status center shows the message;
  -- overlapping spans resolve by severity (1 = error wins)
  local path = fake_server([[
local function pushd(v, msg, sev, s0, s1)
  sendmsg({ jsonrpc = "2.0", method = "textDocument/publishDiagnostics",
    params = { uri = "file://", version = v, diagnostics = {
      { range = { start = { line = 0, character = s0 },
        ["end"] = { line = 0, character = s1 } },
        message = msg, severity = sev } } } })
end
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = { capabilities = {} } })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    end
  elseif m.method == "textDocument/didOpen" then
    pushd(1, "oops", 1, 0, 5)
    pushd(1, "dup", 2, 0, 5)  -- same range, weaker severity
    pushd(1, "warn", 2, 6, 11)
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]])
  local e = make_ed("hello world\n")
  e.term.size = function() return ROWS, 80 end -- wide terminal: full msg
  e:lsp_start({ "lua", path })
  lu.assertTrue(lsp_drive(e, function() return e.lsp_diag ~= nil end), "push")
  e.doc:seek("set", 0)
  e.term.s = ""
  local s = frame(e)
  lu.assertStrContains(s, "diag: oops") -- severity 1 wins over "dup"
  lu.assertNotStrContains(s, "diag: dup")
  e.doc:seek("set", 10)
  e.term.s = ""
  s = frame(e)
  lu.assertStrContains(s, "diag: warn")
  e.doc:seek("set", 12) -- past all spans: center goes blank
  e.term.s = ""
  s = frame(e)
  lu.assertNotStrContains(s, "diag: warn")
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestLspDiag:testRunningNoMsgDup()
  -- :lsp on once running: right segment shows lsp:running persistently,
  -- on_status must not also write it into the transient msg
  local path = fake_server([[
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = { capabilities = {} } })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    end
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]])
  local e = make_ed("x\n")
  e:lsp_start({ "lua", path })
  lu.assertTrue(lsp_drive(e, function() return e.lsp.state == "running" end))
  lu.assertNotStrContains(e.msg, "running", "steady state silent in msg")
  e.term.s = ""
  local s = frame(e)
  lu.assertStrContains(s, "lsp:on", "right segment persistent, short form")
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

-- ======== inlay hints (Task 7) ========

TestHint = {}

-- server: answers inlayHint with a fixed hint list, echoes each request
local function hint_code(hints)
  return string.format([[
local hints = %s
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = { capabilities = {
        inlayHintProvider = true } } })
    elseif m.method == "textDocument/inlayHint" then
      sendmsg({ jsonrpc = "2.0", method = "test/req" })
      sendmsg({ jsonrpc = "2.0", id = m.id, result = hints })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    end
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]], hints)
end

local function hint_ed(code, content)
  local path = fake_server(hint_code(code))
  local e = make_ed(content)
  e:lsp_start({ "lua", path })
  lu.assertTrue(lsp_drive(e, function() return e.lsp.state == "running" end))
  return e, path
end

function TestHint:testInjectShift()
  -- hint at (0,0): injected before the text, shifting it right
  local e, path = hint_ed([[
    { { position = { line = 0, character = 0 }, label = "int:" } }]], "local x = 1\n")
  frame(e) -- render end issues the inlayHint request
  lu.assertTrue(lsp_drive(e, function() return e.lsp_hints ~= nil end),
    "hint response")
  frame(e)
  local dim = e.sc:intern(Ed.ATTR_DIM)
  local _, st = e.grid:cell(0, 4) -- hint text starts at content col 0
  lu.assertEquals(st, dim)
  local _, c4 = e.grid:cell(0, 8) -- 'l' of local shifted by #"int:"=4
  lu.assertEquals(c4, e.sc:intern({}))
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestHint:testScrollRefetch()
  -- scroll moves the viewport -> new request (8 lines > 5 visible rows)
  local reqs = 0
  local e, path = hint_ed([[
    { { position = { line = 1, character = 0 }, label = "b:" } }]],
    "aaa\nbbb\nccc\nddd\neee\nfff\nggg\nhhh\n")
  e.lsp:on("test/req", function(p) reqs = reqs + 1 end)
  frame(e)
  lu.assertTrue(lsp_drive(e, function() return e.lsp_hints ~= nil end), "first")
  lu.assertEquals(reqs, 1)
  e:dispatch("G") -- cursor to last line: viewport scrolls
  frame(e)
  lu.assertTrue(lsp_drive(e, function() return reqs >= 2 end), "scrolled refetch")
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestHint:testNullSilent()
  -- sumneko declares support but returns null: must stay silent
  local e, path = hint_ed("y.null", "local x = 1\n")
  frame(e)
  lu.assertTrue(lsp_drive(e, function() return e.lsp_hint_pending == false
    and e.lsp_hint_dirty == false end), "null handled")
  lu.assertNil(e.lsp_hints)
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestHint:testInjectMultiLine()
  -- hints on a later line: per-line cache lookup must use the line
  -- being rendered (regression: line_idx left over from the pass loop)
  local e, path = hint_ed([[
    { { position = { line = 2, character = 0 }, label = "p2:" } }]],
    "aaa\nbbb\nccc\nddd\n")
  frame(e)
  lu.assertTrue(lsp_drive(e, function() return e.lsp_hints ~= nil end),
    "hint response")
  frame(e)
  local dim = e.sc:intern(Ed.ATTR_DIM)
  local _, st = e.grid:cell(2, 4) -- row 2 = doc line 2, hint at col 0
  lu.assertEquals(st, dim)
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestHint:testRelativePathAbsoluteUri()
  -- relative filenames (lua editor.lua foo) must yield absolute LSP
  -- uris: relative file:// URIs break workspace indexing/type resolution
  local e = make_ed("x\n")
  e.filename = "foo.lua"
  local sp = fake_server([[
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = { capabilities = {} } })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    end
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]])
  e:lsp_start({ "lua", sp })
  lu.assertTrue(lsp_drive(e, function() return e.lsp.state == "running" end))
  lu.assertStrContains(e.lsp.uri, "file:///", "absolute uri")
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(sp)
end

function TestHint:testSpawnFailSilentAndLoud()
  -- automatic start (silent): missing server quietly disables lsp;
  -- manual :lsp on (loud): reports the failure in msg
  local e = make_ed("x\n")
  e.filename = "foo.lua"
  lu.assertFalse(e:lsp_start({ "/nonexistent/lsp-server" }, true))
  lu.assertNil(e.lsp)
  lu.assertEquals(e.msg, "", "silent start: no message")
  local e2 = make_ed("x\n")
  e2.filename = "foo.lua"
  lu.assertFalse(e2:lsp_start({ "/nonexistent/lsp-server" }))
  lu.assertNil(e2.lsp)
  lu.assertStrContains(e2.msg, "exited", "loud start: reports failure")
end

function TestHint:testNoServerMsg()
  -- :lsp on for a file with no matching server
  local e = make_ed("x\n")
  e.filename = "foo.txt"
  e.commands.lsp(e, "on")
  lu.assertStrContains(e.msg, "no server")
  lu.assertNil(e.lsp)
end

function TestHint:testCursorMotionSkipsHint()
  -- normal motion (h/l) never rests on a hint: hint-start byte shows
  -- past the hint; only at the insert gap (append) does the cursor sit
  -- on the hint's first char, with input landing before it
  local e, path = hint_ed([[
    { { position = { line = 0, character = 0 }, label = "int:" } }]],
    "hello\n")
  frame(e)
  lu.assertTrue(lsp_drive(e, function() return e.lsp_hints ~= nil end),
    "hint response")
  -- normal mode at doc byte 0 (hint start byte): shifted past the hint
  e.doc:seek("set", 0)
  e.term.s = ""
  frame(e)
  lu.assertStrContains(e.term.s, "\27[1;9H\27[?25h", false, "motion skips hint") -- 0+4+3+2
  -- insert mode at the same byte: cursor on the hint's first char
  e.mode = "INSERT"
  e.term.s = ""
  frame(e)
  lu.assertStrContains(e.term.s, "\27[1;5H\27[?25h", false, "gap sits on hint") -- 0+3+2
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestHint:testEditShiftsHints()
  -- stale hint positions would squeeze new chars: same-line edits
  -- shift hints past the edit point; deleted-range hints drop;
  -- multi-line edits clear the cache
  local e, path = hint_ed([[
    { { position = { line = 0, character = 0 }, label = "int:" } }]],
    "hello\n")
  frame(e)
  lu.assertTrue(lsp_drive(e, function() return e.lsp_hints ~= nil end),
    "hint response")
  -- insert before the hint: hint shifts right by 1
  e.doc:seek("set", 0)
  e:docedit(0, "x")
  lu.assertEquals(e.lsp_hints[0][1].dcol, 1, "insert shifts hint")
  -- delete the char before it: hint shifts back
  e.doc:seek("set", 0)
  e:docedit(1, "")
  lu.assertEquals(e.lsp_hints[0][1].dcol, 0, "delete shifts hint back")
  -- delete over the hint position: hint inside the range is dropped
  e:docedit(1, "") -- delete 'h' again -> hint at 0 is in range 0..1
  lu.assertNil(e.lsp_hints[0][1], "hint in deleted range dropped")
  lu.assertEquals(#e.lsp_hints[0], 0)
  -- multi-line edit: cache cleared (refetch on next frame)
  e:docedit(0, "a\nb")
  lu.assertNil(e.lsp_hints, "multi-line edit clears cache")
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

function TestHint:testConfigAnswer()
  -- server asks workspace/configuration (LuaLS style): editor answers
  -- with hint.enable=true, unknown sections as null
  local path = fake_server([[
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = { capabilities = {
        inlayHintProvider = true } } })
    elseif not m.method then
      sendmsg({ jsonrpc = "2.0", method = "test/cfg", params = m })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    end
  elseif m.method == "initialized" then
    sendmsg({ jsonrpc = "2.0", id = 100, method = "workspace/configuration",
      params = { items = { { section = "Lua" }, { section = "other" } } } })
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]])
  local e = make_ed("x\n")
  e:lsp_start({ "lua", path })
  local cfg
  e.lsp:on("test/cfg", function(p) cfg = p end)
  lu.assertTrue(lsp_drive(e, function() return cfg ~= nil end), "answered")
  lu.assertEquals(cfg.result[1].hint.enable, true)
  lu.assertEquals(cfg.result[2], yyjson.null)
  e.lsp:stop()
  lspio.close(e.lsp.io)
  os.remove(path)
end

os.exit(lu.LuaUnit.run(), true)
