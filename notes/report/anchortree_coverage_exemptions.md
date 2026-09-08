# anchortree coverage exemptions

Status: `just at-cov` reports 100% lines (988/988), 100% functions
(100/100), and 95.7% branches (754/788) for `anchortree.h`. Every line
and function is covered; 34 branch points on 32 source lines remain
uncovered and are listed below. The line list is exactly the final
`just at-unbranched` output.
No `LCOV_EXCL` markers or weakened invariants are used.

Remaining lines: L319, L329, L344, L356, L373, L394, L395, L628,
L634, L645, L734, L740, L861, L864, L893, L943, L970, L973, L1043 (2),
L1044, L1063, L1110, L1160 (2), L1274, L1278, L1400, L1420, L1427,
L1442, L1443, L1462, L1517.

The 18 branch points covered by this pass: `atO_remove` search misses
(L372, L374, L375 x2, L376), `atO_discardemptytail` empty-tail free
(L356), overflow clamp in `atD_remove` (L1439), edge-pool reserve OOM
in `at_splice` (L1460), collect tail-push OOM (L1180), recursive
collect OOM (L1202), insert/append child-split OOM (L1281, L1303),
seam shift rollback (L1379), prepwindow cleanup (L1420, L1426, L1427),
and the zero-index climb in `atD_backwardnode` (L970 x2).

## Macro-expansion phantom branches

- L319 (idx 3), L329 (idx 1), L356 (idx 3), L373 (idx 1):
  the false side of the `atV_len(A)` macro ternary (`A ? hdr->len : 0`)
  is guarded by an outer `A != NULL` / `A == NULL` test on the same
  pointer, so the macro's NULL side can never be selected.
- L861 (idx 1), L864 (idx 1): `atK_parent(C, l)` macro false side
  (`l == 0`). The enclosing `atD_cutrange` loop is `for (kl = l;
  kl > fl; --kl)` with `fl >= 0`, so `kl > 0` at every call.
- L943 (idx 1): same macro in `atD_rebalance`, whose loop is
  `for (; l > 0; --l)`.
- L973 (idx 1): same macro in `atD_backwardnode`; the preceding
  `while (++dl <= l)` increments before the call, so `dl >= 1`.

## Loop bounds guaranteed by tree invariants

- L394 (idx 0), L395 (idx 1): `atO_findedge` loop exits by `i >= cc`.
  Callers pass `0 < pos <= T->bytes`; with the maintained metric
  `sum(bytes) == T->bytes` the loop always finds a slot.
- L628 (idx 0), L634 (idx 0): `atN_locate` loop exits by `i >= cc`.
  `atD_deleteedge` validates `pos <= T->bytes`; `atI_shift` passes
  `pos + 1 <= T->bytes` under `assert(pos < T->bytes)`.
- L734 (idx 0), L740 (idx 0): `atK_find` loop exits by `i >= cc`.
  `at_seek` / `at_advance` only call it when `pos <= T->bytes`.
- L1274 (idx 0), L1278 (idx 0): `atI_insertat` loop exits by
  `i >= cc`; the recursive insertion position is always inside the
  node's byte span.

## Allocator abort

- L645 (idx 1): the default allocator's `realloc` failure path calls
  `abort()`. The coverage harness cannot inject failure into the
  default allocator (and a real OOM is defined to abort).

## Reserve guards and structurally impossible cleanup sides

- L344 (idx 0): `atV_push` failure inside `atO_push`. Every production
  caller pre-reserves the destination tail (`atO_reservenewanchor`,
  `atO_reserve` in `atD_commitside` / `atI_moverights`), so the push
  cannot fail after a successful reserve. This is the documented
  reserve-guard exemption.
- L1420 (idx 2), L1427 (idx 2): false side of `atO_empty(le)` in
  `if (lnew && atO_empty(le))`. `lnew` is set immediately after
  `atI_insertedge(T, L, &le)`; when `lnew` is 1 the edge is newly
  created and has no endpoints, so `atO_empty(le)` is always true.
  The only code between the assignment and the check touches the R
  edge (`R > L`), never `le`.
- L1160 (idx 1, idx 2): `se != &T->origin` false side and
  `atO_empty(se)` false side. `atO_reservenewedge` reserves only when
  `side >= 0`, i.e. `se == ee && sl == el`. In that case
  `atO_reservenewanchor` already reserved the same 2 slots on the same
  side whenever the edge pre-existed (including the origin), so the
  second reserve cannot fail. It can fail only when `se` was newly
  created by `atO_edges` (the pre-reserve was skipped because the edge
  did not exist), and such an edge is empty. Hence both the
  `se == origin` and the non-empty-`se` sides are unreachable.

## Empty-container / transient-stitch states

- L893 (idx 1): true side of `atK_idx(C, p, l) == cc` in
  `atD_mergeleaf`. It requires the left cursor to point one past the
  last slot while the last edge is empty (a transient stitch state).
  The public splices exercised here never leave a settled tree in that
  state; constructing it directly needs an empty tree edge, which
  violates invariant 4.
- L1063 (idx 1): `atN_cc(p) == 0` (empty leaf) in `atD_stitch`. Range
  deletion always leaves the L survivor edge in the leaf, so the leaf
  is non-empty when `atD_stitch` runs.
- L1110 (idx 3): `atN_cc(anc[L]) > 0` false side (empty leaf) in
  `atD_rebalancepath`; same survivor-edge reason.
- L970 (idx 0): `--dl >= 0` false side (climb past the root) in
  `atD_backwardnode`. A valid backward move always finds a predecessor
  slot; the following `assert(dl >= 0)` guards the invariant, and
  reaching it would index `C->paths[-1]`.

## Parameter checks after validation

- L1400 (idx 0): `pos < T->bytes` false side in `atI_insert`. The
  branch is reached only under `pos != 0 && !atO_findedge(T, pos, &e)`;
  since `T->bytes` is the position of the last tree edge,
  `atO_findedge` finds an edge for `pos == T->bytes`, so `!found`
  implies `pos < T->bytes`. The false side needs a stale metric.
- L1442 (idx 0), L1443 (idx 0), L1462 (idx 3): `at_seek` failure
  sides. `at_seek` returns `AT_ERRPARAM` only for a NULL cursor or
  tree; `LC`, `RC`, `C`, and `T` are all non-NULL at these call sites,
  so the failure sides are structurally unreachable.

## Gravity short-circuit

- L1517 (idx 2): `sl == 1 && el == 1`; the `el == 1` false side is
  unreachable because the preceding `if (sl == 1 && el == 0) return
  AT_FIXED;` (L1516) already handles `sl == 1 && el == 0`. Reordering
  the AT_SHIFT / AT_LOCK tests would change the code, not the
  coverage, so the short-circuit is left intact.

## Deep-stitch residual (reachable in principle, not exercised)

- L1043 (idx 1, idx 2), L1044 (idx 1): `atD_stitchnode` reaches these
  lines only when `m < rtcc` at `k > 0`, i.e. a stitch that leaves a
  non-empty remainder in `rt[k]` at an upper level. The current public
  deletion patterns (including the 100-anchor span delete) absorb the
  remainder at `k == 0`, so the `k > 0` and `kl == 0` sides are not
  reached. They are not structurally impossible; a full-tree stitch
  with a leftover upper-level remainder would be needed. Retained as a
  residual exemption rather than constructing a synthetic internal
  stitch state.
