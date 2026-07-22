--- @meta cellgrid

--------------------------------------------------------------------------------
---@class cellgrid.Grid
---Terminal cell grid with dual-buffer diff rendering.
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

---Fill codepoints in row [cs, ce). Does not change style.
---@param r  integer  row
---@param cs integer  start column
---@param ce integer  end column (exclusive)
---@param cp integer  codepoint
function Grid:fill(r, cs, ce, cp) end

---Set style in row [cs, ce). Does not change codepoints.
---@param r  integer  row
---@param cs integer  start column
---@param ce integer  end column (exclusive)
---@param st integer  style ID
function Grid:span(r, cs, ce, st) end

---Write a UTF-8 string at (r, c) with style. Handles wide chars.
---Returns the absolute column after the last character written.
---@param r  integer  row
---@param c  integer  start column
---@param s  string   UTF-8 text
---@param st? integer  style ID (default 0)
---@return integer  end column
function Grid:putline(r, c, s, st) end

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
function Grid:cols() end

---@return integer
function Grid:top() end

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
