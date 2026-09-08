#define AT_FANOUT    8
#define AT_PAGE_SIZE 512
#define AT_STATIC_API

#include <stdio.h>

#include "../anchortree.h"
#include "at_tests.h"
#include "tests.h"

/* anchortree edge-model core tests with fanout 8. */

static int at_insert_seq(at_Tree *T, const size_t *pos, int n) {
    at_Edge *e;
    int      i;
    for (i = 0; i < n; ++i)
        if (atI_insertedge(T, pos[i], &e) != AT_OK) return 0;
    return at_checktree(T);
}

static int at_positions_match(at_Tree *T, const size_t *pos, int n) {
    at_TestSeq s;
    int        i;
    if (at_seq(T, &s) != n) return 0;
    for (i = 0; i < n; ++i)
        if (s.pos[i] != pos[i]) return 0;
    return 1;
}

/* lifecycle: open/newtree/deltree/reset and allocator drain */
TEST(lifecycle) {
    Count     c;
    at_State *S;
    at_Tree  *T;
    int       i;
    c.live = 0;
    S = at_open(&count_alloc, &c);
    assertok(S != NULL);
    T = at_newtree(S);
    assertok(T != NULL);
    assertok(at_checktree(T));
    asserteq(T->bytes, 0);
    asserteq(T->levels, 0);
    at_deltree(T);
    at_close(S);
    asserteq(c.live, 0);
    /* reset keeps the tree allocation */
    S = at_open(&count_alloc, &c);
    T = at_newtree(S);
    for (i = 1; i <= 9; ++i) {
        at_Edge *e;
        assertok(atI_insertedge(T, (size_t)i * 10u, &e) == AT_OK);
    }
    assertok(at_checktree(T));
    at_reset(T);
    assertok(at_checktree(T));
    asserteq(T->bytes, 0);
    asserteq(T->levels, 0);
    at_deltree(T);
    at_close(S);
    asserteq(c.live, 0);
    asserteq(at_newtree(NULL), NULL);
}

/* empty tree cursor and virtual movement */
TEST(empty_seek) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_checkcursor(&C, 0));
    assertok(at_seek(&C, T, 5) == AT_OK);
    assertok(at_checkcursor(&C, 5));
    assertok(at_advance(&C, 3) == AT_OK);
    assertok(at_checkcursor(&C, 8));
    assertok(at_advance(&C, -20) == AT_OK);
    assertok(at_checkcursor(&C, 0));
    at_deltree(T), at_close(S);
}

/* cursor at origin, gaps, exact edges, and beyond the last edge */
TEST(seek_positions) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Edge            *e;
    at_Cursor           C;
    static const size_t seqp[3] = {10, 30, 50};
    static const size_t seeks[10] = {0, 5, 10, 15, 25, 30, 35, 50, 60, 70};
    int                 i;
    assertok(atI_insertedge(T, 10, &e) == AT_OK);
    assertok(atI_insertedge(T, 50, &e) == AT_OK);
    assertok(atI_insertedge(T, 30, &e) == AT_OK);
    assertok(at_positions_match(T, seqp, 3));
    for (i = 0; i < 10; ++i) {
        assertok(at_seek(&C, T, seeks[i]) == AT_OK);
        assertok(at_checkcursor(&C, seeks[i]));
        assertok(at_offset(&C) == seeks[i]);
    }
    at_deltree(T), at_close(S);
}

/* advance must preserve exact-edge and virtual conventions */
TEST(advance_positions) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Edge            *e;
    at_Cursor           C;
    static const size_t seqp[3] = {10, 30, 50};
    int                 i;
    assertok(atI_insertedge(T, 10, &e) == AT_OK);
    assertok(atI_insertedge(T, 50, &e) == AT_OK);
    assertok(atI_insertedge(T, 30, &e) == AT_OK);
    assertok(at_positions_match(T, seqp, 3));
    for (i = 0; i < 55; ++i) {
        assertok(at_seek(&C, T, 0) == AT_OK);
        assertok(at_advance(&C, (at_Delta)i) == AT_OK);
        assertok(at_checkcursor(&C, (size_t)i));
        assertok(at_advance(&C, -1) == AT_OK);
        assertok(at_checkcursor(&C, i ? (size_t)(i - 1) : 0));
    }
    at_deltree(T), at_close(S);
}

/* fanout-4 insertion forces leaf and interior splits */
TEST(split_sequence) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    p[50], i;
    at_Edge  *e;
    for (i = 0; i < 50; ++i) p[i] = (i + 1) * 13u;
    assertok(at_insert_seq(T, p, 50));
    assertok(at_positions_match(T, p, 50));
    assertok(atI_insertedge(T, 650, &e) == AT_OK);
    assertok(at_positions_match(T, p, 50));
    at_deltree(T), at_close(S);
}

/* deletion inside a gap merges bytes into the successor edge */
TEST(delete_middle) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    before[5] = {10, 30, 50, 80, 100};
    size_t    after[4] = {10, 50, 80, 100};
    size_t    extended[5] = {10, 50, 80, 100, 650};
    at_Edge  *e;
    assertok(at_insert_seq(T, before, 5));
    assertok(atD_deleteedge(T, 30) == AT_OK);
    assertok(at_positions_match(T, after, 4));
    assertok(atI_insertedge(T, 650, &e) == AT_OK);
    assertok(at_positions_match(T, extended, 5));
    at_deltree(T), at_close(S);
}

/* deleting the final edge shrinks T->bytes */
TEST(delete_last) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    before[5] = {10, 30, 50, 80, 100};
    size_t    after[4] = {10, 30, 50, 80};
    at_Edge  *e;
    assertok(at_insert_seq(T, before, 5));
    assertok(atD_deleteedge(T, 100) == AT_OK);
    assertok(T->bytes == 80);
    assertok(at_positions_match(T, after, 4));
    assertok(atI_insertedge(T, 90, &e) == AT_OK);
    assertok(T->bytes == 90);
    at_deltree(T), at_close(S);
}

/* deleting all edges must leave a clean empty tree */
TEST(delete_all) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    size_t    p[50], i;
    at_Edge  *e;
    for (i = 0; i < 50; ++i) p[i] = (i + 1) * 7u;
    assertok(at_insert_seq(T, p, 50));
    for (i = 0; i < 50; ++i) assertok(atD_deleteedge(T, p[i]) == AT_OK);
    assertok(T->bytes == 0);
    assertok(T->levels == 0);
    assertok(atN_cc(&T->root) == 0);
    assertok(at_checktree(T));
    assertok(atI_insertedge(T, 123, &e) == AT_OK);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

/* random insert/delete with invariant checks after each mutation */
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
        assertok(atI_insertedge(T, p, &e) == AT_OK);
        assertok(at_checktree(T));
    }
    for (i = 0; i < 300; ++i) {
        seed = seed * 1664525u + 1013904223u;
        p = (seed % 5000u) + 1u;
        if (p <= T->bytes && atO_findedge(T, p, &e))
            assertok(atD_deleteedge(T, p) == AT_OK);
        assertok(at_checktree(T));
    }
    at_deltree(T), at_close(S);
}

/* object/endpoint tests for P3 */

static void at_make_anchor(
        at_Tree *T, size_t pos, size_t len, int gravity, at_Id *id) {
    at_Cursor C;
    assertok(at_seek(&C, T, pos) == AT_OK);
    assertok(at_newanchor(&C, len, 0, gravity, id) == AT_OK);
    assertok(at_checktree(T));
}

/* anchor id allocation starts at 1 and reuses deleted ids */
TEST(anchor_id_reuse) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c;
    size_t    x, y;
    at_make_anchor(T, 10, 5, AT_GROW, &a);
    at_make_anchor(T, 20, 5, AT_FIXED, &b);
    asserteq(a, 1);
    asserteq(b, 2);
    assertok(at_del(T, a) == AT_OK);
    at_make_anchor(T, 30, 5, AT_SHIFT, &c);
    asserteq(c, a);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    asserteq(x, 30);
    asserteq(y, 35);
    assertok(at_gravity(T, c) == AT_SHIFT);
    assertok(at_del(T, b) == AT_OK);
    assertok(at_del(T, c) == AT_OK);
    at_deltree(T), at_close(S);
}

/* all four gravity mappings survive creation and query */
TEST(anchor_gravities) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               id[4];
    static const int    gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    static const size_t ps[4] = {10, 40, 80, 120};
    int                 i;
    for (i = 0; i < 4; ++i) at_make_anchor(T, ps[i], 10, gv[i], &id[i]);
    for (i = 0; i < 4; ++i) assertok(at_gravity(T, id[i]) == gv[i]);
    for (i = 0; i < 4; ++i) assertok(at_del(T, id[i]) == AT_OK);
    asserteq(T->bytes, 0);
    at_deltree(T), at_close(S);
}

/* zero-length anchors keep START and END on the same edge */
TEST(anchor_zero_length) {
    at_State        *S = at_open(NULL, NULL);
    at_Tree         *T = at_newtree(S);
    at_Id            id[4];
    static const int gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    size_t           x, y;
    int              i;
    for (i = 0; i < 4; ++i) at_make_anchor(T, 100, 0, gv[i], &id[i]);
    for (i = 0; i < 4; ++i) {
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        asserteq(x, 100);
        asserteq(y, 100);
        assertok(at_gravity(T, id[i]) == gv[i]);
    }
    for (i = 0; i < 4; ++i) assertok(at_del(T, id[i]) == AT_OK);
    asserteq(T->bytes, 0);
    asserteq(atN_cc(&T->root), 0);
    at_deltree(T), at_close(S);
}

/* ranges cover origin, tree positions, and virtual extension */
TEST(anchor_range_positions) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               a, b, c;
    size_t              x, y;
    static const size_t expect[5] = {5, 20, 23, 1000, 1010};
    at_make_anchor(T, 0, 5, AT_GROW, &a);
    at_make_anchor(T, 1000, 10, AT_FIXED, &b);
    at_make_anchor(T, 20, 3, AT_SHIFT, &c);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    asserteq(x, 0);
    asserteq(y, 5);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    asserteq(x, 1000);
    asserteq(y, 1010);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    asserteq(x, 20);
    asserteq(y, 23);
    assertok(at_positions_match(T, expect, 5));
    assertok(at_del(T, a) == AT_OK);
    assertok(at_del(T, b) == AT_OK);
    assertok(at_del(T, c) == AT_OK);
    asserteq(T->bytes, 0);
    at_deltree(T), at_close(S);
}

/* deleting an anchor removes both edge slots and shrinks the tree */
TEST(anchor_delete_cleans) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Id               a, b;
    size_t              x, y;
    static const size_t two[2] = {20, 25};
    static const size_t four[4] = {20, 25, 100, 110};
    at_make_anchor(T, 20, 5, AT_LOCK, &b);
    at_make_anchor(T, 100, 10, AT_GROW, &a);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    asserteq(x, 100);
    asserteq(y, 110);
    assertok(at_positions_match(T, four, 4));
    assertok(at_del(T, a) == AT_OK);
    asserteq(T->bytes, 25);
    assertok(at_positions_match(T, two, 2));
    assertok(at_del(T, b) == AT_OK);
    asserteq(T->bytes, 0);
    at_deltree(T), at_close(S);
}

/* lifecycle frees dense id storage and endpoint memory */
TEST(anchor_lifecycle_ids) {
    Count     c;
    at_State *S;
    at_Tree  *T;
    at_Id     a, b;
    c.live = 0;
    S = at_open(&count_alloc, &c);
    T = at_newtree(S);
    at_make_anchor(T, 10, 5, AT_GROW, &a);
    at_make_anchor(T, 30, 7, AT_LOCK, &b);
    assertok(T->ids != NULL && T->idcap >= 3);
    at_reset(T);
    asserteq(T->ids, NULL);
    asserteq(T->idcap, 0);
    asserteq(T->idfree, 0);
    at_make_anchor(T, 20, 4, AT_FIXED, &a);
    at_deltree(T);
    at_close(S);
    asserteq(c.live, 0);
}

/* invalid ids and parameters are rejected */
TEST(anchor_invalid_params) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x = 0, y = 0;
    assertok(at_newanchor(NULL, 0, 0, AT_GROW, &id) == AT_ERRPARAM);
    assertok(at_del(NULL, 1) == AT_ERRPARAM);
    assertok(at_del(T, 0) == AT_ERRPARAM);
    assertok(at_del(T, 1) == AT_ERRPARAM);
    assertok(at_range(NULL, 1, &x, &y) == AT_ERRPARAM);
    assertok(at_range(T, 1, &x, &y) == AT_ERRPARAM);
    assertok(at_range(T, 1, NULL, &y) == AT_ERRPARAM);
    assertok(at_gravity(T, 1) < 0);
    assertok(at_gravity(T, 0) < 0);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_newanchor(&C, 0, 0, 99, &id) == AT_ERRPARAM);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, NULL) == AT_ERRPARAM);
    at_make_anchor(T, 50, 5, AT_GROW, &id);
    assertok(at_del(T, id) == AT_OK);
    assertok(at_del(T, id) == AT_ERRPARAM);
    assertok(at_range(T, id, &x, &y) == AT_ERRPARAM);
    assertok(at_gravity(T, id) < 0);
    at_deltree(T), at_close(S);
}

/* P4 splice tests */

static int at_no_empty_edges(at_Tree *T) {
    at_TestSeq s;
    int        i;
    at_seq(T, &s);
    for (i = 0; i < s.n; ++i)
        if (atO_empty(s.edge[i])) return 0;
    return 1;
}

/* splice in an unmarked gap before the first known edge */
TEST(splice_gap_delete_insert) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x, y;
    at_make_anchor(T, 20, 5, AT_GROW, &id);
    at_seek(&C, T, 10);
    assertok(at_splice(&C, 3, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_checkcursor(&C, 10));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 17);
    asserteq(y, 22);
    asserteq(T->bytes, 22);
    at_seek(&C, T, 10);
    assertok(at_splice(&C, 0, 4) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 21);
    asserteq(y, 26);
    asserteq(T->bytes, 26);
    at_deltree(T), at_close(S);
}

/* insert on an edge separates the four gravity endpoint pairs */
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
        at_make_anchor(T, 100, 0, gv[i], &id);
        at_seek(&C, T, 100);
        assertok(at_splice(&C, 0, 7) == AT_OK);
        assertok(at_checktree(T));
        assertok(at_no_empty_edges(T));
        assertok(at_checkcursor(&C, 100));
        assertok(at_gravity(T, id) == gv[i]);
        assertok(at_range(T, id, &x, &y) == AT_OK);
        asserteq(x, ex[i][0]);
        asserteq(y, ex[i][1]);
        if (gv[i] == AT_LOCK)
            asserteq(T->bytes, 100);
        else
            asserteq(T->bytes, 107);
        at_deltree(T), at_close(S);
    }
}

/* deletion spanning several edges collapses interior endpoints to L */
TEST(splice_delete_spanning) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c;
    size_t    x, y;
    at_make_anchor(T, 10, 5, AT_GROW, &a);
    at_make_anchor(T, 30, 5, AT_LOCK, &b);
    at_make_anchor(T, 50, 5, AT_FIXED, &c);
    at_seek(&C, T, 20);
    assertok(at_splice(&C, 25, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
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
    asserteq(T->bytes, 30);
    at_deltree(T), at_close(S);
}

/* delete through the last known edge, then insert at the collapse point */
TEST(splice_delete_last_insert) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x, y;
    at_make_anchor(T, 100, 10, AT_GROW, &id);
    at_seek(&C, T, 80);
    assertok(at_splice(&C, 40, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 80);
    asserteq(y, 80);
    asserteq(T->bytes, 80);
    at_seek(&C, T, 80);
    assertok(at_splice(&C, 0, 6) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 80);
    asserteq(y, 86);
    asserteq(T->bytes, 86);
    at_deltree(T), at_close(S);
}

/* FIXED zero-length insertion creates a numeric cross; later gap insertion
 * shifts only the right-side start endpoint */
TEST(splice_zero_cross) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    size_t    x, y;
    at_make_anchor(T, 100, 0, AT_FIXED, &id);
    at_seek(&C, T, 100);
    assertok(at_splice(&C, 0, 7) == AT_OK);
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 107);
    asserteq(y, 100);
    at_seek(&C, T, 105);
    assertok(at_splice(&C, 0, 4) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_gravity(T, id) == AT_FIXED);
    assertok(at_range(T, id, &x, &y) == AT_OK);
    asserteq(x, 111);
    asserteq(y, 100);
    asserteq(T->bytes, 111);
    at_deltree(T), at_close(S);
}

/* invalid cursor and zero-zero splice are no-ops */
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
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    at_deltree(T), at_close(S);
}

/* OOM in id-array growth leaves an untouched empty tree */
TEST(anchor_oom_idalloc) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    assertok(T != NULL);
    oc.oom = 0;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_newanchor(&C, 5, 0, AT_GROW, &id) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->ids == NULL);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* OOM while reserving an endpoint tail leaves state intact */
TEST(anchor_oom_start_push) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     keep, spare, id;
    size_t    x, y;
    at_make_anchor(T, 100, 0, AT_GROW, &keep);
    at_make_anchor(T, 200, 0, AT_GROW, &spare);
    assertok(at_del(T, spare) == AT_OK);
    oc.oom = 0;
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, keep, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_gravity(T, keep) == AT_GROW);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* OOM while reserving the END endpoint tail leaves state intact */
TEST(anchor_oom_end_push) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     keep, spare, id;
    size_t    x, y;
    at_make_anchor(T, 100, 0, AT_FIXED, &keep);
    at_make_anchor(T, 200, 0, AT_GROW, &spare);
    assertok(at_del(T, spare) == AT_OK);
    oc.oom = 0;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_newanchor(&C, 100, 0, AT_GROW, &id) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, keep, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_gravity(T, keep) == AT_FIXED);
    assertok(atO_listcount(&T->origin.left) == 0);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* OOM before moving right endpoints at the last edge is rolled back */
TEST(splice_oom_insert_last) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    at_make_anchor(T, 100, 0, AT_FIXED, &a);
    at_make_anchor(T, 100, 0, AT_FIXED, &b);
    oc.oom = 4;
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_splice(&C, 0, 7) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 100);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* OOM after an edge shift restores the successor coordinate */
TEST(splice_oom_insert_shift) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c;
    size_t    x, y;
    at_make_anchor(T, 100, 0, AT_FIXED, &a);
    at_make_anchor(T, 100, 0, AT_FIXED, &b);
    at_make_anchor(T, 200, 0, AT_FIXED, &c);
    oc.oom = 4;
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_splice(&C, 0, 7) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 200);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 200 && y == 200);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* OOM in bulk-delete vector reservation leaves the interval untouched */
TEST(splice_oom_delete) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    at_make_anchor(T, 50, 0, AT_GROW, &a);
    at_make_anchor(T, 55, 0, AT_GROW, &b);
    oc.oom = 6;
    assertok(at_seek(&C, T, 30) == AT_OK);
    assertok(at_splice(&C, 40, 0) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 50 && y == 50);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 55 && y == 55);
    assertok(T->bytes == 55);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* deletion cleans newly created boundaries when position allocation fails */
TEST(splice_oom_delete_boundaries) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a;
    size_t    x, y;
    at_make_anchor(T, 80, 10, AT_GROW, &a);
    oc.oom = 4;
    assertok(at_seek(&C, T, 30) == AT_OK);
    assertok(at_splice(&C, 40, 0) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 80 && y == 90);
    assertok(T->bytes == 90);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* at_del performs frees only and must succeed under a zero-budget allocator */
TEST(del_oom_none) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Id     a;
    at_make_anchor(T, 30, 10, AT_GROW, &a);
    oc.oom = 0;
    assertok(at_del(T, a) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 0);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* deleting a tail endpoint exercises the vector search path */
TEST(anchor_tail_delete) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c;
    size_t    x, y;
    at_make_anchor(T, 100, 0, AT_GROW, &a);
    at_make_anchor(T, 100, 0, AT_GROW, &b);
    at_make_anchor(T, 100, 0, AT_GROW, &c);
    assertok(at_del(T, b) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    assertok(at_del(T, a) == AT_OK);
    assertok(at_del(T, c) == AT_OK);
    assertok(T->bytes == 0);
    at_deltree(T), at_close(S);
}

/* a successful delete grows an existing destination tail vector */
TEST(splice_grow_destination_tail) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     ids[10];
    int       i;
    for (i = 0; i < 8; ++i) at_make_anchor(T, 10, 0, AT_GROW, &ids[i]);
    for (i = 8; i < 10; ++i) at_make_anchor(T, 20, 0, AT_GROW, &ids[i]);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 10, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 10);
    at_deltree(T), at_close(S);
}

/* the invariant checker still detects an overfull node */
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

/* OOM while moving origin RIGHT endpoints rolls back the new edge */
TEST(splice_oom_empty_insert) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a;
    size_t    x, y;
    at_make_anchor(T, 0, 0, AT_SHIFT, &a);
    oc.oom = 5;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, 7) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 0);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 0 && y == 0);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* direct insert rolls back the edge shift when edge allocation fails */
TEST(insert_oom_edge_alloc_rollback) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Drain  d;
    at_Id     a, b, c;
    size_t    x, y;
    at_make_anchor(T, 100, 0, AT_FIXED, &a);
    at_make_anchor(T, 100, 0, AT_FIXED, &b);
    at_make_anchor(T, 200, 0, AT_FIXED, &c);
    d = at_drainpool(&S->edges);
    oc.oom = 0;
    assertok(atI_insert(T, 100, 7) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 200);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 200 && y == 200);
    at_refillpool(&S->edges, d);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* second edge insertion failure removes the new left boundary edge */
TEST(edges_oom_end_cleanup) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Drain  d;
    at_Edge  *se, *ee;
    at_Id     tmp;
    at_make_anchor(T, 10, 0, AT_GROW, &tmp);
    assertok(at_del(T, tmp) == AT_OK);
    d = at_drainpool(&S->edges);
    assertok(d.count > 0);
    *(void **)d.chain = NULL;
    S->edges.freed = d.chain, S->edges.freed_obj = 1;
    oc.oom = 0;
    assertok(atO_edges(T, 30, 50, &se, &ee) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 0);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* right-boundary insertion failure removes the new left boundary edge */
TEST(delete_oom_r_boundary_cleanup) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Drain  d;
    at_Id     a;
    size_t    x, y;
    at_make_anchor(T, 80, 10, AT_GROW, &a);
    d = at_drainpool(&S->edges);
    assertok(d.count > 0);
    *(void **)d.chain = NULL;
    S->edges.freed = d.chain, S->edges.freed_obj = 1;
    oc.oom = 0;
    assertok(at_seek(&C, T, 30) == AT_OK);
    assertok(atD_remove(&C, 40) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 80 && y == 90);
    assertok(T->bytes == 90);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* random splice operations against a coordinate/gravity model */

static int at_splice_random_ok(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     ids[10];
    size_t    ms[10], me[10], bound = 0;
    int       mg[10], i, step;
    unsigned  seed = 7;
    for (i = 0; i < 10; ++i) {
        at_seek(&C, T, (size_t)(10 + i * 3));
        mg[i] = i % 4;
        at_newanchor(&C, (size_t)(i * 2 + 1), 0, mg[i], &ids[i]);
        assertok(at_range(T, ids[i], &ms[i], &me[i]) == AT_OK);
        assertok(at_checktree(T));
        assertok(at_no_empty_edges(T));
    }
    for (step = 0; step < 300; ++step) {
        size_t L, del, ins;
        bound = 0;
        for (i = 0; i < 10; ++i) {
            if (ms[i] > bound) bound = ms[i];
            if (me[i] > bound) bound = me[i];
        }
        seed = seed * 1664525u + 1013904223u;
        L = (size_t)((seed >> 8) % (bound + 20u));
        seed = seed * 1664525u + 1013904223u;
        del = (size_t)((seed >> 8) % 25u);
        seed = seed * 1664525u + 1013904223u;
        ins = (size_t)((seed >> 8) % 10u);
        for (i = 0; i < 10; ++i) {
            size_t p, R;
            if (del && L < bound) {
                R = L + del > bound ? bound : L + del;
                p = ms[i];
                if (p > L && p <= R)
                    p = L;
                else if (p > R)
                    p -= del;
                ms[i] = p;
                p = me[i];
                if (p > L && p <= R)
                    p = L;
                else if (p > R)
                    p -= del;
                me[i] = p;
            }
            if (ins) {
                int s0 = mg[i] == AT_FIXED || mg[i] == AT_SHIFT;
                int s1 = mg[i] == AT_GROW || mg[i] == AT_SHIFT;
                p = ms[i];
                if (p == L) {
                    if (s0) p += ins;
                } else if (p > L)
                    p += ins;
                ms[i] = p;
                p = me[i];
                if (p == L) {
                    if (s1) p += ins;
                } else if (p > L)
                    p += ins;
                me[i] = p;
            }
        }
        assertok(at_seek(&C, T, L) == AT_OK);
        assertok(at_splice(&C, del, ins) == AT_OK);
        assertok(at_checktree(T));
        assertok(at_no_empty_edges(T));
        assertok(at_checkcursor(&C, L));
        for (i = 0; i < 10; ++i) {
            size_t x, y;
            assertok(at_range(T, ids[i], &x, &y) == AT_OK);
            check(x == ms[i] && y == me[i],
                  "splice model mismatch step %d id %d gravity %d\n", step, i,
                  mg[i]);
        }
    }
    at_deltree(T), at_close(S);
    return 1;
}

TEST(splice_random_model) { assertok(at_splice_random_ok()); }

/* P5 object iteration tests */

static int at_collect(at_Tree *T, size_t pos, at_Id *out, int cap) {
    at_Cursor C;
    at_Id     id;
    int       n = 0;
    assertok(at_seek(&C, T, pos) == AT_OK);
    for (;;) {
        id = at_next(&C);
        if (id == AT_NONE) return n;
        assertok(n < cap);
        out[n] = id, n += 1;
    }
}

/* iteration covers multiple edges, all gravities, and skips END items */
TEST(iter_multiple_edges) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, c, d, got[4];
    at_make_anchor(T, 10, 10, AT_GROW, &a);
    at_make_anchor(T, 10, 0, AT_FIXED, &b);
    at_make_anchor(T, 20, 10, AT_SHIFT, &c);
    at_make_anchor(T, 30, 0, AT_LOCK, &d);
    assertok(at_collect(T, 0, got, 4) == 4);
    assertok(got[0] == a && got[1] == b && got[2] == c && got[3] == d);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

/* origin START precedes a first tree edge that also has STARTs */
TEST(iter_origin_first_start) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, got[2];
    at_make_anchor(T, 0, 5, AT_GROW, &a);
    at_make_anchor(T, 5, 0, AT_LOCK, &b);
    assertok(at_collect(T, 0, got, 2) == 2);
    assertok(got[0] == a && got[1] == b);
    assertok(at_collect(T, 1, got, 2) == 1);
    assertok(got[0] == b);
    at_deltree(T), at_close(S);
}

/* a zero-length object at origin iterates even with no tree edges */
TEST(iter_origin_empty_tree) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, got[1];
    at_make_anchor(T, 0, 0, AT_GROW, &a);
    assertok(T->bytes == 0);
    assertok(at_collect(T, 0, got, 1) == 1);
    assertok(got[0] == a);
    at_deltree(T), at_close(S);
}

/* iteration crosses leaf splits without losing coordinate order */
TEST(iter_many_edges_cross_leaves) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     id[60], got[60];
    int       i, n;
    for (i = 0; i < 60; ++i)
        at_make_anchor(T, (size_t)(1 + i * 3), 0, AT_GROW, &id[i]);
    n = at_collect(T, 0, got, 60);
    assertok(n == 60);
    for (i = 0; i < 60; ++i) assertok(got[i] == id[i]);
    n = at_collect(T, 61, got, 60);
    assertok(n == 40);
    for (i = 0; i < 40; ++i) assertok(got[i] == id[20 + i]);
    at_deltree(T), at_close(S);
}

/* zero-length START is emitted once; its same-edge END is skipped */
TEST(iter_zero_length_once) {
    at_State        *S = at_open(NULL, NULL);
    at_Tree         *T = at_newtree(S);
    static const int gv[4] = {AT_GROW, AT_FIXED, AT_SHIFT, AT_LOCK};
    at_Id            id[4], got[4];
    size_t           x, y;
    int              i, j, n;
    for (i = 0; i < 4; ++i) at_make_anchor(T, 100, 0, gv[i], &id[i]);
    n = at_collect(T, 0, got, 4);
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

/* seeking in a gap includes the next edge; exact-edge seek includes it */
TEST(iter_gap_and_edge_starts) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b, got[2];
    at_make_anchor(T, 30, 5, AT_GROW, &a);
    at_make_anchor(T, 50, 5, AT_LOCK, &b);
    assertok(at_collect(T, 40, got, 2) == 1);
    assertok(got[0] == b);
    assertok(at_collect(T, 30, got, 2) == 2);
    assertok(got[0] == a && got[1] == b);
    assertok(at_collect(T, 50, got, 2) == 1);
    assertok(got[0] == b);
    assertok(at_collect(T, 55, got, 2) == 0);
    assertok(at_collect(T, 60, got, 2) == 0);
    at_deltree(T), at_close(S);
}

/* empty tree and end-of-iteration are idempotent */
TEST(iter_empty_end_idempotent) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C, D;
    at_Id     a;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C) == AT_NONE);
    assertok(at_next(&C) == AT_NONE);
    memset(&D, 0, sizeof(D));
    assertok(at_next(NULL) == AT_NONE);
    assertok(at_next(&D) == AT_NONE);
    at_make_anchor(T, 10, 0, AT_GROW, &a);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C) == a);
    assertok(at_next(&C) == AT_NONE);
    assertok(at_next(&C) == AT_NONE);
    at_deltree(T), at_close(S);
}

/* after insertion, iteration reflects shifted endpoint positions */
TEST(iter_after_splice_insert) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c, got[3];
    size_t    x, y;
    at_make_anchor(T, 0, 5, AT_GROW, &c);
    at_make_anchor(T, 10, 10, AT_GROW, &a);
    at_make_anchor(T, 20, 10, AT_LOCK, &b);
    assertok(at_seek(&C, T, 15) == AT_OK);
    assertok(at_splice(&C, 0, 5) == AT_OK);
    assertok(at_collect(T, 0, got, 3) == 3);
    assertok(got[0] == c && got[1] == a && got[2] == b);
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 10 && y == 25);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 25 && y == 35);
    at_deltree(T), at_close(S);
}

/* after deletion, iteration sees collapsed and shifted starts */
TEST(iter_after_splice_delete) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c, got[3];
    size_t    x, y;
    at_make_anchor(T, 10, 5, AT_GROW, &a);
    at_make_anchor(T, 30, 5, AT_LOCK, &b);
    at_make_anchor(T, 50, 5, AT_FIXED, &c);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, 25, 0) == AT_OK);
    assertok(at_collect(T, 0, got, 3) == 3);
    assertok(got[0] == a && got[1] == b && got[2] == c);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 20 && y == 20);
    assertok(at_range(T, c, &x, &y) == AT_OK);
    assertok(x == 25 && y == 30);
    at_deltree(T), at_close(S);
}

#include "anchortree_test_fanout8.gen.inc"
