# piecetab
<!-- BADGES: add badges here -->

A lightweight, stb-style single-header library implementing a piece table backed by a B+ tree for fast text editing.

## Motivation
This project is driven by the need for a **high-performance, low-latency text buffer** that remains predictable under heavy edits, large files, and complex Unicode content. We focus on:
- Stable performance characteristics under insert/delete workloads
- Efficient statistics (line/char/byte) for editor features
- A compact, single-header implementation suitable for embedding

## Design Summary
- **Core structure**: B+ tree
- **Pieces**: stored only at leaf nodes
- **Pieces are immutable**, but can be split/merged as edits occur
- **Statistics** are stored per piece and aggregated in internal nodes
- **Cursor** carries path state; nodes do not store parent pointers
- **COW + versioning** supports batch edits

## Why a Single-Header Library?
`piecetab` is designed in the **stb style**:
- Drop-in, single-file integration
- Minimal build system friction
- Clear public API with optional internal configuration

## Features
- Fast insert/delete with path-based cursor
- Whole-subtree deletion optimization
- Configurable piece chunking strategy
- Per-piece tags with mixed persistence model
- Batch editing via versioned COW

## Building
This is a header-only library. Include `piecetab.h` in your project:
```c
#define PT_IMPLEMENTATION
#include "piecetab.h"
```

## Example
<!-- TODO: add examples -->

## Reference

#### Error codes
- `PT_OK` — success.
- `PT_ERRPARAM` — invalid parameter (e.g., null cursor).
- `PT_ERREMPTY` — empty tree or no next/previous fragment.

### Snapshot & ownership model
- `pt_Blob` is an immutable view of a buffer state.
- All functions that return a `pt_Blob` return it with **one owned reference**. You must call `pt_release` when you no longer need it.
- `pt_retain` increments a blob reference; `pt_release` decrements it.
- A `pt_Cursor` does **not** own a blob reference. It only borrows the
	blob passed to `pt_locate`/`pt_locline`.
- The caller must keep the input blob alive while the cursor is in use.
- The library **does not** maintain a history tree. If you want undo/redo graphs, store `pt_Blob` values externally and manage relationships yourself.

### Navigation
- `pt_peek(c, &len)` returns a pointer to the current readable fragment and its
	length in bytes. The fragment is the contiguous range from the cursor position
	to the end of the current line segment within the current piece (or the piece
	end if there is no further line break in that piece). If there is no readable
	fragment, it returns `NULL` and sets `len = 0`.

- `pt_next(c)` advances the cursor to the next line fragment. It returns
	`PT_OK` on success, `PT_ERRPARAM` for a null cursor, or `PT_ERREMPTY` when
	there is no next fragment (end of tree).

- `pt_prev(c)` moves the cursor to the previous line fragment. It returns
	`PT_OK` on success, `PT_ERRPARAM` for a null cursor, or `PT_ERREMPTY` when
	there is no previous fragment (start of tree).

- `pt_advance(c, delta)` moves the cursor by a byte offset.
	- `delta == 0` is a no-op and returns `PT_OK`.
	- Advancing past the end clamps to the end position (`off == bytes`).
	- Moving before the start clamps to the beginning (`off == 0`).
	- Returns `PT_ERRPARAM` if `c` is null.

- `pt_advline(c, delta)` moves the cursor by line count (`delta` can be
	positive or negative) and lands at the **beginning of the target line**
	(column `0`).
	If you need to keep/set a specific column after line movement, call
	`pt_setcol` explicitly (for example: `pt_advline` then `pt_setcol`).

- `pt_setcol(c, column)` sets the cursor column within the current line.
	`column` is a target column (not a delta). If the target exceeds line length,
	the position is clamped to line end.

#### Lazy fields
`pt_Cursor` caches some computed values:
- `linecol.col` is lazily computed; a value of `-1` means “unknown”. It is
	recomputed on demand by `pt_linepos()`.
- `remain` is lazily computed; a value of `-1` means “unknown”. It is
	recomputed on demand by `pt_remainbytes()`.

Navigation calls (`pt_next`, `pt_prev`, `pt_advance`, `pt_advline`, `pt_setcol`) may
invalidate these caches by setting them back to `-1`.

### Zero-copy content model
- `pt_insert` **does not copy** the input bytes. It only records the pointer/length. The caller must ensure the input memory remains valid for as long as any blob referencing it is alive.
- `pt_literal` returns writable memory owned by the library for building content in-place.
	It may return fewer bytes than requested by updating `*plen` to the
	actual size granted.
- `pt_replace` replaces current piece content with new content. It may split current piece if necessary. It **does not** copy the input bytes. It only replace the current place pointer and length with the new one. The caller must ensure the input memory remains valid for as long as any blob referencing it is alive.

### Cursor editing state
- A cursor is positioned by `pt_locate` or `pt_locline` against a blob.
- The first `pt_insert`/`pt_remove`/`pt_replace` on a cursor automatically creates a **new internal blob** and advances its version.
- Subsequent edits on the same cursor continue to modify that internal blob until `pt_commit` or `pt_rollback` is called.

### Edit failure semantics (non-atomic)
- Edit operations are **not atomic** under memory pressure.
- If an edit returns `PT_ERRMEM`, any portion already inserted/removed **remains visible** and the data structure stays consistent.
- The cursor remains valid and is left at a legal tree position (typically the last successfully edited location).

### Commit / rollback semantics
- If the cursor **has no pending edits**:
	- `pt_commit` returns the current blob (owned reference).
	- Internally this means `pt_commit` must retain once before returning,
	  even when no new blob is created.
	- `pt_rollback` is a no-op.
- If the cursor **has pending edits**:
	- `pt_commit` returns the internal new blob (owned reference), then clears the pending state and sets the cursor’s current blob to the committed one.
	- `pt_rollback` discards the internal new blob and restores the cursor to the pre-edit blob.
- Ownership rule after `pt_commit`:
	- The returned blob is already owned by the caller (no extra
	  `pt_retain` needed).
	- If you passed an existing blob into the cursor, release that previous
	  blob when it is replaced by the committed one.

### Versioning
- `pt_version(b)` returns the blob version number.
- The internal auto-blob created by the first edit on a cursor increments the version (typically parent version + 1).

### Initialization helpers
- `pt_empty(state)` returns an empty blob (owned reference).
- `pt_from(state, ptr, len)` creates a blob referencing external memory without copying.

## License

Same as Lua 5.3 (MIT License). See `LICENSE` for details.