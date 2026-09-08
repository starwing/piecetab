# anchortree coverage exemptions

Status: `just at-cov` reports 100% lines / 100% functions / 93.3% branches
for `anchortree.h` (726/778 branch points). Every line and function is covered;
the 52 remaining uncovered branch points are listed below by source line.
No `LCOV_EXCL` markers or weakened invariants are used. Counts in parentheses
are uncovered branch points when a line has more than one.

## Structural search / loop bounds

- L390, L391, L591, L597, L697, L703, L1236, L1240:
  search loops in `atO_findedge` / `atN_locate` / `atK_find` / `atI_insertat` /
  `atI_appendat`. The uncovered sides are the "run past the last slot" and
  "no exact slot match" outcomes. Public calls bound positions by `T->bytes`,
  node sums are maintained by invariants, and asserts guard the selected slot
  before use; these sides require prior tree corruption.
- L823, L826, L905, L1005 (2), L1006:
  stitch traversal / loop-boundary sides in `atD_cutrange`, `atD_rebalance`, and
  `atD_stitchnode`. The deterministic tests cover the normal fold and findroom
  paths; the remaining sides need a synthetic cursor state outside public-API
  reachability.
- L932 (3), L935:
  `atD_backwardnode` climb/descend loop sides. The direct test covers the
  `d > i` move; the remaining sides are loop-boundary permutations not produced
  by the public splice sequences exercised here.

## Endpoint vector representation

- L315, L325, L340, L352 (2), L368, L369, L370, L371 (2), L372, L1142:
  `atO_listcount`, `atO_push`, `atO_discardemptytail`, `atO_remove`, and
  `atD_collectside` first-vs-tail representation and search sides. Tests cover
  first entries and non-empty tails on both sides; the remaining sides are
  null/empty-tail alternates and loop/search fall-throughs that are structural
  (tail non-null iff first is full) or allocation-failure sides.
- L855:
  `atD_mergeleaf` cursor-at-tail correction. The remaining side is the
  not-at-tail case for a zero-byte last slot, which is a temporary stitch state
  not present in a settled public tree.
- L1025:
  `atD_stitch` zero-byte last-slot cleanup. The remaining side is the
  no-zero-last-slot case; the zero slot appears only transiently during the
  covered trim/stitch paths.
- L1072:
  `atD_rebalancepath` tail-slot selection. The remaining side is the
  `idx[L] >= cc` fallback, reached only when a deleted-slot cursor sits one past
  the container tail (defensive post-delete state).

## OOM ordering, allocator abort, overflow guards

- L608:
  default allocator `realloc` failure intentionally aborts; it is not
  injectable in the coverage harness.
- L1122 (2), L1164, L1243, L1265, L1382 (2), L1388, L1389 (2), L1422:
  OOM cleanup / error-propagation sides in `atO_reservenewedge`,
  `atD_collectnode`, `atI_insertat`, `atI_appendat`, `atD_prepwindow`, and
  `at_splice`. Tests cover the primary boundary-reserve, split, delete, and
  collect OOM paths; these residual sides are alternate orderings that preserve
  the same transactional rollback behavior.
- L1341, L1362:
  rollback `atI_shift` guards. The uncovered sides are `pos >= T->bytes`, i.e.
  no shift was performed, which cannot reach the rollback branch through the
  public splice paths covered.
- L1401:
  `del > SIZE_MAX - L` saturated-overflow side. Ordinary test ranges cannot
  form a deletion length large enough to overflow `size_t`.
- L1404, L1405, L1424:
  `at_seek` failure checks after `C` / `T` have already been validated; the
  failure side is structurally unreachable at these call sites.
- L1479:
  `at_gravity` SHIFT short-circuit side. All four gravity values are covered by
  `anchor_gravities`; the remaining side is the `sl == 1` false short-circuit,
  not a missing gravity outcome.
