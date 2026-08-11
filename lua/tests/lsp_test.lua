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

-- write a fake LSP server script; reads frames, replies to requests.
-- The preamble exposes readmsg()/sendmsg()/y (JSON null) to the code.
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

-- drive: pump until pred() is true or the frame budget runs out.
-- pump is a zero-arg function (or an object with a poll() method).
local function drive(pump, pred, frames)
  frames = frames or 200
  for _ = 1, frames do
    if type(pump) == "function" then pump() else pump:poll() end
    if pred() then return true end
    os.execute("sleep 0.01")
  end
  return false
end

-- drive an IO handle: pump + drain decoded frames through a persistent
-- decoder (split frames survive) until want(msg) matches
--- @param h lsp.IO
--- @param want fun(msg: table): boolean
--- @param frames integer?
--- @return table?
local function io_drive(h, want, frames)
  local d = lsp.RPC.decoder(h.reader)
  local got
  drive(function()
    lsp.IO.pump(h)
    while true do
      local m = d:read()
      if m then
        if want(m) then got = m; return end
      else
        return
      end
    end
  end, function() return got ~= nil end, frames)
  return got
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
  local got = assert(io_drive(h, function(m) return m.id == 1 end),
    "echo response")
  lu.assertEquals(got.result, "hi!")
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
  local got = assert(io_drive(h, function(m) return m.id == 1 end, 300),
    "response")
  lu.assertEquals(got.result, 42)
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
  lu.assertNotNil(io_drive(h, function(m) return m.id == 1 end),
    "response")
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

TestProto = {}

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

-- protocol loop variant: ask workspace/configuration after initialized,
-- echo the client's answer back as test/resp
local CODE_CFG = [[
while true do
  local m = readmsg()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id,
        result = { capabilities = { textDocumentSync = { change = 2 } } } })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = y.null })
    elseif not m.method then
      sendmsg({ jsonrpc = "2.0", method = "test/resp", params = m })
    end
  elseif m.method == "initialized" then
    sendmsg({ jsonrpc = "2.0", id = 100, method = "workspace/configuration",
      params = { items = { { section = "Lua" }, { section = "other" } } } })
  elseif m.method == "exit" then
    os.exit(0)
  end
end
]]

-- fake document: array of lines; byte offsets computed against
-- table.concat(lines, "\n"); cfg overrides the default config table
local function new_client(code, lines, cfg)
  local path = fake_server(code)
  local c = lsp.Protocol.new({
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
    config = cfg,
  })
  c:start({ "lua", path }, "file:///t.lua", "lua")
  return c, path
end

function TestProto:testHandshakeDidOpen()
  local got
  local c, path = new_client(CODE, { "hello", "world" })
  c:on("test/didOpen", function(p) got = p end)
  lu.assertTrue(drive(c, function() return got ~= nil end), "didOpen echo")
  lu.assertEquals(c.state, "running")
  lu.assertEquals(got.text, "hello\nworld")
  lu.assertEquals(got.uri, "file:///t.lua")
  lsp.IO.close(c.io)
  os.remove(path)
end

function TestProto:testHelloIgnored()
  -- sumneko sends $/hello before the initialize response; the client
  -- must ignore it and still complete the handshake
  local got
  local c, path = new_client([[
sendmsg({ jsonrpc = "2.0", method = "$/hello", params = { "world" } })
]] .. CODE, { "x" })
  c:on("test/didOpen", function(p) got = p end)
  lu.assertTrue(drive(c, function() return got ~= nil end), "didOpen echo")
  lu.assertEquals(c.state, "running")
  lsp.IO.close(c.io)
  os.remove(path)
end

function TestProto:testEditSync()
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
  lsp.IO.close(c.io)
  os.remove(path)
end

function TestProto:testEmojiUtf16()
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
  lsp.IO.close(c.io)
  os.remove(path)
end

function TestProto:testDiagPush()
  local got
  local c, path = new_client(CODE_DIAG, { "hello" })
  c:on("textDocument/publishDiagnostics", function(p) got = p end)
  lu.assertTrue(drive(c, function() return got ~= nil end), "diag push")
  lu.assertEquals(got.uri, "file:///t.lua")
  lu.assertEquals(got.diagnostics[1].message, "oops")
  lsp.IO.close(c.io)
  os.remove(path)
end

function TestProto:testSyncFull()
  -- undo/redo path: one full didChange (no range) replaces the doc
  local changes = {}
  local c, path = new_client(CODE, { "hello", "world" })
  c:on("test/didChange", function(p) changes[#changes + 1] = p end)
  lu.assertTrue(drive(c, function() return c.state == "running" end))
  c:sync_full()
  lu.assertTrue(drive(c, function() return #changes == 1 end), "sync")
  lu.assertEquals(changes[1].textDocument.version, 2)
  lu.assertEquals(changes[1].contentChanges[1].text, "hello\nworld")
  lu.assertNil(changes[1].contentChanges[1].range, "full replace, no range")
  lsp.IO.close(c.io)
  os.remove(path)
end

function TestProto:testServerRequest()
  -- server->client request (workspace/configuration): client answers
  -- via on_server, nil results encode as JSON null
  local got, resp
  local c, path = new_client(CODE_CFG, { "x" })
  c:on_server("workspace/configuration", function(p)
    got = p
    return { { hint = { enable = true } }, yyjson.null }
  end)
  c:on("test/resp", function(p) resp = p end)
  lu.assertTrue(drive(c, function() return resp ~= nil end), "answered")
  lu.assertEquals(got.items[1].section, "Lua")
  lu.assertEquals(resp.result[1].hint.enable, true)
  lu.assertEquals(resp.result[2], yyjson.null, "null preserved")
  lsp.IO.close(c.io)
  os.remove(path)
end

function TestProto:testConfigAnswer()
  -- built-in workspace/configuration: Lua section from opts.config
  -- (hints on), unknown sections answered as JSON null
  local resp
  local c, path = new_client(CODE_CFG, { "x" })
  c:on("test/resp", function(p) resp = p end)
  lu.assertTrue(drive(c, function() return resp ~= nil end), "answered")
  lu.assertEquals(resp.result[1].hint.enable, true)
  lu.assertEquals(resp.result[1].hint.setType, true)
  lu.assertEquals(resp.result[2], yyjson.null, "unknown section null")
  lsp.IO.close(c.io)
  os.remove(path)
end

function TestProto:testConfigOverrideEmpty()
  -- empty config table overrides the default: every section answers null
  local resp
  local c, path = new_client(CODE_CFG, { "x" }, {})
  c:on("test/resp", function(p) resp = p end)
  lu.assertTrue(drive(c, function() return resp ~= nil end), "answered")
  lu.assertEquals(resp.result[1], yyjson.null, "Lua section null")
  lu.assertEquals(resp.result[2], yyjson.null, "unknown section null")
  lsp.IO.close(c.io)
  os.remove(path)
end

function TestProto:testShutdownExit()
  local c, path = new_client(CODE, { "x" })
  lu.assertTrue(drive(c, function() return c.state == "running" end))
  c:stop()
  lu.assertTrue(drive(c, function() return c.state == "exited" end), "exited")
  lsp.IO.close(c.io)
  os.remove(path)
end

TestClient = {}

-- fake protocol: records requests (+ callbacks for manual responses) and
-- lifecycle calls; state/version simulate a running server
local function fake_proto()
  local p = {
    state = "running", version = 3,
    capabilities = { inlayHintProvider = true },
    reqs = {}, cbs = {}, handlers = {},
    edits = nil, synced = false,
  }
  p.request = function(self, method, params, cb)
    self.reqs[#self.reqs + 1] = method
    self.cbs[#self.cbs + 1] = cb
  end
  p.notify_edit = function(self, off, del, s) self.edits = { off, del, s } end
  p.sync_full = function(self) self.synced = true end
  p.poll = function(self) end
  p.stop = function(self) end
  p.on = function(self, method, fn) self.handlers[method] = fn end
  p.start = function(self, argv, uri) self.uri = uri; return true end
  return p
end

-- client with a fake proto/doc/clock/vtext; `over` overrides defaults
-- (lines replaces the doc table, nonow drops the injected clock),
-- setclock(n) advances the injected wall clock
local function mk_client(over)
  over = over or {}
  local proto = fake_proto()
  local got = {}
  local clock = over.clock or 10
  local lines = over.lines or { "hello", "world" }
  local c = lsp.Client.new({
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
    dcol_fn = function(line, bcol) return bcol end, -- ASCII: dcol == bytecol
    viewport_fn = function() return { top = over.top or 0, rows = over.rows or 5 } end,
    now_fn = over.nonow and nil or function() return clock end,
    attrmap = { comment = "c", diag = "d" },
    vtext = {
      set = function(line, list) got[#got + 1] = { line, list } end,
      clear = function() got[#got + 1] = { "clear" } end,
    },
    proto = proto,
  })
  return c, proto, got, function(n) clock = n end
end

function TestClient:testTickIdleRequestsHints()
  local c, proto = mk_client()
  c:tick()
  lu.assertEquals(#proto.reqs, 1)
  lu.assertEquals(proto.reqs[1], "textDocument/inlayHint")
end

function TestClient:testTickSkipsWhenTyping()
  local c, proto = mk_client()
  c.last_edit_t = 9.5 -- 0.5s since edit < hint_idle (1.0)
  c.hint_view = 0     -- viewport unchanged
  c:tick()
  lu.assertEquals(#proto.reqs, 0)
end

function TestClient:testTickViewportChange()
  local c, proto = mk_client()
  c.hint_dirty = false
  c.hint_view = 3 -- stale viewport top (now 0)
  c:tick()
  lu.assertEquals(#proto.reqs, 1)
end

function TestClient:testTickNullRetryBudget()
  local c, proto, _, setclock = mk_client()
  c:tick()
  proto.cbs[#proto.cbs](nil) -- server not ready -> null
  for i = 1, 8 do
    setclock(10 + 2 * i)
    c:tick()
    lu.assertEquals(#proto.reqs, 1 + i, "retry " .. i)
    proto.cbs[#proto.cbs](nil)
  end
  setclock(30)
  c:tick()
  lu.assertEquals(#proto.reqs, 9, "silent after the retry budget")
end

function TestClient:testStaleResponseDropped()
  local c, proto, got = mk_client()
  c:tick()
  proto.version = 4 -- edited while in flight
  proto.cbs[1]({ { position = { line = 0, character = 3 }, label = "int" } })
  lu.assertEquals(#got, 0, "stale response must not write vtext")
end

function TestClient:testResponseWritesVtext()
  local c, proto, got = mk_client()
  c:tick()
  proto.cbs[1]({ { position = { line = 0, character = 3 }, label = "int" } })
  lu.assertEquals(#got, 1)
  lu.assertEquals(got[1][1], 0)
  lu.assertEquals(got[1][2][1].dcol, 3)
  lu.assertEquals(got[1][2][1].text, "int")
  -- next response on another line clears the stale slot
  c.hint_dirty = true
  c:tick()
  proto.cbs[2]({ { position = { line = 1, character = 2 }, label = "x" } })
  lu.assertEquals(#got, 3)
  lu.assertEquals(got[2][1], 1)
  lu.assertEquals(got[3][1], 0)
  lu.assertNil(got[3][2], "old line cleared")
end

function TestClient:testPostRenderSemanticPull()
  local c, proto = mk_client()
  proto.capabilities.semanticTokensProvider = { full = true }
  c:post_render()
  lu.assertEquals(#proto.reqs, 1)
  lu.assertEquals(proto.reqs[1], "textDocument/semanticTokens/full")
  c:post_render() -- already pending: no second request
  lu.assertEquals(#proto.reqs, 1)
end

function TestClient:testSemanticDecodeAscii()
  local c, proto = mk_client()
  proto.capabilities.semanticTokensProvider = { full = true,
    legend = { tokenTypes = { "comment" } } }
  c:post_render()
  -- tokens (5 ints each: dline/dunit/len/ttype/mod): line 0 unit 3
  -- len 2; line 1 unit 0 len 1; unknown ttype idx 1 must be skipped
  proto.cbs[1]({ data = { 0, 3, 2, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0 } })
  lu.assertEquals(#c.sem.spans, 2)
  lu.assertEquals(c.sem.spans[1].offset, 3)
  lu.assertEquals(c.sem.spans[1].length, 2)
  lu.assertEquals(c.sem.spans[1].attr, "c")
  lu.assertEquals(c.sem.spans[2].offset, 6) -- line 1 starts at byte 6
  lu.assertEquals(c.sem.spans[2].length, 1)
  lu.assertFalse(c.sem.dirty, "decode clears dirty")
  c:post_render() -- not dirty: no new request
  lu.assertEquals(#proto.reqs, 1)
end

function TestClient:testSemanticDecodeCjkUtf16()
  -- "你好a😀": CJK = 1 unit / 3 bytes, emoji = 2 units / 4 bytes
  local c, proto = mk_client({ lines = { "你好a😀", "x" } })
  proto.capabilities.semanticTokensProvider = { full = true,
    legend = { tokenTypes = { "comment" } } }
  c:post_render()
  -- tokens (5 ints each): unit 2 len 1 ("a", bytes 6-7); unit 3 len 2
  -- (emoji, bytes 7-11); line 1 unit 0 len 1
  proto.cbs[1]({ data = { 0, 2, 1, 0, 0, 0, 1, 2, 0, 0, 1, 0, 1, 0, 0 } })
  lu.assertEquals(#c.sem.spans, 3)
  lu.assertEquals(c.sem.spans[1].offset, 6)
  lu.assertEquals(c.sem.spans[1].length, 1)
  lu.assertEquals(c.sem.spans[2].offset, 7)
  lu.assertEquals(c.sem.spans[2].length, 4)
  lu.assertEquals(c.sem.spans[3].offset, 12) -- line 1 starts at byte 12
  lu.assertEquals(c.sem.spans[3].length, 1)
end

function TestClient:testOnEditMarksDirty()
  local c, proto = mk_client()
  c.sem.dirty, c.hint_dirty = false, false
  c:on_edit(3, 2, "xy")
  lu.assertEquals(proto.edits[1], 3)
  lu.assertEquals(proto.edits[2], 2)
  lu.assertEquals(proto.edits[3], "xy")
  lu.assertTrue(c.sem.dirty)
  lu.assertTrue(c.hint_dirty)
end

function TestClient:testResyncClears()
  local c, proto, got = mk_client()
  c:resync()
  lu.assertTrue(proto.synced)
  lu.assertEquals(#got, 1)
  lu.assertEquals(got[1][1], "clear")
  lu.assertTrue(c.sem.dirty)
  lu.assertTrue(c.hint_dirty)
end

function TestClient:testQuerySpansClip()
  local c = mk_client()
  c.sem.spans = {
    { offset = 0, length = 5, attr = "a" },
    { offset = 10, length = 5, attr = "b" },
  }
  c.diag = { version = 1, spans = { { offset = 2, length = 3, attr = "d" } } }
  local q = c:query_spans(1, 6)
  lu.assertEquals(#q.sem, 1)
  lu.assertEquals(q.sem[1].offset, 0)
  lu.assertEquals(#q.diag, 1)
  lu.assertEquals(q.diag[1].offset, 2)
  local q2 = c:query_spans(7, 20)
  lu.assertEquals(#q2.sem, 1)
  lu.assertEquals(q2.sem[1].offset, 10)
  lu.assertEquals(#q2.diag, 0)
end

function TestClient:testDiagAt()
  local c = mk_client()
  c.diag = { spans = {
    { offset = 1, length = 5, severity = 2, msg = "warn" },
    { offset = 2, length = 3, severity = 1, msg = "error" },
  } }
  lu.assertEquals(c:diag_at(3).msg, "error") -- lowest severity wins
  lu.assertNil(c:diag_at(0))
  c.diag = nil
  lu.assertNil(c:diag_at(3))
end

function TestClient:testDiagDecodeCjkAndVersionGuard()
  local c, proto = mk_client({ lines = { "你好", "x" } })
  lu.assertTrue(c:start({ "lua" }, "file:///t.lua", "lua"))
  local handler = proto.handlers["textDocument/publishDiagnostics"]
  lu.assertNotNil(handler)
  -- CJK: chars 0-2 span 6 bytes; line 1 chars 0-1 span 1 byte (off 8)
  handler({ uri = "file:///t.lua", version = 2,
    diagnostics = {
      { range = { start = { line = 0, character = 0 },
        ["end"] = { line = 0, character = 2 } }, message = "boom",
        severity = 1 },
      { range = { start = { line = 1, character = 0 },
        ["end"] = { line = 1, character = 1 } }, message = "x" },
    } })
  lu.assertEquals(c.diag.version, 2)
  lu.assertEquals(#c.diag.spans, 2)
  lu.assertEquals(c.diag.spans[1].offset, 0)
  lu.assertEquals(c.diag.spans[1].length, 6)
  lu.assertEquals(c.diag.spans[1].msg, "boom")
  lu.assertEquals(c.diag.spans[2].offset, 7) -- line 1 starts at byte 7
  -- stale snapshot (older version) dropped
  handler({ uri = "file:///t.lua", version = 1,
    diagnostics = { { range = { start = { line = 0, character = 0 },
      ["end"] = { line = 0, character = 1 } }, message = "stale" } } })
  lu.assertEquals(#c.diag.spans, 2)
  lu.assertEquals(c.diag.spans[1].msg, "boom")
end

function TestClient:testStartFailureClears()
  local c, proto, got = mk_client()
  proto.start = function() return false end
  local ok = c:start({ "lua" }, "file:///t.lua", "lua")
  lu.assertFalse(ok)
  lu.assertNil(c.proto)
  lu.assertEquals(#got, 1)
  lu.assertEquals(got[1][1], "clear")
end

function TestClient:testTickWithoutNowFn()
  -- no now_fn injected: Client falls back to luv.hrtime()/1e9
  local c, proto = mk_client({ nonow = true })
  c:tick()
  lu.assertEquals(#proto.reqs, 1)
end

function TestClient:testRestartResetsHints()
  local c, proto, got = mk_client()
  c:tick()
  proto.cbs[1]({ { position = { line = 0, character = 3 }, label = "int" } })
  lu.assertEquals(#got, 1)
  c:stop()
  lu.assertEquals(#got, 2)
  lu.assertEquals(got[2][1], "clear")
  lu.assertTrue(c:start({ "lua" }, "file:///t.lua", "lua"))
  c:tick()
  -- response on a different line: no stale clear of line 0 from the old run
  proto.cbs[#proto.cbs]({ { position = { line = 1, character = 2 }, label = "x" } })
  lu.assertEquals(#got, 3, "restart resets hint scheduling")
  lu.assertEquals(got[3][1], 1)
  lu.assertEquals(got[3][2][1].text, "x")
end

os.exit(lu.LuaUnit.run(), true)
