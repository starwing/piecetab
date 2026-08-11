-- lsp: LSP client building blocks — RPC framing + server process IO.
-- RPC encodes/decodes JSON-RPC 2.0 messages over Content-Length byte
-- frames; pure functions with an injected chunk reader, fully testable
-- offline. IO spawns the server process over luv pipes and pumps libuv
-- callbacks on demand so the editor main loop never restructures.
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

local IO = {}
local IM = { __index = IO }

-- lspio: LSP server process bridge over luv (libuv binding).
-- Owns spawn + stdio pipes; pumps libuv callbacks on demand so the
-- editor main loop (blocking termfeed getkey) never restructures:
-- each frame: lspio.pump(h) then drain messages via jsonrpc.decoder.

--- @class lspio.Handle
--- @field exit boolean  process exited
--- @field exit_code integer
--- @field proc table
--- @field err string?  spawn error
--- @field stdin table
--- @field stdout table
--- @field buf table
--- @field outqueue table

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
--- @return lspio.Handle?
--- @return string?  err (executable not found, etc.)
function IO.spawn(argv)
  --- @type lspio.Handle
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
--- @param h lspio.Handle
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
--- @param h lspio.Handle
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
--- @param h lspio.Handle
--- @param bytes string
function IO.send(h, bytes)
  h.outqueue[#h.outqueue + 1] = bytes
end

-- Pump libuv callbacks once (non-blocking); process pending writes.
--- @param h lspio.Handle
function IO.pump(h)
  luv.run("nowait")
  IO.drain(h)
end

-- Terminate the server process.
--- @param h lspio.Handle
function IO.close(h)
  if not h.exit and h.proc then
    luv.process_kill(h.proc)
  end
  h.stdin:close()
  luv.read_stop(h.stdout)
  h.stdout:close()
end

lsp.IO = IO

return lsp
