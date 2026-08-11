-- editor_tmux_test.lua — editor display behavior integration tests: a
-- real editor.lua process inside a real terminal (tmux helper owns the
-- PTY and parses the CSI stream), asserting screen text and cursor
-- coordinates. The unit-level counterpart editor_test.lua covers logic;
-- this file covers what shows on screen.
-- run: just lua/ed-tmux (skips when tmux is unavailable)
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
local yy = require "json"
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

function TestDisplay:testMoveVertSkipsHint()
  -- j onto a hint: the cursor skips past the injected text (Neovim
  -- coladvance), never lands inside it or short of it. Screen x = 4
  -- (gutter "%3d ") + display col.
  local s = spawn_ed("package.path\nlocal pt = require\n",
    { { line = 1, character = 8, label = ": table" } })
  wait_screen(s, ": table")
  for _ = 1, 11 do s:feed("l") end -- col 11: the 'h' of package.path
  lu.assertEquals(s:cursor().x, 15) -- 4 + 11, no hint on line 0
  s:feed("j")
  s:wait(function() return s:cursor().y == 1 end)
  lu.assertEquals(s:cursor().x, 19) -- 4 + 15: first text cell past the hint
end

function TestDisplay:testMoveVertKeepsGoalAcrossEmpty()
  -- j over an empty line keeps the theoretical column (Neovim curswant):
  -- the cursor re-lands on the long line below instead of staying at 0
  local s = spawn_ed("abcdefghijklmnop\n\nabcdefghijklmnop\n", nil)
  for _ = 1, 12 do s:feed("l") end
  s:feed("j")
  s:wait(function() return s:cursor().y == 1 end)
  lu.assertEquals(s:cursor().x, 4) -- empty line: clamped to line start
  s:feed("j")
  s:wait(function() return s:cursor().y == 2 end)
  lu.assertEquals(s:cursor().x, 16) -- 4 + 12: goal restored
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

-- migrated display-behavior cases (screen state asserted; the escape/
-- diff-shape assertions stayed in editor_test)

function TestDisplay:testInsertModeStatus()
  local s = spawn_ed("x\n", nil)
  lu.assertStrContains(s:capture()[24] or "", "NORMAL")
  s:feed("i")
  s:wait(function()
    local row = s:capture()[24] or ""
    return row:find("INSERT", 1, true) ~= nil
  end)
end

function TestDisplay:testCommandLinePrompt()
  local s = spawn_ed("x\n", nil)
  s:feed(":")
  s:wait(function()
    local row = s:capture()[24] or ""
    return row:find(":", 1, true) ~= nil
  end)
end

function TestDisplay:testEditAndRender()
  -- typing renders at the content column; line number stays "1"
  local s = spawn_ed("ab\n", nil)
  s:feed("i", "x", "Escape")
  s:wait(function()
    local row = s:capture()[1] or ""
    return row:find("xab", 1, true) ~= nil
  end)
  lu.assertStrContains(s:capture()[1] or "", "xab")
end

function TestDisplay:testScrollLineNumbers()
  -- 30 lines: j to the end scrolls the viewport; the bottom line number
  -- must track the file (regression: line-number redraw bug in the
  -- unit-level SD path)
  local lines = {}
  for i = 1, 30 do lines[i] = "line " .. i end
  local s = spawn_ed(table.concat(lines, "\n") .. "\n", nil)
  for _ = 1, 29 do s:feed("j") end
  s:wait(function()
    local row = s:capture()[23] or ""
    return row:find("line 30", 1, true) ~= nil
  end)
  local row = s:capture()[23] or ""
  lu.assertStrContains(row, "30")    -- line number "30" at the pane edge
  lu.assertStrContains(row, "line 30")
end

function TestDisplay:testCursorClampedToScreen()
  -- long line: display col 100 clamps to the pane width (80)
  local s = spawn_ed(string.rep("x", 100) .. "\n", nil)
  s:feed("$")
  s:wait(function() return s:cursor().x > 0 end)
  lu.assertTrue(s:cursor().x < 80, "cursor x " .. s:cursor().x)
end

function TestDisplay:testLongLineTruncated()
  -- a line wider than the pane is cut at the pane edge (no wrap):
  -- the row is filled edge to edge (line number + truncated content)
  local s = spawn_ed(string.rep("x", 120) .. "\n", nil)
  local row = s:capture()[1] or ""
  lu.assertEquals(#row, 80)
  lu.assertTrue(row:match("^%s*%d+") ~= nil, "line number prefix")
end

function TestDisplay:testTabExpands()
  -- tab renders at the next tab stop (default tabstop 4)
  local s = spawn_ed("a\tb\n", nil)
  s:wait(function()
    local row = s:capture()[1] or ""
    return row:find("b", 1, true) ~= nil
  end)
  local row = s:capture()[1] or ""
  lu.assertStrContains(row, "a   b") -- tab -> 3 spaces (col 4)
end

os.exit(lu.LuaUnit.run(), true)
