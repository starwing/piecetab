--- @meta piecetab

--------------------------------------------------------------------------------
---@class piecetab.Buffer
---Immutable text buffer. Created by `pt.from()`, `pt.empty()`, `b:compact()`,
---or `c:commit()` / `c:rollback()`.
local Buffer

---Release the buffer explicitly. Idempotent.
function Buffer:delete() end

---Release the buffer (deterministic close for Lua 5.4+ to-be-closed).
function Buffer:__close() end

---Release the buffer (GC finalizer).
function Buffer:__gc() end

---Total bytes in the buffer.
---@return integer
function Buffer:__len() end

---Total bytes in the buffer (same as `#b`).
---@return integer
function Buffer:len() end

---Read bytes from the buffer without advancing a cursor.
---Always returns a string (clamped at buffer end).
---@param off  integer  0-based byte offset
---@param len? integer  bytes to read (default: to end)
---@return string
function Buffer:read(off, len) end

---Iterate over all pieces in the buffer.
---@return fun(): integer, integer, string
---@usage
---```lua
---for off, len, s in b:pieces() do
---  -- off: byte offset, len: byte length, s: data string
---end
---```
function Buffer:pieces() end

---Compact the buffer into a new contiguous buffer.
---@return piecetab.Buffer
function Buffer:compact() end

---Dump the full buffer content as a string.
---@return string
function Buffer:dump() end

---Create an editing cursor into this buffer.
---@param off? integer  initial cursor position (default 0)
---@return piecetab.Cursor
function Buffer:cursor(off) end

--------------------------------------------------------------------------------
---@class piecetab.Cursor
---Editing session on a buffer. Edit methods return `self` for chaining.
---After `commit()` or `rollback()`, the cursor is **invalid** and any method
---call will raise an error.
local Cursor

---Finalize edits and produce a new buffer. Cursor becomes invalid.
---@return piecetab.Buffer
function Cursor:commit() end

---Discard edits and return to the original buffer. Cursor becomes invalid.
---@return piecetab.Buffer
function Cursor:rollback() end

---Retain release (deterministic close + GC finalizer).
function Cursor:__close() end

function Cursor:__gc() end

---Current byte offset (0-based).
---@return integer
function Cursor:offset() end

---Absolute seek to `off`. Clamped to valid range.
---@param off integer  0-based byte offset
---@return self
function Cursor:locate(off) end

---Relative move by `d` bytes (may be negative). Clamped to valid range.
---@param d integer  signed delta
---@return self
function Cursor:advance(d) end

---Read `len` bytes from the current position. Does not move cursor.
---@param len integer  bytes to read (clamped at end)
---@return string
function Cursor:read(len) end

---Insert text **at** the cursor position, cursor advances to insertion end.
---Literal path (full arena copy, no length limit).
---@param s string
---@return self
function Cursor:append(s) end

---Insert text **before** the cursor, cursor does **not** move.
---Literal path (full arena copy, no length limit).
---@param s string
---@return self
function Cursor:insert(s) end

---Delete `del` bytes then insert `s`. Hole path (memcpy, fast).
---`#s` must be ≤ `pt.MAX_HOLESIZE` (64). Cursor advances to insertion end.
---@param del integer  bytes to delete
---@param s   string   text to insert
---@return self
function Cursor:edit(del, s) end

---Delete `len` bytes forward. Cursor stays at deletion point.
---@param len integer
---@return self
function Cursor:remove(len) end

---Delete `del` bytes then insert `s`. Literal path (no length limit).
---Cursor advances to insertion end.
---@param del integer  bytes to delete
---@param s   string   text to insert
---@return self
function Cursor:splice(del, s) end

--------------------------------------------------------------------------------
---@class piecetab.Doc
---Mutable document with undo history and line-cache.
---Editing methods return `self` for chaining.
---History navigation (`undo`, `redo`, `earlier`, `later`) raises an error
---when there are uncommitted (fresh) edits.
local Doc

---Retain release (deterministic close + GC finalizer).
function Doc:__close() end

function Doc:__gc() end

---Total bytes in the document.
---@return integer
function Doc:__len() end

---Seek to a position and return the new offset.
---@return integer
---@overload fun(self: piecetab.Doc): integer
---@overload fun(self: piecetab.Doc, off: integer): integer
---@overload fun(self: piecetab.Doc, whence: '"set"', off?: integer): integer
---@overload fun(self: piecetab.Doc, whence: '"cur"', delta: integer): integer
---@overload fun(self: piecetab.Doc, whence: '"end"', off?: integer): integer
---@overload fun(self: piecetab.Doc, whence: '"line"', lnum: integer): integer
function Doc:seek() end

---Read from the current position. Position advances by bytes consumed.
---EOF returns `nil` (except `"a"` / `"*a"` which always returns a string).
---@return string?                     -- no args: read one line without \n
---@overload fun(self: piecetab.Doc, n: integer): string?
---@overload fun(self: piecetab.Doc, format: '"l"'|'"*l"'): string?
---@overload fun(self: piecetab.Doc, format: '"L"'|'"*L"'): string?
---@overload fun(self: piecetab.Doc, format: '"a"'|'"*a"'): string
function Doc:read(n) end

---Insert text at cursor position. Cursor advances to insertion end.
---Alias for `append`. Journals for undo.
---@param s string
---@return self
function Doc:write(s) end

---Insert text at cursor position. Cursor advances to insertion end.
---Same as `write` (same C function, dual name).
---@param s string
---@return self
function Doc:append(s) end

---Insert text before cursor. Cursor does **not** move. Journals for undo.
---@param s string
---@return self
function Doc:insert(s) end

---Delete `del` bytes then insert `s`. Hole path. `#s` ≤ `MAX_HOLESIZE`.
---Cursor advances to insertion end. Journals for undo.
---@param del integer  bytes to delete
---@param s   string   text to insert
---@return self
function Doc:edit(del, s) end

---Delete `del` bytes then insert `s`. Literal path (no length limit).
---Cursor advances to insertion end. Journals for undo.
---@param del integer  bytes to delete
---@param s   string   text to insert
---@return self
function Doc:splice(del, s) end

---Delete `n` bytes forward. Cursor stays at deletion point. Journals for undo.
---@param n integer
---@return self
function Doc:remove(n) end

---Current byte offset (0-based).
---@return integer
function Doc:offset() end

---Column at current position (0-based byte offset within line). Syncs linecache.
---@return integer
function Doc:column() end

---Line number at current position (0-based). Syncs linecache.
---@return integer
function Doc:line() end

---Length of a line, including trailing `\n` if present.
---@param lnum? integer  line number (0-based, default: current line)
---@return integer
function Doc:linelen(lnum) end

---Total lines in the document (always `breaks + 1`). Syncs linecache.
---@return integer
function Doc:breaks() end

---Return an iterator that repeatedly calls `read(fmt, ...)` until nil.
---No args = `read()` (line without `\n`), like `file:lines()`.
---@return fun(): string?         -- no args
---@overload fun(fmt: '"l"'|'"*l"'): fun(): string?
---@overload fun(fmt: '"L"'|'"*L"'): fun(): string?
---@overload fun(n: integer): fun(): string?
---@usage
---```lua
---for text in d:lines() do       -- lines without \n
---  print(text)
---end
---for text in d:lines("*L") do   -- lines with \n
---  print(text)
---end
---for chunk in d:lines(80) do    -- 80-byte chunks
---  print(chunk)
---end
---```
function Doc:lines(fmt) end

---Commit fresh edits and create a new history version.
---@return integer  vid (version id, monotonically increasing)
function Doc:commit() end

---Undo to a previous version. Discards any uncommitted edits.
---@param vid? integer  target version id (default: parent version)
---@return integer      vid of the version after undo
function Doc:undo(vid) end

---Redo to the first child version. Errors if there are uncommitted edits.
---@return integer  vid of the version after redo
function Doc:redo() end

---Navigate to an older (chronologically previous) version.
---Errors if there are uncommitted edits.
---@return integer  vid of the version after navigation
function Doc:earlier() end

---Navigate to a younger (chronologically next) version.
---Errors if there are uncommitted edits.
---@return integer  vid of the version after navigation
function Doc:later() end

---Export a snapshot of a specific version as a new buffer.
---@param vid? integer  version id (default: current version)
---@return piecetab.Buffer
function Doc:buffer(vid) end

---Dump the full document content as a string.
---Position-independent: always reads from byte 0 to end.
---@return string
function Doc:dump() end

---Return the length of the current piece, or the next/previous piece length.
---The cursor *IS* moved if called with "next" or "prev".
--- @param opt "len"|"next"|"prev"
--- @return integer
function Doc:piece(opt) end

--------------------------------------------------------------------------------
-- Module exports (return value of `require "piecetab"`)

local piecetab = {}

---Library version string.
---@type string
piecetab.version = "0.1.0"

---Maximum length for hole-path editing. Use `splice` for longer inserts.
---@type integer
piecetab.MAX_HOLESIZE = 64

---Create a buffer from a string.
---@param s string
---@return piecetab.Buffer
function piecetab.from(s) end

---Create an empty buffer.
---@return piecetab.Buffer
function piecetab.empty() end

---Create a new document.
---@param src? string|piecetab.Buffer  initial content (nil = empty)
---@return piecetab.Doc
function piecetab.doc(src) end

return piecetab
