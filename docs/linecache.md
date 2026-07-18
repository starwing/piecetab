# linecache.h API Reference & Implementation Notes

**English** | [中文](linecache.zh.md)

> Single-header C89 library that maintains byte-offset→line-number mapping via a Metric B+ Tree.
> Prefix `lc_`. Tests use minimal fanout (`LC_FANOUT=4`, `LC_LEAF_FANOUT=4`) to force tree splits.

---

## 1. Data Types

### Error Codes

| Macro         | Value | Meaning                     |
| ------------- | ----- | --------------------------- |
| `LC_OK`       | 0     | Success                     |
| `LC_ERRPARAM` | -1    | Null pointer or out-of-range parameter |
| `LC_ERRMEM`   | -2    | Memory allocation failure   |

### lc_State — Memory Context

```c
typedef struct lc_State lc_State;
```

Owns two object pools (`lc_Pool`): `nodes` (``lc_Node``) and `leaves` (`lc_Leaf`). Obtains physical memory via the `lc_Alloc` callback (page granularity `LC_PAGE_SIZE=65536`). Multiple trees share the same `lc_State`.

### lc_Cache — B+ Tree

```c
typedef struct lc_Cache lc_Cache;
```

A B+ tree. `root` is embedded (not a pointer), so `lc_newcache` never fails with OOM (only the root, embedded in the cache structure, is allocated). Multiple trees are independent of each other.

### lc_Cursor — Cursor

```c
typedef struct lc_Cursor lc_Cursor;
```

A non-persistent navigator, initialized by `lc_seek`/`lc_seekline`. `paths[0]` points to a slot in the root (`&root.children[...]`), `paths[levels]` points to a leaf slot (`&parent->children[...]`). The four fields `off/lnu/loff/col` jointly encode the cursor's absolute position (byte/line number) and leaf-relative position.

`lc_Diff` is `ptrdiff_t`, used for signed offsets in move APIs.

---

## 2. Public API

### Lifecycle

```c
lc_State *lc_open(lc_Alloc *allocf, void *ud);
void      lc_close(lc_State *S);
void      lc_reset(lc_State *S);
```

- `lc_open`: Creates a state object. `allocf` is a custom allocator (realloc semantics), `ud` is a pass-through user data pointer. Returns `NULL` on failure.
- `lc_close`: Frees the state and all trees and pools within it. Accepts `NULL`.
- `lc_reset`: Clears all pools within the state (including all nodes/leaves of all trees). The state object itself is retained. Accepts `NULL`.

### Tree Lifecycle

```c
lc_Cache *lc_newcache(lc_State *S);
void      lc_delcache(lc_State *S, lc_Cache *c);
```

- `lc_newcache`: Allocates an empty tree (root embedded, `levels=0`, `child_count=0`). Returns `NULL` on failure.
- `lc_delcache`: Frees all nodes and leaves in the tree, then frees the cache structure.

**Constraint**: Nodes/leaves must not be shared across trees — each tree is independent. All trees are invalidated after `lc_reset`.

### Simple Queries

```c
size_t lc_breaks(const lc_Cache *c);
size_t lc_bytes(const lc_Cache *c);
```

- `lc_breaks`: Returns the total number of line breaks in the tree (= total number of lines).
- `lc_bytes`: Returns the total number of bytes in the tree (sum of all line lengths).

### lc_scan — Bulk Load Line Breaks

```c
int lc_scan(lc_Cache *c, lc_Scanner *sc, void *ud);
```

**Behavior**: Repeatedly calls `scanner(ud, current_byte_offset)` to obtain line length (including `\n`); a return value of 0 signals scan termination. Lines are appended left-to-right to the tail of the tree. **Multiple `lc_scan` calls are additive** — subsequent scans append after existing data.

**Return values**: `LC_OK` (success), `LC_ERRPARAM` (null pointer), `LC_ERRMEM` (OOM). On `LC_ERRMEM`, already-scanned lines remain in the tree (partial completion); the caller may retry after freeing memory.

**Constraints**: The scanner must return a line length ≥ 1 (including newline). Cursor operations may be interleaved between multiple scans on the same tree.

**Note**: The scanner may be called a few extra times (probing rounds return 0 to stop appending after an initial probe); the `pos` argument is the current absolute byte offset and can be used for consistency checks.

### Cursor Positioning

```c
int lc_seek(lc_Cursor *C, lc_Cache *c, size_t offset);
int lc_seekline(lc_Cursor *C, lc_Cache *c, size_t line);
```

- `lc_seek`: Positions the cursor at byte offset `offset`. If `offset >= lc_bytes(c)`, the cursor is placed at the tree tail with `C->col = offset - lc_bytes(c)` (virtual trailing region).
- `lc_seekline`: Positions the cursor at the beginning of line `line`. `line` may equal `lc_breaks(c)` (tail of last line, i.e. trailing position). `line > lc_breaks(c)` yields `LC_ERRPARAM`. For an empty tree (`child_count==0`), `line=0` is valid.

**Return values**: `LC_OK`, `LC_ERRPARAM`.

**Post-conditions**: `C->tree = c`, `C->paths[]` initialized pointing along the root→leaf path. `C->col` is 0 (seekline) or intra-line offset (seek).

### Cursor Movement

```c
int lc_advance(lc_Cursor *C, lc_Diff delta);
int lc_advline(lc_Cursor *C, lc_Diff delta);
```

- `lc_advance`: Moves the cursor by byte offset. `delta > 0` forward, `< 0` backward. Out-of-bounds is auto-clamped (`≤0 → offset=0`, `≥bytes → locend + col`). For an empty tree (bytes=0), delta=0 is a valid no-op and delta≠0 returns an error.
- `lc_advline`: Moves the cursor by line offset. For an empty tree (bytes=0), always returns `LC_OK` (no-op). Out-of-bounds is clamped similarly (`≤0 → line=0`, `≥breaks → tail of last line`). Moving 0 lines behaves as a re-seek to the current line (col reset to 0), a no-op.

**Return values**: `LC_OK`, `LC_ERRPARAM`.

### Cursor Queries

```c
#define lc_offset(C)     ((C)->off + (C)->loff + (C)->col)
#define lc_line(C)       ((C)->nu + (C)->lnu)
#define lc_col(C)        ((C)->col)
#define lc_lineoffset(C) ((C)->off + (C)->loff)
unsigned lc_linelen(const lc_Cursor *C);
```

- `lc_offset`: The cursor's absolute byte offset (macro, directly dereferences; caller must ensure C is non-NULL).
- `lc_line`: The cursor's line number (0-based, macro).
- `lc_linelen`: The byte length of the line at the cursor. If `C` is in the trailing region (`lnu == breaks[leaf]`), returns `C->col` (virtual line length). Returns 0 if `C==NULL`.
- `lc_col`: The cursor's column offset within the current line (0-based, macro).
- `lc_lineoffset`: The byte offset at the start of the cursor's current line (macro, = offset - col).

### Mark/Clear Line Breaks

```c
int lc_markbreak(lc_Cursor *C, unsigned len);
#define lc_clearbreaks(C, len) lc_splice((C), (len), (len))
```

- `lc_markbreak`: Inserts a line break at the cursor position. `len` is the new line length (including `\n`), must be ≥ 1.
  - If `len == lc_linelen(C) - C->col`: no-op (break is already at the end)
  - If `len > remaining line length`: first inserts `len` bytes to the end of the line, then breaks at `len` (see `lc_markbreak`'s splice fallback logic)
  - Empty tree: directly builds a single-line tree with `len`
  - After return, `C` is positioned at the start of the new line after the break (`col=0`, `lnu+=1`, `loff`/`off/nu` updated)

**Semantics**: The line break is placed at `lc_offset(C) + len`, splitting the line at that byte boundary. The cursor ends at the beginning of the line *after* the break (offset = original offset + len, col = 0). When the cursor is in the trailing virtual region (beyond tree tail), `lc_markbreak` calls `lcB_oneline` to build a single-line tree, **pinning** the virtual column as actual content — the trailing position becomes a real line.
- `lc_clearbreaks`: Deletes all line breaks within `len` bytes from the cursor position (segments are joined into one line). Equivalent to the `lc_splice(C, len, len)` macro. The cursor ends up at column position within the merged line.
  - A macro wrapper around `lc_splice`. Parameter validation is delegated to `lc_splice`.

**Return values**: Same as `lc_splice` — `LC_OK`, `LC_ERRPARAM`.

### lc_remove — Erase Interval Between Cursors

```c
int lc_remove(lc_Cursor *L, lc_Cursor *R);
```

**Behavior**: Deletes all bytes (including line breaks) between cursors L and R; content to the right shifts left. L and R must belong to the same tree and `lc_offset(L) < lc_offset(R)`.

**Parameter validation**:
- `L==NULL` or `R==NULL` or `!L->tree` or `L->tree != R->tree`: returns `LC_ERRPARAM`
- Empty interval, inverted order, or L out of bounds: returns `LC_OK`, no-op

**Return values**: `LC_OK` (success or no-op), `LC_ERRPARAM` (invalid parameters). After the operation, R is invalidated (tree structure changed); L points to the deletion point.

**Implementation**: Same-leaf calls `lcD_rmleaf`; cross-leaf calls `lcD_rmrange` (three-phase method: trim→cut→stitch). Internal `lcP_reserve` pre-allocation guarantees no OOM.

### lc_splice — Interval Delete/Insert Bytes

```c
int lc_splice(lc_Cursor *C, size_t del, unsigned ins);
```

**Parameter validation**: `C==NULL` or `C->tree==NULL` returns `LC_ERRPARAM`. `del` is auto-clamped to `bytes - offset`. Insert is skipped when `ins` is 0.

**Behavior**:
- Cursor in trailing region: `C->col += ins` returns immediately
- Delete + insert bytes: internal `lc_advance` + `lc_remove` handles deletion, then `lcD_addbytes` adds back the inserted bytes. If the tree becomes empty after deletion, the tree is reset.
- Insert bytes only: if the cursor is within a valid line, `leaf->bytes[C->lnu] += ins` lengthens the current line; `C->col += ins` moves the cursor.

**Return values**: `LC_OK` (success), `LC_ERRPARAM` (invalid parameters). The deletion path is guaranteed OOM-free by `lc_remove`'s pre-allocation.

**Note**: Deletion intervals may span arbitrary leaf boundaries. `lc_splice` internally delegates deletion to `lc_remove` — `splice(C, del, ins)` is equivalent to `remove + addbytes`. `lc_clearbreaks` is a convenience macro for `splice(len, len)`.

### lc_append — Insert Text/Lines at Midpoint

```c
int lc_append(lc_Cursor *C, unsigned e, lc_Scanner *sc, void *ud);
```

**Behavior**: If `sc == NULL`, does not go through the split/insert flow — directly appends `e` bytes to the current line at the cursor position (equivalent to `lcD_addbytes`), `C->col += e`, metrics propagated. This fast path is for pure byte appends without newlines.

If `sc != NULL`, splits the current line at the cursor position, scanner output is appended after the split point, then the right-side data is stitched back into the tree. `e` is the trailing incomplete-line bytes (added back only after stitching completes); the scanner returns 0 to signal the end.

**Two independent concepts**:
- `scanner` output: complete lines (with `\n`). Filled in line by line after the split point.
- `e`: residual bytes that do not form a complete line. Always added back to the end of the split-point line only after all scanner output + stitching + `rm` replenishment are complete. `C->col` increases by `e`.

**Flow**: cutleaf splits the tree → [append scanner lines, findroom expands] loop → stitch joins → fixsource replenishes `rm` → adds back `e`

**Return values**: `LC_OK`, `LC_ERRPARAM`, `LC_ERRMEM`. On OOM, the tree is fully restored to the pre-cutleaf state via `lcB_rollback`.

**Constraint**: When the tree has no data (`root.child_count == 0` and `levels == 0`), `lc_append` is equivalent to an initial fill — scanner behavior is identical to `lc_scan`, and `e` must still be added back at the end.

### lc_insert — Insert Text/Lines at Cursor (Cursor Stays Put)

```c
int lc_insert(lc_Cursor *C, unsigned e, lc_Scanner *sc, void *ud);
```

**Behavior**: Same flow as `lc_append` (cutleaf → append → stitch → addbytes), but after insertion completes, the cursor is restored to its original position. That is, the inserted content appears at the cursor (original content shifts right), and the cursor remains before the inserted content.

**Equivalent to**: `lc_append(C, e, sc, ud)` + `lc_advance(C, -(total bytes of new text))`.

**Return values**: Same as `lc_append` — `LC_OK`, `LC_ERRPARAM`, `LC_ERRMEM`. On OOM, the tree is fully restored to the pre-cutleaf state via `lcB_rollback`.

---

## 3. Data Structure Notes

### Metric B+ Tree

```
root (depth 0..15)
├── children[0]: Node* (clusters[0..62])
│   ├── bytes[0..62]: cumulative bytes per subtree
│   ├── breaks[0..62]: cumulative line count per subtree
│   └── children[0..62]: Node* / Leaf*
├── children[1]: Node*
│   ...
└── children[n-1]: Leaf* (leaf layer: stores raw line lengths)
    └── bytes[0..62]: line byte lengths
```

**Core design decisions**:
1. **Leaves have no `child_count`** — the effective line count is determined by the parent's `breaks[i]`. Leaves only store line byte lengths, constrained by the LC_LEAF_FANOUT upper bound.
2. **Dual metrics (bytes + breaks)** — each internal node stores two cumulative arrays, enabling O(log n) bidirectional navigation between byte offsets and line numbers.
3. **Embedded root** — `lc_Cache.root` is a value, not a pointer, saving one allocation and preventing the root from being freed independently.
4. **No parent pointers** — the cursor maintains the ancestor path via `paths[]`. All upward propagation (`lcM_up`) relies on this array.
5. **Line length ≥ 1** — every value in the tree's `bytes` array is at least 1 (the newline character itself occupies 1 byte); zero-length lines are not supported.

### Cursor Field Encoding

```
C->off    : cumulative bytes of all preceding leaves (excluding current leaf)
C->nu     : cumulative line count of all preceding leaves
C->loff   : within the current leaf, sum of bytes before line C->lnu (leaf-internal byte offset)
C->lnu    : line index within the current leaf (0-based)
C->col    : column offset within the current line

offset = off + loff + col
line   = nu + lnu
```

Design intent: `off`+`nu` anchors the leaf position, `loff`+`lnu` provides precise positioning within the leaf, `col` fine-tunes within the line. The three groups accumulate to a global offset without scanning.

### Virtual Lines (Trailing Region)

The cursor may be positioned after all leaf data (tree tail or empty tree). In this state, `C->lnu == p->breaks[li]` (end-of-leaf line count), called the **trailing region**. This region has no corresponding leaf data; `C->col` stores the virtual byte offset from the leaf end.

| Operation                 | Trailing Region Behavior                                     |
| ------------------------- | ------------------------------------------------------------ |
| `lc_linelen()`            | Returns `C->col` (virtual line length)                       |
| `lc_seek(C, c, n)`        | If `n ≥ lc_bytes(c)`, `C->col = n - lc_bytes(c)`             |
| `lc_seekline(C, c, n)`    | `n == lc_breaks(c)` positions at tail of last line           |
| `lc_splice(C, del, ins)`  | Only `C->col += ins`, tree structure unchanged               |
| `lc_markbreak(C, len)`    | Empty tree: directly builds single-line tree; non-empty tree with `lnu==breaks[i]`: appends line at leaf tail |
| `lc_append(C, e, sc, ud)` | Empty tree: equivalent to initial fill (same as `lc_scan`)   |

The virtual line design allows the cursor to operate outside the text, eliminating many "before boundary" and "after boundary" branches. `lc_seek` on an empty tree sets `C->col=n`; `lc_append`/`lc_markbreak` subsequently build tree data from that position.

### Object Pool (lc_Pool)

Each page is `LC_PAGE_SIZE` bytes, allocating objects of `sizeof(lc_Node)` or `sizeof(lc_Leaf)`. A free list (`freed`) recycles freed objects. `S->nodes` and `S->leaves` are managed independently.

`lcP_reserve(n)` guarantees at least `n` available objects in the pool (including the freelist), used for transactions — pre-allocation at the stitch entry ensures no OOM throughout the entire process.

---

## 4. Core Algorithm Notes

### 1. Onion-Layer Stitch (lcD_stitchnode)

stitchnode is the most intricate algorithm in linecache — onion-order `for (k=0; k≤levels; ++k)` where `k` is the onion layer (k=0=leaf layer, k=levels=root layer), and `kl = levels - k` is the tree level corresponding to rt[k].

**Fixed point**: each round first copies (`m = min(rtcc, FANOUT-pcc)`) to fill the current parent node, `lcM_up` updates ancestor metrics. If all are moved (`m == rtcc`) and not at root layer → skip the repair phase for this layer. Otherwise, enter the repair block executing foldnode + findroom to build a new chain for remaining data.

**kl==0 safety net**: the root layer always enters repair — regardless of whether earlier layers have been processed, at kl==0 foldnode is forced → if root has only one child, shrink the root. The loop condition `k <= lcK_levels(C)` dynamically adapts to root shrinking.

**Deferred effect of d**: d records "number of right-side child nodes awaiting repair", set at the end of the current round and consumed by backwardnode in the **next round**. This is because the new findroom chain is built on the right, and its underfill requires the current round's foldnode to finish repairing before C can be moved back to that position.

### 2. Three-Phase Interval Deletion (lcD_rmrange)

L/R dual cursors delimit the deletion interval; the operation proceeds in three phases:
1. **Find fork + trim edges**: Find the first level where `L->paths[l] != R->paths[l]`. `trimright(L)` deletes from L's leaf to the right; `trimleft(R)` deletes from R's leaf to the left.
2. **Bottom-cut + hollow out**: From leaf up to level `l+1` — delete all right-sibling subtrees of L, lazily move right-sibling subtrees of R into `rt[]`. At the fork level `l`, delete the middle subtrees between L and R.
3. **Stitch + repair**: `stitch(L, rt)` stitches R's right-side subtrees back, then `foldleaf(L)` + possibly `rebalance` to balance the tree.

`rt[]` lazy use: only writes `rt[k].child_count > 0` at levels where there is right-side data to move; stitchnode skips a layer when `rtcc == 0`.

### 3. lc_append Five-Phase Flow

1. **Parameter validation**: null pointer → `LC_ERRPARAM`; `rt[]` zeroed.
2. **Leaf split (cutleaf)**: Moves rows to the right of C into `rt[0]`; `rm = C->col` (residual bytes at the split point) is deducted from `rt[0].bytes[0]`. Saves sC snapshot.
3. **Append-rotate loop**: `append(scanner)` fills leaves → if the sibling is full, `findroom` expands and moves the right sibling into rt. OOM → rollback.
4. **Stitch**: mergeleaf (merge split-point leaf with rt[0]) → stitchnode (onion order) → foldleaf → position correction.
5. **Replenish e/rm**: fixsource uses sC to replenish `rm` into the split-point leaf. `e` is added back to the end of the current line.

### 4. foldleaf/foldnode Cursor Correction Invariant

The sign of the redistribution direction `dl`/`dn` is strictly bound to `*ls == o`/`*ns == o`:
- **C originally on left (`*ls==o`)**: right sibling has more segments → `dl<0`, data moves from right into the tail of the left leaf. C's position in the left leaf is unchanged; no correction needed.
- **C originally on right (`*ls!=o`)**: left sibling has more segments → `dl>0`, data moves from left into the front of the right leaf. C is pushed back `dl` positions in the right leaf: `C->lnu += dl`.

When `dl==0`/`dn==0`, `db==0`/balancenode returns 0 and exits early, so by the time the assert is reached, `dl≠0` and `(dl<0) != (*ls!=o)` is always true. This invariant is guaranteed by the **rounding direction** of the redistribution formula:
- `balanceleaf` — `d = l - ((l + r + 1) >> 1)`, always ceiling division
- `balancenode` — `d = l - ((l + r + (left != 0)) >> 1)`, `left` = `(*ns == o)`

When C is originally on the left, `mid` leaves one extra segment for the left side, and vice versa. This ensures the leaf/node pointed to by the cursor is never the relocation destination from the right side.

### 5. lcB_rollback — OOM Rollback

When `lc_append` encounters OOM at any phase, `lcB_rollback` fully restores the tree to the state after `lcB_cutleaf`. Steps:
1. **Descend root**: Loop `for(k=levels; k>sl; --k)` clears the extra levels added by findroom rootpush, lowering the root back to `sC`'s levels.
2. **Merge split-point leaf**: `rt[0].children[0]` is copied back into the left-half leaf `bytes[C->lnu]`.
3. **Stitch rt**: Onion-order stitches all right-sibling subtrees from `rt[]` back. Net metric delta is propagated via `lcM_up`.

Relies on stitch's transactional nature: because OOM inside stitch is prevented by the entry-point `lcP_reserve`, rollback never executes after stitch succeeds (if append/findroom has already failed, rollback is taken directly; if failure occurs after stitch... theoretically impossible).

### 6. fixsource — rm Positioning and Replenishment

- `sC` is the cursor snapshot after cutleaf, preserving split-point leaf information
- `lcK_levels(S) - sl` is the level delta from rootpush during stitch; sC's old path must be shifted down by `k` levels
- Newly added levels are filled with the full left-side path (`sp[l] = &parent(sC, l)->children[0]`); the old root layer `paths[0]` offset is restored
- `lcK_leaf(sC)->bytes[sC->lnu] += rm; lcM_up(sC, sl, rm, 0)` replenishes the metric

### 7. Full-Leaf Parent Fill Logic in lc_scan Bulk Loading

`lcB_append` returns 1 when `pcc == LC_FANOUT && li == LC_LEAF_FANOUT` (parent full and last leaf full). The `lc_scan` loop responds to this signal:
- First searches bottom-up for the first non-full level `l`
- `lcD_makechain(C, l, levels, 0)` builds a chain of empty nodes
- Continues to the next append round, filling scanner output into the new chain

`lcP_reserve` pre-allocates nodes before makechain; on OOM, safely returns `LC_ERRMEM`.

---

## 5. Configuration Macros

| Macro             | Default | Meaning                                       |
| ----------------- | ------- | --------------------------------------------- |
| `LC_FANOUT`       | 62      | Maximum children per internal node            |
| `LC_LEAF_FANOUT`  | 62      | Maximum lines per leaf                        |
| `LC_MAX_LEVEL`    | 16      | Maximum tree depth (paths array size)         |
| `LC_PAGE_SIZE`    | 65536   | Pool allocator page size                      |
| `LC_STATIC_API`   | —       | When defined, all functions are static (single-file embedding) |
| `LC_POOL_STATS`   | —       | When defined, tracks live objects in pools    |
