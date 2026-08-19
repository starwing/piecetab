-- tmux.lua — tmux-backed display test helper (a fake PTY: tmux owns the
-- terminal, parses the CSI stream, and hands out screen state).
-- run: just lua/tmux (requires tmux in PATH)
--- @class tmux
--- @field name string  tmux session name
--- @field files {path: string, content: string}[]  temp files written/removed
local tmux = {}

local M = { __index = tmux }

--- @param args string
--- @return string
local function run(args)
  local p = assert(io.popen("tmux " .. args .. " 2>&1"))
  local out = p:read("*a")
  p:close()
  return out
end

local function server_down(out)
  return out:find("no server running", 1, true) ~= nil
      or out:find("can't find pane", 1, true) ~= nil
end

local nextid = 0

-- opts: {cmd=string, rows=integer, cols=integer, files={{path=, content=}}}
-- files are written before spawn and removed on kill. The command is
-- double-quoted so inner single quotes (e.g. env values with spaces)
-- survive the popen shell and reach the tmux session shell intact.
--- @param opts {cmd: string, rows?: integer, cols?: integer,
---   files?: {path: string, content: string}[]}
--- @return tmux
function tmux.new(opts)
  nextid = nextid + 1
  local self = setmetatable({}, M)
  self.name = "pttest" .. nextid
  self.files = opts.files or {}
  for _, f in ipairs(self.files) do
    local fh = assert(io.open(f.path, "w"))
    fh:write(f.content)
    fh:close()
  end
  local cmd = string.format("new-session -d -s %s -x %d -y %d \"%s\"",
    self.name, opts.cols or 80, opts.rows or 24, opts.cmd)
  local ok = false
  for _ = 1, 5 do
    if run(cmd):find("^$") then ok = true break end
    os.execute("sleep 0.1")
  end
  assert(ok, "tmux spawn failed: " .. cmd)
  assert(self:wait(function() return #self:capture() > 0 end, 500),
    "tmux server not ready for " .. self.name)
  return self
end

-- Send named/plain keys through tmux (characters, "Enter", "Escape", ...)
--- @param self tmux
--- @param ... string
function tmux:feed(...)
  local parts = {}
  for _, k in ipairs({ ... }) do parts[#parts + 1] = "'" .. k .. "'" end
  run("send-keys -t " .. self.name .. " " .. table.concat(parts, " "))
end

-- Send raw escape bytes (hex pairs, e.g. "1b5b41" = ESC [ A) via -H
--- @param self tmux
--- @param hex string
function tmux:raw(hex)
  run("send-keys -t " .. self.name .. " -H '" .. hex .. "'")
end

-- Screen snapshot: one string per row, trailing whitespace trimmed
--- @param self tmux
--- @return string[]
function tmux:capture()
  local out = run("capture-pane -t " .. self.name .. " -p")
  if server_down(out) then return {} end
  local rows = {}
  for line in (out .. "\n"):gmatch("(.-)\n") do
    rows[#rows + 1] = line:gsub("%s+$", "")
  end
  return rows
end

-- Cursor position {x, y} (0-based within the pane)
--- @param self tmux
--- @return {x: integer, y: integer}
function tmux:cursor()
  local out = run("display -p -t " .. self.name .. " '#{cursor_x} #{cursor_y}'")
  if server_down(out) then return { x = -1, y = -1 } end
  local x, y = out:match("(%d+) (%d+)")
  if not x or not y then return { x = -1, y = -1 } end
  return { x = tonumber(x), y = tonumber(y) }
end

-- Poll until pred(s) is true; returns the final pred result
--- @param self tmux
--- @param pred fun(s: tmux): boolean
--- @param timeout integer?
--- @return boolean
function tmux:wait(pred, timeout)
  timeout = timeout or 300
  for _ = 1, timeout do
    if pred(self) then return true end
    os.execute("sleep 0.02")
  end
  return pred(self)
end

-- True once the session no longer exists (process exited)
--- @param self tmux
--- @return boolean
function tmux:gone()
  return run("list-sessions 2>&1"):find(self.name) == nil
end

-- Kill the session and remove spawned temp files
--- @param self tmux
function tmux:kill()
  run("kill-session -t " .. self.name .. " 2>&1")
  for _, f in ipairs(self.files or {}) do os.remove(f.path) end
end

return tmux
