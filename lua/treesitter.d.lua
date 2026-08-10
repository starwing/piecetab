--- @meta treesitter

-- Module exports (return value of `require "treesitter"`)
local ts = {}

---Load a grammar from `lua/grammar/<name>.so` (symbol `tree_sitter_<name>`).
---`name` may be `"path/to/so"` with an explicit symbol name as 2nd arg.
---@param name string
---@param symbol? string  symbol name override (default `tree_sitter_<name>`)
---@return treesitter.Language
---@overload fun(): treesitter.Language
function ts.require(name, symbol) end

---@return integer grammar ABI version
ts.LANGUAGE_VERSION = 0

---@return integer oldest compatible grammar ABI
ts.MIN_COMPATIBLE_LANGUAGE_VERSION = 0

---Factory for parser objects (see treesitter.Parser).
ts.parser = nil

---Factory for tree cursors (see treesitter.TreeCursor).
ts.tree_cursor = nil

---Factory for query cursors (see treesitter.QueryCursor).
ts.query_cursor = nil

---Factory for lookahead iterators (see treesitter.LookaheadIterator).
ts.lookahead_iterator = nil

--------------------------------------------------------------------------------
---@class treesitter.Language
---tree-sitter language (from a grammar .so). Immutable, never freed.
---Indexes are 1-based (Lua convention).
---@field version integer  ABI version of the grammar
---@field symbol_count integer
---@field field_count integer
---@field state_count integer
local Language = {}

---Compile a query from query-source text.
---On failure returns `nil, error-name, byte-offset` (error-name is one of
---"syntax" / "node_type" / "field" / "capture" / "structure" / "language").
---@param src string  query source
---@return treesitter.Query query  on success
---@return nil error  on failure: nil, error-string, byte-offset
function Language:query(src) end

---Create a query cursor bound to this language's query set (equivalent to
---`treesitter.query_cursor.new()`; the query is supplied later via
---`cursor:exec(query, node)`).
---@return treesitter.QueryCursor
function Language:cursor() end

---Symbol id for name, or name for id.
---Name lookup honors an optional `is_named` flag; `id` is 1-based and must
---be in `[1, symbol_count]` (out of range raises an argument error).
---@overload fun(self: treesitter.Language, id: integer): string?
---@param name string
---@param is_named? boolean  only match named symbols (string form)
---@return integer? symbol id
function Language:symbol(name, is_named) end

---Symbol type as a string: "regular", "anonymous" or "auxiliary".
---@param id integer 1-based symbol id (or symbol name)
---@return string
function Language:symbol_type(id) end

---Field id for name, or name for id.
---@overload fun(self: treesitter.Language, id: integer): string
---@param name string
---@return integer? field id (0/nil when the field does not exist)
function Language:field(name) end

---Next state from a state/symbol pair (LR table access).
---@param state integer 1-based state id
---@param symbol integer|string  1-based symbol id, or symbol name
---@return integer 1-based state id
function Language:next_state(state, symbol) end

---Create a lookahead iterator for the given parser state.
---@param state integer 1-based state id
---@return treesitter.LookaheadIterator
function Language:lookahead_iterator(state) end


--------------------------------------------------------------------------------
---@class treesitter.Parser
---Parse text into a syntax tree. Mutable: language, logger settable.
---@field language treesitter.Language?  grammar bound for parsing (read/write)
---@field logger fun(message: string, type: string)?  parse logger callback
---  (read/write; getter returns a callable closure `f(type, message)`)
---@field included_ranges function?  read: range slice; write: iterator
---  `fn() -> start_byte, end_byte, start_row, start_col, end_row, end_col`
---  yielding 6 values per range, then `nil` to finish
local Parser = {}

---Create a parser.
---@return treesitter.Parser
function Parser.new() end

---Release the parser. Idempotent.
function Parser:delete() end

---Release the parser (GC finalizer).
function Parser:__gc() end

---Reset parser state (drop partial parse progress).
---@return treesitter.Parser self
function Parser:reset() end

---Parse text. `old_tree` enables incremental parse; `content` is either a
---string or a read function. When no language is bound, returns nil.
---@param old_tree? treesitter.Tree
---@param content string|function  source text, or `fn(byte, row, col) ->
---  string, offset?` (byte is 1-based; optional 2nd return advances the
---  read position, default 1)
---@param encoding? string "utf8" (default) | "utf16"
---@return treesitter.Tree?  nil when parsing fails
function Parser:parse(old_tree, content, encoding) end

---Debug: dump parse states as a dot graph.
---@param file file*  open file handle
---@return treesitter.Parser self
function Parser:print_dot_graphs(file) end


--------------------------------------------------------------------------------
---@class treesitter.Tree
---Syntax tree. Owns nodes; edit() translates positions for incremental parse.
---@field root treesitter.Node
---@field language treesitter.Language
---@field included_ranges treesitter.Slice  ranges applied at parse time
local Tree = {}

---Deep-copy the tree (independent root node).
---@return treesitter.Tree
function Tree:copy() end

---Release the tree. Idempotent. After this, any access raises an error.
function Tree:delete() end

---Release the tree (GC finalizer).
function Tree:__gc() end

---Translate the tree for an edit. All offsets/points are 1-based.
---@param start_byte integer
---@param old_end_byte integer
---@param new_end_byte integer
---@param start_row integer
---@param start_col integer
---@param old_end_row integer
---@param old_end_col integer
---@param new_end_row integer
---@param new_end_col integer
---@return treesitter.Tree self
function Tree:edit(start_byte, old_end_byte, new_end_byte,
                   start_row, start_col, old_end_row, old_end_col,
                   new_end_row, new_end_col)
end

---Ranges changed between this tree and `old` (for incremental re-parse).
---@param old treesitter.Tree
---@return treesitter.Slice  each element: start_byte, end_byte,
---  start_row, start_col, end_row, end_col
function Tree:changed_ranges(old) end

---Root node with an offset applied (node positions shifted).
---@param offset_byte integer
---@param start_row integer  offset point row
---@param start_col integer  offset point column
---@return treesitter.Node
function Tree:root_with_offset(offset_byte, start_row, start_col) end

---Debug: dump the tree as a dot graph.
---@param file file*  open file handle
---@return treesitter.Tree self
function Tree:print_dot_graph(file) end


--------------------------------------------------------------------------------
---@class treesitter.Node
---A syntax node. Lifetime is tied to its tree (held as uservalue).
---Byte offsets are 1-based. Index access: `n[i]` = i-th named child
---(out of range yields nil); `#n` = named child count.
---@field type string  node type ("translation_unit", "int", ...)
---@field start_byte integer  1-based start byte
---@field end_byte integer  1-based end byte (exclusive)
---@field tree treesitter.Tree  owning tree
---@field language treesitter.Language
---@field symbol integer  1-based symbol id
---@field symbol_name string
---@field grammar_type string  type after alias resolution
---@field grammar_symbol integer
---@field grammar_symbol_name string
---@field child_count integer
---@field named_child_count integer
---@field parent treesitter.Node?  nil at tree root
---@field next_sibling treesitter.Node?
---@field prev_sibling treesitter.Node?
---@field next_named_sibling treesitter.Node?
---@field prev_named_sibling treesitter.Node?
---@field descendant_count integer
---@field is_null boolean
---@field is_named boolean
---@field is_missing boolean  missing node from error recovery
---@field is_extra boolean  extra (e.g. comment) node
---@field has_changes boolean  touched by an incremental edit
---@field has_error boolean  subtree contains a syntax error
---@field is_error boolean  this node is an error node
---@field parse_state integer  1-based parse state at node start
---@field next_parse_state integer  1-based parse state after node
local Node = {}

---Node identity: `n == other` compares tree position.
---@param other treesitter.Node
---@return boolean
function Node.__eq(other) end

---@return integer named child count
function Node.__len() end

---Start position as (row, column), 0-based.
---@return integer row
---@return integer column
function Node:start_point() end

---End position as (row, column), 0-based.
---@return integer row
---@return integer column
function Node:end_point() end

---Child by 1-based index, or by field name via `n:child("field")`.
---Numeric index covers all children (named and anonymous); string lookup
---returns nil when the field is absent. Out-of-range index raises an error.
---@param i integer|string  1-based index, or field name
---@return treesitter.Node?
function Node:child(i) end

---Named child by 1-based index (same as `n[i]`).
---@param i integer 1-based
---@return treesitter.Node?
function Node:named_child(i) end

---Child by field id (see treesitter.Language:field).
---@param id integer 1-based field id
---@return treesitter.Node?
function Node:child_by_field_id(id) end

---Field name of the i-th child (nil when the child has no field).
---@param i integer 1-based child index
---@return string?
function Node:field_name_for_child(i) end

---First child starting at or after byte (1-based).
---@param byte integer
---@return treesitter.Node?
function Node:first_child_for_byte(byte) end

---First named child starting at or after byte (1-based).
---@param byte integer
---@return treesitter.Node?
function Node:first_named_child_for_byte(byte) end

---Smallest descendant covering [s, e] (1-based bytes).
---@param s integer
---@param e integer
---@return treesitter.Node?
function Node:descendant_for_byte_range(s, e) end

---Smallest descendant covering the point range
---(rows/cols 1-based; `sr, sc` start point, `er, ec` end point).
---@param sr integer
---@param sc integer
---@param er integer
---@param ec integer
---@return treesitter.Node?
function Node:descendant_for_point_range(sr, sc, er, ec) end

---Smallest named descendant covering [s, e] (1-based bytes).
---@param s integer
---@param e integer
---@return treesitter.Node?
function Node:named_descendant_for_byte_range(s, e) end

---Smallest named descendant covering the point range
---(rows/cols 1-based; `sr, sc` start point, `er, ec` end point).
---@param sr integer
---@param sc integer
---@param er integer
---@param ec integer
---@return treesitter.Node?
function Node:named_descendant_for_point_range(sr, sc, er, ec) end

---Translate node position for an edit (rarely needed; same 9 args as
---treesitter.Tree:edit, applied to this node only).
---@param ... integer  9 edit coordinates (see Tree:edit)
---@return treesitter.Node self
function Node:edit(...) end

---S-expression dump of the subtree.
---@return string
function Node:tostring() end

---Positional equality (same as `==`).
---@param other treesitter.Node
---@return boolean
function Node:equal(other) end

---Create a tree cursor rooted at this node.
---@return treesitter.TreeCursor
function Node:cursor() end


--------------------------------------------------------------------------------
---@class treesitter.TreeCursor
---Walk a subtree depth-first. Created via `node:cursor()` or
---`treesitter.tree_cursor.new(node)`.
---@field node treesitter.Node  current node
---@field field integer  1-based field id of current position (0 at root)
---@field field_name string?  field name of current position
---@field descendant_index integer  1-based order in parent
---@field depth integer  0 at the root
local TreeCursor = {}

---Create a cursor (usually via `node:cursor()`).
---@param node treesitter.Node
---@return treesitter.TreeCursor
function TreeCursor.new(node) end

---Copy the cursor (independent position).
---@return treesitter.TreeCursor
function TreeCursor:copy() end

---Release. Idempotent.
function TreeCursor:delete() end

---Move to this node (keeps the cursor's tree).
---@param node treesitter.Node
---@return treesitter.TreeCursor self
function TreeCursor:reset(node) end

---@return treesitter.TreeCursor self
function TreeCursor:goto_parent() end

---@return treesitter.TreeCursor self
function TreeCursor:goto_next_sibling() end

---@return treesitter.TreeCursor self
function TreeCursor:goto_prev_sibling() end

---@return treesitter.TreeCursor self
function TreeCursor:goto_first_child() end

---@return treesitter.TreeCursor self
function TreeCursor:goto_last_child() end

---Jump to the descendant at the given pre-order index (see
---treesitter.Node:descendant_count for the valid range).
---@param index integer 1-based
---@return treesitter.TreeCursor self
function TreeCursor:goto_descendant(index) end

---First child whose range starts at or after the byte; stays on the
---current node when no such child exists.
---@param byte integer 1-based
---@return treesitter.TreeCursor self
function TreeCursor:goto_first_child_for_byte(byte) end

---First child covering the point; stays on the current node when none.
---@param row integer 1-based
---@param col integer 1-based
---@return treesitter.TreeCursor self
function TreeCursor:goto_first_child_for_point(row, col) end


--------------------------------------------------------------------------------
---@class treesitter.Query
---Compiled query patterns with captures.
---@field pattern_count integer
---@field capture_count integer
---@field string_count integer
local Query = {}

---Release. Idempotent.
function Query:delete() end

---Execute on a node and return a ready-to-iterate query cursor.
---@param node treesitter.Node
---@return treesitter.QueryCursor
function Query:exec(node) end

---Start byte of a pattern in the query source (1-based byte).
---@param pattern integer 1-based
---@return integer
function Query:start_byte_for_pattern(pattern) end

---Predicate steps of a pattern as a slice; each element is
---`type, value_id` where type is "done" / "capture" / "string".
---@param pattern integer 1-based
---@return treesitter.Slice
function Query:predicates_for_pattern(pattern) end

---Whether the pattern matches at the root of the tree.
---@param pattern integer 1-based
---@return boolean
function Query:is_pattern_rooted(pattern) end

---Whether the pattern's match may extend across sibling boundaries.
---@param pattern integer 1-based
---@return boolean
function Query:is_pattern_non_local(pattern) end

---Whether every byte at the given offset is guaranteed to be matched
---by exactly one pattern step.
---@param byte_offset integer 1-based
---@return boolean
function Query:is_pattern_guaranteed_at_step(byte_offset) end

---Capture name by 1-based capture id.
---@param id integer 1-based
---@return string
function Query:capture_name_for_id(id) end

---Capture quantifier in a pattern: "zero" / "zero_or_one" /
---"zero_or_more" / "one" / "one_or_more".
---@param pattern integer 1-based
---@param capture integer 1-based
---@return string
function Query:capture_quantifier_for_id(pattern, capture) end

---String-literal value by 1-based id (query strings such as `"keyword"`).
---@param id integer 1-based
---@return string
function Query:string_value_for_id(id) end

---Disable a capture by name (all its patterns).
---@param name string
---@return treesitter.Query self
function Query:disable_capture(name) end

---Disable a pattern by 1-based index.
---@param pattern integer 1-based
---@return treesitter.Query self
function Query:disable_pattern(pattern) end


--------------------------------------------------------------------------------
---@class treesitter.QueryCursor
---Iterate query matches/captures over a node.
---After `next_match`, `c[i]` yields the i-th capture node of the match,
---and `#c` its capture count.
---@field did_exceed_match_limit boolean  match limit was hit
---@field match_limit integer  read/write; 0 = unlimited
---@field max_start_depth integer  write-only; restrict pattern start depth
---@field match_id integer  current match id (after next_match)
---@field pattern_index integer  1-based pattern of current match
local QueryCursor = {}

---Create a cursor (usually via `treesitter.Query:exec`).
---@return treesitter.QueryCursor
function QueryCursor.new() end

---Release. Idempotent.
function QueryCursor:delete() end

---Execute a query on a node.
---@param query treesitter.Query
---@param node treesitter.Node
---@return treesitter.QueryCursor self
function QueryCursor:exec(query, node) end

---Restrict matches to [s, e] (1-based bytes).
---@param s integer
---@param e integer
---@return treesitter.QueryCursor self
function QueryCursor:set_byte_range(s, e) end

---Restrict matches to a point range (1-based rows/cols).
---@param sr integer
---@param sc integer
---@param er integer
---@param ec integer
---@return treesitter.QueryCursor self
function QueryCursor:set_point_range(sr, sc, er, ec) end

---Advance to the next match. Returns self while a match exists, nil at
---the end; captures are then available via `c[i]`, `#c`, `c.match_id`,
---`c.pattern_index`.
---@return treesitter.QueryCursor? self
function QueryCursor:next_match() end

---Advance to the next capture within the current match. Returns the
---1-based capture id, or nil when exhausted; `c[id]` yields its node.
---@return integer?
function QueryCursor:next_capture() end

---Remove a match by id (dedup; id from `c.match_id`).
---@param id integer 1-based match id
---@return treesitter.QueryCursor self
function QueryCursor:remove_match(id) end

---Node and capture index of a capture in the current match.
---@param id integer 1-based capture id (within match)
---@return treesitter.Node
---@return integer  0-based index in the match's capture list
function QueryCursor:captures(id) end


--------------------------------------------------------------------------------
---@class treesitter.LookaheadIterator
---Iterate terminal symbols valid at a parser state.
---@field symbol integer  1-based current symbol id
---@field symbol_name string  current symbol name
---@field language treesitter.Language
local LookaheadIterator = {}

---@param lang treesitter.Language
---@param state integer 1-based state id
---@return treesitter.LookaheadIterator
function LookaheadIterator.new(lang, state) end

---Release. Idempotent.
function LookaheadIterator:delete() end

---Advance to the next valid symbol. Returns self while a symbol exists,
---nil at the end; the new symbol is read via `it.symbol`.
---@return treesitter.LookaheadIterator? self
function LookaheadIterator:next() end

---Restart iteration at a state. With 2 args the state is in the
---iterator's own language; with 3 args a language is supplied.
---@param lang? treesitter.Language  optional language (3-arg form)
---@param state integer 1-based state id
---@return boolean  false when the state is invalid
function LookaheadIterator:reset(lang, state) end


--------------------------------------------------------------------------------
---@class treesitter.Slice
---Lazy-iterable range array (from changed_ranges, predicates_for_pattern,
---included_ranges). `#r` is the element count.
---`r[i]` yields only the first field (Lua truncates index multi-returns);
---the full fields come from iterating:
---`for _, a, b, c, d, e, f in r do` (6 fields for TSRange slices; the
---leading `_` is the numeric index).
---Note: `r:delete()` is a method; other field access on the slice
---(metatable lookup) does not auto-call.
---@field [integer] integer[]
local Slice = {}

---@return integer element count
function Slice.__len() end

---Element by 1-based index. Lua truncates the C-side multi-return to a
---single value, so `r[i]` gives start_byte only. Returns nil out of range.
---@param i integer 1-based
---@return integer start_byte
function Slice.__index(i) end

---Release the slice (frees the backing range array). Idempotent.
function Slice:delete() end

---Release (GC finalizer).
function Slice:__gc() end

---Debug description (`"<kind>Slice: <ptr>[<count>]"`).
---@return string
function Slice:tostring() end

return ts
