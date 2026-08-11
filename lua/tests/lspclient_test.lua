-- lspclient tests (luaunit harness): real spawned fake server processes
-- that answer the LSP handshake and echo received sync messages back as
-- test/* notifications. run: just lua/lspclient
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
local lspio = require "lspio"
local lspclient = require "lspclient"

-- protocol loop: handshake + echo sync messages as test/* notifications
local CODE = [[
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id,
        result = { capabilities = { textDocumentSync = { change = 2 } } } })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    end
  elseif m.method == "initialized" then
    sendmsg({ jsonrpc = "2.0", method = "test/initialized" })
  elseif m.method == "textDocument/didOpen" then
    sendmsg({ jsonrpc = "2.0", method = "test/didOpen",
      params = { text = m.params.textDocument.text,
                 uri = m.params.textDocument.uri } })
  elseif m.method == "textDocument/didChange" then
    sendmsg({ jsonrpc = "2.0", method = "test/didChange", params = m.params })
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]]

-- protocol loop variant: also push diagnostics after didOpen
local CODE_DIAG = [[
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id,
        result = { capabilities = {} } })
    end
  elseif m.method == "textDocument/didOpen" then
    sendmsg({ jsonrpc = "2.0", method = "textDocument/publishDiagnostics",
      params = { uri = "file:///t.lua",
        diagnostics = { { range = { start = { line = 0, character = 0 },
          ["end"] = { line = 0, character = 5 } }, message = "oops" } } } })
  end
end
]]

-- write a fake server script with the shared frame helpers
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
local y = require("yyjson")
]])
  f:write(code)
  f:close()
  return path
end

-- fake document: array of lines; byte offsets computed against
-- table.concat(lines, "\n")
local function new_client(code, lines)
  local path = fake_server(code)
  local c = lspclient.new({
    get_text = function() return table.concat(lines, "\n") end,
    get_line = function(l) return lines[l + 1] end,
    offset_pos = function(off)
      for l, t in ipairs(lines) do
        if off <= #t then return l - 1, off end
        off = off - #t - 1
      end
      return #lines - 1, 0
    end,
    on_status = function() end,
  })
  c:start({ "lua", path }, "file:///t.lua", "lua")
  return c, path
end

-- pump until pred() is true or the frame budget runs out
local function drive(c, pred, frames)
  frames = frames or 200
  for _ = 1, frames do
    c:poll()
    if pred() then return true end
    os.execute("sleep 0.01")
  end
  return false
end

TestLspClient = {}

function TestLspClient:testHandshakeDidOpen()
  local got
  local c, path = new_client(CODE, { "hello", "world" })
  c:on("test/didOpen", function(p) got = p end)
  lu.assertTrue(drive(c, function() return got ~= nil end), "didOpen echo")
  lu.assertEquals(c.state, "running")
  lu.assertEquals(got.text, "hello\nworld")
  lu.assertEquals(got.uri, "file:///t.lua")
  lspio.close(c.io)
  os.remove(path)
end

function TestLspClient:testHelloIgnored()
  -- sumneko sends $/hello before the initialize response; the client
  -- must ignore it and still complete the handshake
  local got
  local c, path = new_client([[
sendmsg({ jsonrpc = "2.0", method = "$/hello", params = { "world" } })
]] .. CODE, { "x" })
  c:on("test/didOpen", function(p) got = p end)
  lu.assertTrue(drive(c, function() return got ~= nil end), "didOpen echo")
  lu.assertEquals(c.state, "running")
  lspio.close(c.io)
  os.remove(path)
end

function TestLspClient:testEditSync()
  -- lines: "hello" / CJK x3 / "abc"; edit at line 1 col 0 is byte 6
  local changes = {}
  local c, path = new_client(CODE, { "hello", "你好世界", "abc" })
  c:on("test/didChange", function(p) changes[#changes + 1] = p end)
  lu.assertTrue(drive(c, function() return c.state == "running" end))
  c:notify_edit(0, 0, "X")       -- line 0 col 0
  c:notify_edit(9, 0, "界")       -- after "你" (byte 3): utf16 col 1
  lu.assertTrue(drive(c, function() return #changes == 2 end), "changes")
  lu.assertEquals(changes[1].textDocument.version, 2)
  lu.assertEquals(changes[1].contentChanges[1].text, "X")
  lu.assertEquals(changes[1].contentChanges[1].range.start.character, 0)
  lu.assertEquals(changes[2].textDocument.version, 3)
  lu.assertEquals(changes[2].contentChanges[1].text, "界")
  lu.assertEquals(changes[2].contentChanges[1].range.start.line, 1)
  lu.assertEquals(changes[2].contentChanges[1].range.start.character, 1)
  lu.assertEquals(changes[2].contentChanges[1].range["end"].character, 1)
  lspio.close(c.io)
  os.remove(path)
end

function TestLspClient:testEmojiUtf16()
  -- "a😀b": emoji is 4 UTF-8 bytes = 2 UTF-16 units; byte 1 (after "a")
  -- -> col 1, byte 5 (after emoji) -> col 3
  local changes = {}
  local c, path = new_client(CODE, { "a😀b" })
  c:on("test/didChange", function(p) changes[#changes + 1] = p end)
  lu.assertTrue(drive(c, function() return c.state == "running" end))
  c:notify_edit(1, 0, "x")
  c:notify_edit(5, 0, "y")
  lu.assertTrue(drive(c, function() return #changes == 2 end), "changes")
  lu.assertEquals(changes[1].contentChanges[1].range.start.character, 1)
  lu.assertEquals(changes[2].contentChanges[1].range.start.character, 3)
  lspio.close(c.io)
  os.remove(path)
end

function TestLspClient:testDiagPush()
  local got
  local c, path = new_client(CODE_DIAG, { "hello" })
  c:on("textDocument/publishDiagnostics", function(p) got = p end)
  lu.assertTrue(drive(c, function() return got ~= nil end), "diag push")
  lu.assertEquals(got.uri, "file:///t.lua")
  lu.assertEquals(got.diagnostics[1].message, "oops")
  lspio.close(c.io)
  os.remove(path)
end

function TestLspClient:testShutdownExit()
  local c, path = new_client(CODE, { "x" })
  lu.assertTrue(drive(c, function() return c.state == "running" end))
  c:stop()
  lu.assertTrue(drive(c, function() return c.state == "exited" end), "exited")
  lspio.close(c.io)
  os.remove(path)
end

os.exit(lu.LuaUnit.run(), true)
