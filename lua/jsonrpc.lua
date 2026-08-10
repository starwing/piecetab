-- JSON-RPC 2.0 message framing over byte streams (LSP transport).
-- Wire format: "Content-Length: <n>\r\n\r\n<body>" with body = JSON.
-- Pure functions + injected chunk reader -> fully testable offline.
local yyjson = require("yyjson")

--- @class jsonrpc.Mod
local jsonrpc = {}

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
function jsonrpc.enc_request(id, method, params)
  local msg = { jsonrpc = "2.0", id = id, method = method }
  if params ~= nil then msg.params = params end
  return frame(yyjson.encode(msg))
end

--- Encode a notification frame.
--- @param method string
--- @param params any?
--- @return string
function jsonrpc.enc_notify(method, params)
  local msg = { jsonrpc = "2.0", method = method }
  if params ~= nil then msg.params = params end
  return frame(yyjson.encode(msg))
end

--- Encode a success response frame.
--- @param id integer
--- @param result any
--- @return string
function jsonrpc.enc_result(id, result)
  return frame(yyjson.encode({ jsonrpc = "2.0", id = id, result = result }))
end

--- Encode an error response frame.
--- @param id integer
--- @param code integer
--- @param message string
--- @return string
function jsonrpc.enc_error(id, code, message)
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
function jsonrpc.decoder(readchunk)
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

return jsonrpc
