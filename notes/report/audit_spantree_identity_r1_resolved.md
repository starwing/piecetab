# audit_spantree_identity_r1 — resolution

Date: 2026-09-01. Companion to `audit_spantree_identity_r1.md`.
All items below are implemented in `lua/spantree.c`; validation: `just lua/sp`
(161 tests, PUC + LuaJIT), `just lua/ed`, `just lua/tm`, `just sp`,
`just luals`, `clang-format --dry-run --Werror` all green. Coverage:
98.9% lines / 100% functions / 90.0% branches (see
`notes/reports/coverage_spantree_identity.md`).

## #1 major — lst_remove_mark over-deletes hole marks

FIXED. Replaced min/max-union fill with a right-to-left per-fragment pass:

- scan with `sp_prev(C, m->ns)` from `sp_bytes(t->T)`;
- each hit is cleared alone: `sp_fill(&C, op, len)` at that segment start,
  then `sp_seek(&C, t->T, start)` and continue left;
- non-target same-ns segments are skipped (`i == n → continue`).

Regression tests:

- `TestIdentity:testUnmarkExactKeepsHoleMarks` — A mark `[0,10)` split by
  `clear(4,2)`, B mark fills `[4,6)`; `unmark(A)==1` leaves B, then B
  unmarks normally.
- `TestSensitive:testHoleMarkSurvivesOtherInvalidation` — same shape with
  `"is"` flags; `remove(1,1)` auto-invalidates A, B survives at its shifted
  offset.

Docs updated: `notes/design_spantree_identity.md` §7.1,
`notes/design_spantree_lua.md` §8.3.

## #2 major — cp_refup / cp_refdown stale-mark OOB fall-through

FIXED. Both now bail on any id outside the composite domain after a
`cpM_get` miss:

```c
if (id < CP_COMP_START || id >= CP_MARK_BASE) return;
```

This is defense in depth: normal tree paths never carry a stale mark id
(refcnt invariant), but the guard removes the latent OOB read/write.

## #3 minor — mark slot / generation exhaustion

FIXED in release builds (no assert-only behavior):

- `cp_markalloc`: `marknext >= CP_MARK_SLOTS` → `luaL_error(L, "spantree:
  mark space exhausted")`.
- `cp_markfree`: if `gen + 1 >= CP_MARK_GENMASK`, the slot is retired —
  `ns`/`attr` cleared and the slot is NOT returned to the freelist, so the
  old handle can never alias; the gen field cannot wrap into lower bits.

## #4 minor — O(k·N) content-sensitive removal

Accepted for v1; recorded as a performance risk in
`notes/design_spantree_lua.md` §8.4 and complexity note in
`notes/design_spantree_identity.md` §7.1. If it becomes hot: one pass
collecting all affected mark ids, or a mark→segment index (v2).

## #5 nit — lst_identitytail hanging continuation

FIXED. Rewritten as ordinary `if { ... }` blocks with explicit
`c->mlen = 0, c->mcur = ...; return end;` statements.

## #6 nit — cpP_hashkey cap only asserted

FIXED with a compile-time bound that is active in release builds:

```c
typedef char cpM_keysize[(CP_KEY_SZ > (SP_MASK_BITS + 1) * 21) ? 1 : -1];
```

`21` = 20 decimal digits of `sp_Id` max (`2^64 - 1`) + `';'`; key length
upper bound is `(SP_MASK_BITS + 1)` leaves. The runtime `assert` remains as
a debug check; no runtime error path/branch was added (keeps branch
coverage stable).

## #7 minor — mark record leak if sp_fill fails

FIXED. New helper `cpM_markfill(L, S, C, op, len)` fills and, on error,
rolls the fresh mark back via `cp_markfree` before raising
`spantree: error(%d)`. Used by both `Ltree_mark` and `Lcur_mark` identity
branches. (Only reachable on OOM: `sp_fill` returns before mutating the
tree once `spP_reserve` fails.)
