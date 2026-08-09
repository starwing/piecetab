-- termfeed Lua binding tests. run: just lua-tf (cwd = repo root)
-- Coverage: termfeed.c binding code; OOM paths (ltf_checkerror TF_ERRMEM)
--   and lookup-alloc failure are not Lua-reachable (defensive, reported).
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/lua/luajit/?.so;"
    or root .. "/lua/?.so;") .. package.cpath

local lu = require "luaunit"
local tf = require "termfeed"

local function feedone(s)
    local t = tf.new()
    t:feed(s)
    local r = t:readkey()
    return t, r
end

-- ======== Lifecycle ========
TestLifecycle = {}

function TestLifecycle:testNew()
    local t = tf.new()
    lu.assertEquals(t:key(), "NONE")
    lu.assertEquals(select("#", t:data()), 0)
    lu.assertEquals(t:format(), "<>") -- KEYSYM NONE, empty name
    lu.assertEquals(t:string(), "")
    lu.assertEquals(t:mod(), "")
end

function TestLifecycle:testDeleteIdempotent()
    local t = tf.new()
    t:delete()
    t:delete()
end

function TestLifecycle:testGC()
    do
        local t = tf.new()
        t:feed("a")
    end
    collectgarbage()
    collectgarbage()
end

function TestLifecycle:testSetflagOld()
    local t = tf.new()
    lu.assertEquals(t:setflag(tf.FLAG_DELBS), 0)
    lu.assertEquals(t:setflag(0), tf.FLAG_DELBS)
end

-- ======== Feed / readkey ========
TestFeed = {}

function TestFeed:testUnicode()
    local t, r = feedone("a")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "UNICODE")
    local s, cp = t:data()
    lu.assertEquals(s, "a")
    lu.assertEquals(cp, 97)
    lu.assertEquals(t:format(), "a")
end

function TestFeed:testUnicodeUtf8()
    local t, r = feedone("\xe4\xb8\xad") -- 中
    lu.assertEquals(r, "KEY")
    local s, cp = t:data()
    lu.assertEquals(s, "\xe4\xb8\xad")
    lu.assertEquals(cp, 0x4E2D)
end

function TestFeed:testPartial()
    local t = tf.new()
    t:feed("\xe4") -- lead byte of 中 (U+4E2D)
    lu.assertEquals(t:readkey(), "AGAIN")
    t:feed("\xb8\xad") -- continuation bytes
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:key(), "UNICODE")
    local s = t:data()
    lu.assertEquals(s, "\xe4\xb8\xad")
end

function TestFeed:testFeedReplacesChunk()
    local t = tf.new()
    t:feed("ab")
    t:feed("c") -- replaces pending chunk
    lu.assertEquals(t:readkey(), "KEY")
    local s = t:data()
    lu.assertEquals(s, "c")
end

function TestFeed:testFeedEmpty()
    local t = tf.new()
    t:feed("")
    lu.assertEquals(t:readkey(), "NONE")
end

function TestFeed:testMultipleKeys()
    local t = tf.new()
    t:feed("\x1b[A\x1b[B")
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:format(), "<Up>")
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:format(), "<Down>")
    lu.assertEquals(t:readkey(), "NONE")
end

-- ======== Key types ========
TestTypes = {}

function TestTypes:testFunction()
    local t, r = feedone("\x1b[15~") -- F5
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "FUNCTION")
    lu.assertEquals(t:data(), 5)
    lu.assertEquals(t:format(), "<F5>")
end

function TestTypes:testKeysym()
    local t, r = feedone("\x1b[A")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "KEYSYM")
    local nm, sym = t:data()
    lu.assertEquals(nm, "Up")
    lu.assertEquals(sym, tf.sym("Up"))
    lu.assertEquals(t:format(), "<Up>")
end

function TestTypes:testMouse()
    local t, r = feedone("\x1b[M\x20\x22\x23") -- X10: btn 0, col 2, line 3
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "MOUSE")
    local ev, btn, line, col = t:data()
    lu.assertEquals(ev, "PRESS")
    lu.assertEquals(btn, 1)
    lu.assertEquals(line, 3)
    lu.assertEquals(col, 2)
    lu.assertEquals(t:format(), "<Mouse>")
end

function TestTypes:testMouseDrag()
    -- code 0x21: 0x20 bit set (drag), pure 1 -> button 2
    local t, r = feedone("\x1b[M\x41\x21\x21")
    lu.assertEquals(r, "KEY")
    local ev, btn, line, col = t:data()
    lu.assertEquals(ev, "DRAG")
    lu.assertEquals(btn, 2)
    lu.assertEquals(line, 1)
    lu.assertEquals(col, 1)
end

function TestTypes:testMouseRelease()
    -- code 3 (raw 0x23): release
    local t, r = feedone("\x1b[M\x23\x21\x21")
    lu.assertEquals(r, "KEY")
    local ev, btn = t:data()
    lu.assertEquals(ev, "RELEASE")
    lu.assertEquals(btn, 0)
end

function TestTypes:testMouseUnknown()
    -- code 0xC0 (raw 0xE0): outside all known ranges
    local t, r = feedone("\x1b[M\xe0\x21\x21")
    lu.assertEquals(r, "KEY")
    local ev, btn = t:data()
    lu.assertEquals(ev, "UNKNOWN")
    lu.assertEquals(btn, 0)
end

function TestTypes:testPosition()
    local t, r = feedone("\x1b[?1;2R")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "POSITION")
    local line, col = t:data()
    lu.assertEquals(line, 1)
    lu.assertEquals(col, 2)
end

function TestTypes:testModereport()
    local t, r = feedone("\x1b[1$y")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "MODEREPORT")
    local init, mode, val = t:data()
    lu.assertEquals(init, 0)
    lu.assertEquals(mode, 1)
    lu.assertEquals(val, -1)
end

function TestTypes:testKittyReport()
    local t, r = feedone("\x1b[?u")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "KITTYREPORT")
    lu.assertEquals(t:data(), -1)
end

function TestTypes:testUnknownCsi()
    local t, r = feedone("\x1b[27;99~")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "UNKNOWN_CSI")
    local d = t:data()
    lu.assertEquals(d.cmd, 126) -- '~'
    lu.assertEquals(d[1], 27)
    lu.assertEquals(d[2], 99)
end

function TestTypes:testUnknownCsiInitial()
    -- '?' initial byte (0x3C-0x3F range)
    local t, r = feedone("\x1b[?999z")
    lu.assertEquals(r, "KEY")
    local d = t:data()
    lu.assertEquals(d.initial, 63) -- '?'
    lu.assertEquals(d.cmd, 122)    -- 'z'
    lu.assertNil(d.intermediate)
end

function TestTypes:testUnknownCsiIntermediate()
    -- '!' intermediate byte (0x20-0x2F range)
    local t, r = feedone("\x1b[!999z")
    lu.assertEquals(r, "KEY")
    local d = t:data()
    lu.assertEquals(d.intermediate, 33) -- '!'
    lu.assertEquals(d.cmd, 122)         -- 'z'
    lu.assertNil(d.initial)
end

function TestTypes:testDCS()
    local t, r = feedone("\x1bPtest\x1b\\")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "DCS")
    lu.assertEquals(t:string(), "test")
    lu.assertEquals(t:data(), "test")
end

function TestTypes:testOSC()
    local t, r = feedone("\x1b]0;title\x07")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "OSC")
    lu.assertEquals(t:string(), "0;title")
end

function TestTypes:testAPC()
    local t, r = feedone("\x1b_foo\x1b\\")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "APC")
    lu.assertEquals(t:string(), "foo")
end

-- ======== Flags ========
TestFlags = {}

function TestFlags:testDelbs()
    local t, r = feedone("\x7f")
    lu.assertEquals(t:format(), "<Delete>")
    t = tf.new()
    t:setflag(tf.FLAG_DELBS)
    t:feed("\x7f")
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:format(), "<Backspace>")
end

function TestFlags:testKeepc0()
    local t, r = feedone("\x01")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "UNICODE")
    lu.assertEquals(t:format(), "<C-a>")
    t = tf.new()
    t:setflag(tf.FLAG_KEEPC0)
    t:feed("\x01")
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:key(), "UNICODE")
    local s, cp = t:data()
    lu.assertEquals(s, "\x01")
    lu.assertEquals(cp, 1)
    lu.assertEquals(t:format(), "\x01")
end

function TestFlags:testSpacesymbol()
    local t, r = feedone(" ")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "UNICODE")
    t = tf.new()
    t:setflag(tf.FLAG_SPACESYMBOL)
    t:feed(" ")
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:key(), "KEYSYM")
    lu.assertEquals(t:format(), "<Space>")
end

function TestFlags:testConvertkp()
    local t, r = feedone("\x1bOj")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "KEYSYM")
    lu.assertEquals(t:format(), "<kMultiply>")
    t = tf.new()
    t:setflag(tf.FLAG_CONVERTKP)
    t:feed("\x1bOj")
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:key(), "UNICODE")
    local s, cp = t:data()
    lu.assertEquals(s, "*")
    lu.assertEquals(cp, 42)
end

-- ======== Modifiers ========
TestMod = {}

function TestMod:testNoArg()
    local t, r = feedone("\x1b[1;2A") -- S-Up
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:mod(), "S")
    lu.assertEquals(t:format(), "<S-Up>")
end

function TestMod:testLetters()
    local t, r = feedone("\x1b[1;2A")
    lu.assertTrue(t:mod("S"))
    lu.assertFalse(t:mod("C"))
    lu.assertFalse(t:mod("A"))
    lu.assertFalse(t:mod("D"))
    lu.assertFalse(t:mod("T"))
    lu.assertFalse(t:mod("H"))
    -- empty string is the no-arg form: returns modifier string
    lu.assertEquals(t:mod(""), "S")
end

function TestMod:testAltCtrl()
    local t, r = feedone("\x1b[1;5C") -- C-Right
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:mod(), "C")
    lu.assertTrue(t:mod("c"))
    t = tf.new()
    t:feed("\x1ba") -- M-a
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:mod(), "A")
    lu.assertTrue(t:mod("m"))
    lu.assertTrue(t:mod("M"))
    lu.assertTrue(t:mod("a"))
end

function TestMod:testUnknown()
    local t = tf.new()
    t:feed("a")
    t:readkey()
    lu.assertNil(t:mod("Z"))
end

function TestMod:testExtraModifiers()
    -- modifyOtherKeys: \e[27;<mods+1>;<code>~
    local function mods_of(m, code)
        local t = tf.new()
        t:feed(string.format("\x1b[27;%d;%d~", m, code))
        lu.assertEquals(t:readkey(), "KEY")
        return t:mod()
    end
    lu.assertEquals(mods_of(10, 97), "SD")  -- SHIFT | SUPER
    lu.assertEquals(mods_of(7, 97), "AC")   -- ALT | CTRL
    lu.assertEquals(mods_of(33, 97), "T")   -- META
    lu.assertEquals(mods_of(17, 97), "H")   -- HYPER
    local t = tf.new()
    t:feed("\x1b[27;9;97~") -- SUPER
    t:readkey()
    lu.assertTrue(t:mod("d"))
    lu.assertFalse(t:mod("s"))
    lu.assertFalse(t:mod("h"))
    lu.assertFalse(t:mod("t"))
end

-- ======== Format ========
TestFormat = {}

function TestFormat:testDefaultVim()
    local t, r = feedone("\x1b[A")
    lu.assertEquals(t:format(), "<Up>")
    lu.assertEquals(t:format(tf.FORMAT_VIM), "<Up>")
    lu.assertEquals(t:format(tf.FORMAT_VIM + tf.FORMAT_LONGMOD), "<Up>")
end

function TestFormat:testCaretCtrl()
    local t, r = feedone("\x01")
    lu.assertEquals(t:format(tf.FORMAT_CARETCTRL), "^A")
end

function TestFormat:testSpacemod()
    local t, r = feedone("\x1ba")
    lu.assertEquals(t:format(tf.FORMAT_SPACEMOD), "A a")
    lu.assertEquals(t:format(), "<M-a>")
end

-- ======== Parse ========
TestParse = {}

function TestParse:testKeysym()
    local t = tf.new()
    local ok, n = t:parse("<Up>")
    lu.assertTrue(ok)
    lu.assertEquals(n, 4)
    lu.assertEquals(t:format(), "<Up>")
    lu.assertEquals(t:key(), "KEYSYM")
end

function TestParse:testPlain()
    local t = tf.new()
    local ok, n = t:parse("a")
    lu.assertTrue(ok)
    lu.assertEquals(n, 1)
    lu.assertEquals(t:key(), "UNICODE")
end

function TestParse:testMods()
    local t = tf.new()
    local ok = t:parse("<C-S-Up>")
    lu.assertTrue(ok)
    lu.assertEquals(t:mod(), "SC")
    -- termfeed formats Shift before Ctrl
    lu.assertEquals(t:format(), "<S-C-Up>")
end

function TestParse:testFailures()
    local t = tf.new()
    lu.assertNil(t:parse(""))
    lu.assertNil(t:parse("<>"))
    lu.assertNil(t:parse("<foo>"))
    lu.assertNil(t:parse("\xc3")) -- dangling lead byte
end

-- ======== Name / sym ========
TestNames = {}

function TestNames:testRoundtrip()
    lu.assertEquals(tf.name(tf.sym("Up")), "Up")
    lu.assertEquals(tf.name(tf.sym("PageDown")), "PageDown")
end

function TestNames:testInvalid()
    lu.assertNil(tf.name(0))
    lu.assertNil(tf.name(9999))
    lu.assertNil(tf.sym("nope"))
end

function TestNames:testConstants()
    lu.assertEquals(tf.FLAG_KEEPC0, 1)
    lu.assertEquals(tf.FLAG_CONVERTKP, 2)
    lu.assertEquals(tf.FLAG_SPACESYMBOL, 4)
    lu.assertEquals(tf.FLAG_DELBS, 8)
    lu.assertEquals(tf.FORMAT_LONGMOD, 1)
    lu.assertEquals(tf.FORMAT_CARETCTRL, 2)
    lu.assertEquals(tf.FORMAT_ALTISMETA, 4)
    lu.assertEquals(tf.FORMAT_WRAPBRACKET, 8)
    lu.assertEquals(tf.FORMAT_SPACEMOD, 16)
    lu.assertEquals(tf.FORMAT_LOWERMOD, 32)
    lu.assertEquals(tf.FORMAT_LOWERSPACE, 64)
    lu.assertEquals(tf.FORMAT_VIM, 12)
end

-- ======== Flush ========
TestFlush = {}

function TestFlush:testPartialEscape()
    local t = tf.new()
    t:feed("\x1b[")
    lu.assertEquals(t:readkey(), "AGAIN") -- CSI partial: bytes consumed
    lu.assertEquals(t:flush(), "KEY")
    lu.assertEquals(t:format(), "<M-[>") -- ALT, vim-style M- token
    lu.assertEquals(t:mod(), "A")
end

function TestFlush:testIdle()
    local t = tf.new()
    lu.assertEquals(t:flush(), "KEY")
    lu.assertEquals(t:format(), "<>") -- KEYSYM NONE, empty name
    -- KEYSYM NONE: data() yields nil name
    local nm, sym = t:data()
    lu.assertNil(nm)
    lu.assertEquals(sym, 0)
end

function TestFlush:testEscape()
    local t = tf.new()
    t:feed("\x1b")
    lu.assertEquals(t:readkey(), "AGAIN") -- ESC consumed, awaiting prefix
    lu.assertEquals(t:flush(), "KEY")
    lu.assertEquals(t:format(), "<Escape>")
end

-- ======== Setlookup (terminfo) ========
TestLookup = {}

function TestLookup:testNone()
    local t, r = feedone("\x1b[27;99~")
    lu.assertEquals(r, "KEY")
    lu.assertEquals(t:key(), "UNKNOWN_CSI")
end

function TestLookup:testCallback()
    local t = tf.new()
    t:setlookup(function(name)
        if name == "key_home" then return "\x1b[7~" end
    end)
    t:feed("\x1b[7~")
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:format(), "<Home>")
    lu.assertEquals(t:key(), "KEYSYM")
end

function TestLookup:testLookupError()
    local t = tf.new()
    lu.assertErrorMsgContains("boom",
        function() t:setlookup(function() error("boom") end) end)
end

function TestLookup:testClear()
    local t = tf.new()
    t:setlookup(function(name)
        if name == "key_home" then return "\x1b[7~" end
    end)
    t:setlookup(nil)
    t:feed("\x1b[27;99~")
    lu.assertEquals(t:readkey(), "KEY")
    lu.assertEquals(t:key(), "UNKNOWN_CSI")
end

-- ======== Waitkey ========
TestWaitkey = {}

local function filefd(path, mode)
    local f = assert(io.open(path, mode))
    local ok, fd = pcall(function() return f["fd"](f) end)
    if not ok then return nil end
    return f, fd
end

function TestWaitkey:testReadFile()
    local path = os.tmpname()
    local wf = assert(io.open(path, "w"))
    wf:write("\x1b[A\x1b[B")
    wf:close()
    local f, fd = filefd(path, "r")
    if fd then
        local t = tf.new()
        lu.assertEquals(t:waitkey(fd, 0), "KEY")
        lu.assertEquals(t:format(), "<Up>")
        lu.assertEquals(t:waitkey(fd, 0), "KEY")
        lu.assertEquals(t:format(), "<Down>")
        lu.assertEquals(t:waitkey(fd, 0), "NONE")
        f:close()
    end
    os.remove(path)
end

function TestWaitkey:testEof()
    local f, fd = filefd("/dev/null", "r")
    if fd then
        local t = tf.new()
        lu.assertEquals(t:waitkey(fd, 0), "NONE")
        f:close()
    end
end

function TestWaitkey:testKeyReady()
    -- pre-fed key: readkey consumes before poll, any fd works
    local t = tf.new()
    t:feed("a")
    lu.assertEquals(t:waitkey(1, 0), "KEY")
    lu.assertEquals(t:format(), "a")
end

function TestWaitkey:testBadFd()
    local t = tf.new()
    lu.assertErrorMsgContains("invalid parameter",
        function() t:waitkey(-1, 0) end)
end

-- ======== Raw / cooked (tty only) ========
TestRaw = {}

function TestRaw:testBadFd()
    local t = tf.new()
    lu.assertErrorMsgContains("tcgetattr",
        function() t:raw(999999) end)
end

function TestRaw:testCookedNoop()
    local t = tf.new()
    t:cooked()
    t:cooked()
end

function TestRaw:testTty()
    local f = io.open("/dev/tty", "r")
    if not f then return end
    local ok, fd = pcall(function() return f["fd"](f) end)
    f:close()
    if not ok then return end
    local t = tf.new()
    t:raw(fd)
    t:cooked()
    t:raw(fd) -- raw twice; delete must restore
    t:delete()
end

os.exit(lu.LuaUnit.run(), true)
