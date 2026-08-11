-- tmux_test.lua — display behavior integration tests (real terminal via
-- the tmux helper). Skips when tmux is unavailable.
-- run: just lua/tmux
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = root .. "/?.lua;" .. dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/lua/luajit/?.so;"
    or root .. "/lua/?.so;") .. package.cpath

if not os.execute("command -v tmux >/dev/null 2>&1") then
  io.write("SKIP: tmux not installed\n")
  os.exit(0)
end

local lu = require "luaunit"
local tmux = require "tmux"

-- short-lived temp files in the cwd: short names keep the status bar
-- within the pane width (long /var/folders paths would truncate it)
local tmp_n = 0
--- @return string
local function tmpfile()
  tmp_n = tmp_n + 1
  return string.format("t%d_%d", os.time() % 100000, tmp_n)
end

-- fakelsp: LSP server script wired through the editor's PT_LSP_CMD
-- hook. Handles initialize/inlayHint/shutdown; with `diag` pushes a
-- publishDiagnostics after didOpen (file uri captured from the frame).
--- @param hints {line: integer, character: integer, label: string}[]?
--- @param diag string?
--- @return string
local function fakelsp_src(hints, diag)
  local parts = {}
  for _, h in ipairs(hints or {}) do
    parts[#parts + 1] = string.format(
      "{ position = { line = %d, character = %d }, label = %q }",
      h.line, h.character, h.label)
  end
  local hint_lit = "{" .. table.concat(parts, ", ") .. "}"
  local diag_lit = diag and string.format(
      "{{ range = { start = { line = 0, character = 0 }, "
      .. "['end'] = { line = 0, character = 1 } }, message = %q, "
      .. "severity = 1 } }", diag) or "nil"
  return string.format([[
package.path = package.path .. ";" .. %q .. "/lua/?.lua"
package.cpath = package.cpath .. ";" .. %q .. "/lua/?.so;"
    .. "/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
local lsp = require "lsp"
local yy = require "yyjson"
local HINTS = %s
local DIAG = %s
-- read one byte at a time: io.read(n) blocks until n bytes arrive on a
-- pipe, so a frame shorter than n would deadlock at its tail
local dec = lsp.RPC.decoder(function() return io.read(1) end)
local function sendmsg(t)
  local s = yy.encode(t)
  io.write("Content-Length: ", #s, "\r\n\r\n", s)
  io.flush()
end
while true do
  local m = dec:read()
  if not m then break end
  if m.id then
    if m.method == "initialize" then
      sendmsg({ jsonrpc = "2.0", id = m.id,
        result = { capabilities = { inlayHintProvider = true } } })
    elseif m.method == "textDocument/inlayHint" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = HINTS })
    elseif m.method == "shutdown" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = yy.null })
    end
  elseif m.method == "textDocument/didOpen" then
    local uri = m.params and m.params.textDocument
        and m.params.textDocument.uri
    if DIAG then
      sendmsg({ jsonrpc = "2.0", method = "textDocument/publishDiagnostics",
        params = { uri = uri, version = 1, diagnostics = DIAG } })
    end
  end
end
]], root, root, hint_lit, diag_lit)
end

-- open editor sessions: killed in tearDown even when a test fails
local sessions = {}

-- Spawn the editor on a temp file with fakelsp behind PT_LSP_CMD.
-- Waits for "lsp:on" in the status bar (handshake round trip).
--- @param content string
--- @param hints {line: integer, character: integer, label: string}[]?
--- @param opts {diag?: string}?
--- @return tmux
local function spawn_ed(content, hints, opts)
  opts = opts or {}
  local f = tmpfile()
  local fs = tmpfile()
  local lua_bin = _G["jit"] and "luajit" or "lua"
  local s = tmux.new({
    cmd = string.format("PT_LSP_CMD='%s %s' PT_HINT_IDLE=0 %s %s %s",
      lua_bin, fs, lua_bin, root .. "/editor.lua", f),
    files = { { path = f, content = content },
              { path = fs, content = fakelsp_src(hints, opts.diag) } },
  })
  sessions[#sessions + 1] = s
  s:wait(function()
    for _, row in ipairs(s:capture()) do
      if row:find("lsp:on", 1, true) then return true end
    end
    return false
  end)
  return s
end

-- fakelsp variant that pushes a diagnostic after didOpen
--- @param content string
--- @param msg string
--- @return tmux
local function spawn_ed_diag(content, msg)
  return spawn_ed(content, nil, { diag = msg })
end

-- Poll the screen until a row contains text (plain substring)
--- @param s tmux
--- @param text string
local function wait_screen(s, text)
  s:wait(function()
    for _, row in ipairs(s:capture()) do
      if row:find(text, 1, true) then return true end
    end
    return false
  end)
end

TestDisplay = {}

-- kill leftover sessions on every test exit (pass or fail)
function TestDisplay:tearDown()
  for _, s in ipairs(sessions) do s:kill() end
  sessions = {}
end

function TestDisplay:testBasicRender()
  local s = spawn_ed("local x = 1\n", nil)
  local rows = s:capture()
  lu.assertStrContains(rows[1] or "", "local x = 1")
  lu.assertStrContains(rows[1] or "", "1")            -- line-number column
  lu.assertStrContains(rows[24] or "", "NORMAL")      -- status bar
end

function TestDisplay:testJKKeepsScreenCol()
  -- core bug: hint "int:" at byte 0 of line 0; j must keep the cursor's
  -- screen column on line 1 (Neovim semantics)
  local s = spawn_ed("hello\nworld\n",
    { { line = 0, character = 0, label = "int:" } })
  wait_screen(s, "int:")
  lu.assertStrContains(s:capture()[1] or "", "int:hello")
  local x = s:cursor().x
  s:feed("j")
  s:wait(function() return s:cursor().y == 1 end)
  lu.assertEquals(s:cursor().x, x)   -- screen column preserved
end

function TestDisplay:testInsertGapCursor()
  -- insert at the hint gap: the cursor snaps before the hint text
  local s = spawn_ed("hello\n",
    { { line = 0, character = 0, label = "int:" } })
  wait_screen(s, "int:")
  s:feed("i")
  s:wait(function() return s:cursor().x == 4 end)
end

function TestDisplay:testEditNoTear()
  -- typing before the hint shifts it with the text (no tear/overwrite)
  local s = spawn_ed("hello\n",
    { { line = 0, character = 0, label = "int:" } })
  wait_screen(s, "int:")
  s:feed("i", "X", "Escape")
  s:wait(function()
    local row = s:capture()[1] or ""
    return row:find("Xint:hello", 1, true) ~= nil
  end)
  lu.assertStrContains(s:capture()[1] or "", "Xint:hello")
end

function TestDisplay:testScroll()
  local lines = {}
  for i = 1, 40 do lines[i] = "line " .. i end
  local s = spawn_ed(table.concat(lines, "\n") .. "\n", nil)
  s:feed("G")                       -- jump to last line: viewport scrolls
  s:wait(function()
    local row = s:capture()[1] or ""
    return row:find("line 18", 1, true) ~= nil
  end)
  lu.assertStrContains(table.concat(s:capture(), "\n"), "line 40")
  s:feed("gg")
  s:wait(function()
    local row = s:capture()[1] or ""
    return row:find("^%s*1%s+line 1") ~= nil
  end)
end

function TestDisplay:testStatusDiag()
  -- fakelsp variant: pushes publishDiagnostics after initialized
  local s = spawn_ed_diag("local x = 1\n", "boom")
  wait_screen(s, "diag: boom")
end

function TestDisplay:testQuit()
  local s = spawn_ed("x\n", nil)
  s:feed(":", "q", "Enter")
  lu.assertTrue(s:wait(function() return s:gone() end), "editor did not quit")
end

os.exit(lu.LuaUnit.run(), true)
