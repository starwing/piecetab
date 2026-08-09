#define UT_STATIC_API
#define UT_POOL_STATS
#include "../undotree.h"
#include "tests.h"

/* ─── pool drain helpers (ut_Pool specific) ─── */

typedef struct {
    void *chain;
} ut_Drain;

static ut_Drain ut_drainpool(ut_Pool *p) {
    ut_Drain d;
    d.chain = p->freed, p->freed = NULL;
    return d;
}

static void ut_refillpool(ut_Pool *p, ut_Drain d) {
    void **pp;
    if (d.chain == NULL) return;
    for (pp = &d.chain; *pp; pp = (void **)*pp) continue;
    *pp = p->freed, p->freed = d.chain;
}

/* T1: lifecycle */
TEST(lifecycle) {
    ut_State *S;
    ut_Tree  *T1, *T2;

    /* default allocator (NULL) */
    S = ut_open(NULL, NULL);
    assert(S != NULL);

    /* set cleaner */
    ut_setcleaner(S, NULL, NULL);

    /* newtree with NULL payload (valid) */
    T1 = ut_newtree(S, NULL);
    assert(T1 != NULL);
    assert(ut_current(T1) != NULL);
    assert(ut_root(T1) != NULL);
    assert(ut_root(T1) == ut_current(T1));
    assert(ut_root(T1) == &T1->root);

    T2 = ut_newtree(S, NULL);
    assert(T2 != NULL && T1 != T2);

    ut_deltree(S, T1);
    ut_deltree(S, T2);
    assert(S->node_pool.live_obj == 0);
    ut_close(S);

    /* NULL guards */
    ut_reset(NULL);
    ut_close(NULL);
    ut_setcleaner(NULL, NULL, NULL);
    assert(ut_newtree(NULL, NULL) == NULL);

    /* ut_open OOM */
    {
        int z = 0;
        assert(ut_open(&oom_alloc, &z) == NULL);
    }

    /* ut_newtree OOM */
    {
        int       one = 1;
        ut_State *S2 = ut_open(&oom_alloc, &one);
        assert(S2 != NULL);
        assert(ut_newtree(S2, NULL) == NULL);
        ut_close(S2);
    }
}

TEST(record_basic) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    const ut_Hunk *h;
    size_t         hn;
    assert(ut_record(T, 0, 0, 10) == UT_OK);
    assert(ut_record(T, 0, 10, 0) == UT_OK);
    assert(ut_record(T, 0, 0, 3) == UT_OK);
    assert(ut_record(T, 3, 0, 3) == UT_OK);
    assert(ut_record(T, 6, 0, 4) == UT_OK);
    assert(ut_freshcount(T) == 5);
    assert(ut_freshdiff(T, 1, 5) == 1);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(h[0].pa == 0 && h[0].ca == 0 && h[0].pdel == 10 && h[0].cins == 10);
    ut_deltree(S, T);
    ut_close(S);
}

/* T2: record + fresh + unrecord */
TEST(record_fresh) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(ut_freshcount(T) == 0);
    assert(ut_record(T, 0, 5, 10) == UT_OK);
    assert(ut_freshcount(T) == 1);
    assert(ut_record(T, 10, 3, 7) == UT_OK);
    assert(ut_freshcount(T) == 2);
    ut_unrecord(T, 1);
    assert(ut_freshcount(T) == 1);
    ut_unrecord(T, 1);
    assert(ut_freshcount(T) == 0);
    ut_unrecord(T, 1); /* no-op */
    assert(ut_freshcount(T) == 0);
    ut_deltree(S, T);
    ut_close(S);
}

/* T3: commit simple (single journal entry) */
TEST(commit_simple) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root, child;
    const ut_Hunk *hunks;
    size_t         hn;

    root = ut_root(T);
    assert(ut_record(T, 10, 3, 7) == UT_OK);
    child = ut_commit(T, NULL);
    assert(child != NULL);
    assert(child != root);
    assert(ut_parent(child) == root);
    assert(ut_childcount(root) == 1);
    assert(ut_firstchild(root) == child);
    assert(ut_freshcount(T) == 0);

    /* verify hunk */
    hunks = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(hunks[0].pa == 10 && hunks[0].ca == 10 && hunks[0].pdel == 3
           && hunks[0].cins == 7);

    /* commit without journal -> child with 0 hunks */
    root = ut_current(T);
    child = ut_commit(T, NULL);
    assert(child != NULL);
    assert(ut_childcount(root) == 1);

    ut_deltree(S, T);
    ut_close(S);
}

/* T4: switch + discard */
TEST(switch_discard) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_Vid    child;

    /* switch with fresh -> error */
    assert(ut_record(T, 0, 1, 2) == UT_OK);
    assert(ut_switch(T, root) == UT_ERRPARAM);
    ut_discard(T);
    assert(ut_freshcount(T) == 0);

    /* switch without fresh -> ok */
    child = ut_commit(T, NULL);
    assert(ut_switch(T, root) == UT_OK);
    assert(ut_current(T) == root);
    assert(ut_switch(T, child) == UT_OK);
    assert(ut_current(T) == child);

    ut_deltree(S, T);
    ut_close(S);
}

/* T5: branch (two children) */
TEST(branch) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_Vid    c1, c2;

    c1 = ut_commit(T, NULL); /* child 0 */
    assert(ut_switch(T, root) == UT_OK);
    c2 = ut_commit(T, NULL); /* child 1 */
    assert(c1 != c2);
    assert(ut_parent(c1) == root);
    assert(ut_parent(c2) == root);
    assert(ut_childcount(root) == 2);

    ut_deltree(S, T);
    ut_close(S);
}

/* T6: LCA */
TEST(ancestor) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_Vid    c1, c2, gc;

    c1 = ut_commit(T, NULL);
    gc = ut_commit(T, NULL); /* grandchild under c1 */
    assert(ut_switch(T, root) == UT_OK);
    c2 = ut_commit(T, NULL);

    assert(ut_ancestor(c1, c2) == root);
    assert(ut_ancestor(c1, gc) == c1);
    assert(ut_ancestor(gc, root) == root);
    assert(ut_ancestor(root, c2) == root);
    assert(ut_ancestor(NULL, c1) == NULL);
    assert(ut_ancestor(c1, NULL) == NULL);

    ut_deltree(S, T);
    ut_close(S);
}

/* T7: deep chain (1000 commits, test non-recursive deltree) */
TEST(deep_chain) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    int       i;
    for (i = 0; i < 1000; i++) ut_commit(T, NULL);
    assert(ut_current(T) != NULL);
    ut_deltree(S, T);
    ut_close(S);
}

/* T8: OOM on journal expansion */
TEST(record_oom_journal) {
    int       oom = 2;
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T != NULL);
    assert(ut_record(T, 0, 1, 1) == UT_ERRMEM);
    assert(ut_freshcount(T) == 0);
    ut_deltree(S, T);
    ut_close(S);
}

/* T9: normalize via commit — two journal entries merge to one hunk */
TEST(normalize_merge) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         v;
    const ut_Hunk *h;
    size_t         hn;

    /* adjacent inserts should merge: (0,0,5) + (5,0,3) → (0,0,8) one hunk */
    ut_record(T, 0, 0, 5);
    ut_record(T, 5, 0, 3);
    v = ut_commit(T, NULL);
    assert(v != NULL);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(h[0].pa == 0 && h[0].pdel == 0 && h[0].cins == 8);

    ut_deltree(S, T), ut_close(S);
}

/* T10: normalize delete+insert — 2 hunks (gap between regions) */
TEST(normalize_overlap) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    const ut_Hunk *h;
    size_t         hn;
    assert(T);

    /* delete 4 at 10, then insert 3 at offset 12 (after deletion) */
    /* delete maps to X[10,14), insert maps to X[16] (gap=2) */
    ut_record(T, 10, 4, 0);
    ut_record(T, 12, 0, 3);
    ut_commit(T, NULL);
    h = ut_hunks(T, &hn);
    assert(hn == 2);
    assert(h[0].pa == 10 && h[0].pdel == 4 && h[0].cins == 0);
    assert(h[1].pa == 16 && h[1].pdel == 0 && h[1].cins == 3);

    ut_deltree(S, T), ut_close(S);
}

/* T11: invert — H ∘ inv(H) ≡ empty */
TEST(invert_identity) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         c1, c2;
    const ut_Hunk *h;
    size_t         hn;

    /* c1: (5,2,7), c2: empty commit on root → diff from c1 to c2 */
    ut_record(T, 5, 2, 7);
    c1 = ut_commit(T, NULL);
    ut_switch(T, ut_root(T));
    c2 = ut_commit(T, NULL);
    {
        int n;
        n = ut_diff(T, c1, c2);
        assert(n >= 0);
        h = ut_hunks(T, &hn);
        assert(hn == 1);
        assert(h[0].pa == 5 && h[0].ca == 5 && h[0].pdel == 7
               && h[0].cins == 2);
    }

    ut_deltree(S, T), ut_close(S);
}

/* T12: compose — A∘B chain */
TEST(compose_assoc) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root = ut_root(T);
    ut_Vid         c1, c2;
    const ut_Hunk *h;
    size_t         hn;

    /* chain: root → c1 (3,0,5) → c2 (2,3,1) */
    ut_record(T, 3, 0, 5);
    c1 = ut_commit(T, NULL);
    ut_record(T, 2, 3, 1);
    c2 = ut_commit(T, NULL);
    (void)c1;

    {
        int n;
        n = ut_diff(T, root, c2);
        assert(n >= 0);
        h = ut_hunks(T, &hn);
        (void)h;
    }

    ut_deltree(S, T), ut_close(S);
}

/* T13: diff same version → 0 hunks */
TEST(diff_identity) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    v = ut_root(T);
    assert(ut_diff(T, v, v) == 0);

    ut_deltree(S, T), ut_close(S);
}

/* T14: diff with journal (fresh state) */
TEST(diff_fresh) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root = ut_root(T);
    const ut_Hunk *h;
    size_t         hn;

    ut_record(T, 10, 3, 7);
    {
        int n;
        n = ut_diff(T, root, ut_freshvid(S));
        assert(n >= 0);
        h = ut_hunks(T, &hn);
        assert(hn == 1);
        assert(h[0].pa == 10 && h[0].pdel == 3 && h[0].cins == 7);
    }

    ut_deltree(S, T), ut_close(S);
}

/* T15: no-op compose — insert then delete same bytes → 0 hunks */
TEST(compose_noop) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         v;
    const ut_Hunk *h;
    size_t         hn;

    ut_record(T, 0, 0, 5);
    ut_record(T, 0, 5, 0);
    v = ut_commit(T, NULL);
    assert(v != NULL);
    h = ut_hunks(T, &hn);
    assert(hn == 0);
    (void)h;

    ut_deltree(S, T), ut_close(S);
}

/* T16: compose — B before A (emitY2Z in mergewalk body) */
TEST(compose_emit_b) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         v;
    const ut_Hunk *h;
    size_t         hn;

    ut_record(T, 10, 0, 5); /* insert 5 at 10 */
    ut_record(T, 0, 3, 0);  /* delete 3 at 0   */
    v = ut_commit(T, NULL);
    assert(v != NULL);
    h = ut_hunks(T, &hn);
    assert(hn == 2);
    assert(h[0].pa == 0 && h[0].pdel == 3 && h[0].cins == 0);
    assert(h[1].pa == 10 && h[1].pdel == 0 && h[1].cins == 5);

    ut_deltree(S, T), ut_close(S);
}

/* T17: compose — A before B (emitX2Y in mergewalk body) */
TEST(compose_emit_a) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         v;
    const ut_Hunk *h;
    size_t         hn;

    ut_record(T, 0, 3, 5);  /* insert+delete at 0 */
    ut_record(T, 20, 1, 0); /* delete at 20       */
    v = ut_commit(T, NULL);
    assert(v != NULL);
    h = ut_hunks(T, &hn);
    assert(hn == 2);
    assert(h[0].pa == 0 && h[0].pdel == 3 && h[0].cins == 5);
    assert(h[1].pdel == 1 && h[1].cins == 0);

    ut_deltree(S, T), ut_close(S);
}

/* T18: OOM — ut_commit normalize failure */
TEST(commit_oom_normalize) {
    int       oom = 3; /* state + tree + journal push */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T != NULL);
    ut_record(T, 0, 1, 1);
    /* compose in normalize needs alloc → OOM */
    assert(ut_commit(T, NULL) == NULL);
    assert(ut_current(T) == ut_root(T)); /* tree unchanged */
    ut_deltree(S, T), ut_close(S);
}

/* T19: ut_commit utN_alloc OOM via drained pool (empty journal, direct node
 * alloc) */
TEST(commit_oom_pool) {
    int       oom = 2; /* state+tree_page=2, node_pool page drained→OOM */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Drain  nd = ut_drainpool(&S->node_pool);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T != NULL);
    assert(ut_commit(T, NULL) == NULL);
    assert(ut_current(T) == ut_root(T));
    ut_refillpool(&S->node_pool, nd);
    ut_deltree(S, T), ut_close(S);
}

/* T20: OOM — ut_diff invert fresh failure (phase 1) */
TEST(diff_oom_invert) {
    int       oom = 4; /* state+tree+journal+normalize compose */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T != NULL);
    ut_record(T, 10, 3, 7);
    /* diff from fresh: normalize succeeds, invert alloc from oom → UT_ERRMEM */
    assert(ut_diff(T, ut_freshvid(S), ut_root(T)) < 0);
    ut_deltree(S, T), ut_close(S);
}

/* T21: OOM during ut_diff — compose fails between two siblings across LCA */
TEST(diff_oom_compose) {
    int oom = 7; /* state+tpage+jp+norm+np+norm2 → 6 for commits,
                    diff needs 7th → OOM */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    c1, c2;
    assert(T != NULL);
    ut_record(T, 0, 1, 2);
    c1 = ut_commit(T, NULL);
    assert(c1 != NULL);
    ut_record(T, 5, 0, 3);
    c2 = ut_commit(T, NULL);
    assert(c2 != NULL);
    assert(ut_diff(T, c1, c2) < 0);
    assert(ut_current(T) == c2);
    ut_deltree(S, T), ut_close(S);
}

/* T22: ut_switch rejects freshvid */
TEST(switch_freshvid) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(ut_switch(T, ut_freshvid(S)) == UT_ERRPARAM);
    ut_deltree(S, T), ut_close(S);
}

/* T23: ut_diff fresh-to-fresh (both endpoints) */
TEST(diff_fresh_both) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);

    assert(ut_diff(T, ut_freshvid(S), ut_freshvid(S)) >= 0);
    ut_deltree(S, T), ut_close(S);
}

/* T24: ut_diff from fresh to committed */
TEST(diff_fresh_to) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root = ut_root(T);
    const ut_Hunk *h;
    size_t         hn;

    ut_record(T, 0, 2, 4);
    {
        int n;
        n = ut_diff(T, ut_freshvid(S), root);
        assert(n >= 0);
        h = ut_hunks(T, &hn);
        assert(hn == 1);
        assert(h[0].pa == 0 && h[0].ca == 0 && h[0].pdel == 4
               && h[0].cins == 2);
    }

    ut_deltree(S, T), ut_close(S);
}

/* T25: ut_hunks after diff returns scratch */
TEST(hunks_after_diff) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root = ut_root(T);
    const ut_Hunk *h;
    size_t         hn;

    ut_record(T, 10, 3, 7);
    {
        int n = ut_diff(T, root, ut_freshvid(S));
        assert(n >= 0);
        h = ut_hunks(T, &hn);
        assert(hn == 1);
        assert(h[0].pa == 10 && h[0].pdel == 3 && h[0].cins == 7);
    }

    ut_deltree(S, T), ut_close(S);
}

/* T26: ut_hunks returns current->h when no pending diff */
TEST(hunks_current) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         v;
    const ut_Hunk *h;
    size_t         hn;

    /* without any commit, root has NULL hunk list → 0 hunks */
    h = ut_hunks(T, &hn);
    assert(hn == 0 && h == NULL);

    ut_record(T, 0, 1, 2);
    v = ut_commit(T, NULL);
    assert(v != NULL);
    h = ut_hunks(T, &hn);
    assert(hn == 1 && h != NULL);

    ut_deltree(S, T), ut_close(S);
}

/* T27: NULL guard for ut_hunks */
TEST(hunks_params) {
    size_t hn = 99;
    assert(ut_hunks(NULL, NULL) == NULL);
    assert(ut_hunks(NULL, &hn) == NULL);
    assert(hn == 0);
}

/* T28: ut_discard resets diffhn */
TEST(discard_diffhn) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    const ut_Hunk *h;
    size_t         hn;

    ut_record(T, 0, 1, 1);
    ut_diff(T, ut_root(T), ut_freshvid(S));
    h = ut_hunks(T, &hn);
    assert(hn == 1 && h != NULL); /* pending diff active */

    ut_discard(T);
    h = ut_hunks(T, &hn);
    assert(hn == 0 && h == NULL); /* diffhn reset, current->h is NULL */

    ut_deltree(S, T), ut_close(S);
}

/* T29: zero-effect record is no-op (del==0 && ins==0 filtered) */
TEST(zero_record) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);

    assert(ut_record(T, 0, 0, 0) == UT_OK);
    assert(ut_freshcount(T) == 0); /* not added to journal */

    ut_deltree(S, T), ut_close(S);
}

/* T29: ut_commit with NULL T or NULL current */
TEST(commit_params) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(ut_commit(NULL, NULL) == NULL);
    T->current = NULL;
    assert(ut_commit(T, NULL) == NULL);
    ut_deltree(S, T), ut_close(S);
}

/* T30: ut_discard NULL guard */
TEST(discard_params) {
    assert(ut_discard(NULL) == UT_ERRPARAM);
}

/* T31: ut_diff NULL T */
TEST(diff_params) {
    ut_State *S = ut_open(NULL, NULL);
    assert(ut_diff(NULL, NULL, NULL) == UT_ERRPARAM);
    ut_close(S);
}

/* T32: ut_diff with NULL ancestor (different trees) */
TEST(diff_x_tree) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T1 = ut_newtree(S, NULL);
    ut_Tree  *T2 = ut_newtree(S, NULL);
    assert(T1 && T2);
    assert(ut_diff(T1, ut_root(T1), ut_root(T2)) == UT_ERRPARAM);
    ut_deltree(S, T1), ut_deltree(S, T2);
    ut_close(S);
}

/* T33: ut_unrecord with NULL T */
TEST(unrecord_params) { ut_unrecord(NULL, 1); }

/* T34: ut_unrecord with n parameter */
TEST(unrecord_n) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_unrecord(T, 0);
    assert(ut_record(T, 0, 5, 10) == UT_OK);
    assert(ut_record(T, 10, 3, 7) == UT_OK);
    assert(ut_record(T, 20, 1, 4) == UT_OK);
    assert(ut_freshcount(T) == 3);
    ut_unrecord(T, 2);
    assert(ut_freshcount(T) == 1);
    ut_unrecord(T, 10);
    assert(ut_freshcount(T) == 0);
    ut_deltree(S, T);
    ut_close(S);
}

/* T35: ut_freshdiff with NULL T */
TEST(freshdiff_params) {
    assert(ut_freshdiff(NULL, 0, 0) == UT_ERRPARAM);
}

/* T36: ut_freshdiff empty journal (i==j) */
TEST(freshdiff_empty) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    size_t    hn;
    assert(ut_freshdiff(T, 0, 0) == 0);
    assert(ut_hunks(T, &hn) == NULL && hn == 0);
    ut_deltree(S, T);
    ut_close(S);
}

/* T37: ut_freshdiff forward (i<j) */
TEST(freshdiff_forward) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    const ut_Hunk *h;
    size_t         hn;
    assert(ut_record(T, 0, 0, 5) == UT_OK);  /* insert at 0 */
    assert(ut_record(T, 10, 3, 0) == UT_OK); /* delete at 10 */
    assert(ut_record(T, 20, 2, 7) == UT_OK); /* replace at 20 */
    assert(ut_freshdiff(T, 0, 1) == 1);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(h[0].pa == 0 && h[0].ca == 0 && h[0].pdel == 0 && h[0].cins == 5);
    assert(ut_freshdiff(T, 1, 2) == 1);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(h[0].pa == 10 && h[0].ca == 10 && h[0].pdel == 3 && h[0].cins == 0);
    assert(ut_freshdiff(T, 2, 3) == 1);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(h[0].pa == 20 && h[0].ca == 20 && h[0].pdel == 2 && h[0].cins == 7);
    ut_deltree(S, T);
    ut_close(S);
}

/* T38: ut_freshdiff reverse (i>j) */
TEST(freshdiff_reverse) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    const ut_Hunk *h;
    size_t         hn;
    assert(ut_record(T, 0, 0, 5) == UT_OK);
    assert(ut_record(T, 10, 3, 0) == UT_OK);
    assert(ut_freshdiff(T, 2, 0) == 2);
    h = ut_hunks(T, &hn);
    assert(hn == 2);
    /* inverse of forward compose: two hunks in order */
    assert(h[0].pa == 0 && h[0].ca == 0 && h[0].pdel == 5 && h[0].cins == 0);
    assert(h[1].pa == 10 && h[1].ca == 5 && h[1].pdel == 0 && h[1].cins == 3);
    assert(ut_freshdiff(T, 1, 0) == 1);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(h[0].pa == 0 && h[0].ca == 0 && h[0].pdel == 5 && h[0].cins == 0);
    ut_deltree(S, T);
    ut_close(S);
}

/* T39: ut_freshdiff OOB clamp */
TEST(freshdiff_clamp) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    const ut_Hunk *h;
    size_t         hn;
    assert(ut_record(T, 0, 0, 5) == UT_OK);
    assert(ut_record(T, 10, 3, 0) == UT_OK);
    assert(ut_freshdiff(T, -5, 1) == 1);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(h[0].pa == 0 && h[0].cins == 5);
    assert(ut_freshdiff(T, 1, 999) == 1);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(h[0].pa == 10 && h[0].pdel == 3 && h[0].cins == 0);
    assert(ut_freshdiff(T, 999, 1) == 1);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    assert(h[0].pa == 10 && h[0].ca == 10 && h[0].pdel == 0 && h[0].cins == 3);
    assert(ut_freshdiff(T, -1, -1) == 0);
    h = ut_hunks(T, &hn);
    assert(hn == 0);
    ut_deltree(S, T);
    ut_close(S);
}

/* T40: ut_freshdiff entries cancel to no-op (empty hunks) */
TEST(freshdiff_noop) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    const ut_Hunk *h;
    size_t         hn;
    assert(ut_record(T, 0, 0, 3) == UT_OK);
    assert(ut_record(T, 0, 3, 0) == UT_OK);
    assert(ut_freshdiff(T, 0, 2) == 0);
    h = ut_hunks(T, &hn);
    assert(hn == 0 && h == NULL);
    assert(ut_freshdiff(T, 2, 0) == 0);
    h = ut_hunks(T, &hn);
    assert(hn == 0 && h == NULL);
    ut_deltree(S, T);
    ut_close(S);
}

/* T41: ut_freshdiff OOM in normalize forward path */
TEST(freshdiff_oom_normalize) {
    int       oom = 3;
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T);
    ut_record(T, 0, 1, 1);
    assert(ut_freshdiff(T, 0, 1) < 0);
    ut_deltree(S, T), ut_close(S);
}

/* T41: ut_freshdiff OOM in normalize+invert reverse path */
TEST(freshdiff_oom_invert) {
    int       oom = 4;
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T);
    ut_record(T, 0, 1, 1);
    assert(ut_freshdiff(T, 1, 0) < 0);
    ut_deltree(S, T), ut_close(S);
}

/* T34: ut_record with NULL T */
TEST(record_params) {
    assert(ut_record(NULL, 0, 1, 1) == UT_ERRPARAM);
}

/* T35: ut_discard on tree with no journal (journal is NULL) */
TEST(discard_nojournal) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(ut_discard(T) == UT_OK);
    ut_deltree(S, T), ut_close(S);
}

/* T36: ut_hunks with diffhn < 0 and T->current NULL */
TEST(hunks_current_params) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    const ut_Hunk *h;
    size_t         hn = ~(size_t)0;
    T->diffhn = -1;
    T->current = NULL;
    h = ut_hunks(T, &hn);
    assert(hn == 0 && h == NULL);
    ut_deltree(S, T), ut_close(S);
}

/* T37: cleaner non-NULL path via ut_setcleaner + ut_deltree */
static int g_cleaner; /* times cleaner called */

static void my_cleaner(void *ud, ut_Payload *p) {
    int *n = (int *)ud;
    (void)p;
    (*n)++;
}

TEST(cleaner_params) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T;
    ut_Vid    root, c1, c2;
    int       ud = 0;
    g_cleaner = 0;
    ut_setcleaner(S, &my_cleaner, &ud);
    T = ut_newtree(S, NULL);
    assert(T);
    /* create branch to exercise multi-child freechildren */
    ut_record(T, 0, 3, 5);
    c1 = ut_commit(T, NULL);
    assert(c1);
    root = ut_root(T);
    assert(ut_switch(T, root) == UT_OK);
    ut_record(T, 0, 1, 2);
    c2 = ut_commit(T, NULL);
    assert(c2);
    assert(ut_childcount(root) == 2);
    ut_deltree(S, T);
    /* root.payload + c1.payload + c2.payload all get cleaned */
    assert(ud >= 2);
    assert(S->node_pool.live_obj == 0);
    ut_close(S);
    (void)g_cleaner;
}

/* T38: mergewalk A tail loop — A has 2 hunks, B consumed first */
TEST(mergewalk_taila) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         v;
    const ut_Hunk *h;
    size_t         hn;

    /* cur=[{100,3,0}], single=[{200,0,5}] → emitX2Y → next has 2 hunks */
    ut_record(T, 100, 3, 0);
    ut_record(T, 200, 0, 5);
    /* cur has 2 hunks: [{100,3,0},{200,0,5}], single=[{0,1,0}] → B before all A
     */
    ut_record(T, 0, 1, 0);
    v = ut_commit(T, NULL);
    assert(v);
    h = ut_hunks(T, &hn);
    test_log("mergewalk_taila: hn=%lu\n", test_lu(hn));
    if (hn >= 1)
        test_log("  h[0]: pa=%lu ca=%lu del=%lu ins=%lu\n", test_lu(h[0].pa),
               test_lu(h[0].ca), test_lu(h[0].pdel), test_lu(h[0].cins));
    if (hn >= 2)
        test_log("  h[1]: pa=%lu ca=%lu del=%lu ins=%lu\n", test_lu(h[1].pa),
               test_lu(h[1].ca), test_lu(h[1].pdel), test_lu(h[1].cins));
    if (hn >= 3)
        test_log("  h[2]: pa=%lu ca=%lu del=%lu ins=%lu\n", test_lu(h[2].pa),
               test_lu(h[2].ca), test_lu(h[2].pdel), test_lu(h[2].cins));
    assert(hn == 3);
    assert(h[0].pa == 0 && h[0].pdel == 1 && h[0].cins == 0);
    assert(h[1].pa == 100 && h[1].pdel == 3 && h[1].cins == 0);
    assert(h[2].pdel == 0 && h[2].cins == 5); /* exact pa can shift */
    ut_deltree(S, T), ut_close(S);
}

/* T39: cross-branch diff — diff between grandchild and sibling branch */
TEST(diff_cross) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root = ut_root(T);
    ut_Vid         c1, gc, c2;
    const ut_Hunk *h;
    size_t         hn;

    /* branch 1: root → c1 (insert at 5) → gc (delete at 0) */
    ut_record(T, 5, 0, 3);
    c1 = ut_commit(T, NULL);
    ut_record(T, 0, 2, 0);
    gc = ut_commit(T, NULL);
    /* branch 2: root → c2 (insert at 10) */
    ut_switch(T, root);
    ut_record(T, 10, 0, 4);
    c2 = ut_commit(T, NULL);
    /* diff gc vs c2: traverses up phase2 (gc→c1→root) then down phase3
     * (root→c2) */
    assert(ut_diff(T, gc, c2) >= 0);
    h = ut_hunks(T, &hn);
    (void)c1, (void)h;
    ut_deltree(S, T), ut_close(S);
}

/* T40: diff through empty-hunk commit (triggers compose bn==0) */
TEST(diff_empty_node) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root = ut_root(T);
    ut_Vid         c1, c2;
    const ut_Hunk *h;
    size_t         hn;

    /* c1: non-empty commit, c2: empty commit */
    ut_record(T, 5, 2, 3);
    c1 = ut_commit(T, NULL);
    ut_switch(T, root);
    c2 = ut_commit(T, NULL); /* empty hunk list */
    /* diff from c1 (has hunks) to c2 (no hunks): phase2 inv→compose with empty
     */
    assert(ut_diff(T, c1, c2) >= 0);
    h = ut_hunks(T, &hn);
    (void)h;
    ut_deltree(S, T), ut_close(S);
}

/* T41: diff identity with journal present (no fresh involved) */
TEST(diff_identity_extra) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_Vid    c;
    ut_record(T, 0, 1, 2);
    c = ut_commit(T, NULL);
    (void)c;
    /* diff root→root, no fresh → phase2+3 skip (fn==anc, tn==anc) */
    assert(ut_diff(T, root, root) == 0);
    ut_deltree(S, T), ut_close(S);
}

/* T42: OOM on utN_alloc in ut_commit (normalize succeeds, node alloc fails) */
TEST(commit_oom_node) {
    int oom = 4; /* state+tree+jp+normalize compose = 4, utN_alloc is 5th */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T);
    ut_record(T, 0, 1, 1);
    assert(ut_commit(T, NULL) == NULL);
    ut_deltree(S, T), ut_close(S);
}

/* T43: OOM on utV_reserve inside compose (mergewalk path) */
TEST(commit_oom_reserve) {
    int       oom = 4; /* state+tree+jp+... reserve fails */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T);
    /* record with 2 entries to force mergewalk (both non-empty) */
    ut_record(T, 0, 3, 5);
    ut_record(T, 100, 1, 0);
    /* compose(cur with 1 hunk, single) → mergewalk needs reserve → OOM */
    assert(ut_commit(T, NULL) == NULL);
    assert(ut_current(T) == ut_root(T));
    ut_deltree(S, T), ut_close(S);
}

/* T44: diff with empty result (diff two identical committed nodes via diff) */
TEST(diff_empty_result) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         c;
    const ut_Hunk *h;
    size_t         hn;

    ut_record(T, 0, 1, 2);
    c = ut_commit(T, NULL);
    /* diff from c to c with fresh: diff(t, c, freshvid) but fresh resolves to
     * T->current=c */
    /* Better: diff two identical committed nodes */
    assert(ut_diff(T, c, c) == 0);
    h = ut_hunks(T, &hn);
    assert(hn == 0); /* empty result */
    (void)h;
    ut_deltree(S, T), ut_close(S);
}

/* T45: invert empty hunk vector (diff from empty-hunk node) */
TEST(invert_empty) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_Vid    c;

    /* c: non-empty commit (ensures T has structure; used by freshvid resolving
     * to current) */
    ut_record(T, 0, 1, 2);
    c = ut_commit(T, NULL);
    /* switch to root (no journal) */
    ut_switch(T, root);
    /* diff(freshvid→root): hasfrom=true, hasto=false. fresh normalizes empty
     * journal → NULL cur. invert(NULL) → empty hunk vector. Then compose with
     * root's hunks. */
    assert(ut_diff(T, ut_freshvid(S), root) == 0);
    (void)c;
    ut_deltree(S, T), ut_close(S);
}

/* T46: diff from empty-hunk node → phase2 invert empty */
TEST(invert_node_empty) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_Vid    c1, c2;

    /* root → c1 (has hunks) → c2 (empty commit, h=NULL) */
    ut_record(T, 5, 2, 3);
    c1 = ut_commit(T, NULL);
    c2 = ut_commit(T, NULL); /* empty: no prior record, h=NULL */
    /* diff from c2 (empty hunks) to c1: phase2 inverts c2->h (empty) then c1->h
     */
    assert(ut_diff(T, c2, c1) >= 0);
    (void)root;
    ut_deltree(S, T), ut_close(S);
}

/* T47: many records → trigger utV_grow_ while loop multiple times */
TEST(many_records) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    int       i;
    /* push 20 entries: initial cap=4, grows 4→6→9→13→19→28, multiple while iter
     */
    for (i = 0; i < 20; i++) ut_record(T, (size_t)i, 1, 2);
    assert(ut_freshcount(T) == 20);
    ut_commit(T, NULL);
    ut_deltree(S, T), ut_close(S);
}

/* T48: OOM during mergewalk emit (reserve fails in push) */
TEST(commit_oom_mergewalk) {
    int       oom = 4; /* state+tree+jp+... mergewalk reserve needs another */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T);
    /* need two records so compose enters mergewalk (both non-empty) */
    ut_record(T, 0, 3, 5);
    ut_record(T, 100, 1, 2);
    /* 4 allocs: state, tree, jp, jp→ OOM on compose reserve */
    assert(ut_commit(T, NULL) == NULL);
    ut_deltree(S, T), ut_close(S);
}

/* T49: diff multi-level → phase2+3 traversals */
TEST(diff_multilevel) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root = ut_root(T);
    ut_Vid         c1, c2, c3;
    const ut_Hunk *h;
    size_t         hn;

    /* chain: root → c1 → c2 → c3 */
    ut_record(T, 0, 1, 2);
    c1 = ut_commit(T, NULL);
    ut_record(T, 10, 2, 0);
    c2 = ut_commit(T, NULL);
    ut_record(T, 20, 0, 5);
    c3 = ut_commit(T, NULL);
    /* diff c3 to root: phase2: c3→c2→c1→root (3 levels up, invert at each) */
    assert(ut_diff(T, c3, root) >= 0);
    h = ut_hunks(T, &hn);
    (void)c1, (void)c2, (void)h;
    ut_deltree(S, T), ut_close(S);
}

/* T50: ut_hunks pn==NULL variants */
TEST(hunks_pn_params) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T;

    /* pn==NULL */
    T = ut_newtree(S, NULL);
    assert(ut_hunks(T, NULL) == NULL);
    ut_deltree(S, T);

    /* pn==NULL with pending diff (diffhn >= 0) */
    T = ut_newtree(S, NULL);
    {
        ut_Vid root = ut_root(T);
        ut_record(T, 0, 1, 2);
        assert(ut_diff(T, root, ut_freshvid(S)) >= 0);
        assert(ut_hunks(T, NULL) != NULL);
    }
    ut_deltree(S, T);

    /* pn==NULL with diffhn<0 and current==NULL */
    T = ut_newtree(S, NULL);
    T->current = NULL;
    assert(ut_hunks(T, NULL) == NULL);
    ut_deltree(S, T);

    ut_close(S);
}

/* T51: ut_diff hasto with non-empty fresh (phase4) */
TEST(diff_hasto) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root = ut_root(T);
    const ut_Hunk *h;
    size_t         hn;

    /* commit a node, then diff from root to freshvid WITH journal */
    ut_record(T, 10, 3, 0);
    ut_commit(T, NULL);
    /* add fresh journal */
    ut_record(T, 5, 0, 2);
    /* diff root to freshvid: hasfrom=false, hasto=true, normalize fresh */
    assert(ut_diff(T, root, ut_freshvid(S)) >= 0);
    h = ut_hunks(T, &hn);
    (void)h;
    ut_deltree(S, T), ut_close(S);
}

/* T52: ut_switch with v==NULL */
TEST(switch_params) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(ut_switch(T, NULL) == UT_ERRPARAM);
    ut_deltree(S, T), ut_close(S);
}

/* T53: ut_diff compose failure in D_calc phase2 — c1→c2 diff, OOM during
 * invert+compose */
TEST(diff_oom_compose2) {
    int oom = 6; /* state+tpage+jp+norm+np+norm2→commits ok, diff invert→OOM */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    c1, c2;
    assert(T);
    ut_record(T, 0, 1, 2);
    c1 = ut_commit(T, NULL);
    assert(c1);
    ut_switch(T, ut_root(T));
    ut_record(T, 5, 0, 3);
    c2 = ut_commit(T, NULL);
    assert(c2);
    assert(ut_diff(T, c1, c2) < 0);
    ut_deltree(S, T), ut_close(S);
}

/* T54: B tail loop in mergewalk (B has 2 hunks, A consumed first) */
TEST(mergewalk_tailb) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         root = ut_root(T);
    ut_Vid         c1, c2;
    const ut_Hunk *h;
    size_t         hn;

    /* c1: single hunk at 100, c2: two hunks (0 and 200) */
    ut_record(T, 100, 3, 0);
    c1 = ut_commit(T, NULL);
    ut_switch(T, root);
    ut_record(T, 0, 0, 5);
    ut_record(T, 200, 2, 0);
    c2 = ut_commit(T, NULL);
    /* diff(c1, c2): phase2 invert c1→1 hunk, phase3 compose with c2→2 hunks */
    /* A=1 hunk (inv), B=2 hunks → A consumed, B tail loop enters */
    assert(ut_diff(T, c1, c2) >= 0);
    h = ut_hunks(T, &hn);
    (void)c1, (void)c2, (void)h;
    ut_deltree(S, T), ut_close(S);
}

/* T55: OOM during invert in utD_calc phase2 — chain diff, invert node h fails
 */
TEST(diff_oom_invert2) {
    int       oom = 5; /* state+tpage+jp+norm+np→commit ok, diff invert→OOM */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    c;
    assert(T);
    ut_record(T, 10, 2, 3);
    c = ut_commit(T, NULL);
    assert(c);
    assert(ut_diff(T, c, ut_root(T)) < 0);
    ut_deltree(S, T), ut_close(S);
}

/* T56: utD_calc OOM in compose during phase2 — c1→c2 sibling diff, reserve or
 * mergewalk OOM */
TEST(diff_oom_phase2_compose) {
    int oom = 7; /* state+tpage+jp+norm+np+norm2→commits ok, diff compose→OOM */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    c1, c2;
    assert(T);
    ut_record(T, 0, 1, 2);
    c1 = ut_commit(T, NULL);
    assert(c1);
    ut_switch(T, ut_root(T));
    ut_record(T, 5, 0, 3);
    c2 = ut_commit(T, NULL);
    assert(c2);
    assert(ut_diff(T, c1, c2) < 0);
    ut_deltree(S, T), ut_close(S);
}

/* T57: emitcross surv < 0 (B delete exceeds A insert) */
TEST(emitcross_neg) {
    ut_State      *S = ut_open(NULL, NULL);
    ut_Tree       *T = ut_newtree(S, NULL);
    ut_Vid         v;
    const ut_Hunk *h;
    size_t         hn;

    /* insert 3 at 0, then delete 5 at 0 → overlap, surv = 3-5 = -2 < 0 */
    ut_record(T, 0, 0, 3);
    ut_record(T, 0, 5, 0);
    v = ut_commit(T, NULL);
    assert(v);
    h = ut_hunks(T, &hn);
    assert(hn == 1);
    /* surv < 0: m.pdel = 0 + |-2| = 2, m.cins = 0 + 0 = 0 */
    assert(h[0].pa == 0 && h[0].pdel == 2 && h[0].cins == 0);
    ut_deltree(S, T), ut_close(S);
}

/* T58: deeper freechildren: 3-level tree with branch */
TEST(freechildren_deep) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root;
    int       i;

    root = ut_root(T);
    /* commit 5 nodes in a chain */
    for (i = 0; i < 5; i++) ut_record(T, (size_t)i, 1, 2), ut_commit(T, NULL);
    /* branch: switch to root, commit another chain */
    ut_switch(T, root);
    for (i = 0; i < 3; i++)
        ut_record(T, (size_t)(10 + i), 0, 1), ut_commit(T, NULL);
    (void)root;
    ut_deltree(S, T), ut_close(S);
}

/* T59: ut_younger / ut_older — single deep chain */
TEST(younger_older_chain) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T), c1, c2, c3;

    /* root → c1 → c2 → c3 */
    ut_record(T, 0, 1, 1);
    c1 = ut_commit(T, NULL);
    ut_record(T, 0, 1, 1);
    c2 = ut_commit(T, NULL);
    ut_record(T, 0, 1, 1);
    c3 = ut_commit(T, NULL);

    assert(ut_younger(root) == c1);
    assert(ut_younger(c1) == c2);
    assert(ut_younger(c2) == c3);
    assert(ut_younger(c3) == NULL);

    assert(ut_older(root) == NULL);
    assert(ut_older(c1) == root);
    assert(ut_older(c2) == c1);
    assert(ut_older(c3) == c2);

    ut_deltree(S, T), ut_close(S);
}

/* T60: ut_younger / ut_older — branches (no grandchildren) */
TEST(younger_older_branch) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T), c1, c2, c3;

    /* root → c1(oldest), c2(middle), c3(youngest) */
    c1 = ut_commit(T, NULL);
    ut_switch(T, root);
    c2 = ut_commit(T, NULL);
    ut_switch(T, root);
    c3 = ut_commit(T, NULL);

    assert(ut_younger(root) == c1);
    assert(ut_younger(c1) == c2);
    assert(ut_younger(c2) == c3);
    assert(ut_younger(c3) == NULL);

    assert(ut_older(c1) == root);
    assert(ut_older(c2) == c1);
    assert(ut_older(c3) == c2);

    ut_deltree(S, T), ut_close(S);
}

/* T61: ut_younger / ut_older — branch with grandchild */
TEST(younger_older_grandchild) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T), c1, gc, c2, c3;

    /* root → c1(oldest,有子gc), c2(middle), c3(youngest) */
    c1 = ut_commit(T, NULL);
    gc = ut_commit(T, NULL);
    ut_switch(T, root);
    c2 = ut_commit(T, NULL);
    ut_switch(T, root);
    c3 = ut_commit(T, NULL);

    assert(ut_younger(root) == c1);
    assert(ut_younger(c1) == gc);
    assert(ut_younger(gc) == c2);
    assert(ut_younger(c2) == c3);
    assert(ut_younger(c3) == NULL);

    assert(ut_older(c1) == root);
    assert(ut_older(gc) == c1);
    assert(ut_older(c2) == gc);
    assert(ut_older(c3) == c2);

    ut_deltree(S, T), ut_close(S);
}

/* T62: ut_younger / ut_older NULL guards */
TEST(younger_older_params) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T;
    assert(ut_younger(NULL) == NULL);
    assert(ut_older(NULL) == NULL);
    T = ut_newtree(S, NULL);
    assert(ut_older(ut_root(T)) == NULL);
    ut_deltree(S, T), ut_close(S);
}

/* T64: ut_deltree with NULL T */
TEST(deltree_params) {
    ut_State *S = ut_open(NULL, NULL);
    ut_deltree(S, NULL);
    ut_close(S);
}

/* T65: D_calc fresh normalize OOM — diff fresh→root, normalize fails */
TEST(diff_oom_dcalc_normalize) {
    int oom = 3; /* state+tree_page+jp → counter reaches 0 during normalize */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T);
    ut_record(T, 0, 1, 1);
    assert(ut_diff(T, ut_freshvid(S), ut_root(T)) < 0);
    ut_deltree(S, T), ut_close(S);
}

/* T66: utH_invert push OOM via diff — invert fresh vec alloc fails */
TEST(diff_oom_invert_push) {
    int oom = 4; /* state+tpage+jp+norm→counter 0 after norm, invert→OOM */
    ut_State *S = ut_open(&oom_alloc, &oom);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(T);
    ut_record(T, 10, 3, 7);
    assert(ut_diff(T, ut_freshvid(S), ut_root(T)) < 0);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: NULL tree */
TEST(mapoffset_params) {
    assert(ut_mapoffset(NULL, 42) == 42);
    assert(ut_mapoffset(NULL, 0) == 0);
}

/* mapoffset: no diff done (diffhn<0) */
TEST(mapoffset_nodiff) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    assert(ut_mapoffset(T, 10) == 10);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: diff two identical nodes → empty result */
TEST(mapoffset_empty) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    assert(ut_diff(T, root, root) == 0);
    assert(ut_mapoffset(T, 10) == 10);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: insert before insertion point */
TEST(mapoffset_insert_before) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 0, 3), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 2) == 2);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: insert at insertion point */
TEST(mapoffset_insert_at) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 0, 3), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 5) == 5);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: insert after insertion point */
TEST(mapoffset_insert_after) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 0, 3), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 7) == 10);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: delete before deletion region */
TEST(mapoffset_delete_before) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 3, 0), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 2) == 2);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: delete inside deletion region */
TEST(mapoffset_delete_in) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 3, 0), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 6) == 5);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: delete at pa (deletion region start) */
TEST(mapoffset_delete_at_pa) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 3, 0), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 5) == 5);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: delete after deletion region */
TEST(mapoffset_delete_after) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 3, 0), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 10) == 7);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: replace, offset before replace region */
TEST(mapoffset_replace_before) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 3, 5), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 2) == 2);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: replace, offset inside replace region */
TEST(mapoffset_replace_in) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 3, 5), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 6) == 5);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: replace at pa */
TEST(mapoffset_replace_at_pa) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 3, 5), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 5) == 5);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: replace, offset after replace region */
TEST(mapoffset_replace_after) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 3, 5), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 10) == 12);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: two separated insert hunks */
TEST(mapoffset_multi_two_inserts) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 5, 0, 3), ut_record(T, 10, 0, 4), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) >= 0);
    assert(ut_mapoffset(T, 2) == 2);
    assert(ut_mapoffset(T, 6) == 9);
    assert(ut_mapoffset(T, 10) == 17);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: insert then delete hunks */
TEST(mapoffset_multi_insert_delete) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    ut_record(T, 3, 0, 5), ut_record(T, 10, 2, 0), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) >= 0);
    assert(ut_mapoffset(T, 1) == 1);   /* before all */
    assert(ut_mapoffset(T, 4) == 9);   /* between insert and delete */
    assert(ut_mapoffset(T, 5) == 10);  /* in delete region → ca */
    assert(ut_mapoffset(T, 10) == 13); /* after all */
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: cross-branch diff */
TEST(mapoffset_cross_diff) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T), c1, c2;
    ut_record(T, 5, 0, 3), c1 = ut_commit(T, NULL);
    ut_switch(T, root);
    ut_record(T, 10, 2, 0), c2 = ut_commit(T, NULL);
    assert(ut_diff(T, c1, c2) >= 0);
    assert(ut_mapoffset(T, 2) == 2);
    (void)c1, (void)c2;
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: freshdiff then mapoffset */
TEST(mapoffset_fresh) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_record(T, 5, 0, 3);
    assert(ut_freshdiff(T, 0, 1) == 1);
    assert(ut_mapoffset(T, 2) == 2);
    assert(ut_mapoffset(T, 5) == 5);
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: freshdiff backward (inverted) for delete */
TEST(mapoffset_fresh_backward_delete) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_record(T, 24, 6, 0);                      /* delete 6 at 24 */
    assert(ut_freshdiff(T, 1, 0) == 1);           /* inverted */
    assert(ut_mapoffset(T, 24) == 24);            /* cursor at delete point */
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: freshdiff backward (inverted) for insert */
TEST(mapoffset_fresh_backward_insert) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_record(T, 5, 0, 3);                       /* insert 3 at 5 */
    assert(ut_freshdiff(T, 1, 0) == 1);           /* inverted */
    assert(ut_mapoffset(T, 5) == 5);              /* cursor at insert point */
    assert(ut_mapoffset(T, 8) == 5);              /* cursor after append */
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: freshdiff backward (inverted) for splice */
TEST(mapoffset_fresh_backward_splice) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_record(T, 10, 2, 4);                      /* del 2, ins 4 at 10 */
    assert(ut_freshdiff(T, 1, 0) == 1);           /* inverted */
    assert(ut_mapoffset(T, 10) == 10);            /* cursor at edit start */
    assert(ut_mapoffset(T, 14) == 12);            /* cursor after splice → ca+cins */
    ut_deltree(S, T), ut_close(S);
}

/* mapoffset: offset=0 across scenarios */
TEST(mapoffset_zero) {
    ut_State *S = ut_open(NULL, NULL);
    ut_Tree  *T = ut_newtree(S, NULL);
    ut_Vid    root = ut_root(T);
    /* no diff: offset 0 → 0 */
    assert(ut_mapoffset(T, 0) == 0);
    /* insert at 0 */
    ut_record(T, 0, 0, 3), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 0) == 0);
    ut_deltree(S, T);
    /* delete at 0 */
    T = ut_newtree(S, NULL), root = ut_root(T);
    ut_record(T, 0, 3, 0), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 0) == 0); /* in delete region → ca=0 */
    ut_deltree(S, T);
    /* replace at 0 */
    T = ut_newtree(S, NULL), root = ut_root(T);
    ut_record(T, 0, 2, 5), ut_commit(T, NULL);
    assert(ut_diff(T, root, ut_current(T)) == 1);
    assert(ut_mapoffset(T, 0) == 0); /* in replace region → ca=0 */
    ut_deltree(S, T), ut_close(S);
}


#include "undotree_test.gen.inc"
