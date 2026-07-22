--- @meta termkey

--------------------------------------------------------------------------------
---@class termkey.Termkey
---Terminal key input parser. Created by `termkey.new(fd, flags)`.
local Termkey

---Release the termkey instance. Idempotent.
function Termkey:delete() end

---Release the termkey (GC finalizer).
function Termkey:__gc() end

---Start terminal mode. Turn on raw/cbreak mode via tcsetattr.
---@return self?
function Termkey:start() end

---Stop terminal mode. Restore original terminal settings.
---@return self
function Termkey:stop() end

---Set canonicalisation flags.
---@param flag '"delbs"'  delete-remaps-backspace
---@return self
function Termkey:setcanonflags(flag) end

---Non-blocking: try to parse the next key from the fd.
---@param force? '"f"'  force key delivery without waiting for more bytes
---@return '"NONE"'|'"KEY"'|'"EOF"'|'"AGAIN"'|'"ERROR"'
function Termkey:getkey(force) end

---Blocking: wait for and parse the next key from the fd.
---@return '"NONE"'|'"KEY"'|'"EOF"'|'"AGAIN"'|'"ERROR"'
function Termkey:waitkey() end

---Set the wait timeout for key sequences (milliseconds).
---@param msec integer
---@return self
function Termkey:setwaittime(msec) end

---Get the wait timeout for key sequences (milliseconds).
---@return integer
function Termkey:getwaittime() end

---Advise the driver that the fd is readable; returns what getkey would return.
---@return '"NONE"'|'"KEY"'|'"EOF"'|'"AGAIN"'|'"ERROR"'
function Termkey:advisereadable() end

---Set the fd to blocking mode (POSIX only).
---@return self
function Termkey:blocking() end

---Canonicalise the last-parsed key (e.g. convert Ctrl-H to Backspace).
---@return self
function Termkey:canonicalise() end

---Get the type of the last-parsed key.
---@return '"UNICODE"'|'"FUNCTION"'|'"KEYSYM"'|'"MOUSE"'|'"POSITION"'|'"MODEREPORT"'|'"DCS"'|'"OSC"'|'"UNKNOWN_CSI"'
function Termkey:key() end

---Get the data of the last-parsed key. Returns vary by key type:
---
---**UNICODE**: `utf8_string, codepoint`
---
---**FUNCTION**: `number` (e.g. F1=1)
---
---**KEYSYM**: `keyname_string, sym_integer`
---
---**MOUSE**: `event_string, button, line, col`
---  - event: `"PRESS"`, `"DRAG"`, `"RELEASE"`
---
---**POSITION**: `line, col`
---
---**MODEREPORT**: `initial, mode, value`
---
---**UNKNOWN_CSI**: `{ intermediate = integer?, initial = integer?, cmd = integer, [1..n] = integer }`
---
---**DCS / OSC**: *(no returns)*
function Termkey:data() end

---Get modifier state of the last-parsed key.
---
---With no arg: returns a compact string of modifier letters (e.g. `"SA"`, `"C"`, `""`).
---
---With a letter arg: returns boolean for that modifier.
---@param mod? '"S"'|'"A"'|'"M"'|'"C"'|'"s"'|'"a"'|'"m"'|'"c"'  `S`=shift, `A`/`M`=alt/meta, `C`=ctrl (case-insensitive)
---@return string|boolean
function Termkey:mod(mod) end

---Parse a key name string back into the internal key state.
---@param str   string  key name (e.g. `"Up"`, `"C-Space"`, `"Enter"`)
---@param format? integer  format flags (from `formatflags`)
---@return true, integer       -- consumed-byte-count (1-based)
---@return nil                 -- parse failed
function Termkey:parse(str, format) end

---Format the last-parsed key as a human-readable string.
---@param format? integer  format flags (from `formatflags`, default 0)
---@return string
function Termkey:format(format) end

---Combine format flags and return an integer for use with `:format()` and `:parse()`.
---
---Valid flags: `"LONGMOD"`, `"CARETCTRL"`, `"ALTISMETA"`, `"WRAPBRACKET"`,
---`"SPACEMOD"`, `"LOWERMOD"`, `"LOWERSPACE"`, `"MOUSE_POS"`.
---@param ... '"LONGMOD"'|'"CARETCTRL"'|'"ALTISMETA"'|'"WRAPBRACKET"'|'"SPACEMOD"'|'"LOWERMOD"'|'"LOWERSPACE"'|'"MOUSE_POS"'
---@return integer
function Termkey:formatflags(...) end

--------------------------------------------------------------------------------
-- Module exports (return value of `require "termkey"`)

local termkey = {}

---Create a new Termkey instance.
---@param fd    integer  file descriptor (usually 0 for stdin)
---@param flags? boolean true = TERMKEY_FLAG_NOTERMIOS (skip tcsetattr)
---@return termkey.Termkey?
function termkey.new(fd, flags) end

---Create a new abstract Termkey instance (no fd, for testing).
---@param term  string   terminal name (e.g. "xterm-256color")
---@param flags? boolean true = TERMKEY_FLAG_NOTERMIOS
---@return termkey.Termkey?
function termkey.new_abstract(term, flags) end

---TERMKEY_FORMAT_VIM: ALTISMETA | WRAPBRACKET (e.g. `<Enter>`, `<C-r>`).
termkey.FORMAT_VIM = 12

---TERMKEY_FORMAT_URWID: LONGMOD | ALTISMETA | LOWERMOD | SPACEMOD | LOWERSPACE.
termkey.FORMAT_URWID = 117

return termkey
