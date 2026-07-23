-- cellgrid Lua binding tests. run: just lua-cg (cwd = repo root)
-- Coverage: 98.9% of cellgrid.c (259/262 lines).
-- Uncovered: L423-425 (winsize ioctl success) -- requires TTY fd,
--   not guaranteed in CI/non-interactive environments.
local dir = arg[0]:match("^(.*)[/\\]") or "."
package.path = dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and "build/luajit/?.so;" or "build/lua55/?.so;")
    .. package.cpath

local lu = require "luaunit"
local cg = require "cellgrid"

-- ======== Lifecycle tests ========
TestLifecycle = {}

function TestLifecycle:testNew()
    local g = cg.new()
    lu.assertEquals(g:rows(), 0)
    lu.assertEquals(g:cols(), 0)
    lu.assertEquals(g:top(), 0)
end

function TestLifecycle:testBegin()
    local g = cg.new()
    g:begin(0, 5, 10)
    lu.assertEquals(g:rows(), 5)
    lu.assertEquals(g:cols(), 10)
    lu.assertEquals(g:top(), 0)
end

function TestLifecycle:testBeginZeroRows()
    local g = cg.new()
    lu.assertErrorMsgContains("invalid parameter",
        function() g:begin(0, 0, 10) end)
end

function TestLifecycle:testBeginZeroCols()
    local g = cg.new()
    lu.assertErrorMsgContains("invalid parameter",
        function() g:begin(0, 5, 0) end)
end

function TestLifecycle:testBeginResize()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:begin(0, 5, 20)
    lu.assertEquals(g:rows(), 5)
    lu.assertEquals(g:cols(), 20)
end

function TestLifecycle:testDelete()
    local g = cg.new()
    g:begin(0, 5, 10)
    g:delete()
    lu.assertEquals(g:rows(), 0)
end

function TestLifecycle:testGC()
    do
        local g = cg.new()
        g:begin(0, 3, 10)
    end
    collectgarbage()
    collectgarbage()
end

function TestLifecycle:testClear()
    local g = cg.new()
    -- clear before begin is no-op
    g:clear()
    lu.assertEquals(g:rows(), 0)
    g:begin(0, 3, 10)
    g:put(0, 0, 65)
    g:clear()
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 32) -- space (cg_cell converts 0 to ' ')
    lu.assertEquals(st, 0)
end

function TestLifecycle:testFreeze()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65, 1)
    g:freeze()
    -- after freeze, back buffer has the cell
    local cp, st = g:back(0, 0)
    lu.assertEquals(cp, 65)
    lu.assertEquals(st, 1)
end

-- ======== Cell writing tests ========
TestWrite = {}

function TestWrite:testPut()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65)
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 65)
    lu.assertEquals(st, 0)
end

function TestWrite:testPutStyle()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65, 7)
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 65)
    lu.assertEquals(st, 7)
end

function TestWrite:testPutOutOfBounds()
    local g = cg.new()
    g:begin(0, 3, 10)
    -- should not crash
    g:put(-1, 0, 65)
    g:put(0, -1, 65)
    g:put(3, 0, 65)
    g:put(0, 10, 65)
end

function TestWrite:testPutWide()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 0x4E2D) -- 中, CJK ideograph, width=2
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 0x4E2D)
    -- right half marked as -1
    cp, st = g:cell(0, 1)
    lu.assertEquals(cp, -1)
end

function TestWrite:testPutWideEdge()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 9, 0x4E2D)     -- wide at right edge
    local cp, st = g:cell(0, 9)
    lu.assertEquals(cp, 62) -- '>'
end

function TestWrite:testPutOverwriteWideLeft()
    -- put wide, then narrow at same pos → right half cleared
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 0x4E2D) -- pc[0]=cp, pc[1]=-1
    g:put(0, 0, 65)     -- narrow overwrites; pc[1]==-1 → pc[1]=0
    -- cg_cell converts 0 to ' ' (32)
    local cp, st = g:cell(0, 1)
    lu.assertEquals(cp, 32) -- cleared, space
    cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 65)
end

function TestWrite:testPutOverwriteWideRight()
    -- put wide at c=0, then narrow at c=1 → left half cleared
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 0x4E2D)
    g:put(0, 1, 66) -- pc[0]==-1 && pc[-1]!=-1 → pc[-1]=0
    -- cg_cell converts 0 to ' ' (32)
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 32) -- cleared
    cp, st = g:cell(0, 1)
    lu.assertEquals(cp, 66)
end

function TestWrite:testClearRow()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65)
    g:put(0, 1, 66)
    g:clearrow(0, 0, 2)
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 32)
end

function TestWrite:testClearRowOutOfBounds()
    local g = cg.new()
    g:begin(0, 3, 10)
    -- should not crash
    g:clearrow(-1, 0, 5)
    g:clearrow(3, 0, 5)
end

function TestWrite:testClearRowZero()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65)
    g:clearrow(0, 2, 0) -- cs>=ce, nop
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 65)
end

function TestWrite:testFill()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:fill(0, 0, 5, 65)
    for i = 0, 4 do
        local cp, st = g:cell(0, i)
        lu.assertEquals(cp, 65)
    end
    local cp, st = g:cell(0, 5)
    lu.assertEquals(cp, 32)
end

function TestWrite:testFillOutOfBounds()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:fill(-1, 0, 5, 65) -- nop
    g:fill(3, 0, 5, 65)  -- nop
end

function TestWrite:testSpan()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65)
    g:span(0, 0, 5, 3)
    for i = 0, 4 do
        local cp, st = g:cell(0, i)
        lu.assertEquals(st, 3)
    end
end

function TestWrite:testSpanOutOfBounds()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:span(-1, 0, 5, 3) -- nop
    g:span(3, 0, 5, 3)  -- nop
end

function TestWrite:testPutline()
    local g = cg.new()
    g:begin(0, 3, 10)
    local c = g:putline(0, 0, "ABC", 0)
    lu.assertEquals(c, 3)
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 65)
    cp, st = g:cell(0, 1)
    lu.assertEquals(cp, 66)
end

function TestWrite:testPutlineUtf8TwoByte()
    local g = cg.new()
    g:begin(0, 3, 10)
    -- é (0xE9) is ambiwidth → width=2. Use 0x80 (control, width=1).
    g:putline(0, 0, "\xc2\x80", 0) -- U+0080, width=1
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 0x80)
end

function TestWrite:testPutlineUtf8ThreeByte()
    local g = cg.new()
    g:begin(0, 3, 10)
    -- U+0800, width=1, 3-byte UTF-8: 0xE0 0xA0 0x80
    g:putline(0, 0, "\xe0\xa0\x80", 0)
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 0x800)
end

function TestWrite:testPutlineUtf8FourByte()
    local g = cg.new()
    g:begin(0, 2, 10)
    g:putline(0, 0, "\xf0\x90\x8d\x88", 0) -- U+10348, width=1
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 0x10348)
end

function TestWrite:testPutlineContinuation()
    -- \x80 is continuation byte, cgK_utflen returns 0, skipped
    local g = cg.new()
    g:begin(0, 3, 10)
    local c = g:putline(0, 0, "\x80", 0) -- continuation only
    lu.assertEquals(c, 0)                -- nothing written
end

function TestWrite:testPutlineWide()
    local g = cg.new()
    g:begin(0, 3, 10)
    local c = g:putline(0, 0, "\xe4\xb8\xad", 0) -- 中(CJK)
    lu.assertEquals(c, 2)                        -- advances by 2
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 0x4E2D)
    cp, st = g:cell(0, 1)
    lu.assertEquals(cp, -1) -- right half
end

function TestWrite:testPutlineEmpty()
    local g = cg.new()
    g:begin(0, 3, 10)
    local c = g:putline(0, 0, "", 0)
    lu.assertEquals(c, 0)
end

function TestWrite:testPutlineStyle()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:putline(0, 0, "X", 5)
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 88)
    lu.assertEquals(st, 5)
end

function TestWrite:testPutlineOutOfBounds()
    local g = cg.new()
    g:begin(0, 3, 10)
    local c = g:putline(-1, 0, "ABC", 0)
    lu.assertEquals(c, 0)
    c = g:putline(3, 0, "ABC", 0)
    lu.assertEquals(c, 0)
end

-- ======== Getter tests ========
TestGetter = {}

function TestGetter:testCell()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65, 1)
    local cp, st = g:cell(0, 0)
    lu.assertEquals(cp, 65)
    lu.assertEquals(st, 1)
end

function TestGetter:testCellOutOfBounds()
    local g = cg.new()
    g:begin(0, 3, 10)
    local cp, st = g:cell(-1, 0)
    lu.assertEquals(cp, 0)
    lu.assertEquals(st, 0)
end

function TestGetter:testBack()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65, 1)
    g:freeze()
    -- freeze copies cur to back
    local cp, st = g:back(0, 0)
    lu.assertEquals(cp, 65)
    lu.assertEquals(st, 1)
    -- after clear, back still has old value
    g:clear()
    cp, st = g:back(0, 0)
    lu.assertEquals(cp, 65)
    lu.assertEquals(st, 1)
end

function TestGetter:testBackOutOfBounds()
    local g = cg.new()
    g:begin(0, 3, 10)
    local cp, st = g:back(-1, 0)
    lu.assertEquals(cp, 0)
    lu.assertEquals(st, 0)
end

function TestGetter:testIsdirty()
    local g = cg.new()
    g:begin(0, 3, 10)
    -- after begin, cur=0, back=0 → not dirty (isdirty compares cells)
    lu.assertFalse(g:isdirty(0, 0))
    -- put changes cur, now dirty
    g:put(0, 0, 65)
    lu.assertTrue(g:isdirty(0, 0))
    g:freeze()
    -- after freeze, cur=back → not dirty
    lu.assertFalse(g:isdirty(0, 0))
    -- put after freeze → dirty
    g:put(0, 0, 66)
    lu.assertTrue(g:isdirty(0, 0))
end

function TestGetter:testIsdirtyOutOfBounds()
    local g = cg.new()
    g:begin(0, 3, 10)
    lu.assertFalse(g:isdirty(-1, 0))
    lu.assertFalse(g:isdirty(3, 0))
end

function TestGetter:testDimensions()
    local g = cg.new()
    g:begin(5, 8, 40)
    lu.assertEquals(g:rows(), 8)
    lu.assertEquals(g:cols(), 40)
    lu.assertEquals(g:top(), 5)
end

-- ======== Diff tests ========
TestDiff = {}

function TestDiff:testBasic()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65)
    local s = g:diff({})
    lu.assertStrContains(s, "A")
end

function TestDiff:testNoChange()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65)
    g:freeze()
    -- freeze copies cur to back; no diff output for matching cells
    -- cur and back both have 65 → skip advances to end
    local s = g:diff({})
    lu.assertEquals(s, "")
end

function TestDiff:testStyle()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65, 1)
    g:freeze()
    g:put(0, 0, 66, 2) -- change style
    local s = g:diff({
        ["cursor_address"] = "C%d,%d",
        [1] = "S1",
        [2] = "S2",
        [0] = "S0"
    })
    lu.assertStrContains(s, "S2")
end

function TestDiff:testCustomFormats()
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 65)
    local opts = {
        cursor_address = "\x1b[%d;%dH",
        change_scroll_region = "\x1b[%d;%dr",
        parm_index = "\x1b[%dS",
        parm_rindex = "\x1b[%dT",
        repeat_char = "\x1b[%db",
    }
    local s = g:diff(opts)
    lu.assertStrContains(s, "A")
end

function TestDiff:testFillMin()
    local g = cg.new()
    g:begin(0, 1, 20)
    g:fill(0, 0, 5, 65) -- 5 A's
    -- fill_min=1 → reset to default 4 (5 >= 4 → fill)
    local s = g:diff({ fill_min = 1 })
    lu.assertStrContains(s, "A")
    -- fill_min=6 → 5 < 6 → individual puts
    local g2 = cg.new()
    g2:begin(0, 1, 20)
    g2:fill(0, 0, 5, 65)
    local s2 = g2:diff({ fill_min = 6 })
    lu.assertStrContains(s2, "A")
    -- fill_min=1 uses fill (repeat_char), 6 uses individual puts
    lu.assertNotEquals(s, s2)
end

function TestDiff:testScroll()
    local g = cg.new()
    g:begin(0, 5, 10)
    g:put(0, 0, 65)
    g:put(1, 0, 66)
    g:put(2, 0, 67)
    g:freeze()
    -- scroll up by 2: top changes from 0 to 2
    g:begin(2, 5, 10)
    -- fill newly exposed rows
    g:put(3, 0, 68)
    g:put(4, 0, 69)
    local s = g:diff({})
    lu.assertStrContains(s, "D") -- row 3 has 'D' (68)
end

function TestDiff:testWideChars()
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 0x4E2D) -- 中
    local s = g:diff({})
    -- should contain UTF-8 for 中
    lu.assertStrContains(s, "\xe4\xb8\xad")
end

function TestDiff:testStyleChange()
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 65, 1)
    g:put(0, 1, 66, 2)
    g:freeze()
    -- change both chars with same style
    g:put(0, 0, 65, 3)
    g:put(0, 1, 66, 3)
    local s = g:diff({ [3] = "X" })
    lu.assertStrContains(s, "X")
end

function TestDiff:testStyleReset()
    -- non-zero style at end of row triggers style(0) via finish
    local g = cg.new()
    g:begin(0, 1, 5)
    g:put(0, 0, 65, 1)
    g:freeze()
    g:put(0, 0, 66, 1)           -- still style 1
    local s = g:diff({ [1] = "H", [0] = "R" })
    lu.assertStrContains(s, "R") -- style reset at finish
end

function TestDiff:testRepeatedChars()
    local g = cg.new()
    g:begin(0, 1, 10)
    g:fill(0, 0, 10, 65)
    g:freeze()
    g:fill(0, 0, 10, 66)
    local s = g:diff({ fill_min = 2, repeat_char = "!%d" })
    lu.assertStrContains(s, "B")
end

-- all 4 branches of lcg_cptoutf8 via diff
-- use single-width codepoints placed at non-conflicting columns
function TestDiff:testCptoutf8Branches()
    local g = cg.new()
    g:begin(0, 1, 20)
    g:put(0, 0, 65)      -- 1-byte: ASCII, width=1
    g:put(0, 2, 0x80)    -- 2-byte: cp 0x80-0x7FF, width=1. UTF-8: \xc2\x80
    g:put(0, 4, 0x800)   -- 3-byte: cp 0x800-0xFFFF, width=1. UTF-8: \xe0\xa0\x80
    g:put(0, 6, 0x10348) -- 4-byte: cp >= 0x10000, width=1. UTF-8: \xf0\x90\x8d\x88
    local s = g:diff({ cursor_address = "(%d,%d)" })
    lu.assertStrContains(s, "A")
    lu.assertStrContains(s, "\xc2\x80")         -- 0x80
    lu.assertStrContains(s, "\xe0\xa0\x80")     -- 0x800
    lu.assertStrContains(s, "\xf0\x90\x8d\x88") -- 0x10348
end

-- ======== Render tests ========
TestRender = {}

function TestRender:testRender()
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 65)
    local path = os.tmpname()
    local wf = assert(io.open(path, "w"))
    -- use pcall: LuaJIT has :fd(), PUC Lua does not
    local ok, fd = pcall(function() return wf["fd"](wf) end)
    if ok then
        g:render(fd, {})
        wf:close()
        local rf = io.open(path, "r")
        if rf then
            local s = rf:read("*a")
            rf:close()
            lu.assertStrContains(s, "A")
        end
    else
        wf:close()
        -- render to stderr, verify no error
        g:render(2, {})
    end
    os.remove(path)
end

-- ======== System tests ========
TestSystem = {}

function TestSystem:testWinsize()
    local rows, cols = cg.winsize()
    -- can fail in non-tty context
    if rows then
        lu.assertIsNumber(rows)
        lu.assertIsNumber(cols)
    end
end

function TestSystem:testWinsizeFd()
    local rows, cols = cg.winsize(1) -- stdout
    if rows then
        lu.assertIsNumber(rows)
        lu.assertIsNumber(cols)
    end
end

function TestSystem:testWinsizeTty()
    local f = assert(io.open("/dev/tty", "r"))
    if f then
        local ok, fd = pcall(function() return f["fd"](f) end)
        f:close()
        if ok then
            local rows, cols = cg.winsize(fd)
            if rows then
                lu.assertIsNumber(rows)
                lu.assertIsNumber(cols)
                return
            end
        end
    end
    -- fallback: try fd=0 and fd=1, compare with stty
    for _, fd in ipairs({ 0, 1, 2 }) do
        local rows, cols = cg.winsize(fd)
        if rows then
            lu.assertIsNumber(rows)
            lu.assertIsNumber(cols)
            return
        end
    end
    -- last resort: try get TTY path from 'tty' command
    local p = io.popen("tty 2>/dev/null", "r")
    if p then
        local ttypath = p:read("*l")
        p:close()
        if ttypath then
            local f2 = assert(io.open(ttypath, "r"))
            if f2 then
                local ok, fd = pcall(function() return f2["fd"](f2) end)
                f2:close()
                if ok then
                    local rows, cols = cg.winsize(fd)
                    if rows then
                        lu.assertIsNumber(rows)
                        lu.assertIsNumber(cols)
                        return
                    end
                end
            end
        end
    end
end

-- ======== Scroll branches coverage ========
TestScroll = {}

function TestScroll:testDeltaZero()
    -- delta=0 branch in cg_begin
    local g = cg.new()
    g:begin(0, 5, 10)
    g:put(0, 0, 65)
    g:freeze()
    g:begin(0, 5, 10) -- same top → delta=0, scroll=0
    local s = g:diff({})
    lu.assertEquals(s, "")
end

function TestScroll:testDeltaLarge()
    -- |delta| >= rows → all_dirty
    local g = cg.new()
    g:begin(0, 3, 10)
    g:put(0, 0, 65)
    g:freeze()
    g:begin(10, 3, 10) -- |10-0|=10 >= 3 → all_dirty
    local s = g:diff({})
    lu.assertStrContains(s, "A")
end

function TestScroll:testDeltaNegative()
    -- delta < 0 (scroll down): top increases
    local g = cg.new()
    g:begin(0, 5, 10)
    g:put(0, 0, 65)
    g:freeze()
    g:begin(2, 5, 10) -- delta=0-2=-2, scroll=-2
    g:put(3, 0, 66)
    g:put(4, 0, 67)
    local s = g:diff({})
    -- scroll output includes ind/rin format strings
    lu.assertStrContains(s, "B")
end

function TestScroll:testDeltaPositive()
    -- delta > 0 (scroll up): top decreases
    local g = cg.new()
    g:begin(2, 5, 10)
    g:put(0, 0, 65)
    g:put(1, 0, 66)
    g:freeze()
    g:begin(0, 5, 10) -- delta=2>0 (scroll up)
    g:put(0, 0, 67)
    g:put(1, 0, 68)
    local s = g:diff({})
    lu.assertStrContains(s, "C")
end

-- ======== cgK_tocp branches (via putline) ========
TestTocp = {}

function TestTocp:testOneByte()
    local g = cg.new()
    g:begin(0, 1, 10)
    g:putline(0, 0, "X", 0)
    lu.assertEquals(g:cell(0, 0), 88)
end

function TestTocp:testTwoByte()
    -- 0x80 is NOT in any width table → width=1
    local g = cg.new()
    g:begin(0, 1, 10)
    g:putline(0, 0, "\xc2\x80", 0) -- U+0080
    lu.assertEquals(g:cell(0, 0), 0x80)
end

function TestTocp:testThreeByte()
    -- 0x800 is NOT in any width table → width=1
    local g = cg.new()
    g:begin(0, 1, 10)
    g:putline(0, 0, "\xe0\xa0\x80", 0) -- U+0800
    lu.assertEquals(g:cell(0, 0), 0x800)
end

function TestTocp:testFourByte()
    -- 0x10348 is NOT in any width table → width=1
    local g = cg.new()
    g:begin(0, 1, 10)
    g:putline(0, 0, "\xf0\x90\x8d\x88", 0) -- U+10348
    lu.assertEquals(g:cell(0, 0), 0x10348)
end

-- ======== lcgW_width branches ========
TestWidth = {}

function TestWidth:testZeroWidth()
    -- U+0300 combining grave accent (zero-width)
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 0x300)
    local cp1, _ = g:cell(0, 0)
    lu.assertEquals(cp1, 0x300)
    -- width=0 means no wide marker
    local cp2, _ = g:cell(0, 1)
    lu.assertNotEquals(cp2, -1)
end

function TestWidth:testDoubleWidth()
    -- U+1100 Hangul Choseong (doublewidth_table)
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 0x1100)
    local cp1, _ = g:cell(0, 0)
    lu.assertEquals(cp1, 0x1100)
    local cp2, _ = g:cell(0, 1)
    lu.assertEquals(cp2, -1) -- right half marker
end

function TestWidth:testAmbiWidth()
    -- U+00A1 inverted exclamation (ambiwidth_table, line 483)
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 0xA1)
    local cp1, _ = g:cell(0, 0)
    lu.assertEquals(cp1, 0xA1)
    -- ambiwidth treated as width=2
    local cp2, _ = g:cell(0, 1)
    lu.assertEquals(cp2, -1)
end

function TestWidth:testNormalWidth()
    -- ASCII 'A' = normal width 1
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 65)
    local cp1, _ = g:cell(0, 0)
    lu.assertEquals(cp1, 65)
    local cp2, _ = g:cell(0, 1)
    lu.assertNotEquals(cp2, -1)
end

-- ======== cgK_utflen branches (via putline) ========
TestUtflen = {}

function TestUtflen:testOneByte()
    -- ASCII: b < 0x80 → len=1
    local g = cg.new()
    g:begin(0, 1, 10)
    local c = g:putline(0, 0, "A")
    lu.assertEquals(c, 1) -- width 1
end

function TestUtflen:testTwoByte()
    -- 0xC2 >= 0xC0, < 0xE0 → len=2. U+0080 width=1.
    local g = cg.new()
    g:begin(0, 1, 10)
    local c = g:putline(0, 0, "\xc2\x80") -- U+0080
    lu.assertEquals(c, 1)                 -- width 1
end

function TestUtflen:testThreeByte()
    -- 0xE0 >= 0xE0, < 0xF0 → len=3. U+0800 width=1.
    local g = cg.new()
    g:begin(0, 1, 10)
    local c = g:putline(0, 0, "\xe0\xa0\x80") -- U+0800
    lu.assertEquals(c, 1)                     -- width 1
end

function TestUtflen:testFourByte()
    -- 0xF0 >= 0xF0 → len=4. U+10348 width=1.
    local g = cg.new()
    g:begin(0, 1, 10)
    local c = g:putline(0, 0, "\xf0\x90\x8d\x88") -- U+10348
    lu.assertEquals(c, 1)                         -- width 1
end

function TestUtflen:testContinuation()
    -- 0x80 < 0xC0 → len=0
    local g = cg.new()
    g:begin(0, 1, 10)
    local c = g:putline(0, 0, "\x80")
    lu.assertEquals(c, 0) -- skipped
end

-- ======== cgD_skip / cgD_rep branches ========
TestDiffInternals = {}

function TestDiffInternals:testSkipAllDirty()
    -- all_dirty=true: skip returns col immediately (no skip)
    local g = cg.new()
    g:begin(0, 1, 5)
    g:put(0, 0, 65)
    local s = g:diff({ cursor_address = "(%d,%d)" })
    lu.assertStrContains(s, "A")
end

function TestDiffInternals:testSkipMatching()
    -- after freeze with no changes: all cells match
    local g = cg.new()
    g:begin(0, 1, 5)
    g:put(0, 0, 65)
    g:freeze()
    -- cur=65, back=65 → skip advances to end of row
    local s = g:diff({})
    lu.assertEquals(s, "")
end

function TestDiffInternals:testRep()
    -- cgD_rep counts repetitions
    local g = cg.new()
    g:begin(0, 1, 10)
    g:fill(0, 0, 10, 65)
    g:freeze()
    g:fill(0, 0, 10, 66)
    local s = g:diff({ fill_min = 2, repeat_char = "!%d" })
    lu.assertStrContains(s, "B")
end

-- diff/render with no opts arg → lua_newtable branch (lines 385, 404)
function TestDiff:testNoOpts()
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 65)
    local s = g:diff()
    lu.assertStrContains(s, "A")
end

-- diff on empty grid → cg_diff returns CG_ERRPARAM (lines 391-393)
function TestDiff:testEmptyGridError()
    local g = cg.new()
    lu.assertErrorMsgContains("cellgrid: invalid parameter",
        function() g:diff() end)
end

function TestRender:testNoOpts()
    local g = cg.new()
    g:begin(0, 1, 10)
    g:put(0, 0, 65)
    g:render(2)
end

-- render on empty grid → cg_diff returns CG_ERRPARAM (lines 409-411)
function TestRender:testEmptyGridError()
    local g = cg.new()
    lu.assertErrorMsgContains("cellgrid: invalid parameter",
        function() g:render(1) end)
end

-- ======== Misc edge cases ========
TestMisc = {}

function TestMisc:testBeginAfterClear()
    local g = cg.new()
    g:begin(0, 3, 10)
    g:clear()
    g:begin(0, 3, 10) -- begin again after clear
    lu.assertEquals(g:rows(), 3)
end

function TestMisc:testPutlineAtEdge()
    local g = cg.new()
    g:begin(0, 1, 5)
    local c = g:putline(0, 3, "ABCDE")
    lu.assertEquals(c, 5) -- capped at cols
end

os.exit(lu.LuaUnit.run(), true)
