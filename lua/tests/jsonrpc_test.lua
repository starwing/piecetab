-- jsonrpc framing tests (luaunit harness).
-- run: just lua/jsonrpc
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = root .. "/?.lua;" .. dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/lua/luajit/?.so;"
    or root .. "/lua/?.so;")
    .. package.cpath
    .. ";./lua/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"

local lu = require "luaunit"
local yyjson = require "yyjson"
local jsonrpc = require "jsonrpc"

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

TestEncode = {}

function TestEncode:testRequest()
  local s = jsonrpc.enc_request(3, "textDocument/didOpen", { uri = "u" })
  lu.assertStrContains(s, "Content-Length: ")
  local msg = assert(yyjson.decode(s:match("\r\n\r\n(.*)$")))
  lu.assertEquals(msg.jsonrpc, "2.0")
  lu.assertEquals(msg.id, 3)
  lu.assertEquals(msg.method, "textDocument/didOpen")
  lu.assertEquals(msg.params.uri, "u")
end

function TestEncode:testNotifyNoParams()
  local s = jsonrpc.enc_notify("initialized")
  local msg = assert(yyjson.decode(s:match("\r\n\r\n(.*)$")))
  lu.assertEquals(msg.method, "initialized")
  lu.assertNil(msg.params)
  lu.assertNil(msg.id)
end

function TestEncode:testResultAndError()
  local msg = assert(yyjson.decode(jsonrpc.enc_result(5, { ok = true }):match("\r\n\r\n(.*)$")))
  lu.assertEquals(msg.id, 5)
  lu.assertTrue(msg.result.ok)
  local msg2 = assert(yyjson.decode(jsonrpc.enc_error(6, -32601, "nope"):match("\r\n\r\n(.*)$")))
  lu.assertEquals(msg2.error.code, -32601)
  lu.assertEquals(msg2.error.message, "nope")
end

function TestEncode:testUtf8Length()
  -- Content-Length is BYTE length; CJK body must count correctly
  local s = jsonrpc.enc_notify("x", { text = "你好" })
  local len = tonumber(s:match("Content%-Length: (%d+)"))
  lu.assertEquals(len, #(s:match("\r\n\r\n(.*)$")))
end

TestRead = {}

function TestRead:testWholeFrame()
  local wire = jsonrpc.enc_notify("initialized")
  local msg, err = jsonrpc.decoder(reader(wire)):read()
  lu.assertNil(err)
  lu.assertEquals(msg.method, "initialized")
end

function TestRead:testSplitHeaderAndBody()
  local wire = jsonrpc.enc_request(1, "m", { a = 1 })
  -- feed one byte at a time
  local msg2 = jsonrpc.decoder(reader(wire, 1)):read()
  lu.assertEquals(msg2.id, 1)
  lu.assertEquals(msg2.method, "m")
  lu.assertEquals(msg2.params.a, 1)
end

function TestRead:testBackToBackFrames()
  -- two frames concatenated: first read consumes exactly one frame
  local a = jsonrpc.enc_notify("a")
  local b = jsonrpc.enc_notify("b")
  local chunks, pos = { a, b }, 1
  local function rd()
    if pos > #chunks then return nil end
    local c = chunks[pos]
    pos = pos + 1
    return c
  end
  local msg1 = jsonrpc.decoder(rd):read()
  lu.assertEquals(msg1.method, "a")
  lu.assertEquals(pos, 2) -- second frame untouched
  local msg2 = jsonrpc.decoder(rd):read()
  lu.assertEquals(msg2.method, "b")
  lu.assertEquals(pos, 3)
end

function TestRead:testEof()
  local msg, err = jsonrpc.decoder(function() return nil end):read()
  lu.assertNil(msg)
  lu.assertEquals(err, "eof")
end

function TestRead:testTruncatedBody()
  local wire = jsonrpc.enc_notify("x")
  local msg, err = jsonrpc.decoder(reader(wire:sub(1, #wire - 3))):read()
  lu.assertNil(msg)
  lu.assertEquals(err, "eof")
end

function TestRead:testBadJson()
  local msg, err = jsonrpc.decoder(reader("Content-Length: 3\r\n\r\nbad")):read()
  lu.assertNil(msg)
  lu.assertNotNil(err)
end

function TestRead:testResponseShape()
  local wire = jsonrpc.enc_result(9, { data = { 1, 2 } })
  local msg = jsonrpc.decoder(reader(wire)):read()
  lu.assertEquals(msg.id, 9)
  lu.assertEquals(msg.result.data[2], 2)
end

os.exit(lu.LuaUnit.run(), true)
