--- @meta json

-- json binding annotations (LuaLS)

--- @alias json.value boolean | integer | number | string | table

--- JSON kind reported by json.type.
--- @alias json.kind
---     | "null" | "array" | "object" | "boolean"
---     | "integer" | "number" | "string"

--- Parse failure name; compare with `err == "unexpected_end"` etc.
--- Never hardcode other spellings — the union below is the full set.
--- @alias json.err_code
---     | "invalid_parameter" | "memory_allocation" | "empty_content"
---     | "unexpected_content" | "unexpected_end" | "unexpected_character"
---     | "json_structure" | "invalid_comment" | "invalid_number"
---     | "invalid_string" | "literal"

--- @class json.Mod
local json = {}

--- Decode JSON string to a Lua value. On failure returns nil.
--- Mapping: null -> json.null marker table, objects/arrays -> tables tagged
--- with the matching marker metatable (arrays dense 1..n), ints -> integer,
--- floats -> number.
--- @param s string
--- @return json.value? v  nil on failure
--- @return json.err_code? err
--- @return integer? err_pos  byte offset of the failure (0-based)
function json.decode(s) end

--- Encode a Lua value to a compact JSON string.
--- Accepted: boolean, integer, number, string, tables (dense arrays with
--- keys 1..n, objects with string keys), any table tagged via json.array/
--- json.object/json.null. Rejected (error): nil, function, userdata,
--- thread, numeric object keys.
--- @param v any
--- @return string
function json.encode(v) end

--- Tag a table as a JSON array (empty tables then encode as []), or
--- create a fresh tagged array table when called without arguments.
--- @param t? table
--- @return table
function json.array(t) end

--- Tag a table as a JSON object (dense numeric keys stay object keys),
--- or create a fresh tagged object table when called without arguments.
--- @param t? table
--- @return table
function json.object(t) end

--- JSON null marker: decoded nulls are this fixed table (compare with
--- `v == json.null`); also `setmetatable(t, json.null)` tags any table
--- as a JSON null for encode.
--- @type table
json.null = nil

--- JSON kind of a Lua value: marker metatable first, then table shape
--- (dense 1..#t array else object). Unsupported values return nil.
--- @param v any
--- @return json.kind?
function json.type(v) end

return json
