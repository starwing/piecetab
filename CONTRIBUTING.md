# Contributing to piecetab

Thanks for your interest in contributing! This project follows a strict set
of coding conventions. Please read this document before submitting changes.

## Build & Test

All build/test commands go through [`just`](https://github.com/casey/just):

```sh
just lc          # linecache tests (ASan/UBSan, FANOUT=4)
just lc8         # linecache tests with FANOUT=8
just pt          # piecetab tests
just lc-cov      # coverage build + report for linecache.h
just pt-cov      # coverage build + report for piecetab.h
just clean       # remove generated files
```

Run a subset by prefix: `just pt insert` runs all tests starting with
`insert`. Never invoke `gcc` or test binaries directly — always go through
`just` so flags stay consistent.

Both `linecache.h` and `piecetab.h` maintain **100% line coverage** and
~90% branch coverage. Changes must not regress coverage; add tests to
`tests/lc_tests.h` / `tests/pt_tests.h` alongside code changes. Never delete
existing tests.

## Coding Rules

- **C89 only** — no C99/C11 features.
- **Functions: 25 lines soft limit, 30 lines hard limit.** Simplify control
  flow before splitting; never split merely to satisfy the line count. Split
  by cohesive logic, not arbitrary line boundaries.
- **Function signatures must fit in 79 columns** — no wrapping. Too many
  parameters means the design needs rework.
- **No `goto` / labels.**
- **Assert invariants** instead of adding runtime checks. Internal `static`
  helpers must not add defensive parameter validation — use `assert` for the
  preconditions they rely on.
- **Avoid unnecessary casts** — cast only to silence a legitimate compiler
  warning.
- Consolidate duplicated logic into shared helpers when it improves clarity.
- Extract helpers at the right abstraction level: low-level primitives are
  preferred over high-level policy splits.
- Fix root causes, not symptoms. A workaround is equivalent to "not fixed"
  in review. Prefer data flow that is naturally correct after an operation
  over post-hoc fixups (no save-original-then-restore patterns).
- Run `clang-format` (config in `.clang-format`) after every change.

## Naming Conventions (strict)

| Kind            | Pattern                | Example                                    |
| --------------- | ---------------------- | ------------------------------------------ |
| Public API      | `lc_name`              | `lc_scan`, `lc_markbreak`                  |
| Public type     | `lc_Name`              | `lc_State`, `lc_Cache`                     |
| Internal func   | `lcX_name`             | `lcB_scanempty` (not ~~`lcB_scan_empty`~~) |
| Internal type   | `lc_Name` / `lcX_Name` | `lcB_Item` (not ~~`lcB_item`~~)            |
| Macro           | `lcX_name`             | `lcK_levels`                               |

- `X` is a single-letter category code: `K` = cursor, `B` = insert,
  `D` = delete, `M` = metrics, `N` = node, `L` = leaf.
- The `name` part of internal identifiers must **never** contain
  underscores.
- `piecetab.h` follows the same rules with the `pt` prefix: internal
  `ptX_name`, public `pt_name`.
- Local variables and function parameters must not contain underscores;
  struct fields may.

## Documentation

- `notes/` contains design documents and algorithm write-ups — read
  `notes/brief_linecache.md` and `notes/brief_piecetab.md` for a project
  overview before diving into the code.
- Update comments only when necessary; detailed semantics belong in
  `README.md` / `docs/`.

## Testing Notes

- Tests run with tiny fanout (`LC_FANOUT=4`, `PT_FANOUT=4`) to force tree
  splits early.
- Prefer `assert_tree` shape matching over loose predicates so structural
  regressions surface immediately.
- Debugging: add temporary `fprintf(stderr, ...)` (or `lc_log`/`pt_log`)
  rather than reaching for a debugger. See `notes/guide_debug.md`.
