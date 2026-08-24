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

-- tree-sitter is optional: the styled assertions below only make sense
-- when the binding and the Lua grammar are present. Skip them after a
-- clean build / on CI hosts that do not build tree-sitter yet.
local has_treesitter = pcall(function()
  local ts = require "treesitter"
  ts.require("lua")
end)

local function skip_without_treesitter()
  lu.skipIf(not has_treesitter, "tree-sitter not available")
end

-- short-lived temp files in TMPDIR: os.tmpname() returns a unique path
-- per call; contents are (over)written by tmux.new's io.open(path, "w").
-- The ".lua" suffix makes tree-sitter highlight the edited file (the
-- display tests assert TS colors). Long paths may truncate the status
-- bar, but no assertion depends on it.
--- @return string
local function tmpfile()
  return os.tmpname() .. ".lua"
end

-- Render a small Lua value as a Lua literal (used to embed fakelsp
-- semantic-token payloads into the generated server script).
--- @param v any
--- @return string
local function lua_lit(v)
  if type(v) == "string" then return string.format("%q", v) end
  if type(v) == "number" or type(v) == "boolean" then return tostring(v) end
  if type(v) == "table" then
    local arr = {}
    for _, x in ipairs(v) do arr[#arr + 1] = lua_lit(x) end
    if #arr > 0 then return "{" .. table.concat(arr, ", ") .. "}" end
    local kv = {}
    for k, x in pairs(v) do
      kv[#kv + 1] = "[" .. lua_lit(k) .. "] = " .. lua_lit(x)
    end
    table.sort(kv)
    return "{" .. table.concat(kv, ", ") .. "}"
  end
  return "nil"
end

-- Encode high-level semantic tokens into the relative-encoded data
-- array used by textDocument/semanticTokens/full.
--- @param legend string[]
--- @param tokens {line: integer, character: integer, len: integer, type: string}[]
--- @return {legend: string[], data: integer[]}
local function sem_tokens(legend, tokens)
  local data = {}
  local line, unit = 0, 0
  for _, t in ipairs(tokens) do
    local ttype
    for i, name in ipairs(legend) do
      if name == t.type then ttype = i - 1 break end
    end
    assert(ttype ~= nil, "unknown semantic token type " .. tostring(t.type))
    data[#data + 1] = t.line - line
    if t.line == line then
      data[#data + 1] = t.character - unit
    else
      data[#data + 1] = t.character
    end
    data[#data + 1] = t.len
    data[#data + 1] = ttype
    data[#data + 1] = 0
    line = t.line
    unit = t.character
  end
  return { legend = legend, data = data }
end

-- fakelsp: LSP server script wired through the editor's PT_LSP_CMD
-- hook. Handles initialize/inlayHint/semanticTokens/shutdown; with
-- `diag` pushes a publishDiagnostics after didOpen; with `sem` answers
-- textDocument/semanticTokens/full (relative-encoded data).
--- @param hints {line: integer, character: integer, label: string}[]?
--- @param diag string?
--- @param sem {legend?: string[], data?: integer[]}?
--- @return string
local function fakelsp_src(hints, diag, sem)
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
  local sem_lit = sem and lua_lit(sem) or "nil"
  local sem_legend_lit = sem and lua_lit(sem.legend or {}) or "nil"
  return string.format([[
package.path = package.path .. ";" .. %q .. "/lua/?.lua"
package.cpath = package.cpath .. ";" .. %q .. "/lua/?.so;"
    .. "/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"
local lsp = require "lsp"
local yy = require "json"
local HINTS = %s
local DIAG = %s
local SEM = %s
local SEM_LEGEND = %s
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
        result = { capabilities = {
          inlayHintProvider = true,
          semanticTokensProvider = SEM and {
            full = true,
            legend = { tokenTypes = SEM_LEGEND },
          } or nil,
        } } })
    elseif m.method == "textDocument/inlayHint" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = HINTS })
    elseif m.method == "textDocument/semanticTokens/full" then
      sendmsg({ jsonrpc = "2.0", id = m.id, result = { data = SEM.data } })
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
]], root, root, hint_lit, diag_lit, sem_lit, sem_legend_lit)
end

-- open editor sessions: killed in tearDown even when a test fails
local sessions = {}

local function dump_screen(s, label)
  io.stderr:write(label, "\n")
  local rows = s:capture()
  if #rows == 0 then io.stderr:write("  (no captured rows)\n") end
  for i, row in ipairs(rows) do
    io.stderr:write(string.format("  %2d: %q\n", i, row))
  end
  local cur = s:cursor()
  io.stderr:write(string.format("  cursor: %s,%s\n",
    tostring(cur.x), tostring(cur.y)))
  if s.codefile then
    local f = io.open(s.codefile, "r")
    if f then
      local code = f:read("*a")
      f:close()
      if code ~= "" then io.stderr:write("  exit code: ", code, "\n") end
    end
  end
  if s.errfile then
    local f = io.open(s.errfile, "r")
    if f then
      local err = f:read("*a")
      f:close()
      if err ~= "" then io.stderr:write("  stderr: ", err, "\n") end
    end
  end
end

-- Spawn the editor on a temp file with fakelsp behind PT_LSP_CMD.
-- Waits for "lsp:on" in the status bar (handshake round trip).
--- @param content string
--- @param hints {line: integer, character: integer, label: string}[]?
--- @param opts {diag?: string, sem?: {legend?: string[], data?: integer[]}}?
--- @return tmux
local function spawn_ed(content, hints, opts)
  opts = opts or {}
  local f = tmpfile()
  local fs = tmpfile()
  local errf = tmpfile()
  local codef = tmpfile()
  local lua_bin = _G["jit"] and "luajit" or "lua"
  local s = tmux.new({
    cmd = string.format(
      "PT_LSP_CMD='%s %s' PT_HINT_IDLE=0 %s %s %s 2>%s; echo \\$? > %s",
      lua_bin, fs, lua_bin, root .. "/editor.lua", f, errf, codef),
    files = { { path = f, content = content },
      { path = fs,    content = fakelsp_src(hints, opts.diag, opts.sem) },
      { path = errf,  content = "" },
      { path = codef, content = "" } },
  })
  s.errfile = errf
  s.codefile = codef
  sessions[#sessions + 1] = s
  local ok = s:wait(function()
    for _, row in ipairs(s:capture()) do
      if row:find("lsp:on", 1, true) then return true end
    end
    return false
  end)
  if not ok then dump_screen(s, "spawn_ed timeout waiting for lsp:on") end
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
--- @return boolean
local function wait_screen(s, text)
  local ok = s:wait(function()
    for _, row in ipairs(s:capture()) do
      if row:find(text, 1, true) then return true end
    end
    return false
  end)
  if not ok then dump_screen(s, "wait_screen timeout for " .. string.format("%q", text)) end
  return ok
end

-- Poll the styled screen until a row contains a styled-match substring
--- @param s tmux
--- @param text string
--- @return boolean
local function wait_styled(s, text)
  local ok = s:wait(function()
    for _, row in ipairs(s:capture_styled()) do
      if row:find(text, 1, true) then return true end
    end
    return false
  end)
  if not ok then dump_screen(s, "wait_styled timeout for " .. string.format("%q", text)) end
  return ok
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
  lu.assertStrContains(rows[1] or "", "1")       -- line-number column
  lu.assertStrContains(rows[24] or "", "NORMAL") -- status bar
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
  lu.assertEquals(s:cursor().x, x) -- screen column preserved
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
  for _ = 1, 11 do s:feed("l") end  -- col 11: the 'h' of package.path
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
  s:feed("G") -- jump to last line: viewport scrolls
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
  lu.assertIsTrue(s:wait(function() return s:gone() end), "editor did not quit")
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
  lu.assertStrContains(row, "30") -- line number "30" at the pane edge
  lu.assertStrContains(row, "line 30")
end

function TestDisplay:testCursorClampedToScreen()
  -- long line: display col 100 clamps to the pane width (80)
  local s = spawn_ed(string.rep("x", 100) .. "\n", nil)
  s:feed("$")
  s:wait(function() return s:cursor().x > 0 end)
  lu.assertIsTrue(s:cursor().x < 80, "cursor x " .. s:cursor().x)
end

function TestDisplay:testLongLineTruncated()
  -- a line wider than the pane is cut at the pane edge (no wrap):
  -- the row is filled edge to edge (line number + truncated content)
  local s = spawn_ed(string.rep("x", 120) .. "\n", nil)
  local row = s:capture()[1] or ""
  lu.assertEquals(#row, 80)
  lu.assertIsTrue(row:match("^%s*%d+") ~= nil, "line number prefix")
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

function TestDisplay:testTabAfterHint()
  -- tab width uses the screen-column base: after a + hint(2) the cursor
  -- is at screen col 3, so the tab expands to the next stop (col 4) and
  -- only one space is shown (a + hi + ' ' + b)
  local s = spawn_ed("a\tb\n",
    { { line = 0, character = 1, label = "hi" } })
  wait_screen(s, "hi")
  s:wait(function()
    local row = s:capture()[1] or ""
    return row:find("b", 1, true) ~= nil
  end)
  local row = s:capture()[1] or ""
  -- b lands at screen col 4: hint shifts the tab's start, not its width
  lu.assertStrContains(row, "ahi b")
end

function TestDisplay:testTsSyntaxHighlight()
  skip_without_treesitter()
  -- tree-sitter colors are the base layer: keyword/function/comment/
  -- string all render with their ATTR_* colors when no LSP semantic
  -- tokens are advertised.
  local s = spawn_ed(
    "local x = hello()\n  -- hi\n  return \"s\"\n", nil)
  wait_styled(s, tmux.styled("local", { fg = 207 }))
  local rows = s:capture_styled()
  lu.assertStrContains(rows[1] or "", tmux.styled("local", { fg = 207 }))
  lu.assertStrContains(rows[1] or "", tmux.styled("hello", { fg = 81 }))
  lu.assertStrContains(rows[2] or "", tmux.styled("-- hi", { fg = 245 }))
  lu.assertStrContains(rows[3] or "", tmux.styled("\"s\"", { fg = 114 }))
end

function TestDisplay:testLspSemDoesNotWipeTs()
  skip_without_treesitter()
  -- Mixed highlighting: LSP semantic tokens paint variable/comment, but
  -- the tree-sitter keyword `local` must stay visible through the sem
  -- layer. The long comment (>64 bytes) is the end-to-end regression
  -- for the get_line cap bug that used to stretch a comment token to EOF
  -- and wipe every lower-layer color.
  local comment = "-- this comment is longer than sixty four bytes "
      .. "to trigger the old bug"
  local content = "local x = 1\n" .. comment .. "\n"
  local sem = sem_tokens({ "comment", "variable" }, {
    { line = 0, character = 6, len = 1, type = "variable" },
    { line = 1, character = 0, len = #comment, type = "comment" },
  })
  local s = spawn_ed(content, nil, { sem = sem })
  -- LSP semantic tokens have arrived once x is painted by the sem layer.
  wait_styled(s, tmux.styled("x", { fg = 114 }))
  local rows = s:capture_styled()
  lu.assertStrContains(rows[1] or "", tmux.styled("local", { fg = 207 }))
  lu.assertStrContains(rows[1] or "", tmux.styled("x", { fg = 114 }))
  lu.assertStrContains(rows[2] or "",
    tmux.styled(comment, { fg = 245 }))
end

os.exit(lu.LuaUnit.run(), true)
