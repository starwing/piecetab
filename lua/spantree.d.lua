--- @meta spantree

--------------------------------------------------------------------------------
---@class spantree.Tree
---Span storage with an embedded style compositor: attr tables intern
---to stable style ids, namespace layers fold by priority, edits shift
---spans. Compositor state is global — every tree shares intern ids and
---the namespace registry. Edits invalidate previously created cursors
---and span/styled iterators.
local Tree

---Total bytes in the tree (edit-sync length, no content).
---@return integer
function Tree:bytes() end

---Set the canon field whitelist: only listed field names take part in
---intern canon (sorted once; unlisted fields are ignored). Default is
---the SGR set fg/bg/bold/dim/italic/underline/reverse.
---@param fields string[]  field names, any order
function Tree:setfields(fields) end

---Intern an attr table to a style id. Identical attrs intern to the
---same id; 0 is the pre-interned empty attr. A table with a __hash
---metamethod skips canon: its (string) return value is the intern key.
---Attr ids are stable across edits.
---@param attr table  attribute fields (fg/bg numbers, rgb tables,
---                   bold/dim/italic/underline/reverse booleans)
---@return integer  attr id
function Tree:intern(attr) end

---Inverse lookup: style id -> attr table (owned, do not mutate).
---Both attr ids and styled() ids resolve here. Nil for unknown ids.
---@param id integer  style id
---@return table?
function Tree:attr(id) end

---Namespace registry (ns = writer identity, extmark-style).
---
---1 arg: query — returns the priority and the mode ("ephemeral" for
---eph ns, nil for ordinary), or a single nil when unregistered.
---2 args, nil priority: unregister — returns the old priority.
---2 args, numeric priority: register — on an existing name updates
---the priority and returns the old one; fresh registration returns
---nil.
---3 args: flags string, chars parsed individually: "c" = strict
---register (raises when the name exists), "e" = ephemeral mode; both
---may combine ("ce"/"ec").
---@param name string  namespace name (non-empty)
---@return number?  priority (nil = unregistered)
---@return string?  "ephemeral" (query only; nil = ordinary)
---@overload fun(self: spantree.Tree, name: string, p: number, flags?: string): number?
---@overload fun(self: spantree.Tree, name: string, p: nil): number?
function Tree:namespace(name) end

---Write attr (or an interned attr id) into the ns layer over
---[off, off+len). Same ns overwrites; other layers survive by
---priority. ns nil = unaffiliated (lowest layer). Returns the attr id.
---@param ns   string?        namespace name (registered; nil = unaffiliated)
---@param attr table|integer  attr table or attr id
---@param off  integer        0-based byte offset
---@param len  integer        length
---@return integer  attr id
function Tree:mark(ns, attr, off, len) end

---Clear spans. clear(ns) = whole-tree clear of the ns layer;
---clear(ns, off, len) = range clear of the layer (ns nil = every
---layer); clear() = whole-tree full clear.
---@param ns? string?  namespace name
---@return self
---@overload fun(self: spantree.Tree): self
---@overload fun(self: spantree.Tree, ns: string?, off: integer, len: integer): self
function Tree:clear(ns) end

---Edit sync: delete del bytes at off, insert ins bytes (inherits the
---left segment id). Every eph layer resets.
---@param off integer  0-based byte offset
---@param del integer  bytes to delete
---@param ins integer  bytes to insert
---@return self
function Tree:splice(off, del, ins) end

---Insert ins bytes at off, inheriting the left segment id.
---@param off integer  0-based byte offset
---@param ins integer  bytes to insert
---@return self
function Tree:append(off, ins) end

---Insert ins bytes at off, inheriting the right segment id.
---@param off integer  0-based byte offset
---@param ins integer  bytes to insert
---@return self
function Tree:insert(off, ins) end

---Delete [off, off+len).
---@param off integer  0-based byte offset
---@param len integer  length
---@return self
function Tree:remove(off, len) end

---Mark-flow iterator over [off, off+len): yields (off, len, attr, id)
---per mark — attr = the mark's attr table, id = its attr id (both
---stable). A merged segment holding several marks yields each mark
---separately (overlapping, priority order). With ns, only that
---layer's slots; nil = any mark. Editing the tree inside the loop
---raises.
---@param ns?  string?  namespace filter (nil = any mark)
---@param off  integer  0-based start offset
---@param len  integer  range length
---@return fun(): integer, integer, table, integer  iterator (off, len, attr, id)
---@overload fun(self: spantree.Tree, off: integer, len: integer): fun(): integer, integer, table, integer
---@usage
---```lua
---for off, len, attr, id in t:span(0, #doc) do
---  -- attr: interned attr table, id: attr id
---end
---```
function Tree:span(ns, off, len) end

---Style-flow iterator over [off, off+len): yields (off, len, attr, id)
---per folded run — attr = the folded attr table, id = the synthetic
---style id (resolves via t:attr). Do not cache styled ids across
---edits (composite ids may be recycled); attr ids are stable.
---@param off integer  0-based start offset
---@param len integer  range length
---@return fun(): integer, integer, table, integer  iterator (off, len, attr, id)
function Tree:styled(off, len) end

---Delete every mark carrying the given attr id (all namespaces).
---Returns the number of cleared segments; unknown id returns 0.
---@param id integer  attr id
---@return integer  cleared segment count
function Tree:unmark(id) end

---Create a reusable cursor handle positioned at offset 0. Rebinds
---cheaply via seek; keeps its bound tree alive.
---@return spantree.Cursor
function Tree:cursor() end

---Cursor entry point: seek(off) creates a fresh cursor at off;
---seek(off, c) repositions an existing cursor (any bound tree, even a
---stale one — seek rebuilds paths). Returns the cursor.
---@param off integer  0-based byte offset
---@param c?  spantree.Cursor  cursor to reuse
---@return spantree.Cursor
function Tree:seek(off, c) end

--------------------------------------------------------------------------------
---@class spantree.Cursor
---Reusable cursor over a span tree. Tree edits invalidate it: any
---read/edit after another edit raises until seek rebinds it. Cursor
---edits self-sync (the editing cursor stays valid).
local Cursor

---Rebind to any tree at off (seek rebuilds paths; the stale-guard
---resets). Re-anchors the tree reference.
---@param t   spantree.Tree
---@param off integer  0-based byte offset
---@return self
function Cursor:seek(t, off) end

---Move the cursor to a byte offset (incremental walk).
---@param off integer  0-based byte offset
---@return self
function Cursor:locate(off) end

---Advance the cursor by a signed byte delta.
---@param d integer  signed byte delta
---@return self
function Cursor:advance(d) end

---Current byte offset.
---@return integer
function Cursor:offset() end

---Current mark at the cursor: off, len, attr table and attr id, or
---nil at a segment end / past the tree end.
---@return integer? off
---@return integer? len
---@return table?   attr
---@return integer? attr id
function Cursor:style() end

---Move onto the next matching mark and return it (same 4-tuple as
---style), or nil past the tree end. ns filters that layer; nil = any.
---@param ns? string?  namespace filter (nil = any mark)
---@return integer? off
---@return integer? len
---@return table?   attr
---@return integer? attr id
function Cursor:next(ns) end

---Move onto the previous matching mark and return it (same 4-tuple as
---style), or nil before the start. ns semantics match next().
---@param ns? string?  namespace filter (nil = any mark)
---@return integer? off
---@return integer? len
---@return table?   attr
---@return integer? attr id
function Cursor:prev(ns) end

---Write attr (or an interned attr id) into the ns layer for len bytes
---from the cursor. ns nil = unaffiliated. Returns the attr id.
---@param ns   string?        namespace name
---@param attr table|integer  attr table or attr id
---@param len  integer        length
---@return integer  attr id
function Cursor:mark(ns, attr, len) end

---Clear len bytes from the cursor. clear(len) = every layer;
---clear(ns, len) = the ns layer only (nil = every layer).
---@param ns? string?  namespace name
---@param len integer   length
---@return self
---@overload fun(self: spantree.Cursor, len: integer): self
function Cursor:clear(ns, len) end

---Delete del bytes and insert ins bytes at the cursor.
---@param del integer  bytes to delete
---@param ins integer  bytes to insert
---@return self
function Cursor:splice(del, ins) end

---Insert ins bytes at the cursor, inheriting the left segment id.
---@param ins integer  bytes to insert
---@return self
function Cursor:append(ins) end

---Insert ins bytes at the cursor, inheriting the right segment id.
---@param ins integer  bytes to insert
---@return self
function Cursor:insert(ins) end

---Delete len bytes from the cursor.
---@param len integer  length
---@return self
function Cursor:remove(len) end

--------------------------------------------------------------------------------
-- Module exports (return value of `require "spantree"`)

local spantree = {}

---Create a new span tree (compositor state is shared across trees).
---@return spantree.Tree
function spantree.new() end

return spantree
