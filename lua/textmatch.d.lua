--- @meta textmatch

--------------------------------------------------------------------------------
---@class textmatch
---Lua-pattern matching over a Lua string or a `piecetab.Buffer`.
---The compatibility functions use Lua 1-based indices.
local M

---@alias textmatch.err "malformed pattern"|"pattern too complex"|"textmatch: error"|"textmatch: substring read failed"|"textmatch: invalid capture"

---Create a stateful matcher bound to a source.
---@param src string|piecetab.Buffer
---@return textmatch.State
function M.new(src) end

---Find a pattern. Returns `nil` when not found; on error returns
---`nil, errmsg`. Otherwise returns the 1-based inclusive start/end
---positions, followed by captures when the pattern has captures.
---@param src     string|piecetab.Buffer  text source
---@param pattern string                  Lua pattern
---@param init?   integer                 1-based start position (default 1)
---@param plain?  boolean                 literal search (default false)
---@return integer? start
---@return integer? finish
---@return string|integer? ...  captures (position captures are numbers)
---@return textmatch.err? errmsg
function M.find(src, pattern, init, plain) end

---Match a pattern. Returns `nil` when not found; on error returns
---`nil, errmsg`. Otherwise returns the whole match when the pattern has
---no captures, or the captures when it does.
---@param src     string|piecetab.Buffer  text source
---@param pattern string                  Lua pattern
---@param init?   integer                 1-based start position (default 1)
---@return string|integer|nil ...
---@return textmatch.err? errmsg
function M.match(src, pattern, init) end

---Return an iterator over all matches. Each iteration yields the whole
---match when the pattern has no captures, or the captures when it does.
---Errors during iteration are raised; wrap iterator calls in `pcall` if
---needed. Like Lua `string.gmatch`, a leading `^` is treated as a literal
---character rather than an anchor.
---
---Returns four values for use in a `for-in` loop: the iterator function,
---`nil`, `nil`, and the closeable iterator State. The fourth value can be
---used as a `<close>` variable to guarantee cleanup.
---@param src     string|piecetab.Buffer  text source
---@param pattern string                  Lua pattern
---@return fun(): string|integer|nil ...
---@return nil
---@return nil
---@return textmatch.State
function M.gmatch(src, pattern) end

--------------------------------------------------------------------------------
---@class textmatch.State
---Stateful 0-based matcher. Positions are 0-based byte offsets and match
---intervals are `[start, endpos)`.
local State

---Rebind to a new source and reset cursor, captures, and options.
---@param src string|piecetab.Buffer
---@return textmatch.State
function State:reset(src) end

---Release retained source/pattern references and mark the State deleted.
---After deletion the State is invalid: `find`/`match`/`gfind` raise
---`invalid State`; only `reset` can revive it.
---@return nil
function State:delete() end

---Get or set an option. With a value, sets the option and returns `self`;
---without a value (or with `nil`), returns the current boolean.
---@param name  "plain"|"lineanchor"
---@param value? boolean
---@return textmatch.State
---@overload fun(self: textmatch.State, name: "plain"|"lineanchor"): boolean
function State:option(name, value) end

---Search from `off` up to exclusive `endoff` (`nil` or negative means
---unlimited). Returns `nil` when not found, or `nil, errmsg` on error.
---Otherwise returns `pos, endpos, capturecount`.
---@param pattern string
---@param off     integer  0-based start offset
---@param endoff? integer  0-based exclusive end; nil/negative = unlimited
---@return integer? pos
---@return integer? endpos
---@return integer? capturecount
---@return textmatch.err? errmsg
function State:find(pattern, off, endoff) end

---Match at the exact 0-based `off`. Returns `nil` when not found, or
---`nil, errmsg` on error. Otherwise returns `pos, endpos, capturecount`.
---@param pattern string
---@param off     integer  0-based offset
---@return integer? pos
---@return integer? endpos
---@return integer? capturecount
---@return textmatch.err? errmsg
function State:match(pattern, off) end

---Return an iterator over matches in `[off, endoff)`. Each iteration
---yields `pos, endpos, capturecount`. Errors during iteration are raised.
---
---Returns four values for use in a `for-in` loop: the iterator function,
---`nil`, `nil`, and the closeable iterator State. The fourth value can be
---used as a `<close>` variable to guarantee cleanup.
---@param pattern string
---@param off     integer  0-based start offset
---@param endoff? integer  0-based exclusive end; nil/negative = unlimited
---@return fun(): integer, integer, integer
---@return nil
---@return nil
---@return textmatch.State
function State:gfind(pattern, off, endoff) end

---Get capture information. With no argument (or `nil`) returns the capture
---count. With `n` returns the 0-based `pos, endpos` of that capture, or `nil`
---when out of range.
---@param n? integer  0-based capture index
---@return integer? count
---@return integer? pos
---@return integer? endpos
function State:capture(n) end

return M
