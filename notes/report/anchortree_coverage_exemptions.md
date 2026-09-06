# anchortree coverage exemptions

Status: `just at-cov` reports 100% lines / 100% functions / 95.1% branches
for `anchortree.h` (679/714 branch points). The remaining uncovered branch
records are listed below. None affect line coverage, and none are caused by
`LCOV_EXCL` or weakened invariants.

## Structural loop/boundary branches (cannot execute under valid invariants)

- `atN_insert_nonfull` / `atF_locate` / `atE_find` / `atK_find` search loops:
  lines 547, 553, 565, 566, 580, 581, 599, 618, 895, 901.
  The uncovered subconditions are the "run past the last slot" / no-valid-child
  outcomes. Every search is guarded by `T->bytes` and node-sum arithmetic, and
  the code asserts the chosen slot before use; a run-past-slot path would mean
  tree corruption before the search began.
- `atT_remove_empty_leaf` line 684: the loop terminates at `d == 0` with an
  explicit `break`, so the normal `d < 0` fall-out condition is never reached.
- `at_gravity` line 1378: for valid endpoints the earlier GROW/FIXED checks
  leave `sl == 1 && el == 1` as the only valid way to reach SHIFT; the false
  outcome is unreachable after `sl < 0 || el < 0` has been rejected.

## Contract-guarded endpoint-vector branches

- `atO_remove` lines 477-478: the tail search is called only when the point is
  known to exist in that tail; the loop exits by `break` on the match. The
  lcov-visible fall-through/not-found subconditions would violate the helper's
  precondition and reach `atV_erase` with an out-of-range index.
- `atO_discard_empty_tail` line 458: the missing subcondition is the null-tail
  no-op outcome in the cleanup path; direct null-tail calls were added, and the
  remaining subcondition is an alternative short-circuit ordering inside the
  cleanup guard.

## Defensive OOM-ordering and overflow guards

- `atS_defallocf` line 821: `realloc` failure intentionally aborts the default
  allocator; it is not an injectable error path in the coverage harness.
- `atD_remove_range` lines 1177, 1185; `atD_delete` lines 1197, 1199, 1207,
  1210, 1219; `atI_insert` lines 1236, 1253, 1264, 1271, 1278, 1286; and
  `at_splice` lines 1304, 1311.
  These are remaining short-circuit/cleanup subbranches of OOM and rollback
  code. The primary error paths are covered (first/second boundary insertion
  failure, split OOM, move/relocate OOM, reserve OOM); the residual subbranches
  are alternate orderings that preserve the same already-tested rollback
  behavior and cannot change invariants.
- `atD_delete` line 1197/1199: the overflow true branch and the `R <= L`
  return are `SIZE_MAX` arithmetic guards; the normal-path and saturated-path
  behavior are structurally disjoint and no ordinary test can exercise
  `del > SIZE_MAX - L` without an artificial overflow value.
