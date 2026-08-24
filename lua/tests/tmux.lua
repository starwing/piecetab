-- tmux.lua — tmux-backed display test helper (a fake PTY: tmux owns the
-- terminal, parses the CSI stream, and hands out screen state).
-- run: just lua/tmux (requires tmux in PATH)
--- @class tmux
--- @field name string  tmux session name
--- @field files {path: string, content: string}[]  temp files written/removed
--- @field errfile string? temp file path for stderr capture (if set)
--- @field codefile string? temp file path for exit code capture (if set)
local tmux = {}
tmux.__index = tmux

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
  local self = setmetatable({}, tmux)
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
    if run(cmd):find("^$") then
      ok = true
      break
    end
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

-- Styled-match markers: fullwidth brackets so they cannot collide with
-- ordinary ASCII brackets in captured content.
local ST = "\239\188\155" -- ［ U+FF3B
local ET = "\239\188\157" -- ］ U+FF3D

-- Build a styled match string: "［F207G236B］text［R］".
-- Absent attributes are omitted; default text has no marker.
--- @param text string
--- @param attr {fg?: integer|string, bg?: integer|string, bold?: boolean,
---   dim?: boolean, italic?: boolean, underline?: boolean,
---   reverse?: boolean}?
--- @return string
function tmux.styled(text, attr)
  if not attr or next(attr) == nil then return text end
  local parts = { ST }
  if attr.fg ~= nil then parts[#parts + 1] = "F" .. tostring(attr.fg) end
  if attr.bg ~= nil then parts[#parts + 1] = "G" .. tostring(attr.bg) end
  if attr.bold then parts[#parts + 1] = "B" end
  if attr.dim then parts[#parts + 1] = "D" end
  if attr.italic then parts[#parts + 1] = "I" end
  if attr.underline then parts[#parts + 1] = "U" end
  if attr.reverse then parts[#parts + 1] = "V" end
  parts[#parts + 1] = ET
  return table.concat(parts) .. text .. ST .. "R" .. ET
end

-- Apply one SGR parameter string to an attr table in place.
--- @param attr table
--- @param s string
local function apply_sgr(attr, s)
  local n = {}
  for v in s:gmatch("%d+") do n[#n + 1] = tonumber(v) end
  if #n == 0 then
    for k in pairs(attr) do attr[k] = nil end
    return
  end
  local i = 1
  while i <= #n do
    local c = n[i]
    if c == 0 then
      for k in pairs(attr) do attr[k] = nil end
    elseif c == 1 then attr.bold = true
    elseif c == 2 then attr.dim = true
    elseif c == 3 then attr.italic = true
    elseif c == 4 then attr.underline = true
    elseif c == 7 then attr.reverse = true
    elseif c == 22 then attr.bold, attr.dim = nil, nil
    elseif c == 23 then attr.italic = nil
    elseif c == 24 then attr.underline = nil
    elseif c == 27 then attr.reverse = nil
    elseif c == 39 then attr.fg = nil
    elseif c == 49 then attr.bg = nil
    elseif c >= 30 and c <= 37 then attr.fg = c - 30
    elseif c >= 40 and c <= 47 then attr.bg = c - 40
    elseif c >= 90 and c <= 97 then attr.fg = c - 90 + 8
    elseif c >= 100 and c <= 107 then attr.bg = c - 100 + 8
    elseif c == 38 or c == 48 then
      local mode = n[i + 1]
      if mode == 5 then
        if c == 38 then attr.fg = n[i + 2] else attr.bg = n[i + 2] end
        i = i + 2
      elseif mode == 2 then
        local rgb = table.concat({ n[i + 2], n[i + 3], n[i + 4] }, ",")
        if c == 38 then attr.fg = rgb else attr.bg = rgb end
        i = i + 4
      end
    end
    i = i + 1
  end
end

-- Convert one ANSI-SGR row into the styled-match representation.
--- @param row string
--- @return string
local function render_sgr(row)
  local out = {}
  local attr = {}
  local buf = {}
  local function flush()
    if #buf == 0 then return end
    local text = table.concat(buf)
    buf = {}
    if next(attr) == nil then
      out[#out + 1] = text
    else
      out[#out + 1] = tmux.styled(text, attr)
    end
  end
  local i, n = 1, #row
  while i <= n do
    if row:sub(i, i + 1) == "\27[" then
      local j = i + 2
      while j <= n and not row:sub(j, j):match("%a") do j = j + 1 end
      local letter = row:sub(j, j)
      if letter == "m" then
        flush()
        apply_sgr(attr, row:sub(i + 2, j - 1))
      end
      i = j + 1
    else
      buf[#buf + 1] = row:sub(i, i)
      i = i + 1
    end
  end
  flush()
  return table.concat(out)
end

-- Styled screen snapshot: one string per row, trailing plain whitespace
-- trimmed. Same shape as capture(), but ANSI SGR runs are replaced with
-- one-way match strings (see tmux.styled).
--- @param self tmux
--- @return string[]
function tmux:capture_styled()
  local out = run("capture-pane -t " .. self.name .. " -e -p")
  if server_down(out) then return {} end
  local rows = {}
  for line in (out .. "\n"):gmatch("(.-)\n") do
    local s = render_sgr(line)
    rows[#rows + 1] = s:gsub("%s+$", "")
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
