# Audit: spantree identity layer v1

Reviewed `lua/spantree.c`, `lua/spantree.d.lua`, `lua/tests/spantree_test.lua`
against `notes/design_spantree_identity.md`, `notes/design_spantree_lua.md` §8,
`AGENTS.md`, `notes/brief_refine.md`, `notes/brief_tests.md`.
All new identity/sensitive tests pass under PUC Lua and LuaJIT.

## Findings

1. **major** — `lua/spantree.c:1753-1772` (`lst_remove_mark`): exact delete is not exact when a mark has been split and another same-ns mark occupies a hole.
   Evidence: the function collects the min/max over all fragments of the target mark and then does one `sp_fill(CLEAR ns, max-min)`. If mark A is split by `clear` into `[0,4)` + `[6,10)` and mark B is later placed in the hole `[4,6)`, `unmark(A)` clears `[0,10)` and deletes B as well (reproduced: `unmark(A)` returns 1, `unmark(B)` returns 0 afterward). The same over-deletion happens in content-sensitive auto-invalidation because `lst_editcommit` calls this function for each pending mark.
   Suggested fix: delete per hit segment instead of the union — collect each `(start, len)` containing the mark and `sp_fill(CLEAR ns, len)` at each start (right-to-left or after collecting), or otherwise clear only the target mark id's actual fragments. Update design §7.1, which currently assumes the min/max interval contains only the target object; non-overlap does not guarantee that when holes are re-filled.

2. **major** — `lua/spantree.c:760-782` (`cp_refup` / `cp_refdown`): stale mark ids fall through to composite-slot indexing and can cause out-of-bounds access.
   Evidence: both functions first try `cpM_get`; if it returns NULL for an `id >= CP_MARK_BASE` (stale/unknown mark), the next check is only `id < CP_COMP_START`, so control reaches `S->slots[(size_t)(id - CP_COMP_START)]` with a huge index. Normal current paths do not appear to pass a stale mark here, but this is a latent memory-safety violation in exactly the mark-id domain the change introduces.
   Suggested fix: after the `cpM_get` miss, add `if (id >= CP_MARK_BASE) return;` (or assert) before the composite branch in both functions.

3. **minor** — `lua/spantree.c:735-757` (`cp_markalloc` / `cp_markfree`): mark slot/generation exhaustion is not handled safely.
   Evidence: `cp_markfree` asserts `m->gen + 1 < CP_MARK_GENMASK`, but with `-DNDEBUG` the increment can reach `1 << 20`; the encoded `gen << 28` then aliases back into the `CP_MARK_BASE` bit / lower generation, so a very old stale handle could become valid again. `cp_markalloc` also never checks `marknext` against `CP_MARK_SLOTS`, so slot space can wrap/collide after 2^28 allocations.
   Suggested fix: when a slot's generation is about to exceed the 20-bit field (or `marknext` reaches `CP_MARK_SLOTS`), stop reusing that slot / raise a clear error instead of wrapping; use a limit check that is active in release builds.

4. **minor** — `lua/spantree.c:1391-1396` + `1753-1772`: content-sensitive invalidation can be O(k·N).
   Evidence: `lst_editcommit` calls `lst_remove_mark` once per pending mark, and each call rescans the tree (`sp_seek(0)` + `sp_next` over the ns). A single edit can enqueue up to `SP_MASK_BITS * 3` marks, so worst case is repeated full/partial tree scans. This is acceptable for v1 but worth recording as a performance risk.
   Suggested fix: if this becomes hot, collect all affected mark ids and remove them in one pass, or add a mark→segment index (already listed as v2).

5. **nit** — `lua/spantree.c:2016-2018` (`lst_identitytail`): the multi-value comma-expression return is written as a hanging continuation, violating `notes/brief_refine.md` §26 / no-hanging-continuation style.
   Suggested fix: rewrite as a small `if` block with normal assignments and `return end;`.

6. **nit** — `lua/spantree.c:645-662` (`cpP_hashkey`): `cap` is only used in an `assert`, which is compiled out under `-DNDEBUG`; there is no release-mode bounds enforcement.
   Evidence: `(void)cap;` followed by `assert(off + j + 1 <= cap)`. The current `CP_KEY_SZ` is large enough, but the helper is fragile if the constant changes.
   Suggested fix: use a bounded write that is active in release builds, or add a compile-time/static assertion on the maximum key size.

No test expectations were found that contradict the design; the new tests all pass. The main untested gap is the hole/re-fill exact-delete case in finding 1.

7. **minor** — `lua/spantree.c:1560-1565` and `2324-2328` (`Ltree_mark` / `Lcur_mark`): an identity mark is allocated before `sp_fill`; if `sp_fill` fails (e.g. OOM), the mark record is left with `refcnt == 0` and `ns != 0`, not on the freelist, so the slot leaks.
   Suggested fix: on the error path roll the fresh mark back (or arrange allocation so failure cannot leak, e.g. allocate only after the fill is known to be possible and free the record if `sp_fill` reports failure).
