-- yyjson binding tests (luaunit harness).
-- run: just lua/yyjson
local dir = arg[0]:match("^(.*)[/\\]") or "."
local root = dir .. "/../.."
package.path = root .. "/?.lua;" .. dir .. "/?.lua;" .. package.path
package.cpath = (_G["jit"] and root .. "/lua/luajit/?.so;"
    or root .. "/lua/?.so;")
    .. package.cpath
    .. ";./lua/?.so;/opt/homebrew/lib/lua/5.5/?.so;/opt/homebrew/lib/lua/5.4/?.so"

local lu = require "luaunit"
local yyjson = require "yyjson"

-- decode that must succeed (assert narrows the nil case for LuaLS)
local function dec(s)
  local v, err = yyjson.decode(s)
  assert(v ~= nil, err)
  return v
end

TestDecode = {}

function TestDecode:testScalars()
  lu.assertEquals(dec("null"), yyjson.null)
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
  lu.assertEquals(v.c, yyjson.null)
end

function TestDecode:testEscapes()
  lu.assertEquals(dec('"a\\nb\\t\\\\\\""'), 'a\nb\t\\"')
  lu.assertEquals(dec('"\\u4f60\\u597d"'), "你好")
end

function TestDecode:testParseError()
  local v, err, pos = yyjson.decode("{bad")
  lu.assertNil(v)
  lu.assertEquals(err, "unexpected_character")
  lu.assertEquals(pos, 1)
  local v2, err2 = yyjson.decode("")
  lu.assertNil(v2)
  lu.assertNotNil(err2)
end

function TestDecode:testErrorCodes()
  -- one bad input per reachable yyjson_read_code (2/8/11/13 are not
  -- producible through the binding: OOM / comment flag / internal literal
  -- / binding-managed file reads)
  local cases = {
    { "", "invalid_parameter" },
    { "  ", "empty_content" },
    { "1.2.3", "unexpected_content" },
    { "[1,", "unexpected_end" },
    { "{bad", "unexpected_character" },
    { "[1,2,]", "json_structure" },
    { "01", "invalid_number" },
    { '"a\\q"', "invalid_string" },
  }
  for _, c in ipairs(cases) do
    local v, err = yyjson.decode(c[1])
    lu.assertNil(v)
    lu.assertEquals(err, c[2])
  end
end

TestEncode = {}

function TestEncode:testScalars()
  lu.assertEquals(yyjson.encode(true), "true")
  lu.assertEquals(yyjson.encode(false), "false")
  lu.assertEquals(yyjson.encode(42), "42")
  lu.assertEquals(yyjson.encode(-7), "-7")
  lu.assertEquals(yyjson.encode(3.5), "3.5")
  lu.assertEquals(yyjson.encode("hi"), '"hi"')
  lu.assertEquals(yyjson.encode(yyjson.null), "null")
end

function TestEncode:testTable()
  lu.assertEquals(yyjson.encode({}), "{}")
  lu.assertEquals(yyjson.encode({ 1, 2, 3 }), "[1,2,3]")
  -- object key order is unspecified (pairs); compare via decode
  local v = dec(yyjson.encode({ a = 1, b = "x" }))
  lu.assertEquals(v.a, 1)
  lu.assertEquals(v.b, "x")
  -- mixed keys: not a pure array -> object; numeric keys are rejected
  lu.assertError(function() yyjson.encode({ 1, 2, x = 3 }) end)
end

function TestEncode:testNested()
  local s = yyjson.encode({ a = { 1, 2 }, b = yyjson.null })
  local v = dec(s)
  lu.assertEquals(v.a[2], 2)
  lu.assertEquals(v.b, yyjson.null)
end

function TestEncode:testRejects()
  lu.assertError(function() yyjson.encode(nil) end)
  lu.assertError(function() yyjson.encode(function() end) end)
  lu.assertError(function() yyjson.encode(io.stdout) end) -- non-sentinel userdata
end

function TestEncode:testDeepNesting()
  -- depth guard trips above the max nesting level
  --- @type any
  local v = 1
  for _ = 1, 70 do v = { v } end
  lu.assertError(function() yyjson.encode(v) end)
  local deep = string.rep("[", 70) .. "1" .. string.rep("]", 70)
  lu.assertError(function() yyjson.decode(deep) end)
end

TestRoundtrip = {}

function TestRoundtrip:testDeep()
  local v = {
    name = "test",
    count = 1234567890,
    ratio = 0.25,
    tags = { "a", "b" },
    meta = { ok = true, note = yyjson.null },
    empty = {},
  }
  local d = dec(yyjson.encode(v))
  lu.assertEquals(d.name, "test")
  lu.assertEquals(d.count, 1234567890)
  lu.assertEquals(d.ratio, 0.25)
  lu.assertEquals(d.tags[2], "b")
  lu.assertEquals(d.meta.ok, true)
  lu.assertEquals(d.meta.note, yyjson.null)
end

TestFiles = {}

function TestFiles:testDumpLoad()
  local path = os.tmpname()
  lu.assertTrue(yyjson.dump(path, { x = 1, y = { 2 } }))
  local v, err = yyjson.load(path)
  assert(v ~= nil, err)
  lu.assertEquals(v.x, 1)
  lu.assertEquals(v.y[1], 2)
  os.remove(path)
end

function TestFiles:testLoadMissing()
  local v, err = yyjson.load("/nonexistent/path.json")
  lu.assertNil(v)
  lu.assertNotNil(err)
end

function TestFiles:testLoadBadJson()
  local path = os.tmpname()
  local f = assert(io.open(path, "w")); f:write("{oops"); f:close()
  local v, err = yyjson.load(path)
  lu.assertNil(v)
  lu.assertNotNil(err)
  os.remove(path)
end

function TestFiles:testDumpBadPath()
  local ok, err = yyjson.dump("/nonexistent/dir/x.json", { 1 })
  lu.assertNil(ok)
  lu.assertNotNil(err)
end

function TestFiles:testLoadBigFile()
  -- >64KB file exercises the read-buffer growth path twice
  local path = os.tmpname()
  local big = {}
  for i = 1, 4000 do big[i] = '"k' .. i .. '":' .. i end
  lu.assertTrue(yyjson.dump(path, dec("{" .. table.concat(big, ",") .. "}")))
  local v, err = yyjson.load(path)
  assert(v ~= nil, err)
  lu.assertEquals(v.k4000, 4000)
  os.remove(path)
end

function TestEncode:testHoleKeyRejected()
  -- out-of-range numeric key inside a dense prefix -> object path error
  lu.assertError(function() yyjson.encode({ [1] = 1, [0] = 0 }) end)
  lu.assertError(function() yyjson.encode({ [1] = 1, [5] = 5 }) end)
end

os.exit(lu.LuaUnit.run(), true)
