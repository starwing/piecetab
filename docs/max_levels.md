# MAX_LEVELS safety reference

`PT_MAX_LEVEL`, `LC_MAX_LEVEL`, and `SP_MAX_LEVEL` control the size of the
cursor path array (`paths[]`) and the scratch-node arrays (`rt[]`). They are
compile-time configuration macros; define them before including the header
that implements the library.

If `MAX_LEVELS` is too small, a deep tree can write past the end of `paths[]`
when the root is split or a new chain is built. The table below gives the
smallest `MAX_LEVELS` that cannot overflow for any tree whose piece/line/span
count is representable in `size_t` (`SIZE_MAX`), assuming the worst case where
every node is at the minimum allowed occupancy (`FANOUT/2`).

## Formula

For piecetab and spantree the leaf and internal nodes share the same fanout:

```
MAX_LEVELS = ceil(log_b(SIZE_MAX))
b          = FANOUT / 2   (integer floor)
```

For linecache the leaf fanout is independent:

```
MAX_LEVELS = ceil(log_b(SIZE_MAX / lf)) + 1
b           = LC_FANOUT / 2         (integer floor)
lf          = LC_LEAF_FANOUT / 2    (integer floor)
```

When `LC_LEAF_FANOUT == LC_FANOUT`, the linecache value matches the piecetab /
spantree table below.

## Safe defaults

The current library defaults are:

- `PT_FANOUT = 31` → `PT_MAX_LEVEL = 17` (64-bit) / `9` (32-bit)
- `LC_FANOUT = LC_LEAF_FANOUT = 62` → `LC_MAX_LEVEL = 13`
- `SP_FANOUT = 34` → `SP_MAX_LEVEL = 16`

The 64-bit `PT_MAX_LEVEL = 17` is safe on both 32-bit and 64-bit builds
(the 32-bit safe value is only 9, so 17 is still small).

| FANOUT | b = FANOUT/2 | 32-bit MAX_LEVELS | 64-bit MAX_LEVELS |
| ------ | ------------ | ----------------- | ----------------- |
| 4  | 2  | 32 | 64 |
| 5  | 2  | 32 | 64 |
| 6  | 3  | 21 | 41 |
| 7  | 3  | 21 | 41 |
| 8  | 4  | 16 | 32 |
| 9  | 4  | 16 | 32 |
| 10 | 5  | 14 | 28 |
| 11 | 5  | 14 | 28 |
| 12 | 6  | 13 | 25 |
| 13 | 6  | 13 | 25 |
| 14 | 7  | 12 | 23 |
| 15 | 7  | 12 | 23 |
| 16 | 8  | 11 | 22 |
| 17 | 8  | 11 | 22 |
| 18 | 9  | 11 | 21 |
| 19 | 9  | 11 | 21 |
| 20 | 10 | 10 | 20 |
| 21 | 10 | 10 | 20 |
| 22 | 11 | 10 | 19 |
| 23 | 11 | 10 | 19 |
| 24 | 12 | 9  | 18 |
| 25 | 12 | 9  | 18 |
| 26 | 13 | 9  | 18 |
| 27 | 13 | 9  | 18 |
| 28 | 14 | 9  | 17 |
| 29 | 14 | 9  | 17 |
| 30 | 15 | 9  | 17 |
| 31 | 15 | 9  | 17 |
| 32 | 16 | 8  | 16 |
| 33 | 16 | 8  | 16 |
| 34 | 17 | 8  | 16 |
| 35 | 17 | 8  | 16 |
| 36 | 18 | 8  | 16 |
| 37 | 18 | 8  | 16 |
| 38 | 19 | 8  | 16 |
| 39 | 19 | 8  | 16 |
| 40 | 20 | 8  | 15 |
| 41 | 20 | 8  | 15 |
| 42 | 21 | 8  | 15 |
| 43 | 21 | 8  | 15 |
| 44 | 22 | 8  | 15 |
| 45 | 22 | 8  | 15 |
| 46 | 23 | 8  | 15 |
| 47 | 23 | 8  | 15 |
| 48 | 24 | 7  | 14 |
| 49 | 24 | 7  | 14 |
| 50 | 25 | 7  | 14 |
| 51 | 25 | 7  | 14 |
| 52 | 26 | 7  | 14 |
| 53 | 26 | 7  | 14 |
| 54 | 27 | 7  | 14 |
| 55 | 27 | 7  | 14 |
| 56 | 28 | 7  | 14 |
| 57 | 28 | 7  | 14 |
| 58 | 29 | 7  | 14 |
| 59 | 29 | 7  | 14 |
| 60 | 30 | 7  | 14 |
| 61 | 30 | 7  | 14 |
| 62 | 31 | 7  | 13 |
| 63 | 31 | 7  | 13 |
| 64 | 32 | 7  | 13 |

Quick rules:

- 64-bit `size_t`: the piecetab default `PT_FANOUT = 31` needs
  `PT_MAX_LEVEL = 17`; the linecache default `FANOUT = 62` needs
  `LC_MAX_LEVEL = 13`; the spantree default `SP_FANOUT = 34` needs
  `SP_MAX_LEVEL = 16`.
- 32-bit `size_t`: the defaults are also safe (`PT_FANOUT = 31` needs 9,
  `FANOUT = 62` needs 7, `SP_FANOUT = 34` needs 8); use the table when
  overriding to smaller fanouts.

## How to use

- If you keep the library defaults, you do not need to define any
  `MAX_LEVELS`; the built-in defaults are safe.
- If you override `PT_FANOUT`, `LC_FANOUT`, `LC_LEAF_FANOUT`, or `SP_FANOUT`
  to a lower value, define the corresponding `MAX_LEVELS` to at least the
  table value for your `size_t` width.
- Example: a 64-bit build with `PT_FANOUT = 4` must use
  `-DPT_MAX_LEVEL=64`.

The tree code also contains `assert(levels < MAX_LEVELS)` immediately after
every level-increasing operation, so debug builds will catch an undersized
`MAX_LEVELS` before the path array is corrupted.
