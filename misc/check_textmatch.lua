#!/usr/bin/env lua
-- Check that the textmatch match layer never mentions the state variable S.
-- The match layer is between these markers in textmatch.h:
--   /* === match layer begin === */
--   /* === match layer end === */
-- Match functions only receive tm_Match *M; source state S is invisible there.

local path = arg[1] or "textmatch.h"
local f = assert(io.open(path, "r"))
local lines = {}
for line in f:lines() do
    table.insert(lines, line)
end
f:close()

local in_match = false
local bad = {}
for i, line in ipairs(lines) do
    if line:find("%f[%a]match layer begin%f[%A]") then
        in_match = true
    elseif line:find("%f[%a]match layer end%f[%A]") then
        in_match = false
    elseif in_match and line:find("%f[%a]S%f[%A]") then
        table.insert(bad, string.format("%d: %s", i, line))
    end
end

if #bad > 0 then
    io.stderr:write("textmatch match layer must not reference S:\n")
    for _, b in ipairs(bad) do
        io.stderr:write(b, "\n")
    end
    os.exit(1)
end
