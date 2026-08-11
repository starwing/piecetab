-- lsp RPC/IO tests (luaunit harness).
-- TestRPC: pure framing/decoder; TestIO: real spawned processes.
-- run: just lua/lsp
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
local lsp = require "lsp"

-- chunk reader from a string; chunk_size splits it (0 = whole)
local function reader(s, chunk_size)
  local pos = 1
  return function()
    if pos > #s then return nil end
    local n = chunk_size or #s
    local chunk = s:sub(pos, pos + n - 1)
    pos = pos + #chunk
    return chunk
  end
end

TestRPC = {}

function TestRPC:testRequest()
  local s = lsp.RPC.enc_request(3, "textDocument/didOpen", { uri = "u" })
  lu.assertStrContains(s, "Content-Length: ")
  local msg = assert(yyjson.decode(s:match("\r\n\r\n(.*)$")))
  lu.assertEquals(msg.jsonrpc, "2.0")
  lu.assertEquals(msg.id, 3)
  lu.assertEquals(msg.method, "textDocument/didOpen")
  lu.assertEquals(msg.params.uri, "u")
end

function TestRPC:testNotifyNoParams()
  local s = lsp.RPC.enc_notify("initialized")
  local msg = assert(yyjson.decode(s:match("\r\n\r\n(.*)$")))
  lu.assertEquals(msg.method, "initialized")
  lu.assertNil(msg.params)
  lu.assertNil(msg.id)
end

function TestRPC:testResultAndError()
  local msg = assert(yyjson.decode(lsp.RPC.enc_result(5, { ok = true }):match("\r\n\r\n(.*)$")))
  lu.assertEquals(msg.id, 5)
  lu.assertTrue(msg.result.ok)
  local msg2 = assert(yyjson.decode(lsp.RPC.enc_error(6, -32601, "nope"):match("\r\n\r\n(.*)$")))
  lu.assertEquals(msg2.error.code, -32601)
  lu.assertEquals(msg2.error.message, "nope")
end

function TestRPC:testUtf8Length()
  -- Content-Length is BYTE length; CJK body must count correctly
  local s = lsp.RPC.enc_notify("x", { text = "你好" })
  local len = tonumber(s:match("Content%-Length: (%d+)"))
  lu.assertEquals(len, #(s:match("\r\n\r\n(.*)$")))
end

function TestRPC:testWholeFrame()
  local wire = lsp.RPC.enc_notify("initialized")
  local msg, err = lsp.RPC.decoder(reader(wire)):read()
  lu.assertNil(err)
  lu.assertEquals(msg.method, "initialized")
end

function TestRPC:testSplitHeaderAndBody()
  local wire = lsp.RPC.enc_request(1, "m", { a = 1 })
  -- feed one byte at a time
  local msg2 = lsp.RPC.decoder(reader(wire, 1)):read()
  lu.assertEquals(msg2.id, 1)
  lu.assertEquals(msg2.method, "m")
  lu.assertEquals(msg2.params.a, 1)
end

function TestRPC:testBackToBackFrames()
  -- two frames concatenated: first read consumes exactly one frame
  local a = lsp.RPC.enc_notify("a")
  local b = lsp.RPC.enc_notify("b")
  local chunks, pos = { a, b }, 1
  local function rd()
    if pos > #chunks then return nil end
    local c = chunks[pos]
    pos = pos + 1
    return c
  end
  local msg1 = lsp.RPC.decoder(rd):read()
  lu.assertEquals(msg1.method, "a")
  lu.assertEquals(pos, 2) -- second frame untouched
  local msg2 = lsp.RPC.decoder(rd):read()
  lu.assertEquals(msg2.method, "b")
  lu.assertEquals(pos, 3)
end

function TestRPC:testEof()
  local msg, err = lsp.RPC.decoder(function() return nil end):read()
  lu.assertNil(msg)
  lu.assertEquals(err, "eof")
end

function TestRPC:testTruncatedBody()
  local wire = lsp.RPC.enc_notify("x")
  local msg, err = lsp.RPC.decoder(reader(wire:sub(1, #wire - 3))):read()
  lu.assertNil(msg)
  lu.assertEquals(err, "eof")
end

function TestRPC:testBadJson()
  local msg, err = lsp.RPC.decoder(reader("Content-Length: 3\r\n\r\nbad")):read()
  lu.assertNil(msg)
  lu.assertNotNil(err)
end

function TestRPC:testResponseShape()
  local wire = lsp.RPC.enc_result(9, { data = { 1, 2 } })
  local msg = lsp.RPC.decoder(reader(wire)):read()
  lu.assertEquals(msg.id, 9)
  lu.assertEquals(msg.result.data[2], 2)
end

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
  local d = lsp.RPC.decoder(h.reader)
  for _ = 1, frames do
    lsp.IO.pump(h)
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

TestIO = {}

function TestIO:testEchoRequest()
  local path = fake_server([[
while true do
  local m = readmsg()
  if not m then break end
  sendmsg({ jsonrpc = "2.0", id = m.id, result = m.params .. "!" })
end
]])
  local h = assert(lsp.IO.spawn({ "lua", path }))
  lsp.IO.send(h, lsp.RPC.enc_request(1, "echo", "hi"))
  local msg, err = drive(h, function(m) return m.id == 1 end)
  lu.assertNil(err)
  assert(msg ~= nil, err)
  lu.assertEquals(msg.result, "hi!")
  lsp.IO.close(h)
  os.remove(path)
end

function TestIO:testSlowSplitOutput()
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
  local h = assert(lsp.IO.spawn({ "lua", path }))
  lsp.IO.send(h, lsp.RPC.enc_request(1, "x", {}))
  local msg, err = drive(h, function(m) return m.id == 1 end, 300)
  lu.assertNil(err)
  assert(msg ~= nil, err)
  lu.assertEquals(msg.result, 42)
  lsp.IO.close(h)
  os.remove(path)
end

function TestIO:testEofDetected()
  local path = fake_server([[
local m = readmsg()
sendmsg({ jsonrpc = "2.0", id = m.id, result = true })
-- exit -> stdout EOF
]])
  local h = assert(lsp.IO.spawn({ "lua", path }))
  lsp.IO.send(h, lsp.RPC.enc_request(1, "x", {}))
  local msg, err = drive(h, function(m) return m.id == 1 end)
  lu.assertNil(err)
  -- after exit, reader must report EOF (nil, not pause)
  lsp.IO.pump(h)
  local m2, err2 = lsp.RPC.decoder(h.reader):read()
  lu.assertNil(m2)
  lu.assertEquals(err2, "eof")
  lsp.IO.close(h)
  os.remove(path)
end

function TestIO:testExitCode()
  local path = fake_server([[
readmsg() -- consume; exit with code 3
os.exit(3)
]])
  local h = assert(lsp.IO.spawn({ "lua", path }))
  lsp.IO.send(h, lsp.RPC.enc_notify("x", {}))
  for _ = 1, 100 do
    lsp.IO.pump(h)
    if h.exit then break end
    os.execute("sleep 0.01")
  end
  lu.assertTrue(h.exit)
  lu.assertEquals(h.exit_code, 3)
  lsp.IO.close(h)
  os.remove(path)
end

os.exit(lu.LuaUnit.run(), true)
