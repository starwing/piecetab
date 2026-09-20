/* anchortree_test_fanout32.c -- default-config anchortree runner.
 *
 * Deliberately defines no AT_* knobs: it exercises the library defaults
 * (AT_FANOUT=32, AT_MAX_LEVEL=16, AT_PAGE_SIZE=65536) with stress scenarios
 * whose depth/layout depends on the default fanout.  It uses only the public
 * API plus the shared atT_/at_ model/checkers from at_tests.h; structural
 * depth assertions go through the shared checker helper atT_checkdepth. */
#define AT_STATIC_API

#include "../anchortree.h"
#include "at_tests.h"
#include "tests.h"

/* 640 anchors at unique increasing positions leave ~640 edges, above the
 * 512-edge minimum occupancy that forces a second internal level at the
 * default fanout (2 * AT_FANOUT/2 * AT_FANOUT/2). */
#define AT32_ANCHORS 640

TEST_STATIC void atT_build32(at_Tree *T, att_Model *m, int n) {
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
TEST_STATIC void atT_splice32(at_Tree *T, att_Model *m, int n, att_Splice *w) {
    at_Cursor C;
    atT_modelsplice(m, n, w);
    assertok(at_seek(&C, T, w->L) == AT_OK);
    assertok(at_splice(&C, w->del, w->ins) == AT_OK);
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    assertok(at_checkcursor(&C, w->L + w->ins));
    assertok(atT_modelok(T, m, n));
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
    atT_build32(T, m, AT32_ANCHORS);
    assertok(atT_checkdepth(T, 2));
    for (step = 0; step < 400; ++step) {
        w.L = atT_lcg(&seed) % (atT_modelbound(m, AT32_ANCHORS) + 20u);
        w.del = atT_lcg(&seed) % 25u, w.ins = atT_lcg(&seed) % 10u;
        atT_splice32(T, m, AT32_ANCHORS, &w);
        if (!deep) deep = atT_checkdepth(T, 2);
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
    atT_build32(T, m, n);
    assertok(atT_checkdepth(T, 2));
    /* delete a middle span crossing many leaves; small enough that the
     * remaining anchor-pinned edges keep the default-fanout tree deep */
    total = at_bytes(T);
    w.L = total / 3u, w.del = total / 10u, w.ins = 0;
    atT_splice32(T, m, n, &w);
    assertok(atT_checkdepth(T, 2));
    /* insert at four deep positions */
    for (i = 0; i < 4; ++i) {
        w.L = (at_bytes(T) / 5u) * (size_t)i + 1u;
        w.del = 0, w.ins = (size_t)i * 7u + 3u;
        atT_splice32(T, m, n, &w);
    }
    assertok(atT_checkdepth(T, 2));
    /* delete five spread anchors, then re-add four at deep positions */
    for (i = 0; i < 5; ++i) {
        k = (i + 1) * n / 6;
        assertok(at_delanchor(T, m[k].id) == AT_OK);
        m[k] = m[n - 1], n -= 1;
        assertok(atT_checkchains(T));
        assertok(atT_noemptyedges(T));
        assertok(atT_modelok(T, m, n));
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
    assertok(atT_checkchains(T));
    assertok(atT_noemptyedges(T));
    assertok(atT_modelok(T, m, n));
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

#include "anchortree_test_fanout32.gen.inc"
