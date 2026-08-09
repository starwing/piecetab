-- editor.lua class skeleton tests (luaunit harness).
-- run: just lua-ed (cwd = lua/)
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
                 size = function() return ROWS, COLS end }
  local e = Ed.new(content, term)
  e.log = function() end
  return e
end

TestSkeleton = {}

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
  lu.assertEquals(self.e.doc:column(), 8) -- end of "line one" (col 8)
  self.e:dispatch("0")
  lu.assertEquals(self.e.doc:column(), 0)
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
  local f = assert(io.open(path, "w")); f:write("a\n"); f:close()
  local e = Ed.open(path)
  e.log = function() end
  e:dispatch(":")
  e:dispatch("wq")
  e:dispatch("<Enter>")
  lu.assertTrue(e.done)
  local out = assert(io.open(path, "r"))
  lu.assertEquals(out:read("*a"), "a\n")
  out:close()
  os.remove(path)
end

function TestCommand:testEreloadsFile()
  local path = os.tmpname()
  local f = assert(io.open(path, "w")); f:write("one\n"); f:close()
  local e = Ed.open(path)
  e.log = function() end
  e:dispatch("i")
  e:dispatch("<Escape>")
  e:dispatch(":")
  e:dispatch("e ")
  e:dispatch(path)
  e:dispatch("<Enter>")
  lu.assertEquals(e.filename, path)
  lu.assertEquals(e.doc:breaks(), 1)
  os.remove(path)
end

os.exit(lu.LuaUnit.run(), true)
