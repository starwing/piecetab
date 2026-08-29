# piecetab.h API Reference

**English** | [中文](piecetab.zh.md)

> Single-header C89 library that maintains a pure byte-level piece table via a B+ tree,
> supporting COW snapshots (Buffer), mutable transient state (hole leaves), and
> transaction commit/rollback. Prefix `pt_`.
> Line/character mapping is fully externalized to `linecache.h` (prefix `lc_`);
> piecetab knows nothing about lines or encoding — clean octet.

---

## 1. Data Types

### Error Codes

| Macro         | Value | Meaning                                |
| ------------- | ----- | -------------------------------------- |
| `PT_OK`       | 0     | Success                                |
| `PT_ERRPARAM` | -1    | Null pointer or out-of-range parameter |
| `PT_ERRMEM`   | -2    | Memory allocation failure              |

### pt_State — Memory Context

```c
typedef struct pt_State pt_State;
```

Opaque memory context. It owns the allocator callback, the object pools used by
all trees, and the global COW version counter. All buffers and cursors are tied
to the `pt_State` that created them; nodes/trees must not be shared across states.

### pt_Buffer — COW Snapshot

```c
typedef const struct pt_Tree *pt_Buffer;
```

`pt_Buffer` is an **immutable view** of the buffer state, backed by a
reference-counted B+ tree. All functions returning `pt_Buffer` return an
**owned reference**; the caller must call `pt_release` when it is no longer
needed.

**Sentinel**: `pt_empty()` returns the state-embedded empty tree, whose
reference count is 1 but is **never decremented** — `pt_release` returns 0
immediately for it. Buffers returned by `pt_empty` therefore need no release.

### pt_Cursor — Cursor

```c
struct pt_Cursor {
    struct pt_Node **paths[PT_MAX_LEVEL]; /* root→leaf path slot pointers */
    struct pt_Tree  *tree;                /* current buffer (internal new tree when transient) */
    size_t           poff;                /* offset within current piece */
    size_t           off;                 /* cumulative bytes before current piece */
    int              dirty;               /* transient (editing) flag */
};
```

- `paths[]` stores root-to-leaf slot pointers; `paths[levels]` points into the
  leaf container that directly holds piece pointers.
- Absolute offset = `pt_offset(C)` = `off + poff`.
- **The cursor owns no buffer reference** — it only borrows the buffer passed to
  `pt_seek`/`pt_locate`. The caller must keep that buffer alive while the cursor
  is in use.
- Constructed by `pt_seek` (clears `dirty`); `pt_locate`/`pt_advance` preserve
  `dirty`.

### pt_Delta

```c
typedef ptrdiff_t pt_Delta;
```

Signed byte offset for move-type APIs.

### pt_Alloc — Allocator

```c
typedef void *pt_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
```

Custom allocator with realloc semantics. `ptr=NULL, osize=0` allocates a new
block; `nsize=0` frees `ptr`. The default allocator wraps `realloc` and aborts
on allocation failure. Pass `NULL` to `pt_open` to select it.

---

## 2. Configuration Macros

| Symbol            | Default | Meaning                                   |
| ----------------- | ------- | ----------------------------------------- |
| `PT_FANOUT`       | 31      | Maximum children per node (≤64)           |
| `PT_MAX_HOLESIZE` | 64      | Hole capacity (bytes)                     |
| `PT_MAX_LEVEL`    | 17      | Maximum tree depth / cursor path size (see [max_levels.md](max_levels.md)) |
| `PT_PAGE_SIZE`    | 65536   | Pool allocator page size                  |
| `PT_ARENA_SIZE`   | 1024    | Arena block minimum capacity              |
| `PT_COMPACT_RANGES` | 64    | Compact range array initial capacity      |
| `PT_STATIC_API`   | —       | When defined, all functions become static |

Half-full threshold = `FANOUT/2`. `PT_FANOUT >= 4` has a static assertion
(makeroom needs at most 2 empty slots).

---

## 3. Public API

### 3.1 Lifecycle

```c
pt_State *pt_open(pt_Alloc *allocf, void *ud);
void      pt_reset(pt_State *S);
void      pt_close(pt_State *S);
pt_Alloc *pt_getallocf(pt_State *S, void **pud);
```

- **`pt_open`**: Creates a state object. `allocf` is a custom allocator; pass
  `NULL` for the default `realloc` wrapper. Initializes the object pools and the
  empty sentinel. Returns `NULL` on failure (OOM).
- **`pt_reset`**: Frees all pools within the state (including all allocated
  nodes, holes, and trees) and all remaining arena blocks. The state object
  itself is retained. All buffers and cursors depending on this state are
  invalidated. Accepts `NULL`.
- **`pt_close`**: `pt_reset` + frees the `pt_State` structure. Accepts `NULL`.
- **`pt_getallocf`**: Returns the allocator function and user data associated
  with the state (output via `pud`, which may be `NULL`).

**Constraint**: Nodes/holes/trees must not be shared across states. All buffers
and cursors are invalidated after `pt_reset`.

### 3.2 Buffer Reference Counting

```c
unsigned pt_retain(pt_Buffer b);
unsigned pt_release(pt_Buffer b);
```

- **`pt_retain`**: Increments the reference count, returns the new count.
  `b==NULL` returns 0.
- **`pt_release`**: Decrements the reference count. When it reaches zero,
  recursively frees the tree's private nodes/holes/arenas, then follows the COW
  source chain to release nodes private to prior versions, terminating at the
  state's empty sentinel. `b==NULL` or `b==&S->empty` returns 0.

**COW source chain**: each tree records the tree it was forked from. During
release, this chain ensures shared nodes survive: only nodes whose version
belongs to the tree being released are freed.

### 3.3 Buffer Construction & Queries

```c
pt_Buffer   pt_empty(pt_State *S);
pt_Buffer   pt_from(pt_State *S, const char *s, size_t len);
pt_Buffer   pt_compact(pt_State *S, pt_Buffer b);
unsigned  pt_version(pt_Buffer b);
size_t    pt_bytes(pt_Buffer b);
```

- **`pt_empty`**: Returns the state-embedded sentinel (zero allocation, zero
  bytes, no pieces). No `pt_release` needed. `S==NULL` returns `NULL`.
- **`pt_from`**: Constructs a single-piece buffer from external memory. **Does
  not copy** — only records pointer and length; the caller must ensure `s`
  remains valid for the buffer's lifetime. `len==0` returns an empty tree
  (`bytes=0`). `S==NULL` or `s==NULL && len>0` returns `NULL`. The returned
  buffer has one reference.
- **`pt_compact`**: Produces a **fresh standalone buffer** holding only the
  content currently reachable from `b`, with the COW history chain cut.
  - Internal literals (bytes living in any arena along `b`'s COW source chain)
    are copied into the new buffer's own compact arena; adjacent internal pieces
    become physically contiguous and merge into single pieces.
  - External literals (user memory from `pt_from`/`pt_insert`, e.g. a large
    mmap) keep their original pointers — never copied.
  - Does **not** release `b`; the caller releases the old chain afterwards to
    reclaim all of its memory (`pt_Buffer nb = pt_compact(S, b); pt_release(b);`).
  - `b->bytes==0` (including the sentinel) returns `pt_empty(S)`; on OOM returns
    `NULL` with `b` untouched; `S==NULL`, `b==NULL`, or a buffer from another
    state returns `NULL`.
  - Cost is O(fragments), independent of the original file size — removed
    content is not in the tree and is never visited.
- **`pt_version`**: Returns the buffer's version number. `b==NULL` returns 0.
  The version is a per-state counter value assigned at creation time.
- **`pt_bytes`**: Returns total byte count of the buffer. `b==NULL` returns 0.
  O(1).

### 3.4 Cursor Query Macros

```c
#define pt_offset(C) ((C)->off + (C)->poff)
#define pt_buffer(C)   ((C)->tree)
```

- **`pt_offset`**: The cursor's current absolute byte offset. Direct
  dereference; caller must ensure `C` is non-NULL.
- **`pt_buffer`**: The tree pointer currently associated with the cursor. In
  transient state this is the internal new tree; otherwise the buffer passed to
  `pt_seek`.

### 3.5 Cursor Positioning & Movement

```c
int pt_seek(pt_Cursor *C, pt_Buffer b, size_t off);
int pt_locate(pt_Cursor *C, size_t off);
int pt_advance(pt_Cursor *C, pt_Delta d);
```

- **`pt_seek`** — Cursor constructor: binds a buffer and clears dirty. After
  resetting the cursor, if `off >= b->bytes` positions at tree tail, otherwise
  descends top-down to the target leaf. Returns `PT_OK` or `PT_ERRPARAM`.
- **`pt_locate`** — Relocates the cursor within the bound buffer. Resets
  `C->off`/`C->poff` before locating. **Preserves dirty state** (reposition
  during editing). Returns `PT_OK` or `PT_ERRPARAM`.
- **`pt_advance`** — Moves the cursor by byte offset. `d>0` forward, `d<0`
  backward.
  - Out-of-bounds auto-clamps: `<0` clamps to offset=0, `>bytes` clamps to
    offset=bytes.
  - `d==0` or empty tree (`bytes==0`) returns `PT_OK` immediately.
  - **Preserves dirty state**.
  - Returns `PT_OK` or `PT_ERRPARAM`.

### 3.6 Piece Traversal & Reading

```c
const char *pt_piece(pt_Cursor *C, size_t *plen);
const char *pt_next(pt_Cursor *C, size_t *plen);
const char *pt_prev(pt_Cursor *C, size_t *plen);
size_t      pt_read(pt_Cursor *C, char *buf, size_t len);
```

**Semantics: "Move then return the landing point"** — `pt_next`/`pt_prev` move
the cursor first, then return the target piece's data pointer.

- **`pt_piece`**: Returns the **remaining** data of the current piece from
  `C->poff` onward; out parameter `plen` is set to the remaining length. When
  `C->poff >= piece->bytes[i]` (cursor past piece end), the tree has zero
  logical bytes (e.g. after deleting all content), or no tree exists,
  returns `NULL` with `*plen = 0`. Typical traversal idiom:
  `for (p = pt_piece(c, &n); n; p = pt_next(c, &n))`.
- **`pt_next`**: If the cursor is inside the current piece (`poff < bytes[i]`),
  consumes the remaining bytes, moves right to the start of the next piece, and
  returns the new piece's full data pointer. If already at piece end
  (`poff == bytes[i]`), jumps directly to the next piece. When no next piece
  exists (tree tail), returns `NULL` with `*plen=0`, cursor stays at tree tail.
- **`pt_prev`**: If the cursor is inside the current piece (`poff > 0`), moves
  left to the piece start and returns the full data pointer. If already at piece
  start (`poff == 0`) and not at tree head, moves left to the previous piece
  start and returns. At tree head (`off==0 && poff==0`), returns `NULL` with
  `*plen=0`.
- **`pt_read`**: Copies `len` bytes from the cursor position piece-by-piece into
  `buf`, advancing the cursor as it goes. Automatically crosses piece
  boundaries. Returns actual bytes copied when fewer than `len` are available.
  `C==NULL` or `buf==NULL` returns 0. `pt_read` internally loops over
  `pt_piece`/`pt_next`, **moving the cursor**.

### 3.7 Editing — Hole Semantics (copy)

```c
int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len);
```

Deletes `del` bytes at the cursor then inserts `s` (`len` bytes), equivalent to
`pt_remove + pt_append`, but the inserted data goes through a **hole piece**
(internally allocated fixed-capacity buffer, copied), giving **copy semantics**.

- `len` **must** be `≤ PT_MAX_HOLESIZE`, else returns `PT_ERRPARAM`.
- Calls `pt_remove(C, del)` before insertion (transactional, see §4).
- Insertion tries to merge with an adjacent trailing hole first: if the current
  or left-side piece is a hole with sufficient capacity, it appends locally
  without splitting the leaf — fast path.
- Otherwise it splits/inserts a new hole piece in the B+ tree.
- `del==0 && len==0` is a valid no-op.
- `s==NULL && len>0` returns `PT_ERRPARAM`.

### 3.8 Editing — Literal Semantics (reference)

```c
int pt_insert(pt_Cursor *C, const char *s, size_t len);
int pt_append(pt_Cursor *C, const char *s, size_t len);
int pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len);
int pt_remove(pt_Cursor *C, size_t len);
```

**Does not copy input bytes** — only records pointer/length. The caller must
ensure `s` remains valid for the lifetime of all buffers that reference it.

- **`pt_insert`**: Inserts `s` **before** the cursor position; cursor does not
  move. Equivalent to `pt_append + pt_advance(-len)`. `len==0` no-op;
  `s==NULL` returns `PT_ERRPARAM`.
- **`pt_append`**: Appends `s` **after** the cursor position; cursor moves to
  the end of the insertion (`poff=len`). `len==0` no-op; `s==NULL` returns
  `PT_ERRPARAM`.
  - **Zero-copy merge**: If the insertion point is physically contiguous with an
    adjacent literal piece, directly extends that piece's length without
    splitting the leaf. Merge conditions: left literal tail adjacent or current
    literal tail adjacent.
- **`pt_splice`**: First `pt_remove(C, del)` deletes `del` bytes, then
  `pt_append(C, s, len)`. `del==0 && (s==NULL || len==0)` is a valid no-op.
- **`pt_remove`**: Deletes `len` bytes from the cursor position. Auto-clamps
  `len` to `bytes - offset`. `len==0` or cursor already at tree tail
  (`offset >= bytes`) returns `PT_OK` immediately.
  - Same-leaf deletion and cross-leaf deletion are handled internally; the tree
    is rebalanced as needed.
  - Cursor landing: after deletion, the cursor points to the original deletion
    start position — i.e., the first byte after the deleted range.

### 3.9 Transactions

```c
pt_Buffer pt_rollback(pt_Cursor *C);
pt_Buffer pt_commit(pt_Cursor *C);
```

**Cursor transient state**: The first `pt_edit`/`pt_insert`/`pt_append`/
`pt_splice`/`pt_remove` automatically forks a new internal tree with a fresh
version, records the previous tree as its COW source, and retains it. This
internal tree is held only by the cursor and is unreachable externally.
Subsequent edits continue on this internal tree until commit or rollback.

Both functions return an **owned reference** and **detach the cursor**
(`C->tree = NULL`) — re-attach with `pt_seek` on the returned buffer.

- **`pt_commit`**:
  - **No pending edits (`!C->dirty`)**: retains the current buffer once and
    returns it.
  - **Pending edits (`C->dirty`)**: freezes hole data into arena blocks, replaces
    hole pieces with literal pointers, merges physically adjacent literals, and
    rebalances the tree. Clears dirty, returns the new buffer.
  - If the freeze OOMs (arena allocation fails), returns `NULL`: the tree stays
    consistent, dirty is preserved, and the cursor stays attached (repositioned)
    so the commit can be retried.

- **`pt_rollback`**:
  - **No pending edits**: retains the current buffer once and returns it (same
    as clean commit).
  - **Pending edits**: discards the internal transient tree and returns the
    pre-edit source buffer, retained for the caller. This is unconditionally
    safe — the returned reference keeps the source alive even if no one else
    held it.

**Ownership rule summary**:
- Buffers returned by `pt_commit` / `pt_rollback` are **already owned by the
  caller**; no extra `pt_retain` needed.
- If the caller previously held the pre-edit buffer, it should `pt_release` the
  old buffer after commit (replaced by the new buffer).
- `pt_Cursor` itself **owns no buffer references**.

### 3.10 Arena Direct Write

```c
char       *pt_reserve(pt_Cursor *C, size_t len);
char       *pt_scratch(pt_Cursor *C, size_t *plen);
const char *pt_literal(pt_Cursor *C, size_t len);
```

The arena is a **per-tree** block chain that stores frozen literal data.

**Typical flow**:
1. `pt_reserve(C, n)` reserves ≥n bytes of writable buffer.
2. User writes data directly to the returned pointer.
3. `pt_literal(C, n)` consumes the just-written n bytes as a literal piece
   (suitable for appending to the tree).

- **`pt_reserve`**: Reserves ≥`len` bytes of contiguous writable space in the
  cursor's current tree arena. `len==0` reserves `PT_ARENA_SIZE` bytes. Searches
  the active chain for the first block with sufficient space; allocates a new
  block if none is found. Marks the tree dirty if needed. Returns writable
  pointer, `NULL` on failure.
  - Full blocks are moved to a separate full chain; subsequent reserves may
    traverse the full chain to find blocks with remaining space.
  - If `len > PT_ARENA_SIZE` and all blocks are insufficient, allocates a new
    block of exactly ≥`len`.
- **`pt_scratch`**: Queries remaining writable space at the current arena write
  head. Does not allocate, does not dirty. Returns a pointer to the start of
  remaining space in the current block; returns `NULL` with `*plen=0` if no
  block exists.
- **`pt_literal`**: Consumes `len` bytes from scratch space as a literal piece.
  Marks the tree dirty if needed. Requires the current block to have ≥`len`
  bytes remaining, else returns `NULL`. After consumption, if the block is full,
  it is moved to the full chain. Returns `const char*` (data ownership
  transferred to arena).
  - `len==0` returns `NULL`.
  - The returned pointer can be passed directly to `pt_append`/`pt_insert` etc.
    — data is already in the arena and will be freed with the tree.

---

## 4. Ownership, Lifetime & Threading

- Every `pt_Buffer` is an owned reference unless it is the sentinel from
  `pt_empty`.
- A cursor borrows its buffer; keep the buffer alive while the cursor is in use.
- After `pt_reset`/`pt_close`, all buffers and cursors created from that state
  are invalid; any remaining arena blocks are reclaimed by the state.
- After `pt_commit`/`pt_rollback`, the cursor is detached; re-seek it on the
  returned buffer to continue.
- Multiple cursors can share one transient tree if copied from the same cursor,
  but tree-structure changes can invalidate the other cursor's path pointers.
  The library does not guard against this; callers must avoid concurrent
  structural edits through aliased cursors.
- The library is not thread-safe: a state, its buffers, and its cursors must not
  be accessed concurrently without external synchronization.

---

## 5. Design Overview

### 5.1 Piece Table & B+ Tree

piecetab stores a sequence of bytes as a B+ tree whose leaves are **pieces**.
Each piece is either:

- **literal** — a non-owning `const char*` pointer into user memory or frozen
  arena data; zero extra overhead.
- **hole** — a mutable fixed-capacity buffer used only during an edit session;
  its content is copied in, and is frozen into the arena at commit.

Internal nodes store cumulative byte counts, giving O(log n) offset lookup and
O(1) total byte count. A small bitmap on each node tracks which children are
holes (or contain holes), letting commit skip hole-free subtrees quickly.

### 5.2 COW Snapshots & Transient Edits

A `pt_Buffer` is immutable. Editing always goes through a cursor:

1. On first edit, the cursor forks a private transient tree from the buffer's
   tree, assigns a fresh version, and retains the source.
2. Nodes along the edited paths are copied only when their version differs from
   the transient root (copy-on-write).
3. `pt_commit` freezes holes into arena literals and returns a new immutable
   buffer.
4. `pt_rollback` discards the transient tree and returns the retained source.

This gives cheap snapshots: unchanged subtrees are shared between versions, and
the source chain is kept alive through reference counts.

### 5.3 Transactional Error Handling

Edit operations pre-reserve every pool object they may need before mutating the
tree, so allocation failure happens strictly before any mutation. If
`pt_edit`/`pt_append`/`pt_insert`/`pt_splice`/`pt_remove` returns `PT_ERRMEM`:

- The buffer content is **unchanged** (transactional failure).
- The cursor remains valid at its original position.
- The caller may retry the edit, continue with other edits, or
  `pt_rollback`/`pt_commit` as usual.

### 5.4 Compaction

`pt_compact` is a low-frequency maintenance operation with two phases:

1. **Classify pieces**: walk the buffer's COW source chain, collect every arena
   block interval, and binary-search each source piece to decide whether it
   points into an arena (internal, migrate) or into external user memory (keep).
2. **Rebuild**: stream all reachable pieces into a fresh rightmost chain, merge
   physically adjacent internal pieces, and rebalance the trailing path.

Internal pieces are migrated through `pt_reserve` + `memcpy` + `pt_literal`, so
consecutive migrations land back-to-back and merge. On OOM the half-built
transient is discarded via rollback and `NULL` is returned; the source buffer is
never touched.

---

## 6. Further Reading

Implementation-level details, invariants, and historical design rationale are
kept in the notes directory:

- `notes/design_piecetab_v2.md` — full design document
- `notes/brief_piecetab.md` — concise implementation constraints and invariants
