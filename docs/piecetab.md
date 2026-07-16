# piecetab.h API Reference & Implementation Notes

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

Owns three object pools (`pt_Pool`): `nodes` (`pt_Node`), `holes` (`pt_Hole`),
`trees` (`pt_Tree`), plus an embedded sentinel `empty` tree (zero allocation).
Obtains physical memory via the `pt_Alloc` callback.
`max_version` is the global COW version counter, incremented each time a new tree root is allocated.

### pt_Buffer — COW Snapshot

```c
typedef const struct pt_Tree *pt_Buffer;
```

`pt_Buffer` is an **immutable view** of the buffer state, backed by a reference-counted B+ tree (`pt_Tree`).
All functions returning `pt_Buffer` return an **owned reference**; the caller must `pt_release` when no longer needed.
`pt_Buffer` is a `const` pointer — the tree must not be modified through a Buffer; editing must go through a cursor (`pt_Cursor`).

**Sentinel**: `pt_empty()` returns `&S->empty`, whose refcount is 1 but **never decremented** — `pt_release` returns 0 immediately on `S->empty`. Thus buffers returned by `pt_empty` need no release.

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

- `paths[l]` points to a slot in `ptK_parent(C, l)->children`; `paths[levels]` points to a leaf slot
- Absolute offset = `pt_offset(C)` = `off + poff`
- **The cursor owns no buffer reference** — it only borrows the buffer passed to `pt_seek`/`pt_locate`. The caller must keep that buffer alive while the cursor is in use
- Constructed by `pt_seek` (clears dirty); `pt_locate`/`pt_advance` preserve dirty

### pt_Delta

```c
typedef ptrdiff_t pt_Delta;
```

Signed byte offset for move-type APIs.

### pt_Alloc

```c
typedef void *pt_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
```

Custom allocator (realloc semantics). `ptr=NULL, osize=0` allocates a new block; `nsize=0` frees `ptr`.
Default `ptS_defallocf` wraps `realloc`, aborting on failure.

---

## 2. Configuration Macros

| Symbol            | Default | Meaning                                   |
| ----------------- | ------- | ----------------------------------------- |
| `PT_FANOUT`       | 62      | Maximum children per node (≤64)           |
| `PT_MAX_HOLESIZE` | 64      | Hole capacity (bytes)                     |
| `PT_MAX_LEVEL`    | 16      | Maximum tree depth (paths array size)     |
| `PT_PAGE_SIZE`    | 65536   | Pool allocator page size                  |
| `PT_ARENA_SIZE`   | 1024    | Arena block minimum capacity              |
| `PT_COMPACT_RANGES` | 64    | Compact range array initial capacity      |
| `PT_STATIC_API`   | —       | When defined, all functions become static |

Half-full threshold = `FANOUT/2`. `PT_FANOUT >= 4` has a static assertion (makeroom needs at most 2 empty slots).

---

## 3. Public API

### 3.1 Lifecycle

```c
pt_State *pt_open(pt_Alloc *allocf, void *ud);
void      pt_reset(pt_State *S);
void      pt_close(pt_State *S);
pt_Alloc *pt_getallocf(pt_State *S, void **pud);
```

- **`pt_open`**: Creates a state object. `allocf` is a custom allocator; pass `NULL` for default `realloc`.
  Initializes three object pools and the `empty` sentinel (`refc=1`). Returns `NULL` on failure (OOM).
- **`pt_reset`**: Frees all pools within the state (including all allocated nodes, holes, trees). The state object itself is retained.
  All buffers and cursors depending on this state are invalidated. Accepts `NULL`.
- **`pt_close`**: `pt_reset` + frees the `pt_State` structure. Accepts `NULL`.
- **`pt_getallocf`**: Returns the allocator function and user data associated with the state (output via `pud`, which may be `NULL`).

**Constraint**: Nodes/holes/trees must not be shared across states. All buffers and cursors are invalidated after `pt_reset`.

### 3.2 Buffer Reference Counting

```c
unsigned pt_retain(pt_Buffer b);
unsigned pt_release(pt_Buffer b);
```

- **`pt_retain`**: Increments the reference count, returns the new count. `b==NULL` returns 0.
- **`pt_release`**: Decrements the reference count. When it reaches zero, recursively frees private nodes + holes + arenas, then follows the `from` chain (COW source chain) to release nodes private to prior versions, terminating at the `S->empty` sentinel. `b==NULL` or `b==&S->empty` returns 0.

`from` chain mechanism: each tree records its COW fork source (`tree->from`); during release, this chain ensures shared nodes survive — only nodes with `version == this tree's version` are freed.

### 3.3 Buffer Construction & Queries

```c
pt_Buffer   pt_empty(pt_State *S);
pt_Buffer   pt_from(pt_State *S, const char *s, size_t len);
pt_Buffer   pt_compact(pt_State *S, pt_Buffer b);
unsigned  pt_version(pt_Buffer b);
size_t    pt_bytes(pt_Buffer b);
```

- **`pt_empty`**: Returns `&S->empty` (state-embedded sentinel, zero allocation, zero bytes, no pieces).
  No `pt_release` needed. `S==NULL` returns `NULL`.
- **`pt_from`**: Constructs a single-piece buffer from external memory. **Does not copy** — only records pointer and length;
  the caller must ensure `s` remains valid for the buffer's lifetime. `len==0` returns an empty tree (`bytes=0`).
  `S==NULL` or `s==NULL && len>0` returns `NULL`. The returned buffer has one reference (refc=1).
- **`pt_compact`**: Produces a **fresh standalone buffer** holding only the content currently
  reachable from `b`, with `from = empty` (the COW history chain is cut).
  - **internal** literals (bytes living in any arena along `b`'s `from` chain) are copied into the
    new buffer's own compact arena; adjacent internal pieces become physically contiguous and merge
    into single pieces
  - **external** literals (user memory from `pt_from`/`pt_insert`, e.g. a large mmap) keep their
    original pointers — never copied
  - Does **not** release `b`; the caller releases the old chain afterwards to reclaim all of its
    memory (`pt_Buffer nb = pt_compact(S, b); pt_release(b);`)
  - `b->bytes==0` (including the sentinel) returns `pt_empty(S)`; on OOM returns `NULL` with `b`
    untouched; `S==NULL`, `b==NULL` or a buffer from another state returns `NULL`
  - Cost is O(fragments), independent of the original file size — removed content is not in the
    tree and is never visited
- **`pt_version`**: Returns the buffer's version number (the root node's `version` field). `b==NULL` returns 0.
  The version number is `++S->max_version` assigned at creation time.
- **`pt_bytes`**: Returns total byte count of the buffer. `b==NULL` returns 0. O(1).

### 3.4 Cursor Query Macros

```c
#define pt_offset(C) ((C)->off + (C)->poff)
#define pt_buffer(C)   ((C)->tree)
```

- **`pt_offset`**: The cursor's current absolute byte offset. Direct dereference; caller must ensure `C` is non-NULL.
- **`pt_buffer`**: The tree pointer currently associated with the cursor. In transient state this is the internal new tree; otherwise the buffer passed to `pt_seek`.

### 3.5 Cursor Positioning & Movement

```c
int pt_seek(pt_Cursor *C, pt_Buffer b, size_t off);
int pt_locate(pt_Cursor *C, size_t off);
int pt_advance(pt_Cursor *C, pt_Delta d);
```

- **`pt_seek`** — Cursor constructor: binds a buffer and clears dirty. After `memset(C,0,sizeof(pt_Cursor))` resets the cursor,
  if `off >= b->bytes` positions at tree tail (`locend`), otherwise `findleaf` descends top-down. Returns `PT_OK` or `PT_ERRPARAM`.
- **`pt_locate`** — Relocates the cursor within the bound buffer. `C->off`, `C->poff` are zeroed before locating.
  **Preserves dirty state** (reposition during editing). Returns `PT_OK` or `PT_ERRPARAM`.
- **`pt_advance`** — Moves the cursor by byte offset. `d>0` forward, `d<0` backward.
  - Out-of-bounds auto-clamps: `<0` clamps to offset=0, `>bytes` clamps to offset=bytes
  - `d==0` or empty tree (`bytes==0`) returns `PT_OK` immediately
  - **Preserves dirty state**
  - Returns `PT_OK` or `PT_ERRPARAM`

### 3.6 Piece Traversal & Reading

```c
const char *pt_piece(pt_Cursor *C, size_t *plen);
const char *pt_next(pt_Cursor *C, size_t *plen);
const char *pt_prev(pt_Cursor *C, size_t *plen);
size_t      pt_read(pt_Cursor *C, char *buf, size_t len);
```

**Semantics: "Move then return the landing point"** — `pt_next`/`pt_prev` move the cursor first, then return the target piece's data pointer.

- **`pt_piece`**: Returns the **remaining** data of the current piece from `C->poff` onward; out parameter `plen` is set to the remaining length. When `C->poff >= piece->bytes[i]` (cursor past piece end) or no tree exists, returns `NULL` with `*plen = 0`. Typical traversal idiom: `for (p = pt_piece(c, &n); n; p = pt_next(c, &n))`.
- **`pt_next`**: If the cursor is inside the current piece (`poff < bytes[i]`), consumes the remaining bytes,
  moves right to the start of the next piece, and returns the new piece's full data pointer. If already at piece end
  (`poff == bytes[i]`), jumps directly to the next piece. When no next piece exists (tree tail), returns `NULL`
  with `*plen=0`, cursor stays at tree tail. Subsequent `pt_piece` returns `NULL`.
- **`pt_prev`**: If the cursor is inside the current piece (`poff > 0`), moves left to the piece start,
  returns the full data pointer. If already at piece start (`poff == 0`) and not at tree head, moves left
  to the previous piece start and returns. At tree head (`off==0 && poff==0`), returns `NULL`
  with `*plen=0`.
- **`pt_read`**: Copies `len` bytes from the cursor position piece-by-piece into `buf`, advancing the cursor as it goes.
  Automatically crosses piece boundaries. Returns actual bytes copied when fewer than `len` are available. `C==NULL` or `buf==NULL` returns 0.
  `pt_read` internally loops over `pt_piece`/`pt_next`, **moving the cursor**.

### 3.7 Editing — Hole Semantics (copy)

```c
int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len);
```

Deletes `del` bytes at the cursor then inserts `s` (`len` bytes), equivalent to `pt_remove + pt_append`,
but the inserted data goes through a **hole piece** (internally allocated fixed-capacity buffer, memcpy'd), giving **copy semantics**.

- `len` **must** be `≤ PT_MAX_HOLESIZE`, else returns `PT_ERRPARAM`
- Calls `pt_remove(C, del)` before insertion (see non-atomic edit semantics below)
- Insertion tries to merge with an adjacent trailing hole first: if the current or left-side piece is a hole with sufficient capacity
  (`ptH_fit`), appends locally via `memmove` without splitting the leaf — fast path
- Otherwise goes through `ptI_splitins` to split the leaf and insert a new hole
- `del==0 && len==0` is a valid no-op
- `s==NULL && len>0` returns `PT_ERRPARAM`

### 3.8 Editing — Literal Semantics (reference)

```c
int pt_insert(pt_Cursor *C, const char *s, size_t len);
int pt_append(pt_Cursor *C, const char *s, size_t len);
int pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len);
int pt_remove(pt_Cursor *C, size_t len);
```

**Does not copy input bytes** — only records pointer/length. The caller must ensure `s` remains valid for the lifetime of all buffers that reference it.

- **`pt_insert`**: Inserts `s` **before** the cursor position; cursor does not move. Equivalent to `pt_append + pt_advance(-len)`.
  `len==0` no-op; `s==NULL` returns `PT_ERRPARAM`.
- **`pt_append`**: Appends `s` **after** the cursor position; cursor moves to the end of the insertion (`poff=len`).
  `len==0` no-op; `s==NULL` returns `PT_ERRPARAM`.
  - **Zero-copy merge**: If the insertion point is physically contiguous with an adjacent literal piece, directly extends that piece's `bytes[i]`
    without splitting the leaf. Merge conditions: `(C->poff==0, left piece non-hole, pointers contiguous)` or
    `(C->poff==bytes[i], current piece non-hole, pointers contiguous)`
- **`pt_splice`**: First `pt_remove(C, del)` deletes `del` bytes, then `pt_append(C, s, len)`.
  `del==0 && (s==NULL || len==0)` is a valid no-op.
- **`pt_remove`**: Deletes `len` bytes from the cursor position. Auto-clamps `len` to `bytes - offset`.
  `len==0` or cursor already at tree tail (`offset >= bytes`) returns `PT_OK` immediately.
  - Same-leaf deletion → `ptD_rmleaf` (literal split uses makeroom-style layer splitting)
  - Cross-leaf deletion → `ptD_rmrange` (dual cursor, three-phase: trim→cut→stitch)
  - Cursor landing: after deletion, the cursor points to the original deletion start position — i.e., the first byte after the deleted range

### 3.9 Transactions

```c
pt_Buffer pt_rollback(pt_Cursor *C);
pt_Buffer pt_commit(pt_Cursor *C);
```

**Cursor transient state**: The first `pt_edit`/`pt_insert`/`pt_append`/`pt_splice`/`pt_remove`
automatically forks a new internal tree via `ptK_markdirty` (`version = ++max_version`,
`from = old tree` and retained), held only by the cursor, unreachable externally. Subsequent edits continue on this internal tree
until commit or rollback.

Both functions return an **owned reference** and **detach the cursor**
(`C->tree = NULL`) — re-attach with `pt_seek` on the returned buffer.

- **`pt_commit`**:
  - **No pending edits (`!C->dirty`)**: retains the current buffer once and returns it.
  - **Pending edits (`C->dirty`)**: freezes hole data into the arena (`ptC_freeze`:
    hole content copied into arena blocks, hole pieces replaced with literal pointers,
    physically adjacent literals merged, tree rebalanced), clears dirty, returns the new buffer.
  - If the freeze OOMs (arena allocation fails), returns `NULL`: the tree stays
    consistent, dirty is preserved, and the cursor stays attached (repositioned via
    `pt_locate`) so the commit can be retried

- **`pt_rollback`**:
  - **No pending edits**: retains the current buffer once and returns it (same as clean commit).
  - **Pending edits**: discards the internal transient tree and returns the pre-edit
    source buffer (`from`), retained for the caller. This is unconditionally safe —
    the returned reference keeps the source alive even if no one else held it.

**Ownership rule summary**:
- Buffers returned by `pt_commit` / `pt_rollback` are **already owned by the caller** (owned reference); no extra `pt_retain` needed
- If the caller previously held the pre-edit buffer, it should `pt_release` the old buffer after commit (replaced by the new buffer)
- `pt_Cursor` itself **owns no buffer references**

### 3.10 Arena Direct Write

```c
char       *pt_reserve(pt_Cursor *C, size_t len);
char       *pt_scratch(pt_Cursor *C, size_t *plen);
const char *pt_literal(pt_Cursor *C, size_t len);
```

The arena is a **per-tree** block chain (`pt_Arena`) that stores frozen literal data.

**Typical flow**:
1. `pt_reserve(C, n)` reserves ≥n bytes of writable buffer
2. User writes data directly to the returned pointer
3. `pt_literal(C, n)` consumes the just-written n bytes as a literal piece (suitable for appending to the tree)

- **`pt_reserve`**: Reserves ≥`len` bytes of contiguous writable space in the cursor's current tree arena.
  `len==0` reserves `PT_ARENA_SIZE` bytes. Searches the `current` chain for the first block with sufficient space;
  allocates a new block if none found. Internally calls `ptK_markdirty` (if not already dirty). Returns writable pointer, `NULL` on failure.
  - Full blocks are moved to the `full` chain; subsequent reserves may traverse the full chain to find blocks with remaining space
  - If `len > PT_ARENA_SIZE` and all blocks are insufficient, allocates a new block of exactly ≥`len` (`ptA_alloc`)

- **`pt_scratch`**: Queries remaining writable space at the current arena write head. Does not allocate, does not dirty.
  Returns a pointer to the start of remaining space in the current `current` block; returns `NULL` with `*plen=0` if no block exists.
  Used to check how much space was left by the last `pt_reserve`.

- **`pt_literal`**: Consumes `len` bytes from scratch space as a literal piece.
  Internally calls `ptK_markdirty`. Requires the current `current` block to have ≥`len` bytes remaining, else returns `NULL`.
  After consumption, if the block is full, moves it to the `full` chain. Returns `const char*` (data ownership transferred to arena).
  - `len==0` returns `NULL`
  - The returned `const char*` can be passed directly to `pt_append`/`pt_insert` etc. — data is already in the arena and will be freed with the tree

---

## 4. Data Structure Notes

### 4.1 Leaf = Piece (two kinds)

- **literal**: A non-owning raw `char*` pointing to user memory or frozen arena data. Zero extra overhead.
  Stored in the leaf container's `children[i]` as a `(pt_Node *)` pointer (type-punned).
- **hole**: Mutable leaf in transient state, pooled fixed-capacity buffer:

  ```c
  typedef struct pt_Hole { char data[PT_MAX_HOLESIZE]; } pt_Hole;
  ```

  Length is **not stored in the hole itself** but recorded in the parent node's `bytes[i]` field. The `mask` bitmap distinguishes holes from literals.

### 4.2 pt_Node — Internal Node (also Leaf Container)

```c
typedef struct pt_Node {
    struct pt_Node *children[PT_FANOUT]; /* inner layer=child pointers, leaf layer=data pointers */
    size_t          bytes[PT_FANOUT];    /* inner layer=cumulative subtree bytes, leaf layer=piece length */
    pt_Mask         mask;                /* leaf layer=is-hole, inner layer=subtree contains hole */
    pt_Ver          version;             /* COW version vs tree root */
    unsigned short  child_count;         /* valid child count */
} pt_Node;
```

- `pt_Mask = size_t` single-word bitmap — `PT_FANOUT ≤ sizeof(void*)*CHAR_BIT` (64) has a static assertion
- Half-full threshold = `FANOUT/2`
- The `mask` bitmap enables fast skipping of hole-free subtrees during COW commit freeze

### 4.3 pt_Tree — Tree (Buffer)

```c
typedef struct pt_Tree {
    pt_Node         root;      /* embedded root node (not a pointer); when levels=0 it is a leaf container */
    pt_State       *S;
    struct pt_Tree *from;      /* COW fork source, terminating at S->empty */
    pt_Arena        arena;     /* per-tree literal data arena (lazy) */
    size_t          bytes;     /* total bytes O(1) */
    unsigned        refc;      /* reference count */
    unsigned short  levels;    /* tree height, 0 = root is leaf container */
} pt_Tree;
```

- `root` is embedded in the tree structure (not a pointer), so `pt_from` can build an empty tree with no extra allocation
- `from` is the COW lifecycle chain: `pt_release` follows this chain, freeing only version-matched private nodes
- `levels` model: layer number -1=root, 0..levels-1=internal nodes, levels=leaf container
  - `ptK_parent(C, levels)` = leaf container, whose `children[]` directly stores data pointers

### 4.4 Object Pool (pt_Pool)

Each page is `PT_PAGE_SIZE` bytes, allocating fixed-size objects. Three pools independently manage `pt_Node`/`pt_Hole`/`pt_Tree`.
`ptP_reserve(n)` guarantees at least `n` available objects in the pool (including freelist), used for transaction pre-allocation to prevent OOM mid-edit.
`ptP_ralloc` only takes from the freelist; `assert(freed)` ensures pre-allocation was done.

### 4.5 pt_Arena

```c
typedef struct pt_Arena {
    pt_Block *current; /* chain of blocks with free space (head=active writable) */
    pt_Block *full;    /* chain of full blocks */
} pt_Arena;
```

Block-chain arena (`current`=free-space chain, `full`=full chain), used to store literal data during editing or commit freeze.
Per-tree, freed with the tree. `pt_reserve` searches the `current` chain for blocks with enough space; full blocks are moved to the `full` chain.
Lazily initialized — blocks are allocated only on first `pt_reserve`/`pt_literal`.

---

## 5. Key Algorithms & Semantics

### 5.1 COW / Transaction Model

1. **Mark dirty** (`ptK_markdirty`): On first edit, forks a new tree — copies the old tree structure,
   sets `version = ++max_version`, `from = old tree` and `pt_retain(old)` to keep the source alive.
   The new tree is held only by the cursor, unreachable externally.
2. **Node COW** (`ptK_cow`): Along paths, checks `node->version != root->version` layer by layer;
   if versions don't match, `ptP_ralloc` copies the node and fixes paths to point to the new node.
3. **Beginedit** (`ptK_beginedit`): `ptP_reserve` pre-allocates nodes + `ptK_markdirty`
   + COW all layers `0..levels-1` along paths.
4. **Commit** (`ptC_freeze`): DFS traversal (skipping hole-free subtrees via internal node mask bits),
   copies hole data into arena, replaces hole pieces with literal pointers, returns `pt_Hole` objects to the pool.
   Clears dirty, returns the frozen buffer.
5. **Rollback**: Discards the internal new tree (`pt_release`), returns the retained `from` buffer and detaches the cursor.

### 5.2 Non-Atomic Edit Semantics

Edit operations are **not atomic** under memory pressure. If `pt_edit`/`pt_append`/`pt_insert`/`pt_splice`/
`pt_remove` returns `PT_ERRMEM`:
- Partially completed changes (already inserted/deleted bytes) **remain visible** in the cursor's internal tree
- Data structure invariants (child_count, half-full, bytes sum, mask consistency) are preserved
- The cursor remains valid, positioned at the last successful edit
- The caller may choose `pt_rollback` to discard partial edits, or continue editing and then `pt_commit`

### 5.3 Leaf Split & Insert (ptI_splitins)

Shared insertion path for `pt_edit`/`pt_append`:
1. `ptI_fillrt` — assembles the new piece (+ right half split from the original piece) in `S->rt[0]`, needs = 1 or 2 slots
2. Leaf container has room → `ptN_makespace` + `ptN_copy` inserts directly
3. Not enough room → `ptI_insertrt`: bottom-up search for first non-full layer (if all full, `ptI_splitroot` deepens the tree),
   split layers with `ptI_splitchild`, stitch remaining slots at the target layer
4. `ptM_up` propagates metric delta (bytes) + mask bit segmentally

### 5.4 Range Deletion (ptD_rmrange)

1. Right cursor `R = C + advance(len)`; `ptD_cowpaths` COWs both paths and finds the fork level `fl`
2. Same leaf (l > levels) → `ptD_rmleaf`
3. Cross-leaf → `ptD_rmrange`:
   - `trimright(L)` — deletes right side of L's leaf
   - `trimleft(R)` — deletes left side of R's leaf (literal head-trim uses pointer offset without copy, hole uses memmove)
   - `cutrange` — onion-layer deletion of all child nodes between L and R
   - `stitch` — `mergeleaf` merges the severed leaf ends → `stitchnode` stitches back in onion order → `rebalance` finishes up
   - Cursor lands at the deletion point

### 5.5 Onion-Order Stitch (ptD_stitchnode)

Onion order `for (k=0; k<=levels; ++k)` where `k` is the onion layer (k=0=leaf layer, k=levels=root layer),
`kl = levels - k` is the tree level corresponding to rt[k].
- **Fixed point**: each round first copies (`m = min(rtcc, FANOUT-pcc)`) to fill the current parent node, `ptM_up` updates ancestor metrics
- **kl==0 safety net**: the root layer always enters repair — regardless of whether earlier layers have been processed, at kl==0 foldnode is forced → if root has only one child, shrink the root
- **Deferred effect of d**: records the number of right-side child nodes awaiting repair, set at the end of the current round and consumed by backwardnode in the **next round**

### 5.6 Trailing Empty Leaf (trailing-in-leaf)

Deletion may cause `p->bytes[i]==0` in a leaf container (non-empty leaf, just a zero-length piece). In `ptD_rmleaf`,
`if(ptN_cc(p)==0)` positions the cursor at leaf index 0 (`C->paths[l] = &p->children[0]`),
`C->off=0, C->poff=0`. `pt_piece` therefore returns `NULL` (`poff >= bytes[i]`, since
`bytes[0]==0`), indicating past leaf end.

`ptD_stitch` checks `p->bytes[cc-1] == 0` before mergeleaf and clears that empty slot.
`ptM_up(C, levels, 0)` finishes with mask correction.

### 5.7 Zero-Copy Merge (literal piece physical contiguity)

`pt_append` directly extends an existing literal piece without splitting the leaf when:
- `C->poff == 0 && i>0 && !ishole(i-1) && lit[i-1]+bytes[i-1] == s` — left literal tail adjacent
- `C->poff == bytes[i] && !ishole(i) && lit[i]+bytes[i] == s` — current literal tail adjacent

This allows callers to `pt_append` segments from a sequentially allocated buffer and produce a single physically contiguous piece.

### 5.8 Compaction (pt_compact)

Two phases, both internal to the single low-frequency function:

1. **Interval classification** (`ptZ_collect` / `ptZ_inranges`): walks `b`'s `from` chain
   collecting every arena block interval `[block+1, block+1+size)` into a growable array
   (initial capacity `PT_COMPACT_RANGES`, geometric growth), then qsorts by start address.
   Each source piece is classified by binary search: pointer inside some interval = internal
   (migrate), outside = external (keep). No per-literal tag is stored anywhere — the data
   structures and hot paths carry zero overhead for this.
2. **Bulk tree build** (`ptZ_bulkbuild`, lc_scan style, O(n)): pieces stream into the
   rightmost leaf container (`ptZ_bulkleaf`), merging physically adjacent runs on the fly;
   when the leaf container fills, `ptD_makechain` extends a fresh rightmost chain (deepening
   the root when every level is full). A final top-down `ptD_foldnode` pass plus
   `ptD_rebalance` restores the half-full invariant on the trailing path.

Internal pieces are migrated with `pt_reserve` + `memcpy` + `pt_literal` into the new tree's
arena; consecutive migrations land back-to-back and therefore merge. On OOM the half-built
transient is discarded via rollback and `NULL` is returned; the source buffer is never touched.

---

## 6. Internal Function Naming Scheme

| Prefix | Full Name   | Responsibility                           |
| ------ | ----------- | ---------------------------------------- |
| `ptK_` | Kurser      | Cursor navigation + transient entry      |
| `ptI_` | Insert      | Insert / leaf split                      |
| `ptD_` | Delete      | Delete / rebalance / stitch              |
| `ptM_` | Mask/Metric | Bitmap + metric propagation              |
| `ptN_` | Node        | Node operations (copy/move/remove/purge) |
| `ptH_` | Hole        | Hole operations (new/append/remove)      |
| `ptC_` | Commit      | Freeze hole→literal                      |
| `ptA_` | Arena       | Arena allocation/destruction             |
| `ptZ_` | Zip/Compact | Compaction: range classification + bulk tree build |
| `ptP_` | Pool        | Pool allocator                           |
| `ptS_` | State       | Default allocator                        |

`ptM_up` handles both metric delta `db` and mask propagation upward in one pass. Early exit when `db==0 && mask unchanged`.
`ptM_up(C, levels, 0)` idiom = pure mask correction.
