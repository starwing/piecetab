-- json binding tests (luaunit harness).
-- run: just lua/json
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = root .. "/?.lua;" .. dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/lua/luajit/?.so;"
      or root .. "/lua/?.so;")
    .. package.cpath
    .. ";./lua/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"

local lu = require "luaunit"
local json = require "json"

-- decode that must succeed (assert narrows the nil case for LuaLS)
local function dec(s)
  local v, err = json.decode(s)
  assert(v ~= nil, err)
  return v
end

TestDecode = {}

function TestDecode:testScalars()
  lu.assertEquals(dec("null"), json.null)
  lu.assertEquals(dec("true"), true)
  lu.assertEquals(dec("false"), false)
  lu.assertEquals(dec("42"), 42)
  lu.assertEquals(dec("-7"), -7)
  lu.assertEquals(dec("3.5"), 3.5)
  lu.assertEquals(dec('"hi"'), "hi")
  lu.assertEquals(dec('""'), "")
end

function TestDecode:testNested()
  local v = dec('{"a": [1, 2, {"b": "x"}], "c": null}')
  lu.assertEquals(v.a[1], 1)
  lu.assertEquals(v.a[3].b, "x")
  lu.assertEquals(v.c, json.null)
end

function TestDecode:testEscapes()
  lu.assertEquals(dec('"a\\nb\\t\\\\\\""'), 'a\nb\t\\"')
  lu.assertEquals(dec('"\\u4f60\\u597d"'), "你好")
end

function TestDecode:testParseError()
  local v, err, pos = json.decode("{bad")
  lu.assertIsNil(v)
  lu.assertEquals(err, "unexpected_character")
  lu.assertEquals(pos, 1)
  local v2, err2 = json.decode("")
  lu.assertIsNil(v2)
  lu.assertNotIsNil(err2)
end

function TestDecode:testErrorCodes()
  -- one bad input per reachable yyjson_read_code (2/8/11 are not
  -- producible through the binding: OOM / comment flag / internal literal)
  local cases = {
    { "",       "invalid_parameter" },
    { "  ",     "empty_content" },
    { "1.2.3",  "unexpected_content" },
    { "[1,",    "unexpected_end" },
    { "{bad",   "unexpected_character" },
    { "[1,2,]", "json_structure" },
    { "01",     "invalid_number" },
    { '"a\\q"', "invalid_string" },
  }
  for _, c in ipairs(cases) do
    local v, err = json.decode(c[1])
    lu.assertIsNil(v)
    lu.assertEquals(err, c[2])
  end
end

TestEncode = {}

function TestEncode:testScalars()
  lu.assertEquals(json.encode(true), "true")
  lu.assertEquals(json.encode(false), "false")
  lu.assertEquals(json.encode(42), "42")
  lu.assertEquals(json.encode(-7), "-7")
  lu.assertEquals(json.encode(3.5), "3.5")
  lu.assertEquals(json.encode("hi"), '"hi"')
  lu.assertEquals(json.encode(json.null), "null")
end

function TestEncode:testTable()
  lu.assertEquals(json.encode({}), "{}")
  lu.assertEquals(json.encode({ 1, 2, 3 }), "[1,2,3]")
  -- object key order is unspecified (pairs); compare via decode
  local v = dec(json.encode({ a = 1, b = "x" }))
  lu.assertEquals(v.a, 1)
  lu.assertEquals(v.b, "x")
  -- mixed keys: not a pure array -> object; numeric keys are rejected
  lu.assertError(function() json.encode({ 1, 2, x = 3 }) end)
end

function TestEncode:testNested()
  local s = json.encode({ a = { 1, 2 }, b = json.null })
  local v = dec(s)
  lu.assertEquals(v.a[2], 2)
  lu.assertEquals(v.b, json.null)
end

function TestEncode:testRejects()
  lu.assertError(function() json.encode(nil) end)
  lu.assertError(function() json.encode(function() end) end)
  lu.assertError(function() json.encode(io.stdout) end) -- non-marker userdata
end

function TestEncode:testDeepNesting()
  -- depth guard trips above the max nesting level
  --- @type any
  local v = 1
  for _ = 1, 70 do v = { v } end
  lu.assertError(function() json.encode(v) end)
  local deep = string.rep("[", 70) .. "1" .. string.rep("]", 70)
  lu.assertError(function() json.decode(deep) end)
end

TestMarkers = {}

TestType = {}

function TestMarkers:testDecodeRoundtrip()
  -- decoded arrays/objects keep their JSON kind through encode
  lu.assertEquals(json.encode(dec("[]")), "[]")
  lu.assertEquals(json.encode(dec("{}")), "{}")
  lu.assertEquals(json.encode(dec("[1,2]")), "[1,2]")
  lu.assertEquals(json.encode(dec('{"a":1}')), '{"a":1}')
end

function TestMarkers:testConstructors()
  lu.assertEquals(json.encode(json.array()), "[]")
  lu.assertEquals(json.encode(json.object()), "{}")
  lu.assertEquals(json.encode(json.array({ 1 })), "[1]")
  -- tagged objects keep strict string-key rule; dense numeric keys error
  local v = dec(json.encode(json.object({ x = 1 })))
  lu.assertEquals(v.x, 1)
  lu.assertError(function() json.encode(json.object({ 1, 2 })) end)
  -- tag in place: existing tables can opt in
  local t = {}
  json.array(t)
  lu.assertEquals(json.encode(t), "[]")
end

function TestMarkers:testNullMarker()
  -- any table tagged with the null metatable encodes as JSON null
  local t = {}
  setmetatable(t, json.null)
  lu.assertEquals(json.encode(t), "null")
  -- decoded null is the marker table itself: identity compare holds
  lu.assertIsTrue(dec("null") == json.null)
end

function TestType:testKind()
  -- marker metatable first
  lu.assertEquals(json.type(json.null), "null")
  lu.assertEquals(json.type(dec("null")), "null")
  lu.assertEquals(json.type(json.array()), "array")
  lu.assertEquals(json.type(json.object()), "object")
  lu.assertEquals(json.type(dec("[]")), "array")
  lu.assertEquals(json.type(dec("{}")), "object")
  -- scalars
  lu.assertEquals(json.type(true), "boolean")
  lu.assertEquals(json.type(42), "integer")
  lu.assertEquals(json.type(3.5), "number")
  lu.assertEquals(json.type("s"), "string")
  -- table shape: dense array vs object
  lu.assertEquals(json.type({ 1, 2, 3 }), "array")
  lu.assertEquals(json.type({}), "object")
  lu.assertEquals(json.type({ a = 1 }), "object")
  lu.assertEquals(json.type({ [1] = 1, [5] = 5 }), "object")
  -- unsupported
  lu.assertIsNil(json.type(nil))
  lu.assertIsNil(json.type(function() end))
end

function TestMarkers:testMarkersNest()
  local v = dec('{"a": [], "b": {"c": 1}}')
  lu.assertEquals(json.encode(v.a), "[]")
  lu.assertEquals(json.encode(v.b), '{"c":1}')
end

TestRoundtrip = {}

function TestRoundtrip:testDeep()
  local v = {
    name = "test",
    count = 1234567890,
    ratio = 0.25,
    tags = { "a", "b" },
    meta = { ok = true, note = json.null },
    empty = {},
  }
  local d = dec(json.encode(v))
  lu.assertEquals(d.name, "test")
  lu.assertEquals(d.count, 1234567890)
  lu.assertEquals(d.ratio, 0.25)
  lu.assertEquals(d.tags[2], "b")
  lu.assertEquals(d.meta.ok, true)
  lu.assertEquals(d.meta.note, json.null)
end

function TestEncode:testHoleKeyRejected()
  -- out-of-range numeric key inside a dense prefix -> object path error
  lu.assertError(function() json.encode({ [1] = 1, [0] = 0 }) end)
  lu.assertError(function() json.encode({ [1] = 1, [5] = 5 }) end)
end

os.exit(lu.LuaUnit.run(), true)
