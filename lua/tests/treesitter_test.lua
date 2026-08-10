-- tree-sitter Lua binding tests. run: just lua-ts (cwd = repo root)
-- Coverage: binding code exercised via c/lua grammars (parse/query/edit/
--   changed_ranges); Windows/5.1-only paths are not Lua-reachable here.
-- NOTE: cpath is cwd-relative (just runs with cwd = lua/). Loading a
--   PUC-built .so under LuaJIT is ABI-unsafe (luaL_Buffer layout), so
--   each VM must load its own build: ./luajit/?.so vs ./?.so.
local dir = arg[0]:match("^(.*)[/\\]") or "."
package.path = dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and "./luajit/?.so;" or "./?.so;") .. package.cpath

local lu = require "luaunit"
local ts = require "treesitter"

-- ======== Language ========
TestLang = {}

function TestLang:testRequireC()
    local lang = ts.require("c")
    lu.assertNotNil(lang)
    lu.assertTrue(lang.symbol_count > 0)
    lu.assertTrue(lang.version > 0)
end

function TestLang:testRequireUnknown()
    lu.assertError(function() ts.require("nosuchlang") end)
end

function TestLang:testSymbolRoundtrip()
    local lang = ts.require("c")
    local id = assert(lang:symbol("int"))
    lu.assertNotNil(lang:symbol(id))
end

-- ======== Parse ========
TestParse = {}

function TestParse:testParseString()
    local p = ts.parser.new()
    p.language = ts.require("c")
    local t = assert(p:parse(nil, "int main(void) { return 0; }\n"))
    lu.assertEquals(t.root.type, "translation_unit")
    lu.assertEquals(t.root.start_byte, 1)
    lu.assertTrue(t.root.end_byte > 0)
end

function TestParse:testIncrementalEdit()
    local p = ts.parser.new()
    p.language = ts.require("lua")
    local t = assert(p:parse(nil, "local x = 1\n"))
    -- insert "2" at byte 10 (after "local x = ")
    t:edit(10, 10, 11, 1, 11, 1, 11, 1, 12)
    local t2 = assert(p:parse(t, "local x = 21\n"))
    local r = t2:changed_ranges(t)
    lu.assertNotNil(r[1])
end

function TestParse:testReadFunctionInput()
    -- TSInput via Lua function: non-contiguous content.
    -- NB: read callback byte is 1-based (binding convention).
    local p = ts.parser.new()
    p.language = ts.require("c")
    local src = "int main(void) { return 0; }\n"
    local t = assert(p:parse(nil, function(byte, row, col)
        lu.assertTrue(byte >= 0)
        lu.assertTrue(row >= 0)
        lu.assertTrue(col >= 0)
        return src:sub(byte)
    end))
    lu.assertNotNil(t)
    lu.assertEquals(t.root.type, "translation_unit")
end

-- ======== Query ========
TestQuery = {}

function TestQuery:testCaptureMatch()
    local lang = ts.require("c")
    local q = lang:query([[
        "return" @keyword
        (identifier) @variable
    ]])
    lu.assertNotNil(q)
    lu.assertTrue(q.capture_count > 0)
    lu.assertEquals(q:capture_name_for_id(1), "keyword")
    local p = ts.parser.new()
    p.language = lang
    local c = q:exec(p:parse(nil, "return x;\n").root)
    local n = 0
    while c:next_match() do
        n = n + 1
    end
    lu.assertTrue(n > 0)
end

function TestQuery:testQueryError()
    local lang = ts.require("c")
    local q, err, off = lang:query("(no_such_node @x)")
    lu.assertNil(q)
    lu.assertNotNil(err)
    lu.assertTrue(off > -1)
end

os.exit(lu.LuaUnit.run(), true)
