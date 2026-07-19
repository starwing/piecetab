# undotree.h API Reference

**English** | [中文](undotree.zh.md)

> Single-header C89 library providing version-tree + edit journal + diff service
> on top of the `(off, del, ins)` interval algebra. Prefix `ut_`.
> Rides on `pt_Buffer` COW snapshots (payload carries the full text for any
> version); **not** a COW replacement.

---

## 1. Data Types

### Error Codes

| Macro         | Value | Meaning                        |
| ------------- | ----- | ------------------------------ |
| `UT_OK`       | 0     | Success                        |
| `UT_ERRPARAM` | -1    | Null pointer or invalid param  |
| `UT_ERRMEM`   | -2    | Memory allocation failure      |

### ut_Alloc — Allocator

```c
typedef void *ut_Alloc(void *ud, void *p, size_t osize, size_t nsize);
```

realloc semantics. `p=NULL, osize=0` = allocate new; `nsize=0` = free `p`.
Default `utS_defallocf` wraps `realloc`, aborts on failure.

### ut_State — Memory Context

```c
typedef struct ut_State ut_State;
```

Owns the allocator callback + userdata, the optional `ut_Cleaner` callback,
and the `scratch` buffer used by `ut_diff`/`ut_hunks`.

### ut_Cleaner — Payload Release Callback

```c
typedef void ut_Cleaner(void *ud, ut_Payload *p);
```

Called by `ut_deltree` (and `utN_freechildren`) for each node's payload
before the node is freed. Set via `ut_setcleaner`.

### ut_Payload — Opaque Version Snapshot

```c
typedef struct ut_Payload ut_Payload;
```

Opaque pointer. Callers cast their snapshot (e.g. `pt_Buffer`) into it
and store it as a node's payload. undotree never interprets it.

### ut_Vid — Version Handle

```c
typedef const struct ut_Node *ut_Vid;
```

Opaque pointer to a version node. Address is stable for the life of the
tree (no pruning). Pass to `ut_diff`, navigation macros, and `ut_switch`.

### ut_Hunk — Change Segment

```c
typedef struct ut_Hunk {
    size_t pa;   /* parent: delete start offset            */
    size_t ca;   /* child:  insert start offset            */
    size_t pdel; /* parent: bytes deleted                  */
    size_t cins; /* child:  bytes inserted                 */
} ut_Hunk;
```

Semantics (parent → child): "parent bytes `[pa, pa+pdel)` are replaced
by child bytes `[ca, ca+cins)`". `pa`/`pdel` are in the parent's
coordinate system; `ca`/`cins` are in the child's.

### ut_Entry — Journal Entry

```c
typedef struct { size_t off, del, ins; } ut_Entry;
```

A single edit before commit: delete `del` bytes at offset `off`, insert
`ins` bytes at the same position.

---

## 2. Lifecycle API

### ut_open

```c
UT_API ut_State *ut_open(ut_Alloc *allocf, void *ud);
```

Create a new undotree state. Pass `NULL` for the default allocator.
Returns `NULL` on OOM.

### ut_close

```c
UT_API void ut_close(ut_State *S);
```

Close a state. Frees `scratch`. `S=NULL` is a no-op. Does NOT free
trees — call `ut_deltree` first.

### ut_setcleaner

```c
UT_API void ut_setcleaner(ut_State *S, ut_Cleaner *f, void *ud);
```

Register a payload release callback. Called when a node is destroyed
(via `ut_deltree`). `S=NULL` is a no-op.

---

## 3. Tree Lifecycle

### ut_newtree

```c
UT_API ut_Tree *ut_newtree(ut_State *S, ut_Payload *pl);
```

Create a new version tree. `pl` becomes the root node's payload.
Returns `NULL` if `S` is `NULL` or on OOM.

### ut_deltree

```c
UT_API void ut_deltree(ut_State *S, ut_Tree *T);
```

Destroy a tree with all its nodes. Each node's payload is passed through
the `cleaner` callback. Uses iterative pointer traversal — non-recursive,
zero extra allocation. `T=NULL` is a no-op.

---

## 4. Journal API

Edits are recorded as `(off, del, ins)` tuples before being committed.

### ut_record

```c
UT_API int ut_record(ut_Tree *T, size_t off, size_t del, size_t ins);
```

Append an edit to the journal. No side effects on failure. Returns
`UT_OK` or `UT_ERRMEM`.

### ut_unrecord

```c
UT_API void ut_unrecord(ut_Tree *T, unsigned n);
```

Pop the most recent `n` journal entries. No-op if `n == 0` or journal
is empty. Clears all remaining entries if `n` exceeds journal length.
No-op if `T` is NULL.

### ut_freshcount

```c
#define ut_freshcount(T) ((T) ? (int)utV_len((T)->journal) : 0)
```

Number of uncommitted journal entries.

### ut_discard

```c
UT_API int ut_discard(ut_Tree *T);
```

Discard all uncommitted journal entries (set count to 0). Also resets
`diffhn` to -1 (invalidates any pending diff).

---

## 5. Versioning API

### ut_commit

```c
UT_API ut_Vid ut_commit(ut_Tree *T, ut_Payload *pl);
```

Commit the current journal entries: normalise them into a hunk list,
create a new node as a child of `current`, clear the journal, and set
`current` to the new node. `pl` is the new node's payload (e.g. the
post-edit `pt_Buffer`).

Returns the new `ut_Vid`, or `NULL` on error (OOM — journal is preserved,
tree unchanged, retryable).

Committing without journal entries still creates a node (with an empty
hunk list).

### ut_switch

```c
UT_API int ut_switch(ut_Tree *T, ut_Vid v);
```

Move `current` to another version node. `ut_freshvid(S)` is explicitly
rejected (`UT_ERRPARAM`) — call `ut_discard` first. Fails with
`UT_ERRPARAM` if journal is non-empty.

### ut_discard

See §4 above — also invalidates pending diff.

---

## 6. Navigation

All macros are safe on NULL input.

| Macro / Function                        | Returns            |
| --------------------------------------- | ------------------ |
| `ut_root(T)`                            | `&T->root`         |
| `ut_current(T)`                         | `T->current`       |
| `ut_parent(v)`                          | `v->parent`        |
| `ut_payload(v)`                         | `v->payload`       |
| `ut_childcount(v)`                      | `int`: # children  |
| `ut_firstchild(v)`                      | oldest child       |
| `ut_lastchild(v)`                       | youngest child     |
| `ut_nextsib(c)`                         | next younger sib   |
| `ut_younger(v)`                         | chrono next node   |
| `ut_older(v)`                           | chrono prev node   |

### ut_ancestor

```c
UT_API ut_Vid ut_ancestor(ut_Vid a, ut_Vid b);
```

Lowest common ancestor via depth-levelling + parent chasing. Returns
`NULL` for cross-tree comparisons or `NULL` input.

---

## 7. Diff API

### ut_freshvid

```c
#define ut_freshvid(S) ((ut_Vid)(S))
```

Sentinel representing the uncommitted journal state. When passed as
`from` or `to` to `ut_diff`, replaces that endpoint with `T->current`
and prepends/appends the journal contents.

### ut_diff

```c
UT_API int ut_diff(ut_Tree *T, ut_Vid from, ut_Vid to);
```

Compute the diff between two versions. Both `from` and `to` may use
`ut_freshvid(S)` to represent the uncommitted state.

Returns the number of hunks (≥0), or a negative error code.
The result is stored in `S->scratch` and remains valid until the next
`ut_diff` call on the same state.

Internally: four-phase compose:
`[inv(journal)] + from→LCA⁻¹ + LCA→to + [journal]`

### ut_freshdiff

```c
UT_API int ut_freshdiff(ut_Tree *T, int i, int j);
```

Compute the diff spanned by journal entries in `[i, j)`. `i` and `j` are
journal snapshot indices: `fresh(i)` = state after applying `journal[0..i)`.
`i < j` computes a forward diff (push state from i to j); `i > j` computes
a reverse diff (normalise `journal[j..i)` then invert). Out-of-range `i`/`j`
are clamped to `[0, journal_len]`.

Returns the hunk count (≥0) or a negative error code. Result is stored in
`S->scratch`; retrieve it via `ut_hunks`. Shares scratch with `ut_diff`.

### ut_hunks

```c
UT_API const ut_Hunk *ut_hunks(ut_Tree *T, size_t *pn);
```

Get the current hunk list. If a pending diff exists (`diffhn ≥ 0`),
returns `S->scratch`; otherwise returns `current->h` (the committed
changeset from `current`'s parent to `current`).

Writes the hunk count to `*pn` (if `pn` is non-NULL). Returns `NULL` if
`T` is `NULL`.

---

## 8. Integration Guide

### Edit Flow

```
ut_record(T, off, del, ins)   -- journal the edit (may fail, no side effect)
pt_splice(...)                 -- perform the edit on the buffer
ut_commit(T, new_buffer)      -- freeze into a version node
```

On `pt_splice` failure: `ut_unrecord(T, 1)` to pop the failed entry.

### Consumer Flow (e.g. linecache)

```
ut_diff(T, last_applied_vid, ut_freshvid(S))  -- get pending changes
h = ut_hunks(T, &hn);
for (i = hn-1; i >= 0; i--)                   -- apply right-to-left
    lc_splice(cache, h[i].pa, h[i].pdel, h[i].cins);
```

---

## 9. Configuration

| Macro            | Default | Meaning                          |
| ---------------- | ------- | -------------------------------- |
| `UT_PAGE_SIZE`   | 65536   | Pool allocator page size         |

`UT_STATIC_API` (shared with all headers) makes API functions static.
No fanout or capacity macros — undotree uses dynamic vecs, not B+ trees.

---

## 10. License

[MIT](../LICENSE), same as Lua.
