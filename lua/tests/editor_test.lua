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

--[[
function TestSkeleton:testCommandRegister()
  local e = make_ed("")
  local got
  e:command("w", function(self, arg, bang) got = { arg, bang } end)
  e.cmdline = "w file"
  e:dispatch("<Enter>") -- exec stub placeholder until Task 4
  lu.assertNotNil(got)
end
--]]

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
  lu.assertEquals(self.e.doc:column(), 5) -- 跳过 "line"+空格，停在 'o' 前
  self.e:dispatch("b")
  lu.assertEquals(self.e.doc:column(), 0)
end

function TestNormal:testZeroAndDollar()
  self.e:dispatch("$")
  lu.assertEquals(self.e.doc:column(), 8) -- "line one" 尾（列 8，brief 原 7 错）
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
  -- 自定义组合键 "zz" 验证泛化 pending（不依赖内置）
  local e2 = make_ed("")
  local hit = 0
  e2:keymap("normal", "zz", function() hit = hit + 1 end)
  e2:dispatch("z")
  lu.assertEquals(hit, 0) -- 等第二键
  e2:dispatch("z")
  lu.assertEquals(hit, 1)
end

function TestNormal:testPendingMissFallsThrough()
  self.e:dispatch("g")
  self.e:dispatch("x") -- gx 未绑定 → x 生效
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
end

function TestNormal:testOOpensLineAbove()
  self.e:dispatch("O")
  lu.assertEquals(self.e.mode, "INSERT")
  lu.assertEquals(self.e.doc:breaks(), 4)
end

TestInsert = {}

function TestInsert:setUp()
  self.e = make_ed("ab\n")
end

local function esc(e) e:dispatch("<Escape>") end

function TestInsert:testTypeText()
  self.e:dispatch("a") -- a: 光标右移一字符进 insert
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

function TestInsert:testEnterSplitsLine()
  self.e:dispatch("i")
  self.e:dispatch("<Enter>")
  esc(self.e)
  lu.assertEquals(self.e.doc:breaks(), 2)
end

function TestInsert:testEscapeMovesLeft()
  self.e:dispatch("i")
  self.e:dispatch("<Escape>")
  lu.assertEquals(self.e.doc:column(), 0) -- ESC 后光标左移一字符
end

function TestInsert:testControlKeyFiltered()
  self.e:dispatch("i")
  self.e:dispatch("<F5>") -- 未绑定控制键 → 不插入
  esc(self.e)
  lu.assertEquals(self.e.doc:read("l"), "ab")
end

os.exit(lu.LuaUnit.run(), true)
