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

--------------------------------------------------------------------------------
---@class treesitter.Language
---tree-sitter language (from a grammar .so). Immutable, never freed.
---Indexes are 1-based (Lua convention).
local Language = {}

---Compile a query from query-source text.
---@param src string  query source
---@return treesitter.Query query  on success
---@return nil error  on failure: nil, error-string, byte-offset
function Language:query(src) end

---Symbol id for name (or name for id, id is 1-based).
---@param name string
---@return integer? symbol id
function Language:symbol(name) end

---@return string? symbol name
function Language:symbol(id) end

---@param id integer 1-based symbol id
---@return string? symbol type ("anonymous"/"named"/"auxiliary")
function Language:symbol_type(id) end

---Next state from a state/symbol pair (LR table access).
---@return integer 1-based state id
function Language:next_state(state, symbol) end

---ABI version of the grammar.
---@return integer
function Language:version() end

---@return integer
function Language:symbol_count() end

---@return integer
function Language:field_count() end

---@return integer
function Language:state_count() end

--------------------------------------------------------------------------------
---@class treesitter.Parser
---Parse text into a syntax tree. Mutable: language, logger settable.
local Parser

---Create a parser.
---@return treesitter.Parser
function Parser.new() end

---Release the parser. Idempotent.
function Parser:delete() end

---Release the parser (GC finalizer).
function Parser:__gc() end

---Bind a language. Must be set before parse.
---@param lang treesitter.Language
function Parser:language(lang) end

---Set a logger callback: `fn(message: string, type: integer)`.
---@param fn? function
function Parser:logger(fn) end

---Get/set included ranges: iterable of {start_byte,end_byte,row,col...}.
---@return function  iterator (getter)
function Parser:included_ranges() end

---Reset parser state.
function Parser:reset() end

---Parse text. `old_tree` enables incremental parse; `content` is either a
---string or a read function `fn(byte, row, col) -> string` (TSInput callback).
---@param old_tree? treesitter.Tree
---@param content string|function
---@param encoding? string "utf8"|"utf16"
---@return treesitter.Tree?
function Parser:parse(old_tree, content, encoding) end

---Debug: dump parse states as a dot graph.
function Parser:print_dot_graphs(file) end

---@return treesitter.Language?
function Parser.language() end

--------------------------------------------------------------------------------
---@class treesitter.Tree
---Syntax tree. Owns nodes; edit() translates positions for incremental parse.
local Tree

---@return treesitter.Tree
function Tree:copy() end

---Release the tree. Idempotent.
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
function Tree:edit(start_byte, old_end_byte, new_end_byte,
                   start_row, start_col, old_end_row, old_end_col,
                   new_end_row, new_end_col)
end

---Ranges changed between this tree and `old`.
---@param old treesitter.Tree
---@return table  slice of {start_byte,end_byte,start_row,start_col,end_row,end_col}
function Tree:changed_ranges(old) end

---Root node with an offset applied.
---@return treesitter.Node
function Tree:root_with_offset(offset, point) end

---Debug: dump the tree as a dot graph.
function Tree:print_dot_graph(file) end

---@return treesitter.Node
function Tree.root() end

---@return treesitter.Language
function Tree.language() end

--------------------------------------------------------------------------------
---@class treesitter.Node
---A syntax node. Lifetime is tied to its tree (held as uservalue).
---Byte offsets are 1-based. Index access: `n[i]` = child, `#n` = named count.
local Node

---@return string? node type ("translation_unit", "int", ...)
function Node.type() end

---@return integer 1-based start byte
function Node.start_byte() end

---@return integer 1-based end byte (exclusive)
function Node.end_byte() end

---@return integer 1-based row
function Node.start_row() end

---@return integer 1-based column
function Node.start_col() end

---@return integer 1-based row
function Node.end_row() end

---@return integer 1-based column
function Node.end_col() end

---Node identity: `n == other` compares tree position.
---@param other treesitter.Node
---@return boolean
function Node.__eq(other) end

---@return integer named child count
function Node.__len() end

---Child by 1-based index (or via `n[i]`).
---@param i integer 1-based
---@return treesitter.Node?
function Node:child(i) end

---Named child by 1-based index.
---@param i integer
---@return treesitter.Node?
function Node:named_child(i) end

---@param name string  field name
---@return treesitter.Node?
function Node:child_by_field_name(name) end

---@param i integer 1-based child index
---@return string? field name
function Node:field_name_for_child(i) end

---@return treesitter.Node?
function Node:next_sibling() end

---@return treesitter.Node?
function Node:prev_sibling() end

---First child starting at or after byte (1-based).
---@return treesitter.Node?
function Node:first_child_for_byte(byte) end

---First named child starting at or after byte (1-based).
---@return treesitter.Node?
function Node:first_named_child_for_byte(byte) end

---Smallest descendant covering [s, e] (1-based bytes).
---@return treesitter.Node?
function Node:descendant_for_byte_range(s, e) end

---Smallest named descendant covering [s, e] (1-based bytes).
---@return treesitter.Node?
function Node:named_descendant_for_byte_range(s, e) end

---@return treesitter.TreeCursor
function Node:cursor() end

---@return treesitter.Node?
function Node:parent() end

---Translate node position for an edit (rarely needed).
function Node:edit(...) end

---@return string S-expression dump
function Node:tostring() end

--------------------------------------------------------------------------------
---@class treesitter.TreeCursor
---Walk a subtree depth-first.
local TreeCursor

---Create a cursor (usually via `node:cursor()`).
---@param node treesitter.Node
function TreeCursor.new(node) end

---Release. Idempotent.
function TreeCursor:delete() end

---@return treesitter.Node current node
function TreeCursor.node() end

---@return string? field name, integer? field id
function TreeCursor.field() end

---@return boolean moved
function TreeCursor:goto_parent() end

---@return boolean moved
function TreeCursor:goto_next_sibling() end

---@return boolean moved
function TreeCursor:goto_prev_sibling() end

---@return boolean moved
function TreeCursor:goto_first_child() end

---@return boolean moved
function TreeCursor:goto_last_child() end

---@return boolean moved
function TreeCursor:goto_first_child_for_byte(byte) end

---@return boolean moved
function TreeCursor:goto_descendant(index) end

---@return integer
function TreeCursor.descendant_index() end

---@return integer
function TreeCursor.depth() end

--------------------------------------------------------------------------------
---@class treesitter.Query
---Compiled query patterns with captures.
local Query

---Release. Idempotent.
function Query:delete() end

---Create a cursor for this query and execute on a node.
---@param node treesitter.Node
---@return treesitter.QueryCursor
function Query:exec(node) end

---@return integer
function Query.pattern_count() end

---@return integer
function Query.capture_count() end

---@return integer
function Query.string_count() end

---Capture name by 1-based capture id.
---@param id integer 1-based
---@return string
function Query:capture_name_for_id(id) end

---@param pattern integer 1-based
---@return integer
function Query:start_byte_for_pattern(pattern) end

---Disable a capture by name (all its patterns).
---@param name string
function Query:disable_capture(name) end

---Disable a pattern by 1-based index.
---@param pattern integer
function Query:disable_pattern(pattern) end

--------------------------------------------------------------------------------
---@class treesitter.QueryCursor
---Iterate query matches/captures over a node.
---After `next_match`, `c[i]` yields the i-th capture node of the match.
local QueryCursor

---Create a cursor.
function QueryCursor.new() end

---Release. Idempotent.
function QueryCursor:delete() end

---Execute a query on a node.
---@param query treesitter.Query
---@param node treesitter.Node
function QueryCursor:exec(query, node) end

---Restrict matches to [s, e] (1-based).
---@return boolean
function QueryCursor:set_byte_range(s, e) end

---Restrict matches to a point range (1-based rows/cols).
---@return boolean
function QueryCursor:set_point_range(srow, scol, erow, ecol) end

---@return boolean next match exists; captures via `c[i]`
function QueryCursor:next_match() end

---@return integer? 1-based capture id; node via `c[id]`
function QueryCursor:next_capture() end

---Remove the current match (used for dedup).
function QueryCursor:remove_match(id) end

---@return boolean
function QueryCursor.did_exceed_match_limit() end

---@param limit integer
function QueryCursor.match_limit(limit) end

---Maximum start depth for matches.
function QueryCursor.max_start_depth(depth) end

--------------------------------------------------------------------------------
---@class treesitter.LookaheadIterator
---Iterate terminal symbols valid at a parser state.
local LookaheadIterator

---@param lang treesitter.Language
---@param state integer 1-based state id
function LookaheadIterator.new(lang, state) end

---Release. Idempotent.
function LookaheadIterator:delete() end

---@return boolean advanced; symbol via `it.symbol`
function LookaheadIterator:next() end

---@return integer 1-based symbol id
function LookaheadIterator.symbol() end

---@return string
function LookaheadIterator.symbol_name() end

--------------------------------------------------------------------------------
---@class treesitter.Slice
---Lazy-iterable range array (from changed_ranges etc.). Index 1-based.
---@field [integer] integer[]
local Slice

---@return integer count
function Slice.__len() end

---@param i integer 1-based
---@return integer start_byte
---@return integer end_byte
---@return integer start_row
---@return integer start_col
---@return integer end_row
---@return integer end_col
function Slice.__index(i) end

---Release. Idempotent.
function Slice:delete() end

---Release (GC finalizer).
function Slice:__gc() end

---@return string
function Slice:tostring() end

ts.parser = Parser
ts.tree_cursor = TreeCursor
ts.query_cursor = QueryCursor
ts.lookahead_iterator = LookaheadIterator

return ts
