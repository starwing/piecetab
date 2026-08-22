# spantree.h API Reference & Implementation Notes

**English** | [中文](spantree.zh.md)

> Single-header C89 library that stores **final rendered coloring** as a
> full-coverage partition of byte spans: every byte belongs to exactly one
> span `(len > 0, sp_Id)`. Prefix `sp_`. It shares the B+ tree skeleton and
> pool allocator style of `piecetab.h`/`linecache.h`, but stores only byte
> lengths and attribute ids — no text content, no COW snapshots.

---

## 1. Data Types

### Error Codes

| Macro         | Value        | Meaning                                  |
| ------------- | ------------ | ---------------------------------------- |
| `SP_OK`       | 0            | Success                                  |
| `SP_ERRPARAM` | -1           | Null pointer or out-of-range parameter   |
| `SP_ERRMEM`   | -2           | Memory allocation failure                |
| `SP_NONE`     | `~(sp_Id)0`  | End-of-iteration sentinel (never a valid id) |

### sp_State — Memory Context

```c
typedef struct sp_State sp_State;
```

Owns the allocator callback/userdata and one object pool (`sp_Pool`) for
`sp_Node` plus embedded scratch nodes used during tree stitching. Multiple
trees can share one `sp_State`.

### sp_Tree — Span Tree

```c
typedef struct sp_Tree sp_Tree;
```

One span tree. The root is embedded in the structure, so `sp_newtree` can
only fail on the tree-structure allocation itself. A tree is bound to its
owning `sp_State` for its whole lifetime.

### sp_Cursor — Cursor

```c
typedef struct sp_Cursor sp_Cursor;
```

Non-persistent navigator initialized by `sp_seek`. `paths[0..SP_MAX_LEVEL]`
hold root-to-leaf slot pointers; `off` is the cumulative bytes before the
current span and `poff` is the offset inside it. The absolute byte offset is:

```c
#define sp_offset(C) ((C)->off + (C)->poff)
```

The cursor does **not** own the tree; the caller must keep the tree alive
while the cursor is in use. Like `pt_Cursor`/`lc_Cursor`, the cursor is never
left at a segment end except at the tree end (the "no mid-tree segment-tail"
invariant).

### sp_Delta, sp_Id, sp_Mask

```c
typedef ptrdiff_t sp_Delta;  /* signed byte delta */
typedef size_t    sp_Id;     /* attribute / style id */
typedef size_t    sp_Mask;   /* namespace bitset; layout hidden from users */
```

- `sp_Id` 0 is the valid **uncolored** id. `SP_NONE` is the iteration
  sentinel and must never be stored in a tree.
- `sp_Mask` stores namespace bits. Bit layout is private; use
  `sp_addns`/`sp_delns`/`sp_hasns`. The valid namespace domain is
  `1..SP_MASK_BITS`, where `SP_MASK_BITS = sizeof(sp_Mask) * CHAR_BIT`.

### sp_Alloc — Allocator

```c
typedef void *sp_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
```

realloc semantics. `ptr=NULL, osize=0` allocates a new block; `nsize=0`
frees `ptr`. The default `spS_defallocf` wraps `realloc` and aborts on
failure. Pass `NULL` to `sp_open` to use the default.

### sp_Arbiterf — Blending Callback

```c
typedef sp_Id sp_Arbiterf(void *ud, sp_Id id, sp_Id old, sp_Mask *mask);
```

Called for every style write/merge/delete decision. The parameter order is
"new first": `id` is the incoming id, `old` is the current segment id, and
`mask` is an in/out namespace set.

Three-state contract:

- `arb(ud, 0, old, &mask)` — **death** event (merge/delete/trim/freetree).
- `arb(ud, in, 0, &mask)` — **birth** event (write into an empty segment,
  split fragment, inheritance); must return `in` unchanged.
- `arb(ud, in, old, &mask)` — **merge decision**; return the new id and set
  `mask` to the exact namespace set of the returned id (via
  `sp_addns`/`sp_delns`).

When the returned id is 0, the tree forces the mask to 0. The mask is an
optimization channel for pruned namespace iteration; an incorrect mask can
cause false negatives (the tree does not validate it).

---

## 2. Configuration Macros

| Symbol          | Default | Meaning                                  |
| --------------- | ------- | ---------------------------------------- |
| `SP_FANOUT`     | 62      | Max children per node (must be ≥ 4)      |
| `SP_PAGE_SIZE`  | 65536   | Pool allocator page size                 |
| `SP_MAX_LEVEL`  | 13      | Max tree depth / cursor path array size (see [max_levels.md](max_levels.md)) |
| `SP_STATIC_API` | —       | When defined, all `SP_API` functions become static |

`SP_FANOUT >= 4` is enforced by a static assertion.

---

## 3. Public API

### 3.1 Lifecycle

```c
sp_State *sp_open(sp_Alloc *allocf, void *ud);
void      sp_close(sp_State *S);
```

- **`sp_open`**: creates a state. `allocf == NULL` selects the default
  realloc wrapper. Returns `NULL` on OOM.
- **`sp_close`**: frees the node pool and the state structure. `S == NULL`
  is a no-op. **It does not free trees** — call `sp_freetree` on every
  remaining tree first.

### 3.2 Tree Lifecycle

```c
sp_Tree *sp_newtree(sp_State *S);
void     sp_freetree(sp_Tree *T);
size_t   sp_bytes(const sp_Tree *T);
```

- **`sp_newtree`**: creates an empty span tree. Returns `NULL` if `S` is
  `NULL` or on OOM.
- **`sp_freetree`**: purges all nodes and frees the tree structure.
  `T == NULL` is a no-op.
- **`sp_bytes`**: total number of bytes covered by the spans. Returns 0 for
  `NULL`.

### 3.3 Blending and Namespaces

```c
void sp_setarbiter(sp_Tree *T, sp_Arbiterf *cb, void *ud);
int  sp_addns(sp_Mask *mask, int ns);
int  sp_delns(sp_Mask *mask, int ns);
int  sp_hasns(const sp_Mask *mask, int ns);
```

- **`sp_setarbiter`**: sets the blending callback and userdata for a tree.
  Called on writes, merges, deletes, and tree teardown.
- **`sp_addns`** / **`sp_delns`**: add/remove namespace `ns` in `mask`.
  Returns `SP_OK`; returns `SP_ERRPARAM` if `mask` is `NULL` or `ns` is out
  of the valid domain.
- **`sp_hasns`**: returns 1 if `mask` contains `ns`, 0 otherwise. Returns 0
  for invalid arguments.

### 3.4 Cursor Navigation

```c
int  sp_seek(sp_Cursor *C, sp_Tree *T, size_t off);
int  sp_locate(sp_Cursor *C, size_t off);
int  sp_advance(sp_Cursor *C, sp_Delta d);
#define sp_offset(C) ((C)->off + (C)->poff)
```

- **`sp_seek`**: initializes (or rebinds) a cursor to tree `T` at byte
  offset `off`. If `off >= sp_bytes(T)`, the cursor lands in the trailing
  virtual area (`poff` becomes the distance past the end).
- **`sp_locate`**: moves an existing cursor to `off`, preserving its tree
  binding. Same trailing-virtual behavior as `sp_seek`.
- **`sp_advance`**: moves by a signed byte delta. Movement clamps at the
  start and end; past the end it enters the virtual trailing area.
- **`sp_offset`**: macro returning the absolute byte offset.

All navigation functions return `SP_OK` or `SP_ERRPARAM`.

### 3.5 Marking

```c
int sp_fill(sp_Cursor *C, sp_Id id, size_t len);
int sp_clear(sp_Tree *T, int ns, sp_Id id);
```

- **`sp_fill`**: writes attribute `id` over `[sp_offset(C), sp_offset(C)+len)`.
  Every affected segment is passed through the arbiter (write operations must
  touch all segments in range). `len == 0` is a no-op; `id == SP_NONE` is
  rejected. Writing past the tree end pads the gap with id-0 spans first.
  Returns `SP_OK`, `SP_ERRPARAM`, or `SP_ERRMEM`.
- **`sp_clear`**: prunes by namespace: for every span whose namespace set
  contains `ns`, calls the arbiter once and writes back the result (id 0
  clears the mask). Matching leaf containers are processed in bulk;
  non-matching leaves are not visited — this is the reason `sp_clear` exists
  as a separate operation. `ns` must be in `1..SP_MASK_BITS`;
  `id == SP_NONE` is rejected. Returns `SP_OK` or `SP_ERRPARAM`.

### 3.6 Reading

```c
sp_Id sp_style(sp_Cursor *C, size_t *plen, sp_Mask *pmask);
sp_Id sp_next(sp_Cursor *C, int ns, size_t *plen);
sp_Id sp_prev(sp_Cursor *C, int ns, size_t *plen);
```

- **`sp_style`**: returns the id of the current span, `*plen` = remaining
  bytes in that span, and optionally `*pmask` = the span's exact namespace
  set. At the tree end / virtual area it returns `SP_NONE` and sets `*plen`
  to 0.
- **`sp_next`**: **exclusive** next — skips the current span and returns the
  next span matching namespace `ns` (`ns == 0` means any namespace). On
  success it returns the id, sets `*plen` to the full segment length, and
  leaves the cursor at the segment head. At the end it returns `SP_NONE` and
  `*plen = 0`. Out-of-domain `ns` returns `SP_NONE`.
- **`sp_prev`**: previous matching span. If the cursor is inside a matching
  span it returns the prefix from that span's head to the cursor
  (`*plen = C->poff`) and moves to the segment head; otherwise it walks back
  to the previous matching segment. At the start it returns `SP_NONE`.

Consumption pattern: `seek(0)` → `sp_style` (peek current) → `sp_next(ns)`
(step). Because `sp_next` is exclusive and lands on segment heads, this loop
cannot livelock.

### 3.7 Editing

```c
int sp_splice(sp_Cursor *C, size_t del, size_t ins);
int sp_append(sp_Cursor *C, size_t ins);
int sp_insert(sp_Cursor *C, size_t ins);
int sp_remove(sp_Cursor *L, sp_Cursor *R);
```

- **`sp_splice`**: deletes `del` bytes at the cursor, then inserts `ins`
  bytes. Deletion is clamped to the tree end; insertion inherits the **left**
  segment id.
- **`sp_append`**: inserts `ins` bytes at the cursor, inheriting the **left**
  segment id (like `a` in vim/sam).
- **`sp_insert`**: inserts `ins` bytes at the cursor, inheriting the **right**
  segment id (like `i`).
- **`sp_remove`**: deletes bytes from `sp_offset(L)` to `sp_offset(R)`.
  `L` and `R` must belong to the same tree; if `L >= R` it is a no-op.
  Returns `SP_ERRPARAM` on invalid cursors/trees.

All editing functions return `SP_OK`, `SP_ERRPARAM`, or `SP_ERRMEM`. On
`SP_ERRMEM` the tree is left structurally consistent.

---

## 4. Data Structure Notes

- **Full-coverage span model**: the tree is a partition of `[0, sp_bytes)`
  into `(len > 0, id)` runs. There are no zero-length marks, no byte-gap
  semantics, and no extmark-style point identities. Sparse coloring is just a
  large id-0 span.
- **No COW**: spantree is a frame-buffer-like final result store. It does
  not snapshot, does not hold content, and does not need versioning.
- **B+ tree skeleton**: shares the piecetab/linecache structure: embedded
  root, pool allocation, byte metrics, split/merge/stitch operations.
- **Namespace mask as third metric channel**: each node slot carries a
  `sp_Mask`; interior slots are the OR of their children. This enables
  pruned `sp_next`/`sp_prev`/`sp_clear` without scanning the whole tree.
- **Arbiter single layer**: blending policy is fully external. The tree has
  zero format knowledge; it only stores ids and calls the arbiter. The
  arbiter can encode operators, attrs, and writer identity in its id space.
- **No gravity**: gravity is decided by the operation, not stored in marks:
  `sp_append` inherits left, `sp_insert` inherits right, `sp_remove` only
  shrinks spans.
- **Cursor invariant**: cursors never rest at a mid-tree segment end; the
  only end position is the tree end (virtual area). This keeps `sp_style` +
  `sp_next` consumption loops well-formed.

See [`../notes/design_spantree.md`](../notes/design_spantree.md) for the
full design rationale, algorithm history, and namespace/arbiter details.
The Lua binding (`sp.compositor` / `sp.new` / cursor API) is documented in
`lua/spantree.d.lua` and `../notes/design_spantree_lua.md`.
