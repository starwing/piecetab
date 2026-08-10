-- lspio bridge tests (luaunit harness): real spawned processes.
-- run: just lua/lspio
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = root .. "/?.lua;" .. dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/lua/luajit/?.so;"
    or root .. "/lua/?.so;")
    .. package.cpath
    .. ";./lua/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
package.cpath = package.cpath
    .. (_G["jit"] and ";/opt/homebrew/lib/lua/5.1/?.so" or ";/opt/homebrew/lib/lua/5.5/?.so")

local lu = require "luaunit"
local yyjson = require "yyjson"
local jsonrpc = require "jsonrpc"
local lspio = require "lspio"

-- write a fake LSP server script; reads frames, replies to requests
local function fake_server(code)
  local path = os.tmpname()
  local f = assert(io.open(path, "w"))
  f:write([[
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
]])
  f:write(code)
  f:close()
  return path
end

-- drive: spawn, run pumps until pred(msg) or timeout. The decoder is
-- created once: partial frames survive across pumps.
local function drive(h, pred, frames)
  frames = frames or 100
  local d = jsonrpc.decoder(h.reader)
  for _ = 1, frames do
    lspio.pump(h)
    while true do
      local msg, err = d:read()
      if msg then
        if pred(msg) then return msg end
      elseif err ~= "again" then
        return nil, err
      else
        break
      end
    end
    os.execute("sleep 0.01")
  end
  return nil, "timeout"
end

TestSpawn = {}

function TestSpawn:testEchoRequest()
  local path = fake_server([[
while true do
  local m = readmsg()
  if not m then break end
  sendmsg({ jsonrpc = "2.0", id = m.id, result = m.params .. "!" })
end
]])
  local h = lspio.spawn({ "lua", path })
  lspio.send(h, jsonrpc.enc_request(1, "echo", "hi"))
  local msg, err = drive(h, function(m) return m.id == 1 end)
  lu.assertNil(err)
  assert(msg ~= nil, err)
  lu.assertEquals(msg.result, "hi!")
  lspio.close(h)
  os.remove(path)
end

function TestSpawn:testSlowSplitOutput()
  -- server writes the frame in 3 chunks with delays
  local path = fake_server([[
local m = readmsg()
local s = require("yyjson").encode({ jsonrpc = "2.0", id = m.id, result = 42 })
io.write("Content-Length: ", #s, "\r\n\r\n")
io.flush()
os.execute("sleep 0.05")
io.write(s:sub(1, 3))
io.flush()
os.execute("sleep 0.05")
io.write(s:sub(4))
io.flush()
]])
  local h = lspio.spawn({ "lua", path })
  lspio.send(h, jsonrpc.enc_request(1, "x", {}))
  local msg, err = drive(h, function(m) return m.id == 1 end, 300)
  lu.assertNil(err)
  assert(msg ~= nil, err)
  lu.assertEquals(msg.result, 42)
  lspio.close(h)
  os.remove(path)
end

function TestSpawn:testEofDetected()
  local path = fake_server([[
local m = readmsg()
sendmsg({ jsonrpc = "2.0", id = m.id, result = true })
-- exit -> stdout EOF
]])
  local h = lspio.spawn({ "lua", path })
  lspio.send(h, jsonrpc.enc_request(1, "x", {}))
  local msg, err = drive(h, function(m) return m.id == 1 end)
  lu.assertNil(err)
  -- after exit, reader must report EOF (nil, not pause)
  lspio.pump(h)
  local m2, err2 = jsonrpc.decoder(h.reader):read()
  lu.assertNil(m2)
  lu.assertEquals(err2, "eof")
  lspio.close(h)
  os.remove(path)
end

function TestSpawn:testExitCode()
  local path = fake_server([[
readmsg() -- consume; exit with code 3
os.exit(3)
]])
  local h = lspio.spawn({ "lua", path })
  lspio.send(h, jsonrpc.enc_notify("x", {}))
  for _ = 1, 100 do
    lspio.pump(h)
    if h.exit then break end
    os.execute("sleep 0.01")
  end
  lu.assertTrue(h.exit)
  lu.assertEquals(h.exit_code, 3)
  lspio.close(h)
  os.remove(path)
end

os.exit(lu.LuaUnit.run(), true)
