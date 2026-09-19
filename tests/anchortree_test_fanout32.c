/* anchortree_test_fanout32.c -- default-config anchortree runner.
 *
 * Deliberately defines no AT_* knobs: it exercises the library defaults
 * (AT_FANOUT=32, AT_MAX_LEVEL=16, AT_PAGE_SIZE=65536) that the reduced
 * fanout-4/5/7/8/9 runners never reach.  It uses only the public API plus
 * the shared att_* model/checkers from at_tests.h; structural depth
 * assertions go through the shared structural checker helper att_checkdepth.
 * at_cases.h is included for the fanout-independent shared M1 regression
 * iter_exhaust_commit; the other shared scenarios are not registered because
 * several of them force fanout-4/5 layouts.
 */
#define AT_STATIC_API

#include "../anchortree.h"
#include "at_cases.h"
#include "at_tests.h"
#include "tests.h"

/* shared M1 regression: at_next exhaust commit semantics at default fanout. */
TEST(iter_exhaust_commit) { att_case_iter_exhaust_commit(); }

/* fanout-independent API and stitch regressions at the default fanout. */
TEST(remove_params) { att_case_remove_params(); }
TEST(remove_cursor) { att_case_remove_cursor(); }
TEST(insert_cursor) { att_case_insert_cursor(); }
TEST(append_cursor) { att_case_append_cursor(); }
TEST(splice_atomic_oom) { att_case_splice_atomic_oom(); }
TEST(remove_append_equivalence) { att_case_remove_append_equivalence(); }
TEST(stitch_delete_cross_leaf) { att_case_stitch_delete_cross_leaf(); }
TEST(stitch_delete_tail) { att_case_stitch_delete_tail(); }
TEST(stitch_delete_same_leaf) { att_case_stitch_delete_same_leaf(); }
TEST(stitch_seam_cleared) { att_case_stitch_seam_cleared(); }
TEST(stitch_rt_cleared) { att_case_stitch_rt_cleared(); }
TEST(stitch_firstedge_keep) { att_case_stitch_firstedge_keep(); }
TEST(stitch_tail_seam_anchors) { att_case_stitch_tail_seam_anchors(); }
TEST(purge_insertion) { att_case_purge_insertion(); }
TEST(purge_delanchor) { att_case_purge_delanchor(); }
TEST(anchor_query) { att_case_anchor_query(); }

/* 640 anchors at unique increasing positions leave ~640 edges, above the
 * 512-edge minimum occupancy that forces a second internal level at the
 * default fanout (2 * AT_FANOUT/2 * AT_FANOUT/2). */
#define AT32_ANCHORS 640

TEST_STATIC void att32_build(at_Tree *T, att_Model *m, int n) {
    at_Cursor C;
    int       i;
    for (i = 0; i < n; ++i) {
        assertok(at_seek(&C, T, (size_t)i * 3u + 1u) == AT_OK);
        m[i].gravity = i % 4;
        assertok(at_newanchor(&C, 0, 0, m[i].gravity, &m[i].id) == AT_OK);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
    }
}

/* one model-checked splice: update the model, run the public mutation and
 * then check structure, chains, cursor and every model range */
TEST_STATIC void att32_splice(at_Tree *T, att_Model *m, int n, att_Splice *w) {
    at_Cursor C;
    att_model_splice(m, n, w);
    assertok(at_seek(&C, T, w->L) == AT_OK);
    assertok(at_splice(&C, w->del, w->ins) == AT_OK);
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    assertok(att_checkcursor(&C, w->L + w->ins));
    assertok(att_model_ok(T, m, n));
}

TEST(fanout32_model_stress) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    att_Model  m[AT32_ANCHORS];
    att_Splice w;
    size_t     seed = 0x9e3779b9u;
    int        step, deep = 0;
    assertok(S != NULL && T != NULL);
    att32_build(T, m, AT32_ANCHORS);
    assertok(att_checkdepth(T, 2));
    for (step = 0; step < 400; ++step) {
        w.L = att_lcg(&seed) % (att_model_bound(m, AT32_ANCHORS) + 20u);
        w.del = att_lcg(&seed) % 25u, w.ins = att_lcg(&seed) % 10u;
        att32_splice(T, m, AT32_ANCHORS, &w);
        if (!deep) deep = att_checkdepth(T, 2);
    }
    assertok(deep);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(fanout32_deep_delete_insert) {
    Count      c = {0};
    at_State  *S = at_open(&count_alloc, &c);
    at_Tree   *T = at_newtree(S);
    att_Model  m[AT32_ANCHORS + 4];
    att_Splice w;
    size_t     total;
    int        i, k, n = AT32_ANCHORS;
    assertok(S != NULL && T != NULL);
    att32_build(T, m, n);
    assertok(att_checkdepth(T, 2));
    /* delete a middle span crossing many leaves; small enough that the
     * remaining anchor-pinned edges keep the default-fanout tree deep */
    total = at_bytes(T);
    w.L = total / 3u, w.del = total / 10u, w.ins = 0;
    att32_splice(T, m, n, &w);
    assertok(att_checkdepth(T, 2));
    /* insert at four deep positions */
    for (i = 0; i < 4; ++i) {
        w.L = (at_bytes(T) / 5u) * (size_t)i + 1u;
        w.del = 0, w.ins = (size_t)i * 7u + 3u;
        att32_splice(T, m, n, &w);
    }
    assertok(att_checkdepth(T, 2));
    /* delete five spread anchors, then re-add four at deep positions */
    for (i = 0; i < 5; ++i) {
        k = (i + 1) * n / 6;
        assertok(at_delanchor(T, m[k].id) == AT_OK);
        m[k] = m[n - 1], n -= 1;
        assertok(att_checkchains(T));
        assertok(att_no_empty_edges(T));
        assertok(att_model_ok(T, m, n));
    }
    for (i = 0; i < 4; ++i) {
        at_Cursor C;
        size_t    pos = (at_bytes(T) / 4u) * (size_t)i + 1u;
        assertok(at_seek(&C, T, pos) == AT_OK);
        m[n].gravity = i % 4;
        assertok(at_newanchor(&C, 0, 0, m[n].gravity, &m[n].id) == AT_OK);
        assertok(at_range(T, m[n].id, &m[n].start, &m[n].end) == AT_OK);
        n += 1;
    }
    assertok(att_checkchains(T));
    assertok(att_no_empty_edges(T));
    assertok(att_model_ok(T, m, n));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

#include "anchortree_test_fanout32.gen.inc"
