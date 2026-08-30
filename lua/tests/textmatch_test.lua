-- textmatch Lua binding tests. run: just lua/tm
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/lua/luajit/?.so;"
    or root .. "/lua/?.so;") .. package.cpath

local lu = require "luaunit"
local tm = require "textmatch"
local pt = require "piecetab"

-- string find ------------------------------------------------------------

TestFindString = {}

function TestFindString:testBasic()
    lu.assertEquals({ tm.find("abc", "b") }, { 2, 2 })
    lu.assertEquals({ tm.find("hello world", "world") }, { 7, 11 })
    lu.assertIsNil(tm.find("abc", "z"))
end

function TestFindString:testInit()
    lu.assertEquals({ tm.find("abc", "b", 2) }, { 2, 2 })
    lu.assertIsNil(tm.find("abc", "b", 3))
    lu.assertIsNil(tm.find("abc", "b", -1))
    lu.assertEquals({ tm.find("abc", "b", -2) }, { 2, 2 })
    lu.assertEquals({ tm.find("abc", "b", 0) }, { 2, 2 })
end

function TestFindString:testPlain()
    lu.assertIsNil(tm.find("abc", "a.c", 1, true))
    lu.assertEquals({ tm.find("abc", "a.c") }, { 1, 3 })
    lu.assertEquals({ tm.find("a.c", "a.c", 1, true) }, { 1, 3 })
end

function TestFindString:testCaptures()
    local a, b, c, d = tm.find("hello world", "(%a+) (%a+)")
    lu.assertEquals(a, 1)
    lu.assertEquals(b, 11)
    lu.assertEquals(c, "hello")
    lu.assertEquals(d, "world")
end

function TestFindString:testPositionCapture()
    local a, b, c = tm.find("abc", "()b")
    lu.assertEquals(a, 2)
    lu.assertEquals(b, 2)
    lu.assertEquals(c, 2)
end

-- string match -----------------------------------------------------------

TestMatchString = {}

function TestMatchString:testWholeMatch()
    lu.assertEquals(tm.match("hello world", "%a+"), "hello")
    lu.assertIsNil(tm.match("abc", "z"))
end

function TestMatchString:testCaptures()
    local a, b = tm.match("hello world", "(%a+) (%a+)")
    lu.assertEquals(a, "hello")
    lu.assertEquals(b, "world")
end

function TestMatchString:testPositionCapture()
    lu.assertEquals(tm.match("abc", "()b"), 2)
    lu.assertEquals(tm.match("abc", "(a*)"), "a")
end

-- string gmatch ----------------------------------------------------------

TestGmatchString = {}

function TestGmatchString:testWords()
    local out = {}
    for w in tm.gmatch("hello world", "%a+") do
        out[#out + 1] = w
    end
    lu.assertEquals(out, { "hello", "world" })
end

function TestGmatchString:testCaptures()
    local out = {}
    for k, v in tm.gmatch("a=1,b=2", "(%w+)=(%w+)") do
        out[#out + 1] = { k, v }
    end
    lu.assertEquals(out, { { "a", "1" }, { "b", "2" } })
end

function TestGmatchString:testEmptyMatch()
    local n = 0
    for _ in tm.gmatch("abc", "") do
        n = n + 1
    end
    lu.assertEquals(n, 4)
end

function TestGmatchString:testEmptyMatchInvalidUtf8()
    local function count(s)
        local n = 0
        for _ in tm.gmatch(s, "") do
            n = n + 1
        end
        return n
    end
    lu.assertEquals(count("\xC3A"), 3)
    lu.assertEquals(count("\xC3"), 2)
end

function TestGmatchString:testReturnProtocol()
    local f = tm.gmatch("abc", "a")
    lu.assertIsFunction(f)
    lu.assertEquals(select("#", tm.gmatch("abc", "a")), 4)
end

function TestGmatchString:testLeadingCaretLiteral()
    local out = {}
    for w in tm.gmatch("^a", "^a") do
        out[#out + 1] = w
    end
    lu.assertEquals(out, { "^a" })
    local n = 0
    for _ in tm.gmatch("abc", "^.") do
        n = n + 1
    end
    lu.assertEquals(n, 0)
    out = {}
    for w in tm.gmatch("^^", "^^") do
        out[#out + 1] = w
    end
    lu.assertEquals(out, { "^^" })
end

-- buffer find ------------------------------------------------------------

TestFindBuffer = {}

local function buf(s)
    return pt.from(s)
end

function TestFindBuffer:testBasic()
    local b = buf("hello world")
    lu.assertEquals({ tm.find(b, "world") }, { 7, 11 })
    lu.assertIsNil(tm.find(b, "z"))
end

function TestFindBuffer:testInit()
    local b = buf("abc")
    lu.assertEquals({ tm.find(b, "b", 2) }, { 2, 2 })
    lu.assertIsNil(tm.find(b, "b", 3))
    lu.assertIsNil(tm.find(b, "b", -1))
    lu.assertEquals({ tm.find(b, "b", -2) }, { 2, 2 })
end

function TestFindBuffer:testPlain()
    local b = buf("abc")
    lu.assertIsNil(tm.find(b, "a.c", 1, true))
    lu.assertEquals({ tm.find(b, "a.c") }, { 1, 3 })
end

function TestFindBuffer:testCaptures()
    local b = buf("hello world")
    local a, c, d, e = tm.find(b, "(%a+) (%a+)")
    lu.assertEquals(a, 1)
    lu.assertEquals(c, 11)
    lu.assertEquals(d, "hello")
    lu.assertEquals(e, "world")
end

function TestFindBuffer:testPositionCapture()
    local b = buf("abc")
    local a, c = tm.find(b, "()b")
    lu.assertEquals(a, 2)
    lu.assertEquals(c, 2)
end

-- buffer match -----------------------------------------------------------

TestMatchBuffer = {}

function TestMatchBuffer:testWholeMatch()
    local b = buf("hello world")
    lu.assertEquals(tm.match(b, "%a+"), "hello")
    lu.assertIsNil(tm.match(b, "z"))
end

function TestMatchBuffer:testCaptures()
    local b = buf("hello world")
    local a, c = tm.match(b, "(%a+) (%a+)")
    lu.assertEquals(a, "hello")
    lu.assertEquals(c, "world")
end

function TestMatchBuffer:testPositionCapture()
    local b = buf("abc")
    lu.assertEquals(tm.match(b, "()b"), 2)
end

-- buffer gmatch ----------------------------------------------------------

TestGmatchBuffer = {}

function TestGmatchBuffer:testWords()
    local b = buf("hello world")
    local out = {}
    for w in tm.gmatch(b, "%a+") do
        out[#out + 1] = w
    end
    lu.assertEquals(out, { "hello", "world" })
end

function TestGmatchBuffer:testCaptures()
    local b = buf("a=1,b=2")
    local out = {}
    for k, v in tm.gmatch(b, "(%w+)=(%w+)") do
        out[#out + 1] = { k, v }
    end
    lu.assertEquals(out, { { "a", "1" }, { "b", "2" } })
end

function TestGmatchBuffer:testEmptyMatch()
    local b = buf("abc")
    local n = 0
    for _ in tm.gmatch(b, "") do
        n = n + 1
    end
    lu.assertEquals(n, 4)
end

function TestGmatchBuffer:testBufferDeleted()
    local b = buf("hello world")
    local it = tm.gmatch(b, "%a+")
    b:delete()
    local out = {}
    for w in it do
        out[#out + 1] = w
    end
    lu.assertEquals(out, { "hello", "world" })
end

-- fragmented buffer (multi-piece pt_advance path) ------------------------

TestFindBufferFragmented = {}
TestGmatchBufferFragmented = {}

local function frag(s, edits)
    local d = pt.doc(s)
    for _, e in ipairs(edits) do
        d:seek(e[1])
        d:edit(e[2], e[3])
    end
    d:commit()
    return d:buffer()
end

function TestFindBufferFragmented:testFindAcrossPieces()
    local b = frag("abcdef", { { 2, 0, "XX" } }) -- "abXXcdef"
    lu.assertEquals({ tm.find(b, "XXc") }, { 3, 5 })
    lu.assertEquals({ tm.find(b, "cde") }, { 5, 7 })
    lu.assertEquals({ tm.find(b, "^ab") }, { 1, 2 })
    lu.assertEquals({ tm.find(b, "ef$") }, { 7, 8 })
end

function TestFindBufferFragmented:testCapturesAcrossPieces()
    local b = frag("abcdef", { { 2, 0, "XX" } }) -- "abXXcdef"
    local a, c, d, e = tm.find(b, "(ab)XX(cdef)")
    lu.assertEquals(a, 1)
    lu.assertEquals(c, 8)
    lu.assertEquals(d, "ab")
    lu.assertEquals(e, "cdef")
end

function TestFindBufferFragmented:testLiteralAcrossPieces()
    local b = frag("abcdef", { { 2, 0, "XX" } }) -- "abXXcdef"
    lu.assertIsNil(tm.find(b, "a.c", 1, true))
    lu.assertEquals({ tm.find(b, "XXc", 1, true) }, { 3, 5 })
end

function TestGmatchBufferFragmented:testWords()
    local b = frag("abcdef", { { 2, 0, "XX" } }) -- "abXXcdef"
    local out = {}
    for w in tm.gmatch(b, "%l+") do
        out[#out + 1] = w
    end
    lu.assertEquals(out, { "ab", "cdef" })
end

function TestGmatchBufferFragmented:testEmptyMatch()
    local b = frag("abcdef", { { 2, 0, "XX" } }) -- "abXXcdef"
    local n = 0
    for _ in tm.gmatch(b, "") do
        n = n + 1
    end
    lu.assertEquals(n, 9)
end

function TestGmatchBufferFragmented:testBackrefAcrossPieces()
    local b = frag("abab", { { 2, 0, "XX" } }) -- "abXXab"
    lu.assertEquals({ tm.find(b, "(ab)XX%1") }, { 1, 6, "ab" })
end

-- stateful object API ----------------------------------------------------

TestState = {}

function TestState:testNewBindsSource()
    local st = tm.new("hello world")
    local p, e, c = st:find("world", 0)
    lu.assertEquals(p, 6)
    lu.assertEquals(e, 11)
    lu.assertEquals(c, 0)
end

function TestState:testOption()
    local st = tm.new("abc")
    lu.assertFalse(st:option("plain"))
    lu.assertFalse(st:option("lineanchor"))
    lu.assertEquals(st:option("plain", true), st)
    lu.assertTrue(st:option("plain"))
    lu.assertFalse(st:option("lineanchor"))
    lu.assertEquals(st:option("lineanchor", true), st)
    lu.assertTrue(st:option("lineanchor"))
    lu.assertEquals(st:option("plain", false), st)
    lu.assertFalse(st:option("plain"))
    lu.assertTrue(st:option("lineanchor"))
    lu.assertEquals(st:option("lineanchor", nil), true)
    lu.assertTrue(st:option("lineanchor"))
    lu.assertEquals(st:option("plain", nil), false)
    lu.assertFalse(st:option("plain"))
end

function TestState:testPlainFlag()
    local st = tm.new("abc"):option("plain", true)
    lu.assertIsNil(st:find("a.c", 0))
    local st2 = tm.new("abc")
    local p, e, c = st2:find("a.c", 0)
    lu.assertEquals(p, 0)
    lu.assertEquals(e, 3)
    lu.assertEquals(c, 0)
end

function TestState:testLineAnchorFlag()
    local st = tm.new("a\nb\nc"):option("lineanchor", true)
    local p, e, c = st:find("^b", 0)
    lu.assertEquals(p, 2)
    lu.assertEquals(e, 3)
    lu.assertEquals(c, 0)
end

function TestState:testFindRange()
    local st = tm.new("hello world")
    lu.assertIsNil(st:find("world", 0, 6))
    local p, e, c = st:find("world", 0, 11)
    lu.assertEquals(p, 6)
    lu.assertEquals(e, 11)
    lu.assertEquals(c, 0)
    lu.assertIsNil(st:find("world", 7, 11))
end

function TestState:testMatchAtOffset()
    local st = tm.new("abc")
    local p, e, c = st:match("b", 1)
    lu.assertEquals(p, 1)
    lu.assertEquals(e, 2)
    lu.assertEquals(c, 0)
    lu.assertIsNil(st:match("b", 0))
end

function TestState:testCaptures()
    local st = tm.new("hello world")
    local p, e, c = st:find("(%a+) (%a+)", 0)
    lu.assertEquals(p, 0)
    lu.assertEquals(e, 11)
    lu.assertEquals(c, 2)
    lu.assertEquals(st:capture(), 2)
    local s1, e1 = st:capture(0)
    lu.assertEquals(s1, 0)
    lu.assertEquals(e1, 5)
    local s2, e2 = st:capture(1)
    lu.assertEquals(s2, 6)
    lu.assertEquals(e2, 11)
end

function TestState:testPositionCapture()
    local st = tm.new("abc")
    local p, e, c = st:find("()b", 0)
    lu.assertEquals(p, 1)
    lu.assertEquals(e, 2)
    lu.assertEquals(c, 1)
    local s, en = st:capture(0)
    lu.assertEquals(s, 1)
    lu.assertEquals(en, 1)
end

function TestState:testGfind()
    local out = {}
    for p, e, c in tm.new("hello world"):gfind("%a+", 0) do
        out[#out + 1] = { p, e, c }
    end
    lu.assertEquals(out, { { 0, 5, 0 }, { 6, 11, 0 } })
end

function TestState:testGfindRange()
    local out = {}
    for p, e, c in tm.new("hello world"):gfind("l", 0, 5) do
        out[#out + 1] = { p, e, c }
    end
    lu.assertEquals(out, { { 2, 3, 0 }, { 3, 4, 0 } })
end

function TestState:testNilOptionalEndoff()
    local st = tm.new("hello world")
    local p, e, c = st:find("world", 0, nil)
    lu.assertEquals(p, 6)
    lu.assertEquals(e, 11)
    lu.assertEquals(c, 0)
    local out = {}
    for q, r, c2 in tm.new("hello world"):gfind("l", 0, nil) do
        out[#out + 1] = { q, r, c2 }
    end
    lu.assertEquals(out, { { 2, 3, 0 }, { 3, 4, 0 }, { 9, 10, 0 } })
end

function TestState:testEndoffNotPersistent()
    local st = tm.new("hello world")
    lu.assertIsNil(st:find("world", 0, 6))
    local p, e, c = st:find("world", 0)
    lu.assertEquals(p, 6)
    lu.assertEquals(e, 11)
    lu.assertEquals(c, 0)
end

function TestState:testDeleted()
    local st = tm.new("hello world")
    lu.assertEquals(st:delete(), nil)
    lu.assertError(function() st:find("world", 0) end)
    lu.assertError(function() st:match("world", 0) end)
    lu.assertError(function() st:gfind("world", 0) end)
end

function TestState:testInvalidRebindKeepsOldSource()
    local st = tm.new("hello world")
    lu.assertError(function() st:reset({}) end)
    local p, e, c = st:find("world", 0)
    lu.assertEquals(p, 6)
    lu.assertEquals(e, 11)
    lu.assertEquals(c, 0)
end

function TestState:testBufferSource()
    local b = buf("hello world")
    local st = tm.new(b)
    local p, e, c = st:find("world", 0)
    lu.assertEquals(p, 6)
    lu.assertEquals(e, 11)
    lu.assertEquals(c, 0)
end

function TestState:testCharClasses()
    local function has(pat, src)
        return tm.new(src):find(pat, 0) ~= nil
    end
    lu.assertTrue(has("%a", "abc"))
    lu.assertTrue(has("%c", "\1"))
    lu.assertTrue(has("%d", "1"))
    lu.assertTrue(has("%g", "!"))
    lu.assertTrue(has("%l", "a"))
    lu.assertTrue(has("%p", "!"))
    lu.assertTrue(has("%s", " "))
    lu.assertTrue(has("%u", "A"))
    lu.assertTrue(has("%w", "a"))
    lu.assertTrue(has("%x", "f"))
end

function TestState:testRebindSource()
    local b = buf("hello")
    local st = tm.new(b)
    st:reset("hello world")
    local p, e, c = st:find("world", 0)
    lu.assertEquals(p, 6)
    lu.assertEquals(e, 11)
    lu.assertEquals(c, 0)
end

function TestState:testGfindAfterBufferDeleteFails()
    local b = buf("hello world")
    local st = tm.new(b)
    b:delete()
    local p, e, c = st:find("l", 0)
    lu.assertEquals(p, 2)
    lu.assertEquals(e, 3)
    lu.assertEquals(c, 0)
    lu.assertError(function() st:gfind("l", 0) end)
end

function TestState:testResetClearsOptions()
    local st = tm.new("hello world"):option("plain", true):option("lineanchor", true)
    st:find("world", 0)
    st:reset("abc")
    lu.assertFalse(st:option("plain"))
    lu.assertFalse(st:option("lineanchor"))
    local p, e, c = st:find("b", 0)
    lu.assertEquals(p, 1)
    lu.assertEquals(e, 2)
    lu.assertEquals(c, 0)
end

function TestState:testResetRevivesDeleted()
    local st = tm.new("hello world")
    st:delete()
    st:reset("abc")
    local p, e, c = st:find("b", 0)
    lu.assertEquals(p, 1)
    lu.assertEquals(e, 2)
    lu.assertEquals(c, 0)
end

function TestState:testCaptureOutOfRange()
    local st = tm.new("abc")
    st:find("()b", 0)
    lu.assertEquals(st:capture(nil), 1)
    lu.assertIsNil(st:capture(9))
end

function TestState:testInvalidOption()
    ---@diagnostic disable-next-line: param-type-mismatch
    lu.assertError(function() tm.new("abc"):option("bogus") end)
end

function TestState:testErrorsReturnNilMessage()
    local r, e = tm.new("abc"):find("[", 0)
    lu.assertIsNil(r)
    lu.assertEquals(e, "malformed pattern")
    local r2, e2 = tm.new("abc"):match("[", 0)
    lu.assertIsNil(r2)
    lu.assertEquals(e2, "malformed pattern")
    lu.assertError(function()
        local f = tm.new("abc"):gfind("[", 0)
        f()
    end)
end

function TestState:testMissingArguments()
    ---@diagnostic disable-next-line: missing-parameter
    lu.assertError(function() tm.new("abc"):find() end)
    ---@diagnostic disable-next-line: missing-parameter
    lu.assertError(function() tm.new("abc"):match("b") end)
    ---@diagnostic disable-next-line: missing-parameter
    lu.assertError(function() tm.new("abc"):gfind("b") end)
end

-- errors -----------------------------------------------------------------

TestErrors = {}

function TestErrors:testMalformedPattern()
    local r, e = tm.find("abc", "[")
    lu.assertIsNil(r)
    lu.assertEquals(e, "malformed pattern")
    local r2, e2 = tm.match("abc", "[")
    lu.assertIsNil(r2)
    lu.assertEquals(e2, "malformed pattern")
    lu.assertError(function()
        local f = tm.gmatch("abc", "[")
        f()
    end)
end

function TestErrors:testTooComplex()
    local s = string.rep("a", 300)
    local p = string.rep("a?", 300)
    local r, e = tm.find(s, p)
    lu.assertIsNil(r)
    lu.assertEquals(e, "pattern too complex")
end

function TestErrors:testBadBuffer()
    lu.assertError(function() tm.find({}, "a") end)
end

os.exit(lu.LuaUnit.run(), true)
