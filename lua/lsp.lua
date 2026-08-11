-- lsp: LSP client building blocks — RPC framing + server process IO.
-- RPC encodes/decodes JSON-RPC 2.0 messages over Content-Length byte
-- frames; pure functions with an injected chunk reader, fully testable
-- offline. IO spawns the server process over luv pipes and pumps libuv
-- callbacks on demand so the editor main loop never restructures.
-- Protocol: editor-agnostic client core (state machine, sync, config).
local lsp = {}
local yyjson = require("yyjson")
local luv = require("luv")

local RPC = {}
local M = { __index = RPC }

-- JSON-RPC 2.0 message framing over byte streams (LSP transport).
-- Wire format: "Content-Length: <n>\r\n\r\n<body>" with body = JSON.
-- Pure functions + injected chunk reader -> fully testable offline.

-- Wrap a JSON body string in the LSP frame header.
--- @param body string
--- @return string
local function frame(body)
  return "Content-Length: " .. #body .. "\r\n\r\n" .. body
end

--- Encode a request frame.
--- @param id integer
--- @param method string
--- @param params any?
--- @return string
function RPC.enc_request(id, method, params)
  local msg = { jsonrpc = "2.0", id = id, method = method }
  if params ~= nil then msg.params = params end
  return frame(yyjson.encode(msg))
end

--- Encode a notification frame.
--- @param method string
--- @param params any?
--- @return string
function RPC.enc_notify(method, params)
  local msg = { jsonrpc = "2.0", method = method }
  if params ~= nil then msg.params = params end
  return frame(yyjson.encode(msg))
end

--- Encode a success response frame.
--- @param id integer
--- @param result any
--- @return string
function RPC.enc_result(id, result)
  return frame(yyjson.encode({ jsonrpc = "2.0", id = id, result = result }))
end

--- Encode an error response frame.
--- @param id integer
--- @param code integer
--- @param message string
--- @return string
function RPC.enc_error(id, code, message)
  return frame(yyjson.encode({
    jsonrpc = "2.0", id = id,
    error = { code = code, message = message },
  }))
end

-- Parse "Content-Length: N" out of the header block.
--- @param header string
--- @return integer?
local function header_length(header)
  for line in header:gmatch("[^\r\n]+") do
    local name, value = line:match("^([^:]+):%s*(.*)$")
    if name and name:lower() == "content-length" then
      return tonumber(value)
    end
  end
end

-- Create a persistent frame decoder over a chunk reader. The decoder
-- keeps partial-frame state across calls, so split/segmented frames
-- survive "again" pauses. Reader protocol:
--   chunk (non-empty string)  -> next bytes
--   "" (empty string)         -> no data available yet (pause)
--   nil                       -> end of stream (EOF)
-- decoder:read() returns msg, or nil + err ("eof"/"again"/message).
-- Message shape: {method=, params=, id=} (request/notification) or
-- {result=, error=, id=} (response).
--- @param readchunk fun(): string?
--- @return table decoder
function RPC.decoder(readchunk)
  local state = { data = "", body_start = nil, body_len = nil }
  local function fill()
    local chunk = readchunk()
    if chunk == nil then return "eof" end
    if #chunk == 0 then return "again" end
    state.data = state.data .. chunk
    return "ok"
  end
  local function read()
    while true do
      if not state.body_len then
        local hdr_end = state.data:find("\r\n\r\n", 1, true)
        if hdr_end then
          local header = state.data:sub(1, hdr_end - 1)
          state.body_start = hdr_end + 4
          state.body_len = header_length(header)
          if not state.body_len then return nil, "missing Content-Length" end
        else
          local st = fill()
          if st ~= "ok" then return nil, st end
        end
      end
      if state.body_len then
        local body_end = state.body_start + state.body_len - 1
        while #state.data < body_end do
          local st = fill()
          if st ~= "ok" then return nil, st end
        end
        local body = state.data:sub(state.body_start, body_end)
        -- drop consumed bytes; keep any trailing (next frame) data
        state.data = state.data:sub(body_end + 1)
        state.body_start, state.body_len = nil, nil
        local msg, err = yyjson.decode(body)
        if msg == nil then return nil, "bad JSON: " .. err end
        return msg
      end
    end
  end
  return { read = read }
end

lsp.RPC = RPC

-- lspio: LSP server process bridge over luv (libuv binding).
-- Owns spawn + stdio pipes; pumps libuv callbacks on demand so the
-- editor main loop (blocking termfeed getkey) never restructures:
-- each frame: lspio.pump(h) then drain messages via jsonrpc.decoder.

--- @class lsp.IO
--- @field exit boolean  process exited
--- @field exit_code integer
--- @field proc table
--- @field err string?  spawn error
--- @field stdin table
--- @field stdout table
--- @field buf table
--- @field outqueue table
local IO = {}
local IM = { __index = IO }

-- argv minus the executable (luv args exclude it; plain loop keeps
-- 5.1/LuaJIT compat where table.unpack does not exist)
local function args_without_exec(argv)
  local args = {}
  for i = 2, #argv do
    args[#args + 1] = argv[i]
  end
  return args
end

-- Spawn a server process; argv[1] = executable. Stderr inherited.
--- @param argv string[]
--- @return lsp.IO?
--- @return string?  err (executable not found, etc.)
function IO.spawn(argv)
  --- @type lsp.IO
  local h = setmetatable({
    stdin = luv.new_pipe(false),
    stdout = luv.new_pipe(false),
    buf = {},
    outqueue = {},
    exit = false,
    exit_code = 0,
    reader = nil,
  }, IM)
  h.proc, h.err = luv.spawn(argv[1], {
    args = args_without_exec(argv),
    stdio = { h.stdin, h.stdout, nil },
  }, function(code)
    h.exit = true
    h.exit_code = code
  end)
  if not h.proc then return nil, h.err end
  luv.read_start(h.stdout, function(err, data)
    if data then
      h.buf[#h.buf + 1] = data
    elseif err then
      h.exit = true
    end
  end)
  h.reader = IO.reader(h)
  return h
end

-- Persistent chunk reader: "" = no data yet,
-- nil = process exited. Index advances across pauses, so the reader
-- survives "again" retries (frames are consumed incrementally).
--- @param h lsp.IO
--- @return fun(): string?
function IO.reader(h)
  local i = 1
  return function()
    if i <= #h.buf then
      local chunk = h.buf[i]
      i = i + 1
      return chunk
    end
    if h.exit then return nil end
    return ""
  end
end

-- Flush the out queue; partial writes stay queued (retried on pump).
--- @param h lsp.IO
function IO.drain(h)
  while #h.outqueue > 0 do
    local n = luv.try_write(h.stdin, h.outqueue[1])
    if type(n) ~= "number" then return end -- EAGAIN: retry next pump
    if n >= #h.outqueue[1] then
      table.remove(h.outqueue, 1)
    else
      h.outqueue[1] = h.outqueue[1]:sub(n + 1)
    end
  end
end

-- Queue raw bytes for the server (written on next pump).
--- @param h lsp.IO
--- @param bytes string
function IO.send(h, bytes)
  h.outqueue[#h.outqueue + 1] = bytes
end

-- Pump libuv callbacks once (non-blocking); process pending writes.
--- @param h lsp.IO
function IO.pump(h)
  luv.run("nowait")
  IO.drain(h)
end

-- Terminate the server process.
--- @param h lsp.IO
function IO.close(h)
  if not h.exit and h.proc then
    luv.process_kill(h.proc)
  end
  h.stdin:close()
  luv.read_stop(h.stdout)
  h.stdout:close()
end

lsp.IO = IO

-- lspclient: LSP client core — lifecycle state machine, pending requests,
-- notification dispatch, incremental text sync.
-- Editor-agnostic: transport (lspio) spawns the server process; document
-- accessors are injected via opts (editor wires the real doc, tests use
-- tables). Every edit is synced immediately (one didChange per docedit),
-- measured against the state the server already has — no merge/rebase.

--- @class lsp.Protocol
--- @field state string  starting|running|shutting|exited
--- @field io lsp.IO?
--- @field dec table
--- @field pending table
--- @field handlers table
--- @field server_handlers table  server->client requests
--- @field version integer
--- @field uri string
--- @field langid string
--- @field capabilities table
--- @field opts table
local Protocol = {}
local PM = { __index = Protocol }

-- UTF-8 char length from its lead byte (continuation bytes skipped).
--- @param b integer
--- @return integer
local function charlen(b)
  if b < 0xc0 then return 1 end
  if b < 0xe0 then return 2 end
  if b < 0xf0 then return 3 end
  return 4
end

-- Byte offset within a UTF-8 line -> UTF-16 code unit column.
-- BMP chars (incl. CJK) are 1 unit; only 4-byte supplementary-plane
-- chars (emoji) are 2. Counting lead bytes needs no decode.
-- TODO(C): column math family candidate (cellgrid), same as editor's
-- text_byte_to_dcol — promote both together after bench evidence.
--- @param text string
--- @param byte integer
--- @return integer
function Protocol.text_byte_to_utf16(text, byte)
  local units = 0
  local i = 1
  local blen = math.min(byte, #text)
  while i <= blen do
    local n = charlen(text:byte(i))
    units = units + (n == 4 and 2 or 1)
    i = i + n
  end
  return units
end

-- Client factory. opts:
--   get_text() -> full doc text (didOpen)
--   get_line(lnum) -> line text without "\n" (UTF-16 conversion)
--   offset_pos(off) -> line, bytecol of a byte offset
--   on_status(state, msg?) -> state change callback
--   config? -> per-section config table (default: Lua hints on)
--- @param opts table
--- @return lsp.Protocol
function Protocol.new(opts)
  opts.config = opts.config
    or { Lua = { hint = { enable = true, setType = true } } }
  local self = setmetatable({
    state = "starting",
    io = nil,
    dec = nil,
    pending = {},
    handlers = {},
    server_handlers = {},
    version = 0,
    uri = nil,
    langid = nil,
    capabilities = {},
    reqid = 0,
    opts = opts,
  }, PM)
  -- answer workspace/configuration (LuaLS reads Lua settings via it);
  -- unknown sections are JSON null, on_server can override
  self:on_server("workspace/configuration", function(params)
    local out = {}
    for _, item in ipairs(params and params.items or {}) do
      local cfg = (self.opts.config or {})[item.section]
      out[#out + 1] = cfg or yyjson.null
    end
    return out
  end)
  return self
end

-- ---- internals (must precede the methods that call them: local
-- functions are only visible after their definition)

-- UTF-16 unit column at (line, bytecol) of the current doc.
--- @param line integer
--- @param bytecol integer
--- @return integer
local function _utf16(self, line, bytecol)
  return Protocol.text_byte_to_utf16(self.opts.get_line(line), bytecol)
end

-- Route a message: response -> pending callback, server request ->
-- server handler (answered with a result), notification -> handler.
-- JSON null results (e.g. sumneko inlayHint) surface as nil.
--- @param msg table
local function _on_msg(self, msg)
  if msg.id ~= nil and msg.method then
    local fn = self.server_handlers[msg.method]
    if fn then
      local result, err = fn(msg.params)
      local encoded
      if err then
        encoded = RPC.enc_error(msg.id, -32603, err)
      else
        encoded = RPC.enc_result(msg.id, result == nil and yyjson.null
          or result)
      end
      IO.send(self.io, encoded)
    end
  elseif msg.id ~= nil then
    local entry = self.pending[msg.id]
    if entry then
      self.pending[msg.id] = nil
      local result = msg.result
      if result == yyjson.null then result = nil end
      entry.cb(result, msg.error)
    end
  elseif msg.method then
    local fn = self.handlers[msg.method]
    if fn then fn(msg.params) end
  end
end

-- Transition to exited (EOF, protocol error, or user stop mid-start).
--- @param why string
local function _fail(self, why)
  if self.state == "exited" then return end
  self.state = "exited"
  self.opts.on_status("exited", why)
end

-- Full-document sync right after the handshake (version starts at 1).
local function _send_did_open(self)
  self.version = 1
  self:notify("textDocument/didOpen", {
    textDocument = { uri = self.uri, languageId = self.langid,
      version = self.version, text = self.opts.get_text() },
  })
end

-- Register a notification handler (publishDiagnostics, test echoes).
--- @param method string
--- @param fn fun(params: table?)
function Protocol:on(method, fn)
  self.handlers[method] = fn
end

-- Register a server->client request handler (workspace/configuration);
-- fn(params) returns result (or result, err-string for a protocol error).
--- @param method string
--- @param fn fun(params: table?): any?, string?
function Protocol:on_server(method, fn)
  self.server_handlers[method] = fn
end

-- Spawn the server process and start the initialize handshake.
--- @param argv string[]
--- @param uri string  document uri (didOpen)
--- @param langid string
--- @param root string?  workspace root uri, defaults to uri
function Protocol:start(argv, uri, langid, root)
  self.uri, self.langid = uri, langid
  self.io, self.spawn_err = IO.spawn(argv)
  if not self.io then
    self.state = "exited"
    self.opts.on_status("exited", self.spawn_err)
    return false
  end
  self.dec = RPC.decoder(self.io.reader)
  self.state = "starting"
  self.opts.on_status("starting")
  local root_uri = root or uri
  -- workspaceFolders required: without it sumneko falls back to an
  -- unmanaged workspace and never pushes diagnostics for opened docs
  self:request("initialize", {
    rootUri = root_uri,
    workspaceFolders = { { uri = root_uri, name = "lsp" } },
    capabilities = {
      -- answer workspace/configuration (LuaLS reads Lua settings via it)
      workspace = { configuration = true },
    },
  }, function(result, err)
      if err then
        _fail(self, "initialize: " .. tostring(err.message))
        return
      end
      self.capabilities = result and result.capabilities or {}
      self.state = "running"
      self.opts.on_status("running")
      self:notify("initialized", {})
      _send_did_open(self)
    end)
  return true
end

-- Queue a request; cb(result, error) fires on the matching response.
--- @param method string
--- @param params table?
--- @param cb fun(result: any?, error: table?)
--- @return integer
function Protocol:request(method, params, cb)
  self.reqid = self.reqid + 1
  self.pending[self.reqid] = { cb = cb }
  IO.send(self.io, RPC.enc_request(self.reqid, method, params))
  return self.reqid
end

-- Queue a notification (fire and forget).
--- @param method string
--- @param params table?
function Protocol:notify(method, params)
  IO.send(self.io, RPC.enc_notify(method, params))
end

-- Pump the transport and dispatch any complete messages.
function Protocol:poll()
  if not self.io then return end
  IO.pump(self.io)
  while true do
    local msg, err = self.dec:read()
    if msg then
      _on_msg(self, msg)
    elseif err == "again" then
      break
    else
      _fail(self, err)
      break
    end
  end
end

-- Record an edit at byte offset `off` (measured BEFORE the edit) and
-- sync it immediately: the range is measured against the state the
-- server already has, so no merge/rebase is ever needed.
--- @param off integer
--- @param del integer
--- @param s string
function Protocol:notify_edit(off, del, s)
  if self.state ~= "running" then return end
  local sl, sc = self.opts.offset_pos(off)
  local el, ec = self.opts.offset_pos(off + del)
  self.version = self.version + 1
  self:notify("textDocument/didChange", {
    textDocument = { uri = self.uri, version = self.version },
    contentChanges = { {
      range = {
        start = { line = sl, character = _utf16(self, sl, sc) },
        ["end"] = { line = el, character = _utf16(self, el, ec) },
      },
      text = s,
    } },
  })
end

-- Resync the whole document after jumps the client cannot localize
-- (undo/redo): one full didChange (no range = whole-document replace).
function Protocol:sync_full()
  if self.state ~= "running" then return end
  self.version = self.version + 1
  self:notify("textDocument/didChange", {
    textDocument = { uri = self.uri, version = self.version },
    contentChanges = { { text = self.opts.get_text() } },
  })
end

-- Graceful shutdown: shutdown request, exit notification, then the
-- process ends itself (EOF detected on the next poll).
function Protocol:stop()
  if self.state ~= "running" then
    if self.state == "starting" then _fail(self, "stopped") end
    return
  end
  self.state = "shutting"
  self.opts.on_status("shutting")
  self:request("shutdown", {}, function()
    if self.state ~= "shutting" then return end
    self:notify("exit", {})
  end)
end

lsp.Protocol = Protocol

return lsp
