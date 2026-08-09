--- @meta termfeed

--------------------------------------------------------------------------------
---@class termfeed.State
---Terminal key parser state (termfeed).
---
---Usage: `tf:feed(bytes)` then `tf:readkey()` until "NONE"/"AGAIN",
---or `tf:waitkey(fd, timeout)` for blocking reads. Key payload is queried
---with `key()`, `data()`, `mod()`, `format()`.
local State

---Release parser memory and restore raw mode if active. Idempotent.
function State:delete() end

---Release parser (GC finalizer).
function State:__gc() end

---Set raw mode on fd, save previous termios for cooked().
---@param fd? integer file descriptor (default 0)
---@return termfeed.State self
function State:raw(fd) end

---Restore termios saved by raw() (no-op if not in raw mode).
---@return termfeed.State self
function State:cooked() end

---Set parser flags (bitmask of termfeed.FLAG_*). Returns old flags.
---@param flag integer
---@return integer old flags
function State:setflag(flag) end

---Build the terminfo trie via callback (name -> sequence string).
---The callback is invoked once per supported terminfo name during the
---build; it is not retained. nil clears the callback without rebuilding.
---@param fn? function(name: string) -> string?
---@return termfeed.State self
function State:load(fn) end

---Feed bytes into the parser, replacing any pending chunk.
---@param s string
---@return termfeed.State self
function State:feed(s) end

---Parse one key from fed data.
---@return "KEY"|"NONE"|"AGAIN" result
function State:readkey() end

---Blocking read: poll fd, feed, parse until a key or timeout.
---@param fd? integer file descriptor (default 0)
---@param timeout? integer milliseconds, -1 = block forever (default)
---@return "KEY"|"NONE"|"AGAIN" result
function State:waitkey(fd, timeout) end

---Flush a partial sequence as a best-effort key (ESC timeout path).
---@return "KEY"|"NONE"|"AGAIN" result
function State:flush() end

---Type of the last parsed key.
---@return "NONE"|"UNICODE"|"FUNCTION"|"KEYSYM"|"MOUSE"|"POSITION"|
---        "MODEREPORT"|"KITTYREPORT"|"UNKNOWN_CSI"|"DCS"|"OSC"|"APC" typ
function State:key() end

---Payload of the last parsed key, per type:
---UNICODE: utf8 string, codepoint. FUNCTION: number. KEYSYM: name?, sym.
---MOUSE: event, button, line, col. POSITION: line, col.
---MODEREPORT: initial, mode, value. KITTYREPORT: number.
---UNKNOWN_CSI: {cmd, initial?, intermediate?, [1..n]=args}.
---DCS/OSC/APC: payload string. NONE: nothing.
---@return any
function State:data() end

---Modifier query: no arg -> "SAC" style string; letter -> boolean.
---@param mod? string "S"|"A"|"C"|"D"|"T"|"H" (lowercase accepted)
---@return string|boolean
function State:mod(mod) end

---Format the last key as a string (default: Vim-style `<Esc>` etc).
---@param fmt? integer termfeed.FORMAT_* bitmask
---@return string
function State:format(fmt) end

---Parse key notation, storing the key in this state.
---@param s string e.g. "<C-S-Up>"
---@return boolean? true
---@return integer? consumed byte count
function State:parse(s) end

---Payload of the last DCS/OSC/APC key ("" when none).
---@return string
function State:string() end

--------------------------------------------------------------------------------
-- Module exports (return value of `require "termfeed"`)

local termfeed = {}

---Create a new parser state.
---@return termfeed.State
function termfeed.new() end

---Key symbol name for a tf_Sym number (nil when invalid).
---@param sym integer
---@return string?
function termfeed.name(sym) end

---tf_Sym number for a key name (nil when unknown).
---@param name string e.g. "Up"
---@return integer?
function termfeed.sym(name) end

---@alias termfeed.Flag integer
---@alias termfeed.Fmt integer

termfeed.FLAG_KEEPC0 = 1    -- C0 (0x00-0x1F) raw, no Ctrl mapping
termfeed.FLAG_CONVERTKP = 2 -- keypad (SS3 'p'-'y') to ordinary keys
termfeed.FLAG_SPACESYMBOL = 4 -- space (0x20) as Space keysym
termfeed.FLAG_DELBS = 8     -- DEL (0x7f) as Backspace keysym

termfeed.FORMAT_LONGMOD = 1     -- Shift-A rather than S-A
termfeed.FORMAT_CARETCTRL = 2   -- ^X rather than C-X
termfeed.FORMAT_ALTISMETA = 4   -- M- rather than A-
termfeed.FORMAT_WRAPBRACKET = 8 -- <Escape> brackets special keys
termfeed.FORMAT_SPACEMOD = 16   -- M Foo rather than M-Foo
termfeed.FORMAT_LOWERMOD = 32   -- meta rather than Meta
termfeed.FORMAT_LOWERSPACE = 64 -- page down rather than PageDown
termfeed.FORMAT_VIM = 12        -- WRAPBRACKET | ALTISMETA (default)

return termfeed
