--- @meta cellgrid

--------------------------------------------------------------------------------
---@class cellgrid.Grid
---Terminal cell grid with dual-buffer diff rendering.
---
---Coordinate convention: grid rows/cols are 0-based. The string-slice
---arguments to `cols`/`byte`/`next`/`putslice` use Lua's 1-based
---`string.sub` view, so converting between a 0-based byte offset from
---piecetab/spantree and a 1-based slice offset is the caller's glue.
local Grid

---Release grid memory. Idempotent.
function Grid:delete() end

---Release grid (GC finalizer).
function Grid:__gc() end

---Begin a new frame.
---@param top  integer  scroll top line (0-based)
---@param rows integer  visible rows
---@param cols integer  visible columns
function Grid:begin(top, rows, cols) end

---Clear current frame, mark all cells dirty.
function Grid:clear() end

---Freeze: snapshot current frame as back buffer for next diff.
function Grid:freeze() end

---Write a codepoint with style at (r, c). 0-based coords.
---@param r  integer  row
---@param c  integer  column
---@param cp integer  Unicode codepoint
---@param st? integer  style ID (default 0)
function Grid:put(r, c, cp, st) end

---Clear cells in row [cs, ce). Sets cp=0, st=0.
---@param r  integer  row
---@param cs integer  start column
---@param ce integer  end column (exclusive)
function Grid:clearrow(r, cs, ce) end

---Fill codepoints in row [cs, ce). Optional st combines with a
---span (fill + style in one call).
---@param r  integer  row
---@param cs integer  start column
---@param ce integer  end column (exclusive)
---@param cp integer  codepoint
---@param st? integer  style id (when given, also spans the range)
function Grid:fill(r, cs, ce, cp, st) end

---Set style in row [cs, ce). Does not change codepoints.
---@param r  integer  row
---@param cs integer  start column
---@param ce integer  end column (exclusive)
---@param st integer  style ID
function Grid:span(r, cs, ce, st) end

---Generate CSI string from diff of current vs frozen frame.
---@param tbl? cellgrid.DiffTable  style/format overrides (optional)
---@return string  CSI escape sequence
function Grid:diff(tbl) end

---Render diff to file descriptor (calls write() directly).
---@param fd  integer            file descriptor
---@param tbl? cellgrid.DiffTable  style/format overrides (optional)
function Grid:render(fd, tbl) end

---Get current cell: returns codepoint, style. 0-based coords.
---@param r integer  row
---@param c integer  column
---@return integer cp   codepoint (0x20 for space)
---@return integer st   style
function Grid:cell(r, c) end

---Get back-buffer cell: returns codepoint, style.
---@param r integer  row
---@param c integer  column
---@return integer cp
---@return integer st
function Grid:back(r, c) end

---Check if cell is dirty (cur != back).
---@param r integer  row
---@param c integer  column
---@return boolean
function Grid:isdirty(r, c) end

---@return integer
function Grid:rows() end

---@return integer
function Grid:ncols() end

---@return integer
function Grid:top() end

---Set tab expansion width used by cols/byte/next (and putslice's
---tab handling), or get current tabstop if ts is nil.
---@overload fun(self: cellgrid.Grid, ts: integer): integer
---@return integer
function Grid:tabstop() end

---Display width of s[i..j] rendered from starting column c (tab
---expanded via grid tabstop, wcwidth from unidata). Two call forms,
---split on the first argument: `cols(s, i?, j?)` (c = 0) or
---`cols(c, s, i?, j?)`. A prefix width is `cols(s, 1, off)`; an empty
---range yields 0. i/j accept negative indices (string.sub style,
---counted from the end).
---@param s string  UTF-8 text
---@param i? integer 1-based start byte (default 1)
---@param j? integer 1-based end byte, inclusive (default #text)
---@return integer  display column
---@overload fun(self: cellgrid.Grid, c: integer, s: string, i?: integer, j?: integer): integer
function Grid:cols(s, i, j) end

---Byte position (1-based, string.sub view — offsets are relative to
---the sub `s[i..j]`, not the full text) of the character whose display
---range contains column c+col: scans chars within the slice, clamps
---back to the char start when the column lands inside a tab or wide
---char; col < 0 and empty ranges clamp to the slice start (1). Two
---call forms, split on the SECOND argument:
---`byte(col, s, i?, j?)` (c = 0) or `byte(c, col, s, i?, j?)`.
---@param s   string  UTF-8 text
---@param col integer target display column (relative to c)
---@param i?  integer 1-based start byte (default 1)
---@param j?  integer 1-based end byte, inclusive (default #s)
---@return integer?  1-based byte position within the slice
---@overload fun(self: cellgrid.Grid, c: integer, col: integer, s: string, i?: integer, j?: integer): integer
function Grid:byte(col, s, i, j) end

---Cluster iterator: for byte, col in g:next(s) — yields each
---character's 1-based byte offset and starting display column within
---s[i..j] (tab expanded; continuation bytes yield width 0). Two call
---forms, split on the first argument: `next(s, i?, j?)` (c = 0) or
---`next(c, s, i?, j?)`.
---@param s string  UTF-8 text
---@param i? integer 1-based start byte (default 1)
---@param j? integer 1-based end byte, inclusive (default #s)
---@return fun(): integer?, integer?  iterator
---@overload fun(self: cellgrid.Grid, c: integer, s: string, i?: integer, j?: integer): fun(): integer?, integer?
function Grid:next(s, i, j) end

---Write a span of s[i..j] at (r, c) with style. Tabs expand to
---spaces (grid tabstop, render-column base); wide chars handled.
---i/j follow string.sub semantics: 1-based, inclusive.
---@param r  integer  row
---@param c  integer  start column
---@param st integer  style id
---@param s  string   UTF-8 text
---@param i? integer  start byte offset (default 1)
---@param j? integer  end byte offset, inclusive (default #s)
---@return integer  end column
function Grid:putslice(r, c, st, s, i, j) end

--------------------------------------------------------------------------------
---@class cellgrid.DiffTable
---Style and CSI format table passed to `Grid:diff()` or `Grid:render()`.
---
---All string keys are optional (defaults provided).
---
---Example:
---```
---local tbl = {
---    -- Numeric keys: style ID -> SGR string
---    [0] = "\x1b[0m",              -- RESET (st==0)
---    [1] = "\x1b[2m",              -- DIM
---    [3] = "\x1b[48;5;237m",      -- gray bg
---
---    -- fill_min: repeat threshold (default 4)
---    fill_min = 4,
---
---    -- CSI format strings (all optional)
---    cursor_address        = "\x1b[%d;%dH",   -- CSI CUP
---    change_scroll_region   = "\x1b[%d;%dr",   -- set scroll region
---    parm_index            = "\x1b[%dS",       -- scroll up
---    parm_rindex           = "\x1b[%dT",       -- scroll down
---    repeat_char           = "\x1b[%db",       -- REP
---}
---```
local DiffTable

--------------------------------------------------------------------------------
-- Module exports (return value of `require "cellgrid"`)

local cellgrid = {}

---Create a new Grid instance.
---@return cellgrid.Grid
function cellgrid.new() end

---Get terminal window size via ioctl(TIOCGWINSZ).
---@param fd? integer  file descriptor (default 0 = stdin)
---@return integer? rows
---@return integer? cols
function cellgrid.winsize(fd) end

return cellgrid
