-- lspclient: LSP client core — lifecycle state machine, pending requests,
-- notification dispatch, incremental text sync.
-- Editor-agnostic: transport (lspio) spawns the server process; document
-- accessors are injected via opts (editor wires the real doc, tests use
-- tables). Every edit is synced immediately (one didChange per docedit),
-- measured against the state the server already has — no merge/rebase.
local jsonrpc = require("jsonrpc")
local lspio = require("lspio")
local yyjson = require("yyjson")

--- @class lspclient.Client
--- @field state string  starting|running|shutting|exited
--- @field io table?
--- @field dec table
--- @field pending table
--- @field handlers table
--- @field server_handlers table  server->client requests
--- @field version integer
--- @field uri string
--- @field langid string
--- @field capabilities table
--- @field opts table
local lspclient = {}

local M = { __index = lspclient }

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
function lspclient.text_byte_to_utf16(text, byte)
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
--- @param opts table
--- @return lspclient.Client
function lspclient.new(opts)
  return setmetatable({
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
  }, M)
end

-- ---- internals (must precede the methods that call them: local
-- functions are only visible after their definition)

-- UTF-16 unit column at (line, bytecol) of the current doc.
--- @param line integer
--- @param bytecol integer
--- @return integer
local function _utf16(self, line, bytecol)
  return lspclient.text_byte_to_utf16(self.opts.get_line(line), bytecol)
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
        encoded = jsonrpc.enc_error(msg.id, -32603, err)
      else
        encoded = jsonrpc.enc_result(msg.id, result == nil and yyjson.null
          or result)
      end
      lspio.send(self.io, encoded)
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
function lspclient:on(method, fn)
  self.handlers[method] = fn
end

-- Register a server->client request handler (workspace/configuration);
-- fn(params) returns result (or result, err-string for a protocol error).
--- @param method string
--- @param fn fun(params: table?): any?, string?
function lspclient:on_server(method, fn)
  self.server_handlers[method] = fn
end

-- Spawn the server process and start the initialize handshake.
--- @param argv string[]
--- @param uri string  document uri (didOpen)
--- @param langid string
--- @param root string?  workspace root uri, defaults to uri
function lspclient:start(argv, uri, langid, root)
  self.uri, self.langid = uri, langid
  self.io, self.spawn_err = lspio.spawn(argv)
  if not self.io then
    self.state = "exited"
    self.opts.on_status("exited", self.spawn_err)
    return false
  end
  self.dec = jsonrpc.decoder(self.io.reader)
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
function lspclient:request(method, params, cb)
  self.reqid = self.reqid + 1
  self.pending[self.reqid] = { cb = cb }
  lspio.send(self.io, jsonrpc.enc_request(self.reqid, method, params))
  return self.reqid
end

-- Queue a notification (fire and forget).
--- @param method string
--- @param params table?
function lspclient:notify(method, params)
  lspio.send(self.io, jsonrpc.enc_notify(method, params))
end

-- Pump the transport and dispatch any complete messages.
function lspclient:poll()
  if not self.io then return end
  lspio.pump(self.io)
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
function lspclient:notify_edit(off, del, s)
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
function lspclient:sync_full()
  if self.state ~= "running" then return end
  self.version = self.version + 1
  self:notify("textDocument/didChange", {
    textDocument = { uri = self.uri, version = self.version },
    contentChanges = { { text = self.opts.get_text() } },
  })
end

-- Graceful shutdown: shutdown request, exit notification, then the
-- process ends itself (EOF detected on the next poll).
function lspclient:stop()
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

return lspclient
