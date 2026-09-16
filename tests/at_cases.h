/* at_cases.h -- shared black-box anchortree test cases.
 *
 * Included by the fanout-4/5/8 runners and by the odd fanout-7/9 runners
 * (which recompile the fanout5 source).  Each case is a TEST_STATIC function
 * wrapped by a TEST(...) entry in the runner so tests/gen_entries.lua can
 * scan the runner source.
 */

#ifndef AT_CASES_H
#define AT_CASES_H

#include "at_tests.h"

TEST_STATIC int att_insert_seq(at_Tree *T, const size_t *pos, int n) {
    at_Edge *e;
    int      i;
    for (i = 0; i < n; ++i)
        if (att_setup_edge(T, pos[i], &e) != AT_OK) return 0;
    return att_checktree(T);
}
TEST_STATIC int att_positions_match(at_Tree *T, const size_t *pos, int n) {
    at_Seq s;
    int    i;
    if (att_seq(T, &s) != n) return 0;
    for (i = 0; i < n; ++i)
        if (s.pos[i] != pos[i]) return 0;
    return 1;
}

TEST_STATIC void att_rangeeq(at_Tree *T, at_Id id, size_t s, size_t e) {
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
TEST_STATIC void att_brute_build(at_Tree *T, att_Model *m) {
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
TEST_STATIC int att_splice_brute_ok(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    att_Model m[AT_BRUTE_N];
    size_t    pos, del, ins, limit;
    att_brute_build(T, m);
    limit = at_bytes(T) + 1;
    for (pos = 0; pos <= limit; ++pos) {
        for (del = 0; del <= limit; ++del) {
            for (ins = 0; ins <= 2; ++ins) {
                att_Splice w;
                at_reset(T), att_brute_build(T, m);
                w.L = pos, w.del = del, w.ins = ins;
                att_model_splice(m, AT_BRUTE_N, &w);
                assertok(at_seek(&C, T, pos) == AT_OK);
                assertok(at_splice(&C, del, ins) == AT_OK);
                assertok(att_checktree(T));
                assertok(att_no_empty_edges(T));
                assertok(att_checkcursor(&C, pos));
                assertok(att_model_ok(T, m, AT_BRUTE_N));
            }
        }
    }
    at_deltree(T), at_close(S);
    return 1;
}

TEST_STATIC void att_random_splice_run(at_Tree *T, att_Model *m, int n) {
    at_Cursor  C;
    att_Splice w;
    size_t     L, del, ins, bound;
    size_t     seed = 7;
    int        step;
    for (step = 0; step < 300; ++step) {
        bound = att_model_bound(m, n);
        L = att_lcg(&seed) % (bound + 20u);
        del = att_lcg(&seed) % 25u;
        ins = att_lcg(&seed) % 10u;
        w.L = L, w.del = del, w.ins = ins;
        att_model_splice(m, n, &w);
        assertok(at_seek(&C, T, L) == AT_OK);
        assertok(at_splice(&C, del, ins) == AT_OK);
        assertok(att_checktree(T));
        assertok(att_no_empty_edges(T));
        assertok(att_checkcursor(&C, L));
        assertok(att_model_ok(T, m, n));
    }
}

TEST_STATIC int att_splice_random_ok(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    att_Model m[10];
    int       i;
    for (i = 0; i < 10; ++i) {
        m[i].gravity = i % 4;
        m[i].id = att_anew(
                T, (size_t)(10 + i * 3), (size_t)(i * 2 + 1), m[i].gravity);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
        assertok(att_checktree(T));
        assertok(att_no_empty_edges(T));
    }
    att_random_splice_run(T, m, 10);
    at_deltree(T), at_close(S);
    return 1;
}

TEST_STATIC int att_cns(at_Tree *T, size_t p, int ns, at_Id *out, int cap) {
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
TEST_STATIC int att_collect(at_Tree *T, size_t pos, at_Id *out, int cap) {
    return att_cns(T, pos, 0, out, cap);
}
TEST_STATIC int att_idset(const at_Id *got, int n, const at_Id *want, int m) {
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

TEST_STATIC void att_lifecycle_basic(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Id     id;
    assertok(S != NULL && T != NULL);
    assertok(att_checktree(T));
    asserteq(at_bytes(NULL), 0);
    asserteq(at_bytes(T), 0);
    /* setup-only: force the first node/edge allocation through pages. */
    (void)att_drainpool(&S->nodes), (void)att_drainpool(&S->edges);
    att_anchor(T, 0, 5, AT_GROW, &id);
    assertok(att_checktree(T));
    asserteq(at_bytes(T), 5);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void att_lifecycle_reset(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    int       i;
    for (i = 1; i <= 9; ++i)
        assertok(att_setup_edge(T, (size_t)i * 10u, &e) == AT_OK);
    assertok(att_checktree(T));
    at_reset(T);
    assertok(att_checktree(T));
    asserteq(at_bytes(T), 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
    asserteq(at_newtree(NULL), NULL);
}

TEST_STATIC void att_case_lifecycle(void) {
    att_lifecycle_basic();
    att_lifecycle_reset();
}

TEST_STATIC void att_guards_open(void) {
    OomCount  oc0 = {0, 0}, oc1 = {0, 1};
    at_State *S;
    assertok(at_open(&oomcount_alloc, &oc0) == NULL);
    at_close(NULL), at_deltree(NULL), at_reset(NULL);
    S = at_open(&oomcount_alloc, &oc1);
    assertok(S != NULL);
    assertok(at_newtree(S) == NULL);
    at_close(S);
}

TEST_STATIC void att_guards_cursor(at_Tree *T) {
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
    BadC.tree = NULL;
    assertok(at_splice(&BadC, 0, 0) == AT_ERRPARAM);
    assertok(at_newanchor(&BadC, 0, 0, AT_GROW, &id) == AT_ERRPARAM);
}

TEST_STATIC void att_guards_setup(at_Tree *T) {
    at_Edge *e;
    size_t   x = 0, y = 0;
    assertok(att_setup_edge(NULL, 5, &e) == AT_ERRPARAM);
    assertok(att_setup_edge(T, 5, NULL) == AT_ERRPARAM);
    assertok(att_setup_dropedge(NULL, 1) == AT_ERRPARAM);
    assertok(att_setup_dropedge(T, 0) == AT_ERRPARAM);
    assertok(att_setup_dropedge(T, 1) == AT_ERRPARAM);
    assertok(at_range(NULL, 1, &x, &y) == AT_ERRPARAM);
    assertok(at_range(T, 1, &x, NULL) == AT_ERRPARAM);
    assertok(at_gravity(NULL, 1) == AT_ERRPARAM);
}

TEST_STATIC void att_case_param_guards(void) {
    at_State *S;
    at_Tree  *T;
    att_guards_open();
    S = at_open(NULL, NULL);
    T = at_newtree(S);
    att_guards_cursor(T);
    att_guards_setup(T);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_edge_guards_pos(at_Tree *T) {
    at_Cursor C;
    at_Edge  *e;
    at_Id     id;
    assertok(at_seek(&C, T, (size_t)-1) == AT_OK);
    assertok(at_newanchor(&C, 1, 0, AT_GROW, &id) == AT_ERRPARAM);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_newanchor(&C, (size_t)-1, 0, AT_GROW, &id) == AT_ERRPARAM);
    assertok(att_setup_insert(T, 0, 0) == AT_OK);
    assertok(att_setup_insert(T, 0, 7) == AT_OK);
    assertok(att_setup_edge(T, 10, &e) == AT_OK);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, 0) == AT_OK);
    assertok(att_setup_findedge(T, 0, &e) == 1);
    assertok(att_setup_findedge(T, 11, &e) == 0);
    assertok(att_setup_dropedge(T, 5) == AT_ERRPARAM);
}

TEST_STATIC void att_edge_guards_idcap(at_Tree *T) {
    at_Cursor C;
    at_Id     id;
    size_t    x = 0, y = 0, oldcap;
    assertok(at_seek(&C, T, 0) == AT_OK);
    /* setup-only: force id-pool exhaustion without growing the pool. */
    oldcap = T->idcap;
    T->idcap = (size_t)-1, T->idfree = 0;
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_ERRMEM);
    T->idcap = AT_ANCHOR_MAX, T->idfree = 0;
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_ERRMEM);
    T->idcap = oldcap;
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_OK);
    assertok(at_delanchor(T, 1000) == AT_ERRPARAM);
    assertok(at_range(T, 1000, &x, &y) == AT_ERRPARAM);
    assertok(at_gravity(T, 1000) == AT_ERRPARAM);
    assertok(att_checkchains(T));
}

TEST_STATIC void att_case_edge_guards(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    att_edge_guards_pos(T);
    att_edge_guards_idcap(T);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_empty_seek(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    assertok(at_locate(NULL, 0) == AT_ERRPARAM);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_locate(&C, 0) == AT_OK);
    assertok(att_checkcursor(&C, 0));
    assertok(at_seek(&C, T, 5) == AT_OK);
    assertok(att_checkcursor(&C, 5));
    assertok(at_advance(&C, 3) == AT_OK);
    assertok(att_checkcursor(&C, 8));
    assertok(at_advance(&C, -20) == AT_OK);
    assertok(att_checkcursor(&C, 0));
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_advance(&C, -3) == AT_OK);
    assertok(att_checkcursor(&C, 7));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_seek_positions(void) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Edge            *e;
    at_Cursor           C;
    static const size_t seqp[4] = {0, 10, 30, 50};
    static const size_t seeks[10] = {0, 5, 10, 15, 25, 30, 35, 50, 60, 70};
    int                 i;
    assertok(att_setup_edge(T, 10, &e) == AT_OK);
    assertok(att_setup_edge(T, 50, &e) == AT_OK);
    assertok(att_setup_edge(T, 30, &e) == AT_OK);
    assertok(att_positions_match(T, seqp, 4));
    for (i = 0; i < 10; ++i) {
        assertok(at_seek(&C, T, seeks[i]) == AT_OK);
        assertok(att_checkcursor(&C, seeks[i]));
        assertok(at_offset(&C) == seeks[i]);
    }
    assertok(at_locate(&C, 35) == AT_OK);
    assertok(att_checkcursor(&C, 35));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_advance_positions(void) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Edge            *e;
    at_Cursor           C;
    static const size_t seqp[4] = {0, 10, 30, 50};
    int                 i;
    assertok(att_setup_edge(T, 10, &e) == AT_OK);
    assertok(att_setup_edge(T, 50, &e) == AT_OK);
    assertok(att_setup_edge(T, 30, &e) == AT_OK);
    assertok(att_positions_match(T, seqp, 4));
    for (i = 0; i < 55; ++i) {
        assertok(at_seek(&C, T, 0) == AT_OK);
        assertok(at_advance(&C, (at_Delta)i) == AT_OK);
        assertok(att_checkcursor(&C, (size_t)i));
        assertok(at_advance(&C, -1) == AT_OK);
        assertok(att_checkcursor(&C, i ? (size_t)(i - 1) : 0));
    }
    assertok(at_seek(&C, T, 1000) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(att_checkcursor(&C, 1000));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_advance_span(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    at_Cursor C;
    int       i;
    for (i = 1; i <= 20; ++i)
        assertok(att_setup_edge(T, (size_t)i * 10, &e) == AT_OK);
    /* setup-only: require a multi-level tree for crossing advances. */
    assertok(T->levels > 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_advance(&C, 95) == AT_OK);
    assertok(att_checkcursor(&C, 95));
    assertok(at_advance(&C, 60) == AT_OK);
    assertok(att_checkcursor(&C, 155));
    assertok(at_advance(&C, -40) == AT_OK);
    assertok(att_checkcursor(&C, 115));
    assertok(at_advance(&C, -115) == AT_OK);
    assertok(att_checkcursor(&C, 0));
    assertok(at_advance(&C, 200) == AT_OK);
    assertok(att_checkcursor(&C, 200));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_split_sequence(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    p[51], i;
    at_Edge  *e;
    p[0] = 0;
    for (i = 0; i < 50; ++i) p[i + 1] = (i + 1) * 13u;
    assertok(att_insert_seq(T, p + 1, 50));
    assertok(att_positions_match(T, p, 51));
    assertok(att_setup_edge(T, 650, &e) == AT_OK);
    assertok(att_positions_match(T, p, 51));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_delete_middle(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    before[5] = {10, 30, 50, 80, 100};
    size_t    after[5] = {0, 10, 50, 80, 100};
    size_t    extended[6] = {0, 10, 50, 80, 100, 650};
    at_Edge  *e;
    assertok(att_insert_seq(T, before, 5));
    assertok(att_setup_dropedge(T, 30) == AT_OK);
    assertok(att_positions_match(T, after, 5));
    assertok(att_setup_edge(T, 650, &e) == AT_OK);
    assertok(att_positions_match(T, extended, 6));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_delete_last(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    before[5] = {10, 30, 50, 80, 100};
    size_t    after[5] = {0, 10, 30, 50, 80};
    size_t    split[6] = {0, 10, 30, 50, 80, 90};
    at_Edge  *e;
    assertok(att_insert_seq(T, before, 5));
    assertok(att_setup_dropedge(T, 100) == AT_OK);
    assertok(at_bytes(T) == 100);
    assertok(att_positions_match(T, after, 5));
    assertok(att_setup_edge(T, 90, &e) == AT_OK);
    assertok(at_bytes(T) == 100);
    assertok(att_positions_match(T, split, 6));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_delete_all(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    p[50], i;
    at_Edge  *e;
    for (i = 0; i < 50; ++i) p[i] = (i + 1) * 7u;
    assertok(att_insert_seq(T, p, 50));
    for (i = 0; i < 50; ++i) assertok(att_setup_dropedge(T, p[i]) == AT_OK);
    assertok(at_bytes(T) == 350);
    assertok(att_checktree(T));
    assertok(att_setup_edge(T, 123, &e) == AT_OK);
    assertok(att_checktree(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_random_mutate(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    size_t    p;
    unsigned  seed = 1;
    int       i;
    for (i = 0; i < 200; ++i) {
        seed = seed * 1664525u + 1013904223u;
        p = (seed % 5000u) + 1u;
        assertok(att_setup_edge(T, p, &e) == AT_OK);
        assertok(att_checktree(T));
    }
    for (i = 0; i < 300; ++i) {
        seed = seed * 1664525u + 1013904223u;
        p = (seed % 5000u) + 1u;
        /* setup-only: find an existing zero-offset edge to drop. */
        if (p <= at_bytes(T) && att_setup_findedge(T, p, &e))
            assertok(att_setup_dropedge(T, p) == AT_OK);
        assertok(att_checktree(T));
    }
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_id_reuse(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c;
    size_t    x, y;
    att_anchor(T, 10, 5, AT_GROW, &a);
    att_anchor(T, 20, 5, AT_FIXED, &b);
    asserteq(a, 1);
    asserteq(b, 2);
    assertok(at_delanchor(T, a) == AT_OK);
    att_anchor(T, 30, 5, AT_SHIFT, &c);
    asserteq(c, a);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    asserteq(x, 30);
    asserteq(y, 35);
    assertok(at_gravity(T, c) == AT_SHIFT);
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(at_delanchor(T, c) == AT_OK);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_gravities(void) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               id[4];
    static const int    gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    static const size_t ps[4] = {10, 40, 80, 120};
    int                 i;
    for (i = 0; i < 4; ++i) att_anchor(T, ps[i], 10, gv[i], &id[i]);
    for (i = 0; i < 4; ++i) assertok(at_gravity(T, id[i]) == gv[i]);
    for (i = 0; i < 4; ++i) assertok(at_delanchor(T, id[i]) == AT_OK);
    asserteq(at_bytes(T), 130);
    assertok(att_checktree(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_zero_length(void) {
    at_State        *S = at_open(NULL, NULL);
    at_Tree         *T = at_newtree(S);
    at_Id            id[4];
    static const int gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    size_t           x, y;
    int              i;
    for (i = 0; i < 4; ++i) att_anchor(T, 100, 0, gv[i], &id[i]);
    for (i = 0; i < 4; ++i) {
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        asserteq(x, 100);
        asserteq(y, 100);
        assertok(at_gravity(T, id[i]) == gv[i]);
    }
    for (i = 0; i < 4; ++i) assertok(at_delanchor(T, id[i]) == AT_OK);
    asserteq(at_bytes(T), 100);
    assertok(att_checktree(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_ns_tag(void) {
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
    assertok(att_cns(T, 0, 7, got, 2) == 1 && att_idset(got, 1, want, 1));
    assertok(att_cns(T, 0, 8, got, 2) == 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 8, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 7, &len) == id);
    asserteq(len, 0);
    assertok(att_checkchains(T));
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 0, 3) == AT_OK);
    assertok(at_range(T, id, &x, &y) == AT_OK);
    assertok(x == 13 && y == 13);
    assertok(att_cns(T, 0, 7, got, 2) == 1 && att_idset(got, 1, want, 1));
    assertok(att_cns(T, 0, 0, got, 2) == 1 && att_idset(got, 1, want, 1));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_ns_relocate(void) {
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
    assertok(att_cns(T, 0, 7, got, 2) == 1 && att_idset(got, 1, want, 1));
    want[0] = b;
    assertok(att_cns(T, 0, 9, got, 2) == 1 && att_idset(got, 1, want, 1));
    assertok(att_cns(T, 0, 8, got, 2) == 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 7, &len) == a);
    asserteq(len, 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 9, &len) == b);
    asserteq(len, 0);
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_range_positions(void) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               a, b, c;
    size_t              x, y;
    static const size_t expect[6] = {0, 5, 20, 23, 1000, 1010};
    att_anchor(T, 0, 5, AT_GROW, &a);
    att_anchor(T, 1000, 10, AT_FIXED, &b);
    att_anchor(T, 20, 3, AT_SHIFT, &c);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    asserteq(x, 0);
    asserteq(y, 5);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    asserteq(x, 1000);
    asserteq(y, 1010);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    asserteq(x, 20);
    asserteq(y, 23);
    assertok(att_positions_match(T, expect, 6));
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(at_delanchor(T, c) == AT_OK);
    asserteq(at_bytes(T), 1010);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_delete_cleans(void) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               a, b;
    size_t              x, y;
    static const size_t two[3] = {0, 20, 25};
    static const size_t four[5] = {0, 20, 25, 100, 110};
    att_anchor(T, 20, 5, AT_LOCK, &b);
    att_anchor(T, 100, 10, AT_GROW, &a);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    asserteq(x, 100);
    asserteq(y, 110);
    assertok(att_positions_match(T, four, 5));
    assertok(at_delanchor(T, a) == AT_OK);
    asserteq(at_bytes(T), 110);
    assertok(att_positions_match(T, two, 3));
    assertok(at_delanchor(T, b) == AT_OK);
    asserteq(at_bytes(T), 110);
    assertok(att_checktree(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_lifecycle_ids(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, reuse;
    size_t    x, y;
    assertok(S != NULL && T != NULL);
    att_anchor(T, 10, 5, AT_GROW, &a);
    att_anchor(T, 30, 7, AT_LOCK, &b);
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
    att_anchor(T, 40, 1, AT_FIXED, &reuse);
    assertok(at_range(T, reuse, &x, &y) == AT_OK);
    assertok(x == 40 && y == 41);
    assertok(at_range(T, reuse == a ? b : a, &x, &y) == AT_ERRPARAM);
    at_reset(T);
    assertok(at_range(T, reuse, &x, &y) == AT_ERRPARAM);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    att_anchor(T, 20, 4, AT_FIXED, &a);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 20 && y == 24);
    assertok(att_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void att_case_anchor_invalid_params(void) {
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
    att_anchor(T, 50, 5, AT_GROW, &id);
    assertok(at_delanchor(T, id) == AT_OK);
    assertok(at_delanchor(T, id) == AT_ERRPARAM);
    assertok(at_range(T, id, &x, &y) == AT_ERRPARAM);
    assertok(at_gravity(T, id) < 0);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_gap_delete_insert(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x, y;
    att_anchor(T, 20, 5, AT_GROW, &id);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 3, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkcursor(&C, 10));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 17);
    asserteq(y, 22);
    asserteq(at_bytes(T), 22);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 0, 4) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 21);
    asserteq(y, 26);
    asserteq(at_bytes(T), 26);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_edge_gravity_insert(void) {
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
        att_anchor(T, 100, 0, gv[i], &id);
        assertok(at_seek(&C, T, 100) == AT_OK);
        assertok(at_splice(&C, 0, 7) == AT_OK);
        assertok(att_checktree(T));
        assertok(att_no_empty_edges(T));
        assertok(att_checkcursor(&C, 100));
        assertok(at_gravity(T, id) == gv[i]);
        assertok(at_range(T, id, &x, &y) == AT_OK);
        asserteq(x, ex[i][0]);
        asserteq(y, ex[i][1]);
        asserteq(at_bytes(T), 107);
        at_deltree(T), at_close(S);
    }
}

TEST_STATIC void att_case_splice_delete_spanning(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c;
    size_t    x, y;
    att_anchor(T, 10, 5, AT_GROW, &a);
    att_anchor(T, 30, 5, AT_LOCK, &b);
    att_anchor(T, 50, 5, AT_FIXED, &c);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, 25, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkcursor(&C, 20));
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

TEST_STATIC void att_case_splice_delete_last_insert(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x, y;
    att_anchor(T, 100, 10, AT_GROW, &id);
    assertok(at_seek(&C, T, 80) == AT_OK);
    assertok(at_splice(&C, 40, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 80);
    asserteq(y, 80);
    asserteq(at_bytes(T), 80);
    assertok(at_seek(&C, T, 80) == AT_OK);
    assertok(at_splice(&C, 0, 6) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 80);
    asserteq(y, 86);
    asserteq(at_bytes(T), 86);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_zero_cross(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x, y;
    att_anchor(T, 100, 0, AT_FIXED, &id);
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_splice(&C, 0, 7) == AT_OK);
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 107);
    asserteq(y, 100);
    assertok(at_seek(&C, T, 105) == AT_OK);
    assertok(at_splice(&C, 0, 4) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(at_gravity(T, id) == AT_FIXED);
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 111);
    asserteq(y, 100);
    asserteq(at_bytes(T), 111);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_params(void) {
    at_State *S;
    at_Tree  *T;
    at_Cursor C;
    assertok(at_splice(NULL, 0, 0) == AT_ERRPARAM);
    S = at_open(NULL, NULL);
    T = at_newtree(S);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, 0) == AT_OK);
    assertok(att_checkcursor(&C, 0));
    assertok(att_setup_insert(T, 0, 7) == AT_OK);
    assertok(att_setup_insert(T, 5, 7) == AT_OK);
    assertok(att_checkchains(T));
    assertok(at_bytes(T) == 14);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_splice(&C, 5, 0) == AT_OK);
    assertok(att_checkcursor(&C, 100));
    assertok(att_checktree(T));
    asserteq(at_bytes(T), 14);
    at_deltree(T), at_close(S);
}

TEST_STATIC size_t att_poolcap(size_t obj_size) {
    return (AT_PAGE_SIZE - sizeof(void *) - obj_size) / obj_size + 1u;
}

TEST_STATIC size_t att_poolpages(size_t obj_size, size_t need) {
    size_t cap = att_poolcap(obj_size);
    return (need + cap - 1u) / cap;
}

/* setup-only: OOM injection budget derived from the current tree depth. */
TEST_STATIC size_t att_nodepages(const at_Tree *T, size_t passes) {
    size_t need = passes * (4u * (size_t)(T->levels + 2) + 8u);
    return att_poolpages(sizeof(at_Node), need);
}

TEST_STATIC void att_case_anchor_oom_idalloc(void) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    assertok(S != NULL && T != NULL);
    oc.oom = 0;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_newanchor(&C, 5, 0, AT_GROW, &id) == AT_ERRMEM);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    asserteq(at_bytes(T), 0);
    /* structural checker (white-box): OOM before id-pool growth leaves the
     * pool unallocated; not observable through the public API. */
    assertok(T->ids == NULL);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST_STATIC void att_oom_newanchor_setup(at_Tree *T, at_Id *keep) {
    at_Id ids[32];
    int   i;
    for (i = 0; i < 32; ++i) att_anchor(T, (size_t)i * 7u, 0, AT_GROW, &ids[i]);
    /* setup-only: require a multi-level tree so reserve OOM is meaningful. */
    assertok(T->levels >= 1);
    *keep = ids[31];
    assertok(at_delanchor(T, ids[0]) == AT_OK);
}

TEST_STATIC void att_oom_newanchor(int mode) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     keep, id;
    size_t    fail, bytes;
    assertok(S != NULL && T != NULL);
    att_oom_newanchor_setup(T, &keep);
    bytes = at_bytes(T);
    /* setup-only: force page allocations for deterministic fail-at-N. */
    (void)att_drainpool(&S->nodes), (void)att_drainpool(&S->edges);
    fail = mode == 2 ? att_nodepages(T, 2u) : (size_t)mode;
    oc.oom = (int)fail;
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_ERRMEM);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    asserteq(at_bytes(T), bytes);
    att_rangeeq(T, keep, 217, 217);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST_STATIC void att_case_anchor_oom_reserve(void) {
    att_oom_newanchor(0);
    att_oom_newanchor(1);
    att_oom_newanchor(2);
}

TEST_STATIC void att_oom_splice(int mode) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    fail, bytes;
    assertok(S != NULL && T != NULL);
    att_anchor(T, 10, 5, AT_GROW, &a);
    att_anchor(T, 30, 5, AT_LOCK, &b);
    bytes = at_bytes(T);
    /* setup-only: force page allocations for deterministic fail-at-N. */
    (void)att_drainpool(&S->nodes), (void)att_drainpool(&S->edges);
    fail = mode == 2 ? att_nodepages(T, 3u) : (size_t)mode;
    oc.oom = (int)fail;
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, 5, 1) == AT_ERRMEM);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    asserteq(at_bytes(T), bytes);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(att_checkcursor(&C, 20));
    att_rangeeq(T, a, 10, 15);
    att_rangeeq(T, b, 30, 35);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST_STATIC void att_case_splice_oom_reserve(void) {
    att_oom_splice(0);
    att_oom_splice(1);
    att_oom_splice(2);
}

TEST_STATIC void att_case_del_oom_zero_alloc(void) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    size_t    x, y;
    assertok(S != NULL && T != NULL);
    att_anchor(T, 30, 10, AT_GROW, &a);
    att_anchor(T, 100, 5, AT_GROW, &b);
    oc.oom = 0;
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    asserteq(at_bytes(T), 105);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    asserteq(x, 100);
    asserteq(y, 105);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST_STATIC void att_case_splice_noop_beyond_tail_oom(void) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C, D;
    at_Id     a, b;
    size_t    x, y, len;
    assertok(S != NULL && T != NULL);
    a = att_anew(T, 100, 0, AT_GROW);
    b = att_anew(T, 300, 0, AT_GROW);
    assertok(at_seek(&C, T, 500) == AT_OK);
    assertok(at_seek(&D, T, 0) == AT_OK);
    oc.oom = 0;
    assertok(at_splice(&C, 0, 0) == AT_OK);
    assertok(at_advance(&D, 0) == AT_OK);
    assertok(at_next(&D, 0, &len) == a);
    asserteq(len, 0);
    assertok(att_checkcursor(&D, 100));
    assertok(at_range(T, b, &x, &y) == AT_OK);
    asserteq(x, 300);
    asserteq(y, 300);
    asserteq(at_bytes(T), 300);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST_STATIC void att_case_anchor_tail_delete(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c;
    size_t    x, y;
    att_anchor(T, 100, 0, AT_GROW, &a);
    att_anchor(T, 100, 0, AT_GROW, &b);
    att_anchor(T, 100, 0, AT_GROW, &c);
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_delanchor(T, c) == AT_OK);
    assertok(at_bytes(T) == 100);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_tail_edge_contract(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, got[2], want[2];
    size_t    x, y, len;
    att_anchor(T, 100, 0, AT_SHIFT, &a);
    att_anchor(T, 100, 0, AT_SHIFT, &b);
    want[0] = a, want[1] = b;
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(att_cns(T, 100, 0, got, 2) == 2);
    assertok(att_idset(got, 2, want, 2));
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_next(&C, 0, &len) != AT_NONE);
    asserteq(len, 0);
    assertok(att_checkchains(T));
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_range(T, a, &x, &y) == AT_ERRPARAM);
    assertok(att_cns(T, 0, 0, got, 2) == 1 && got[0] == b);
    assertok(att_checkchains(T));
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(att_cns(T, 0, 0, got, 2) == 0);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    asserteq(at_bytes(T), 100);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_delete_grow_tail(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     ids[10];
    int       i;
    for (i = 0; i < 8; ++i) att_anchor(T, 10, 0, AT_GROW, &ids[i]);
    for (i = 8; i < 10; ++i) att_anchor(T, 20, 0, AT_GROW, &ids[i]);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 10, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(at_bytes(T) == 10);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_delete_contracts(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c;
    att_anchor(T, 20, 5, AT_GROW, &a);
    att_anchor(T, 50, 5, AT_FIXED, &b);
    att_anchor(T, 80, 5, AT_SHIFT, &c);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 70, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    att_rangeeq(T, c, 10, 15);
    assertok(at_splice(&C, 0, 7) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    asserteq(at_bytes(T), 0);
    at_deltree(T), at_close(S);
}

/* checker self-test: an overfull root must be rejected. */
TEST_STATIC void att_case_checktree_overfull(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Index  old = T->root.child_count;
    T->root.child_count = (at_Index)(AT_FANOUT + 1);
    assertok(att_checktree(T) == 0);
    T->root.child_count = old;
    assertok(att_checktree(T));
    at_deltree(T), at_close(S);
}

/* checker self-test helpers: mutate one internal link/metric at a time. */
TEST_STATIC void att_badleaf(at_Tree *T, at_Node *in, at_Node *lf, at_Edge *e) {
    at_Node *oldparent;
    at_Index oldidx;
    size_t   oldbytes;
    oldbytes = lf->bytes[0];
    lf->bytes[0] = 0;
    assertok(att_checktree(T) == 0);
    lf->bytes[0] = oldbytes;
    oldparent = e->parent;
    e->parent = NULL;
    assertok(att_checktree(T) == 0);
    e->parent = oldparent;
    oldidx = e->idx;
    e->idx = (at_Index)(e->idx == 0 ? 1 : 0);
    assertok(att_checktree(T) == 0);
    e->idx = oldidx;
    oldbytes = in->bytes[0];
    in->bytes[0] = oldbytes + 1;
    assertok(att_checktree(T) == 0);
    in->bytes[0] = oldbytes;
}

TEST_STATIC void att_badnode(at_Tree *T, at_Node *lf) {
    at_Node *oldparent;
    at_Index oldidx;
    oldparent = lf->parent;
    lf->parent = NULL;
    assertok(att_checktree(T) == 0);
    lf->parent = oldparent;
    oldidx = lf->idx;
    lf->idx = (at_Index)(lf->idx == 0 ? 1 : 0);
    assertok(att_checktree(T) == 0);
    lf->idx = oldidx;
}

/* checker self-test: deliberate destructive mutations are restored. */
TEST_STATIC void att_case_checktree_bad_links(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Node  *internal, *leaf;
    at_Edge  *e, *edge;
    int       i, oldlevels;
    assertok(att_checktree(NULL) == 0);
    for (i = 1; i <= 128 && T->levels < 2; ++i)
        assertok(att_setup_edge(T, (size_t)(i * 10), &e) == AT_OK);
    assertok(T->levels >= 2);
    internal = (at_Node *)T->root.children[0];
    leaf = (at_Node *)internal->children[0];
    assertok(atN_cc(internal) > 0 && atN_cc(leaf) > 0);
    edge = atN_edge(leaf, 0);
    att_badleaf(T, internal, leaf, edge);
    att_badnode(T, leaf);
    oldlevels = T->levels;
    T->levels = AT_MAX_LEVEL;
    assertok(att_checktree(T) == 0);
    T->levels = AT_MAX_LEVEL + 1;
    assertok(att_checktree(T) == 0);
    T->levels = oldlevels;
    T->bytes += 1;
    assertok(att_checktree(T) == 0);
    T->bytes -= 1;
    assertok(att_checktree(T));
    at_deltree(T), at_close(S);
}

/* checker self-test: 600 anchors exceed the draft 512-slot sequence cap. */
TEST_STATIC void att_case_checker_seq_capacity(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     id;
    int       i;
    for (i = 0; i < 600; ++i)
        att_anchor(T, (size_t)i * 3u + 1u, 0, AT_GROW, &id);
    assertok(at_bytes(T) > 0);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_insert_edge_last(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a;
    size_t    x, y;
    att_anchor(T, 100, 0, AT_GROW, &a);
    assertok(att_setup_insert(T, 100, 7) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    asserteq(at_bytes(T), 107);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 107);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_insert_edge_mid(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    size_t    x, y;
    att_anchor(T, 100, 0, AT_GROW, &a);
    att_anchor(T, 200, 0, AT_GROW, &b);
    assertok(att_setup_insert(T, 100, 7) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    asserteq(at_bytes(T), 207);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 107);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 207 && y == 207);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_insert_gap_shift_mid(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    size_t    x, y;
    att_anchor(T, 100, 0, AT_GROW, &a);
    att_anchor(T, 200, 0, AT_GROW, &b);
    assertok(att_setup_insert(T, 150, 7) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    asserteq(at_bytes(T), 207);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 207 && y == 207);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_delete_prefix_leaf_rebalance(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    int       i;
    for (i = 0; i < 50; ++i) {
        assertok(at_seek(&C, T, (size_t)((i + 1) * 20)) == AT_OK);
        assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_OK);
    }
    assertok(att_checktree(T));
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 20, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    at_deltree(T), at_close(S);
}

/* checker self-test: att_sideof reports ids absent from both buckets. */
TEST_STATIC void att_case_edge_side_missing(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    at_Id     a;
    size_t    x, y;
    att_anchor(T, 100, 0, AT_SHIFT, &a);
    assertok(att_setup_findedge(T, 100, &e));
    assertok(att_sideof(T, e, 1000, 0) == -1);
    assertok(att_sideof(T, e, 1000, 1) == -1);
    assertok(att_checktree(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    at_deltree(T), at_close(S);
}

/* structural checker: root collapse is internal; the public range/bytes
 * contract is covered by att_checktree below. */
TEST_STATIC void att_case_delete_root_collapse(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     id[40];
    size_t    prev;
    int       i, shrunk = 0;
    for (i = 0; i < 40; ++i)
        att_anchor(T, (size_t)(10 + i * 7), 0, AT_GROW, &id[i]);
    assertok(att_checktree(T));
    assertok(T->levels >= 1);
    prev = (size_t)T->levels;
    for (i = 0; i < 40; ++i) {
        assertok(at_delanchor(T, id[i]) == AT_OK);
        assertok(att_checktree(T));
        if ((size_t)T->levels < prev && at_bytes(T) > 0) shrunk = 1;
        prev = (size_t)T->levels;
    }
    assertok(shrunk);
    assertok(T->levels == 0 && atN_cc(&T->root) == 1 && at_bytes(T) == 283);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_multi_slot_delete(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[10];
    size_t    x, y;
    int       i;
    for (i = 0; i < 10; ++i)
        att_anchor(T, (size_t)(10 + i * 4), 1, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 12) == AT_OK);
    assertok(at_splice(&C, 16, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    for (i = 0; i < 10; ++i) {
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        assertok(y >= x);
    }
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_cross_leaf_rebalance(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[40];
    int       i;
    for (i = 0; i < 40; ++i)
        att_anchor(T, (size_t)(10 + i * 7), 2, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, 180, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_empty_L_merge(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    att_anchor(T, 50, 5, AT_GROW, &a);
    att_anchor(T, 70, 5, AT_LOCK, &b);
    assertok(at_seek(&C, T, 55) == AT_OK);
    assertok(at_splice(&C, 10, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 50 && y == 55);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 60 && y == 65);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_delete_cross_leaf_merge(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[64];
    int       i;
    for (i = 0; i < 40; ++i)
        att_anchor(T, (size_t)(10 + i * 7), 0, AT_GROW, &id[i]);
    /* setup-only: require a multi-level tree for the crossing merge. */
    assertok(T->levels >= 1);
    assertok(at_seek(&C, T, 12) == AT_OK);
    assertok(at_splice(&C, 120, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkchains(T));
    assertok(att_checkcursor(&C, 12));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_delete_tail_sentinel(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[20];
    int       i;
    for (i = 0; i < 20; ++i)
        att_anchor(T, (size_t)(10 + i * 9), 3, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_delete_shared_tail(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    att_Model m[AT_BRUTE_N];
    size_t    x, y;
    int       i;
    att_brute_build(T, m);
    assertok(at_seek(&C, T, 95) == AT_OK);
    assertok(at_splice(&C, 5, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkcursor(&C, 95));
    for (i = 9; i < AT_BRUTE_N; ++i) {
        assertok(at_range(T, m[i].id, &x, &y) == AT_OK);
        assertok(x == 95 && y == 95);
    }
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_delete_chain(void) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Cursor           C;
    at_Id               id[10];
    size_t              x, y;
    static const size_t ep[6] = {0, 10, 15, 25, 35, 45};
    static const size_t ex[10] = {10, 15, 15, 15, 15, 15, 15, 25, 35, 45};
    int                 i;
    for (i = 0; i < 10; ++i)
        att_anchor(T, (size_t)(10 + i * 10), 0, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 15) == AT_OK);
    assertok(at_splice(&C, 55, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkcursor(&C, 15));
    asserteq(at_bytes(T), 45);
    assertok(att_positions_match(T, ep, 6));
    for (i = 0; i < 10; ++i) {
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        asserteq(x, ex[i]);
        asserteq(y, ex[i]);
    }
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_delete_tail(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[3];
    int       i;
    for (i = 0; i < 3; ++i) att_anchor(T, (size_t)(i * 2), 2, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 2) == AT_OK);
    assertok(at_splice(&C, 4, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkcursor(&C, 2));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_shift_negative(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    att_anchor(T, 100, 0, AT_GROW, &a);
    att_anchor(T, 200, 0, AT_GROW, &b);
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_splice(&C, 7, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 93 && y == 93);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 193 && y == 193);
    asserteq(at_bytes(T), 193);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_multiple_edges(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c, d, got[4];
    att_anchor(T, 10, 10, AT_GROW, &a);
    att_anchor(T, 10, 0, AT_FIXED, &b);
    att_anchor(T, 20, 10, AT_SHIFT, &c);
    att_anchor(T, 30, 0, AT_LOCK, &d);
    assertok(att_collect(T, 0, got, 4) == 4);
    assertok(got[0] == a && got[1] == b && got[2] == c && got[3] == d);
    assertok(att_checktree(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_start_first(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, got[2];
    att_anchor(T, 0, 5, AT_GROW, &a);
    att_anchor(T, 5, 0, AT_LOCK, &b);
    assertok(att_collect(T, 0, got, 2) == 2);
    assertok(got[0] == a && got[1] == b);
    assertok(att_collect(T, 1, got, 2) == 1);
    assertok(got[0] == b);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_start_empty(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, got[1];
    att_anchor(T, 0, 0, AT_GROW, &a);
    assertok(at_bytes(T) == 0);
    assertok(att_collect(T, 0, got, 1) == 1);
    assertok(got[0] == a);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_many_edges_cross_leaves(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     id[60], got[60];
    int       i, n;
    for (i = 0; i < 60; ++i)
        att_anchor(T, (size_t)(1 + i * 3), 0, AT_GROW, &id[i]);
    n = att_collect(T, 0, got, 60);
    assertok(n == 60);
    for (i = 0; i < 60; ++i) assertok(got[i] == id[i]);
    n = att_collect(T, 61, got, 60);
    assertok(n == 40);
    for (i = 0; i < 40; ++i) assertok(got[i] == id[20 + i]);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_zero_length_once(void) {
    at_State        *S = at_open(NULL, NULL);
    at_Tree         *T = at_newtree(S);
    static const int gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    at_Id            id[4], got[4];
    size_t           x, y;
    int              i, j, n;
    for (i = 0; i < 4; ++i) att_anchor(T, 100, 0, gv[i], &id[i]);
    n = att_collect(T, 0, got, 4);
    assertok(n == 4);
    for (i = 0; i < 4; ++i) {
        int c = 0;
        for (j = 0; j < n; ++j)
            if (got[j] == id[i]) c += 1;
        assertok(c == 1);
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        assertok(x == 100 && y == 100);
    }
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_gap_and_edge_starts(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, got[2];
    att_anchor(T, 30, 5, AT_GROW, &a);
    att_anchor(T, 50, 5, AT_LOCK, &b);
    assertok(att_collect(T, 40, got, 2) == 1);
    assertok(got[0] == b);
    assertok(att_collect(T, 32, got, 2) == 1);
    assertok(got[0] == b);
    assertok(att_collect(T, 30, got, 2) == 2);
    assertok(got[0] == a && got[1] == b);
    assertok(att_collect(T, 50, got, 2) == 1);
    assertok(got[0] == b);
    assertok(att_collect(T, 52, got, 2) == 0);
    assertok(att_collect(T, 55, got, 2) == 0);
    assertok(att_collect(T, 60, got, 2) == 0);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_empty_end_idempotent(void) {
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
    assertok(att_setup_insert(T, 0, 10) == AT_OK);
    assertok(at_seek(&C, T, 5) == AT_OK);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    at_reset(T);
    assertok(at_seek(&C, T, 0) == AT_OK);
    att_anchor(T, 10, 0, AT_GROW, &a);
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

TEST_STATIC void att_nsfilter(at_Tree *T, int ns, const at_Id *want, int m) {
    at_Id got[8];
    int   n = att_cns(T, 0, ns, got, 8);
    assertok(att_idset(got, n, want, m));
}

TEST_STATIC void att_case_iter_ns_filter(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     want0[8], want1[3], want2[3], want3[2];
    att_mkns(T, 0, 0, 1, &want0[0]);
    att_mkns(T, 0, 0, 2, &want0[1]);
    att_mkns(T, 0, 30, 1, &want0[2]);
    att_mkns(T, 0, 0, 3, &want0[3]);
    att_mkns(T, 10, 0, 2, &want0[4]);
    att_mkns(T, 10, 0, 1, &want0[5]);
    att_mkns(T, 10, 10, 2, &want0[6]);
    att_mkns(T, 10, 0, 3, &want0[7]);
    want1[0] = want0[0], want1[1] = want0[2], want1[2] = want0[5];
    want2[0] = want0[1], want2[1] = want0[4], want2[2] = want0[6];
    want3[0] = want0[3], want3[1] = want0[7];
    att_nsfilter(T, 0, want0, 8);
    att_nsfilter(T, 1, want1, 3);
    att_nsfilter(T, 2, want2, 3);
    att_nsfilter(T, 3, want3, 2);
    att_nsfilter(T, 99, want0, 0);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_plen(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c;
    size_t    len;
    att_anchor(T, 0, 5, AT_GROW, &a);
    att_anchor(T, 10, 0, AT_GROW, &b);
    att_anchor(T, 50, 0, AT_FIXED, &c);
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

TEST_STATIC void att_case_iter_params(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C, D;
    at_Id     a;
    size_t    len = 99;
    memset(&D, 0, sizeof(D));
    att_anchor(T, 0, 3, AT_GROW, &a);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(NULL, 0, &len) == AT_NONE);
    asserteq(len, 0);
    len = 99;
    assertok(at_next(&D, 0, &len) == AT_NONE);
    asserteq(len, 0);
    len = 99;
    assertok(at_next(&C, -1, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_next(&C, 0, NULL) == a);
    len = 99;
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_exhaust_restart(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    len = 99;
    att_mkns(T, 0, 0, 1, &a);
    att_mkns(T, 10, 0, 1, &b);
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

TEST_STATIC void att_case_iter_exhaust_commit_span(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, 10) == AT_OK);
    asserteq(at_bytes(T), 10);
    assertok(at_seek(&C, T, 5) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    assertok(at_offset(&C) == 5);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    assertok(at_offset(&C) == 5);
    assertok(at_splice(&C, 1, 0) == AT_OK);
    asserteq(at_bytes(T), 9);
    assertok(att_checkcursor(&C, 5));
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_exhaust_commit_boundary(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    bytes, len = 99;
    a = att_anew(T, 0, 10, AT_GROW);
    b = att_anew(T, 20, 5, AT_GROW);
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
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_exhaust_commit(void) {
    att_case_iter_exhaust_commit_span();
    att_case_iter_exhaust_commit_boundary();
}

TEST_STATIC void att_case_splice_after_iter_exhaust(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a;
    size_t    len = 99;
    att_anchor(T, 10, 0, AT_LOCK, &a);
    assertok(at_seek(&C, T, 0) == AT_OK);
    asserteq(at_next(&C, 0, &len), a);
    asserteq(len, 0);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_splice(&C, 0, 5) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_checkcursor(&C, 10));
    asserteq(at_next(&C, 0, &len), a);
    asserteq(len, 0);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_empty_start(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    len = 99;
    att_anchor(T, 0, 0, AT_GROW, &a);
    att_anchor(T, 0, 0, AT_GROW, &b);
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

TEST_STATIC void att_case_iter_after_splice_insert(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c, got[3];
    size_t    x, y;
    att_anchor(T, 0, 5, AT_GROW, &c);
    att_anchor(T, 10, 10, AT_GROW, &a);
    att_anchor(T, 20, 10, AT_LOCK, &b);
    assertok(at_seek(&C, T, 15) == AT_OK);
    assertok(at_splice(&C, 0, 5) == AT_OK);
    assertok(att_collect(T, 0, got, 3) == 3);
    assertok(got[0] == c && got[1] == a && got[2] == b);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 10 && y == 25);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 25 && y == 35);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_iter_after_splice_delete(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c, got[3];
    size_t    x, y;
    att_anchor(T, 10, 5, AT_GROW, &a);
    att_anchor(T, 30, 5, AT_LOCK, &b);
    att_anchor(T, 50, 5, AT_FIXED, &c);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, 25, 0) == AT_OK);
    assertok(att_collect(T, 0, got, 3) == 3);
    assertok(got[0] == a && got[1] == b && got[2] == c);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 20 && y == 20);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 25 && y == 30);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_overflow_del(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    att_anchor(T, 10, 5, AT_GROW, &a);
    att_anchor(T, 40, 5, AT_GROW, &b);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkcursor(&C, 20));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 10 && y == 15);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 20 && y == 20);
    asserteq(at_bytes(T), 20);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_delete_large_span(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[100];
    size_t    x, y;
    int       i;
    for (i = 0; i < 100; ++i)
        att_anchor(T, (size_t)(10 + i * 7), (size_t)(i % 5), 2, &id[i]);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkcursor(&C, 10));
    for (i = 0; i < 100; ++i) assertok(at_range(T, id[i], &x, &y) == AT_OK);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_delete_all(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    size_t    len;
    assertok(att_setup_insert(T, 0, 100) == AT_OK);
    assertok(att_setup_insert(T, 100, 50) == AT_OK);
    asserteq(at_bytes(T), 150);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, at_bytes(T), 0) == AT_OK);
    asserteq(at_bytes(T), 0);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_checkcursor(&C, 0));
    asserteq(at_next(&C, 0, &len), AT_NONE);
    asserteq(len, 0);
    assertok(at_splice(&C, 0, 5) == AT_OK);
    asserteq(at_bytes(T), 5);
    assertok(att_checktree(T));
    assertok(att_checkcursor(&C, 0));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_remove_missing(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c;
    size_t    x, y;
    att_anchor(T, 100, 10, AT_GROW, &a);
    att_anchor(T, 100, 10, AT_GROW, &b);
    att_anchor(T, 300, 10, AT_GROW, &c);
    assertok(at_delanchor(T, b) == AT_OK);
    assertok(at_delanchor(T, b) == AT_ERRPARAM);
    assertok(at_delanchor(T, 9999) == AT_ERRPARAM);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 110);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 300 && y == 310);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_anchor_at_zero_delete(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a;
    size_t    x, y, len;
    att_anchor(T, 0, 0, AT_GROW, &a);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 0 && y == 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, &len) == a);
    asserteq(len, 0);
    assertok(at_delanchor(T, a) == AT_OK);
    assertok(at_range(T, a, &x, &y) == AT_ERRPARAM);
    assertok(att_checktree(T));
    asserteq(at_bytes(T), 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_insert_empty_boundary(void) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               a;
    size_t              x, y;
    static const size_t p[1] = {0};
    assertok(att_setup_insert(T, 0, 10) == AT_OK);
    assertok(att_setup_insert(T, 10, 7) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_no_empty_edges(T));
    assertok(att_positions_match(T, p, 1));
    asserteq(at_bytes(T), 17);
    att_anchor(T, 17, 0, AT_GROW, &a);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 17 && y == 17);
    asserteq(at_bytes(T), 17);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_deleteedge_pos_guard(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    assertok(att_setup_edge(T, 10, &e) == AT_OK);
    assertok(att_setup_dropedge(T, 20) == AT_ERRPARAM);
    assertok(att_setup_dropedge(T, 5) == AT_ERRPARAM);
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_splice_ins_overflow(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    assertok(att_setup_edge(T, 10, &e) == AT_OK);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, (size_t)-1) == AT_ERRPARAM);
    assertok(at_bytes(T) == 10 && att_checktree(T));
    assertok(at_advance(&C, (at_Delta)AT_DELTA_MAX) == AT_OK);
    assertok(at_advance(&C, 1) == AT_OK);
    assertok(at_splice(&C, 0, 1) == AT_ERRPARAM);
    assertok(at_bytes(T) == 10 && att_checktree(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_advance_overflow_guard(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    at_Id     id;
    assertok(att_setup_insert(T, 0, 10) == AT_OK);
    assertok(att_setup_edge(T, 10, &e) == AT_OK);
    assertok(att_setup_insert(T, 10, 10) == AT_OK);
    asserteq(at_bytes(T), 20);
    assertok(at_seek(&C, T, (size_t)-1) == AT_OK);
    assertok(at_advance(&C, 1) == AT_OK);
    assertok(at_offset(&C) == (size_t)-1);
    assertok(at_splice(&C, 0, 1) == AT_ERRPARAM);
    assertok(at_newanchor(&C, 1, 0, AT_GROW, &id) == AT_ERRPARAM);
    asserteq(at_bytes(T), 20);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(at_locate(&C, 0) == AT_OK);
    assertok(at_advance(&C, 1) == AT_OK);
    assertok(att_checkcursor(&C, 1));
    at_deltree(T), at_close(S);
}

typedef struct {
    size_t pos[AT_SEQ_CAP];
    size_t span[AT_SEQ_CAP];
    int    n;
} att_EpochSnap;

TEST_STATIC void att_snapshot(at_Tree *T, att_EpochSnap *s) {
    at_Seq q;
    int    i;
    att_seq(T, &q);
    assert(q.n <= AT_SEQ_CAP);
    s->n = q.n;
    for (i = 0; i < q.n; ++i) s->pos[i] = q.pos[i], s->span[i] = q.span[i];
}

TEST_STATIC int att_snapeq(const att_EpochSnap *a, const att_EpochSnap *b) {
    int i;
    if (a->n != b->n) return 0;
    for (i = 0; i < a->n; ++i)
        if (a->pos[i] != b->pos[i] || a->span[i] != b->span[i]) return 0;
    return 1;
}

TEST_STATIC void att_case_epoch_advance_stale(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    assertok(S != NULL && T != NULL);
    a = att_anew(T, 10, 5, AT_GROW);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &b) == AT_OK);
    assertok(at_advance(&C, 0) == AT_ESTALE);
    assertok(at_advance(&C, 1) == AT_ESTALE);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &b) == AT_ESTALE);
    assertok(at_locate(&C, 10) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(att_checkcursor(&C, 10));
    att_rangeeq(T, a, 10, 15), att_rangeeq(T, b, 10, 10);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void att_epoch_next_scan(at_Tree *T, const at_Id *ids, at_Id ex) {
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

TEST_STATIC void att_case_epoch_next_stale(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     ids[64], extra;
    size_t    len;
    int       i;
    assertok(S != NULL && T != NULL);
    for (i = 0; i < 64; ++i) ids[i] = att_anew(T, (size_t)i * 3u, 0, AT_GROW);
    assertok(at_seek(&C, T, 0) == AT_OK);
    extra = att_anew(T, 300, 0, AT_GROW);
    for (i = 0; i < 40; ++i) assertok(at_delanchor(T, ids[i]) == AT_OK);
    len = 99;
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    len = 99;
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(&C, 0) == AT_ESTALE);
    att_epoch_next_scan(T, ids, extra);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void att_epoch_splice_check(at_Tree *T, at_Id a, at_Id b) {
    at_Cursor C;
    size_t    len;
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 0, 5) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(att_checkcursor(&C, 10));
    assertok(at_next(&C, 0, &len) == a);
    asserteq(len, 5);
    assertok(at_advance(&C, 10) == AT_OK);
    assertok(att_checkcursor(&C, 20));
    att_rangeeq(T, a, 10, 15), att_rangeeq(T, b, 25, 25);
}

TEST_STATIC void att_case_epoch_splice_keeps_own(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    len;
    assertok(S != NULL && T != NULL);
    a = att_anew(T, 10, 0, AT_GROW);
    b = att_anew(T, 20, 0, AT_GROW);
    att_epoch_splice_check(T, a, b);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 5, 0) == AT_OK);
    assertok(at_advance(&C, 0) == AT_OK);
    assertok(att_checkcursor(&C, 10));
    assertok(at_next(&C, 0, &len) == a);
    asserteq(len, 0);
    att_rangeeq(T, b, 20, 20);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void att_epochchecks(at_Tree *T, at_Id a, at_Id b, at_Cursor *c) {
    att_EpochSnap before, after;
    size_t        len;
    len = 99;
    assertok(at_next(c, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(c, 0) == AT_ESTALE);
    att_snapshot(T, &before);
    assertok(at_splice(c, 1, 1) == AT_ESTALE);
    att_snapshot(T, &after);
    assertok(att_snapeq(&before, &after));
    att_rangeeq(T, a, 10, 15), att_rangeeq(T, b, 25, 25);
    assertok(at_locate(c, 20) == AT_OK);
    assertok(at_advance(c, 0) == AT_OK);
    assertok(att_checkcursor(c, 20));
}

TEST_STATIC void att_case_epoch_multi_cursor(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C1, C2;
    at_Id     a, b;
    assertok(S != NULL && T != NULL);
    a = att_anew(T, 10, 0, AT_GROW);
    b = att_anew(T, 20, 0, AT_GROW);
    assertok(at_seek(&C1, T, 10) == AT_OK);
    assertok(at_seek(&C2, T, 20) == AT_OK);
    assertok(at_splice(&C1, 0, 5) == AT_OK);
    assertok(at_advance(&C1, 0) == AT_OK);
    assertok(at_next(&C1, 0, NULL) == a);
    att_epochchecks(T, a, b, &C2);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST_STATIC void att_epoch_nomut_queries(at_Tree *T, at_Id a) {
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
    assertok(att_checkcursor(&C, 10));
    assertok(at_locate(&C, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
}

TEST_STATIC void att_epoch_nomut_oom(void) {
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
    assertok(att_checkcursor(&C, 10));
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

TEST_STATIC void att_case_epoch_no_mutation(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Id     a;
    assertok(S != NULL && T != NULL);
    a = att_anew(T, 10, 5, AT_GROW);
    att_epoch_nomut_queries(T, a);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
    att_epoch_nomut_oom();
}

TEST_STATIC void att_case_epoch_reset(void) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    size_t    len;
    assertok(S != NULL && T != NULL);
    att_anew(T, 10, 5, AT_GROW);
    att_anew(T, 30, 5, AT_GROW);
    assertok(at_seek(&C, T, 10) == AT_OK);
    at_reset(T);
    len = 99;
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    assertok(at_advance(&C, 0) == AT_ESTALE);
    assertok(at_splice(&C, 0, 1) == AT_ESTALE);
    assertok(at_locate(&C, 0) == AT_OK);
    assertok(at_advance(&C, 5) == AT_OK);
    assertok(att_checkcursor(&C, 5));
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

/* setup-only: force a child split on the next insert; reads internals
 * only to choose the already-full child edge. */
TEST_STATIC at_Edge *att_splitchild_edge(at_Tree *T, at_Id *pid) {
    at_Node *last = NULL;
    at_Edge *e = NULL;
    size_t   pos = 10;
    int      guard, i;
    for (guard = 0; guard < 2048; ++guard) {
        att_anchor(T, pos, 0, AT_GROW, pid);
        last = att_lastleaf(T);
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

TEST_STATIC void att_case_insert_split_child(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    at_Id     id = 0;
    size_t    pos;
    e = att_splitchild_edge(T, &id);
    pos = att_setup_edgepos(e);
    assertok(at_seek(&C, T, pos) == AT_OK);
    assertok(at_splice(&C, 0, 7) == AT_OK);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    at_deltree(T), at_close(S);
}

TEST_STATIC void att_case_insert_span_split(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    size_t    x, y;
    assertok(att_setup_insert(T, 0, 100) == AT_OK);
    att_anchor(T, 50, 0, AT_FIXED, &a);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 50 && y == 50);
    att_anchor(T, 75, 10, AT_GROW, &b);
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 75 && y == 85);
    assertok(at_bytes(T) == 100);
    at_deltree(T), at_close(S);
}

#endif /* AT_CASES_H */
