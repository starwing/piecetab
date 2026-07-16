# piecetab

<!-- BADGES: add badges here -->

**English** | [中文](README.zh.md)

Two lightweight, stb-style single-header C89 libraries for building
high-performance text editor buffers:

- **`piecetab.h`** — a byte-level piece table backed by a B+ tree, with
  copy-on-write snapshots, transactional editing, and zero-copy reads.
- **`linecache.h`** — a metric B+ tree mapping byte offsets to line
  numbers, maintaining a line-number cache under heavy edits.

The two libraries are independent and composable: piecetab stores bytes
("clean octets" — no line or encoding awareness), linecache tracks line
breaks. Combine them to get a full editor buffer with O(log n) offset ↔
line navigation.

## Motivation

This project is driven by the need for a **high-performance, low-latency
text buffer** that remains predictable under heavy edits, large files, and
complex content:

- Stable performance under insert/delete workloads
- Cheap snapshots for undo/redo and asynchronous consumers
- A compact, single-header implementation suitable for embedding

## Features

### piecetab.h

- **Immutable buffers + COW**: `pt_Buffer` is a refcounted snapshot; the first
  edit on a cursor forks a private transient tree, `pt_commit` freezes it
  into a new buffer, `pt_rollback` discards it
- **Two piece kinds**: zero-copy *literal* pieces referencing user memory,
  and pooled mutable *hole* pieces absorbing small edits in place
- **Transactional OOM safety**: edits pre-reserve pool objects; on
  `PT_ERRMEM` the structure stays consistent and the cursor stays valid
- **Arena-backed literals**: `pt_reserve` / `pt_scratch` / `pt_literal`
  write bytes directly into the tree's arena without an extra copy

### linecache.h

- **Metric B+ tree**: byte offsets and line breaks are double-counted per
  subtree, enabling O(log n) navigation in both directions
- **Bulk loading**: `lc_scan` builds the tree bottom-up from a scanner
  callback, far cheaper than per-line insertion
- **Full editing**: single break insert (`lc_markbreak`), range delete
  (`lc_remove`), splice (`lc_splice`), and mid-tree text insertion
  (`lc_insert` / `lc_append`) with full OOM rollback

## Quick Start

Both are stb-style: include the header anywhere, define the
`*_IMPLEMENTATION` macro in exactly one translation unit.

### piecetab.h

```c
#define PT_IMPLEMENTATION
#include "piecetab.h"

int main(void) {
    pt_State *S = pt_open(NULL, NULL);        /* default allocator */
    pt_Buffer src, out;
    pt_Cursor C;
    char      buf[32];
    size_t    n;

    src = pt_from(S, "hello world", 11);      /* zero-copy buffer */
    pt_seek(&C, src, 5);
    pt_insert(&C, ",", 1);                    /* reference semantics */
    out = pt_commit(&C);                      /* freeze into new buffer */

    pt_seek(&C, out, 0);
    n = pt_read(&C, buf, sizeof(buf));        /* "hello, world" */

    pt_release(src);
    pt_release(out);
    pt_close(S);
    return (int)n;
}
```

### linecache.h

```c
#define LC_IMPLEMENTATION
#include "linecache.h"
#include <string.h>

/* scanner returns the length of the next line (incl. '\n'), 0 to stop */
static unsigned scan(void *ud, size_t pos) {
    const char **s = (const char **)ud;
    const char  *nl = strchr(*s, '\n');
    unsigned     len;
    (void)pos;
    if (nl == NULL) return 0;
    len = (unsigned)(nl - *s) + 1;
    *s += len;
    return len;
}

int main(void) {
    const char *text = "one\ntwo\nthree\n";
    lc_State *S = lc_open(NULL, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    lc_scan(c, scan, &text);           /* bulk-load line breaks */
    lc_seekline(&C, c, 2);             /* line 2 starts at ...   */
    /* lc_offset(&C) == 8, lc_breaks(c) == 3 */

    lc_close(S);                       /* frees all caches */
    return 0;
}
```

## API Overview

### piecetab.h

| Category  | Functions                                                                           |
| --------- | ----------------------------------------------------------------------------------- |
| Lifecycle | `pt_open`, `pt_close`, `pt_reset`, `pt_getallocf`                                   |
| Buffer    | `pt_empty`, `pt_from`, `pt_retain`, `pt_release`                                    |
| Query     | `pt_bytes`, `pt_version`                                                            |
| Cursor    | `pt_seek`, `pt_locate`, `pt_advance`, `pt_offset`                                   |
| Read      | `pt_read`, `pt_piece`, `pt_next`, `pt_prev`                                         |
| Edit      | `pt_edit` (copy), `pt_insert` / `pt_append` / `pt_splice` / `pt_remove` (reference) |
| Txn       | `pt_commit`, `pt_rollback`                                                          |
| Arena     | `pt_reserve`, `pt_scratch`, `pt_literal`                                            |

Reference-semantics edits (`pt_insert` etc.) do **not** copy input bytes —
the caller must keep the memory alive while any buffer references it.
`pt_edit` copies into hole pieces (`len <= PT_MAX_HOLESIZE` per call).

### linecache.h

| Category  | Functions                                                                            |
| --------- | ------------------------------------------------------------------------------------ |
| Lifecycle | `lc_open`, `lc_close`, `lc_reset`                                                    |
| Cache     | `lc_newcache`, `lc_delcache`                                                         |
| Bulk      | `lc_scan`                                                                            |
| Query     | `lc_breaks`, `lc_bytes`                                                              |
| Cursor    | `lc_seek`, `lc_seekline`, `lc_locate`, `lc_locline`, `lc_advance`, `lc_advline`      |
| Query     | `lc_offset`, `lc_line`, `lc_col`, `lc_lineoffset`, `lc_linelen`                      |
| Edit      | `lc_markbreak`, `lc_clearbreaks`, `lc_remove`, `lc_splice`, `lc_insert`, `lc_append` |

See [`docs/piecetab.md`](docs/piecetab.md) and
[`docs/linecache.md`](docs/linecache.md) for the full API references.

## Configuration

Override before including the implementation:

| Macro                           | Default | Meaning                  |
| ------------------------------- | ------- | ------------------------ |
| `PT_FANOUT` / `LC_FANOUT`       | 62      | max children per node    |
| `LC_LEAF_FANOUT`                | 62      | max lines per leaf       |
| `PT_MAX_HOLESIZE`               | 64      | hole piece capacity      |
| `PT_MAX_LEVEL` / `LC_MAX_LEVEL` | 16      | max tree depth           |
| `PT_PAGE_SIZE` / `LC_PAGE_SIZE` | 65536   | pool allocator page size |
| `PT_ARENA_SIZE`                 | 1024    | arena block minimum size |

Both libraries accept a custom allocator (`lc_Alloc` / `pt_Alloc`,
Lua-style realloc signature) at `*_open`.

## Documentation

- [`docs/piecetab.md`](docs/piecetab.md) — piecetab API reference &
  implementation notes ([中文](docs/piecetab.zh.md))
- [`docs/linecache.md`](docs/linecache.md) — linecache API reference &
  implementation notes ([中文](docs/linecache.zh.md))
- [`notes/`](notes/) — design documents: architecture overviews
  (`brief_*.md`), algorithm designs (`design_*.md`), and the range-delete
  algorithm evolution history

## Testing

Tests run with tiny fanout (4) under ASan/UBSan to force tree splits, plus
coverage builds via lcov. Both headers maintain **100% line / function
coverage** and ~90% branch coverage.

```sh
just lc     # linecache tests
just pt     # piecetab tests
just cov    # coverage report
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for coding conventions.

## License

[MIT](LICENSE), same as Lua.
