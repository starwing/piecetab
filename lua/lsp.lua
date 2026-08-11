-- lsp: LSP client building blocks — RPC framing + server process IO.
-- RPC encodes/decodes JSON-RPC 2.0 messages over Content-Length byte
-- frames; pure functions with an injected chunk reader, fully testable
-- offline. IO spawns the server process over luv pipes and pumps libuv
-- callbacks on demand so the editor main loop never restructures.
-- Protocol: editor-agnostic client core (state machine, sync, config).
local lsp = {}
local json = require("json")
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
  return frame(json.encode(msg))
end

--- Encode a notification frame.
--- @param method string
--- @param params any?
--- @return string
function RPC.enc_notify(method, params)
  local msg = { jsonrpc = "2.0", method = method }
  if params ~= nil then msg.params = params end
  return frame(json.encode(msg))
end

--- Encode a success response frame.
--- @param id integer
--- @param result any
--- @return string
function RPC.enc_result(id, result)
  return frame(json.encode({ jsonrpc = "2.0", id = id, result = result }))
end

--- Encode an error response frame.
--- @param id integer
--- @param code integer
--- @param message string
--- @return string
function RPC.enc_error(id, code, message)
  return frame(json.encode({
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
      return math.floor(assert(tonumber(value)))
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
        local msg, err = json.decode(body)
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
      out[#out + 1] = cfg or json.null
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
        encoded = RPC.enc_result(msg.id, result == nil and json.null
          or result)
      end
      IO.send(self.io, encoded)
    end
  elseif msg.id ~= nil then
    local entry = self.pending[msg.id]
    if entry then
      self.pending[msg.id] = nil
      local result = msg.result
      if result == json.null then result = nil end
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

-- lspclient: LSP integration layer — the editor's only contact surface.
-- Owns protocol orchestration (hint scheduling, semantic/diag caching)
-- and writes decoded data into injected Ed vtext slots; it never holds
-- injected text itself (data lives on Ed, shifting is Ed's job) — only
-- scheduling state. The protocol object is injected via opts (tests use
-- fakes) or built from the same accessor quad.

--- @class lsp.Client
--- @field proto lsp.Protocol?
--- @field opts table
--- @field state string  proxy -> proto.state
--- @field uri string  proxy -> proto.uri
--- @field version integer  proxy -> proto.version
--- @field sem table  semantic cache {spans, dirty, pending}
--- @field diag table?  diagnostic cache {version, spans}
--- @field hint_dirty boolean
--- @field hint_pending boolean
--- @field hint_view integer?
--- @field hint_reqver integer
--- @field hint_retry number
--- @field hint_null integer
--- @field hint_lines table?  lines currently written to vtext slots
--- @field last_edit_t number
--- @field hint_idle number
local Client = {}

-- Methods resolve on the Client table; lifecycle fields proxy the proto
-- so the editor reads c.state/uri/version like a plain Protocol.
local CM = { __index = function(self, key)
  if key == "state" or key == "uri" or key == "version" then
    local p = self.proto
    return p and p[key] or nil
  end
  return Client[key]
end }

-- ---- internals (module-private; see Protocol section for charlen)

-- UTF-16 unit column -> byte offset within a UTF-8 line. BMP chars are
-- 1 unit; 4-byte supplementary chars (emoji) are 2; trailing units
-- clamp to the line end (LSP positions never split chars).
--- @param text string
--- @param units integer
--- @return integer
local function utf16_to_byte(text, units)
  local i = 1
  local blen = #text
  while i <= blen and units > 0 do
    local n = charlen(text:byte(i))
    units = units - (n == 4 and 2 or 1)
    i = i + n
  end
  return i - 1
end

-- Byte offset of each line start (line 0 = 0) from the full doc text.
-- find() returns the 1-based newline index, which is exactly the
-- 0-based offset of the following line.
--- @param text string
--- @return integer[]
local function line_offsets(text)
  local out, pos = { 0 }, 1
  while true do
    local nl = text:find("\n", pos, true)
    if not nl then break end
    out[#out + 1] = nl
    pos = nl + 1
  end
  return out
end

-- LSP position {line, UTF-16 unit} -> byte offset (line base + line pos).
--- @param self lsp.Client
--- @param starts integer[]
--- @param line integer
--- @param unit integer
--- @return integer
local function lsp_pos(self, starts, line, unit)
  local text = self.opts.get_line(line)
  return (starts[line + 1] or 0) + utf16_to_byte(text, unit)
end

-- Decode a semanticTokens/full response into {offset, length, attr}
-- spans in document order. Tokens are relative-encoded: deltaLine
-- accumulates; deltaStartChar is absolute when deltaLine > 0, else
-- relative to the previous token.
--- @param tokens integer[]
--- @param legend table  {tokenTypes = string[]}
--- @param attrmap table  tokenType name -> attr table (unknown: skip)
--- @param posfn fun(line: integer, unit: integer): integer
--- @return table
local function span_decode(tokens, legend, attrmap, posfn)
  local types = legend and legend.tokenTypes or {}
  local line, unit = 0, 0
  local out = {}
  local i = 1
  while i + 4 <= #tokens do
    local dline, dunit = tokens[i], tokens[i + 1]
    local len, ttype = tokens[i + 2], tokens[i + 3]
    i = i + 5
    line = line + dline
    if dline == 0 then unit = unit + dunit else unit = dunit end
    local attr = attrmap[types[ttype + 1]]
    if attr and len > 0 then
      local s = posfn(line, unit)
      local e = posfn(line, unit + len)
      if e > s then
        out[#out + 1] = { offset = s, length = e - s, attr = attr }
      end
    end
  end
  return out
end

-- Slice spans (sorted by offset) to those touching [start, endoff).
--- @param spans table
--- @param start integer
--- @param endoff integer
--- @return table
local function span_clip(spans, start, endoff)
  local n = #spans
  if n == 0 or endoff <= start then return {} end
  local lo, hi = 1, n
  while lo <= hi do
    local mid = math.floor((lo + hi) / 2)
    if spans[mid].offset + spans[mid].length <= start then
      lo = mid + 1
    else
      hi = mid - 1
    end
  end
  local from = lo
  hi = n
  while lo <= hi do
    local mid = math.floor((lo + hi) / 2)
    if spans[mid].offset < endoff then
      lo = mid + 1
    else
      hi = mid - 1
    end
  end
  local out = {}
  for i = from, hi do out[#out + 1] = spans[i] end
  return out
end

-- Decode inlayHint items into per-line hint lists sorted by display
-- column. Position is the insertion point (UTF-16); label is a string
-- or an array of parts.
--- @param self lsp.Client
--- @param hints table?
--- @return table
local function hint_decode(self, hints)
  local out = {}
  for _, h in ipairs(hints or {}) do
    local pos = h.position
    if pos then
      local label = h.label
      if type(label) == "table" then
        local parts = {}
        for _, p in ipairs(label) do parts[#parts + 1] = p.value end
        label = table.concat(parts)
      end
      if type(label) == "string" and #label > 0 then
        local bcol = utf16_to_byte(self.opts.get_line(pos.line), pos.character)
        local lst = out[pos.line]
        if not lst then lst = {}; out[pos.line] = lst end
        lst[#lst + 1] = { dcol = self.opts.dcol_fn(pos.line, bcol),
          text = label }
      end
    end
  end
  for _, lst in pairs(out) do
    table.sort(lst, function(a, b) return a.dcol < b.dcol end)
  end
  return out
end

-- Fresh hint scheduling state. Client.new and Client:start share this
-- shape: a restarted client must not carry stale lines/retry budgets.
--- @param opts table
--- @return table
local function h_state(opts)
  return {
    hint_dirty = true,
    hint_pending = false,
    hint_view = nil,
    hint_reqver = 0,
    hint_retry = 0,
    hint_null = 0,
    last_edit_t = -1e6, -- startup counts as idle: refresh fires at once
    hint_idle = opts.hint_idle ~= nil and opts.hint_idle
        or tonumber(os.getenv("PT_HINT_IDLE")) or 1.0,
  }
end

-- Replace the hint vtext slots: write the new per-line lists, clear
-- stale lines from the previous response (full replacement semantics).
--- @param self lsp.Client
--- @param out table
local function h_write(self, out)
  local seen = {}
  for line, lst in pairs(out) do
    seen[line] = true
    self.opts.vtext.set(line, lst)
  end
  for line in pairs(self.hint_lines or {}) do
    if not seen[line] then self.opts.vtext.set(line, nil) end
  end
  self.hint_lines = seen
end

-- inlayHint response: null/err -> bounded delayed retries (8, then
-- silent until the next edit); stale (version moved) -> keep the
-- shifted slots and refetch; success -> decode + write slots.
--- @param self lsp.Client
--- @param result any?
--- @param err table?
--- @param now number
local function h_response(self, result, err, now)
  self.hint_pending = false
  local top = self.opts.viewport_fn().top
  if err or not result then
    self.hint_view = top
    self.hint_null = self.hint_null + 1
    if self.hint_null <= 8 then
      self.hint_retry = now + 2
    else
      self.hint_retry, self.hint_dirty = 0, false
    end
  elseif self.proto.version ~= self.hint_reqver then
    -- edited while in flight: refetch on the next tick
  else
    h_write(self, hint_decode(self, result))
    self.hint_view = top
    self.hint_dirty = false
    self.hint_retry, self.hint_null = 0, 0
  end
end

-- publishDiagnostics: drop stale snapshots (out-of-order pushes), decode
-- UTF-16 ranges to byte spans via line starts + get_line.
--- @param self lsp.Client
--- @param p table
local function diag_update(self, p)
  if not self.proto or p.uri ~= self.proto.uri then return end
  local cur = self.diag and self.diag.version or -1
  local v = p.version
  if v and v < cur then return end
  local starts = line_offsets(self.opts.get_text())
  local spans = {}
  for _, d in ipairs(p.diagnostics or {}) do
    local r = d.range
    local s = lsp_pos(self, starts, r.start.line, r.start.character)
    local e = lsp_pos(self, starts, r["end"].line, r["end"].character)
    if e > s then
      spans[#spans + 1] = {
        offset = s, length = e - s,
        attr = self.opts.attrmap.diag or { underline = true },
        msg = d.message, severity = d.severity or 2,
      }
    end
  end
  self.diag = { version = v or cur, spans = spans }
end

-- Client factory. opts adds to the Protocol contract:
--   dcol_fn(line, bytecol) -> display column (tabstop-aware)
--   viewport_fn() -> {top, rows} of the visible text area
--   now_fn() -> wall-clock seconds (default: luv.hrtime()/1e9)
--   attrmap -> semantic tokenType name -> attr table (+ optional diag)
--   vtext = { set(line, list), clear() }  injected Ed vtext slot access
--   hint_idle? -> seconds of no typing before a hint refresh
--   proto? -> pre-built Protocol (tests inject fakes)
--- @param opts table
--- @return lsp.Client
function Client.new(opts)
  opts.now_fn = opts.now_fn or function() return luv.hrtime() / 1e9 end
  local self = h_state(opts)
  self.proto = opts.proto or nil
  self.sem = { spans = {}, dirty = true, pending = false }
  self.diag = nil
  self.opts = opts
  return setmetatable(self, CM) --[[@as lsp.Client]]
end

-- Spawn the server and register diag handling. proto comes from opts or
-- is built from the accessor quad; on failure everything is cleared.
--- @param argv string[]
--- @param uri string
--- @param langid string
--- @param root string?
--- @return boolean
function Client:start(argv, uri, langid, root)
  local p = self.opts.proto or Protocol.new({
    get_text = self.opts.get_text,
    get_line = self.opts.get_line,
    offset_pos = self.opts.offset_pos,
    on_status = self.opts.on_status,
  })
  self.proto = p
  p:on("textDocument/publishDiagnostics", function(params)
    diag_update(self, params)
  end)
  -- restart: fresh caches and hint scheduling (no stale slots/retries)
  for k, v in pairs(h_state(self.opts)) do self[k] = v end
  self.sem = { spans = {}, dirty = true, pending = false }
  self.diag, self.hint_lines = nil, nil
  if not p:start(argv, uri, langid, root) then
    self.proto, self.diag = nil, nil
    self.opts.vtext.clear()
    return false
  end
  return true
end

-- Shut the server down and clear all caches + vtext slots.
function Client:stop()
  if self.proto then self.proto:stop() end
  self.sem, self.diag = nil, nil
  self.opts.vtext.clear()
end

-- Record an edit: protocol sync + dirty flags (slot shift is Ed's job).
--- @param off integer
--- @param del integer
--- @param s string
function Client:on_edit(off, del, s)
  if not self.proto then return end
  self.proto:notify_edit(off, del, s)
  if self.sem then self.sem.dirty = true end
  self.hint_dirty = true
  self.last_edit_t = self.opts.now_fn()
  self.hint_retry, self.hint_null = 0, 0
end

-- Resync after jumps the client cannot localize (undo/redo): one full
-- didChange plus a full cache and vtext-slot reset.
function Client:resync()
  if self.proto then self.proto:sync_full() end
  if self.sem then self.sem.dirty = true end
  self.hint_dirty = true
  self.opts.vtext.clear()
end

-- Idle work (main-loop timeouts): refresh inlay hints once typing has
-- stopped (debounce), the viewport moved, or a null retry is due.
-- Continuous typing never requests; the stale-response guard (doc
-- version) stays as belt-and-braces against races.
function Client:tick()
  local p = self.proto
  if not (p and p.state == "running") then return end
  if not (p.capabilities.inlayHintProvider and not self.hint_pending) then return end
  local vp = self.opts.viewport_fn()
  local now = self.opts.now_fn()
  local idle = now - (self.last_edit_t or 0) >= self.hint_idle
  local retry = self.hint_retry > 0 and now >= self.hint_retry
  if not (self.hint_dirty and idle or self.hint_view ~= vp.top or retry) then return end
  self.hint_pending = true
  self.hint_reqver = p.version
  p:request("textDocument/inlayHint", {
    textDocument = { uri = p.uri },
    range = { start = { line = vp.top, character = 0 },
      ["end"] = { line = vp.top + vp.rows - 1, character = 0x7fffffff } },
  }, function(result, err)
      h_response(self, result, err, now)
    end)
end

-- Render-tail work: refresh semantic tokens when dirty (edit once ->
-- one request; skip while a request is already in flight).
function Client:post_render()
  local p = self.proto
  if not (p and p.state == "running" and self.sem and self.sem.dirty
      and not self.sem.pending) then return end
  local cap = p.capabilities.semanticTokensProvider
  if not (cap and cap.full) then
    self.sem.dirty = false
    return
  end
  local sem = self.sem
  sem.pending = true
  p:request("textDocument/semanticTokens/full", {
    textDocument = { uri = p.uri },
  }, function(result, err)
      sem.pending = false
      if err then
        sem.dirty = false
      elseif result and result.data then
        local starts = line_offsets(self.opts.get_text())
        sem.spans = span_decode(result.data, cap.legend, self.opts.attrmap,
          function(line, unit) return lsp_pos(self, starts, line, unit) end)
        sem.dirty = false
      end
    end)
end

-- Slice the semantic/diag caches to [s, e) for the render merge.
--- @param s integer
--- @param e integer
--- @return table
function Client:query_spans(s, e)
  local out = { sem = {}, diag = {} }
  if self.sem then out.sem = span_clip(self.sem.spans, s, e) end
  if self.diag then out.diag = span_clip(self.diag.spans, s, e) end
  return out
end

-- Diag span containing byte offset `off`; lowest severity number wins
-- (LSP: 1 = error, higher numbers are weaker).
--- @param off integer
--- @return table?
function Client:diag_at(off)
  if not self.diag then return nil end
  local best
  for _, sp in ipairs(self.diag.spans) do
    if sp.offset <= off and off < sp.offset + sp.length then
      if not best or sp.severity < best.severity then best = sp end
    end
  end
  return best
end

-- Server lifecycle state for the status bar.
--- @return string
function Client:status()
  local p = self.proto
  return p and p.state or "exited"
end

-- Pump the transport (main loop).
function Client:poll()
  if self.proto then self.proto:poll() end
end

lsp.Client = Client

return lsp
