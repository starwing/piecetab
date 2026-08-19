-- spantree Lua binding tests (v4 API). run: just lua/sp
-- Coverage: spantree.c 100% lines, >=95% branches.
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/lua/luajit/?.so;"
    or root .. "/lua/?.so;")
    .. package.cpath
    .. ";./lua/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"

local lu = require "luaunit"
local sp = require "spantree"

---@diagnostic disable: need-check-nil

---@type any
local ANY -- deliberately mistyped arguments (runtime error tests)

-- current test compositor: helpers (clear_tree/edit_tree/...) create
-- their own compositor internally and expose it here for the calling
-- test's assert_attr / namespace / intern calls
local cur_comp

local function newcomp()
    cur_comp = sp.compositor()
    return cur_comp
end

local function newtree(comp)
    return sp.new(comp)
end

-- shared stream collector: (id, len) pairs over t:[method](...)
-- (span = mark stream 4-tuples; styled = merged/synthetic ids)
local function collect(t, method, ...)
    local out = {}
    for _, len, _, id in t[method](t, ...) do
        out[#out + 1] = { id, len }
    end
    return out
end

-- key-level attr assertion: tree segments carry synthetic (merged)
-- ids, so compare the folded attr table via comp:attr instead
local function assert_attr(comp, id, attr)
    local a = comp:attr(id)
    lu.assertNotNil(a, "style id " .. tostring(id) .. " has no attr")
    for k, v in pairs(attr) do
        lu.assertEquals(a[k], v, "style key " .. tostring(k))
    end
end

-- release possibly-registered names; pcall so tearDown survives
-- half-registered states after a mid-test failure
local function unregister_names(comp, names)
    for _, n in ipairs(names) do
        pcall(function() comp:namespace(n, nil) end)
    end
end

-- ======== style: pure style service (cp face via Tree) ========
TestStyle = {}

function TestStyle:testNew()
    local comp = newcomp()
    lu.assertEquals(comp:intern({}), 0) -- empty attr pre-interned as 0
end

function TestStyle:testInternReuse()
    local comp = newcomp()
    local a = { fg = 207 }
    lu.assertEquals(comp:intern(a), comp:intern(a))
    lu.assertEquals(comp:intern({ fg = 207 }), comp:intern(a))
end

function TestStyle:testInternOrder()
    local comp = newcomp()
    lu.assertEquals(comp:intern({ fg = 1, bold = true }),
        comp:intern({ bold = true, fg = 1 }))
end

function TestStyle:testInternSkipFalse()
    local comp = newcomp()
    -- nil values are skipped; false serializes as a value (distinct)
    lu.assertEquals(comp:intern({ bold = true, dim = nil }),
        comp:intern({ bold = true }))
    lu.assertNotEquals(comp:intern({ bold = true, underline = false }),
        comp:intern({ bold = true }))
    lu.assertEquals(comp:intern({ bold = true, underline = false }),
        comp:intern({ bold = true, underline = false }))
end

function TestStyle:testInternValues()
    local comp = newcomp()
    lu.assertEquals(comp:intern({ fg = 1 }), comp:intern({ fg = 1 }))
    lu.assertNotEquals(comp:intern({ fg = 1 }), comp:intern({ fg = 2 }))
    lu.assertNotEquals(comp:intern({ fg = 1 }), comp:intern({ bg = 1 }))
    -- unlisted fields (string/numeric keys) are ignored: id 0
    lu.assertEquals(comp:intern({ x = "y" }), 0)
    lu.assertEquals(comp:intern({ [1] = "x" }), 0)
end

function TestStyle:testInternColor()
    local comp = newcomp()
    local c = { fg = "#010203" }
    lu.assertEquals(comp:intern(c), comp:intern(c))
    lu.assertEquals(comp:intern({ bg = 5, fg = "#010203" }),
        comp:intern({ fg = "#010203", bg = 5 }))
    lu.assertNotEquals(comp:intern({ fg = 1 }), comp:intern(c))
    -- table values are rejected (attr values are string/number/bool)
    lu.assertErrorMsgContains("unexpected type",
        function() comp:intern({ fg = {} }) end)
end

function TestStyle:testInternFloat()
    local comp = newcomp()
    -- float numbers take the %.14g path (PUC; LuaJIT sees integers)
    lu.assertEquals(comp:intern({ fg = 1.5 }), comp:intern({ fg = 1.5 }))
    lu.assertNotEquals(comp:intern({ fg = 1.5 }), comp:intern({ fg = 1 }))
end

function TestStyle:testInternVtext()
    local comp = newcomp()
    -- "vtext"/"vstyle" are default canon fields (vtext payload): the
    -- label string and the style attr id, order-independent
    lu.assertEquals(comp:intern({ vtext = "int:", vstyle = 3 }),
        comp:intern({ vstyle = 3, vtext = "int:" }))
    -- distinct payloads intern to distinct ids (render needs per-text)
    lu.assertNotEquals(comp:intern({ vtext = "int:" }),
        comp:intern({ vtext = "int" }))
    -- same payload + style: reuse
    lu.assertEquals(comp:intern({ vtext = "int:", vstyle = 3 }),
        comp:intern({ vtext = "int:", vstyle = 3 }))
    -- style attr id participates in the canon key
    lu.assertNotEquals(comp:intern({ vtext = "int:", vstyle = 3 }),
        comp:intern({ vtext = "int:", vstyle = 4 }))
end

function TestStyle:testAttr()
    local comp = newcomp()
    lu.assertEquals(comp:attr(0), {})
    local id = comp:intern({ fg = 7, bold = true })
    local a = comp:attr(id)
    lu.assertEquals(a.fg, 7)
    lu.assertEquals(a.bold, true)
    -- unknown ids return nil (editor contract: csi on unknown = nil)
    lu.assertNil(comp:attr(999))
    lu.assertNil(comp:attr(-1))
end

function TestStyle:testHash()
    local comp = newcomp()
    local mk = function(h)
        return setmetatable({ fg = 1 }, { __hash = function() return h end })
    end
    -- same hash -> same id
    lu.assertEquals(comp:intern(mk("a")), comp:intern(mk("a")))
    -- different hash -> different id
    lu.assertNotEquals(comp:intern(mk("a")), comp:intern(mk("b")))
    -- attr(id) returns the first-seen table
    local t1 = mk("c")
    lu.assertEquals(comp:intern(mk("c")), comp:intern(t1))
    lu.assertEquals(comp:attr(comp:intern(t1)), t1)
    -- non-string __hash return raises
    lu.assertErrorMsgContains("__hash must return a string",
        function()
            comp:intern(setmetatable({}, { __hash = function() return 42 end }))
        end)
    -- hashed and canon tables with the same fields do not collide
    lu.assertNotEquals(comp:intern({ fg = 1 }), comp:intern(mk("x")))
end

function TestStyle:testInternMany()
    local comp = newcomp()
    local seen = {}
    for i = 1, 12 do -- many keys: by_attr table growth + reuse
        seen[i] = comp:intern({ fg = i })
    end
    lu.assertEquals(seen[1], comp:intern({ fg = 1 }))
    lu.assertEquals(seen[12], comp:intern({ fg = 12 }))
end

function TestStyle:testWhitelistIgnore()
    local comp = newcomp()
    -- unlisted fields do not participate in canon: same id
    lu.assertEquals(comp:intern({ fg = 1, foo = 2 }), comp:intern({ fg = 1 }))
    lu.assertEquals(comp:intern({ fg = 1, foo = { r = 1, g = 2, b = 3 } }),
        comp:intern({ fg = 1 }))
end

function TestStyle:testFieldsCustom()
    local comp = newcomp()
    comp:fields({ "foo", "fg" })
    -- custom string-valued field canon
    lu.assertEquals(comp:intern({ foo = "bar" }), comp:intern({ foo = "bar" }))
    lu.assertNotEquals(comp:intern({ foo = "bar" }), comp:intern({ foo = "baz" }))
    -- custom color string field canon
    lu.assertEquals(comp:intern({ foo = "#010203" }),
        comp:intern({ foo = "#010203" }))
    lu.assertNotEquals(comp:intern({ foo = "#010203" }),
        comp:intern({ foo = "#040506" }))
    -- unlisted fields are ignored (the default SGR set is gone)
    lu.assertEquals(comp:intern({ fg = 1, bg = 2 }), comp:intern({ fg = 1 }))
    lu.assertEquals(comp:intern({ bold = true }), 0)
end

function TestStyle:testFieldsOrder()
    -- the caller's order is preserved (canon follows it): no implicit
    -- sorting, the caller sorts if they care
    local comp = newcomp()
    comp:fields({ "zebra", "alpha" })
    lu.assertEquals(comp:fields(), { "zebra", "alpha" })
    -- canon is stable as long as the order is: any attr interned under
    -- this whitelist serializes in that order, so equal tables collide
    lu.assertEquals(comp:intern({ alpha = 1, zebra = 2 }),
        comp:intern({ zebra = 2, alpha = 1 }))
end

function TestStyle:testFieldsRead()
    local comp = newcomp()
    -- default SGR set + vtext payload
    lu.assertEquals(comp:fields(),
        { "bg", "bold", "dim", "fg", "italic", "reverse", "underline",
          "vstyle", "vtext" })
    comp:fields({ "fg", "bg" })
    lu.assertEquals(comp:fields(), { "fg", "bg" })
    comp:fields({})
    lu.assertEquals(comp:fields(), {})
end

function TestStyle:testFieldsAdd()
    local comp = newcomp()
    comp:fields({ "fg", "bg" })
    -- append with dedup: bg already present, italic appended (caller
    -- order preserved)
    comp:fields("add", { "bg", "italic" })
    lu.assertEquals(comp:fields(), { "fg", "bg", "italic" })
    -- the added field takes part in canon
    lu.assertEquals(comp:intern({ fg = 1, italic = true }),
        comp:intern({ italic = true, fg = 1 }))
    -- empty append is a no-op
    comp:fields("add", {})
    lu.assertEquals(comp:fields(), { "fg", "bg", "italic" })
    -- canon order stays stable across appends
    lu.assertEquals(comp:intern({ fg = 1, italic = true, bg = 2 }),
        comp:intern({ bg = 2, italic = true, fg = 1 }))
end

function TestStyle:testFieldsEmpty()
    local comp = newcomp()
    comp:fields({})
    lu.assertEquals(comp:intern({ fg = 1 }), 0) -- no fields: id 0
    lu.assertEquals(comp:intern({ foo = "x" }), 0)
end

function TestStyle:testFieldsInvalid()
    local comp = newcomp()
    ---@type any
    local not_a_list = 123
    lu.assertErrorMsgContains("table expected",
        function() comp:fields("add", not_a_list) end)
    ---@diagnostic disable: param-type-mismatch
    lu.assertErrorMsgContains("unknown fields mode",
        function() comp:fields("bogus", { "fg" }) end)
    ---@diagnostic enable: param-type-mismatch
    ---@type any
    local bad_fields = { "a", 1 }
    lu.assertErrorMsgContains("string expected",
        function() comp:fields(bad_fields) end)
end

function TestStyle:testReload()
    -- re-opening the module exercises the idempotent metatable guards
    package.loaded["spantree"] = nil
    local sp2 = require "spantree"
    local comp2 = sp2.compositor()
    local t = sp2.new(comp2)
    lu.assertEquals(comp2:intern({}), 0)
    package.loaded["spantree"] = sp -- restore for the remaining tests
end

function TestStyle:testSharedComp()
    -- trees bound to one compositor share intern ids; a fresh
    -- compositor owns a separate whitelist
    local comp = newcomp()
    comp:fields({ "bg", "bold", "dim", "fg", "italic", "reverse", "underline" })
    local t1, t2 = newtree(comp), newtree(comp)
    lu.assertEquals(comp:intern({ fg = 1 }), comp:intern({ fg = 1 }))
    lu.assertEquals(comp:attr(comp:intern({ fg = 2 })).fg, 2)
    local comp2 = newcomp()
    comp2:fields({ "fg" })
    lu.assertNotNil(comp2:attr(comp2:intern({ fg = 1 })).fg)
    lu.assertNil(comp2:attr(comp2:intern({ bg = 1 })).bg) -- bg ignored in comp2
    lu.assertEquals(comp:attr(comp:intern({ bg = 1 })).bg, 1) -- still set in comp
end

-- ======== namespace registry (global across trees) ========
TestNs = {}

function TestNs:testQueryStates()
    local comp = newcomp(); local t = newtree(comp)
    -- query on an unregistered name: nil, no error
    lu.assertNil(comp:namespace("q"))
    -- fresh registration returns nil
    lu.assertNil(comp:namespace("q", 1))
    lu.assertEquals(comp:namespace("q"), 1)
    -- re-register changes the priority, returns the old one
    lu.assertEquals(comp:namespace("q", 7), 1)
    lu.assertEquals(comp:namespace("q"), 7)
    -- ordinary mode: second return is nil
    lu.assertNil(select(2, comp:namespace("q")))
    -- release the name (the registry is shared across tests)
    comp:namespace("q", nil)
end

function TestNs:testStrictNew()
    local comp = newcomp(); local t = newtree(comp)
    lu.assertNil(comp:namespace("sn1", 1, "c"))
    -- strict mode on an existing name raises; priority unchanged
    lu.assertErrorMsgContains("namespace already registered",
        function() comp:namespace("sn1", 2, "c") end)
    lu.assertEquals(comp:namespace("sn1"), 1)
    -- non-strict re-register still works (no flags arg)
    lu.assertEquals(comp:namespace("sn1", 3), 1)
    -- strict + ephemeral combination parses
    lu.assertNil(comp:namespace("sn2", 2, "ec"))
    lu.assertEquals(select(2, comp:namespace("sn2")), "ephemeral")
    lu.assertErrorMsgContains("namespace already registered",
        function() comp:namespace("sn2", 3, "ce") end)
    comp:namespace("sn1", nil)
    comp:namespace("sn2", nil)
end

function TestNs:testFlagsErrors()
    local comp = newcomp(); local t = newtree(comp)
    lu.assertErrorMsgContains("invalid namespace flags",
        function() comp:namespace("fe", 1, "x") end)
    lu.assertErrorMsgContains("invalid namespace flags",
        function() comp:namespace("fe", 1, "cx") end)
    lu.assertErrorMsgContains("invalid namespace flags",
        ---@diagnostic disable-next-line: param-type-mismatch
        function() comp:namespace("fe", 1, 7) end)
    lu.assertNil(comp:namespace("fe"))
end

function TestNs:testEphUnlimited()
    local comp = newcomp(); local t = newtree(comp)
    local i
    for i = 1, 70 do
        comp:namespace("e" .. i, i, "e")
    end
    -- eph ns never exhaust the ordinary slot pool
    for i = 1, 64 do
        comp:namespace("n" .. i, 1)
    end
    lu.assertErrorMsgContains("namespace limit reached",
        function() comp:namespace("n65", 1) end)
    -- eph registrations still succeed beyond the ordinary cap
    comp:namespace("e71", 1, "e")
    lu.assertEquals(select(2, comp:namespace("e71")), "ephemeral")
    -- query + unregister on eph ns
    lu.assertEquals(comp:namespace("e1"), 1)
    lu.assertEquals(comp:namespace("e1", nil), 1)
    lu.assertNil(comp:namespace("e1"))
    lu.assertErrorMsgContains("unknown namespace",
        function() comp:namespace("e1", nil) end)
    -- release the ordinary slots and eph names (registry is global)
    for i = 1, 64 do
        comp:namespace("n" .. i, nil)
    end
    for i = 2, 71 do
        comp:namespace("e" .. i, nil)
    end
end

function TestNs:testEphModeSwitch()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("em", 1)
    t:splice(0, 0, 10)
    t:mark("em", { fg = 1 }, 0, 10)
    -- ordinary -> ephemeral: old tree data drops, returns old priority
    lu.assertEquals(comp:namespace("em", 2, "e"), 1)
    lu.assertEquals(select(2, comp:namespace("em")), "ephemeral")
    lu.assertEquals(collect(t, "span", 0, 10), { { 0, 10 } })
    -- ephemeral -> ordinary: eph data drops, returns old priority
    t:mark("em", { fg = 3 }, 2, 4)
    lu.assertEquals(comp:namespace("em", 5), 2)
    lu.assertNil(select(2, comp:namespace("em")))
    lu.assertEquals(collect(t, "span", 0, 10), { { 0, 10 } })
    comp:namespace("em", nil)
end

function TestNs:testUnregister()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("ur", 5)
    -- unregister returns the old priority
    lu.assertEquals(comp:namespace("ur", nil), 5)
    lu.assertNil(comp:namespace("ur"))
    -- unknown name raises
    lu.assertErrorMsgContains("unknown namespace",
        function() comp:namespace("zzz", nil) end)
end

function TestNs:testEmptyName()
    local comp = newcomp(); local t = newtree(comp)
    lu.assertErrorMsgContains("invalid namespace name",
        function() comp:namespace("") end)
    lu.assertErrorMsgContains("invalid namespace name",
        function() comp:namespace("", 1) end)
end

function TestNs:testLimit()
    local comp = newcomp(); local t = newtree(comp)
    local i = 0
    local ok, err
    repeat -- register until the slot pool is exhausted
        i = i + 1
        ok, err = pcall(function() comp:namespace("ns" .. i, i % 7) end)
    until not ok
    lu.assertEquals(i - 1, 64) -- SP_MASK_BITS on 64-bit
    lu.assertStrContains(err, "namespace limit reached")
    -- pool full: any further registration raises
    lu.assertErrorMsgContains("namespace limit reached",
        function() comp:namespace("another", 1) end)
    -- unregister frees a slot, a new name reuses it
    lu.assertEquals(comp:namespace("ns1", nil), 1 % 7)
    comp:namespace("freed", 1)
    lu.assertEquals(comp:namespace("freed"), 1)
    -- release every slot so later tests can register (registry is global)
    for i = 2, 64 do
        lu.assertEquals(comp:namespace("ns" .. i, nil), i % 7)
    end
    lu.assertEquals(comp:namespace("freed", nil), 1)
end

function TestNs:testSlotReuse()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("sr1", 5)
    t:splice(0, 0, 10)
    t:mark("sr1", { fg = 1 }, 0, 10)
    -- unregister prunes the whole tree
    lu.assertEquals(comp:namespace("sr1", nil), 5)
    lu.assertEquals(collect(t, "span", 0, 10), { { 0, 10 } })
    -- a new name takes the freed slot; priority folding stays correct
    comp:namespace("sr2", 5)
    t:mark("sr2", { fg = 2 }, 0, 10)
    assert_attr(cur_comp, collect(t, "styled", 0, 10)[1][1], { fg = 2 })
    lu.assertEquals(comp:namespace("sr2"), 5)
    comp:namespace("sr2", nil)
end

function TestNs:testPriorityFold()
    -- higher priority wins shared keys; others pass through
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("pf1", 1)
    comp:namespace("pf2", 2)
    t:splice(0, 0, 10)
    t:mark("pf1", { fg = 1, bg = 1 }, 0, 10)
    t:mark("pf2", { fg = 2 }, 0, 10)
    local s = collect(t, "styled", 0, 10)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][2], 10)
    assert_attr(cur_comp, s[1][1], { fg = 2, bg = 1 })
    comp:namespace("pf1", nil)
    comp:namespace("pf2", nil)
end

function TestNs:testTieBreak()
    -- same priority: registration order, later wins
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("tb1", 5)
    comp:namespace("tb2", 5)
    t:splice(0, 0, 10)
    t:mark("tb1", { fg = 1 }, 0, 10)
    t:mark("tb2", { fg = 2 }, 0, 10)
    assert_attr(cur_comp, collect(t, "styled", 0, 10)[1][1], { fg = 2 })
    comp:namespace("tb1", nil)
    comp:namespace("tb2", nil)
end

function TestNs:testSortSwap()
    -- fills in reverse priority order exercise the sort swap path
    -- (three layers force consecutive swaps inside the sort loop)
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("ss1", 1)
    comp:namespace("ss2", 2)
    comp:namespace("ss3", 3)
    t:splice(0, 0, 10)
    t:mark("ss3", { fg = 3 }, 0, 10)
    t:mark("ss2", { fg = 2 }, 0, 10)
    t:mark("ss1", { fg = 1 }, 0, 10)
    -- ascending priority order wins the fold: ss3
    assert_attr(cur_comp, collect(t, "styled", 0, 10)[1][1], { fg = 3 })
    comp:namespace("ss1", nil)
    comp:namespace("ss2", nil)
    comp:namespace("ss3", nil)
end

function TestNs:testReprioRefold()
    -- priority change refolds the whole tree: the folding attr flips
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("rr1", 2)
    comp:namespace("rr2", 1)
    t:splice(0, 0, 10)
    t:mark("rr1", { fg = 1 }, 0, 10)
    t:mark("rr2", { fg = 2 }, 0, 10)
    assert_attr(cur_comp, collect(t, "styled", 0, 10)[1][1], { fg = 1 }) -- rr1(2) > rr2(1)
    lu.assertEquals(comp:namespace("rr2", 3), 1) -- returns the old priority
    assert_attr(cur_comp, collect(t, "styled", 0, 10)[1][1], { fg = 2 }) -- rr2(3) > rr1(2)
    comp:namespace("rr1", nil)
    comp:namespace("rr2", nil)
end

function TestNs:testNsIsolation()
    -- trees bound to one compositor share the ns registry; a fresh
    -- compositor is fully isolated
    local comp = newcomp()
    local t1, t2 = newtree(comp), newtree(comp)
    comp:namespace("gr", 1)
    lu.assertEquals(comp:namespace("gr"), 1)
    comp:namespace("gr", 9)
    lu.assertEquals(comp:namespace("gr"), 9)
    comp:namespace("gr", nil)
    lu.assertNil(comp:namespace("gr"))
    local comp2 = newcomp()
    local t3 = newtree(comp2)
    lu.assertNil(comp2:namespace("gr"))
    lu.assertErrorMsgContains("unknown namespace",
        function() t3:mark("gr", { fg = 1 }, 0, 5) end)
end

function TestNs:testEphIsolation()
    -- eph registration is compositor-local; the eph layer data stays
    -- tree-local
    local comp = newcomp()
    local t1, t2 = newtree(comp), newtree(comp)
    comp:namespace("glo", 1, "e")
    t1:mark("glo", { fg = 1 }, 2, 4)
    -- t2 (same compositor) sees the registration but no spans
    lu.assertEquals(collect(t2, "span", "glo", 0, 10), {})
    local s = collect(t1, "span", "glo", 0, 10)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][2], 4)
    lu.assertEquals(s[1][1], comp:intern({ fg = 1 }))
    comp:namespace("glo", nil)
    -- a fresh compositor has no such eph ns
    local comp2 = newcomp()
    lu.assertNil(comp2:namespace("glo"))
end

function TestNs:tearDown()
    -- the registry is global across tests: release every name this
    -- class can leave behind (each test already unregisters its own)
    local names = { "q", "sn1", "sn2", "fe", "em", "ur", "gr", "glo",
        "pf1", "pf2", "tb1", "tb2", "ss1", "ss2", "ss3", "rr1", "rr2",
        "sr1", "sr2", "freed", "another" }
    local k = #names
    for i = 1, 71 do names[k + i] = "e" .. i end
    k = #names
    for i = 1, 64 do names[k + i] = "n" .. i end
    k = #names
    for i = 1, 64 do names[k + i] = "ns" .. i end
    unregister_names(newcomp(), names)
end

-- ======== tree: span storage (offset-arg API) ========
TestTree = {}

function TestTree:testMarkLayers()
    -- distinct ns layers coexist on one range
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    comp:namespace("b", 2)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 0, 10)
    t:mark("b", { bg = 2 }, 0, 10)
    -- span = mark flow: the merged seg decomposes into two marks
    local s = collect(t, "span", 0, 10)
    lu.assertEquals(#s, 2)
    lu.assertEquals(s[1][2], 10)
    lu.assertEquals(s[2][2], 10)
    assert_attr(cur_comp, s[1][1], { fg = 1 })
    assert_attr(cur_comp, s[2][1], { bg = 2 })
end

function TestTree:testMarkSameNs()
    -- same ns refills overwrite; identical attrs merge into one segment
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 0, 6)
    t:mark("a", { fg = 2 }, 0, 6)
    local s = collect(t, "span", 0, 6)
    lu.assertEquals(#s, 1) -- segments starting past the window are skipped
    assert_attr(cur_comp, s[1][1], { fg = 2 })
    lu.assertEquals(s[1][2], 6)
    -- adjacent mark with the same attr merges
    t:mark("a", { fg = 2 }, 6, 4)
    lu.assertEquals(collect(t, "span", 0, 10), { { s[1][1], 10 } })
end

function TestTree:testMarkNil()
    -- nil ns = unaffiliated layer (ns 0, lowest priority)
    local comp = newcomp(); local t = newtree(comp)
    t:splice(0, 0, 10)
    t:mark(nil, { fg = 1 }, 0, 10)
    assert_attr(cur_comp, collect(t, "span", 0, 10)[1][1], { fg = 1 })
    -- idempotent
    t:mark(nil, { fg = 1 }, 0, 10)
    lu.assertEquals(#collect(t, "span", 0, 10), 1)
    -- a registered layer wins shared keys over the nil layer
    comp:namespace("a", 1)
    t:mark("a", { fg = 2, bg = 1 }, 0, 10)
    local s = collect(t, "span", 0, 10)
    lu.assertEquals(#s, 2) -- nil + a marks overlap
    assert_attr(cur_comp, s[1][1], { fg = 1 })
    assert_attr(cur_comp, s[2][1], { fg = 2, bg = 1 })
    assert_attr(cur_comp, collect(t, "styled", 0, 10)[1][1], { fg = 2, bg = 1 })
end

function TestTree:testMarkVirtual()
    -- off beyond bytes pads [bytes, off) as id 0
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:mark("a", { fg = 1 }, 100, 5)
    lu.assertEquals(t:bytes(), 105)
    local s = collect(t, "span", 0, 105)
    lu.assertEquals(s[1], { 0, 100 })
    lu.assertEquals(s[2][2], 5)
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    -- empty tree mark at 0
    local t2 = newtree(comp)
    comp:namespace("a", 1)
    t2:mark("a", { fg = 1 }, 0, 5)
    lu.assertEquals(t2:bytes(), 5)
    lu.assertEquals(collect(t2, "span", 0, 5)[1][2], 5)
end

function TestTree:testMarkEndAppend()
    -- mark exactly at the tree end appends, no pad
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 10, 5)
    lu.assertEquals(t:bytes(), 15)
    local s = collect(t, "span", 0, 15)
    lu.assertEquals(s[1], { 0, 10 })
    lu.assertEquals(s[2][2], 5)
end

-- base layout for the clear tests: [(0,2),(A,2),(AB,2)]
local function clear_tree(comp)
    comp = comp or newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    comp:namespace("b", 2)
    t:splice(0, 0, 6)
    t:mark("a", { fg = 1 }, 2, 4) -- [2,6)
    t:mark("b", { bg = 2 }, 4, 2) -- [4,6)
    return t
end

function TestTree:testClearNs()
    -- clear(ns): whole-tree pruned clear of one layer, others survive
    local t = clear_tree()
    t:clear("a")
    local s = collect(t, "span", 0, 6)
    lu.assertEquals(#s, 2)
    lu.assertEquals(s[1], { 0, 4 })
    assert_attr(cur_comp, s[2][1], { bg = 2 })
    lu.assertEquals(s[2][2], 2)
    lu.assertEquals(t:bytes(), 6) -- length untouched
end

function TestTree:testClearRange()
    -- clear(ns, off, len): range clear of one layer
    local t = clear_tree()
    t:clear("b", 2, 3) -- b survives only on [5,6)
    local s = collect(t, "span", 0, 6)
    lu.assertEquals(#s, 4) -- the ab seg decomposes into a + b marks
    lu.assertEquals(s[1], { 0, 2 })
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    lu.assertEquals(s[2][2], 3)
    assert_attr(cur_comp, s[3][1], { fg = 1 })
    lu.assertEquals(s[3][2], 1)
    assert_attr(cur_comp, s[4][1], { bg = 2 })
    lu.assertEquals(s[4][2], 1)
end

function TestTree:testClearAllRange()
    -- clear(nil, off, len): range full clear of every layer
    local t = clear_tree()
    t:clear(nil, 2, 3)
    local s = collect(t, "span", 0, 6)
    lu.assertEquals(#s, 3) -- a + b marks overlap on [5,6)
    lu.assertEquals(s[1], { 0, 5 })
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    lu.assertEquals(s[2][2], 1)
    assert_attr(cur_comp, s[3][1], { bg = 2 })
    lu.assertEquals(s[3][2], 1)
end

function TestTree:testClearAll()
    local t = clear_tree()
    t:clear()
    lu.assertEquals(collect(t, "span", 0, 6), { { 0, 6 } })
    lu.assertEquals(t:bytes(), 6)
end

-- edit base: [(0,2),(A,6),(0,2)] via a mark on [2,8)
local function edit_tree(comp)
    comp = comp or newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 2, 6)
    return t
end

function TestTree:testSpliceInheritsLeft()
    local t = edit_tree()
    t:splice(3, 2, 4) -- delete 2, insert 4, inherits the left (A)
    lu.assertEquals(t:bytes(), 12)
    local s = collect(t, "span", 0, 12)
    lu.assertEquals(#s, 3)
    lu.assertEquals(s[1], { 0, 2 })
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    lu.assertEquals(s[2][2], 8)
    lu.assertEquals(s[3], { 0, 2 })
end

function TestTree:testAppendInheritsLeft()
    local t = edit_tree()
    t:append(2, 2) -- at the A head: inherits the left (id 0)
    lu.assertEquals(t:bytes(), 12)
    local s = collect(t, "span", 0, 12)
    lu.assertEquals(s[1], { 0, 4 })
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    lu.assertEquals(s[2][2], 6)
    lu.assertEquals(s[3], { 0, 2 })
end

function TestTree:testInsertInheritsRight()
    local t = edit_tree()
    t:insert(2, 2) -- at the A head: inherits the right (A)
    lu.assertEquals(t:bytes(), 12)
    local s = collect(t, "span", 0, 12)
    lu.assertEquals(s[1], { 0, 2 })
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    lu.assertEquals(s[2][2], 8)
    lu.assertEquals(s[3], { 0, 2 })
end

function TestTree:testRemove()
    local t = edit_tree()
    t:remove(2, 4)
    lu.assertEquals(t:bytes(), 6)
    local s = collect(t, "span", 0, 6)
    lu.assertEquals(s[1], { 0, 2 })
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    lu.assertEquals(s[2][2], 2)
    lu.assertEquals(s[3], { 0, 2 })
end

function TestTree:testSpanTruncate()
    local t = edit_tree() -- [(0,2),(A,6),(0,2)]
    local mid = collect(t, "span", 0, 10)[2][1]
    -- spans start at segment heads: the window start does not clip
    -- (len 9 spans past the tree end, the trailing 0 segment shows
    -- in full)
    lu.assertEquals(collect(t, "span", 3, 9), { { mid, 6 }, { 0, 2 } })
    -- off >= bytes: zero iterations
    lu.assertEquals(collect(t, "span", 10, 20), {})
    -- endoff beyond bytes clamps at the tree end
    lu.assertEquals(collect(t, "span", 0, 99), { { 0, 2 }, { mid, 6 }, { 0, 2 } })
    -- a zero-length range still iterates from off to off+len
    local z = collect(t, "span", 5, 5)
    lu.assertEquals(#z, 2)
    lu.assertEquals(z[1][1], mid)
    lu.assertEquals(z[1][2], 6)
    lu.assertEquals(z[2][1], 0)
    lu.assertEquals(z[2][2], 2)
end

-- ns-filtered base: [(0,2),(A,2),(AB,4),(B,2)] (a [2,8), b [4,10))
local function ns_tree(comp)
    comp = comp or newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    comp:namespace("b", 2)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 2, 6)
    t:mark("b", { bg = 2 }, 4, 6)
    return t
end

function TestTree:testSpanNsFilter()
    local t = ns_tree()
    -- every returned span carries the layer's own (unmerged) attr
    local s = collect(t, "span", "a", 0, 10)
    lu.assertEquals(#s, 2)
    assert_attr(cur_comp, s[1][1], { fg = 1 })
    lu.assertEquals(s[1][2], 2)
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    lu.assertEquals(s[2][2], 4)
    local s2 = collect(t, "span", "b", 0, 10)
    lu.assertEquals(#s2, 2)
    assert_attr(cur_comp, s2[1][1], { bg = 2 })
    lu.assertEquals(s2[1][2], 4)
    assert_attr(cur_comp, s2[2][1], { bg = 2 })
    lu.assertEquals(s2[2][2], 2)
    -- inclusive start: the segment at off is part of the query window
    local s3 = collect(t, "span", "a", 3, 5)
    lu.assertEquals(#s3, 2)
    assert_attr(cur_comp, s3[1][1], { fg = 1 })
    lu.assertEquals(s3[1][2], 2)
    assert_attr(cur_comp, s3[2][1], { fg = 1 })
    lu.assertEquals(s3[2][2], 4)
    -- a segment starting exactly at off is hit (row-head hint case)
    local s5 = collect(t, "span", "a", 2, 8)
    lu.assertEquals(#s5, 2)
    assert_attr(cur_comp, s5[1][1], { fg = 1 })
    lu.assertEquals(s5[1][2], 2)
    assert_attr(cur_comp, s5[2][1], { fg = 1 })
    lu.assertEquals(s5[2][2], 4)
    -- truncation inside the first matching segment
    local s4 = collect(t, "span", "a", 0, 3)
    lu.assertEquals(#s4, 1)
    assert_attr(cur_comp, s4[1][1], { fg = 1 })
    lu.assertEquals(s4[1][2], 1)
end

function TestTree:testSpanUnknownNs()
    local t = ns_tree()
    lu.assertErrorMsgContains("unknown namespace",
        function() t:span("zzz", 0, 5) end)
end

function TestTree:testMarkUnknownNs()
    local comp = newcomp(); local t = newtree(comp)
    t:splice(0, 0, 10)
    lu.assertErrorMsgContains("unknown namespace",
        function() t:mark("zzz", { fg = 1 }, 0, 5) end)
end

function TestTree:testClearUnknownNs()
    local comp = newcomp(); local t = newtree(comp)
    t:splice(0, 0, 10)
    lu.assertErrorMsgContains("unknown namespace",
        function() t:clear("zzz") end)
    lu.assertErrorMsgContains("unknown namespace",
        function() t:clear("zzz", 0, 5) end)
end

function TestTree:testParams()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    lu.assertErrorMsgContains("invalid offset",
        function() t:mark("a", {}, -1, 5) end)
    lu.assertErrorMsgContains("invalid length",
        function() t:mark("a", {}, 0, -5) end)
    lu.assertErrorMsgContains("number expected",
        function() t:mark("a", ANY, 0, 5) end)
    lu.assertErrorMsgContains("namespace name must be a string",
        ---@diagnostic disable-next-line: param-type-mismatch
        function() t:mark(42, {}, 0, 5) end)
    lu.assertErrorMsgContains("invalid namespace name",
        function() t:mark("", {}, 0, 5) end)
    lu.assertErrorMsgContains("invalid offset",
        function() t:splice(-1, 0, 0) end)
    lu.assertErrorMsgContains("invalid length",
        function() t:splice(0, -1, 0) end)
    lu.assertErrorMsgContains("invalid length",
        function() t:splice(0, 0, -1) end)
    lu.assertErrorMsgContains("invalid offset",
        function() t:append(-1, 0) end)
    lu.assertErrorMsgContains("invalid length",
        function() t:append(0, -1) end)
    lu.assertErrorMsgContains("invalid offset",
        function() t:insert(-1, 0) end)
    lu.assertErrorMsgContains("invalid length",
        function() t:insert(0, -1) end)
    lu.assertErrorMsgContains("invalid offset",
        function() t:remove(-1, 0) end)
    lu.assertErrorMsgContains("invalid length",
        function() t:remove(0, -1) end)
    lu.assertErrorMsgContains("invalid offset",
        function() t:seek(-1) end)
    lu.assertErrorMsgContains("invalid offset",
        ---@diagnostic disable-next-line: param-type-mismatch, missing-parameter
        function() t:span(-1, 5) end)
    lu.assertErrorMsgContains("invalid length",
        ---@diagnostic disable-next-line: param-type-mismatch, missing-parameter
        function() t:span(0, -1) end)
    lu.assertErrorMsgContains("invalid offset",
        function() t:clear(nil, -1, 5) end)
    lu.assertErrorMsgContains("invalid length",
        function() t:clear(nil, 0, -1) end)
end

function TestTree:tearDown()
    -- helpers (edit_tree/ns_tree/clear_tree) register a and b
    unregister_names(newcomp(), { "a", "b" })
end

-- ======== cursor ========
TestCursor = {}

function TestCursor:testCreate()
    local comp = newcomp(); local t = newtree(comp)
    local c = t:cursor()
    lu.assertEquals(c:offset(), 0)
    lu.assertNil(c:style()) -- empty tree
end

function TestCursor:testSeekRelocate()
    local t = edit_tree() -- [(0,2),(A,6),(0,2)]
    local c1 = t:seek(0)
    lu.assertEquals(c1:offset(), 0)
    -- seek(off) creates a fresh cursor
    local c2 = t:seek(5)
    lu.assertEquals(c2:offset(), 5)
    lu.assertNotEquals(c1, c2)
    -- seek(off, c) reuses the same object
    lu.assertEquals(t:seek(7, c1), c1)
    lu.assertEquals(c1:offset(), 7)
end

function TestCursor:testCrossTreeSeek()
    -- two trees bound to one compositor: c:seek rebinds and
    -- re-anchors (ns registry + intern ids are shared)
    local comp = newcomp()
    local t1, t2 = newtree(comp), newtree(comp)
    comp:namespace("a", 1)
    t1:splice(0, 0, 5)
    t2:splice(0, 0, 5)
    t1:mark("a", { fg = 1 }, 0, 5)
    t2:mark("a", { fg = 2 }, 0, 5)
    local c = t1:seek(2)
    local off, len, tab, id = c:style()
    assert_attr(comp, id, { fg = 1 })
    c:seek(t2, 3)
    lu.assertEquals(c:offset(), 3)
    off, len, tab, id = c:style()
    assert_attr(comp, id, { fg = 2 })
    -- the old tree stays alive and untouched
    lu.assertEquals(t1:bytes(), 5)
    assert_attr(comp, collect(t1, "span", 0, 5)[1][1], { fg = 1 })
    -- edits through the rebound cursor hit the new tree only
    c:mark("a", { fg = 3 }, 2) -- [3,5): the layer value flips there
    local s2 = collect(t2, "span", 0, 5)
    assert_attr(comp, s2[1][1], { fg = 2 })
    lu.assertEquals(s2[1][2], 3)
    assert_attr(comp, s2[2][1], { fg = 3 })
    lu.assertEquals(s2[2][2], 2)
    assert_attr(comp, collect(t1, "span", 0, 5)[1][1], { fg = 1 })
end

function TestCursor:testLocateAdvance()
    local t = edit_tree() -- [(0,2),(A,6),(0,2)]
    local c = t:seek(0)
    c:advance(5)
    lu.assertEquals(c:offset(), 5)
    c:locate(2)
    lu.assertEquals(c:offset(), 2)
    c:advance(-1)
    lu.assertEquals(c:offset(), 1)
    c:advance(0) -- zero delta: no-op
    lu.assertEquals(c:offset(), 1)
    -- locate past the tree end: virtual region reads nil
    c:locate(50)
    lu.assertEquals(c:offset(), 50)
    lu.assertNil(c:style())
end

function TestCursor:testThreeStates()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 2, 8) -- [(0,2),(A,8)]
    -- in-segment: style reads the segment (off = segment head)
    local c = t:seek(2)
    local off, len, tab, id = c:style()
    lu.assertEquals(off, 2)
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(len, 8)
    -- segment-end / tree-end: next is nil, style is nil, prev rewinds
    lu.assertNil(c:next())
    lu.assertNil(c:style())
    off, len, tab, id = c:prev()
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(len, 8)
    -- prev at the segment head falls to the id-0 segment, which is a
    -- real mark in the v4 span flow
    off, len, tab, id = c:prev()
    lu.assertEquals(off, 0)
    lu.assertEquals(len, 2)
    lu.assertNil(tab.fg)
    -- tree head sits on an id-0 segment: style nil, next jumps to A
    local c2 = t:seek(0)
    lu.assertNil(c2:style())
    off, len, tab, id = c2:next()
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(len, 8)
    -- past the tree end: style nil, prev reads the last segment
    local c3 = t:seek(10)
    lu.assertNil(c3:style())
    off, len, tab, id = c3:prev()
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(len, 8)
end

function TestCursor:testNextPrevFilter()
    local t = ns_tree() -- [(0,2),(A,2),(AB,4),(B,2)]
    -- exclusive: seek into the middle of a segment; next skips it
    local c = t:seek(3)
    local off, len, tab, id = c:style()
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(len, 2) -- whole segment (off = segment head)
    off, len, tab, id = c:next("a")
    assert_attr(cur_comp, id, { fg = 1 }) -- a's own attr, not the merge
    lu.assertEquals(len, 4)
    lu.assertNil(c:next("a"))
    -- a filter skips segments without the layer
    local c2 = t:seek(3)
    off, len, tab, id = c2:next("b")
    assert_attr(cur_comp, id, { bg = 2 })
    lu.assertEquals(len, 4)
    off, len, tab, id = c2:next("b")
    assert_attr(cur_comp, id, { bg = 2 })
    lu.assertEquals(len, 2)
    lu.assertNil(c2:next("b"))
    -- prev filters symmetrically
    local c3 = t:seek(9)
    off, len, tab, id = c3:prev("a")
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(len, 4)
    local c4 = t:seek(9)
    off, len, tab, id = c4:prev("b")
    assert_attr(cur_comp, id, { bg = 2 })
    lu.assertEquals(len, 2) -- whole segment (mark span = seg span)
end

function TestCursor:testEditVerbs()
    -- edits step off the run tail (spK_offtail): style reads the
    -- next segment
    local comp = newcomp()
    local t = edit_tree(comp)
    local c = t:seek(4)
    local m = c:mark("a", { fg = 2 }, 3)
    lu.assertEquals(m, comp:intern({ fg = 2 })) -- mark returns the attr id
    lu.assertEquals(c:offset(), 7)
    local off, len, tab, id = c:style()
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(off, 7)
    lu.assertEquals(len, 1)
    local s = collect(t, "span", 0, 10)
    lu.assertEquals(#s, 5)
    lu.assertEquals(s[1], { 0, 2 })
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    lu.assertEquals(s[2][2], 2)
    assert_attr(cur_comp, s[3][1], { fg = 2 })
    lu.assertEquals(s[3][2], 3)
    assert_attr(cur_comp, s[4][1], { fg = 1 })
    lu.assertEquals(s[4][2], 1)
    lu.assertEquals(s[5], { 0, 2 })
    -- clear(len): full clear from the cursor; lands at the run tail
    local t2 = edit_tree(comp)
    local c2 = t2:seek(2)
    c2:clear(3)
    lu.assertEquals(c2:offset(), 5)
    off, len, tab, id = c2:style()
    assert_attr(comp, id, { fg = 1 })
    lu.assertEquals(off, 5)
    lu.assertEquals(len, 3)
    local s2 = collect(t2, "span", 0, 10)
    lu.assertEquals(#s2, 3)
    lu.assertEquals(s2[1], { 0, 5 })
    assert_attr(comp, s2[2][1], { fg = 1 })
    lu.assertEquals(s2[2][2], 3)
    lu.assertEquals(s2[3], { 0, 2 })
    -- clear(nil, len): same as clear(len), from the cursor
    local t2b = edit_tree(comp)
    local c2b = t2b:seek(2)
    ---@diagnostic disable-next-line: param-type-mismatch
    c2b:clear(nil, 3)
    lu.assertEquals(c2b:offset(), 5)
    lu.assertEquals(collect(t2b, "span", 0, 10), { { 0, 5 }, { s2[2][1], 3 }, { 0, 2 } })
    -- clear(ns, len): layer clear from the cursor
    local t3 = edit_tree(comp)
    local c3 = t3:seek(4)
    c3:clear("a", 3)
    lu.assertEquals(c3:offset(), 7)
    off, len, tab, id = c3:style()
    assert_attr(comp, id, { fg = 1 })
    lu.assertEquals(off, 7)
    lu.assertEquals(len, 1)
    local s3 = collect(t3, "span", 0, 10)
    lu.assertEquals(#s3, 5)
    lu.assertEquals(s3[1], { 0, 2 })
    assert_attr(comp, s3[2][1], { fg = 1 })
    lu.assertEquals(s3[2][2], 2)
    lu.assertEquals(s3[3], { 0, 3 })
    assert_attr(comp, s3[4][1], { fg = 1 })
    lu.assertEquals(s3[4][2], 1)
    lu.assertEquals(s3[5], { 0, 2 })
    -- splice lands at the inserted run's tail (del ran past the
    -- cursor segment's tail, so the landing is off + ins)
    local t4 = edit_tree(comp)
    local c4 = t4:seek(4)
    c4:splice(2, 3)
    lu.assertEquals(c4:offset(), 7)
    local off4, len4, tab4, id4 = c4:style()
    assert_attr(comp, id4, { fg = 1 })
    lu.assertEquals(len4, 7) -- whole grown segment [2,9)
    lu.assertEquals(t4:bytes(), 11)
    -- append lands at the inserted run's tail
    local t5 = edit_tree(comp)
    local c5 = t5:seek(4)
    c5:append(3)
    lu.assertEquals(c5:offset(), 7)
    local off5, len5, tab5, id5 = c5:style()
    assert_attr(comp, id5, { fg = 1 })
    lu.assertEquals(len5, 9) -- whole grown segment [2,11)
    -- insert returns to the insertion point
    local t6 = edit_tree(comp)
    local c6 = t6:seek(4)
    c6:insert(3)
    lu.assertEquals(c6:offset(), 4)
    local off6, len6, tab6, id6 = c6:style()
    assert_attr(comp, id6, { fg = 1 })
    lu.assertEquals(len6, 9) -- whole grown segment [2,11)
    -- remove leaves the cursor in place (now at a segment end)
    local t7 = edit_tree(comp)
    local c7 = t7:seek(4)
    c7:remove(4)
    lu.assertEquals(c7:offset(), 4)
    lu.assertNil(c7:style())
    local s7 = collect(t7, "span", 0, 6)
    lu.assertEquals(#s7, 3)
    lu.assertEquals(s7[1], { 0, 2 })
    assert_attr(comp, s7[2][1], { fg = 1 })
    lu.assertEquals(s7[2][2], 2)
    lu.assertEquals(s7[3], { 0, 2 })
end

function TestCursor:testEpochGuard()
    local t = edit_tree()
    local c1 = t:seek(0)
    local c2 = t:seek(0)
    c2:mark("a", { fg = 1 }, 2) -- a foreign edit
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:offset() end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:style() end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:next() end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:prev() end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:locate(0) end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:advance(1) end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:mark("a", {}, 1) end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:clear(1) end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:splice(1, 1) end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:append(1) end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:insert(1) end)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:remove(1) end)
    -- the editing cursor stays valid (self-sync)
    lu.assertEquals(c2:offset(), 2)
    -- tree-level edits invalidate too; seek revives
    c1:seek(t, 0)
    lu.assertEquals(c1:offset(), 0)
    t:mark("a", { fg = 1 }, 0, 1)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c1:offset() end)
end

function TestCursor:testSeekRevive()
    local t = edit_tree()
    local c = t:seek(3)
    t:mark("a", { fg = 1 }, 0, 5)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c:offset() end)
    c:seek(t, 7)
    lu.assertEquals(c:offset(), 7)
    -- t:seek(off, c) is a revive path as well
    local c2 = t:seek(0)
    t:splice(0, 0, 2)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c2:style() end)
    t:seek(4, c2)
    lu.assertEquals(c2:offset(), 4)
end

function TestCursor:testSelfEditSync()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    local c = t:seek(0)
    c:mark("a", { fg = 1 }, 3)
    lu.assertEquals(c:offset(), 3)
    lu.assertNil(c:style()) -- run tail
    c:append(2) -- inherits the marked run; lands at its tail
    lu.assertEquals(c:offset(), 5)
    lu.assertNil(c:style())
    local s = collect(t, "span", 0, 10)
    assert_attr(cur_comp, s[1][1], { fg = 1 })
    lu.assertEquals(s[1][2], 5)
    lu.assertEquals(s[2], { 0, 5 })
end

function TestCursor:testChaining()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    local c = t:seek(0)
    -- mark returns the attr id (not self), so it opens the chain
    c:mark("a", { fg = 1 }, 2)
    local r = c:append(1):insert(1):remove(1)
        :splice(1, 1):clear(2):locate(0):advance(1)
    lu.assertEquals(c, r)
    lu.assertEquals(c:offset(), 1)
    -- tree-level verbs return self too
    lu.assertEquals(t:mark("a", { fg = 1 }, 0, 2), comp:intern({ fg = 1 }))
    lu.assertEquals(t:clear(), t)
end

function TestCursor:testSpanEpochGuard()
    local t = edit_tree()
    lu.assertErrorMsgContains("cursor invalidated",
        function()
            for off, len, tab, id in t:span(0, 5) do
                t:mark("a", { fg = 2 }, 0, 1)
            end
        end)
end

function TestCursor:testParams()
    local t = edit_tree()
    local c = t:seek(0)
    lu.assertErrorMsgContains("invalid length",
        function() c:mark("a", {}, -1) end)
    lu.assertErrorMsgContains("invalid length",
        function() c:clear(-1) end)
    lu.assertErrorMsgContains("invalid length",
        function() c:clear("a", -1) end)
    lu.assertErrorMsgContains("invalid length",
        function() c:splice(-1, 0) end)
    lu.assertErrorMsgContains("invalid length",
        function() c:splice(0, -1) end)
    lu.assertErrorMsgContains("invalid length",
        function() c:append(-1) end)
    lu.assertErrorMsgContains("invalid length",
        function() c:insert(-1) end)
    lu.assertErrorMsgContains("invalid length",
        function() c:remove(-1) end)
    lu.assertErrorMsgContains("invalid offset",
        function() c:locate(-1) end)
    lu.assertErrorMsgContains("invalid offset",
        function() c:seek(t, -1) end)
    lu.assertErrorMsgContains("unknown namespace",
        function() c:next("zzz") end)
    lu.assertErrorMsgContains("unknown namespace",
        function() c:prev("zzz") end)
    lu.assertErrorMsgContains("unknown namespace",
        function() c:mark("zzz", {}, 1) end)
    lu.assertErrorMsgContains("unknown namespace",
        function() c:clear("zzz", 1) end)
    lu.assertErrorMsgContains("number expected",
        function() c:mark(nil, ANY, 1) end)
end

function TestCursor:tearDown()
    -- edit_tree/ns_tree/clear_tree register a and b
    unregister_names(newcomp(), { "a", "b" })
end

-- ======== merge: ns layer composition ========
-- ======== ephemeral layers ========
TestEph = {}

function TestEph:testFillShapes()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    -- fresh range in a gap
    t:mark("h", { fg = 1 }, 4, 10)
    lu.assertEquals(collect(t, "span", "h", 0, 20),
        { { comp:intern({ fg = 1 }), 10 } })
    -- split the middle: [4,14) -> [4,9) [9,11) [11,14)
    t:mark("h", { fg = 2 }, 9, 2)
    local s = collect(t, "span", "h", 0, 20)
    lu.assertEquals(s[1][2], 5)
    lu.assertEquals(s[2][2], 2)
    lu.assertEquals(s[3][2], 3)
    -- cover the whole thing with one id: merges into one seg
    t:mark("h", { fg = 3 }, 3, 12)
    s = collect(t, "span", "h", 0, 20)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][2], 12)
    -- idempotent cover write: same id, same range, no shape change
    t:mark("h", { fg = 3 }, 3, 12)
    s = collect(t, "span", "h", 0, 20)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][2], 12)
end

function TestEph:testFillEdges()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 5, 5)
    -- fill ending exactly at the seg start: joins at the boundary
    t:mark("h", { fg = 1 }, 0, 5)
    lu.assertEquals(collect(t, "span", "h", 0, 20)[1][2], 10)
    -- fill starting exactly at the seg end: joins at the boundary
    t:mark("h", { fg = 1 }, 10, 5)
    lu.assertEquals(collect(t, "span", "h", 0, 20)[1][2], 15)
    -- zero-length fill is a no-op
    t:mark("h", { fg = 9 }, 3, 0)
    lu.assertEquals(collect(t, "span", "h", 0, 20)[1][2], 15)
    -- cover from inside a seg across the next: middle-seg extension
    t:mark("h", { fg = 1 }, 12, 10)
    lu.assertEquals(collect(t, "span", "h", 0, 20)[1][2], 20)
    assert_attr(cur_comp, collect(t, "span", "h", 0, 20)[1][1], { fg = 1 })
end

function TestEph:testClear()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 0, 20)
    -- range clear in the middle splits the seg in two
    t:clear("h", 8, 4)
    local s = collect(t, "span", "h", 0, 20)
    lu.assertEquals(s[1][2], 8)
    lu.assertEquals(s[2][2], 8)
    -- clear the middle seg entirely: the cleared range stays a hole
    -- (adjacent same-id segs never arise from removal)
    t:mark("h", { fg = 2 }, 6, 6)
    t:clear("h", 6, 6)
    s = collect(t, "span", "h", 0, 20)
    lu.assertEquals(#s, 2)
    lu.assertEquals(s[1][2], 6)
    lu.assertEquals(s[2][2], 8)
    -- whole-ns clear empties the layer
    t:clear("h")
    lu.assertEquals(collect(t, "span", "h", 0, 20), {})
    -- zero-length clear is a no-op
    t:mark("h", { fg = 3 }, 0, 10)
    t:clear("h", 3, 0)
    lu.assertEquals(#collect(t, "span", "h", 0, 20), 1)
end

function TestEph:testClearAllForms()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    comp:namespace("a", 2)
    t:splice(0, 0, 20)
    t:mark("a", { bg = 1 }, 0, 20)
    t:mark("h", { fg = 1 }, 5, 10)
    -- t:clear(nil, off, len) clears the tree AND all eph layers
    t:clear(nil, 0, 10)
    lu.assertEquals(collect(t, "span", "h", 0, 20),
        { { comp:intern({ fg = 1 }), 5 } }) -- [10,15) survives
    -- t:clear() whole-tree all-layers
    t:mark("h", { fg = 1 }, 2, 5)
    t:mark("a", { bg = 1 }, 10, 5)
    t:clear()
    lu.assertEquals(collect(t, "span", "h", 0, 20), {})
    lu.assertEquals(collect(t, "styled", 0, 20), { { 0, 20 } })
end

function TestEph:testSegmentsFiltered()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 4, 6)
    t:mark("h", { fg = 2 }, 15, 3)
    -- clipping to the requested range; segs past the window drop
    local s = collect(t, "span", "h", 6, 6)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][1], comp:intern({ fg = 1 }))
    lu.assertEquals(s[1][2], 4)
    -- range in a gap: no segments
    lu.assertEquals(collect(t, "span", "h", 11, 3), {})
    -- seg starting before the window clips its head
    s = collect(t, "span", "h", 8, 6)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][2], 2)
end

function TestEph:testMergedBelow()
    -- p>=0 eph layers sit below the tree: tree attrs win
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("sem", 1)
    comp:namespace("h", 2, "e")
    t:splice(0, 0, 20)
    t:mark("sem", { fg = 7 }, 0, 20)
    t:mark("h", { fg = 9 }, 4, 6)
    local s = collect(t, "styled", 0, 20)
    lu.assertEquals(s[1][2], 4)
    assert_attr(cur_comp, s[1][1], { fg = 7 })
    lu.assertEquals(s[2][2], 6)
    assert_attr(cur_comp, s[2][1], { fg = 7 }) -- sem covers h
    lu.assertEquals(s[3][2], 10)
end

function TestEph:testMergedAbove()
    -- p<0 eph layers sit above the tree: eph attrs win
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("sem", 1)
    comp:namespace("h", -1, "e")
    t:splice(0, 0, 20)
    t:mark("sem", { fg = 7 }, 0, 20)
    t:mark("h", { fg = 9 }, 4, 6)
    local s = collect(t, "styled", 0, 20)
    lu.assertEquals(s[1][2], 4)
    assert_attr(cur_comp, s[1][1], { fg = 7 })
    lu.assertEquals(s[2][2], 6)
    assert_attr(cur_comp, s[2][1], { fg = 9 }) -- h covers sem
    lu.assertEquals(s[3][2], 10)
end

function TestEph:testMergedLayers()
    -- multiple eph layers: priority order among themselves, both
    -- sides of the tree, disjoint keys pass through
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("sem", 1)
    comp:namespace("lo", 5, "e")
    comp:namespace("hi", -5, "e")
    t:splice(0, 0, 20)
    t:mark("sem", { fg = 7, bg = 7 }, 0, 20)
    t:mark("lo", { fg = 1 }, 2, 10)
    t:mark("hi", { bg = 2 }, 4, 6)
    local s = collect(t, "styled", 0, 20)
    -- [4,6): hi above the tree; lo below: bg=2, fg=7
    assert_attr(cur_comp, s[3][1], { fg = 7, bg = 2 })
    -- [2,4): only lo (below): fg 7 wins over fg 1, bg 7 passes
    assert_attr(cur_comp, s[2][1], { fg = 7, bg = 7 })
    -- [6,12): only lo: same as above
    assert_attr(cur_comp, s[4][1], { fg = 7, bg = 7 })
end

function TestEph:testMergedPlain()
    -- uncolored tree (id 0): eph contribution returns its own id
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 5, 5)
    local s = collect(t, "styled", 0, 20)
    lu.assertEquals(s[2][1], comp:intern({ fg = 1 }))
    lu.assertEquals(s[2][2], 5)
    -- no eph coverage: zero synthesis, tree ids pass through
    lu.assertEquals(s[1][1], 0)
    lu.assertEquals(s[3][1], 0)
end

function TestEph:testMergedEphOnly()
    -- eph fill over uncolored tree: merged == eph id itself
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 0, 20)
    local s = collect(t, "styled", 0, 20)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][1], comp:intern({ fg = 1 }))
    lu.assertEquals(s[1][2], 20)
end

function TestEph:testScrollWindows()
    -- two disjoint windows without clearing: both read correctly
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 500)
    t:mark("h", { fg = 1 }, 100, 20)
    t:mark("h", { fg = 2 }, 200, 20)
    local s = collect(t, "styled", 95, 25)
    assert_attr(cur_comp, s[2][1], { fg = 1 })
    lu.assertEquals(s[2][2], 20)
    s = collect(t, "styled", 195, 25)
    assert_attr(cur_comp, s[2][1], { fg = 2 })
    lu.assertEquals(s[2][2], 20)
    -- overlapping windows: same text, same fill id, result identical
    t:mark("h", { fg = 1 }, 105, 20)
    s = collect(t, "styled", 95, 25)
    lu.assertEquals(s[2][2], 20)
    assert_attr(cur_comp, s[2][1], { fg = 1 })
end

function TestEph:testCursorMerged()
    -- cursor eph filter walks the sv list; plain reads = the tree
    -- mark stream (eph synthesis is a styled-flow concern)
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("sem", 1)
    comp:namespace("h", -1, "e")
    t:splice(0, 0, 20)
    t:mark("sem", { fg = 7 }, 0, 20)
    t:mark("h", { fg = 9 }, 4, 6)
    local c = t:cursor()
    -- next from mid-seg: the eph remainder
    c:locate(5)
    local off, len, tab, id = c:next("h")
    lu.assertEquals(off, 5)
    lu.assertEquals(len, 5)
    assert_attr(cur_comp, id, { fg = 9 })
    lu.assertEquals(c:offset(), 10)
    -- next past the last eph seg: nil
    lu.assertNil(c:next("h"))
    -- prev from past the seg: full eph seg, cursor at its head
    c:locate(15)
    off, len, tab, id = c:prev("h")
    lu.assertEquals(off, 4)
    lu.assertEquals(len, 6)
    assert_attr(cur_comp, id, { fg = 9 })
    lu.assertEquals(c:offset(), 4)
    -- plain read at 0: the tree mark (style = current segment)
    off, len, tab, id = c:style()
    lu.assertEquals(off, 0)
    lu.assertEquals(len, 20)
    assert_attr(cur_comp, id, { fg = 7 })
    -- prev at 0 / next past the end: nil
    c:locate(0)
    lu.assertNil(c:prev())
    c:locate(20)
    lu.assertNil(c:next())
end

function TestEph:testCursorEphFilter()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 4, 4)
    t:mark("h", { fg = 2 }, 12, 4)
    local c = t:cursor()
    -- next from inside a seg: remainder
    c:locate(5)
    local off, len, tab, id = c:next("h")
    lu.assertEquals(len, 3)
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(c:offset(), 8)
    -- next from a gap: the next seg full
    off, len, tab, id = c:next("h")
    lu.assertEquals(len, 4)
    assert_attr(cur_comp, id, { fg = 2 })
    lu.assertEquals(c:offset(), 16)
    -- next past the last seg: nil
    lu.assertNil(c:next("h"))
    -- prev from inside a seg: full seg, cursor at head
    c:locate(14)
    off, len, tab, id = c:prev("h")
    lu.assertEquals(len, 4)
    assert_attr(cur_comp, id, { fg = 2 })
    lu.assertEquals(c:offset(), 12)
    -- prev from a gap: the previous seg
    off, len, tab, id = c:prev("h")
    lu.assertEquals(len, 4)
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(c:offset(), 4)
    lu.assertNil(c:prev("h"))
    -- empty layer: nil reads
    t:clear("h")
    c:locate(0)
    lu.assertNil(c:next("h"))
    lu.assertNil(c:prev("h"))
end

function TestEph:testEditClears()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 0, 20)
    -- tree verbs
    t:append(4, 2)
    lu.assertEquals(collect(t, "span", "h", 0, 24), {})
    t:mark("h", { fg = 1 }, 0, 24)
    t:insert(4, 2)
    lu.assertEquals(collect(t, "span", "h", 0, 26), {})
    t:mark("h", { fg = 1 }, 0, 26)
    t:splice(0, 2, 1)
    lu.assertEquals(collect(t, "span", "h", 0, 25), {})
    t:mark("h", { fg = 1 }, 0, 25)
    t:remove(0, 2)
    lu.assertEquals(collect(t, "span", "h", 0, 23), {})
    -- cursor verbs
    t:mark("h", { fg = 1 }, 0, 23)
    local c = t:cursor()
    c:append(2)
    lu.assertEquals(collect(t, "span", "h", 0, 25), {})
    t:mark("h", { fg = 1 }, 0, 25)
    c:locate(0)
    c:insert(2)
    lu.assertEquals(collect(t, "span", "h", 0, 27), {})
    t:mark("h", { fg = 1 }, 0, 27)
    c:locate(0)
    c:splice(2, 1)
    lu.assertEquals(collect(t, "span", "h", 0, 26), {})
    t:mark("h", { fg = 1 }, 0, 26)
    c:locate(0)
    c:remove(2)
    lu.assertEquals(collect(t, "span", "h", 0, 24), {})
    -- ordinary ns fill/clear do NOT clear eph
    comp:namespace("a", 1)
    t:mark("h", { fg = 1 }, 0, 24)
    t:mark("a", { bg = 1 }, 0, 24)
    lu.assertEquals(#collect(t, "span", "h", 0, 24), 1)
    t:clear("a")
    lu.assertEquals(#collect(t, "span", "h", 0, 24), 1)
end

function TestEph:testEphZeroEpoch()
    -- eph fill leaves the tree epoch alone: the cursor stays valid
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    local c = t:cursor()
    -- eph filter reads through the layer; the cursor survives the op
    t:mark("h", { fg = 1 }, 0, 10)
    local off, len, tab, id = c:next("h")
    lu.assertEquals(off, 0)
    lu.assertEquals(len, 10)
    assert_attr(cur_comp, id, { fg = 1 })
    lu.assertEquals(c:offset(), 10)
    t:clear("h")
    lu.assertEquals(c:offset(), 10)
    -- ordinary fill still bumps the epoch (regression guard)
    comp:namespace("a", 1)
    t:mark("a", { bg = 1 }, 0, 10)
    lu.assertErrorMsgContains("cursor invalidated",
        function() c:offset() end)
end

function TestEph:testCursorFillClear()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    local c = t:cursor()
    c:mark("h", { fg = 1 }, 10)
    lu.assertEquals(collect(t, "span", "h", 0, 20),
        { { comp:intern({ fg = 1 }), 10 } })
    -- cursor position unchanged by eph fill
    lu.assertEquals(c:offset(), 0)
    -- c:clear(ns, len) clears from the cursor
    c:clear("h", 5)
    lu.assertEquals(collect(t, "span", "h", 0, 20),
        { { comp:intern({ fg = 1 }), 5 } })
    -- range all-clear clears tree + all eph layers
    comp:namespace("a", 1)
    t:mark("a", { bg = 1 }, 0, 20)
    t:clear(nil, 0, 3)
    lu.assertEquals(collect(t, "span", "h", 0, 20),
        { { comp:intern({ fg = 1 }), 5 } })
    local s = collect(t, "styled", 0, 20)
    lu.assertEquals(s[1][2], 3) -- cleared head: id 0
    assert_attr(cur_comp, s[2][1], { bg = 1 })
    lu.assertEquals(s[2][2], 2)
    assert_attr(cur_comp, s[3][1], { bg = 1, fg = 1 })
end

function TestEph:testMergedIdCache()
    -- repeated merged reads of the same combo reuse the recorded map
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("sem", 1)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("sem", { fg = 7 }, 0, 20)
    t:mark("h", { fg = 9 }, 4, 6)
    local s1 = collect(t, "styled", 0, 20)
    local s2 = collect(t, "styled", 0, 20)
    lu.assertEquals(s1[2][1], s2[2][1])
    -- decode via t: merged id carries both layers
    assert_attr(cur_comp, s1[2][1], { fg = 7 })
    -- re-fill same combo after clear: id stable
    t:clear("h")
    t:mark("h", { fg = 9 }, 4, 6)
    lu.assertEquals(collect(t, "styled", 0, 20)[2][1], s1[2][1])
end

function TestEph:testFillJoinNeighbors()
    -- same-id fill over a full seg: merges both sides into one run
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 0, 5)
    t:mark("h", { fg = 1 }, 10, 5)
    t:mark("h", { fg = 2 }, 5, 5)
    lu.assertEquals(collect(t, "span", "h", 0, 20),
        { { comp:intern({ fg = 1 }), 5 }, { comp:intern({ fg = 2 }), 5 },
          { comp:intern({ fg = 1 }), 5 } })
    t:mark("h", { fg = 1 }, 5, 5)
    local s = collect(t, "span", "h", 0, 20)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][2], 15)
    -- a same-id seg across a gap stays split (no contiguity, no merge)
    t:mark("h", { fg = 3 }, 16, 3)
    s = collect(t, "span", "h", 0, 20)
    lu.assertEquals(#s, 2)
end

function TestEph:testFillTrimBoth()
    -- fill spanning two segs with both partial overlaps: general path
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 0, 10)
    t:mark("h", { fg = 2 }, 10, 10)
    t:mark("h", { fg = 3 }, 5, 10)
    local s = collect(t, "span", "h", 0, 20)
    lu.assertEquals(s[1][1], comp:intern({ fg = 1 }))
    lu.assertEquals(s[1][2], 5)
    lu.assertEquals(s[2][1], comp:intern({ fg = 3 }))
    lu.assertEquals(s[2][2], 10)
    lu.assertEquals(s[3][1], comp:intern({ fg = 2 }))
    lu.assertEquals(s[3][2], 5)
end

function TestEph:testSegGrowth()
    -- many segments force the flat array past its initial capacity
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 100)
    for i = 0, 24 do
        t:mark("h", { fg = i % 3 }, i * 4 + 1, 2)
    end
    local s = collect(t, "span", "h", 0, 100)
    lu.assertEquals(#s, 25)
    lu.assertEquals(s[25][2], 2)
end

function TestEph:testIteratorPastBytes()
    -- merged window extending past the tree end stops cleanly
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 5, 10)
    local s = collect(t, "styled", 0, 40)
    lu.assertEquals(s[#s][2], 5) -- tail [15,20)
    lu.assertEquals(s[#s][1], 0)
end

function TestEph:testStyleBoundary()
    -- a filtered next landing at a seg end that doubles as the next
    -- segment's head: style reads that segment (v4 mark flow)
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    comp:namespace("a", 1)
    t:splice(0, 0, 20)
    t:mark("a", { bg = 1 }, 0, 10)
    t:mark("a", { bg = 2 }, 10, 10)
    t:mark("h", { fg = 1 }, 4, 4)
    local c = t:cursor()
    c:next("a") -- lands at 10 = the first tree seg end
    -- 10 is also the second segment's head: style reads that segment
    local off, len, tab, id = c:style()
    lu.assertEquals(off, 10)
    lu.assertEquals(len, 10)
    assert_attr(cur_comp, id, { bg = 2 })
    c:locate(11)
    off, len, tab, id = c:style()
    lu.assertNotNil(id)
end

function TestEph:testCursorClearAll()
    -- c:clear(len) clears tree + every eph layer from the cursor
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    comp:namespace("a", 1)
    t:splice(0, 0, 20)
    t:mark("a", { bg = 1 }, 0, 20)
    t:mark("h", { fg = 1 }, 0, 10)
    local c = t:cursor()
    c:locate(2)
    c:clear(6)
    -- eph split into two same-id halves around the cleared hole
    local f = comp:intern({ fg = 1 })
    lu.assertEquals(collect(t, "span", "h", 0, 20), { { f, 2 }, { f, 2 } })
    assert_attr(cur_comp, collect(t, "styled", 0, 20)[1][1], { bg = 1 })
    lu.assertEquals(collect(t, "styled", 0, 20)[1][2], 2)
    -- 3-arg nil form clears all layers too (eph fill is zero-epoch,
    -- the cursor stays valid; it sits at 8 after the first clear)
    t:mark("h", { fg = 1 }, 0, 10)
    c:clear(nil, 5)
    lu.assertEquals(collect(t, "span", "h", 0, 20), { { f, 8 } })
end

function TestEph:testEphReprio()
    -- eph re-register with a new priority: zero tree refold
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("h", 1, "e")
    comp:namespace("h2", 2, "e")
    t:splice(0, 0, 20)
    t:mark("h", { fg = 1 }, 0, 10)
    t:mark("h2", { fg = 2 }, 0, 10)
    assert_attr(cur_comp, collect(t, "styled", 0, 20)[1][1], { fg = 2 })
    lu.assertEquals(comp:namespace("h2", 5, "e"), 2)
    assert_attr(cur_comp, collect(t, "styled", 0, 20)[1][1], { fg = 2 })
end

function TestEph:testArbError()
    -- an error inside mark is transactional: the segment keeps its
    -- old value (the write never lands). v4 interning raises on
    -- __hash errors before the fill op touches the tree.
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 0, 10)
    local boom = setmetatable({}, { __hash = function() error("boom") end })
    lu.assertErrorMsgContains("boom",
        function() t:mark("a", boom, 0, 10) end)
    local s = collect(t, "styled", 0, 10)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][2], 10)
    assert_attr(cur_comp, s[1][1], { fg = 1 })
end

function TestEph:tearDown()
    unregister_names(newcomp(), { "a", "h", "h2", "hi", "lo", "sem" })
end

TestMerge = {}

function TestMerge:testPriorityTrace()
    -- shared keys resolve by priority; unshared keys pass through
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    comp:namespace("b", 2)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1, bg = 1 }, 0, 4)
    t:mark("b", { bg = 2 }, 0, 4)
    t:mark("a", { fg = 1, bg = 1 }, 4, 6) -- a alone on [4,10)
    local s = collect(t, "styled", 0, 10)
    lu.assertEquals(#s, 2)
    assert_attr(cur_comp, s[1][1], { fg = 1, bg = 2 }) -- b wins bg
    assert_attr(cur_comp, s[2][1], { fg = 1, bg = 1 })
    -- overwriting a layer leaves the other untouched
    t:mark("a", { fg = 3 }, 0, 4)
    assert_attr(cur_comp, collect(t, "styled", 0, 4)[1][1], { fg = 3, bg = 2 })
end

function TestMerge:testEmptyMap()
    -- clearing every layer empties the map: segment id 0
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    comp:namespace("b", 2)
    t:splice(0, 0, 6)
    t:mark("a", { fg = 1 }, 0, 6)
    t:mark("b", { bg = 2 }, 0, 6)
    t:clear("a")
    t:clear("b")
    lu.assertEquals(collect(t, "styled", 0, 6), { { 0, 6 } })
end

function TestMerge:testUnregisterIsolated()
    -- unregistering one ns only prunes its own layer
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    comp:namespace("b", 2)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 0, 10)
    t:mark("b", { bg = 2 }, 0, 10)
    assert_attr(cur_comp, collect(t, "styled", 0, 10)[1][1], { fg = 1, bg = 2 })
    lu.assertEquals(comp:namespace("a", nil), 1)
    local s = collect(t, "styled", 0, 10)
    lu.assertEquals(#s, 1)
    lu.assertEquals(s[1][2], 10)
    assert_attr(cur_comp, s[1][1], { bg = 2 })
    lu.assertNil(comp:attr(s[1][1]).fg)
    lu.assertEquals(t:bytes(), 10)
end

function TestMerge:testReorderIdempotent()
    -- re-registering the same priority must not disturb the tree
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 0, 10)
    local mid = collect(t, "styled", 0, 10)[1][1]
    lu.assertEquals(comp:namespace("a", 1), 1)
    lu.assertEquals(collect(t, "styled", 0, 10)[1][1], mid)
    lu.assertEquals(collect(t, "styled", 0, 10)[1][2], 10)
    lu.assertEquals(comp:namespace("a"), 1)
end

function TestMerge:testPairGrowth()
    -- many distinct ns sets grow the chain slot pool: correctness
    -- of a deep fold across 17 registered layers (mapof is gone)
    local comp = newcomp(); local t = newtree(comp)
    for i = 1, 17 do comp:namespace("n" .. i, i) end
    t:splice(0, 0, 17)
    for i = 1, 17 do -- span i folds the layers n1..ni
        for j = 1, i do
            t:mark("n" .. j, { fg = j, bg = j + 1 }, i - 1, 1)
        end
    end
    lu.assertEquals(t:bytes(), 17)
    -- the last span folds all 17 layers; the highest priority wins
    local a = comp:attr(collect(t, "styled", 16, 1)[1][1])
    lu.assertEquals(a.fg, 17)
    lu.assertEquals(a.bg, 18)
end

function TestMerge:tearDown()
    local names = {}
    for i = 1, 17 do names[i] = "n" .. i end
    names[18], names[19] = "a", "b"
    unregister_names(newcomp(), names)
end

-- ======== mark stream: span decomposition (v4) ========
TestSpan = {}

function TestSpan:testQuad()
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("qd1", 1)
    t:splice(0, 0, 10)
    local attr = { fg = 7, bold = true }
    local mid = t:mark("qd1", attr, 2, 5)
    lu.assertEquals(mid, comp:intern(attr))
    -- span yields (off, len, table, id); the table is the interned
    -- attr itself: consumers can skip the attr() lookup
    local out = {}
    for off, len, tab, id in t:span(0, 10) do
        out[#out + 1] = { off, len, tab, id }
    end
    lu.assertEquals(#out, 3)
    lu.assertEquals(out[1][1], 0)
    lu.assertEquals(out[1][2], 2)
    lu.assertEquals(out[1][3], comp:attr(0))
    lu.assertEquals(out[1][4], 0)
    lu.assertEquals(out[2][1], 2)
    lu.assertEquals(out[2][2], 5)
    lu.assertEquals(out[2][3], comp:attr(mid))
    lu.assertEquals(out[2][3].fg, 7)
    lu.assertEquals(out[2][3].bold, true)
    lu.assertEquals(out[2][4], mid)
    lu.assertEquals(out[3][1], 7)
    lu.assertEquals(out[3][2], 3)
    lu.assertEquals(out[3][4], 0)
end

function TestSpan:testOverlapEmit()
    -- one merged segment with k marks emits k times: same off/len
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("oe1", 1)
    comp:namespace("oe2", 2)
    t:splice(0, 0, 10)
    local a1 = t:mark("oe1", { fg = 1 }, 0, 10)
    local a2 = t:mark("oe2", { bg = 2 }, 0, 10)
    local s = collect(t, "span", 0, 10)
    lu.assertEquals(#s, 2)
    lu.assertEquals(s[1], { a1, 10 })
    lu.assertEquals(s[2], { a2, 10 })
    local out = {}
    for off, len in t:span(0, 10) do
        out[#out + 1] = { off, len }
    end
    lu.assertEquals(out[1], { 0, 10 })
    lu.assertEquals(out[2], { 0, 10 })
end

function TestSpan:testPriorityOrder()
    -- overlap emission order: ascending priority, ties by registration
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("po1", 3)
    comp:namespace("po2", 1)
    comp:namespace("po3", 2)
    comp:namespace("po4", 2)
    t:splice(0, 0, 10)
    local fg = {}
    for i = 1, 4 do fg[i] = comp:intern({ fg = i }) end
    t:mark("po1", fg[1], 0, 10)
    t:mark("po2", fg[2], 0, 10)
    t:mark("po3", fg[3], 0, 10)
    t:mark("po4", fg[4], 0, 10)
    local ids = {}
    for _, _, _, id in t:span(0, 10) do
        ids[#ids + 1] = id
    end
    lu.assertEquals(ids, { fg[2], fg[3], fg[4], fg[1] })
end

function TestSpan:testNsFilter()
    -- span(ns): that ns's own slot per segment; segments without the
    -- layer are skipped entirely
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("nf1", 1)
    comp:namespace("nf2", 2)
    comp:namespace("nf3", 3)
    t:splice(0, 0, 12)
    t:mark("nf1", { fg = 1 }, 2, 6) -- [2,8)
    t:mark("nf2", { bg = 2 }, 4, 4) -- [4,8)
    t:mark("nf3", { bold = true }, 4, 2) -- [4,6)
    local f1 = comp:intern({ fg = 1 })
    local s1 = collect(t, "span", "nf1", 0, 12)
    lu.assertEquals(s1, { { f1, 2 }, { f1, 2 }, { f1, 2 } })
    local b2 = comp:intern({ bg = 2 })
    lu.assertEquals(collect(t, "span", "nf2", 0, 12), { { b2, 2 }, { b2, 2 } })
    local bl = comp:intern({ bold = true })
    lu.assertEquals(collect(t, "span", "nf3", 0, 12), { { bl, 2 } })
    -- segments without the layer drop out
    lu.assertEquals(collect(t, "span", "nf3", 6, 6), {})
    -- inclusive start: the segment at off is in the query window
    lu.assertEquals(collect(t, "span", "nf1", 3, 9),
        { { f1, 2 }, { f1, 2 }, { f1, 2 } })
end

function TestSpan:testClearNoGhostSeg()
    -- clear on an empty slot must not build a CLEAR segment (the
    -- exclusive-start skip used to hide such ghosts; the query flow
    -- reads them back as empty attrs)
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("cg1", 1)
    t:splice(0, 0, 10)
    t:clear("cg1", 2, 4)
    lu.assertEquals(collect(t, "span", "cg1", 0, 10), {})
    t:mark("cg1", { fg = 1 }, 2, 4)
    lu.assertEquals(#collect(t, "span", "cg1", 0, 10), 1)
    t:clear("cg1", 2, 4)
    lu.assertEquals(collect(t, "span", "cg1", 0, 10), {})
end

function TestSpan:testEphWalk()
    -- eph ns: sv list walk, the window clips both segment ends
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("ew1", 1, "e")
    t:splice(0, 0, 20)
    local f1 = comp:intern({ fg = 1 })
    local f2 = comp:intern({ fg = 2 })
    t:mark("ew1", f1, 4, 6) -- [4,10)
    t:mark("ew1", f2, 15, 3) -- [15,18)
    lu.assertEquals(collect(t, "span", "ew1", 0, 20), { { f1, 6 }, { f2, 3 } })
    -- the window clips a covered segment head
    lu.assertEquals(collect(t, "span", "ew1", 6, 8), { { f1, 4 } })
    -- the window clips a covered segment tail
    lu.assertEquals(collect(t, "span", "ew1", 0, 8), { { f1, 4 } })
    -- gaps yield nothing
    lu.assertEquals(collect(t, "span", "ew1", 11, 3), {})
end

function TestSpan:testCursorMidx()
    -- midx state: one merged segment with two marks walks mark by
    -- mark; the cross-segment next lands on the next segment's first
    -- mark; prev walks back symmetrically
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("mx1", 1)
    comp:namespace("mx2", 2)
    t:splice(0, 0, 16)
    local fg1 = comp:intern({ fg = 1 })
    local bg2 = comp:intern({ bg = 2 })
    local bg3 = comp:intern({ bg = 3 })
    t:mark("mx1", fg1, 0, 10)
    t:mark("mx2", bg2, 0, 10) -- [0,10): mx1 + mx2 composite
    t:mark("mx2", bg3, 10, 6) -- [10,16)
    local c = t:seek(3)
    local off, len, tab, id = c:style()
    lu.assertEquals(off, 0)
    lu.assertEquals(len, 10)
    lu.assertEquals(id, fg1)
    off, len, tab, id = c:next() -- same segment, next mark
    lu.assertEquals(off, 0)
    lu.assertEquals(len, 10)
    lu.assertEquals(id, bg2)
    off, len, tab, id = c:next() -- cross-segment: its first mark
    lu.assertEquals(off, 10)
    lu.assertEquals(len, 6)
    lu.assertEquals(id, bg3)
    lu.assertEquals(c:offset(), 10)
    lu.assertNil(c:next())
    -- prev walks back symmetrically (last mark first)
    off, len, tab, id = c:prev()
    lu.assertEquals(off, 10)
    lu.assertEquals(len, 6)
    lu.assertEquals(id, bg3)
    off, len, tab, id = c:prev()
    lu.assertEquals(off, 0)
    lu.assertEquals(id, bg2)
    off, len, tab, id = c:prev()
    lu.assertEquals(off, 0)
    lu.assertEquals(id, fg1)
    lu.assertNil(c:prev())
    -- style reads the current midx
    local c2 = t:seek(3)
    c2:style() -- establishes the midx state
    c2:next() -- midx + 1
    off, len, tab, id = c2:style()
    lu.assertEquals(id, bg2)
end

function TestSpan:testCursorNsFilter()
    -- next(ns)/prev(ns) prune to the layer's own slot, decomposing
    -- multi-ns segments
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("cn1", 1)
    comp:namespace("cn2", 2)
    comp:namespace("cn3", 3)
    t:splice(0, 0, 12)
    t:mark("cn1", { fg = 1 }, 2, 6) -- [2,8)
    t:mark("cn2", { bg = 2 }, 4, 4) -- [4,8)
    t:mark("cn3", { bold = true }, 4, 2) -- [4,6)
    local c = t:seek(5)
    local off, len, tab, id = c:next("cn1")
    lu.assertEquals(off, 6)
    lu.assertEquals(len, 2)
    lu.assertEquals(id, comp:intern({ fg = 1 }))
    lu.assertEquals(c:offset(), 6)
    lu.assertNil(c:next("cn1"))
    local c2 = t:seek(5)
    off, len, tab, id = c2:next("cn2")
    lu.assertEquals(off, 6)
    lu.assertEquals(id, comp:intern({ bg = 2 }))
    lu.assertNil(c2:next("cn2"))
    -- no more cn3 segments past [4,6): nil
    lu.assertNil(t:seek(5):next("cn3"))
    -- prev prunes symmetrically
    local c3 = t:seek(7)
    off, len, tab, id = c3:prev("cn3")
    lu.assertEquals(off, 4)
    lu.assertEquals(len, 2)
    lu.assertEquals(id, comp:intern({ bold = true }))
    lu.assertNil(c3:prev("cn3"))
end

function TestSpan:testWindowClip()
    -- the window clips segment tails (mark heads keep their range)
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("wc1", 1)
    t:splice(0, 0, 10)
    local fg1 = comp:intern({ fg = 1 })
    t:mark("wc1", fg1, 2, 6) -- [(0,2),(A,6),(0,2)]
    -- styled: boundary-split at the window edges
    local mid = collect(t, "styled", 0, 10)[2][1]
    lu.assertEquals(collect(t, "styled", 3, 5), { { mid, 5 } })
    lu.assertEquals(collect(t, "styled", 0, 4), { { 0, 2 }, { mid, 2 } })
    -- span: mark heads do not clip; tails clip at the window end
    lu.assertEquals(collect(t, "span", 3, 9), { { fg1, 6 }, { 0, 2 } })
    lu.assertEquals(collect(t, "span", 5, 5), { { fg1, 6 }, { 0, 2 } })
    lu.assertEquals(collect(t, "span", 8, 5), { { 0, 2 } })
end

function TestSpan:tearDown()
    unregister_names(newcomp(), { "qd1", "oe1", "oe2", "po1", "po2", "po3",
        "po4", "nf1", "nf2", "nf3", "ew1", "mx1", "mx2", "cn1", "cn2",
        "cn3", "wc1" })
end

-- ======== id domains: plain / op / composite (v4) ========
TestIds = {}

-- COMP_ID_START = 1 << 24 (internal): composite ids live at 2^24 and
-- above, plain ids (attr + op) below; the literal must match the
-- internal constant
local COMP_ID_START = 2 ^ 24

function TestIds:testFlatSeg()
    -- ns 0 (unaffiliated) mark: span yields the attr id; styled id
    -- resolves to the same attr (a re-mark flattens the stored op id
    -- back to the attr id)
    local comp = newcomp(); local t = newtree(comp)
    t:splice(0, 0, 10)
    local attr = { fg = 5 }
    local aid = t:mark(nil, attr, 2, 4)
    lu.assertEquals(aid, comp:intern(attr))
    lu.assertEquals(collect(t, "span", 0, 10), { { 0, 2 }, { aid, 4 }, { 0, 4 } })
    local sid = collect(t, "styled", 2, 4)[1][1]
    lu.assertTrue(sid < COMP_ID_START)
    assert_attr(cur_comp, sid, attr)
    -- idempotent re-mark merges: the stored id flattens to the attr id
    t:mark(nil, attr, 2, 4)
    lu.assertEquals(collect(t, "styled", 2, 4)[1][1], aid)
end

function TestIds:testSingleOpDirect()
    -- one ns: the segment holds the op id directly (zero synthesis) --
    -- still a plain-region id whose attr() returns the folded table
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("sod1", 1)
    t:splice(0, 0, 10)
    local aid = t:mark("sod1", { fg = 9 }, 2, 4)
    lu.assertEquals(collect(t, "span", 0, 10), { { 0, 2 }, { aid, 4 }, { 0, 4 } })
    local s = collect(t, "styled", 0, 10)
    lu.assertEquals(#s, 3)
    lu.assertTrue(s[2][1] < COMP_ID_START)
    assert_attr(cur_comp, s[2][1], { fg = 9 })
end

function TestIds:testCompositeId()
    -- two ns on one range: a composite id (>= 2^24) whose folded attr
    -- spans both layers; span decomposes back into the attr ids
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("co1", 1)
    comp:namespace("co2", 2)
    t:splice(0, 0, 10)
    local f1 = t:mark("co1", { fg = 1 }, 0, 10)
    local b2 = t:mark("co2", { bg = 2 }, 0, 10)
    local sid = collect(t, "styled", 0, 10)[1][1]
    lu.assertTrue(sid >= COMP_ID_START)
    assert_attr(cur_comp, sid, { fg = 1, bg = 2 })
    local s = collect(t, "span", 0, 10)
    lu.assertEquals(#s, 2)
    lu.assertEquals(s[1], { f1, 10 })
    lu.assertEquals(s[2], { b2, 10 })
end

function TestIds:testCompositeReuse()
    -- clearing a layer releases the composite; refilling the same
    -- composition reclaims the same id (freelist reuse)
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("cr1", 1)
    comp:namespace("cr2", 2)
    t:splice(0, 0, 10)
    t:mark("cr1", { fg = 1 }, 0, 10)
    t:mark("cr2", { bg = 2 }, 0, 10)
    local before = collect(t, "styled", 0, 10)[1][1]
    lu.assertTrue(before >= COMP_ID_START)
    t:clear("cr2")
    lu.assertTrue(collect(t, "styled", 0, 10)[1][1] < COMP_ID_START)
    t:mark("cr2", { bg = 2 }, 0, 10)
    lu.assertEquals(collect(t, "styled", 0, 10)[1][1], before)
end

function TestIds:testChainCompression()
    -- 100 alternating overwrites of one layer inside a 3-ns segment:
    -- the chain rebuilds compressed every time (decode stays correct)
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("cc1", 1)
    comp:namespace("cc2", 2)
    comp:namespace("cc3", 3)
    t:splice(0, 0, 10)
    local f1 = comp:intern({ fg = 1 })
    local f2 = comp:intern({ fg = 2 })
    local f3 = comp:intern({ fg = 3 })
    t:mark("cc1", f1, 0, 10)
    t:mark("cc2", f2, 0, 10)
    t:mark("cc3", f3, 0, 10)
    local f4 = comp:intern({ fg = 4 })
    for _ = 1, 100 do
        t:mark("cc2", _ % 2 == 0 and f4 or f2, 0, 10)
    end
    -- span decodes all three marks (cc2 holds the last value)
    local ids = {}
    for _, _, _, id in t:span(0, 10) do
        ids[#ids + 1] = id
    end
    lu.assertEquals(ids, { f1, f4, f3 })
    -- styled folds correctly (cc3 wins fg)
    local s = collect(t, "styled", 0, 10)
    lu.assertEquals(#s, 1)
    assert_attr(cur_comp, s[1][1], { fg = 3 })
end

function TestIds:testUnmark()
    -- unmark(id) clears every mark carrying the attr: count = matching
    -- segments, cross-ns included; unknown ids return 0
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("um1", 1)
    comp:namespace("um2", 2)
    t:splice(0, 0, 20)
    local fg1 = t:mark("um1", { fg = 1 }, 0, 10)
    t:mark("um2", fg1, 4, 6) -- same attr id on a second ns
    t:mark("um2", { bg = 2 }, 10, 4) -- non-matching: excluded from count
    t:mark("um2", fg1, 14, 6)
    -- 3 of 4 segments carry fg=1: partial hit count
    lu.assertEquals(t:unmark(fg1), 3)
    for _, _, _, id in t:span(0, 20) do
        lu.assertNotEquals(id, fg1)
    end
    -- repeat and unknown ids return 0
    lu.assertEquals(t:unmark(fg1), 0)
    lu.assertEquals(t:unmark(99999), 0)
end

function TestIds:testMarkReturnId()
    -- mark returns the attr id; the id round-trips as payload
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("mr1", 1)
    t:splice(0, 0, 10)
    local attr = { fg = 7 }
    local aid = t:mark("mr1", attr, 2, 4)
    lu.assertEquals(aid, comp:intern(attr))
    lu.assertEquals(t:mark("mr1", aid, 6, 2), aid)
    -- adjacent same-id marks merge into one segment
    lu.assertEquals(collect(t, "span", 0, 10), { { 0, 2 }, { aid, 6 }, { 0, 2 } })
    -- cursor mark with an id payload (the number branch)
    local c = t:seek(0)
    local id0 = comp:intern({ fg = 9 })
    lu.assertEquals(c:mark("mr1", id0, 4), id0)
    -- invalid id payload raises
    lu.assertErrorMsgContains("unknown style id",
        function() c:mark("mr1", 99999, 1) end)
    lu.assertErrorMsgContains("unknown style id",
        function() t:mark("mr1", 99999, 0, 1) end)
end

function TestIds:testIdInvalidation()
    -- tree edits may recycle composite ids: a saved styled id must
    -- never crash attr() (nil or a table); attr ids stay stable
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("iv1", 1)
    comp:namespace("iv2", 2)
    t:splice(0, 0, 10)
    t:mark("iv1", { fg = 1 }, 0, 10)
    t:mark("iv2", { bg = 2 }, 0, 10)
    local old = collect(t, "styled", 0, 10)[1][1]
    local aid = comp:intern({ fg = 1 })
    t:mark("iv1", { fg = 3 }, 0, 10) -- invalidates the old composite
    local a = comp:attr(old)
    if a ~= nil then
        lu.assertIsTable(a)
    end
    -- attr ids survive edits (intern semantics)
    lu.assertEquals(comp:attr(aid).fg, 1)
end

function TestIds:tearDown()
    unregister_names(newcomp(), { "sod1", "co1", "co2", "cr1", "cr2",
        "cc1", "cc2", "cc3", "um1", "um2", "mr1", "iv1", "iv2" })
end

-- ======== coverage edge cases ========
TestCov = {}

function TestCov:testInternHugeCanon()
    -- a canon past the buffer capacity forces luaL_addchar's
    -- prepbuffsize growth path in cp_canon
    local comp = newcomp()
    local big = string.rep("x", 9000)
    local a = comp:intern({ fg = big, bg = 1 })
    lu.assertEquals(comp:attr(a).fg, big)
    lu.assertEquals(comp:attr(a).bg, 1)
    lu.assertEquals(comp:intern({ fg = big, bg = 1 }), a)
end

function TestCov:testAttrPastChain()
    -- composite-range ids beyond the chain space read nil (no crash)
    local comp = newcomp()
    lu.assertNil(comp:attr(2 ^ 24 + 100000))
end

function TestCov:testClearSpanningTwoSegs()
    -- range clear trimming inside two adjacent eph segs: the general
    -- sv_clear path (t != s + 1) with both side trims
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("hc1", 1, "e")
    t:splice(0, 0, 20)
    t:mark("hc1", { fg = 1 }, 0, 10)
    t:mark("hc1", { fg = 2 }, 10, 10)
    t:clear("hc1", 3, 14)
    local s = collect(t, "span", "hc1", 0, 20)
    lu.assertEquals(#s, 2)
    lu.assertEquals(s[1][2], 3)
    lu.assertEquals(s[2][2], 3)
    comp:namespace("hc1", nil)
end

function TestCov:testCompositeBirthNilBase()
    -- a composite whose base is a plain attr (ns 0 mark first);
    -- splitting the segment births the halves through arb(id, 0),
    -- exercising lst_segmask's ns-0 pair skip
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("cb1", 1)
    t:splice(0, 0, 10)
    local f1 = comp:intern({ fg = 1 })
    local b2 = comp:intern({ bg = 2 })
    t:mark(nil, f1, 0, 10)
    t:mark("cb1", b2, 0, 10)
    t:splice(3, 0, 1)
    local s = collect(t, "span", 0, 11)
    lu.assertEquals(#s, 2)
    lu.assertEquals(s[1][1], f1)
    lu.assertEquals(s[2][1], b2)
    comp:namespace("cb1", nil)
end

function TestCov:testRtmpGrow()
    -- >128 eph layers force lst_mergecalc's rtmp re-growth through
    -- both the doubled-capacity branch and the cap-doubling loop
    local comp = newcomp(); local t = newtree(comp)
    t:splice(0, 0, 5)
    local i
    for i = 1, 100 do
        comp:namespace("g" .. i, i, "e")
        t:mark("g" .. i, { fg = 1 }, 0, 5)
    end
    lu.assertNotNil(collect(t, "styled", 0, 5)[1])
    for i = 101, 257 do
        comp:namespace("g" .. i, i, "e")
        t:mark("g" .. i, { fg = 1 }, 0, 5)
    end
    local s = collect(t, "styled", 0, 5)
    lu.assertNotNil(s[1])
    for i = 1, 257 do
        comp:namespace("g" .. i, nil)
    end
end

function TestCov:testEphUnregisterTop()
    -- unregistering the highest eph layer shrinks ephcnt
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("et1", 1, "e")
    comp:namespace("et2", 2, "e")
    t:splice(0, 0, 10)
    t:mark("et1", { fg = 1 }, 0, 5)
    t:mark("et2", { fg = 2 }, 0, 5)
    lu.assertEquals(comp:namespace("et2", nil), 2)
    lu.assertNil(comp:namespace("et2"))
    lu.assertEquals(comp:namespace("et1"), 1)
    comp:namespace("et1", nil)
end

function TestCov:testCompositeZeroBase()
    -- a composite whose base pair is the empty attr (id 0): the hash
    -- reuse and the release both walk the a == 0 edge
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("zb1", 1)
    t:splice(0, 0, 20)
    local b2 = comp:intern({ bg = 2 })
    t:mark(nil, 0, 0, 5)
    t:mark("zb1", b2, 0, 5) -- composite [(0,0),(zb1,b2)]
    t:mark(nil, 0, 10, 5)
    t:mark("zb1", b2, 10, 5) -- same composition: hash reuse
    local s = collect(t, "styled", 0, 20)
    lu.assertEquals(s[1][1], s[3][1])
    lu.assertTrue(s[1][1] >= 2 ^ 24)
    -- clearing the layer releases the composite from both segments
    t:clear("zb1")
    lu.assertEquals(collect(t, "styled", 0, 20)[1][1], 0)
    comp:namespace("zb1", nil)
end

function TestCov:testStyledParams()
    local comp = newcomp(); local t = newtree(comp)
    t:splice(0, 0, 10)
    lu.assertErrorMsgContains("invalid offset",
        function() t:styled(-1, 5) end)
    lu.assertErrorMsgContains("invalid length",
        function() t:styled(0, -1) end)
end

function TestCov:testCursorMoveInvalidatesStyleCache()
    -- moving the cursor across a piece boundary with a valid style
    -- cache resets the cached segment on the next read
    local t = ns_tree() -- [(0,2),(A,2),(AB,4),(B,2)]
    local c = t:seek(2)
    local off, len, tab, id = c:style()
    assert_attr(cur_comp, id, { fg = 1 })
    c:advance(2) -- cross into the AB piece
    off, len, tab, id = c:style()
    lu.assertEquals(off, 4)
    lu.assertEquals(len, 4)
    lu.assertNotNil(id)
end

function TestCov:testNextIntoHole()
    -- next() landing on an id-0 tail segment still yields a span
    local t = edit_tree() -- [(0,2),(A,6),(0,2)]
    local c = t:seek(2)
    local off, len, tab, id = c:next()
    lu.assertEquals(off, 8)
    lu.assertEquals(len, 2)
    lu.assertEquals(id, 0)
    lu.assertNil(tab.fg)
end

function TestCov:testCmarkIdPayload()
    -- cursor mark with a numeric id payload: the valid-id edge
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("cp1", 1)
    t:splice(0, 0, 10)
    local c = t:seek(0)
    local id0 = comp:intern({ fg = 9 })
    lu.assertEquals(c:mark("cp1", id0, 4), id0)
    lu.assertErrorMsgContains("unknown style id",
        function() c:mark("cp1", 9999999, 1) end)
    comp:namespace("cp1", nil)
end

function TestCov:testCompositeGhostReuse()
    -- release + rebuild of the same composition: the hash-reuse
    -- interplay between the freelist and the chain table
    local comp = newcomp(); local t = newtree(comp)
    comp:namespace("gh1", 1)
    comp:namespace("gh2", 2)
    t:splice(0, 0, 10)
    t:mark("gh1", { fg = 1 }, 0, 10)
    t:mark("gh2", { bg = 2 }, 0, 10)
    local before = collect(t, "styled", 0, 10)[1][1]
    t:clear("gh2")
    t:mark("gh2", { bg = 2 }, 0, 10)
    lu.assertEquals(collect(t, "styled", 0, 10)[1][1], before)
    comp:namespace("gh1", nil)
    comp:namespace("gh2", nil)
end

function TestCov:tearDown()
    local names = { "hc1", "cb1", "cp1", "gh1", "gh2", "et1", "et2", "zb1" }
    local k = #names
    for i = 1, 257 do names[k + i] = "g" .. i end
    unregister_names(newcomp(), names)
end

-- ======== compositor component (v5): factory / binding / isolation ========
TestComp = {}

function TestComp:testModuleIsTreeMetatable()
    local comp = newcomp()
    local t = newtree(comp)
    lu.assertEquals(getmetatable(t), sp)
    lu.assertEquals(sp.cursor, t.cursor) -- one field, two call forms
end

function TestComp:testNewRequiresComp()
    lu.assertErrorMsgContains("spantree.Compositor expected",
        ---@diagnostic disable-next-line: missing-parameter
        function() sp.new() end)
    lu.assertErrorMsgContains("spantree.Compositor expected",
        function() sp.new({}) end)
end

function TestComp:testCompositorIsolation()
    -- every compositor() call owns a fresh state: intern ids and the
    -- ns registry never leak across compositors
    local c1, c2 = newcomp(), newcomp()
    c1:fields({ "fg", "bg" })
    c2:fields({ "fg" })
    lu.assertNotNil(c1:attr(c1:intern({ bg = 1 })).bg)
    lu.assertNil(c2:attr(c2:intern({ bg = 1 })).bg)
    c1:namespace("iso", 1)
    lu.assertNil(c2:namespace("iso"))
end

function TestComp:testTreeNoStyleFace()
    -- the tree does not forward compositor style services
    local comp = newcomp()
    local t = newtree(comp)
    ---@diagnostic disable: undefined-field
    lu.assertNil(t.intern)
    lu.assertNil(t.attr)
    lu.assertNil(t.fields)
    lu.assertNil(t.namespace)
    ---@diagnostic enable: undefined-field
    -- the compositor keeps them
    lu.assertNotNil(comp.intern)
    lu.assertNotNil(comp.attr)
    lu.assertNotNil(comp.fields)
    lu.assertNotNil(comp.namespace)
end

function TestComp:testUnregisterNoTrees()
    -- unregister with no bound tree: the walk is a no-op, no crash
    local comp = newcomp()
    comp:namespace("empty", 1)
    lu.assertEquals(comp:namespace("empty", nil), 1)
    lu.assertNil(comp:namespace("empty"))
end

function TestComp:testCompositorGc()
    -- dropping a compositor and its tree frees the cp state (gc runs
    -- __gc); a fresh compositor stays fully functional
    local function build()
        local comp = sp.compositor()
        local t = sp.new(comp)
        comp:namespace("g", 1)
        t:splice(0, 0, 4)
        t:mark("g", { fg = 1 }, 0, 4)
    end
    build()
    collectgarbage()
    local comp = newcomp()
    comp:namespace("ok", 1)
    lu.assertEquals(comp:namespace("ok"), 1)
    comp:namespace("ok", nil)
end

function TestComp:testCursorFactory()
    -- sp.cursor(t) = t:cursor(): one field, both call forms
    local comp = newcomp()
    local t = newtree(comp)
    comp:namespace("a", 1)
    t:splice(0, 0, 10)
    t:mark("a", { fg = 1 }, 0, 10)
    local c = sp.cursor(t)
    local off, len, tab, id = c:style()
    lu.assertEquals(off, 0)
    assert_attr(comp, id, { fg = 1 })
end

os.exit(lu.LuaUnit.run(), true)
