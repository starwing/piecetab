--- @meta yyjson

-- yyjson binding annotations (LuaLS)

--- @alias yyjson.value boolean | integer | number | string | table

--- Parse/IO failure name; compare with `err == "unexpected_end"` etc.
--- Never hardcode other spellings — the union below is the full set.
--- @alias yyjson.err_code
---     | "invalid_parameter" | "memory_allocation" | "empty_content"
---     | "unexpected_content" | "unexpected_end" | "unexpected_character"
---     | "json_structure" | "invalid_comment" | "invalid_number"
---     | "invalid_string" | "literal" | "file_open" | "file_read"

--- @class yyjson.Mod
local yyjson = {}

--- Decode JSON string to a Lua value. On failure returns nil.
--- Mapping: null -> yyjson.null sentinel table, objects/arrays -> tables
--- (arrays dense 1..n), ints -> integer, floats -> number.
--- @param s string
--- @return yyjson.value? v  nil on failure
--- @return yyjson.err_code? err
--- @return integer? err_pos  byte offset of the failure (0-based)
function yyjson.decode(s) end

--- Encode a Lua value to a compact JSON string.
--- Accepted: boolean, integer, number, string, tables (dense arrays with
--- keys 1..n, objects with string keys), yyjson.null. Rejected (error):
--- nil, function, userdata, thread, numeric object keys.
--- @param v any
--- @return string
function yyjson.encode(v) end

--- Load JSON from a file.
--- @param path string
--- @return yyjson.value? v  nil on failure
--- @return yyjson.err_code? err
--- @return integer? err_pos
function yyjson.load(path) end

--- Write a Lua value as pretty JSON to a file.
--- @param path string
--- @param v any
--- @return boolean true on success
--- @return string? err  "file_open" | "write_failed"
function yyjson.dump(path, v) end

--- JSON null sentinel: compare decoded nulls with `v == yyjson.null`
--- (identity against this fixed table); also accepted by encode.
--- @type table
yyjson.null = nil

return yyjson
