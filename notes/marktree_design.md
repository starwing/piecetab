# MARKTREE_DESIGN.md

## 1. Overview

A vis-style editor mark system library. Single-header C89, prefix `mk_`. Built on a B+ tree with gap encoding. Supports point marks and range marks (paired). Per-mark user data via `void *`. Stable mark handles via auto-generated `mk_Id` + hash map.

Neovim `marktree.c` is the primary reference (B-tree, B=10, ~2000 lines).

## 2. Architecture Decisions

| Decision              | Choice                                                                                   | Rationale                                                                                                                                                                                                                                                     |
| --------------------- | ---------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Tree type             | B+ tree (not B-tree)                                                                     | 97.6% of marks are in leaves at B=62. Simpler descent (only compare `bytes[]`), cache-friendly sequential leaf scan, simpler split/merge. Neovim uses B-tree (B=10), but B+ tree is better at higher fanout.                                                  |
| Position encoding     | Gap-based (distance from previous mark)                                                  | Splice only modifies one gap value → O(log n). Equivalent to Neovim's relative/delta encoding.                                                                                                                                                                |
| Per-mark data         | `void *` pointer per leaf slot                                                           | All mark tree leaves same size → one shared leaf pool. Simpler than variable-size inline data. Line cache cannot reuse this (wastes ~496B per leaf at 62 fanout), so line cache must be separate.                                                             |
| Stable handle         | Auto-generated `mk_Id` (`uint32_t`) + hash map `id → (leaf*, slot)`                      | O(1) hash lookup + O(h) parent walk = O(log n) handle→position.                                                                                                                                                                                               |
| Parent pointers       | Yes, in all nodes                                                                        | Required for handle→position (bottom-up traversal). +10 bytes per node (~1% overhead). Incompatible with line cache's `paths[]` approach (which avoids parent pointers for COW compatibility in piecetab).                                                    |
| Paired marks (ranges) | Start + end as two separate leaf entries, linked by shared pair ID                       | Supports syntax highlighting ranges, diagnostic underlines, folded regions. Gravity hardcoded: start=right, end=left → exclusive [start, end) semantics.                                                                                                      |
| Intersection          | Eager canonical decomposition (Neovim-style), upgradeable to compressed per-level tuples | Eager: O(B·h) canonical nodes per range. At B=62, h=4: ~248 nodes. Acceptable. Compressed upgrade: O(h)=4 tuples per range if needed.                                                                                                                         |
| Gravity               | Hardcoded defaults, not user-facing                                                      | Point: right gravity. Range start: right gravity. Range end: left gravity. Result: exclusive range semantics. Only needed for range endpoint behavior.                                                                                                        |
| Meta filter           | Not initially                                                                            | Can add per-type bitmask in internal nodes later. Without it, user filters by type during iteration. Multiple trees per type is also viable (N × O(log n) per edit, N typically 3–5).                                                                         |
| Line cache            | Separate library/tree type                                                               | Pure gap-only leaves (~250B), `paths[]`-based cursor, no parent pointers, no hash map, no per-mark data. Incompatible with mark tree's parent pointers and `void*` per slot. Shared: B+ tree core logic (split/merge/gap arithmetic), pool allocator pattern. |
| Fanout                | MK_FANOUT=62 (tunable)                                                                   | Profile later. Higher fanout → fewer levels → faster descent but more work per node operation.                                                                                                                                                                |

## 3. Data Structures

### mk_Node (internal node)

- **bytes[FANOUT]** (`size_t`) — byte span per child subtree
- **marks[FANOUT]** (`unsigned`) — mark count per child subtree
- **child_count** (`unsigned short`)
- **children[FANOUT]** (`mk_Node*`)
- **parent** (`mk_Node*`)
- **parent_idx** (`unsigned short`)
- **isect[MK_MAX_ISECT]** (`uint32_t`) — inline intersection set of range IDs
- **isect_count** (`uint8_t`)

### mk_Leaf

- **gaps[FANOUT]** (`unsigned`) — distance from previous mark
- **ids[FANOUT]** (`mk_Id`) — auto-generated mark identifiers
- **data[FANOUT]** (`void*`) — user data pointers
- **child_count** (`unsigned short`)
- **parent** (`mk_Node*`)
- **parent_idx** (`unsigned short`)

Estimated leaf size: 248 (gaps) + 248 (ids) + 496 (data) + 2 (child_count) + 8+2 (parent) ≈ 1004 bytes.

### mk_Tree

- **S** (`mk_State*`)
- **root** (`mk_Node`, embedded)
- **total_marks** (`size_t`)
- **total_bytes** (`size_t`)
- **levels** (`unsigned`)
- **next_id** (`mk_Id`) — auto-increment counter
- **id_map** — hash map `mk_Id → leaf+slot`
- **onremove / onremove_ud** — callback

### mk_Cursor

- **leaf** (`mk_Leaf*`)
- **slot** (`unsigned short`)
- **tree** (`mk_Tree*`)
- **off** (`size_t`) — cached absolute position
- **idx** (`size_t`) — cached ordinal

No `paths[]` array — navigation via parent pointers.

### mk_State

- **alloc_ud / allocf**
- **node_pool** — shared across all trees
- **leaf_pool** — shared, fixed leaf size

## 4. API

### Lifecycle

- `mk_open(allocf, ud) → mk_State*`
- `mk_close(mk_State*)`
- `mk_newtree(mk_State*) → mk_Tree*`
- `mk_deltree(mk_State*, mk_Tree*)`

### Point Marks

- `mk_put(tree, pos, data) → mk_Id` — insert point mark, return stable ID
- `mk_del(tree, mk_Id)` — delete by handle
- `mk_remove(tree, pos) → mk_Id` — delete first mark at pos, return its ID

### Range Marks

- `mk_putrange(tree, start, end, data) → mk_Id` — insert paired start+end
- Gravity hardcoded: start right, end left → exclusive [start, end)

### Handle Queries

- `mk_pos(tree, mk_Id) → size_t` — current position (O(log n) via parent walk)
- `mk_end(tree, mk_Id) → size_t` — range end position
- `mk_data(tree, mk_Id) → void*` — user data pointer

### Positional Queries

- `mk_count(tree) → size_t`
- `mk_bytes(tree) → size_t`
- `mk_markindex(tree, pos) → size_t` — how many marks before pos
- `mk_markpos(tree, nth) → size_t` — position of nth mark

### Cursor

- `mk_locate(cursor, tree, pos)` — position to first mark at/after pos
- `mk_locatenth(cursor, tree, n)` — position to nth mark
- `mk_next(cursor)` / `mk_prev(cursor)` — iterate via parent pointers
- `mk_curpos(cursor)` / `mk_curidx(cursor)` / `mk_curgap(cursor)` — read state
- `mk_curdata(cursor) → void*` — current mark's user data

### Overlap

- `mk_overlap(cursor, tree, pos)` — find ranges containing pos
- `mk_ovnext(cursor)` — next overlapping range

### Edit Notification

- `mk_splice(tree, pos, del, ins)` — atomic: delete marks in [pos, pos+del), shift rest by (ins−del)
- `mk_clear(tree, from, to)` — delete marks in [from, to), no shift

### Bulk

- `mk_build(state, positions, count) → mk_Tree*`
- `mk_extend(tree, positions, count)`

## 5. Intersection Algorithm

### Canonical Decomposition (Eager)

When a range [A, B) is inserted, find all internal nodes whose subtree key-space is fully within [A, B). These are "canonical nodes." Add the range's pair_id to each canonical node's inline `isect[]` array.

At B=62, h=4: up to ~248 canonical nodes per range. Each node update is O(isect_count) for sorted insert/removal.

### Stab Query

Descend from root to the query position. At each level, collect `isect[]` entries — these ranges fully span the node and thus contain the query position. At the leaf level, also check for local range starts whose ends are after the query position. Total: O(h + k).

### Compressed Upgrade Path

Replace per-canonical-node marking with one (range_id, child_lo, child_hi) tuple per level. Reduces insert/delete from O(B·h) to O(h). Stab becomes O(h·T + k) where T = max tuples per node.

## 6. Complexity Summary

| Operation         | Complexity                            |
| ----------------- | ------------------------------------- |
| mk_put            | O(log n)                              |
| mk_del            | O(log n)                              |
| mk_putrange       | O(log n + B·h) for intersection setup |
| mk_pos            | O(log n) via parent walk              |
| mk_splice         | O(log n + k_del + k_boundary · B·h)   |
| mk_markindex      | O(log n)                              |
| mk_markpos        | O(log n)                              |
| mk_locate         | O(log n)                              |
| mk_next/prev      | O(1) amortized, O(log n) worst        |
| mk_overlap (stab) | O(log n + k)                          |

## 7. Memory

Per leaf: ~1004 bytes. Per internal node: ~1.5–2KB.

For 100K marks: ~1600 leaves + ~26 internal nodes ≈ 1.6MB + ~50KB ≈ 1.7MB. Hash map overhead: ~100K entries × ~16 bytes ≈ 1.6MB. Total for 100K marks: ~3.3MB.

## 8. Relationship to Line Cache

Line cache and mark tree share B+ tree algorithmic DNA but differ structurally:

| Aspect         | Line Cache                | Mark Tree                      |
| -------------- | ------------------------- | ------------------------------ |
| Leaf content   | gaps[] only (~250B)       | gaps[] + ids[] + data[] (~1KB) |
| Navigation     | paths[] (cursor-based)    | parent pointers                |
| Per-mark data  | None                      | void*                          |
| Stable handles | None (ordinal-based)      | mk_Id + hash map               |
| Paired marks   | No                        | Yes                            |
| Intersection   | No                        | Yes                            |
| Nodes shared   | No parent pointers needed | Parent pointers required       |

Shared code opportunities: pool allocator, split/merge algorithm templates, gap arithmetic utilities.

## 9. References

- Neovim `src/nvim/marktree.c` (~2000 lines): B-tree, B=10, eager intersection, paired marks, gravity, meta filter, hash map id2node
- Neovim API: `nvim_buf_set_extmark`, `right_gravity`, `end_right_gravity`
- piecetab.h: `pt_Pool`, `pt_Node`, `ptI_splitinsert`, `ptI_splitroot`, `ptC_findpieces`