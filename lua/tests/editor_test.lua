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
  lu.assertEquals(e.doc:breaks(), 2)
  os.remove(path)
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

os.exit(lu.LuaUnit.run(), true)
