# anchortree coverage exemptions

Status (coverage measured at `d0d9533`; later final-review fixes did not change `anchortree.h`, so the totals below remain current):
`just at-cov` now runs the fanout-4/5/7/8/9 runners plus the default-config
fanout-32 runner and reports 100% lines (719/719), 100% functions (82/82),
and 97.2% branches (451/464) for `anchortree.h`. Every line and function is
covered. 13 branch points on 13 distinct source lines remain uncovered:

    L261, L322, L361, L511, L582, L594, L609,
    L851, L865, L897, L918, L923, L1079

`just at-unbranched` prints the other 12 lines; L897's line contains an
`assert` token and is hidden by that recipe's assert filter. `just at-lines`
prints nothing. No `LCOV_EXCL` markers or weakened invariants are used.

Line references are to the current `anchortree.h`; branch sides below are
named by code semantics and cross-checked against `gcov -b -c` (fanout-4
runner) and `BRDA` records in `lcov.info`.

## Reserve-helper shortcut (L261)

L261 is `if (p->freed_obj) return atP_ralloc(p);` in `atP_alloc`; the
uncovered side is the condition true (`freed_obj != 0`).

`atP_alloc` has exactly one caller: `atP_reserve` (L276), whose loop
condition zeroes `p->freed_obj` immediately before every `atP_alloc` call
(`while (p->freed_obj = 0, (obj = atP_alloc(S, p)))`, L276). The shortcut's
true side is therefore unreachable by construction in this library.

Verification: `grep -n 'atP_alloc(' anchortree.h` shows only L258/L276;
gcov branch counts 0 (true) / 1444 (false). The same shortcut is kept in
`spantree.h`, `piecetab.h`, and `linecache.h` for reference parity instead
of being deleted here.

## Bucket-search miss (L322)

`atO_unlink` walks the same-side chain for the requested `{id, is_end}` slot
(L322). The uncovered side is branch 0: the loop head exits because
`p.id == 0`, i.e. the chain walk ended without finding the slot.

`atO_unlink` has exactly two callers, both in `at_delanchor` (L1168 and
L1170), and both pass the live anchor's own start/end slot. The slot is in
the chain by construction, and the assert at L327 documents the contract.

Verification: caller listing above; gcov branch 0 (walk-miss head exit)
taken 0 while the other condition sides are taken 432/18/414/86/346.

## 32-bit overflow guard (L361)

`atO_growids` caps `newcap` by `AT_ANCHOR_MAX` first (L360,
`AT_ANCHOR_MAX == 2^31`); the uncovered true side of
`if (newcap > (size_t)-1 / sizeof(at_Anchor)) return AT_ERRMEM;` is the
32-bit `size_t` overflow catch: on 32-bit,
`AT_ANCHOR_MAX * sizeof(at_Anchor)` exceeds `SIZE_MAX`, so the guard can
fire, while on the 64-bit test runs it cannot. It is a platform-width guard
(brief_refine section 33 allows platform branches) and is kept rather than
removed.

Verification: L360/L361 arithmetic; gcov branch 0 (return) taken 0 /
branch 1 taken 62583.

## Default allocator abort (L511)

L511 is `return (np = realloc(p, nsize)) ? np : (abort(), (void *)0);`; the
uncovered side is the ternary false side (`realloc` returned NULL, abort).
The test harness cannot inject failure into the default allocator; the
allocator-failure tests use the custom `oomcount_alloc` path, and a real
default-allocator OOM is defined by contract to abort.

Verification: gcov branch 0 taken 63920 / branch 1 taken 0.

## Bounds guaranteed by tree invariants (L582, L594, L609)

- L582: `atK_findleaf` walks child spans until the containing one is found
  (`for (i = 0; i < atN_cc(p) && *poff >= p->bytes[i]; ++i)`). The uncovered
  side is branch 0, `i < atN_cc(p)` false (the walk ran past the last child).
  The `assert(i < atN_cc(p))` at L584 documents the invariant. All callers
  pass a target inside an existing child span: `at_locate` returns before
  L666 when `pos >= T->bytes` (L662-664) and only then calls
  `atK_findleaf(C, 0, &pos)`; `atK_forwardoff` (L601) and
  `atK_backwardoff` (L617) pass the residual offset already located inside a
  child. `T->bytes == sum(leaf spans)` makes a containing child exist.
  Verification: caller listing; gcov branch 0 (past-the-end exit) taken 0,
  branch 1 taken 221583.

- L594: `atK_forwardoff` ascends levels for a target below `T->bytes`; the
  uncovered side is branch 0, `l >= 0` false (ran below the root). The
  cursor invariant guarantees a successor slot exists, so the loop breaks at
  a level `>= 0`; the assert at L600 (`l >= 0 && i < atN_cc(p)`) documents
  it. Verification: gcov branch 0 taken 0 / branch 1 taken 38492.

- L609: symmetric case in `atK_backwardoff`; every valid backward target has
  a predecessor slot, so the `l >= 0` false exit (branch 0) is not selected;
  the assert at L615 documents it. Verification: gcov branch 0 taken 0 /
  branch 1 taken 12.

These loops are verbatim with `spantree.h`.

## Macro-expansion phantom branches (L851, L865, L918, L1079)

`atK_parent(C, l)` (L228) expands to
`(l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root`. At these call sites the
macro is guarded by construction to `l > 0`, so only the ternary's false
(`l == 0`, root) side is never selected:

- L851 (`atI_splitat`) and L865 (`atI_pad`) recompute the level after
  `atI_makeroom`. `atI_makeroom` (L836-841) splits the root when every level
  is full and otherwise keeps `levels`, so `l = atK_levels(C) >= 1` at these
  sites. Verification: gcov branch counts L851 8408/0, L865 250434/0.
- L918 is `p = atK_parent(C, l);` inside the `for (; l > 0; --l)` loop of
  `atD_rebalance` (L917), so `l >= 1` on every execution. Verification: gcov
  branch counts 94677/0.
- L1079 (`atI_insertsplit`) is again after `atI_makeroom`.
  Verification: gcov branch counts 717/0.

The macro's `l == 0` side is therefore never selected; gcov records it as a
phantom branch attached to the macro-expansion line.

## Underfull-check early return (L897)

L897 is `if (assert(atN_cc(p) > 1), atN_cc(ns[0]) > atN_min()) return 0;` in
`atD_foldnode`. Coverage builds use `-DNDEBUG`, so the assert expands away
and the uncovered side is the true side of `atN_cc(ns[0]) > atN_min()` (the
early `return 0`).

`atD_foldnode` has exactly two callers:

- `atD_rebalance` L919-920: L919 returns when the cursor-path child is
  `>= atN_min()`, so L920 only calls `atD_foldnode` when that child is
  `< atN_min()`.
- root fold L923-924: guarded by `atN_cc(p->children[i]) < atN_min()`.

The sibling shift (`ns -= 1`) in L898 happens after L897, so `ns[0]` at L897
is the child the caller proved underfull. Hence `atN_cc(ns[0]) >
atN_min()` can never hold. Verification: caller listing; gcov branch 0
(early return) taken 0 / branch 1 taken 247288. The line is hidden by
`at-unbranched` because it contains an assert token.

## Root-fold second operand (L923)

`atD_rebalance` folds an underfull root child only when
`atN_cc(p->children[i]) < atN_min() && atN_cc(p) >= 2` (L923). The uncovered
side is the second operand's false side (`atN_cc(p) >= 2` false, i.e. a
single-child root). gcov counts: L923 is executed 88965 times, the first
comparison's true side 44597, the second comparison is evaluated exactly
44597 times with true side 44597 and false side 0; the fold body (L924) is
executed 44597 times.

Structural reason: the root-occupancy invariant. After every public
operation with `levels > 0`, the collapse loop at L925
(`while (atK_levels(C) && atN_cc(&C->tree->root) == 1)`) removes any
single-child root, so `atD_rebalance` is entered with at least two root
children. During a rebalance the only step that reduces the root child count
is a fold at level 1, and such a fold merges the underfull cursor child with
a sibling that satisfies `atN_min()`, so the merged cursor child has
`>= atN_min()` children and the first conjunct is false. Hence when the first
conjunct is true the root still has at least two children, and the second
operand's false side is never selected. Verification: gcov branch counts
above plus the collapse loop L925.

Fix 6c evaluation: a directed runtime assert inside `atD_rebalance` would
require editing `anchortree.h`, which this task forbids; no library change
was made. The invariant is already pinned at the test level by the shared
`att_checktree` root-occupancy check (`atN_cc(&root) == 1 && levels != 0` is
rejected; `tests/at_tests.h` L330) that every case calls, and the new
`tests/anchortree_test_fanout32.c` deep-delete scenario drives rebalance
folds and root collapses at the default fanout 32.

## Summary

All 13 uncovered branch points are structural: reserve-helper shortcut,
chain-search miss, one 32-bit platform guard, the external-allocator abort
path, three invariant-bounded loop exits, four `atK_parent` phantom macro
sides, one assert-expanded underfull-check early return, and one root-fold
side excluded by the root-occupancy invariant. No coverable logic branch is
left unexercised; 97.2% branch coverage is above the 95% gate.
