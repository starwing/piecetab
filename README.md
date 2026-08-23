![Piecetab](misc/logo.svg)

**Lightweight, stb-style single-header C89 libraries for building high-performance text editor buffers**

**English** | [中文](README.zh.md)

<div align="center">

[![Build](https://github.com/starwing/piecetab/actions/workflows/test.yml/badge.svg)](https://github.com/starwing/piecetab/actions/workflows/test.yml) [![Coverage Status](https://coveralls.io/repos/github/starwing/piecetab/badge.svg?branch=master)](https://coveralls.io/github/starwing/piecetab?branch=master)

[Overview](#overview) • [Features](#features) • [Quick Start](#quick-start) • [API](#api-overview) • [Lua](#lua-bindings) • [Testing](#testing)

</div>

## Overview

The core libraries are:

- **`piecetab.h`** — a byte-level piece table backed by a B+ tree, with
  copy-on-write snapshots, transactional editing, and zero-copy reads.
- **`linecache.h`** — a metric B+ tree mapping byte offsets to line
  numbers, maintaining a line-number cache under heavy edits.
- **`undotree.h`** — a version tree + edit journal + diff service based on
  interval algebra, riding on top of `pt_Buffer` COW snapshots.
- **`spantree.h`** — a span tree for byte-attribute coloring: a B+ tree
  storing full-coverage `(len, id)` runs for final rendered styles, with
  namespace masks, an arbiter callback, and edit-sync operations.

These libraries are independent and composable: piecetab stores bytes
("clean octets" — no line or encoding awareness), linecache tracks line
breaks, undotree manages the version graph and computes diffs between any
two versions, and spantree stores final rendered color spans (full-coverage
byte-attribute runs). Combine the first three to get a full editor buffer
with O(log n) offset ↔ line navigation and undo/redo; spantree layers on
top for syntax highlighting, diagnostics, and other byte-attribute styling.

Peripheral libraries extend the core toward a full editor:

- **`cellgrid.h`** — a screen buffer with a diff layer: grid cells,
  scroll/move/fill primitives, and a redraw-diff driver for efficient
  screen updates.
- **`termfeed.h`** — a libtermkey-style terminal input state machine:
  raw bytes to decoded keys (CSI/SS3, UTF-8, alt keys, mouse, OSC52).

All of them follow the same stb-style layout: single-header C89
implementation, a Lua binding in `lua/`, and a test file in `tests/`.

## Performance

Current tuned defaults (100k structural corpus, ns/op, lower is better):

| Operation       | piecetab.h | linecache.h | spantree.h |
| --------------- | ---------: | ----------: | ---------: |
| seek            |      15.58 |       16.43 |      11.40 |
| locate          |      11.78 |       11.21 |      11.74 |
| advance         |      6.800 |       6.797 |      6.062 |
| splice          |      120.8 |       16.36 |      28.04 |
| next (per item) |      2.281 |           - |      2.383 |
| edit            |      100.7 |           - |          - |
| scan (per line) |          - |       0.964 |          - |
| fill            |          - |           - |      238.7 |

## AI Usage
- All implementations (stb headers) are written by hand; AI was used to find solutions, help with design, and generate documentation.
- All tests are written by AI and reviewed by humans, used to verify the correctness of the implementation and to ensure that the code meets the requirements and specifications.
- `editor.lua` is written by AI, used to demonstrate the usage of the library, and to provide a reference for developers who want to use the library in their own projects.

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
  into a new buffer, `pt_rollback` discards it and returns the source buffer
  (retained for the caller). Both detach the cursor (`C->tree = NULL`);
  re-attach with `pt_seek` on the returned buffer
- **Compacting freeze**: `pt_commit` merges physically adjacent literals
  produced by freezing contiguous holes and rebalances the tree, so long
  typing runs collapse into single pieces instead of fragmenting the tree
- **Two piece kinds**: zero-copy *literal* pieces referencing user memory,
  and pooled mutable *hole* pieces absorbing small edits in place
- **Transactional OOM safety**: edits pre-reserve pool objects; on
  `PT_ERRMEM` the structure stays consistent and the cursor stays valid
- **Arena-backed literals**: `pt_reserve` / `pt_scratch` / `pt_literal`
  write bytes directly into the tree's arena without an extra copy
- **Generational compaction**: each edit generation owns its arena;
  `pt_compact` produces a fresh standalone buffer — bytes owned by the old
  generations are copied into a compact new arena, external memory (e.g. a
  large mmap) is referenced as-is, and releasing the old chain reclaims all
  of its memory

### linecache.h

- **Metric B+ tree**: byte offsets and line breaks are double-counted per
  subtree, enabling O(log n) navigation in both directions
- **Bulk loading**: `lc_scan` builds the tree bottom-up from a scanner
  callback, far cheaper than per-line insertion
- **Full editing**: single break insert (`lc_markbreak`), range delete
  (`lc_remove`), splice (`lc_splice`), and mid-tree text insertion
  (`lc_insert` / `lc_append`) with full OOM rollback

### undotree.h

- **Version graph**: tree of immutable snapshots (`ut_Node`), each carrying a
  changeset (hunk list) from its parent and an opaque payload (e.g. `pt_Buffer`)
- **Edit journal**: uncommitted edits stored as `(off, del, ins)` triples,
  normalised into a hunk list on commit
- **Hunk algebra**: compose (X→Y ∘ Y→Z → X→Z), invert, and normalise operations
  on interval-change hunks
- **Fresh-vid protocol**: `ut_freshvid(S)` sentinel represents the
  uncommitted state; `ut_diff(from, to)` handles any combination of
  committed versions + fresh endpoints via four-phase compose

### spantree.h

- **Full-coverage span model**: stores final rendered coloring as
  `(len > 0, attr id)` runs, so the renderer does zero style composition
- **Arbiter single layer**: writes go through `sp_Arbiterf`, giving an
  external blending/namespace policy while the tree stays format-agnostic
- **Namespace masks**: per-node `sp_Mask` aggregation enables pruned
  `sp_next`/`sp_prev`/`sp_clear` by namespace (`sp_addns`/`sp_delns`)
- **Edit-sync operations**: `sp_splice`/`sp_append`/`sp_insert`/`sp_remove`
  keep spans aligned with text edits, with deterministic gravity
  (append inherits left, insert inherits right)
- **Sparse coloring for free**: uncolored bytes are a single large id-0
  span; no special tree-level offset mechanism needed

## Quick Start

All headers are stb-style: include the header anywhere, define the
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

### spantree.h

```c
#define SP_IMPLEMENTATION
#include "spantree.h"

/* arbiter: external blending policy; here, keep the new id */
static sp_Id keep_new(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud; (void)old; (void)mask;
    return id;
}

int main(void) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *T = sp_newtree(S);
    sp_Cursor C;

    sp_setarbiter(T, keep_new, NULL);
    sp_seek(&C, T, 0);
    sp_fill(&C, 1, 3);            /* bytes 0..3 get attr id 1 */
    sp_append(&C, 2);             /* insert 2 bytes at cursor  */
    /* sp_bytes(T) == 5 */

    sp_freetree(T);
    sp_close(S);
    return 0;
}
```

### editor.lua

![editor.lua demo](misc/demo.svg)

#### Intro

`editor.lua` is an AI-written modal editor demo that wires the libraries
together: a piecetab/linecache buffer (`pt.doc`), a cellgrid screen
buffer, termfeed terminal input, and spantree-styled coloring. `Ed.new(content?,
term?, grid?)` builds an editor from a string; `Ed.open(filename, term?,
grid?)` loads a file; both accept injected term/grid objects (tests use
fakes). It also serves as a C-module incubation ground — helpers marked
`TODO(C)` (char motion, column math) are candidates for promotion into C.

Syntax highlighting: files opened with a `.c`/`.h`/`.lua` extension get
tree-sitter highlighting (keyword/string/comment/function styles) via the
`treesitter` Lua binding (see `lua/treesitter.c`). `Ed:open_language(lang)`
enables it manually. Editing updates highlights incrementally.

#### Prerequisites / modules

- **Lua**: Lua 5.5 is the primary runtime; LuaJIT is also supported for
  compatibility. The demo finds modules relative to the repo root:
  `./lua/?.so`, `./lua/luajit/?.so`, and `./lua/?.lua`.
- **Required C modules**: `piecetab`, `cellgrid`, `termfeed`, `spantree`,
  and `json` (yyjson binding). `json` is required by `lsp.lua`, which
  `editor.lua` loads unconditionally.
- **Optional `treesitter`**: `pcall(require, "treesitter")` is used, so the
  demo runs without it (highlighting is disabled). Full highlighting needs
  `libtree-sitter` and compiled grammars in `lua/grammar`.
- **`lsp.lua`**: pure-Lua LSP client; requires the `json` binding and `luv`.
  LSP is started automatically when a server is configured for the file.
- **Tests only**: `luaunit` for the Lua test harness; the `lua-utf8` rock
  (PUC Lua only) and `tmux` for some editor/cellgrid/display tests.

#### Install / build

From the repo root:

```sh
just lua/deps # builds required C modules only (for running the demo)
just lua/ed   # builds required C modules and runs the editor unit tests
```

The `deps` recipe compiles `piecetab`, `cellgrid`, `termfeed`, `json`, and
`spantree` into `lua/*.so` (PUC Lua) without running tests. For interactive
use you only need those `.so` files present; you can build them individually
with `just lua/json`, `just lua/sp`, etc. LuaJIT variants are produced by the
generic `just lua/build <name>` recipes.

`lua/tests/editor_test.lua` treats tree-sitter as optional: if the binding
or the C grammar is missing, the syntax-highlighting tests are skipped, so
`just lua/ed` passes after a clean checkout/build. Use `just lua/build-ts` to
add tree-sitter support for highlighted demos.

#### Run method

```sh
lua editor.lua [file]
```

Run from the repo root after building the modules. Omit `[file]` to start
with an empty buffer; pass a path to edit that file. The script enters a
raw-mode terminal session; use `:q` or `:wq` to exit.

```lua
local Ed = require("editor")

local e = Ed.open("file.txt")            -- or Ed.new("hello\nworld")
e:keymap("normal", "G", function(self)
  self.doc:seek("line", self.doc:breaks() - 1)
end)
e:command("hello", function(self, arg, bang)
  self.msg = "hello, " .. arg
end)
```

Custom keys/commands hook into the per-mode registries (`mode` is
`"normal"` / `"insert"` / `"command"`). Built-in keys: `h/j/k/l`, `w/b`,
`0/$`, `gg/G`, `x`, `dd`, `i/a/o/O`, `u`/`<C-r>`, `:`; commands:
`:w`, `:q`, `:wq`, `:e`.

Run the unit tests with `just lua/ed`; smoke-test interactively with
`lua editor.lua [file]`.

#### Syntax highlighting troubleshooting

The demo treats `treesitter` as optional, so missing highlighting is a
silent fallback rather than an error. If keywords such as `local`, `if`,
and `return` are not colored, tree-sitter did not load. On macOS the
native modules must be Mach-O binaries built for this machine;
`lua/treesitter.so`, `lua/luajit/treesitter.so`, and `lua/grammar/*.so`
copied from Linux/CI will be ELF binaries and Lua cannot load them.

Rebuild the host-native modules from the repo root (`just lua/ts` also
fetches and compiles the grammars):

```sh
just lua/ts   # fetch/compile lua/grammar/*.so, build treesitter.so + luajit/treesitter.so, test
```

## API Overview

### piecetab.h

| Category  | Functions                                                                           |
| --------- | ----------------------------------------------------------------------------------- |
| Lifecycle | `pt_open`, `pt_close`, `pt_reset`, `pt_getallocf`                                   |
| Buffer    | `pt_empty`, `pt_from`, `pt_compact`, `pt_retain`, `pt_release`                      |
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
| Position  | `lc_offset`, `lc_line`, `lc_col`, `lc_lineoffset`, `lc_linelen`                      |
| Edit      | `lc_markbreak`, `lc_clearbreaks`, `lc_remove`, `lc_splice`, `lc_insert`, `lc_append` |

### undotree.h

| Category  | Functions                                                               |
| --------- | ----------------------------------------------------------------------- |
| Lifecycle | `ut_open`, `ut_close`, `ut_setcleaner`                                  |
| Tree      | `ut_newtree`, `ut_deltree`                                              |
| Journal   | `ut_record`, `ut_unrecord`, `ut_freshcount`, `ut_discard`               |
| Version   | `ut_commit`, `ut_switch`                                                |
| Navigate  | `ut_root`, `ut_current`, `ut_parent`, `ut_payload`, `ut_childcount`     |
| Navigate  | `ut_firstchild`, `ut_lastchild`, `ut_nextsib`, `ut_younger`, `ut_older` |
| Navigate  | `ut_ancestor`                                                           |
| Diff      | `ut_freshvid`, `ut_diff`, `ut_freshdiff`, `ut_hunks`, `ut_mapoffset`    |

### spantree.h

| Category  | Functions                                           |
| --------- | --------------------------------------------------- |
| Lifecycle | `sp_open`, `sp_close`                               |
| Tree      | `sp_newtree`, `sp_freetree`, `sp_bytes`             |
| Blending  | `sp_setarbiter`, `sp_addns`, `sp_delns`, `sp_hasns` |
| Cursor    | `sp_seek`, `sp_locate`, `sp_advance`, `sp_offset`   |
| Marking   | `sp_fill`, `sp_clear`                               |
| Reading   | `sp_next`, `sp_prev`, `sp_style`                    |
| Edit      | `sp_splice`, `sp_append`, `sp_insert`, `sp_remove`  |

See [`docs/piecetab.md`](docs/piecetab.md),
[`docs/linecache.md`](docs/linecache.md),
[`docs/undotree.md`](docs/undotree.md), and
[`docs/spantree.md`](docs/spantree.md) for the full API references.

## Lua Bindings

Each C library has a Lua binding in `lua/` (`name.c` plus `name.d.lua`
type declarations). There are also two pure-Lua / meta-only modules and a
vendored JSON binding:

| Module       | Source / files                                | Description                                                                            |
| ------------ | --------------------------------------------- | -------------------------------------------------------------------------------------- |
| `piecetab`   | `lua/piecetab.c`, `lua/piecetab.d.lua`        | C binding for `piecetab.h` (buffers, cursors, documents)                               |
| `cellgrid`   | `lua/cellgrid.c`, `lua/cellgrid.d.lua`        | C binding for `cellgrid.h` (screen grid + diff)                                        |
| `termfeed`   | `lua/termfeed.c`, `lua/termfeed.d.lua`        | C binding for `termfeed.h` (terminal input)                                            |
| `spantree`   | `lua/spantree.c`, `lua/spantree.d.lua`        | C binding for `spantree.h` (Compositor/Tree/Cursor span coloring)                      |
| `json`       | `lua/json.c`, `lua/json.d.lua`, `lua/yyjson/` | Pure C binding over vendored yyjson (`decode`/`encode`/`array`/`object`/`null`/`type`) |
| `treesitter` | `lua/treesitter.c`, `lua/treesitter.d.lua`    | C binding over `libtree-sitter` (parser/tree/query APIs)                               |
| `lsp`        | `lua/lsp.lua`                                 | Pure-Lua LSP client building blocks; requires `json` and `luv`                         |
| `lua-utf8`   | `lua/lua-utf8.d.lua`                          | Type declarations only (meta) for the `lua-utf8` rock                                  |

Build/test recipes live in `lua/justfile` and are run as `just lua/<name>`
(e.g. `just lua/json`, `just lua/sp`, `just lua/ts`). See the
[`editor.lua`](#editorlua) section for the demo's module requirements.

## Configuration

Override before including the implementation:

| Macro                                                             | Default | Meaning                                                                          |
| ----------------------------------------------------------------- | ------- | -------------------------------------------------------------------------------- |
| `PT_FANOUT`                                                       | 31      | max children per piecetab node                                                   |
| `LC_FANOUT`                                                       | 16      | max children per linecache node                                                  |
| `SP_FANOUT`                                                       | 34      | max children per spantree node                                                   |
| `LC_LEAF_FANOUT`                                                  | 34      | max lines per leaf                                                               |
| `PT_MAX_HOLESIZE`                                                 | 64      | hole piece capacity                                                              |
| `PT_MAX_LEVEL`                                                    | 17      | max tree depth / cursor path size (see [docs/max_levels.md](docs/max_levels.md)) |
| `LC_MAX_LEVEL`                                                    | 21      | max tree depth / cursor path size (see [docs/max_levels.md](docs/max_levels.md)) |
| `SP_MAX_LEVEL`                                                    | 16      | max tree depth / cursor path size (see [docs/max_levels.md](docs/max_levels.md)) |
| `PT_PAGE_SIZE` / `LC_PAGE_SIZE` / `UT_PAGE_SIZE` / `SP_PAGE_SIZE` | 65536   | pool allocator page size                                                         |
| `PT_ARENA_SIZE`                                                   | 1024    | arena block minimum size                                                         |
| `PT_COMPACT_RANGES`                                               | 64      | compact range array initial capacity                                             |

All libraries accept a custom allocator (`lc_Alloc` / `pt_Alloc`
/ `ut_Alloc` / `sp_Alloc`, Lua-style realloc signature) at `*_open`.

## Repository Layout

- `*.h` — stb-style single-header libraries (pure C89, self-contained):
  `piecetab.h`, `linecache.h`, `undotree.h`, `spantree.h`, `cellgrid.h`,
  `termfeed.h`
- `lua/` — Lua side: one binding `name.c` + API declaration `name.d.lua`
  per library, the `editor.lua` demo, and `tests/` with Lua tests.
  Bindings build to `lua/*.so` (Lua 5.5, primary) and `lua/luajit/*.so`
  (LuaJIT, compat)
- `tests/` — C tests: one `*_test.c` per library; `tests.h` (shared
  runner + asserts), `gen_entries.lua` (test-entry generator),
  `lc_tests.h` (linecache-specific helpers shared by both fanout
  variants)
- `docs/`, `notes/` — API reference docs and design records

## Documentation

- [`docs/piecetab.md`](docs/piecetab.md) — piecetab API reference &
  implementation notes ([中文](docs/piecetab.zh.md))
- [`docs/linecache.md`](docs/linecache.md) — linecache API reference &
  implementation notes ([中文](docs/linecache.zh.md))
- [`docs/undotree.md`](docs/undotree.md) — undotree API reference &
  integration guide ([中文](docs/undotree.zh.md))
- [`docs/spantree.md`](docs/spantree.md) — spantree API reference &
  implementation notes ([中文](docs/spantree.zh.md))

Peripheral libraries (`cellgrid.h`, `termfeed.h`) have no API docs yet —
see their `lua/*.d.lua` declarations and `notes/design_cellgrid.md`,
`notes/design_termfeed.md`.

- [`notes/`](notes/) — design documents: architecture overviews
  (`brief_*.md`), algorithm designs (`design_*.md`), and the range-delete
  algorithm evolution history

## Testing

Tests run with tiny fanout (4) under ASan/UBSan to force tree splits, plus
coverage builds via lcov. All libraries maintain **100% line / function
coverage** and ~90% branch coverage.

```sh
# C tests (one runner per lib)
just lc     # linecache tests
just pt     # piecetab tests
just ut     # undotree tests
just sp     # spantree tests
just cg     # cellgrid tests
just tf     # termfeed tests
just cov    # coverage report
just sp-cov # spantree coverage report
just sp-lines  # spantree uncovered lines
just sp-unbranched  # spantree uncovered branches

# Lua binding tests — just lua/<recipe> runs lua/justfile
just lua/pt  # piecetab binding (also lua/cg, lua/tf)
just lua/sp  # spantree binding
just lua/json  # yyjson binding
just lua/lsp  # pure-Lua LSP client tests (builds json)
just lua/ed  # editor.lua unit tests
just lua/ed-tmux  # editor display tests (needs tmux)
just lua/ts  # treesitter binding tests
just lua/ts-cov  # treesitter binding coverage
just lua/ts-lines  # treesitter uncovered lines
```

Dependencies: `libtree-sitter` (homebrew `tree-sitter`) for the
treesitter binding; grammars are fetched and compiled by
`misc/fetch_grammars.sh` (run by `just lua/ts-grammars`).

See [CONTRIBUTING.md](CONTRIBUTING.md) for coding conventions.

## Benchmarking

The `bench/` tree contains a C89 benchmark harness for the piecetab library
family. It uses public-API cases, deterministic structural corpora
(piece/line/span counts, not file bytes), and JSON output.

```sh
just bench/all     # Print the current benchmark summary table
just bench/smoke   # Quick smoke run
just bench/sweep   # Full PT_FANOUT sweep 4..63 (64 excluded: all-ones mask (64) is UB)
just bench/confirm # Focused multi-seed confirmation sweep
just bench/plot    # Plot JSON results (needs matplotlib; falls back to CSV/Markdown)
```

Set `FANOUTS` and `BENCH_ROUNDS` to run a subset, e.g.
`FANOUTS="16 24 31" BENCH_ROUNDS=5 just bench/sweep`. Raw JSON goes to
`bench/results/`, plots and summaries to `bench/reports/`. The tuning report
is `notes/reports/bench_tuning_pt.md` (local/gitignored); see
`notes/design_bench.md` for the design.

## License

[MIT](LICENSE), same as Lua.
