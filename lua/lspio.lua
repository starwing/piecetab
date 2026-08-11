-- lspio: LSP server process bridge over luv (libuv binding).
-- Owns spawn + stdio pipes; pumps libuv callbacks on demand so the
-- editor main loop (blocking termfeed getkey) never restructures:
-- each frame: lspio.pump(h) then drain messages via jsonrpc.decoder.
local luv = require("luv")

--- @class lspio.Handle
--- @field exit boolean  process exited
--- @field exit_code integer
--- @field proc table
--- @field err string?  spawn error
--- @field stdin table
--- @field stdout table
--- @field buf table
--- @field outqueue table
local lspio = {}

local M = { __index = lspio }

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
function lspio.spawn(argv)
  --- @type lspio.Handle
  local h = setmetatable({
    stdin = luv.new_pipe(false),
    stdout = luv.new_pipe(false),
    buf = {},
    outqueue = {},
    exit = false,
    exit_code = 0,
    reader = nil,
  }, M)
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
  h.reader = lspio.reader(h)
  return h
end

-- Persistent chunk reader: "" = no data yet,
-- nil = process exited. Index advances across pauses, so the reader
-- survives "again" retries (frames are consumed incrementally).
--- @param h lspio.Handle
--- @return fun(): string?
function lspio.reader(h)
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
function lspio.drain(h)
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
function lspio.send(h, bytes)
  h.outqueue[#h.outqueue + 1] = bytes
end

-- Pump libuv callbacks once (non-blocking); process pending writes.
--- @param h lspio.Handle
function lspio.pump(h)
  luv.run("nowait")
  lspio.drain(h)
end

-- Terminate the server process.
--- @param h lspio.Handle
function lspio.close(h)
  if not h.exit and h.proc then
    luv.process_kill(h.proc)
  end
  h.stdin:close()
  luv.read_stop(h.stdout)
  h.stdout:close()
end

return lspio
