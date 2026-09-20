#define AT_FANOUT    4
#define AT_MAX_LEVEL 64 /* AT_FANOUT=4: see docs/max_levels.md */
#define AT_PAGE_SIZE 512
#define AT_STATIC_API

#include "../anchortree.h"
#include "at_tests.h"
#include "tests.h"

/* Full anchortree suite at the fanout-4 layout.
 *
 * Fanout-independent cases live here; odd-layout regressions live in
 * anchortree_test_fanout9.c and default-config stress in
 * anchortree_test_fanout32.c. */

TEST_STATIC int atT_insertseq(at_Tree *T, const size_t *pos, int n) {
    at_Edge *e;
    int      i;
    for (i = 0; i < n; ++i)
        if (atT_setupedge(T, pos[i], &e) != AT_OK) return 0;
    return at_checktree(T);
}
TEST_STATIC int atT_positionsmatch(at_Tree *T, const size_t *pos, int n) {
    at_Seq s;
    int    i;
    if (atT_seq(T, &s) != n) return 0;
    for (i = 0; i < n; ++i)
        if (s.pos[i] != pos[i]) return 0;
    return 1;
}

TEST_STATIC void atT_rangeeq(at_Tree *T, at_Id id, size_t s, size_t e) {
    size_t x, y;
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, s);
    asserteq(y, e);
}

#define AT_BRUTE_N 12
TEST_STATIC const size_t att_brute_pos[AT_BRUTE_N] = {
        10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 100, 100};
TEST_STATIC const size_t att_brute_len[AT_BRUTE_N] = {4, 3, 5, 2, 6, 3,
                                                      4, 2, 5, 0, 0, 0};
TEST_STATIC const int    att_brute_grav[AT_BRUTE_N] = {
        AT_GROW,  AT_FIXED, AT_SHIFT, AT_LOCK, AT_GROW, AT_FIXED,
        AT_SHIFT, AT_LOCK,  AT_GROW,  AT_GROW, AT_LOCK, AT_FIXED};
TEST_STATIC void atT_brutebuild(at_Tree *T, att_Model *m) {
    at_Cursor C;
    int       i;
    for (i = 0; i < AT_BRUTE_N; ++i) {
        assertok(at_seek(&C, T, att_brute_pos[i]) == AT_OK);
        m[i].gravity = att_brute_grav[i];
        assertok(
                at_newanchor(&C, att_brute_len[i], 0, m[i].gravity, &m[i].id)
                == AT_OK);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
    }
}
TEST_STATIC int atT_splicebruteok(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    att_Model m[AT_BRUTE_N];
    size_t    pos, del, ins, limit;
    atT_brutebuild(T, m);
    limit = at_bytes(T) + 1;
    for (pos = 0; pos <= limit; ++pos) {
        for (del = 0; del <= limit; ++del) {
            for (ins = 0; ins <= 2; ++ins) {
                att_Splice w;
                at_reset(T), atT_brutebuild(T, m);
                w.L = pos, w.del = del, w.ins = ins;
                atT_modelsplice(m, AT_BRUTE_N, &w);
                assertok(at_seek(&C, T, pos) == AT_OK);
                assertok(at_splice(&C, del, ins) == AT_OK);
                assertok(at_checktree(T));
                assertok(atT_noemptyedges(T));
                assertok(at_checkcursor(&C, pos + ins));
                assertok(atT_modelok(T, m, AT_BRUTE_N));
            }
        }
    }
    at_deltree(T), at_close(S);
    return 1;
}

TEST(splice_brute) { assertok(atT_splicebruteok()); }

TEST_STATIC void atT_randomsplicerun(at_Tree *T, att_Model *m, int n) {
    at_Cursor  C;
    att_Splice w;
    size_t     L, del, ins, bound;
    size_t     seed = 7;
    int        step;
    for (step = 0; step < 300; ++step) {
        bound = atT_modelbound(m, n);
        L = atT_lcg(&seed) % (bound + 20u);
        del = atT_lcg(&seed) % 25u;
        ins = atT_lcg(&seed) % 10u;
        w.L = L, w.del = del, w.ins = ins;
        atT_modelsplice(m, n, &w);
        assertok(at_seek(&C, T, L) == AT_OK);
        assertok(at_splice(&C, del, ins) == AT_OK);
        assertok(at_checktree(T));
        assertok(atT_noemptyedges(T));
        assertok(at_checkcursor(&C, L + ins));
        assertok(atT_modelok(T, m, n));
    }
}

TEST_STATIC int atT_splicerandomok(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    att_Model m[10];
    int       i;
    for (i = 0; i < 10; ++i) {
        m[i].gravity = i % 4;
        m[i].id = atT_anew(
                T, (size_t)(10 + i * 3), (size_t)(i * 2 + 1), m[i].gravity);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
        assertok(at_checktree(T));
        assertok(atT_noemptyedges(T));
    }
    atT_randomsplicerun(T, m, 10);
    at_deltree(T), at_close(S);
    return 1;
}

TEST(splice_random_model) { assertok(atT_splicerandomok()); }

TEST_STATIC int atT_cns(at_Tree *T, size_t p, int ns, at_Id *out, int cap) {
    at_Cursor C;
    at_Id     id;
    int       n = 0;
    assertok(at_seek(&C, T, p) == AT_OK);
    while ((id = at_next(&C, ns, NULL)) != AT_NONE) {
        assertok(n < cap);
        out[n] = id, n += 1;
    }
    return n;
}
TEST_STATIC int atT_collect(at_Tree *T, size_t pos, at_Id *out, int cap) {
    return atT_cns(T, pos, 0, out, cap);
}
TEST_STATIC int atT_idset(const at_Id *got, int n, const at_Id *want, int m) {
    int i, j, hit;
    if (n != m) return 0;
    for (i = 0; i < n; ++i) {
        hit = 0;
        for (j = 0; j < m; ++j)
            if (got[i] == want[j]) hit = 1;
        if (!hit) return 0;
        for (j = i + 1; j < n; ++j)
            if (got[i] == got[j]) return 0;
    }
    return 1;
}

TEST_STATIC void atT_lifecyclebasic(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Id     id;
    assertok(S != NULL && T != NULL);
    assertok(at_checktree(T));
    asserteq(at_bytes(NULL), 0);
    asserteq(at_bytes(T), 0);
    /* setup-only: force the first node/edge allocation through pages. */
    (void)atT_drainpool(&S->nodes), (void)atT_drainpool(&S->edges);
    atT_anchor(T, 0, 5, AT_GROW, &id);
    assertok(at_checktree(T));
    asserteq(at_bytes(T), 5);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void atT_lifecyclereset(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    int       i;
    for (i = 1; i <= 9; ++i)
        assertok(atT_setupedge(T, (size_t)i * 10u, &e) == AT_OK);
    assertok(at_checktree(T));
    at_reset(T);
    assertok(at_checktree(T));
    asserteq(at_bytes(T), 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
    asserteq(at_newtree(NULL), NULL);
}

TEST(lifecycle) {
    asserteq(AT_NONE, 0);
    atT_lifecyclebasic();
    atT_lifecyclereset();
}

TEST_STATIC void atT_guardsopen(void) {
    OomCount  oc0 = {0, 0}, oc1 = {0, 1};
    at_State *S;
    assertok(at_open(&oomcount_alloc, &oc0) == NULL);
    at_close(NULL), at_deltree(NULL), at_reset(NULL);
    S = at_open(&oomcount_alloc, &oc1);
    assertok(S != NULL);
    assertok(at_newtree(S) == NULL);
    at_close(S);
}

TEST_STATIC void atT_guardscursor(at_Tree *T) {
    at_Cursor C, BadC;
    at_Id     id;
    memset(&C, 0, sizeof(C)), memset(&BadC, 0, sizeof(BadC));
    assertok(at_seek(NULL, T, 0) == AT_ERRPARAM);
    assertok(at_seek(&C, NULL, 0) == AT_ERRPARAM);
    assertok(at_locate(NULL, 0) == AT_ERRPARAM);
    assertok(at_locate(&BadC, 0) == AT_ERRPARAM);
    assertok(at_advance(NULL, 0) == AT_ERRPARAM);
    assertok(at_advance(&BadC, 0) == AT_ERRPARAM);
    BadC.tree = T;
    assertok(at_advance(&BadC, 0) == AT_ESTALE);
    assertok(at_splice(&BadC, 0, 0) == AT_ESTALE);
    assertok(at_locate(&BadC, 0) == AT_OK);
    assertok(at_splice(&BadC, 0, 0) == AT_OK);
    assertok(at_newanchor(&BadC, 0, -1, AT_GROW, &id) == AT_ERRPARAM);
    assertok(
            at_newanchor(&BadC, 0, (int)AT_MAX_NS + 1, AT_GROW, &id)
            == AT_ERRPARAM);
    BadC.tree = NULL;
    assertok(at_splice(&BadC, 0, 0) == AT_ERRPARAM);
    assertok(at_newanchor(&BadC, 0, 0, AT_GROW, &id) == AT_ERRPARAM);
}

TEST_STATIC void atT_guardssetup(at_Tree *T) {
    at_Edge *e;
    size_t   x = 0, y = 0;
    assertok(atT_setupedge(NULL, 5, &e) == AT_ERRPARAM);
    assertok(atT_setupedge(T, 5, NULL) == AT_ERRPARAM);
    assertok(atT_setupdropedge(NULL, 1) == AT_ERRPARAM);
    assertok(atT_setupdropedge(T, 0) == AT_ERRPARAM);
    assertok(atT_setupdropedge(T, 1) == AT_ERRPARAM);
    assertok(at_range(NULL, 1, &x, &y) == AT_ERRPARAM);
    assertok(at_range(T, 1, &x, NULL) == AT_ERRPARAM);
    assertok(at_gravity(NULL, 1) == AT_ERRPARAM);
}

TEST(param_guards) {
    at_State *S;
    at_Tree  *T;
    atT_guardsopen();
    S = at_open(NULL, NULL);
    T = at_newtree(S);
    atT_guardscursor(T);
    atT_guardssetup(T);
    at_deltree(T), at_close(S);
}

TEST_STATIC void atT_edgeguardspos(at_Tree *T) {
    at_Cursor C;
    at_Edge  *e;
    at_Id     id;
    assertok(at_seek(&C, T, (size_t)-1) == AT_OK);
    assertok(at_newanchor(&C, 1, 0, AT_GROW, &id) == AT_ERROVERFLOW);
    assertok(at_seek(&C, T, 1) == AT_OK);
    assertok(at_newanchor(&C, (size_t)-1, 0, AT_GROW, &id) == AT_ERROVERFLOW);
    assertok(atT_setupinsert(T, 0, 0) == AT_OK);
    assertok(atT_setupinsert(T, 0, 7) == AT_OK);
    assertok(atT_setupedge(T, 10, &e) == AT_OK);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, 0) == AT_OK);
    assertok(atT_setupfindedge(T, 0, &e) == 1);
    assertok(atT_setupfindedge(T, 11, &e) == 0);
    assertok(atT_setupdropedge(T, 5) == AT_ERRPARAM);
}

TEST_STATIC void atT_edgeguardsidmax(at_Tree *T) {
    at_Cursor C;
    at_Id     id;
    size_t    x = 0, y = 0, oldcap;
    assertok(at_seek(&C, T, 0) == AT_OK);
    /* setup-only: force id-pool growth guards without growing the pool. */
    oldcap = T->idmax;
    T->idmax = 1, T->idfree = 0;
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_ERRMEM);
    T->idmax = AT_ANCHOR_MAX, T->idfree = 0;
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_ERRMEM);
    T->idmax = (at_Id)oldcap;
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_OK);
    assertok(at_delanchor(T, 1000) == AT_ERRPARAM);
    assertok(at_range(T, 1000, &x, &y) == AT_ERRPARAM);
    assertok(at_gravity(T, 1000) == AT_ERRPARAM);
    assertok(atT_checkchains(T));
}

TEST(edge_guards) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    atT_edgeguardspos(T);
    atT_edgeguardsidmax(T);
    at_deltree(T), at_close(S);
}

TEST(empty_seek) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    assertok(at_locate(NULL, 0) == AT_ERRPARAM);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_locate(&C, 0) == AT_OK);
    assertok(at_checkcursor(&C, 0));
    assertok(at_seek(&C, T, 5) == AT_OK);
    assertok(at_checkcursor(&C, 5));
    assertok(at_advance(&C, 3) == AT_OK);
    assertok(at_checkcursor(&C, 8));
    assertok(at_advance(&C, -20) == AT_OK);
    assertok(at_checkcursor(&C, 0));
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_advance(&C, -3) == AT_OK);
    assertok(at_checkcursor(&C, 7));
    at_deltree(T), at_close(S);
}

TEST(seek_positions) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Edge            *e;
    at_Cursor           C;
    static const size_t seqp[4] = {0, 10, 30, 50};
    static const size_t seeks[10] = {0, 5, 10, 15, 25, 30, 35, 50, 60, 70};
    int                 i;
    assertok(atT_setupedge(T, 10, &e) == AT_OK);
    assertok(atT_setupedge(T, 50, &e) == AT_OK);
    assertok(atT_setupedge(T, 30, &e) == AT_OK);
    assertok(atT_positionsmatch(T, seqp, 4));
    for (i = 0; i < 10; ++i) {
        assertok(at_seek(&C, T, seeks[i]) == AT_OK);
        assertok(at_checkcursor(&C, seeks[i]));
        assertok(at_offset(&C) == seeks[i]);
    }
    assertok(at_locate(&C, 35) == AT_OK);
    assertok(at_checkcursor(&C, 35));
    at_deltree(T), at_close(S);
}

TEST(advance_positions) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Edge            *e;
    at_Cursor           C;
    static const size_t seqp[4] = {0, 10, 30, 50};
    int                 i;
    assertok(atT_setupedge(T, 10, &e) == AT_OK);
    assertok(atT_setupedge(T, 50, &e) == AT_OK);
    assertok(atT_setupedge(T, 30, &e) == AT_OK);
    assertok(atT_positionsmatch(T, seqp, 4));
    for (i = 0; i < 55; ++i) {
        assertok(at_seek(&C, T, 0) == AT_OK);
        assertok(at_advance(&C, (at_Delta)i) == AT_OK);
        assertok(at_checkcursor(&C, (size_t)i));
        assertok(at_advance(&C, -1) == AT_OK);
        assertok(at_checkcursor(&C, i ? (size_t)(i - 1) : 0));
    }
    assertok(at_seek(&C, T, 1000) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_checkcursor(&C, 1000));
    at_deltree(T), at_close(S);
}

TEST(advance_span) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    at_Cursor C;
    int       i;
    for (i = 1; i <= 20; ++i)
        assertok(atT_setupedge(T, (size_t)i * 10, &e) == AT_OK);
    /* setup-only: require a multi-level tree for crossing advances. */
    assertok(T->levels > 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_advance(&C, 95) == AT_OK);
    assertok(at_checkcursor(&C, 95));
    assertok(at_advance(&C, 60) == AT_OK);
    assertok(at_checkcursor(&C, 155));
    assertok(at_advance(&C, -40) == AT_OK);
    assertok(at_checkcursor(&C, 115));
    assertok(at_advance(&C, -115) == AT_OK);
    assertok(at_checkcursor(&C, 0));
    assertok(at_advance(&C, 200) == AT_OK);
    assertok(at_checkcursor(&C, 200));
    at_deltree(T), at_close(S);
}

TEST(split_sequence) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    p[51], i;
    at_Edge  *e;
    p[0] = 0;
    for (i = 0; i < 50; ++i) p[i + 1] = (i + 1) * 13u;
    assertok(atT_insertseq(T, p + 1, 50));
    assertok(atT_positionsmatch(T, p, 51));
    assertok(atT_setupedge(T, 650, &e) == AT_OK);
    assertok(atT_positionsmatch(T, p, 51));
    at_deltree(T), at_close(S);
}

TEST(delete_middle) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    before[5] = {10, 30, 50, 80, 100};
    size_t    after[5] = {0, 10, 50, 80, 100};
    size_t    extended[6] = {0, 10, 50, 80, 100, 650};
    at_Edge  *e;
    assertok(atT_insertseq(T, before, 5));
    assertok(atT_setupdropedge(T, 30) == AT_OK);
    assertok(atT_positionsmatch(T, after, 5));
    assertok(atT_setupedge(T, 650, &e) == AT_OK);
    assertok(atT_positionsmatch(T, extended, 6));
    at_deltree(T), at_close(S);
}

TEST(delete_last) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    before[5] = {10, 30, 50, 80, 100};
    size_t    after[5] = {0, 10, 30, 50, 80};
    size_t    split[6] = {0, 10, 30, 50, 80, 90};
    at_Edge  *e;
    assertok(atT_insertseq(T, before, 5));
    assertok(atT_setupdropedge(T, 100) == AT_OK);
    assertok(at_bytes(T) == 100);
    assertok(atT_positionsmatch(T, after, 5));
    assertok(atT_setupedge(T, 90, &e) == AT_OK);
    assertok(at_bytes(T) == 100);
    assertok(atT_positionsmatch(T, split, 6));
    at_deltree(T), at_close(S);
}

TEST(delete_all) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    p[50], i;
    at_Edge  *e;
    for (i = 0; i < 50; ++i) p[i] = (i + 1) * 7u;
    assertok(atT_insertseq(T, p, 50));
    for (i = 0; i < 50; ++i) assertok(atT_setupdropedge(T, p[i]) == AT_OK);
    assertok(at_bytes(T) == 350);
    assertok(at_checktree(T));
    assertok(atT_setupedge(T, 123, &e) == AT_OK);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

TEST(random_mutate) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    size_t    p;
    unsigned  seed = 1;
    int       i;
    for (i = 0; i < 200; ++i) {
        seed = seed * 1664525u + 1013904223u;
        p = (seed % 5000u) + 1u;
        assertok(atT_setupedge(T, p, &e) == AT_OK);
        assertok(at_checktree(T));
    }
    for (i = 0; i < 300; ++i) {
        seed = seed * 1664525u + 1013904223u;
        p = (seed % 5000u) + 1u;
        /* setup-only: find an existing zero-offset edge to drop. */
        if (p <= at_bytes(T) && atT_setupfindedge(T, p, &e))
            assertok(atT_setupdropedge(T, p) == AT_OK);
        assertok(at_checktree(T));
    }
    at_deltree(T), at_close(S);
}

TEST(anchor_id_reuse) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c;
    size_t    x, y;
    atT_anchor(T, 10, 5, AT_GROW, &a);
    atT_anchor(T, 20, 5, AT_FIXED, &b);
    asserteq(a, 1);
    asserteq(b, 2);
    assertok(at_delanchor(T, a) == AT_OK);
    atT_anchor(T, 30, 5, AT_SHIFT, &c);
    asserteq(c, a);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    asserteq(x, 30);
    asserteq(y, 35);
    assertok(at_gravity(T, c) == AT_SHIFT);
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(at_delanchor(T, c) == AT_OK);
    at_deltree(T), at_close(S);
}

TEST(anchor_gravities) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               id[4];
    static const int    gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    static const size_t ps[4] = {10, 40, 80, 120};
    int                 i;
    for (i = 0; i < 4; ++i) atT_anchor(T, ps[i], 10, gv[i], &id[i]);
    for (i = 0; i < 4; ++i) assertok(at_gravity(T, id[i]) == gv[i]);
    for (i = 0; i < 4; ++i) assertok(at_delanchor(T, id[i]) == AT_OK);
    asserteq(at_bytes(T), 130);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

TEST(anchor_zero_length) {
    at_State        *S = at_open(NULL, NULL);
    at_Tree         *T = at_newtree(S);
    at_Id            id[4];
    static const int gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    size_t           x, y;
    int              i;
    for (i = 0; i < 4; ++i) atT_anchor(T, 100, 0, gv[i], &id[i]);
    for (i = 0; i < 4; ++i) {
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        asserteq(x, 100);
        asserteq(y, 100);
        assertok(at_gravity(T, id[i]) == gv[i]);
    }
    for (i = 0; i < 4; ++i) assertok(at_delanchor(T, id[i]) == AT_OK);
    asserteq(at_bytes(T), 100);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

TEST(anchor_ns_tag) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id, got[2], want[1];
    size_t    x, y, len;
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_newanchor(&C, 0, 7, AT_SHIFT, &id) == AT_OK);
    want[0] = id;
    assertok(at_range(T, id, &x, &y) == AT_OK);
    assertok(x == 10 && y == 10);
    assertok(atT_cns(T, 0, 7, got, 2) == 1 && atT_idset(got, 1, want, 1));
    assertok(atT_cns(T, 0, 8, got, 2) == 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 8, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 7, &len) == id);
    asserteq(len, 0);
    assertok(atT_checkchains(T));
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 0, 3) == AT_OK);
    assertok(at_range(T, id, &x, &y) == AT_OK);
    assertok(x == 13 && y == 13);
    assertok(atT_cns(T, 0, 7, got, 2) == 1 && atT_idset(got, 1, want, 1));
    assertok(atT_cns(T, 0, 0, got, 2) == 1 && atT_idset(got, 1, want, 1));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST(anchor_ns_relocate) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, got[2], want[1];
    size_t    x, y, len;
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_newanchor(&C, 0, 7, AT_GROW, &a) == AT_OK);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_newanchor(&C, 5, 9, AT_LOCK, &b) == AT_OK);
    assertok(at_seek(&C, T, 5) == AT_OK);
    assertok(at_splice(&C, 20, 0) == AT_OK);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 5 && y == 5);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 5 && y == 5);
    want[0] = a;
    assertok(atT_cns(T, 0, 7, got, 2) == 1 && atT_idset(got, 1, want, 1));
    want[0] = b;
    assertok(atT_cns(T, 0, 9, got, 2) == 1 && atT_idset(got, 1, want, 1));
    assertok(atT_cns(T, 0, 8, got, 2) == 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 7, &len) == a);
    asserteq(len, 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 9, &len) == b);
    asserteq(len, 0);
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST(anchor_range_positions) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               a, b, c;
    size_t              x, y;
    static const size_t expect[6] = {0, 5, 20, 23, 1000, 1010};
    atT_anchor(T, 0, 5, AT_GROW, &a);
    atT_anchor(T, 1000, 10, AT_FIXED, &b);
    atT_anchor(T, 20, 3, AT_SHIFT, &c);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    asserteq(x, 0);
    asserteq(y, 5);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    asserteq(x, 1000);
    asserteq(y, 1010);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    asserteq(x, 20);
    asserteq(y, 23);
    assertok(atT_positionsmatch(T, expect, 6));
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(at_delanchor(T, c) == AT_OK);
    asserteq(at_bytes(T), 1010);
    at_deltree(T), at_close(S);
}

TEST(anchor_query) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C, D, E;
    at_Id     a, b, c, d, id;
    size_t    len, x, y;
    int       ns;

    memset(&D, 0, sizeof(D));
    atT_mkns(T, 0, 5, 3, &a);
    atT_mkns(T, 10, 0, 7, &b);
    atT_anchor(T, 20, 0, AT_FIXED, &d);
    atT_anchor(T, 50, 0, AT_GROW, &c);

    /* live query after at_next: ns and length are written */
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
    id = at_anchor(&C, &ns, &len);
    asserteq(id, a);
    asserteq(ns, 3);
    asserteq(len, 5);
    id = at_anchor(&C, &ns, &len);
    asserteq(id, a); /* read-only: repeatable, scan not advanced */
    assertok(at_checkcursor(&C, 0));
    assertok(at_next(&C, 0, NULL) == b);
    assertok(at_anchor(&C, &ns, &len) == b);
    asserteq(ns, 7);
    asserteq(len, 0);
    assertok(at_anchor(&C, NULL, &len) == b);
    asserteq(len, 0);
    assertok(at_anchor(&C, &ns, NULL) == b);
    asserteq(ns, 7);
    assertok(at_anchor(&C, NULL, NULL) == b);

    /* AT_FIXED zero-length anchor crossed by insert: start > end */
    assertok(at_seek(&E, T, 20) == AT_OK);
    assertok(at_insert(&E, 4) == AT_OK);
    assertok(at_range(T, d, &x, &y) == AT_OK);
    assertok(x > y);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_next(&C, 0, NULL) == b);
    assertok(at_next(&C, 0, NULL) == d);
    assertok(at_anchor(&C, &ns, &len) == d);
    asserteq(len, 0);

    /* exhausted scan: atC_done(C) */
    assertok(at_next(&C, 0, NULL) == c);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    len = 99;
    assertok(at_anchor(&C, &ns, &len) == AT_NONE);
    asserteq(len, 0);

    /* atC_fresh(C): seek without consuming a slot */
    assertok(at_seek(&C, T, 0) == AT_OK);
    len = 99;
    assertok(at_anchor(&C, &ns, &len) == AT_NONE);
    asserteq(len, 0);

    /* NULL cursor, unbound cursor and stale cursor */
    len = 99;
    assertok(at_anchor(NULL, &ns, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_anchor(NULL, NULL, NULL) == AT_NONE);
    len = 99;
    assertok(at_anchor(&D, &ns, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_seek(&E, T, 0) == AT_OK);
    assertok(at_insert(&E, 1) == AT_OK);
    len = 99;
    assertok(at_anchor(&C, &ns, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

TEST(anchor_delete_cleans) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               a, b;
    size_t              x, y;
    static const size_t two[3] = {0, 20, 25};
    static const size_t four[5] = {0, 20, 25, 100, 110};
    atT_anchor(T, 20, 5, AT_LOCK, &b);
    atT_anchor(T, 100, 10, AT_GROW, &a);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    asserteq(x, 100);
    asserteq(y, 110);
    assertok(atT_positionsmatch(T, four, 5));
    assertok(at_delanchor(T, a) == AT_OK);
    asserteq(at_bytes(T), 110);
    assertok(atT_positionsmatch(T, two, 3));
    assertok(at_delanchor(T, b) == AT_OK);
    asserteq(at_bytes(T), 110);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

TEST(anchor_lifecycle_ids) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, reuse;
    size_t    x, y;
    assertok(S != NULL && T != NULL);
    atT_anchor(T, 10, 5, AT_GROW, &a);
    atT_anchor(T, 30, 7, AT_LOCK, &b);
    assertok(a != b);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 10 && y == 15);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 30 && y == 37);
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_range(T, a, &x, &y) == AT_ERRPARAM);
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(at_range(T, b, &x, &y) == AT_ERRPARAM);
    /* public contract: the new id is valid and the unchosen freed id stays
     * invalid; freelist reuse order (e.g. LIFO) is not promised. */
    atT_anchor(T, 40, 1, AT_FIXED, &reuse);
    assertok(at_range(T, reuse, &x, &y) == AT_OK);
    assertok(x == 40 && y == 41);
    assertok(at_range(T, reuse == a ? b : a, &x, &y) == AT_ERRPARAM);
    at_reset(T);
    assertok(at_range(T, reuse, &x, &y) == AT_ERRPARAM);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    atT_anchor(T, 20, 4, AT_FIXED, &a);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 20 && y == 24);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(anchor_invalid_params) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x = 0, y = 0;
    assertok(at_newanchor(NULL, 0, 0, AT_GROW, &id) == AT_ERRPARAM);
    assertok(at_delanchor(NULL, 1) == AT_ERRPARAM);
    assertok(at_delanchor(T, 0) == AT_ERRPARAM);
    assertok(at_delanchor(T, 1) == AT_ERRPARAM);
    assertok(at_range(NULL, 1, &x, &y) == AT_ERRPARAM);
    assertok(at_range(T, 1, &x, &y) == AT_ERRPARAM);
    assertok(at_range(T, 1, NULL, &y) == AT_ERRPARAM);
    assertok(at_gravity(T, 1) < 0);
    assertok(at_gravity(T, 0) < 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_newanchor(&C, 0, 0, 99, &id) == AT_ERRPARAM);
    assertok(at_newanchor(&C, 0, 0, -1, &id) == AT_ERRPARAM);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, NULL) == AT_ERRPARAM);
    atT_anchor(T, 50, 5, AT_GROW, &id);
    assertok(at_delanchor(T, id) == AT_OK);
    assertok(at_delanchor(T, id) == AT_ERRPARAM);
    assertok(at_range(T, id, &x, &y) == AT_ERRPARAM);
    assertok(at_gravity(T, id) < 0);
    at_deltree(T), at_close(S);
}

TEST(splice_gap_delete_insert) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x, y;
    atT_anchor(T, 20, 5, AT_GROW, &id);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 3, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_checkcursor(&C, 10));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 17);
    asserteq(y, 22);
    asserteq(at_bytes(T), 22);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 0, 4) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 21);
    asserteq(y, 26);
    asserteq(at_bytes(T), 26);
    at_deltree(T), at_close(S);
}

TEST(splice_edge_gravity_insert) {
    static const int    gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    static const size_t ex[4][2] = {
            {100, 107}, {107, 100}, {107, 107}, {100, 100}};
    int i;
    for (i = 0; i < 4; ++i) {
        at_State *S = at_open(NULL, NULL);
        at_Tree  *T = at_newtree(S);
        at_Cursor C;
        at_Id     id;
        size_t    x, y;
        atT_anchor(T, 100, 0, gv[i], &id);
        assertok(at_seek(&C, T, 100) == AT_OK);
        assertok(at_splice(&C, 0, 7) == AT_OK);
        assertok(at_checktree(T));
        assertok(atT_noemptyedges(T));
        assertok(at_checkcursor(&C, 107));
        assertok(at_gravity(T, id) == gv[i]);
        assertok(at_range(T, id, &x, &y) == AT_OK);
        asserteq(x, ex[i][0]);
        asserteq(y, ex[i][1]);
        asserteq(at_bytes(T), 107);
        at_deltree(T), at_close(S);
    }
}

TEST(splice_delete_spanning) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c;
    size_t    x, y;
    atT_anchor(T, 10, 5, AT_GROW, &a);
    atT_anchor(T, 30, 5, AT_LOCK, &b);
    atT_anchor(T, 50, 5, AT_FIXED, &c);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, 25, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_checkcursor(&C, 20));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    asserteq(x, 10);
    asserteq(y, 15);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    asserteq(x, 20);
    asserteq(y, 20);
    assertok(at_gravity(T, b) == AT_LOCK);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    asserteq(x, 25);
    asserteq(y, 30);
    asserteq(at_bytes(T), 30);
    at_deltree(T), at_close(S);
}

TEST(splice_delete_last_insert) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x, y;
    atT_anchor(T, 100, 10, AT_GROW, &id);
    assertok(at_seek(&C, T, 80) == AT_OK);
    assertok(at_splice(&C, 40, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 80);
    asserteq(y, 80);
    asserteq(at_bytes(T), 80);
    assertok(at_seek(&C, T, 80) == AT_OK);
    assertok(at_splice(&C, 0, 6) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 80);
    asserteq(y, 86);
    asserteq(at_bytes(T), 86);
    at_deltree(T), at_close(S);
}

TEST(splice_zero_cross) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x, y;
    atT_anchor(T, 100, 0, AT_FIXED, &id);
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_splice(&C, 0, 7) == AT_OK);
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 107);
    asserteq(y, 100);
    assertok(at_seek(&C, T, 105) == AT_OK);
    assertok(at_splice(&C, 0, 4) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_gravity(T, id) == AT_FIXED);
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 111);
    asserteq(y, 100);
    asserteq(at_bytes(T), 111);
    at_deltree(T), at_close(S);
}

TEST(splice_params) {
    at_State *S;
    at_Tree  *T;
    at_Cursor C;
    assertok(at_splice(NULL, 0, 0) == AT_ERRPARAM);
    S = at_open(NULL, NULL);
    T = at_newtree(S);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, 0) == AT_OK);
    assertok(at_checkcursor(&C, 0));
    assertok(atT_setupinsert(T, 0, 7) == AT_OK);
    assertok(atT_setupinsert(T, 5, 7) == AT_OK);
    assertok(atT_checkchains(T));
    assertok(at_bytes(T) == 14);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_splice(&C, 5, 0) == AT_OK);
    assertok(at_checkcursor(&C, 100));
    assertok(at_checktree(T));
    asserteq(at_bytes(T), 14);
    at_deltree(T), at_close(S);
}

TEST_STATIC size_t atT_poolcap(size_t obj_size) {
    return (AT_PAGE_SIZE - sizeof(void *) - obj_size) / obj_size + 1u;
}

TEST_STATIC size_t atT_poolpages(size_t obj_size, size_t need) {
    size_t cap = atT_poolcap(obj_size);
    return (need + cap - 1u) / cap;
}

#define ATT_ANCHOR_SNAP 128
typedef struct {
    at_State  *state;
    size_t     bytes;
    int        levels;
    unsigned   epoch;
    at_Node    root;
    at_Anchor *anchors;
    at_Id      idmax;
    at_Id      idfree;
    at_Edge   *seam;
    int        live;
    int        nslot;
    at_Anchor  slot[ATT_ANCHOR_SNAP];
} att_TreeSnap;

TEST_STATIC void atT_treesnap(const at_Tree *T, att_TreeSnap *s) {
    assertok(T != NULL && s != NULL);
    assertok((int)T->idmax <= ATT_ANCHOR_SNAP);
    memset(s, 0, sizeof(*s));
    s->state = T->S, s->bytes = T->bytes, s->levels = T->levels;
    s->epoch = T->epoch, s->root = T->root, s->anchors = T->anchors;
    s->idmax = T->idmax, s->idfree = T->idfree, s->seam = T->seam;
    s->live = atT_liveanchors(T);
    if (T->anchors != NULL && T->idmax > 0) {
        s->nslot = (int)T->idmax;
        memcpy(s->slot, T->anchors, (size_t)T->idmax * sizeof(at_Anchor));
    }
}

TEST_STATIC int atT_treesnapeq(const at_Tree *T, const att_TreeSnap *s) {
    if (T->S != s->state || T->bytes != s->bytes || T->levels != s->levels)
        return 0;
    if (T->epoch != s->epoch || T->anchors != s->anchors) return 0;
    if (T->idmax != s->idmax || T->idfree != s->idfree) return 0;
    if (T->seam != s->seam) return 0;
    if (memcmp(&T->root, &s->root, sizeof(at_Node)) != 0) return 0;
    if (atT_liveanchors(T) != s->live) return 0;
    if (T->idmax > 0
        && memcmp(T->anchors, s->slot, (size_t)T->idmax * sizeof(at_Anchor))
                   != 0)
        return 0;
    return 1;
}

/* setup-only: at_newanchor allocates/frees an id before reserve fails, so
 * the anchor free-list slot no longer compares byte-for-byte; still require
 * the tree metadata, root and live-anchor set to be untouched. */
TEST_STATIC int atT_treebaseeq(const at_Tree *T, const att_TreeSnap *s) {
    if (T->S != s->state || T->bytes != s->bytes || T->levels != s->levels)
        return 0;
    if (T->epoch != s->epoch || T->seam != s->seam) return 0;
    if (memcmp(&T->root, &s->root, sizeof(at_Node)) != 0) return 0;
    return atT_liveanchors(T) == s->live;
}

TEST(anchor_oom_idalloc) {
    OomCount     oc = {0, 1 << 20};
    at_State    *S = at_open(&oomcount_alloc, &oc);
    at_Tree     *T = at_newtree(S);
    at_Cursor    C, C0;
    att_TreeSnap ts;
    at_Id        id;
    assertok(S != NULL && T != NULL);
    oc.oom = 0;
    assertok(at_seek(&C, T, 0) == AT_OK);
    C0 = C, atT_treesnap(T, &ts);
    assertok(at_newanchor(&C, 5, 0, AT_GROW, &id) == AT_ERRMEM);
    assertok(memcmp(&C0, &C, sizeof C) == 0);
    assertok(atT_treesnapeq(T, &ts));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    asserteq(at_bytes(T), 0);
    /* structural checker (white-box): OOM before id-pool growth leaves the
     * pool unallocated; not observable through the public API. */
    assertok(T->anchors == NULL);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST_STATIC void atT_oomnewanchorsetup(at_Tree *T, at_Id *keep) {
    at_Id ids[32];
    int   i;
    for (i = 0; i < 32; ++i) atT_anchor(T, (size_t)i * 7u, 0, AT_GROW, &ids[i]);
    /* setup-only: require a multi-level tree so reserve OOM is meaningful. */
    assertok(T->levels >= 1);
    *keep = ids[31];
    assertok(at_delanchor(T, ids[0]) == AT_OK);
}

TEST_STATIC void atT_oomnewanchor(int mode) {
    OomCount     oc = {0, 1 << 20};
    at_State    *S = at_open(&oomcount_alloc, &oc);
    at_Tree     *T = at_newtree(S);
    at_Cursor    C, C0;
    att_TreeSnap ts;
    at_Id        keep, id;
    size_t       fail, bytes;
    assertok(S != NULL && T != NULL);
    atT_oomnewanchorsetup(T, &keep);
    bytes = at_bytes(T);
    /* setup-only: force page allocations for deterministic fail-at-N. */
    (void)atT_drainpool(&S->nodes), (void)atT_drainpool(&S->edges);
    fail = mode == 2
                 ? atT_poolpages(sizeof(at_Node), 2u * (size_t)T->levels + 5u)
                 : (size_t)mode;
    oc.oom = (int)fail;
    assertok(at_seek(&C, T, 50) == AT_OK);
    C0 = C, atT_treesnap(T, &ts);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_ERRMEM);
    assertok(memcmp(&C0, &C, sizeof C) == 0);
    assertok(atT_treebaseeq(T, &ts));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    asserteq(at_bytes(T), bytes);
    atT_rangeeq(T, keep, 217, 217);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST(anchor_oom_reserve) {
    atT_oomnewanchor(0);
    atT_oomnewanchor(1);
    atT_oomnewanchor(2);
}

TEST_STATIC void atT_oomsplice(int mode) {
    OomCount     oc = {0, 1 << 20};
    at_State    *S = at_open(&oomcount_alloc, &oc);
    at_Tree     *T = at_newtree(S);
    at_Cursor    C, C0;
    att_TreeSnap ts;
    at_Id        a, b;
    size_t       fail, bytes;
    assertok(S != NULL && T != NULL);
    atT_anchor(T, 10, 5, AT_GROW, &a);
    atT_anchor(T, 30, 5, AT_LOCK, &b);
    bytes = at_bytes(T);
    /* setup-only: force page allocations for deterministic fail-at-N. */
    (void)atT_drainpool(&S->nodes), (void)atT_drainpool(&S->edges);
    fail = mode == 2
                 ? atT_poolpages(sizeof(at_Node), 2u * (size_t)T->levels + 2u)
                 : (size_t)mode;
    oc.oom = (int)fail;
    assertok(at_seek(&C, T, 20) == AT_OK);
    C0 = C, atT_treesnap(T, &ts);
    assertok(at_splice(&C, 5, 1) == AT_ERRMEM);
    assertok(memcmp(&C0, &C, sizeof C) == 0);
    assertok(atT_treesnapeq(T, &ts));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    asserteq(at_bytes(T), bytes);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_checkcursor(&C, 20));
    atT_rangeeq(T, a, 10, 15);
    atT_rangeeq(T, b, 30, 35);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST(splice_oom_reserve) {
    atT_oomsplice(0);
    atT_oomsplice(1);
    atT_oomsplice(2);
}

TEST(del_oom_zero_alloc) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    size_t    x, y;
    assertok(S != NULL && T != NULL);
    atT_anchor(T, 30, 10, AT_GROW, &a);
    atT_anchor(T, 100, 5, AT_GROW, &b);
    oc.oom = 0;
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    asserteq(at_bytes(T), 105);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    asserteq(x, 100);
    asserteq(y, 105);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST(splice_noop_beyond_tail_oom) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C, D;
    at_Id     a, b;
    size_t    x, y, len;
    assertok(S != NULL && T != NULL);
    a = atT_anew(T, 100, 0, AT_GROW);
    b = atT_anew(T, 300, 0, AT_GROW);
    assertok(at_seek(&C, T, 500) == AT_OK);
    assertok(at_seek(&D, T, 0) == AT_OK);
    oc.oom = 0;
    assertok(at_splice(&C, 0, 0) == AT_OK);
    assertok(at_advance(&D, 0) == AT_OK);
    assertok(at_next(&D, 0, &len) == a);
    asserteq(len, 0);
    assertok(at_checkcursor(&D, 100));
    assertok(at_range(T, b, &x, &y) == AT_OK);
    asserteq(x, 300);
    asserteq(y, 300);
    asserteq(at_bytes(T), 300);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST(anchor_tail_delete) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c;
    size_t    x, y;
    atT_anchor(T, 100, 0, AT_GROW, &a);
    atT_anchor(T, 100, 0, AT_GROW, &b);
    atT_anchor(T, 100, 0, AT_GROW, &c);
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_delanchor(T, c) == AT_OK);
    assertok(at_bytes(T) == 100);
    at_deltree(T), at_close(S);
}

TEST(tail_edge_contract) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, got[2], want[2];
    size_t    x, y, len;
    atT_anchor(T, 100, 0, AT_SHIFT, &a);
    atT_anchor(T, 100, 0, AT_SHIFT, &b);
    want[0] = a, want[1] = b;
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(atT_cns(T, 100, 0, got, 2) == 2);
    assertok(atT_idset(got, 2, want, 2));
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_next(&C, 0, &len) != AT_NONE);
    asserteq(len, 0);
    assertok(atT_checkchains(T));
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_range(T, a, &x, &y) == AT_ERRPARAM);
    assertok(atT_cns(T, 0, 0, got, 2) == 1 && got[0] == b);
    assertok(atT_checkchains(T));
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(atT_cns(T, 0, 0, got, 2) == 0);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    asserteq(at_bytes(T), 100);
    at_deltree(T), at_close(S);
}

TEST(splice_delete_grow_tail) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     ids[10];
    int       i;
    for (i = 0; i < 8; ++i) atT_anchor(T, 10, 0, AT_GROW, &ids[i]);
    for (i = 8; i < 10; ++i) atT_anchor(T, 20, 0, AT_GROW, &ids[i]);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 10, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_bytes(T) == 10);
    at_deltree(T), at_close(S);
}

TEST(delete_contracts) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c;
    atT_anchor(T, 20, 5, AT_GROW, &a);
    atT_anchor(T, 50, 5, AT_FIXED, &b);
    atT_anchor(T, 80, 5, AT_SHIFT, &c);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 70, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    atT_rangeeq(T, c, 10, 15);
    assertok(at_splice(&C, 0, 7) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    asserteq(at_bytes(T), 0);
    at_deltree(T), at_close(S);
}

/* checker self-test: an overfull root must be rejected. */
TEST(checktree_overfull) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Index  old = T->root.child_count;
    T->root.child_count = (at_Index)(AT_FANOUT + 1);
    assertok(at_checktree(T) == 0);
    T->root.child_count = old;
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

/* checker self-test helpers: mutate one internal link/metric at a time. */
TEST_STATIC void atT_badleaf(at_Tree *T, at_Node *in, at_Node *lf, at_Edge *e) {
    at_Node *oldparent;
    at_Index oldidx;
    size_t   oldbytes;
    oldbytes = lf->bytes[0];
    lf->bytes[0] = 0;
    assertok(at_checktree(T) == 0);
    lf->bytes[0] = oldbytes;
    oldparent = e->parent;
    e->parent = NULL;
    assertok(at_checktree(T) == 0);
    e->parent = oldparent;
    oldidx = e->idx;
    e->idx = (at_Index)(e->idx == 0 ? 1 : 0);
    assertok(at_checktree(T) == 0);
    e->idx = oldidx;
    oldbytes = in->bytes[0];
    in->bytes[0] = oldbytes + 1;
    assertok(at_checktree(T) == 0);
    in->bytes[0] = oldbytes;
}

TEST_STATIC void atT_badnode(at_Tree *T, at_Node *lf) {
    at_Node *oldparent;
    at_Index oldidx;
    oldparent = lf->parent;
    lf->parent = NULL;
    assertok(at_checktree(T) == 0);
    lf->parent = oldparent;
    oldidx = lf->idx;
    lf->idx = (at_Index)(lf->idx == 0 ? 1 : 0);
    assertok(at_checktree(T) == 0);
    lf->idx = oldidx;
}

/* checker self-test: deliberate destructive mutations are restored. */
TEST(checktree_bad_links) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Node  *internal, *leaf;
    at_Edge  *e, *edge;
    int       i, oldlevels;
    assertok(at_checktree(NULL) == 0);
    for (i = 1; i <= 128 && T->levels < 2; ++i)
        assertok(atT_setupedge(T, (size_t)(i * 10), &e) == AT_OK);
    assertok(T->levels >= 2);
    internal = (at_Node *)T->root.children[0];
    leaf = (at_Node *)internal->children[0];
    assertok(atN_cc(internal) > 0 && atN_cc(leaf) > 0);
    edge = atN_edge(leaf, 0);
    atT_badleaf(T, internal, leaf, edge);
    atT_badnode(T, leaf);
    oldlevels = T->levels;
    T->levels = AT_MAX_LEVEL;
    assertok(at_checktree(T) == 0);
    T->levels = AT_MAX_LEVEL + 1;
    assertok(at_checktree(T) == 0);
    T->levels = oldlevels;
    T->bytes += 1;
    assertok(at_checktree(T) == 0);
    T->bytes -= 1;
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

/* checker self-test: 600 anchors exceed the draft 512-slot sequence cap. */
TEST(checker_seq_capacity) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     id;
    int       i;
    for (i = 0; i < 600; ++i)
        atT_anchor(T, (size_t)i * 3u + 1u, 0, AT_GROW, &id);
    assertok(at_bytes(T) > 0);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    at_deltree(T), at_close(S);
}

TEST(insert_edge_last) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a;
    size_t    x, y;
    atT_anchor(T, 100, 0, AT_GROW, &a);
    assertok(atT_setupinsert(T, 100, 7) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    asserteq(at_bytes(T), 107);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 107);
    at_deltree(T), at_close(S);
}

TEST(insert_edge_mid) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    size_t    x, y;
    atT_anchor(T, 100, 0, AT_GROW, &a);
    atT_anchor(T, 200, 0, AT_GROW, &b);
    assertok(atT_setupinsert(T, 100, 7) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    asserteq(at_bytes(T), 207);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 107);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 207 && y == 207);
    at_deltree(T), at_close(S);
}

TEST(insert_gap_shift_mid) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    size_t    x, y;
    atT_anchor(T, 100, 0, AT_GROW, &a);
    atT_anchor(T, 200, 0, AT_GROW, &b);
    assertok(atT_setupinsert(T, 150, 7) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    asserteq(at_bytes(T), 207);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 207 && y == 207);
    at_deltree(T), at_close(S);
}

TEST(delete_prefix_leaf_rebalance) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    int       i;
    for (i = 0; i < 50; ++i) {
        assertok(at_seek(&C, T, (size_t)((i + 1) * 20)) == AT_OK);
        assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_OK);
    }
    assertok(at_checktree(T));
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 20, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    at_deltree(T), at_close(S);
}

/* checker self-test: atT_sideof reports ids absent from both buckets. */
TEST(edge_side_missing) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    at_Id     a;
    size_t    x, y;
    atT_anchor(T, 100, 0, AT_SHIFT, &a);
    assertok(atT_setupfindedge(T, 100, &e));
    assertok(atT_sideof(T, e, 1000, 0) == -1);
    assertok(atT_sideof(T, e, 1000, 1) == -1);
    assertok(at_checktree(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    at_deltree(T), at_close(S);
}

/* structural checker: root collapse is internal; the public range/bytes
 * contract is covered by at_checktree below. */
TEST(delete_root_collapse) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     id[40];
    size_t    prev;
    int       i, shrunk = 0;
    for (i = 0; i < 40; ++i)
        atT_anchor(T, (size_t)(10 + i * 7), 0, AT_GROW, &id[i]);
    assertok(at_checktree(T));
    assertok(T->levels >= 1);
    prev = (size_t)T->levels;
    for (i = 0; i < 40; ++i) {
        assertok(at_delanchor(T, id[i]) == AT_OK);
        assertok(at_checktree(T));
        if ((size_t)T->levels < prev && at_bytes(T) > 0) shrunk = 1;
        prev = (size_t)T->levels;
    }
    assertok(shrunk);
    assertok(T->levels == 0 && atN_cc(&T->root) == 1 && at_bytes(T) == 283);
    at_deltree(T), at_close(S);
}

TEST(splice_multi_slot_delete) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[10];
    size_t    x, y;
    int       i;
    for (i = 0; i < 10; ++i)
        atT_anchor(T, (size_t)(10 + i * 4), 1, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 12) == AT_OK);
    assertok(at_splice(&C, 16, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    for (i = 0; i < 10; ++i) {
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        assertok(y >= x);
    }
    at_deltree(T), at_close(S);
}

TEST(splice_cross_leaf_rebalance) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[40];
    int       i;
    for (i = 0; i < 40; ++i)
        atT_anchor(T, (size_t)(10 + i * 7), 2, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, 180, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_checkcursor(&C, 20));
    assertok(T->seam == NULL);
    assertok(atT_rtempty(T));
    at_deltree(T), at_close(S);
}

TEST(splice_empty_L_merge) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    atT_anchor(T, 50, 5, AT_GROW, &a);
    atT_anchor(T, 70, 5, AT_LOCK, &b);
    assertok(at_seek(&C, T, 55) == AT_OK);
    assertok(at_splice(&C, 10, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 50 && y == 55);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 60 && y == 65);
    at_deltree(T), at_close(S);
}

TEST(delete_cross_leaf_merge) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[64];
    int       i;
    for (i = 0; i < 40; ++i)
        atT_anchor(T, (size_t)(10 + i * 7), 0, AT_GROW, &id[i]);
    /* setup-only: require a multi-level tree for the crossing merge. */
    assertok(T->levels >= 1);
    assertok(at_seek(&C, T, 12) == AT_OK);
    assertok(at_splice(&C, 120, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(atT_checkchains(T));
    assertok(at_checkcursor(&C, 12));
    assertok(T->seam == NULL);
    assertok(atT_rtempty(T));
    at_deltree(T), at_close(S);
}

TEST(delete_tail_sentinel) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[20];
    int       i;
    for (i = 0; i < 20; ++i)
        atT_anchor(T, (size_t)(10 + i * 9), 3, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(atT_checkchains(T));
    assertok(at_checkcursor(&C, 10));
    assertok(T->seam == NULL);
    assertok(atT_rtempty(T));
    at_deltree(T), at_close(S);
}

TEST(splice_delete_shared_tail) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    att_Model m[AT_BRUTE_N];
    size_t    x, y;
    int       i;
    atT_brutebuild(T, m);
    assertok(at_seek(&C, T, 95) == AT_OK);
    assertok(at_splice(&C, 5, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_checkcursor(&C, 95));
    for (i = 9; i < AT_BRUTE_N; ++i) {
        assertok(at_range(T, m[i].id, &x, &y) == AT_OK);
        assertok(x == 95 && y == 95);
    }
    at_deltree(T), at_close(S);
}

TEST(splice_delete_chain) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Cursor           C;
    at_Id               id[10];
    size_t              x, y;
    static const size_t ep[6] = {0, 10, 15, 25, 35, 45};
    static const size_t ex[10] = {10, 15, 15, 15, 15, 15, 15, 25, 35, 45};
    int                 i;
    for (i = 0; i < 10; ++i)
        atT_anchor(T, (size_t)(10 + i * 10), 0, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 15) == AT_OK);
    assertok(at_splice(&C, 55, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_checkcursor(&C, 15));
    assertok(T->seam == NULL);
    assertok(atT_rtempty(T));
    asserteq(at_bytes(T), 45);
    assertok(atT_positionsmatch(T, ep, 6));
    for (i = 0; i < 10; ++i) {
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        asserteq(x, ex[i]);
        asserteq(y, ex[i]);
    }
    at_deltree(T), at_close(S);
}

TEST(splice_delete_tail) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[3];
    int       i;
    for (i = 0; i < 3; ++i) atT_anchor(T, (size_t)(i * 2), 2, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 2) == AT_OK);
    assertok(at_splice(&C, 4, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_checkcursor(&C, 2));
    assertok(T->seam == NULL);
    assertok(atT_rtempty(T));
    at_deltree(T), at_close(S);
}

TEST(splice_shift_negative) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    atT_anchor(T, 100, 0, AT_GROW, &a);
    atT_anchor(T, 200, 0, AT_GROW, &b);
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_splice(&C, 7, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 93 && y == 93);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 193 && y == 193);
    asserteq(at_bytes(T), 193);
    at_deltree(T), at_close(S);
}

TEST(iter_multiple_edges) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c, d, got[4];
    atT_anchor(T, 10, 10, AT_GROW, &a);
    atT_anchor(T, 10, 0, AT_FIXED, &b);
    atT_anchor(T, 20, 10, AT_SHIFT, &c);
    atT_anchor(T, 30, 0, AT_LOCK, &d);
    assertok(atT_collect(T, 0, got, 4) == 4);
    assertok(got[0] == a && got[1] == b && got[2] == c && got[3] == d);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

TEST(iter_start_first) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, got[2];
    atT_anchor(T, 0, 5, AT_GROW, &a);
    atT_anchor(T, 5, 0, AT_LOCK, &b);
    assertok(atT_collect(T, 0, got, 2) == 2);
    assertok(got[0] == a && got[1] == b);
    assertok(atT_collect(T, 1, got, 2) == 1);
    assertok(got[0] == b);
    at_deltree(T), at_close(S);
}

TEST(iter_start_empty) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, got[1];
    atT_anchor(T, 0, 0, AT_GROW, &a);
    assertok(at_bytes(T) == 0);
    assertok(atT_collect(T, 0, got, 1) == 1);
    assertok(got[0] == a);
    at_deltree(T), at_close(S);
}

TEST(iter_many_edges_cross_leaves) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     id[60], got[60];
    int       i, n;
    for (i = 0; i < 60; ++i)
        atT_anchor(T, (size_t)(1 + i * 3), 0, AT_GROW, &id[i]);
    n = atT_collect(T, 0, got, 60);
    assertok(n == 60);
    for (i = 0; i < 60; ++i) assertok(got[i] == id[i]);
    n = atT_collect(T, 61, got, 60);
    assertok(n == 40);
    for (i = 0; i < 40; ++i) assertok(got[i] == id[20 + i]);
    at_deltree(T), at_close(S);
}

TEST(iter_zero_length_once) {
    at_State        *S = at_open(NULL, NULL);
    at_Tree         *T = at_newtree(S);
    static const int gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    at_Id            id[4], got[4];
    size_t           x, y;
    int              i, j, n;
    for (i = 0; i < 4; ++i) atT_anchor(T, 100, 0, gv[i], &id[i]);
    n = atT_collect(T, 0, got, 4);
    assertok(n == 4);
    assertok(atT_idset(got, n, id, 4));
    for (i = 0; i < 4; ++i) {
        int c = 0;
        for (j = 0; j < n; ++j)
            if (got[j] == id[i]) c += 1;
        assertok(c == 1);
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        assertok(x == 100 && y == 100);
    }
    assertok(atT_collect(T, 50, got, 4) == 4);
    assertok(atT_idset(got, 4, id, 4));
    assertok(atT_collect(T, 100, got, 4) == 4);
    assertok(atT_idset(got, 4, id, 4));
    at_deltree(T), at_close(S);
}

TEST(iter_gap_and_edge_starts) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, got[2];
    atT_anchor(T, 30, 5, AT_GROW, &a);
    atT_anchor(T, 50, 5, AT_LOCK, &b);
    assertok(atT_collect(T, 40, got, 2) == 1);
    assertok(got[0] == b);
    assertok(atT_collect(T, 32, got, 2) == 1);
    assertok(got[0] == b);
    assertok(atT_collect(T, 30, got, 2) == 2);
    assertok(got[0] == a && got[1] == b);
    assertok(atT_collect(T, 50, got, 2) == 1);
    assertok(got[0] == b);
    assertok(atT_collect(T, 52, got, 2) == 0);
    assertok(atT_collect(T, 55, got, 2) == 0);
    assertok(atT_collect(T, 60, got, 2) == 0);
    at_deltree(T), at_close(S);
}

TEST(iter_empty_end_idempotent) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C, D;
    at_Id     a;
    size_t    len = 0;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    memset(&D, 0, sizeof(D));
    assertok(at_next(NULL, 0, NULL) == AT_NONE);
    assertok(at_next(&D, 0, NULL) == AT_NONE);
    assertok(atT_setupinsert(T, 0, 10) == AT_OK);
    assertok(at_seek(&C, T, 5) == AT_OK);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    at_reset(T);
    assertok(at_seek(&C, T, 0) == AT_OK);
    atT_anchor(T, 10, 0, AT_GROW, &a);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    at_deltree(T), at_close(S);
}

TEST_STATIC void atT_nsfilter(at_Tree *T, int ns, const at_Id *want, int m) {
    at_Id got[8];
    int   n = atT_cns(T, 0, ns, got, 8);
    assertok(atT_idset(got, n, want, m));
}

TEST(iter_ns_filter) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     want0[8], want1[3], want2[3], want3[2];
    atT_mkns(T, 0, 0, 1, &want0[0]);
    atT_mkns(T, 0, 0, 2, &want0[1]);
    atT_mkns(T, 0, 30, 1, &want0[2]);
    atT_mkns(T, 0, 0, 3, &want0[3]);
    atT_mkns(T, 10, 0, 2, &want0[4]);
    atT_mkns(T, 10, 0, 1, &want0[5]);
    atT_mkns(T, 10, 10, 2, &want0[6]);
    atT_mkns(T, 10, 0, 3, &want0[7]);
    want1[0] = want0[0], want1[1] = want0[2], want1[2] = want0[5];
    want2[0] = want0[1], want2[1] = want0[4], want2[2] = want0[6];
    want3[0] = want0[3], want3[1] = want0[7];
    atT_nsfilter(T, 0, want0, 8);
    atT_nsfilter(T, 1, want1, 3);
    atT_nsfilter(T, 2, want2, 3);
    atT_nsfilter(T, 3, want3, 2);
    atT_nsfilter(T, 99, want0, 0);
    at_deltree(T), at_close(S);
}

TEST(iter_plen) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c;
    size_t    len;
    atT_anchor(T, 0, 5, AT_GROW, &a);
    atT_anchor(T, 10, 0, AT_GROW, &b);
    atT_anchor(T, 50, 0, AT_FIXED, &c);
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_splice(&C, 0, 7) == AT_OK);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, &len) == a);
    asserteq(len, 5);
    assertok(at_next(&C, 0, &len) == b);
    asserteq(len, 0);
    assertok(at_next(&C, 0, &len) == c);
    asserteq(len, 0);
    len = 99;
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
    at_deltree(T), at_close(S);
}

TEST(iter_params) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C, D;
    at_Id     a;
    size_t    len = 99;
    memset(&D, 0, sizeof(D));
    atT_anchor(T, 0, 3, AT_GROW, &a);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(NULL, 0, &len) == AT_NONE);
    asserteq(len, 0);
    len = 99;
    assertok(at_next(&D, 0, &len) == AT_NONE);
    asserteq(len, 0);
    len = 99;
    assertok(at_next(&C, -1, &len) == AT_NONE);
    asserteq(len, 0);
    len = 99;
    assertok(at_next(&C, (int)AT_MAX_NS + 1, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_next(&C, 0, NULL) == a);
    len = 99;
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    at_deltree(T), at_close(S);
}

TEST(iter_exhaust_restart) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    len = 99;
    atT_mkns(T, 0, 0, 1, &a);
    atT_mkns(T, 10, 0, 1, &b);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 1, NULL) == a);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_next(&C, 1, NULL) == b);
    assertok(at_next(&C, 1, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_next(&C, 1, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_next(&C, 1, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(&C, -10) == AT_OK);
    assertok(at_next(&C, 1, &len) == a);
    asserteq(len, 0);
    assertok(at_advance(&C, 10) == AT_OK);
    assertok(at_next(&C, 1, NULL) == b);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 1, NULL) == a);
    at_deltree(T), at_close(S);
}

TEST_STATIC void atT_exhaustcommitspan(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, 10) == AT_OK);
    asserteq(at_bytes(T), 10);
    assertok(at_seek(&C, T, 5) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    assertok(at_offset(&C) == 10);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    assertok(at_offset(&C) == 10);
    assertok(at_seek(&C, T, 15) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    assertok(at_offset(&C) == 15);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 0, 1) == AT_OK);
    asserteq(at_bytes(T), 11);
    assertok(at_checkcursor(&C, 11));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void atT_exhaustcommitboundary(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    bytes, len = 99;
    a = atT_anew(T, 0, 10, AT_GROW);
    b = atT_anew(T, 20, 5, AT_GROW);
    bytes = at_bytes(T);
    asserteq(bytes, 25);
    assertok(at_seek(&C, T, bytes) == AT_OK);
    assertok(at_offset(&C) == bytes);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_offset(&C) == bytes);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_offset(&C) == bytes);
    assertok(at_advance(&C, -5) == AT_OK);
    assertok(at_offset(&C) == 20);
    assertok(at_next(&C, 0, &len) == b);
    asserteq(len, 5);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_offset(&C) == bytes);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_offset(&C) == bytes);
    assertok(at_advance(&C, -25) == AT_OK);
    assertok(at_offset(&C) == 0);
    assertok(at_next(&C, 0, &len) == a);
    asserteq(len, 10);
    assertok(at_next(&C, 0, &len) == b);
    asserteq(len, 5);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST(iter_exhaust_commit) {
    atT_exhaustcommitspan();
    atT_exhaustcommitboundary();
}

TEST(splice_after_iter_exhaust) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a;
    size_t    len = 99;
    atT_anchor(T, 10, 0, AT_LOCK, &a);
    assertok(at_seek(&C, T, 0) == AT_OK);
    asserteq(at_next(&C, 0, &len), a);
    asserteq(len, 0);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_splice(&C, 0, 5) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(at_checkcursor(&C, 15));
    assertok(at_seek(&C, T, 10) == AT_OK);
    asserteq(at_next(&C, 0, &len), a);
    asserteq(len, 0);
    at_deltree(T), at_close(S);
}

TEST(iter_empty_start) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    len = 99;
    atT_anchor(T, 0, 0, AT_GROW, &a);
    atT_anchor(T, 0, 0, AT_GROW, &b);
    asserteq(a, 1);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == b);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(&C, -1) == AT_OK);
    assertok(at_next(&C, 0, NULL) == b);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_advance(&C, 1) == AT_OK);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(&C, -1) == AT_OK);
    assertok(at_next(&C, 0, NULL) == b);
    at_deltree(T), at_close(S);
}

TEST(iter_after_splice_insert) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c, got[3];
    size_t    x, y;
    atT_anchor(T, 0, 5, AT_GROW, &c);
    atT_anchor(T, 10, 10, AT_GROW, &a);
    atT_anchor(T, 20, 10, AT_LOCK, &b);
    assertok(at_seek(&C, T, 15) == AT_OK);
    assertok(at_splice(&C, 0, 5) == AT_OK);
    assertok(atT_collect(T, 0, got, 3) == 3);
    assertok(got[0] == c && got[1] == a && got[2] == b);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 10 && y == 25);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 25 && y == 35);
    at_deltree(T), at_close(S);
}

TEST(iter_after_splice_delete) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c, got[3];
    size_t    x, y;
    atT_anchor(T, 10, 5, AT_GROW, &a);
    atT_anchor(T, 30, 5, AT_LOCK, &b);
    atT_anchor(T, 50, 5, AT_FIXED, &c);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, 25, 0) == AT_OK);
    assertok(atT_collect(T, 0, got, 3) == 3);
    assertok(got[0] == a && got[1] == b && got[2] == c);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 20 && y == 20);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 25 && y == 30);
    at_deltree(T), at_close(S);
}

TEST(splice_overflow_del) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    atT_anchor(T, 10, 5, AT_GROW, &a);
    atT_anchor(T, 40, 5, AT_GROW, &b);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_checkcursor(&C, 20));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 10 && y == 15);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 20 && y == 20);
    asserteq(at_bytes(T), 20);
    at_deltree(T), at_close(S);
}

TEST(splice_delete_large_span) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[100];
    size_t    x, y;
    int       i;
    for (i = 0; i < 100; ++i)
        atT_anchor(T, (size_t)(10 + i * 7), (size_t)(i % 5), 2, &id[i]);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(at_checkcursor(&C, 10));
    assertok(T->seam == NULL);
    assertok(atT_rtempty(T));
    for (i = 0; i < 100; ++i) assertok(at_range(T, id[i], &x, &y) == AT_OK);
    at_deltree(T), at_close(S);
}

TEST(splice_delete_all) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    size_t    len;
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    assertok(atT_setupinsert(T, 100, 50) == AT_OK);
    asserteq(at_bytes(T), 150);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, at_bytes(T), 0) == AT_OK);
    asserteq(at_bytes(T), 0);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(at_checkcursor(&C, 0));
    asserteq(at_next(&C, 0, &len), AT_NONE);
    asserteq(len, 0);
    assertok(at_splice(&C, 0, 5) == AT_OK);
    asserteq(at_bytes(T), 5);
    assertok(at_checktree(T));
    assertok(at_checkcursor(&C, 5));
    at_deltree(T), at_close(S);
}

TEST(anchor_remove_missing) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c;
    size_t    x, y;
    atT_anchor(T, 100, 10, AT_GROW, &a);
    atT_anchor(T, 100, 10, AT_GROW, &b);
    atT_anchor(T, 300, 10, AT_GROW, &c);
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(at_delanchor(T, b) == AT_ERRPARAM);
    assertok(at_delanchor(T, 9999) == AT_ERRPARAM);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 110);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 300 && y == 310);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST(anchor_at_zero_delete) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a;
    size_t    x, y, len;
    atT_anchor(T, 0, 0, AT_GROW, &a);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 0 && y == 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, &len) == a);
    asserteq(len, 0);
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_range(T, a, &x, &y) == AT_ERRPARAM);
    assertok(at_checktree(T));
    asserteq(at_bytes(T), 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    at_deltree(T), at_close(S);
}

TEST(insert_empty_boundary) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               a;
    size_t              x, y;
    static const size_t p[1] = {0};
    assertok(atT_setupinsert(T, 0, 10) == AT_OK);
    assertok(atT_setupinsert(T, 10, 7) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_noemptyedges(T));
    assertok(atT_positionsmatch(T, p, 1));
    asserteq(at_bytes(T), 17);
    atT_anchor(T, 17, 0, AT_GROW, &a);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 17 && y == 17);
    asserteq(at_bytes(T), 17);
    at_deltree(T), at_close(S);
}

TEST(deleteedge_pos_guard) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    assertok(atT_setupedge(T, 10, &e) == AT_OK);
    assertok(atT_setupdropedge(T, 20) == AT_ERRPARAM);
    assertok(atT_setupdropedge(T, 5) == AT_ERRPARAM);
    at_deltree(T), at_close(S);
}

TEST(splice_ins_overflow) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    assertok(atT_setupedge(T, 10, &e) == AT_OK);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, (size_t)-1) == AT_ERROVERFLOW);
    assertok(at_bytes(T) == 10 && at_checktree(T));
    assertok(at_advance(&C, (at_Delta)(((size_t)-1) >> 1)) == AT_OK);
    assertok(at_advance(&C, (at_Delta)(((size_t)-1) >> 1)) == AT_OK);
    assertok(at_advance(&C, 1) == AT_OK);
    assertok(at_splice(&C, 0, 1) == AT_ERROVERFLOW);
    assertok(at_bytes(T) == 10 && at_checktree(T));
    at_deltree(T), at_close(S);
}

TEST(advance_overflow_guard) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    at_Id     id;
    assertok(atT_setupinsert(T, 0, 10) == AT_OK);
    assertok(atT_setupedge(T, 10, &e) == AT_OK);
    assertok(atT_setupinsert(T, 10, 10) == AT_OK);
    asserteq(at_bytes(T), 20);
    assertok(at_seek(&C, T, (size_t)-1) == AT_OK);
    assertok(at_advance(&C, 1) == AT_ERROVERFLOW);
    assertok(at_offset(&C) == (size_t)-1);
    assertok(at_splice(&C, 0, 1) == AT_ERROVERFLOW);
    assertok(at_newanchor(&C, 1, 0, AT_GROW, &id) == AT_ERROVERFLOW);
    asserteq(at_bytes(T), 20);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(at_locate(&C, 0) == AT_OK);
    assertok(at_advance(&C, 1) == AT_OK);
    assertok(at_checkcursor(&C, 1));
    at_deltree(T), at_close(S);
}

typedef struct {
    size_t pos[AT_SEQ_CAP];
    size_t span[AT_SEQ_CAP];
    int    n;
} att_EpochSnap;

TEST_STATIC void atT_snapshot(at_Tree *T, att_EpochSnap *s) {
    at_Seq q;
    int    i;
    atT_seq(T, &q);
    assert(q.n <= AT_SEQ_CAP);
    s->n = q.n;
    for (i = 0; i < q.n; ++i) s->pos[i] = q.pos[i], s->span[i] = q.span[i];
}

TEST_STATIC int atT_snapeq(const att_EpochSnap *a, const att_EpochSnap *b) {
    int i;
    if (a->n != b->n) return 0;
    for (i = 0; i < a->n; ++i)
        if (a->pos[i] != b->pos[i] || a->span[i] != b->span[i]) return 0;
    return 1;
}

TEST(epoch_advance_stale) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    assertok(S != NULL && T != NULL);
    a = atT_anew(T, 10, 5, AT_GROW);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &b) == AT_OK);
    assertok(at_advance(&C, 0) == AT_ESTALE);
    assertok(at_advance(&C, 1) == AT_ESTALE);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &b) == AT_ESTALE);
    assertok(at_locate(&C, 10) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_checkcursor(&C, 10));
    atT_rangeeq(T, a, 10, 15), atT_rangeeq(T, b, 10, 10);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void atT_epochnextscan(at_Tree *T, const at_Id *ids, at_Id ex) {
    at_Cursor C;
    at_Id     id;
    size_t    len;
    int       i, n = 0, seen[65];
    assertok(at_seek(&C, T, 0) == AT_OK);
    memset(seen, 0, sizeof(seen));
    while ((id = at_next(&C, 0, &len)) != AT_NONE) {
        assertok(len == 0);
        assertok(n < 65);
        assertok(id == ex || (id >= ids[40] && id <= ids[63]));
        assertok(!seen[(int)id - 1]);
        seen[(int)id - 1] = 1, n += 1;
    }
    asserteq(n, 25);
    for (i = 40; i < 64; ++i) assertok(seen[(int)ids[i] - 1]);
    assertok(seen[(int)ex - 1]);
}

TEST(epoch_next_stale) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     ids[64], extra;
    size_t    len;
    int       i;
    assertok(S != NULL && T != NULL);
    for (i = 0; i < 64; ++i) ids[i] = atT_anew(T, (size_t)i * 3u, 0, AT_GROW);
    assertok(at_seek(&C, T, 0) == AT_OK);
    extra = atT_anew(T, 300, 0, AT_GROW);
    for (i = 0; i < 40; ++i) assertok(at_delanchor(T, ids[i]) == AT_OK);
    len = 99;
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    len = 99;
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(&C, 0) == AT_ESTALE);
    atT_epochnextscan(T, ids, extra);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void atT_epochsplicecheck(at_Tree *T, at_Id a, at_Id b) {
    at_Cursor C;
    size_t    len;
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 0, 5) == AT_OK);
    assertok(at_checkcursor(&C, 15));
    assertok(at_locate(&C, 10) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_checkcursor(&C, 10));
    assertok(at_next(&C, 0, &len) == a);
    asserteq(len, 5);
    assertok(at_advance(&C, 10) == AT_OK);
    assertok(at_checkcursor(&C, 20));
    atT_rangeeq(T, a, 10, 15), atT_rangeeq(T, b, 25, 25);
}

TEST(epoch_splice_keeps_own) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    len;
    assertok(S != NULL && T != NULL);
    a = atT_anew(T, 10, 0, AT_GROW);
    b = atT_anew(T, 20, 0, AT_GROW);
    atT_epochsplicecheck(T, a, b);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 5, 0) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_checkcursor(&C, 10));
    assertok(at_next(&C, 0, &len) == a);
    asserteq(len, 0);
    atT_rangeeq(T, b, 20, 20);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void atT_epochchecks(at_Tree *T, at_Id a, at_Id b, at_Cursor *c) {
    att_EpochSnap before, after;
    size_t        len;
    len = 99;
    assertok(at_next(c, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(c, 0) == AT_ESTALE);
    atT_snapshot(T, &before);
    assertok(at_splice(c, 1, 1) == AT_ESTALE);
    atT_snapshot(T, &after);
    assertok(atT_snapeq(&before, &after));
    atT_rangeeq(T, a, 10, 15), atT_rangeeq(T, b, 25, 25);
    assertok(at_locate(c, 20) == AT_OK);
    assertok(at_advance(c, 0) == AT_OK);
    assertok(at_checkcursor(c, 20));
}

TEST(epoch_multi_cursor) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C1, C2;
    at_Id     a, b;
    assertok(S != NULL && T != NULL);
    a = atT_anew(T, 10, 0, AT_GROW);
    b = atT_anew(T, 20, 0, AT_GROW);
    assertok(at_seek(&C1, T, 10) == AT_OK);
    assertok(at_seek(&C2, T, 20) == AT_OK);
    assertok(at_splice(&C1, 0, 5) == AT_OK);
    assertok(at_checkcursor(&C1, 15));
    assertok(at_locate(&C1, 10) == AT_OK);
    assertok(at_advance(&C1, 0) == AT_OK);
    assertok(at_next(&C1, 0, NULL) == a);
    atT_epochchecks(T, a, b, &C2);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void atT_epochnomutqueries(at_Tree *T, at_Id a) {
    at_Cursor C;
    size_t    len;
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_advance(&C, 1) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_locate(&C, 10) == AT_OK);
    assertok(at_next(&C, 0, &len) == a);
    asserteq(len, 5);
    assertok(at_gravity(T, a) == AT_GROW);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_checkcursor(&C, 10));
    assertok(at_locate(&C, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
}

TEST_STATIC void atT_epochnomutoom(void) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     spare;
    assertok(S != NULL && T != NULL);
    assertok(at_seek(&C, T, 10) == AT_OK);
    oc.oom = 0;
    assertok(at_splice(&C, 0, 1) == AT_ERRMEM);
    assertok(at_advance(&C, 0) == AT_OK);
    oc.oom = 1;
    assertok(at_splice(&C, 0, 1) == AT_ERRMEM);
    assertok(at_advance(&C, 0) == AT_OK);
    oc.oom = 1;
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &spare) == AT_ERRMEM);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_checkcursor(&C, 10));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST(epoch_no_mutation) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Id     a;
    assertok(S != NULL && T != NULL);
    a = atT_anew(T, 10, 5, AT_GROW);
    atT_epochnomutqueries(T, a);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
    atT_epochnomutoom();
}

TEST(epoch_reset) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    size_t    len;
    assertok(S != NULL && T != NULL);
    atT_anew(T, 10, 5, AT_GROW);
    atT_anew(T, 30, 5, AT_GROW);
    assertok(at_seek(&C, T, 10) == AT_OK);
    at_reset(T);
    len = 99;
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(&C, 0) == AT_ESTALE);
    assertok(at_splice(&C, 0, 1) == AT_ESTALE);
    assertok(at_locate(&C, 0) == AT_OK);
    assertok(at_advance(&C, 5) == AT_OK);
    assertok(at_checkcursor(&C, 5));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

/* setup-only: force a child split on the next insert; reads internals
 * only to choose the already-full child edge. */
TEST_STATIC at_Edge *atT_splitchildedge(at_Tree *T, at_Id *pid) {
    at_Node *last = NULL;
    at_Edge *e = NULL;
    size_t   pos = 10;
    int      guard, i;
    for (guard = 0; guard < 2048; ++guard) {
        atT_anchor(T, pos, 0, AT_GROW, pid);
        last = atT_lastleaf(T);
        if (T->levels >= 2 && atN_cc(last) == AT_FANOUT
            && atN_cc(last->parent) == AT_FANOUT
            && atN_cc(&T->root) < AT_FANOUT)
            break;
        pos += 10;
    }
    assertok(guard < 2048);
    for (i = 0; i < AT_FANOUT; ++i) {
        e = atN_edge(last, i);
        if (e->right.id != 0) break;
    }
    assertok(i < AT_FANOUT);
    return e;
}

/* fanout4-only: the forced child split relies on the fanout-4 layout. */
TEST(insert_split_child) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    at_Id     id = 0;
    size_t    pos;
    e = atT_splitchildedge(T, &id);
    pos = atT_setupedgepos(e);
    assertok(at_seek(&C, T, pos) == AT_OK);
    assertok(at_splice(&C, 0, 7) == AT_OK);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    at_deltree(T), at_close(S);
}

TEST(insert_span_split) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    size_t    x, y;
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    atT_anchor(T, 50, 0, AT_FIXED, &a);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 50 && y == 50);
    atT_anchor(T, 75, 10, AT_GROW, &b);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 75 && y == 85);
    assertok(at_bytes(T) == 100);
    at_deltree(T), at_close(S);
}

TEST_STATIC void atT_modelbuildspread(
        at_Tree *T, att_Model *m, int n, size_t step) {
    at_Cursor C;
    int       i;
    for (i = 0; i < n; ++i) {
        m[i].gravity = i % 4;
        assertok(at_seek(&C, T, 10u + (size_t)i * step) == AT_OK);
        assertok(
                at_newanchor(&C, (size_t)(i % 3), 0, m[i].gravity, &m[i].id)
                == AT_OK);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
    }
}

TEST_STATIC int atT_modelnext(
        at_Tree *T, const att_Model *m, int n, at_Id *got, int cap) {
    at_Id want[512];
    int   i, k;
    assertok(n <= 512);
    for (i = 0; i < n; ++i) want[i] = m[i].id;
    k = atT_nextall(T, got, cap);
    return k == n && atT_idset(got, n, want, n);
}

TEST_STATIC void atT_modelchecks(
        at_Tree *T, const att_Model *m, int n, at_Id *got, int cap) {
    assertok(atT_modelok(T, m, n));
    asserteq(atT_liveanchors(T), n);
    assertok(atT_modelnext(T, m, n, got, cap));
}

TEST_STATIC void atT_stitchinv(const at_Tree *T) {
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(T->seam == NULL);
    assertok(atT_rtempty(T));
}

#define AT_CONT_N 32

TEST_STATIC void atT_contbuild(at_Tree *T, att_Model *m, int n, int grav) {
    at_Cursor C;
    int       i;
    for (i = 0; i < n; ++i) {
        m[i].gravity = grav < 0 ? i % 4 : grav;
        assertok(at_seek(&C, T, 10u + (size_t)i * 7u) == AT_OK);
        assertok(
                at_newanchor(&C, (size_t)(i % 3), 0, m[i].gravity, &m[i].id)
                == AT_OK);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
    }
}

TEST_STATIC int atT_curside(at_Tree *T, const at_Cursor *C) {
    at_Anchor *a;
    if (C->cur.id == 0) return C->cur.is_end ? 2 : -1;
    a = atA_slot(T, C->cur.id);
    return C->cur.is_end ? (int)a->end_right : (int)a->start_right;
}

TEST_STATIC int atT_contmiss(
        at_Tree *T, at_Cursor *C, const att_Model *m, int n, const at_Id *seen,
        int nseen) {
    at_Id  cont[AT_CONT_N * 2], id;
    size_t pos = at_offset(C), x, y;
    int    nc = 0, i, j;
    while ((id = at_next(C, 0, NULL)) != AT_NONE) {
        assertok(nc < AT_CONT_N * 2);
        assertok(at_range(T, id, &x, &y) == AT_OK);
        cont[nc] = id, nc += 1;
    }
    for (i = 0; i < n; ++i) {
        if (m[i].start < pos) continue;
        for (j = 0; j < nseen && seen[j] != m[i].id; ++j) continue;
        if (j < nseen) continue;
        for (j = 0; j < nc && cont[j] != m[i].id; ++j) continue;
        if (j == nc) {
            test_log(
                    "[cont] miss id=%u start=%lu pos=%lu seen=%d got=%d\n",
                    m[i].id, test_lu(m[i].start), test_lu(pos), nseen, nc);
            return 0;
        }
    }
    return 1;
}

TEST_STATIC int atT_conteditok(
        int op, int grav, int pre, int side, int force, int inside, size_t del,
        size_t ins) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[AT_CONT_N];
    att_Splice w;
    at_Id      seen[AT_CONT_N * 2], id;
    size_t     L = 10, pos;
    int        n = AT_CONT_N, nseen = 0, i;
    assertok(S != NULL && T != NULL);
    atT_contbuild(T, m, n, grav);
    if (inside)
        L = 12;
    else if (force)
        L = 20;
    assertok(at_seek(&C, T, L) == AT_OK);
    if (force) {
        C.cur = atU_point(0, 1);
    } else if (side == 2) {
        assertok(at_next(&C, 0, NULL) == AT_NONE);
    } else {
        for (i = 0; i < pre; ++i) {
            id = at_next(&C, 0, NULL);
            assertok(id != AT_NONE);
            seen[nseen] = id, nseen += 1;
        }
        while (side >= 0 && atT_curside(T, &C) != side) {
            id = at_next(&C, 0, NULL);
            if (id == AT_NONE) break;
            seen[nseen] = id, nseen += 1;
        }
        if (side >= 0) assertok(atT_curside(T, &C) == side);
    }
    pos = at_offset(&C);
    w.L = pos, w.del = del, w.ins = ins;
    atT_modelsplice(m, n, &w);
    if (op == 0)
        assertok(at_remove(&C, del) == AT_OK);
    else if (op == 1)
        assertok(at_insert(&C, ins) == AT_OK);
    else if (op == 2)
        assertok(at_append(&C, ins) == AT_OK);
    else
        assertok(at_splice(&C, del, ins) == AT_OK);
    assertok(atT_contmiss(T, &C, m, n, seen, nseen));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(T->seam == NULL);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
    return 1;
}

TEST_STATIC int atT_contseqok(
        int grav, int pre, int side, int force, int op2, size_t del1,
        size_t del2, size_t ins2) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[AT_CONT_N];
    att_Splice w1, w2;
    at_Id      seen[AT_CONT_N * 2], id;
    size_t     L = force ? 20 : 10, pos;
    int        n = AT_CONT_N, nseen = 0, i;
    assertok(S != NULL && T != NULL);
    atT_contbuild(T, m, n, grav);
    assertok(at_seek(&C, T, L) == AT_OK);
    if (force) {
        C.cur = atU_point(0, 1);
    } else {
        for (i = 0; i < pre; ++i) {
            id = at_next(&C, 0, NULL);
            assertok(id != AT_NONE);
            seen[nseen] = id, nseen += 1;
        }
        while (side >= 0 && atT_curside(T, &C) != side) {
            id = at_next(&C, 0, NULL);
            if (id == AT_NONE) break;
            seen[nseen] = id, nseen += 1;
        }
        if (side >= 0) assertok(atT_curside(T, &C) == side);
    }
    pos = at_offset(&C);
    w1.L = pos, w1.del = del1, w1.ins = 0;
    atT_modelsplice(m, n, &w1);
    assertok(at_remove(&C, del1) == AT_OK);
    pos = at_offset(&C);
    w2.L = pos, w2.del = del2, w2.ins = ins2;
    atT_modelsplice(m, n, &w2);
    if (op2 == 0)
        assertok(at_remove(&C, del2) == AT_OK);
    else
        assertok(at_splice(&C, del2, ins2) == AT_OK);
    assertok(atT_contmiss(T, &C, m, n, seen, nseen));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(T->seam == NULL);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
    return 1;
}

TEST_STATIC void atT_contpoolfree(void) {
    OomCount   oc = {0, 1 << 20};
    at_State  *S = at_open(&oomcount_alloc, &oc);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[300];
    att_Splice w;
    at_Drain   d;
    void      *head, *tail;
    size_t     del;
    int        i, levels;
    assertok(S != NULL && T != NULL);
    atT_modelbuildspread(T, m, 300, 5);
    assertok(T->levels >= 1);
    levels = T->levels;
    assertok(at_seek(&C, T, 500) == AT_OK);
    del = 600;
    w.L = 500, w.del = del, w.ins = 0;
    atT_modelsplice(m, 300, &w);
    d = atT_drainpool(&S->nodes);
    assertok(d.count >= (size_t)levels);
    head = d.chain, tail = NULL;
    for (i = 0; i < levels; ++i) tail = d.chain, d.chain = *(void **)tail;
    *(void **)tail = NULL;
    S->nodes.freed = head, S->nodes.freed_obj = (size_t)levels;
    oc.oom = 0;
    assertok(at_remove(&C, del) == AT_OK);
    assertok(at_checkcursor(&C, 500));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(T->seam == NULL);
    assertok(atT_modelok(T, m, 300));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST_STATIC void atT_contrandom(void) {
    size_t seed = 0x51deu, del, ins;
    int    step, op, grav, side, inside, force, pre;
    for (step = 0; step < 48; ++step) {
        op = (int)(atT_lcg(&seed) % 4u);
        if (atT_lcg(&seed) & 1u)
            grav = (atT_lcg(&seed) & 1u) ? AT_SHIFT : AT_FIXED, side = 1;
        else
            grav = (atT_lcg(&seed) & 1u) ? AT_GROW : AT_LOCK, side = 0;
        del = atT_lcg(&seed) % 80u, ins = atT_lcg(&seed) % 20u;
        inside = atT_lcg(&seed) % 3u == 0;
        force = !inside && op == 0 && atT_lcg(&seed) % 8u == 0;
        pre = inside ? 0 : 1;
        if (inside) side = -1;
        if (op == 0 && del == 0) del = 1;
        if (op != 0 && ins == 0) ins = 1;
        assertok(atT_conteditok(op, grav, pre, side, force, inside, del, ins));
    }
}

TEST_STATIC void atT_contremovecases(void) {
    assertok(atT_conteditok(0, -1, 0, -1, 0, 0, 40, 0));
    assertok(atT_conteditok(0, -1, 1, 0, 0, 0, 40, 0));
    assertok(atT_conteditok(0, AT_SHIFT, 1, 1, 0, 0, 40, 0));
    assertok(atT_conteditok(0, -1, 0, -1, 1, 0, 40, 0));
    assertok(atT_conteditok(0, -1, 1, 0, 0, 0, 1, 0));
    assertok(atT_conteditok(0, AT_SHIFT, 1, 1, 0, 0, 1, 0));
    assertok(atT_contseqok(-1, 0, -1, 1, 1, 40, 20, 15));
    assertok(atT_contseqok(AT_SHIFT, 1, 1, 0, 1, 40, 10, 5));
    assertok(atT_contseqok(AT_SHIFT, 1, 1, 0, 0, 40, 15, 0));
    atT_contrandom();
    atT_contpoolfree();
}

TEST_STATIC void atT_continsertcases(void) {
    assertok(atT_conteditok(1, -1, 0, -1, 0, 1, 0, 15));
    assertok(atT_conteditok(1, AT_SHIFT, 1, 1, 0, 0, 0, 15));
    assertok(atT_conteditok(2, -1, 0, -1, 0, 1, 0, 15));
    assertok(atT_conteditok(3, AT_SHIFT, 1, 1, 0, 0, 20, 15));
}

TEST_STATIC int atT_nonlastleafedge(
        const at_Tree *T, size_t *pos, size_t *span) {
    at_Seq         s;
    const at_Node *lastleaf = atT_lastleaf(T);
    at_Edge       *e;
    int            i;
    atT_seq(T, &s);
    for (i = 0; i < s.n; ++i) {
        e = s.edge[i];
        if (e->parent == lastleaf || s.span[i] == 0) continue;
        if ((int)e->idx == atN_cc(e->parent) - 1)
            return *pos = s.pos[i], *span = s.span[i], 1;
    }
    return 0;
}

#define ATT_EDIT_REMOVE 0
#define ATT_EDIT_INSERT 1
#define ATT_EDIT_APPEND 2
#define ATT_EDIT_SPLICE 3

TEST_STATIC void atT_poolfill(at_State *S, int donors) {
    at_Id id;
    int   i, j;
    for (i = 0; i < donors; ++i) {
        at_Tree *d = at_newtree(S);
        for (j = 0; j < 256; ++j)
            atT_anchor(d, (size_t)j * 5u + 1u, 0, AT_GROW, &id);
        at_deltree(d);
    }
}

TEST_STATIC size_t atT_editneednodes(const at_Tree *T, int op) {
    size_t l = (size_t)T->levels;
    if (op == ATT_EDIT_REMOVE) return l;
    if (op == ATT_EDIT_INSERT || op == ATT_EDIT_APPEND) return l + 2u;
    return 2u * l + 2u;
}

TEST_STATIC size_t atT_editneedpages(const at_Tree *T, int op) {
    return atT_poolpages(sizeof(at_Node), atT_editneednodes(T, op));
}

TEST_STATIC void atT_oomedit(int op, int stage) {
    OomCount      oc = {0, 1 << 20};
    at_State     *S = at_open(&oomcount_alloc, &oc);
    at_Tree      *T;
    at_Cursor     C, C0;
    att_EpochSnap before, after;
    att_TreeSnap  ts;
    size_t        need, pos, del, ins, off, bytes, len;
    unsigned      epoch;
    long          alloc_live;
    int           i, live, r;
    at_Id         id, got;
    assertok(S != NULL);
    atT_poolfill(S, 4);
    T = at_newtree(S);
    for (i = 0; i < 40; ++i)
        atT_anchor(T, (size_t)i * 7u + 1u, 0, AT_GROW, &id);
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    need = atT_editneednodes(T, op);
    pos = 100, del = 40, ins = 30;
    assertok(pos < at_bytes(T));
    assertok(at_seek(&C, T, pos) == AT_OK);
    C0 = C;
    atT_snapshot(T, &before);
    atT_treesnap(T, &ts);
    off = at_offset(&C), bytes = at_bytes(T), epoch = T->epoch;
    live = atT_liveanchors(T);
    if (stage == 2) {
        (void)atT_drainpool(&S->nodes), (void)atT_drainpool(&S->edges);
    } else if (stage == 1) {
        (void)atT_drainpool(&S->edges);
        assertok(S->nodes.freed_obj >= need);
    } else {
        (void)atT_drainpool(&S->nodes), (void)atT_drainpool(&S->edges);
    }
    oc.oom = stage == 2 ? (int)atT_editneedpages(T, op) : 0;
    alloc_live = oc.live;
    if (op == ATT_EDIT_REMOVE)
        r = at_remove(&C, del);
    else if (op == ATT_EDIT_INSERT)
        r = at_insert(&C, ins);
    else if (op == ATT_EDIT_APPEND)
        r = at_append(&C, ins);
    else
        r = at_splice(&C, del, ins);
    assertok(r == AT_ERRMEM);
    assertok(memcmp(&C0, &C, sizeof C) == 0);
    if (stage == 2) {
        assertok(oc.live > alloc_live);
        assertok(S->nodes.freed_obj >= need);
    } else {
        assertok(oc.live == alloc_live);
    }
    assertok(T->epoch == epoch);
    assertok(at_offset(&C) == off);
    asserteq(at_bytes(T), bytes);
    atT_snapshot(T, &after);
    assertok(atT_snapeq(&before, &after));
    assertok(atT_liveanchors(T) == live);
    assertok(atT_treesnapeq(T, &ts));
    atT_stitchinv(T);
    oc.oom = 1 << 20;
    assertok(at_advance(&C, 0) == AT_OK);
    got = at_next(&C, 0, &len);
    assertok(got != AT_NONE);
    off = at_offset(&C);
    assertok(at_insert(&C, 5) == AT_OK);
    asserteq(at_bytes(T), bytes + 5);
    assertok(at_checkcursor(&C, off));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(T->seam == NULL);
    assertok(atT_rtempty(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST(remove_params) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C, D, Z;
    at_Id     a, b;
    size_t    x, y;
    unsigned  e;
    assertok(S != NULL && T != NULL);
    memset(&Z, 0, sizeof(Z));
    assertok(at_remove(NULL, 1) == AT_ERRPARAM);
    assertok(at_remove(&Z, 1) == AT_ERRPARAM);
    atT_anchor(T, 10, 5, AT_GROW, &a);
    atT_anchor(T, 50, 5, AT_LOCK, &b);
    assertok(at_seek(&C, T, 10) == AT_OK);
    e = T->epoch;
    assertok(at_remove(&C, 0) == AT_OK);
    asserteq(T->epoch, e);
    assertok(at_checkcursor(&C, 10));
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_seek(&D, T, 0) == AT_OK);
    assertok(at_remove(&D, 1) == AT_OK);
    assertok(at_remove(&C, 1) == AT_ESTALE);
    assertok(at_locate(&C, 10) == AT_OK);
    assertok(at_remove(&C, (size_t)-1) == AT_OK);
    asserteq(at_bytes(T), 10);
    assertok(at_checkcursor(&C, 10));
    assertok(at_checktree(T));
    assertok(atT_checkchains(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 9 && y == 10);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 10 && y == 10);
    e = T->epoch;
    assertok(at_remove(&C, 7) == AT_OK);
    asserteq(T->epoch, e);
    assertok(at_checkcursor(&C, 10));
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_remove(&C, (size_t)-1) == AT_OK);
    asserteq(at_bytes(T), 0);
    assertok(at_checkcursor(&C, 0));
    e = T->epoch;
    assertok(at_remove(&C, (size_t)-1) == AT_OK);
    asserteq(T->epoch, e);
    atT_stitchinv(T);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(remove_cursor) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[8];
    att_Splice w;
    at_Seq     s;
    at_Id      got[8];
    size_t     L, del;
    int        i, idx;
    assertok(S != NULL && T != NULL);
    atT_contremovecases();
    for (i = 0; i < 8; ++i) {
        m[i].gravity = i % 4;
        assertok(at_seek(&C, T, 10u + (size_t)i * 20u) == AT_OK);
        assertok(
                at_newanchor(
                        &C, (size_t)(i % 4) * 3u, 0, m[i].gravity, &m[i].id)
                == AT_OK);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
    }
    w.L = 20, w.del = 60, w.ins = 0;
    atT_modelsplice(m, 8, &w);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_remove(&C, 60) == AT_OK);
    assertok(at_checkcursor(&C, 20));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 8, got, 8);
    assertok(at_checkcursor(&C, 20));
    assertok(atT_seq(T, &s));
    assertok(s.n >= 3);
    idx = 1;
    while (idx < s.n && s.span[idx] == 0) ++idx;
    assertok(idx < s.n && s.span[idx] > 0);
    L = s.pos[idx], del = s.span[idx];
    w.L = L, w.del = del, w.ins = 0;
    atT_modelsplice(m, 8, &w);
    assertok(at_seek(&C, T, L) == AT_OK);
    assertok(at_remove(&C, del) == AT_OK);
    assertok(at_checkcursor(&C, L));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 8, got, 8);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(insert_cursor) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C, D, Z;
    att_Model  m[6];
    att_Splice w;
    at_Id      got[6];
    size_t     bytes;
    unsigned   e;
    int        i;
    assertok(S != NULL && T != NULL);
    atT_continsertcases();
    memset(&Z, 0, sizeof(Z));
    for (i = 0; i < 6; ++i) {
        m[i].gravity = i % 4;
        assertok(at_seek(&C, T, 20u + (size_t)i * 20u) == AT_OK);
        assertok(at_newanchor(&C, 10, 0, m[i].gravity, &m[i].id) == AT_OK);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
    }
    bytes = at_bytes(T);
    w.L = 50, w.del = 0, w.ins = 20;
    atT_modelsplice(m, 6, &w);
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_insert(&C, 20) == AT_OK);
    assertok(at_checkcursor(&C, 50));
    asserteq(at_bytes(T), bytes + 20);
    atT_stitchinv(T);
    atT_modelchecks(T, m, 6, got, 6);
    e = T->epoch;
    assertok(at_insert(&C, 0) == AT_OK);
    asserteq(T->epoch, e);
    assertok(at_checkcursor(&C, 50));
    assertok(at_seek(&D, T, 0) == AT_OK);
    w.L = 50, w.del = 0, w.ins = 7;
    atT_modelsplice(m, 6, &w);
    assertok(at_append(&C, 7) == AT_OK);
    assertok(at_checkcursor(&C, 57));
    assertok(at_insert(&D, 1) == AT_ESTALE);
    atT_modelchecks(T, m, 6, got, 6);
    bytes = at_bytes(T);
    assertok(at_seek(&C, T, bytes) == AT_OK);
    assertok(at_insert(&C, (size_t)-1) == AT_ERROVERFLOW);
    asserteq(at_bytes(T), bytes);
    assertok(at_checkcursor(&C, bytes));
    atT_stitchinv(T);
    assertok(at_insert(NULL, 1) == AT_ERRPARAM);
    assertok(at_insert(&Z, 1) == AT_ERRPARAM);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(insert_cursor_growleft) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a;
    assertok(S != NULL && T != NULL);
    atT_anchor(T, 10, 5, AT_FIXED, &a);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_insert(&C, 20) == AT_OK);
    assertok(at_checkcursor(&C, 10));
    assertok(C.cur.id == 0 && C.cur.is_end == 0);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

TEST(insert_cursor_splitmove) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    assertok(S != NULL && T != NULL);
    atT_anchor(T, 10, 5, AT_FIXED, &a);
    atT_anchor(T, 0, 10, AT_FIXED, &b);
    assertok(a != b);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_insert(&C, 20) == AT_OK);
    assertok(at_checkcursor(&C, 10));
    assertok(C.cur.id == 0 && C.cur.is_end == 0);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

TEST(append_cursor) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C, D, Z;
    att_Model  m[6];
    att_Splice w;
    at_Id      got[6];
    size_t     bytes;
    unsigned   e;
    int        i;
    assertok(S != NULL && T != NULL);
    memset(&Z, 0, sizeof(Z));
    for (i = 0; i < 6; ++i) {
        m[i].gravity = i % 4;
        assertok(at_seek(&C, T, 20u + (size_t)i * 20u) == AT_OK);
        assertok(at_newanchor(&C, 10, 0, m[i].gravity, &m[i].id) == AT_OK);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
    }
    bytes = at_bytes(T);
    w.L = 50, w.del = 0, w.ins = 25;
    atT_modelsplice(m, 6, &w);
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_append(&C, 25) == AT_OK);
    assertok(at_checkcursor(&C, 75));
    asserteq(at_bytes(T), bytes + 25);
    atT_stitchinv(T);
    atT_modelchecks(T, m, 6, got, 6);
    e = T->epoch;
    assertok(at_append(&C, 0) == AT_OK);
    asserteq(T->epoch, e);
    assertok(at_checkcursor(&C, 75));
    assertok(at_seek(&D, T, 0) == AT_OK);
    assertok(at_append(&D, 4) == AT_OK);
    assertok(at_append(&C, 4) == AT_ESTALE);
    assertok(at_seek(&C, T, at_bytes(T)) == AT_OK);
    assertok(at_append(&C, (size_t)-1) == AT_ERROVERFLOW);
    assertok(at_append(NULL, 1) == AT_ERRPARAM);
    assertok(at_append(&Z, 1) == AT_ERRPARAM);
    atT_stitchinv(T);
    at_reset(T);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_append(&C, 5) == AT_OK);
    asserteq(at_bytes(T), 5);
    assertok(at_checkcursor(&C, 5));
    atT_stitchinv(T);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(splice_atomic_oom) {
    atT_oomedit(ATT_EDIT_REMOVE, 0);
    atT_oomedit(ATT_EDIT_INSERT, 0);
    atT_oomedit(ATT_EDIT_INSERT, 1);
    atT_oomedit(ATT_EDIT_INSERT, 2);
    atT_oomedit(ATT_EDIT_APPEND, 0);
    atT_oomedit(ATT_EDIT_APPEND, 1);
    atT_oomedit(ATT_EDIT_APPEND, 2);
    atT_oomedit(ATT_EDIT_SPLICE, 0);
    atT_oomedit(ATT_EDIT_SPLICE, 1);
    atT_oomedit(ATT_EDIT_SPLICE, 2);
}

TEST(remove_append_equivalence) {
    Count      ca = {0}, cb = {0};
    at_State  *SA = at_open(&count_alloc, &ca);
    at_State  *SB = at_open(&count_alloc, &cb);
    at_Tree   *A = at_newtree(SA), *B = at_newtree(SB);
    att_Model  ma[AT_BRUTE_N], mb[AT_BRUTE_N];
    at_Cursor  CA, CB;
    att_Splice w;
    at_Seq     sa, sb;
    size_t     seed = 0x5eed1234u, bound, L, del, ins;
    int        step;
    assertok(SA != NULL && SB != NULL && A != NULL && B != NULL);
    atT_brutebuild(A, ma);
    atT_brutebuild(B, mb);
    for (step = 0; step < 300; ++step) {
        bound = atT_modelbound(ma, AT_BRUTE_N);
        L = atT_lcg(&seed) % (bound + 20u);
        del = atT_lcg(&seed) % 25u, ins = atT_lcg(&seed) % 10u;
        w.L = L, w.del = del, w.ins = ins;
        atT_modelsplice(ma, AT_BRUTE_N, &w);
        atT_modelsplice(mb, AT_BRUTE_N, &w);
        assertok(at_seek(&CA, A, L) == AT_OK);
        assertok(at_remove(&CA, del) == AT_OK);
        assertok(at_append(&CA, ins) == AT_OK);
        assertok(at_seek(&CB, B, L) == AT_OK);
        assertok(at_splice(&CB, del, ins) == AT_OK);
        assertok(at_offset(&CA) == L + ins && at_offset(&CB) == L + ins);
        assertok(at_checktree(A) && atT_checkchains(A));
        assertok(at_checktree(B) && atT_checkchains(B));
        assertok(atT_rtempty(A) && atT_rtempty(B));
        assertok(A->seam == NULL && B->seam == NULL);
        atT_seq(A, &sa), atT_seq(B, &sb);
        assertok(atT_seqeq(&sa, &sb));
        asserteq(at_bytes(A), at_bytes(B));
        asserteq(A->levels, B->levels);
        assertok(atT_modelok(A, ma, AT_BRUTE_N));
        assertok(atT_modelok(B, mb, AT_BRUTE_N));
    }
    at_deltree(A), at_close(SA);
    at_deltree(B), at_close(SB);
    asserteq(ca.live, 0);
    asserteq(cb.live, 0);
}

TEST(stitch_delete_cross_leaf) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[40];
    att_Splice w;
    at_Id      got[40];
    assertok(S != NULL && T != NULL);
    atT_modelbuildspread(T, m, 40, 7);
    assertok(T->levels >= 1);
    w.L = 12, w.del = 120, w.ins = 0;
    atT_modelsplice(m, 40, &w);
    assertok(at_seek(&C, T, 12) == AT_OK);
    assertok(at_remove(&C, 120) == AT_OK);
    assertok(at_checkcursor(&C, 12));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 40, got, 40);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_delete_multi_level) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[300];
    att_Splice w;
    at_Id      got[300];
    assertok(S != NULL && T != NULL);
    atT_modelbuildspread(T, m, 300, 5);
    assertok(atT_checkdepth(T, 2));
    w.L = 500, w.del = 600, w.ins = 0;
    atT_modelsplice(m, 300, &w);
    assertok(at_seek(&C, T, 500) == AT_OK);
    assertok(at_remove(&C, 600) == AT_OK);
    assertok(at_checkcursor(&C, 500));
    atT_stitchinv(T);
    assertok(atT_checkdepth(T, 2));
    atT_modelchecks(T, m, 300, got, 300);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_delete_tail) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[80];
    att_Splice w;
    at_Seq     s;
    at_Id      got[80];
    size_t     bytes;
    assertok(S != NULL && T != NULL);
    atT_modelbuildspread(T, m, 80, 7);
    bytes = at_bytes(T);
    w.L = 200, w.del = bytes - 200, w.ins = 0;
    atT_modelsplice(m, 80, &w);
    assertok(at_seek(&C, T, 200) == AT_OK);
    assertok(at_remove(&C, bytes - 200) == AT_OK);
    assertok(at_checkcursor(&C, 200));
    asserteq(at_bytes(T), 200);
    atT_stitchinv(T);
    atT_modelchecks(T, m, 80, got, 80);
    assertok(atT_seq(T, &s));
    assertok(s.span[s.n - 1] == 0);
    assertok(atT_anchorslots(T, s.edge[s.n - 1]) > 0);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_delete_same_leaf) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    at_Id      a;
    att_Model  m[1];
    att_Splice w;
    at_Seq     s;
    at_Id      got[1];
    size_t     L, del;
    int        idx;
    assertok(S != NULL && T != NULL);
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    atT_anchor(T, 50, 20, AT_FIXED, &a);
    assertok(T->levels == 0);
    m[0].id = a, m[0].gravity = AT_FIXED;
    assertok(at_range(T, a, &m[0].start, &m[0].end) == AT_OK);
    w.L = 10, w.del = 5, w.ins = 0;
    atT_modelsplice(m, 1, &w);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_remove(&C, 5) == AT_OK);
    assertok(at_checkcursor(&C, 10));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 1, got, 1);
    assertok(atT_seq(T, &s));
    idx = 1;
    while (idx < s.n && s.span[idx] == 0) ++idx;
    assertok(idx < s.n && s.span[idx] > 0);
    L = s.pos[idx], del = s.span[idx];
    w.L = L, w.del = del, w.ins = 0;
    atT_modelsplice(m, 1, &w);
    assertok(at_seek(&C, T, L) == AT_OK);
    assertok(at_remove(&C, del) == AT_OK);
    assertok(at_checkcursor(&C, L));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 1, got, 1);
    assertok(atT_seq(T, &s));
    idx = s.n - 1;
    if (s.span[idx] == 0) idx -= 1;
    assertok(idx >= 0 && s.span[idx] > 0);
    L = s.pos[idx], del = s.span[idx];
    w.L = L, w.del = del, w.ins = 0;
    atT_modelsplice(m, 1, &w);
    assertok(at_seek(&C, T, L) == AT_OK);
    assertok(at_remove(&C, del) == AT_OK);
    assertok(at_checkcursor(&C, L));
    asserteq(at_bytes(T), L);
    atT_stitchinv(T);
    atT_modelchecks(T, m, 1, got, 1);
    at_reset(T);
    assertok(atT_setupinsert(T, 0, 10) == AT_OK);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_remove(&C, 10) == AT_OK);
    asserteq(at_bytes(T), 0);
    assertok(at_checkcursor(&C, 0));
    atT_stitchinv(T);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_seam_cleared) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[60];
    att_Splice w;
    at_Id      got[60];
    size_t     L;
    assertok(S != NULL && T != NULL);
    atT_modelbuildspread(T, m, 60, 7);
    w.L = 30, w.del = 100, w.ins = 0;
    atT_modelsplice(m, 60, &w);
    assertok(at_seek(&C, T, 30) == AT_OK);
    assertok(at_remove(&C, 100) == AT_OK);
    assertok(at_checkcursor(&C, 30));
    atT_stitchinv(T);
    L = at_bytes(T) / 2u;
    w.L = L, w.del = 40, w.ins = 0;
    atT_modelsplice(m, 60, &w);
    assertok(at_seek(&C, T, L) == AT_OK);
    assertok(at_remove(&C, 40) == AT_OK);
    assertok(at_checkcursor(&C, L));
    atT_stitchinv(T);
    L = at_bytes(T) / 3u;
    w.L = L, w.del = 0, w.ins = 25;
    atT_modelsplice(m, 60, &w);
    assertok(at_seek(&C, T, L) == AT_OK);
    assertok(at_insert(&C, 25) == AT_OK);
    assertok(at_checkcursor(&C, L));
    atT_stitchinv(T);
    L = at_bytes(T) / 4u;
    w.L = L, w.del = 10, w.ins = 15;
    atT_modelsplice(m, 60, &w);
    assertok(at_seek(&C, T, L) == AT_OK);
    assertok(at_splice(&C, 10, 15) == AT_OK);
    assertok(at_checkcursor(&C, L + 15));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 60, got, 60);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_rt_cleared) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[80];
    att_Splice w;
    at_Id      got[80];
    size_t     L;
    int        k;
    assertok(S != NULL && T != NULL);
    atT_modelbuildspread(T, m, 80, 5);
    for (k = 0; k < 3; ++k) {
        L = 10u + (size_t)k * 40u;
        w.L = L, w.del = 30u + (size_t)k * 20u, w.ins = 0;
        atT_modelsplice(m, 80, &w);
        assertok(at_seek(&C, T, L) == AT_OK);
        assertok(at_remove(&C, w.del) == AT_OK);
        assertok(at_checkcursor(&C, L));
        assertok(at_checktree(T));
        assertok(atT_checkchains(T));
        assertok(T->seam == NULL);
        assertok(atT_rtempty(T));
    }
    L = at_bytes(T) / 2u;
    w.L = L, w.del = at_bytes(T) - L, w.ins = 0;
    atT_modelsplice(m, 80, &w);
    assertok(at_seek(&C, T, L) == AT_OK);
    assertok(at_remove(&C, w.del) == AT_OK);
    assertok(at_checkcursor(&C, L));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 80, got, 80);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_random_edit) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[160];
    att_Splice w;
    at_Id      got[160];
    size_t     seed = 0xc0ffeeu, bound, pos, L, del, ins, len, want;
    at_Id      id;
    int        op, step, k, nlive = 80;
    assertok(S != NULL && T != NULL);
    atT_modelbuildspread(T, m, nlive, 5);
    for (step = 0; step < 20000; ++step) {
        bound = atT_modelbound(m, nlive);
        op = (int)(atT_lcg(&seed) % 6u);
        if (op == 0 && nlive < 160) {
            pos = atT_lcg(&seed) % (bound ? bound + 1u : 20u);
            len = atT_lcg(&seed) % 40u;
            ins = atT_lcg(&seed) % 4u;
            assertok(at_seek(&C, T, pos) == AT_OK);
            assertok(at_newanchor(&C, len, 0, (int)ins, &id) == AT_OK);
            m[nlive].id = id, m[nlive].start = pos, m[nlive].end = pos + len;
            m[nlive].gravity = (int)ins, nlive += 1;
            atT_stitchinv(T);
            assertok(atT_modelok(T, m, nlive));
            continue;
        }
        if (op == 1 && nlive > 0) {
            k = (int)(atT_lcg(&seed) % (size_t)nlive);
            assertok(at_delanchor(T, m[k].id) == AT_OK);
            m[k] = m[nlive - 1], nlive -= 1;
            atT_stitchinv(T);
            assertok(atT_modelok(T, m, nlive));
            continue;
        }
        op = 2 + (int)(atT_lcg(&seed) % 4u);
        L = atT_lcg(&seed) % (bound + 20u);
        del = atT_lcg(&seed) % 40u, ins = atT_lcg(&seed) % 15u;
        w.L = L, w.del = op == 2 || op == 5 ? del : 0;
        w.ins = op == 3 || op == 4 || op == 5 ? ins : 0;
        want = op == 4 || op == 5 ? L + w.ins : L;
        atT_modelsplice(m, nlive, &w);
        assertok(at_seek(&C, T, L) == AT_OK);
        if (op == 2)
            assertok(at_remove(&C, w.del) == AT_OK);
        else if (op == 3)
            assertok(at_insert(&C, w.ins) == AT_OK);
        else if (op == 4)
            assertok(at_append(&C, w.ins) == AT_OK);
        else
            assertok(at_splice(&C, w.del, w.ins) == AT_OK);
        assertok(at_checkcursor(&C, want));
        assertok(at_checktree(T));
        assertok(atT_checkchains(T));
        assertok(T->seam == NULL);
        assertok(atT_rtempty(T));
        assertok(atT_modelok(T, m, nlive));
        if (step % 20 == 0) assertok(atT_modelnext(T, m, nlive, got, 160));
    }
    atT_modelchecks(T, m, nlive, got, 160);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_delete_boundary) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[200];
    att_Splice w;
    at_Id      got[200];
    size_t     pos, span;
    assertok(S != NULL && T != NULL);
    atT_modelbuildspread(T, m, 200, 5);
    assertok(atT_nonlastleafedge(T, &pos, &span));
    assertok(pos + span + 1 <= at_bytes(T));
    w.L = pos, w.del = span + 1, w.ins = 0;
    atT_modelsplice(m, 200, &w);
    assertok(at_seek(&C, T, pos) == AT_OK);
    assertok(at_remove(&C, span + 1) == AT_OK);
    assertok(at_checkcursor(&C, pos));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 200, got, 200);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_delete_brute) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[32];
    att_Splice w;
    size_t     limit, L, del;
    assertok(S != NULL && T != NULL);
    at_reset(T), atT_modelbuildspread(T, m, 32, 5);
    assertok(T->levels >= 1);
    limit = at_bytes(T);
    for (L = 0; L <= limit; ++L) {
        for (del = 1; del <= limit - L; ++del) {
            at_reset(T), atT_modelbuildspread(T, m, 32, 5);
            w.L = L, w.del = del, w.ins = 0;
            atT_modelsplice(m, 32, &w);
            assertok(at_seek(&C, T, L) == AT_OK);
            assertok(at_remove(&C, del) == AT_OK);
            assertok(at_checkcursor(&C, L));
            atT_stitchinv(T);
            assertok(atT_noemptyedges(T));
            assertok(atT_modelok(T, m, 32));
        }
    }
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_firstedge_keep) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[3];
    att_Splice w;
    at_Seq     s;
    at_Id      got[3];
    assertok(S != NULL && T != NULL);
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    atT_anchor(T, 10, 5, AT_GROW, &m[0].id);
    atT_anchor(T, 40, 5, AT_LOCK, &m[1].id);
    atT_anchor(T, 70, 5, AT_FIXED, &m[2].id);
    m[0].gravity = AT_GROW, m[1].gravity = AT_LOCK, m[2].gravity = AT_FIXED;
    assertok(at_range(T, m[0].id, &m[0].start, &m[0].end) == AT_OK);
    assertok(at_range(T, m[1].id, &m[1].start, &m[1].end) == AT_OK);
    assertok(at_range(T, m[2].id, &m[2].start, &m[2].end) == AT_OK);
    assertok(atT_seq(T, &s));
    assertok(s.span[0] == 10 && atT_empty(s.edge[0]));
    w.L = 5, w.del = 30, w.ins = 0;
    atT_modelsplice(m, 3, &w);
    assertok(at_seek(&C, T, 5) == AT_OK);
    assertok(at_remove(&C, 30) == AT_OK);
    assertok(at_checkcursor(&C, 5));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 3, got, 3);
    assertok(atT_seq(T, &s));
    assertok(s.span[0] == 5 && atT_empty(s.edge[0]));
    atT_rangeeq(T, m[0].id, 5, 5);
    atT_rangeeq(T, m[1].id, 10, 15);
    atT_rangeeq(T, m[2].id, 40, 45);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(stitch_tail_seam_anchors) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    att_Model  m[2];
    att_Splice w;
    at_Seq     s;
    at_Id      got[2];
    assertok(S != NULL && T != NULL);
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    atT_anchor(T, 50, 0, AT_GROW, &m[0].id);
    atT_anchor(T, 100, 0, AT_LOCK, &m[1].id);
    m[0].gravity = AT_GROW, m[1].gravity = AT_LOCK;
    assertok(at_range(T, m[0].id, &m[0].start, &m[0].end) == AT_OK);
    assertok(at_range(T, m[1].id, &m[1].start, &m[1].end) == AT_OK);
    w.L = 50, w.del = 50, w.ins = 0;
    atT_modelsplice(m, 2, &w);
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_remove(&C, 50) == AT_OK);
    assertok(at_checkcursor(&C, 50));
    asserteq(at_bytes(T), 50);
    atT_stitchinv(T);
    atT_modelchecks(T, m, 2, got, 2);
    assertok(atT_seq(T, &s));
    assertok(s.span[s.n - 1] == 0);
    assertok(atT_anchorslots(T, s.edge[s.n - 1]) > 0);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(purge_insertion) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    at_Cursor  C;
    at_Id      a;
    att_Model  m[1];
    att_Splice w;
    at_Id      got[1];
    size_t     x, y;
    assertok(S != NULL && T != NULL);
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    atT_anchor(T, 50, 20, AT_FIXED, &a);
    m[0].id = a, m[0].gravity = AT_FIXED;
    assertok(at_range(T, a, &m[0].start, &m[0].end) == AT_OK);
    w.L = 50, w.del = 0, w.ins = 30;
    atT_modelsplice(m, 1, &w);
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_insert(&C, 30) == AT_OK);
    assertok(at_checkcursor(&C, 50));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 1, got, 1);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 80 && y == 100);
    at_reset(T);
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    atT_anchor(T, 0, 20, AT_FIXED, &a);
    m[0].id = a, m[0].gravity = AT_FIXED;
    assertok(at_range(T, a, &m[0].start, &m[0].end) == AT_OK);
    w.L = 0, w.del = 0, w.ins = 30;
    atT_modelsplice(m, 1, &w);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_insert(&C, 30) == AT_OK);
    assertok(at_checkcursor(&C, 0));
    atT_stitchinv(T);
    atT_modelchecks(T, m, 1, got, 1);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 30 && y == 50);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(purge_delanchor) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Id     a;
    size_t    x, y;
    assertok(S != NULL && T != NULL);
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    atT_anchor(T, 50, 0, AT_GROW, &a);
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_range(T, a, &x, &y) == AT_ERRPARAM);
    asserteq(at_bytes(T), 100);
    atT_stitchinv(T);
    at_reset(T);
    atT_anchor(T, 0, 0, AT_GROW, &a);
    assertok(at_delanchor(T, a) == AT_OK);
    asserteq(at_bytes(T), 0);
    atT_stitchinv(T);
    at_reset(T);
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    atT_anchor(T, 100, 0, AT_GROW, &a);
    assertok(at_delanchor(T, a) == AT_OK);
    asserteq(at_bytes(T), 100);
    atT_stitchinv(T);
    at_reset(T);
    assertok(atT_setupinsert(T, 0, 100) == AT_OK);
    atT_anchor(T, 50, 20, AT_FIXED, &a);
    assertok(at_delanchor(T, a) == AT_OK);
    asserteq(at_bytes(T), 100);
    atT_stitchinv(T);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}
#include "anchortree_test_fanout4.gen.inc"
