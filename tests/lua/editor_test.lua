-- editor.lua headless operation tests (Neovim-tui-style: inject keys,
-- capture render byte stream, assert screen-affecting CSI).
-- run: just lua-ed (cwd = repo root)
-- Requires the lua-utf8 rock (same cpath as editor.lua itself).
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = root .. "/?.lua;" .. dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/build/luajit/?.so;"
    or root .. "/build/lua55/?.so;")
    .. package.cpath
    .. ";./build/lua55/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"

local lu = require "luaunit"
local ed = require "editor"
local term = ed.term

-- ======== Harness: inject keys, capture frames ========

local prev_out = io.output()
local LINES = 20 -- document lines
local ROWS, COLS = 6, 40 -- terminal size: 5 content rows + status bar

local function make_doc()
    local path = os.tmpname()
    local f = assert(io.open(path, "w"))
    for i = 1, LINES do
        f:write("line " .. i .. "\n")
    end
    f:close()
    return path
end

--- Render one frame, return the captured byte stream.
local function frame()
    local path = os.tmpname()
    local f = assert(io.open(path, "w+"))
    io.output(f)
    ed.render()
    io.output(prev_out)
    f:seek("set", 0)
    local s = f:read("*a")
    f:close()
    os.remove(path)
    return s
end

--- Drive one key: dispatch key, then render frame, return frame bytes.
local function keystroke(k)
    ed.dispatch(k)
    return frame()
end

local function setup()
    ed.init(make_doc())
    term.size = function() return ROWS, COLS end
end

-- ======== Scroll regression: viewport down must emit SU, not SD ========

TestScroll = {}

function TestScroll:setUp()
    setup()
end

function TestScroll:testDownScrollEmitsSu()
    -- j to the last visible row (row 4 of 5), then one more j scrolls
    for _ = 1, 4 do
        keystroke("j")
    end
    local s = keystroke("j") -- 5th j: scroll_line 0 -> 1
    lu.assertStrContains(s, "\27[1;5r") -- scroll region rows 1..5
    lu.assertStrContains(s, "\27[1S")   -- SU 1: content scrolls UP
    lu.assertNotStrContains(s, "\27%[1T") -- no SD
    lu.assertEquals(ed.scroll_line, 1)
end

function TestScroll:testDownScrollMany()
    for _ = 1, 4 do
        keystroke("j")
    end
    for i = 1, 10 do
        local s = keystroke("j")
        lu.assertNotStrContains(s, "\27%[1T", "frame " .. i)
        lu.assertEquals(ed.scroll_line, i)
    end
    lu.assertEquals(ed.scroll_line, 10)
end

function TestScroll:testUpScrollEmitsSd()
    for _ = 1, 14 do -- deep scroll down (viewport 10..14)
        keystroke("j")
    end
    -- k until the viewport starts scrolling up: content must scroll DOWN (SD)
    local s
    for _ = 1, 20 do
        s = keystroke("k")
        if ed.scroll_line < 10 then break end
    end
    lu.assertStrContains(s, "\27[1;5r")
    lu.assertStrContains(s, "\27[1T")    -- SD 1: content scrolls DOWN
    lu.assertNotStrContains(s, "\27%[1S") -- no SU
    lu.assertEquals(ed.scroll_line, 9)
end

function TestScroll:testUpScrollRedrawsLineNumbers()
    -- exposed top row after SD is physically blank: the full line number
    -- must be redrawn (regression: only differing digits were emitted,
    -- leaving "54" instead of "854")
    for _ = 1, 14 do -- viewport 10..14, cursor at 14
        keystroke("j")
    end
    local s
    for _ = 1, 20 do
        s = keystroke("k")
        if ed.scroll_line < 10 then break end
    end
    -- row 0 line number "10" (1-based) fully redrawn: digits 1 and 0
    lu.assertStrContains(s, "\27[1;2H1") -- col 1: digit '1'
    lu.assertStrContains(s, "\27[1;3H0") -- col 2: digit '0'
end

-- ======== Basic operation smoke ========

TestOps = {}

function TestOps:setUp()
    setup()
end

function TestOps:testLineNumbersTrackScroll()
    -- bottom line number must stay correct while scrolling
    for _ = 1, 19 do
        keystroke("j")
    end
    local s = frame()
    lu.assertStrContains(s, "20") -- last line visible, numbered 20
    lu.assertEquals(ed.doc:line(), 19)
end

function TestOps:testInsertAndRender()
    ed.dispatch("i")
    local s = keystroke("x")
    lu.assertStrContains(s, "x")
    ed.dispatch("<Escape>")
end

os.exit(lu.LuaUnit.run(), true)
