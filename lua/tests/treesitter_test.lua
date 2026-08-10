-- tree-sitter Lua binding tests. run: just lua/ts
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

-- ======== shared helpers ========

local CSRC = "int main(void) { int x = 0; return 0; }\n"

local function cparse(src)
    local p = ts.parser.new()
    p.language = ts.require("c")
    return p, assert(p:parse(nil, src or CSRC))
end

local function croot(src)
    return cparse(src)
end

local C = ts.require("c")

-- ======== Node fields ========
TestNodeFields = {}

function TestNodeFields:testChildAccess()
    local _, t = croot()
    local fd = assert(t.root[1])
    lu.assertEquals(fd.type, "function_definition")
    lu.assertEquals(#fd, fd.named_child_count)
    lu.assertTrue(fd.child_count >= fd.named_child_count)
    lu.assertNil(t.root[99])
    lu.assertNil(t.root[0])
    lu.assertEquals(fd:child(1).type, "primitive_type")  -- full child incl. type
    lu.assertEquals(fd:named_child(1).type, "primitive_type")
    lu.assertError(function() fd:child(99) end)
    lu.assertError(function() fd:named_child(99) end)
    lu.assertEquals(fd:child("declarator").type, "function_declarator")
    lu.assertEquals(fd:child("body").type, "compound_statement")
    lu.assertNil(fd:child("no_such_field"))
    local fid = C:field("declarator")
    lu.assertTrue(fid > 0)
    lu.assertEquals(fd:child_by_field_id(fid).type, "function_declarator")
    lu.assertError(function() fd:child_by_field_id(99) end)
    lu.assertEquals(fd:field_name_for_child(1), "type")
    lu.assertEquals(fd:field_name_for_child(2), "declarator")
end

function TestNodeFields:testNavigation()
    local _, t = croot()
    local fd = assert(t.root[1])
    lu.assertEquals(fd.parent, t.root)
    lu.assertNil(t.root.parent)      -- root of tree: null node
    lu.assertNil(fd.next_sibling)
    lu.assertNil(fd.prev_sibling)
    lu.assertNil(fd.next_named_sibling)
    lu.assertNil(fd.prev_named_sibling)
    local body = fd:child("body")
    lu.assertEquals(body.parent, fd)
    lu.assertEquals(body.prev_sibling.type, "function_declarator")
    lu.assertEquals(body.prev_named_sibling.type, "function_declarator")
    lu.assertNil(body.next_sibling)
    lu.assertNil(body.next_named_sibling)
    local d = assert(fd:first_child_for_byte(1))
    lu.assertEquals(d.type, "primitive_type")
    lu.assertTrue(fd:first_child_for_byte(20).type:match("compound") ~= nil)
    lu.assertNil(fd:first_child_for_byte(999))
    local d2 = assert(fd:first_named_child_for_byte(1))
    lu.assertEquals(d2.type, "primitive_type")
    lu.assertNil(fd:first_named_child_for_byte(999))
    lu.assertNotNil(fd:descendant_for_byte_range(5, 10))
    lu.assertNotNil(fd:descendant_for_byte_range(100, 200))
    lu.assertNotNil(fd:named_descendant_for_byte_range(1, 14))
    lu.assertNotNil(fd:named_descendant_for_byte_range(1, 2))
    lu.assertNotNil(fd:descendant_for_point_range(1, 1, 1, 14))
    lu.assertNotNil(fd:named_descendant_for_point_range(1, 5, 1, 9))
end

function TestNodeFields:testPredicates()
    local _, t = croot()
    local fd = assert(t.root[1])
    lu.assertFalse(fd.is_null)
    lu.assertTrue(fd.is_named)
    lu.assertFalse(fd.is_missing)
    lu.assertFalse(fd.is_extra)
    lu.assertFalse(fd.has_error)
    lu.assertFalse(fd.is_error)
    lu.assertFalse(fd.has_changes)
    lu.assertTrue(fd.parse_state > 0)
    lu.assertTrue(fd.next_parse_state > 0)
    lu.assertTrue(fd.descendant_count > 0)
end

function TestNodeFields:testIdentity()
    local _, t = croot()
    local fd = assert(t.root[1])
    lu.assertEquals(fd, fd)
    lu.assertNotEquals(fd, t.root)
    lu.assertTrue(fd:tostring():match("function_definition") ~= nil)
    lu.assertEquals(fd.grammar_type, "function_definition")
    lu.assertEquals(fd.grammar_symbol_name, "function_definition")
    lu.assertTrue(fd.symbol > 0)
    lu.assertEquals(fd.symbol_name, "function_definition")
    lu.assertEquals(fd.grammar_symbol, fd.symbol)
    lu.assertEquals(fd.start_byte, 1)
    lu.assertTrue(fd.end_byte > fd.start_byte)
    local sr, sc = fd:start_point()
    local er, ec = fd:end_point()
    lu.assertTrue(sr >= 0 and sc >= 0 and er >= 0 and ec >= 0)
    lu.assertNotNil(fd.tree)
    lu.assertNotNil(fd.language)
    local _, t2 = croot()
    lu.assertNotEquals(fd, t2.root[1])
end

function TestNodeFields:testMissingAndEdit()
    local p = ts.parser.new()
    p.language = C
    local t = assert(p:parse(nil, "int main( {"))
    lu.assertTrue(t.root.has_error)
    local err = assert(t.root[1])
    lu.assertEquals(err.type, "ERROR")
    lu.assertTrue(err.is_error)
    -- node edit: translate position (same shape as tree edit)
    err:edit(1, 1, 2, 1, 1, 1, 1, 1, 2)
    lu.assertNotNil(err)
end

-- ======== Tree methods ========
TestTree = {}

function TestTree:testCopyAndFields()
    local _, t = croot()
    lu.assertNotNil(t.language)
    local t2 = t:copy()
    lu.assertEquals(t2.root.type, t.root.type)
    t2:delete()
    local r = t:root_with_offset(2, 1, 1)
    lu.assertEquals(r.type, "translation_unit")
    t:delete()
end

function TestTree:testDotGraph()
    local _, t = croot()
    if jit then
        local path = os.tmpname()      -- LuaJIT: path-based
        lu.assertEquals(t:print_dot_graph(path), t)
        os.remove(path)
    else
        local f = assert(io.tmpfile())
        lu.assertEquals(t:print_dot_graph(f), t)
        f:close()
    end
end

function TestTree:testIncludedRanges()
    local p = ts.parser.new()
    p.language = C
    local i = 0
    p.included_ranges = function()
        i = i + 1
        if i == 1 then return 1, 39, 1, 1, 1, 39 end
    end
    local t = assert(p:parse(nil, CSRC))
    lu.assertEquals(t.root.type, "translation_unit")
    lu.assertNotNil(t.included_ranges)  -- tree reports the parse ranges
    local r = p.included_ranges
    lu.assertNotNil(r)
    lu.assertTrue(#r >= 1)
    r:delete()
end

-- ======== Parser ========
TestParser = {}

function TestParser:testReset()
    local p = ts.parser.new()
    p.language = C
    lu.assertEquals(p:reset(), p)
    lu.assertNotNil(p:parse(nil, "int main(void) { return 0; }\n"))
    p:delete()
end

function TestParser:testLogger()
    local p = ts.parser.new()
    p.language = C
    local msgs = {}
    p.logger = function(msg, mtype)
        msgs[#msgs + 1] = msg .. ":" .. mtype
    end
    p:parse(nil, "int main(void) { return 0; }\n")
    lu.assertTrue(#msgs > 0)
    local f = p.logger          -- getter returns callable closure
    lu.assertNotNil(f)
    f("parse", "hello")
    f("lex", "world")
    lu.assertError(function() f("bad", "x") end)
    p.logger = nil
    p:parse(nil, "int main(void) { return 0; }\n")
    p:delete()
end

function TestParser:testLoggerError()
    local p = ts.parser.new()
    p.language = C
    p.logger = function() error("logger boom") end  -- errors are swallowed
    lu.assertNotNil(p:parse(nil, CSRC))
    p.logger = nil
    p:delete()
end

function TestParser:testLanguageGetter()
    local p = ts.parser.new()
    p.language = C
    lu.assertNotNil(p.language)
    p.language = nil            -- clear: null-language userdata, parses fail
    lu.assertNotNil(p.language)
    lu.assertError(function() return p:parse(nil, CSRC).root end)
    lu.assertError(function() p.language:query("(identifier)") end)
    p:delete()
end

function TestParser:testDotGraphs()
    local p = ts.parser.new()
    p.language = C
    if jit then
        local path = os.tmpname()      -- LuaJIT: path-based
        lu.assertEquals(p:print_dot_graphs(path), p)
        os.remove(path)
        lu.assertError(function() p:print_dot_graphs("/no/such/dir/x") end)
    else
        local f = assert(io.tmpfile())
        lu.assertEquals(p:print_dot_graphs(f), p)
        f:close()
    end
    p:delete()
end

function TestParser:testUtf16()
    local p = ts.parser.new()
    p.language = C
    local src = "int main(void) { return 0; }\n"
    local u16 = src:gsub(".", function(c) return string.char(c:byte(), 0) end)
    local t = assert(p:parse(nil, u16, nil, "utf16"))
    lu.assertEquals(t.root.type, "translation_unit")
end

-- ======== TreeCursor ========
TestTreeCursor = {}

function TestTreeCursor:testWalk()
    local _, t = croot()
    local fd = assert(t.root[1])
    local c = fd:cursor()
    lu.assertEquals(c.node, fd)
    lu.assertTrue(c.depth >= 0)
    lu.assertTrue(c.descendant_index >= 0)
    lu.assertNotNil(c.field)         -- field id of current position
    lu.assertNil(c.field_name)       -- root position has no field
    lu.assertEquals(c:goto_first_child(), c)
    lu.assertEquals(c.field_name, "type")
    lu.assertEquals(c:goto_next_sibling(), c)
    lu.assertEquals(c.field_name, "declarator")
    lu.assertEquals(c:goto_prev_sibling(), c)
    lu.assertEquals(c.field_name, "type")
    lu.assertEquals(c:goto_parent(), c)      -- back to the function_definition
    lu.assertEquals(c:goto_last_child(), c)
    lu.assertEquals(c.field_name, "body")
    lu.assertEquals(c:goto_first_child_for_byte(10), c)
    lu.assertEquals(c:goto_first_child_for_point(1, 5), c)
    lu.assertEquals(c:goto_descendant(3), c)
    lu.assertEquals(c.node.type, "function_declarator")
    lu.assertEquals(c:goto_parent(), c)
    lu.assertEquals(c.node, fd)
    local c2 = c:copy()
    lu.assertEquals(c2.node, fd)
    local c3 = ts.tree_cursor.new(fd)
    c3:reset(fd)
    c:delete()
    c2:delete()
    c3:delete()
end

-- ======== Query metadata ========
TestQueryMeta = {}

function TestQueryMeta:testPatterns()
    local q = C:query([[
        (function_definition (function_declarator (identifier) @fn))
        (identifier) @var
    ]])
    lu.assertTrue(q.pattern_count >= 2)
    lu.assertTrue(q.capture_count >= 2)
    lu.assertTrue(q.string_count >= 0)
    lu.assertTrue(q:start_byte_for_pattern(1) >= 0)
    lu.assertTrue(q:is_pattern_rooted(1) == true)
    lu.assertTrue(q:is_pattern_non_local(1) == false)
    lu.assertTrue(q:is_pattern_guaranteed_at_step(1) == false)
    lu.assertEquals(q:capture_quantifier_for_id(1, 1), "one")
    q:delete()
end

function TestQueryMeta:testPredicates()
    local q = C:query([[
        (identifier) @x
        (#eq? @x "main")
    ]])
    -- the (#eq? ...) predicate is itself a separate pattern (pattern 2)
    local preds = q:predicates_for_pattern(2)
    lu.assertTrue(#preds >= 1)
    local steps = {}
    for _, t1, v1 in preds do steps[#steps + 1] = { t1, v1 } end
    lu.assertTrue(#steps >= 1)
    lu.assertNotNil(steps[1][1])
    lu.assertNotNil(steps[1][2])
    lu.assertEquals(q.string_count, 2)   -- "eq?" operator + "main"
    lu.assertEquals(q:string_value_for_id(1), "eq?")
    lu.assertEquals(q:string_value_for_id(2), "main")
    q:delete()
end

function TestQueryMeta:testDisable()
    local q = C:query([[
        (identifier) @v
        (function_definition) @f
    ]])
    local p = ts.parser.new()
    p.language = C
    local function matchn(cq)
        local c = ts.query_cursor.new()
        c:exec(cq, p:parse(nil, CSRC).root)
        local n = 0
        while c:next_match() do n = n + 1 end
        c:delete()
        return n
    end
    local count0 = matchn(q)
    lu.assertTrue(count0 >= 2)
    -- disabled captures: matches still occur, captures are skipped
    local qv = C:query("(identifier) @v")
    qv:disable_capture("v")
    local c = qv:exec(p:parse(nil, CSRC).root)
    lu.assertNotNil(c:next_match())
    lu.assertEquals(#c, 0)
    q:disable_pattern(1)
    local count1 = matchn(q)
    lu.assertTrue(count1 < count0)
    q:delete()
end

-- ======== QueryCursor ========
TestQueryCursor = {}

function TestQueryCursor:testMatch()
    local q = C:query("(identifier) @v")
    local _, t = croot()
    local c = q:exec(t.root)
    lu.assertNotNil(c:next_match())
    lu.assertTrue(#c >= 1)
    lu.assertNotNil(c[1])
    lu.assertNotNil(c[1].type)
    lu.assertNil(c[99])
    lu.assertNil(c[0])
    lu.assertTrue(c.match_id >= 0)
    lu.assertTrue(c.pattern_index >= 1)
    lu.assertFalse(c.did_exceed_match_limit)
    local id = c:next_capture()
    lu.assertTrue(id >= 1)
    local n, idx = c:captures(1)
    lu.assertNotNil(n)
    lu.assertTrue(idx >= 0)
    lu.assertNil(c:next_capture())  -- exhausted
    while c:next_match() do end
    lu.assertNil(c:next_match())
    c:delete()
end

function TestQueryCursor:testRanges()
    local q = C:query("(identifier) @v")
    local _, t = croot()
    local c = q:exec(t.root)
    lu.assertEquals(c:set_byte_range(1, 8), c)
    lu.assertNotNil(c:next_match())
    while c:next_match() do end
    lu.assertEquals(c:set_point_range(1, 1, 1, 14), c)
    c:next_match()
    c:delete()
    -- standalone QueryCursor
    local c2 = ts.query_cursor.new()
    c2:exec(q, t.root)
    lu.assertNotNil(c2:next_match())
    c2:delete()
end

function TestQueryCursor:testLimits()
    local q = C:query("(identifier) @v")
    local _, t = croot()
    local c = q:exec(t.root)
    lu.assertTrue(c.match_limit >= 0)
    c.match_limit = 1
    lu.assertEquals(c.match_limit, 1)
    c.max_start_depth = 100   -- deep enough to not restrict anything
    lu.assertError(function() return c.max_start_depth end)
    lu.assertNotNil(c:next_match())
    c:remove_match(c.match_id)
    while c:next_match() do end
    lu.assertFalse(c.did_exceed_match_limit)
    c:delete()
end

-- ======== Language ========
TestLanguage = {}

function TestLanguage:testField()
    lu.assertTrue(C.field_count > 0)
    lu.assertTrue(C.state_count > 0)
    local fid = C:field("declarator")
    lu.assertTrue(fid > 0)
    lu.assertEquals(C:field(fid), "declarator")
    lu.assertEquals(C:field("no_such_field"), 1)  -- absent field encoded as 1
    lu.assertError(function() C:field(999) end)
    lu.assertError(function() C:field(true) end)
    lu.assertEquals(C:symbol_type(C:symbol("function_definition", true)), "regular")
    lu.assertEquals(C:symbol_type("function_definition", true), "regular")
    lu.assertError(function() C:symbol_type(999999) end)
    lu.assertError(function() C:symbol_type(0) end)
    lu.assertError(function() C:symbol_type(true) end)
    lu.assertTrue(C:next_state(1, 1) >= 0)
    lu.assertError(function() C:next_state(999999, 1) end)
    lu.assertError(function() C:next_state(0, 1) end)
    lu.assertError(function() C:next_state(1, true) end)
    lu.assertError(function() C:symbol(0) end)
    lu.assertError(function() C:symbol(999999) end)
    lu.assertError(function() C:symbol(true) end)
    lu.assertEquals(C:symbol(C:symbol("function_definition", true)),
            "function_definition")
end

-- ======== LookaheadIterator ========
TestLookahead = {}

function TestLookahead:testWalk()
    local it = C:lookahead_iterator(1)
    lu.assertNotNil(it)
    lu.assertNotNil(it.language)
    lu.assertTrue(it.symbol > 0)
    lu.assertNotNil(it.symbol_name)
    local seen = 0
    while it:next() do
        seen = seen + 1
        lu.assertTrue(it.symbol > 0)
    end
    lu.assertTrue(seen > 0)
    lu.assertTrue(it:reset(1))
    lu.assertTrue(it:reset(C, 1))
    lu.assertError(function() it:reset(1, 2, 3, 4) end)
    it:delete()
end

-- ======== Slice ========
TestSlice = {}

function TestSlice:testChangedRanges()
    local p = ts.parser.new()
    p.language = ts.require("lua")
    local t = assert(p:parse(nil, "local x = 1\n"))
    t:edit(10, 10, 11, 1, 11, 1, 11, 1, 12)
    local t2 = assert(p:parse(t, "local x = 21\n"))
    local r = assert(t2:changed_ranges(t))
    lu.assertTrue(#r >= 1)
    lu.assertTrue(r[1] > 0)          -- single-value index: start_byte
    local fields
    for _, a, b, c1, d, e, f in r do  -- full 6 fields via iteration
        fields = { a, b, c1, d, e, f }
    end
    lu.assertNotNil(fields)
    lu.assertTrue(fields[2] >= fields[1])
    lu.assertNil(r[99])
    lu.assertNil(r.no_such_field)
    lu.assertNotNil(tostring(r))
    local n = 0
    for _ in r do n = n + 1 end
    lu.assertEquals(n, #r)
    r:delete()
    r:delete()  -- idempotent
    t:delete()
    t2:delete()
end

-- ======== error paths ========
TestErrors = {}

function TestErrors:testParseErrors()
    local p = ts.parser.new()
    p.language = C
    lu.assertError(function() p:parse(nil, 42) end)
    -- failing read callback: error is swallowed, parse yields an empty tree
    local r1 = p:parse(nil, function() error("boom") end)
    lu.assertNotNil(r1)
    lu.assertNotNil(r1.root)
    local r2 = p:parse(nil, function() return 42 end)
    lu.assertNotNil(r2)
    lu.assertNotNil(r2.root)
    p:delete()
    lu.assertError(function() p:parse(nil, "int x;") end)  -- null parser
    -- no language bound: parse returns a null tree (non-nil userdata)
    local p2 = ts.parser.new()
    local r3 = p2:parse(nil, "int x;")
    lu.assertNotNil(r3)
    lu.assertError(function() return r3.root end)
    p2:delete()
end

function TestErrors:testNullObjects()
    local _, t = croot()
    t:delete()
    lu.assertError(function() return t.root end)
    local q = C:query("(identifier) @v")
    q:delete()
    local c = ts.query_cursor.new()
    local p1, t1 = croot()
    lu.assertError(function() c:exec(q, t1.root) end)  -- null query
    c:delete()
    lu.assertError(function() c:next_match() end)     -- null cursor
    local p2, t2 = croot()
    local tc = t2.root:cursor()
    tc:delete()
    lu.assertError(function() return tc.node end)     -- null tree cursor
    local it = C:lookahead_iterator(1)
    it:delete()
    it:delete()                   -- idempotent: *pi == NULL path
    lu.assertError(function() it:next() end)          -- null lookahead
end

function TestErrors:testIndexErrors()
    local _, t = croot()
    local fd = assert(t.root[1])
    lu.assertError(function() fd:child(0) end)
    lu.assertError(function() fd:child(999) end)
    lu.assertError(function() fd:child(true) end)
    lu.assertError(function() fd:named_child(999) end)
    lu.assertError(function() fd:child_by_field_id(-1) end)
    lu.assertError(function() fd:descendant_for_byte_range(0, 5) end)
    lu.assertError(function() fd:first_child_for_byte(0) end)
    lu.assertError(function() fd:field_name_for_child(999) end)
end

function TestErrors:testRequireVariants()
    lu.assertError(function() ts.require("c.so") end)      -- path without symbol
    lu.assertNotNil(ts.require("c", "tree_sitter_c"))       -- explicit symbol
    -- dlopen succeeds, symbol lookup fails
    lu.assertError(function() ts.require("grammar/c.so", "no_such_symbol") end)
end

function TestErrors:testTreeReuse()
    -- deleting a tree frees its nodes; a fresh tree at the same address
    -- must not see stale cached nodes from the old one
    local _, t = croot()
    t:delete()
    local p2, t2 = croot()
    lu.assertNotNil(t2.root)
    lu.assertEquals(t2.root.type, "translation_unit")
    t2:delete()
end

collectgarbage("collect")
collectgarbage("collect")
collectgarbage("collect")
os.exit(lu.LuaUnit.run(), true)
