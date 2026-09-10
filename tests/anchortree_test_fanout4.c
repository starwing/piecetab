#define AT_FANOUT    4
#define AT_MAX_LEVEL 64 /* AT_FANOUT=4: see docs/max_levels.md */
#define AT_PAGE_SIZE 512
#define AT_STATIC_API

#include <stdio.h>

#include "../anchortree.h"
#include "at_tests.h"
#include "tests.h"

/* anchortree edge-model core tests with fanout 4. */

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

/* null/invalid lifecycle, cursor, splice, and internal-entry guards */
TEST(param_guards) {
    OomCount  oc0 = {0, 0};
    OomCount  oc1 = {0, 1};
    at_State *S;
    at_Tree  *T;
    at_Cursor C, BadC;
    at_Edge  *e;
    at_Id     id;
    size_t    x = 0, y = 0;
    memset(&C, 0, sizeof(at_Cursor));
    memset(&BadC, 0, sizeof(at_Cursor));
    assertok(at_open(&oomcount_alloc, &oc0) == NULL);
    at_close(NULL), at_deltree(NULL), at_reset(NULL);
    S = at_open(&oomcount_alloc, &oc1);
    assertok(S != NULL);
    assertok(at_newtree(S) == NULL);
    at_close(S);
    S = at_open(NULL, NULL);
    T = at_newtree(S);
    assertok(at_seek(NULL, T, 0) == AT_ERRPARAM);
    assertok(at_seek(&C, NULL, 0) == AT_ERRPARAM);
    assertok(at_advance(NULL, 0) == AT_ERRPARAM);
    assertok(at_advance(&BadC, 0) == AT_ERRPARAM);
    BadC.tree = T;
    assertok(at_splice(&BadC, 0, 0) == AT_OK);
    BadC.tree = NULL;
    assertok(at_splice(&BadC, 0, 0) == AT_ERRPARAM);
    assertok(at_newanchor(&BadC, 0, 0, AT_GROW, &id) == AT_ERRPARAM);
    assertok(atI_insertedge(NULL, 5, &e) == AT_ERRPARAM);
    assertok(atI_insertedge(T, 5, NULL) == AT_ERRPARAM);
    assertok(atD_deleteedge(NULL, 1) == AT_ERRPARAM);
    assertok(atD_deleteedge(T, 0) == AT_ERRPARAM);
    assertok(atD_deleteedge(T, 1) == AT_ERRPARAM);
    assertok(at_range(NULL, 1, &x, &y) == AT_ERRPARAM);
    assertok(at_range(T, 1, &x, NULL) == AT_ERRPARAM);
    assertok(at_gravity(NULL, 1) == AT_ERRPARAM);
    at_deltree(T), at_close(S);
}

/* internal guards for overflow, missing positions, and reserve OOMs */
TEST(edge_internal_guards) {
    OomCount   oc1 = {0, 2};
    OomCount   oc2 = {0, 3};
    at_State  *S;
    at_Tree   *T;
    at_Cursor  C;
    at_Edge   *e;
    at_Id      id;
    size_t     x = 0, y = 0;
    size_t     oldcap;
    at_Anchor *a;
    S = at_open(NULL, NULL);
    T = at_newtree(S);
    assertok(at_seek(&C, T, (size_t)-1) == AT_OK);
    assertok(at_newanchor(&C, 1, 0, AT_GROW, &id) == AT_ERRPARAM);
    assertok(atI_insert(T, 0, 0) == AT_OK);
    assertok(atI_insert(T, 0, 7) == AT_OK);
    assertok(atD_collectendpoints(T, 10, 20, &T->origin) == AT_OK);
    assertok(atI_insertedge(T, 10, &e) == AT_OK);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(atD_remove(&C, 0) == AT_OK);
    assertok(atO_findedge(T, 0, &e) == 0);
    assertok(atO_findedge(T, 11, &e) == 0);
    assertok(atD_deleteedge(T, 5) == AT_ERRPARAM);
    oldcap = T->idcap;
    T->idcap = (size_t)-1, T->idfree = 0;
    assertok(atO_growids(T) == AT_ERRMEM);
    T->idcap = oldcap;
    assertok(atO_idalloc(T, &id) == AT_OK);
    assertok(at_del(T, 1000) == AT_ERRPARAM);
    assertok(at_range(T, 1000, &x, &y) == AT_ERRPARAM);
    assertok(at_gravity(T, 1000) == AT_ERRPARAM);
    a = &T->ids[id], a->start = &T->origin, a->end = &T->origin;
    assertok(at_gravity(T, id) == AT_ERRPARAM);
    assertok(atO_push(T, &T->origin.left, id, 0, 0) == AT_OK);
    assertok(at_gravity(T, id) == AT_ERRPARAM);
    at_deltree(T), at_close(S);
    S = at_open(&oomcount_alloc, &oc1);
    T = at_newtree(S);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, 1) == AT_ERRMEM);
    at_deltree(T), at_close(S);
    S = at_open(&oomcount_alloc, &oc2);
    T = at_newtree(S);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 0, 1) == AT_ERRMEM);
    at_deltree(T), at_close(S);
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

static void at_make_ns(at_Tree *T, size_t p, size_t n, int ns, at_Id *id) {
    at_Cursor C;
    assertok(at_seek(&C, T, p) == AT_OK);
    assertok(at_newanchor(&C, n, ns, AT_GROW, id) == AT_OK);
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

/* ns tag is stored on both endpoints and survives RIGHT-side migration */
TEST(anchor_ns_tag) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    at_Id     id;
    size_t    x, y;
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_newanchor(&C, 0, 7, AT_SHIFT, &id) == AT_OK);
    assertok(atO_findedge(T, 10, &e));
    assertok(e->right.first.id == id && e->right.first.ns == 7);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, 0, 3) == AT_OK);
    assertok(at_range(T, id, &x, &y) == AT_OK);
    assertok(x == 13 && y == 13);
    assertok(atO_findedge(T, 13, &e));
    assertok(e->right.first.id == id && e->right.first.ns == 7);
    assertok(e->right.tail != NULL && atV_len(e->right.tail) == 1);
    assertok(e->right.tail[0].id == id && e->right.tail[0].ns == 7);
    at_deltree(T), at_close(S);
}

/* ns tag survives endpoint relocation into the surviving edge@L */
TEST(anchor_ns_relocate) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Edge  *e;
    at_Id     a, b;
    size_t    x, y;
    int       i, n9 = 0;
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
    assertok(atO_findedge(T, 5, &e));
    assertok(e->left.first.id == a && e->left.first.ns == 7);
    assertok(e->right.first.id == a && e->right.first.ns == 7);
    for (i = 0; i < (int)atV_len(e->left.tail); ++i)
        if (e->left.tail[i].ns == 9) n9 += 1;
    assertok(n9 == 2);
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

/* OOM while growing an endpoint tail vector leaves state intact */
TEST(anchor_oom_grow_tail) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     ids[5], id;
    size_t    x, y;
    int       i;
    for (i = 0; i < 5; ++i) at_make_anchor(T, 100, 0, AT_GROW, &ids[i]);
    oc.oom = 0;
    assertok(at_seek(&C, T, 100) == AT_OK);
    assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    for (i = 0; i < 5; ++i) {
        assertok(at_range(T, ids[i], &x, &y) == AT_OK);
        assertok(x == 100 && y == 100);
    }
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* OOM while reserving a new same-side zero-length tail leaves state intact */
TEST(anchor_oom_new_same_side) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    assertok(T != NULL);
    oc.oom = 2;
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_newanchor(&C, 0, 0, AT_SHIFT, &id) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 0);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* OOM before the first edge insertion releases the reserved anchor id */
TEST(anchor_oom_edges_start) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Drain  d;
    at_Id     tmp, id;
    at_make_anchor(T, 100, 0, AT_GROW, &tmp);
    assertok(at_del(T, tmp) == AT_OK);
    d = at_drainpool(&S->edges);
    assertok(d.count > 0);
    *(void **)d.chain = NULL;
    S->edges.freed = d.chain, S->edges.freed_obj = 0;
    oc.oom = 0;
    assertok(at_seek(&C, T, 30) == AT_OK);
    assertok(at_newanchor(&C, 20, 0, AT_GROW, &id) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 0);
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
    oc.oom = 2;
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

/* direct tail-vector reserve, discard, and tail-endpoint removal paths */
TEST(tail_vector_internals) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    at_Point *tp = NULL;
    at_Id     id;
    int       r;
    assertok(atV_reserve(T->S, tp, 0) == AT_OK);
    assertok(tp == NULL);
    assertok(atV_reserve(T->S, tp, 2) == AT_OK);
    assertok(tp != NULL && atV_len(tp) == 0 && atV_cap(tp) >= 2);
    assertok(atV_reserve(T->S, tp, 1) == AT_OK);
    assertok(atV_reserve(T->S, tp, 3) == AT_OK);
    atV_free(T->S, tp);
    r = atO_reserve(T, &T->origin.left, 3);
    assertok(r == AT_OK);
    assertok(T->origin.left.tail != NULL);
    assertok(atO_listcount(&T->origin.left) == 0);
    atO_discardemptytail(T, &T->origin.left);
    assertok(T->origin.left.tail == NULL);
    atO_discardemptytail(T, &T->origin.right);
    assertok(T->origin.right.tail == NULL);
    at_make_anchor(T, 100, 0, AT_SHIFT, &id);
    assertok(atO_findedge(T, 100, &e));
    assertok(atO_listcount(&e->left) == 0);
    assertok(atO_listcount(&e->right) == 2);
    atO_remove(T, &e->right, id, 1);
    assertok(atO_listcount(&e->right) == 1);
    atO_remove(T, &e->right, id, 0);
    assertok(atO_listcount(&e->right) == 0);
    atO_clean(T, &T->origin, 0, 999, 0);
    assertok(at_checktree(T));
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

/* the invariant checker rejects broken leaf/internal links and sizes */
TEST(checktree_bad_links) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Node  *internal, *leaf, *oldparent;
    at_Edge  *edge;
    int       oldlevels;
    at_Index  oldidx;
    size_t    oldbytes;
    int       i;
    at_Edge  *e;
    assertok(at_checktree(NULL) == 0);
    for (i = 1; i <= 10; ++i)
        assertok(atI_insertedge(T, (size_t)(i * 10), &e) == AT_OK);
    assertok(T->levels >= 2);
    internal = (at_Node *)T->root.children[0];
    leaf = (at_Node *)internal->children[0];
    assertok(atN_cc(internal) > 0 && atN_cc(leaf) > 0);
    edge = atN_edge(leaf, 0);
    oldlevels = T->levels;
    oldbytes = leaf->bytes[0];
    leaf->bytes[0] = 0;
    assertok(at_checktree(T) == 0);
    leaf->bytes[0] = oldbytes;
    oldparent = edge->parent;
    edge->parent = NULL;
    assertok(at_checktree(T) == 0);
    edge->parent = oldparent;
    oldidx = edge->idx;
    edge->idx = (at_Index)(edge->idx == 0 ? 1 : 0);
    assertok(at_checktree(T) == 0);
    edge->idx = oldidx;
    oldbytes = internal->bytes[0];
    internal->bytes[0] = oldbytes + 1;
    assertok(at_checktree(T) == 0);
    internal->bytes[0] = oldbytes;
    oldparent = leaf->parent;
    leaf->parent = NULL;
    assertok(at_checktree(T) == 0);
    leaf->parent = oldparent;
    oldidx = leaf->idx;
    leaf->idx = (at_Index)(leaf->idx == 0 ? 1 : 0);
    assertok(at_checktree(T) == 0);
    leaf->idx = oldidx;
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

/* empty-tree insert guards and first edge OOM */
TEST(insert_empty_oom) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Id     a;
    at_make_anchor(T, 0, 0, AT_SHIFT, &a);
    assertok(atI_insert(T, 5, 7) == AT_OK);
    oc.oom = 0;
    assertok(atI_insert(T, 0, 7) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(T->bytes == 0);
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

/* second insertion failure with an origin or occupied left edge */
TEST(edges_oom_cleanup_variants) {
    OomCount  oc = {0, 1 << 20};
    at_State *S;
    at_Tree  *T;
    at_Drain  d;
    at_Edge  *se, *ee;
    at_Id     tmp, keep;
    size_t    x, y;
    S = at_open(&oomcount_alloc, &oc);
    T = at_newtree(S);
    at_make_anchor(T, 10, 0, AT_GROW, &tmp);
    assertok(at_del(T, tmp) == AT_OK);
    d = at_drainpool(&S->edges);
    assertok(d.count > 0);
    *(void **)d.chain = NULL;
    S->edges.freed = d.chain, S->edges.freed_obj = 0;
    oc.oom = 0;
    assertok(atO_edges(T, 0, 50, &se, &ee) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(T->bytes == 0);
    at_deltree(T), at_close(S);
    oc.live = 0, oc.oom = 1 << 20;
    S = at_open(&oomcount_alloc, &oc);
    T = at_newtree(S);
    at_make_anchor(T, 30, 0, AT_GROW, &keep);
    d = at_drainpool(&S->edges);
    assertok(d.count > 0);
    *(void **)d.chain = NULL;
    S->edges.freed = d.chain, S->edges.freed_obj = 0;
    oc.oom = 0;
    assertok(atO_edges(T, 30, 50, &se, &ee) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_range(T, keep, &x, &y) == AT_OK);
    assertok(x == 30 && y == 30);
    assertok(T->bytes == 30);
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

/* left-boundary insertion failure leaves the deletion interval untouched */
TEST(delete_oom_l_boundary) {
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
    S->edges.freed = d.chain, S->edges.freed_obj = 0;
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

/* deleting a prefix leaf slot rebalances a non-root underfull leaf */
TEST(delete_prefix_leaf_rebalance) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    int       i;
    for (i = 0; i < 50; ++i) {
        at_seek(&C, T, (size_t)((i + 1) * 20));
        assertok(at_newanchor(&C, 0, 0, AT_GROW, &id) == AT_OK);
    }
    assertok(at_checktree(T));
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_splice(&C, 20, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    at_deltree(T), at_close(S);
}

/* root split OOM frees the newly allocated left node */
TEST(insert_oom_split_root_cleanup) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Drain  d;
    at_Edge  *e;
    int       i;
    for (i = 1; i <= 5; ++i)
        assertok(atI_insertedge(T, (size_t)(i * 10), &e) == AT_OK);
    assertok(atD_deleteedge(T, 50) == AT_OK);
    assertok(atD_deleteedge(T, 40) == AT_OK);
    assertok(atD_deleteedge(T, 30) == AT_OK);
    assertok(atI_insertedge(T, 30, &e) == AT_OK);
    assertok(atI_insertedge(T, 40, &e) == AT_OK);
    assertok(atN_cc(&T->root) == AT_FANOUT);
    d = at_drainpool(&S->nodes);
    assertok(d.count > 0);
    *(void **)d.chain = NULL;
    S->nodes.freed = d.chain, S->nodes.freed_obj = 1;
    oc.oom = 0;
    assertok(atI_insertedge(T, 50, &e) == AT_ERRMEM);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* root-split first allocation OOM leaves the full root untouched */
TEST(insert_oom_split_root_alloc) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Drain  d;
    at_Edge  *e;
    int       i;
    for (i = 1; i <= 5; ++i)
        assertok(atI_insertedge(T, (size_t)(i * 10), &e) == AT_OK);
    assertok(atD_deleteedge(T, 50) == AT_OK);
    assertok(atD_deleteedge(T, 40) == AT_OK);
    assertok(atD_deleteedge(T, 30) == AT_OK);
    assertok(atI_insertedge(T, 30, &e) == AT_OK);
    assertok(atI_insertedge(T, 40, &e) == AT_OK);
    assertok(T->levels == 0 && atN_cc(&T->root) == AT_FANOUT);
    d = at_drainpool(&S->nodes);
    assertok(d.count > 0);
    *(void **)d.chain = NULL;
    S->nodes.freed = d.chain, S->nodes.freed_obj = 0;
    oc.oom = 0;
    assertok(atI_insertedge(T, 50, &e) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(T->levels == 0 && atN_cc(&T->root) == AT_FANOUT);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* edge lookup returns -1 when no endpoint matches */
TEST(edge_side_missing) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    at_Id     a;
    size_t    x, y;
    assertok(atO_sideof(&T->origin, 1000, 0) == -1);
    at_make_anchor(T, 100, 0, AT_SHIFT, &a);
    assertok(atO_findedge(T, 100, &e));
    assertok(atO_sideof(e, 1000, 0) == -1);
    assertok(atO_sideof(e, 1000, 1) == -1);
    assertok(at_checktree(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 100 && y == 100);
    at_deltree(T), at_close(S);
}

/* deleting all anchors of a multi-level tree collapses the root cleanly */
TEST(delete_root_collapse) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     id[40];
    size_t    prev;
    int       i, shrunk = 0;
    for (i = 0; i < 40; ++i)
        at_make_anchor(T, (size_t)(10 + i * 7), 0, AT_GROW, &id[i]);
    assertok(at_checktree(T));
    assertok(T->levels >= 1);
    prev = (size_t)T->levels;
    for (i = 0; i < 40; ++i) {
        assertok(at_del(T, id[i]) == AT_OK);
        assertok(at_checktree(T));
        if ((size_t)T->levels < prev && T->bytes > 0) shrunk = 1;
        prev = (size_t)T->levels;
    }
    assertok(shrunk);
    assertok(T->levels == 0 && atN_cc(&T->root) == 0 && T->bytes == 0);
    at_deltree(T), at_close(S);
}

/* interior root-split OOM restores child parent/index links */
TEST(insert_oom_split_root_interior_cleanup) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Drain  d;
    at_Edge  *e;
    int       i;
    for (i = 1; i <= 9; ++i)
        assertok(atI_insertedge(T, (size_t)(i * 10), &e) == AT_OK);
    assertok(T->levels == 1);
    assertok(atN_cc(&T->root) == AT_FANOUT);
    d = at_drainpool(&S->nodes);
    assertok(d.count > 0);
    *(void **)d.chain = NULL;
    S->nodes.freed = d.chain, S->nodes.freed_obj = 1;
    oc.oom = 0;
    assertok(atI_insertedge(T, 100, &e) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(T->levels == 1 && atN_cc(&T->root) == AT_FANOUT);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* recursive child-split OOM after edge allocation frees the edge */
TEST(insert_oom_recursive_split_rollback) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Drain  dn, de;
    at_Edge  *e;
    int       i;
    for (i = 1; i <= 8; ++i)
        assertok(atI_insertedge(T, (size_t)(i * 10), &e) == AT_OK);
    assertok(T->levels == 1);
    assertok(atN_cc(&T->root) == 3);
    dn = at_drainpool(&S->nodes);
    de = at_drainpool(&S->edges);
    assertok(dn.count > 0 && de.count > 0);
    *(void **)dn.chain = NULL;
    S->nodes.freed = dn.chain, S->nodes.freed_obj = 0;
    S->edges.freed = de.chain, S->edges.freed_obj = 1;
    oc.oom = 0;
    assertok(atI_insertedge(T, 90, &e) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(T->bytes == 80);
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* deleting across several slots inside one leaf keeps every edge alive */
TEST(splice_multi_slot_delete) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[10];
    size_t    x, y;
    int       i;
    for (i = 0; i < 10; ++i)
        at_make_anchor(T, (size_t)(10 + i * 4), 1, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 12) == AT_OK);
    assertok(at_splice(&C, 16, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    for (i = 0; i < 10; ++i) {
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        assertok(y >= x);
    }
    at_deltree(T), at_close(S);
}

/* a range spanning many leaves exercises borrow/merge and root shrink */
TEST(splice_cross_leaf_rebalance) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[40];
    int       i;
    for (i = 0; i < 40; ++i)
        at_make_anchor(T, (size_t)(10 + i * 7), 2, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, 180, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    at_deltree(T), at_close(S);
}

/* deleting from an unmarked gap (empty L edge) leaves no empty slot */
TEST(splice_empty_L_merge) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    at_make_anchor(T, 50, 5, AT_GROW, &a);
    at_make_anchor(T, 70, 5, AT_LOCK, &b);
    assertok(at_seek(&C, T, 55) == AT_OK);
    assertok(at_splice(&C, 10, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 50 && y == 55);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 60 && y == 65);
    at_deltree(T), at_close(S);
}

/* coordinate/gravity model shared by random and exhaustive splice tests */

typedef struct {
    at_Id  id;
    size_t start;
    size_t end;
    int    gravity;
} at_Model;

static size_t at_model_bound(const at_Model *m, int n) {
    size_t bound = 0;
    int    i;
    for (i = 0; i < n; ++i) {
        if (m[i].start > bound) bound = m[i].start;
        if (m[i].end > bound) bound = m[i].end;
    }
    return bound;
}

static size_t at_model_collapse(size_t p, size_t L, size_t del, size_t R) {
    if (p > L && p <= R) return L;
    return p > R ? p - del : p;
}

static size_t at_model_insert(size_t p, size_t L, size_t ins, int right) {
    if (p == L) return right ? p + ins : p;
    return p > L ? p + ins : p;
}

static void at_model_splice(
        at_Model *m, int n, size_t L, size_t del, size_t ins) {
    size_t R, bound = at_model_bound(m, n);
    int    i;
    if (del && L < bound) {
        R = L + del > bound ? bound : L + del;
        for (i = 0; i < n; ++i) {
            m[i].start = at_model_collapse(m[i].start, L, del, R);
            m[i].end = at_model_collapse(m[i].end, L, del, R);
        }
    }
    if (!ins) return;
    for (i = 0; i < n; ++i) {
        int s0 = m[i].gravity == AT_FIXED || m[i].gravity == AT_SHIFT;
        int s1 = m[i].gravity == AT_GROW || m[i].gravity == AT_SHIFT;
        m[i].start = at_model_insert(m[i].start, L, ins, s0);
        m[i].end = at_model_insert(m[i].end, L, ins, s1);
    }
}

static int at_model_ok(at_Tree *T, const at_Model *m, int n) {
    size_t x, y;
    int    i;
    for (i = 0; i < n; ++i) {
        if (at_range(T, m[i].id, &x, &y) != AT_OK) return 0;
        if (x != m[i].start || y != m[i].end) {
            test_log(
                    "model mismatch id=%u gravity=%d got=%lu..%lu "
                    "want=%lu..%lu\n",
                    m[i].id, m[i].gravity, test_lu(x), test_lu(y),
                    test_lu(m[i].start), test_lu(m[i].end));
            return 0;
        }
    }
    return 1;
}

/* deterministic shared-edge tree used by exhaustive and tail tests */

#define AT_BRUTE_N 12

static const size_t at_brute_pos[AT_BRUTE_N] = {10, 20, 30, 40,  50,  60,
                                                70, 80, 90, 100, 100, 100};
static const size_t at_brute_len[AT_BRUTE_N] = {4, 3, 5, 2, 6, 3,
                                                4, 2, 5, 0, 0, 0};
static const int    at_brute_grav[AT_BRUTE_N] = {
        AT_GROW,  AT_FIXED, AT_SHIFT, AT_LOCK, AT_GROW, AT_FIXED,
        AT_SHIFT, AT_LOCK,  AT_GROW,  AT_GROW, AT_LOCK, AT_FIXED};

static void at_brute_build(at_Tree *T, at_Model *m) {
    at_Cursor C;
    int       i;
    for (i = 0; i < AT_BRUTE_N; ++i) {
        assertok(at_seek(&C, T, at_brute_pos[i]) == AT_OK);
        m[i].gravity = at_brute_grav[i];
        assertok(
                at_newanchor(&C, at_brute_len[i], 0, m[i].gravity, &m[i].id)
                == AT_OK);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
    }
}

static int at_splice_brute_ok(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Model  m[AT_BRUTE_N];
    size_t    pos, del, ins, limit;
    at_brute_build(T, m);
    limit = T->bytes + 1;
    for (pos = 0; pos <= limit; ++pos) {
        for (del = 0; del <= limit; ++del) {
            for (ins = 0; ins <= 2; ++ins) {
                at_reset(T), at_brute_build(T, m);
                at_model_splice(m, AT_BRUTE_N, pos, del, ins);
                assertok(at_seek(&C, T, pos) == AT_OK);
                assertok(at_splice(&C, del, ins) == AT_OK);
                assertok(at_checktree(T));
                assertok(at_no_empty_edges(T));
                assertok(at_checkcursor(&C, pos));
                assertok(at_model_ok(T, m, AT_BRUTE_N));
            }
        }
    }
    at_deltree(T), at_close(S);
    return 1;
}

TEST(splice_brute) { assertok(at_splice_brute_ok()); }

/* deleting through a shared edge pushes both tail vectors */
TEST(splice_collect_tail) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Model  m[AT_BRUTE_N];
    size_t    x, y;
    int       i;
    at_brute_build(T, m);
    assertok(at_seek(&C, T, 95) == AT_OK);
    assertok(at_splice(&C, 5, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_checkcursor(&C, 95));
    for (i = 9; i < AT_BRUTE_N; ++i) {
        assertok(at_range(T, m[i].id, &x, &y) == AT_OK);
        assertok(x == 95 && y == 95);
    }
    at_deltree(T), at_close(S);
}

/* deleting [15,70) rebuilds the leaf chain through makechain/findroom */
TEST(splice_delete_makechain) {
    at_State           *S = at_open(NULL, NULL);
    at_Tree            *T = at_newtree(S);
    at_Cursor           C;
    at_Id               id[10];
    size_t              x, y;
    static const size_t ep[5] = {10, 15, 25, 35, 45};
    static const size_t ex[10] = {10, 15, 15, 15, 15, 15, 15, 25, 35, 45};
    int                 i;
    for (i = 0; i < 10; ++i)
        at_make_anchor(T, (size_t)(10 + i * 10), 0, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 15) == AT_OK);
    assertok(at_splice(&C, 55, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_checkcursor(&C, 15));
    asserteq(T->bytes, 45);
    assertok(at_positions_match(T, ep, 5));
    for (i = 0; i < 10; ++i) {
        assertok(at_range(T, id[i], &x, &y) == AT_OK);
        asserteq(x, ex[i]);
        asserteq(y, ex[i]);
    }
    at_deltree(T), at_close(S);
}

/* deletion to the tree tail leaves a single surviving edge */
TEST(splice_delete_tail) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[3];
    int       i;
    for (i = 0; i < 3; ++i)
        at_make_anchor(T, (size_t)(i * 2), 2, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 2) == AT_OK);
    assertok(at_splice(&C, 4, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_checkcursor(&C, 2));
    at_deltree(T), at_close(S);
}

/* direct internal stitch-chain helper coverage */

TEST(splice_internal_makechain) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    at_make_anchor(T, 100, 0, AT_GROW, &id);
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(atP_reserve(S, &S->nodes, 8) == AT_OK);
    assertok(atD_makechain(&C, -1, atK_levels(&C)) == 1);
    assertok(T->levels == 1);
    at_deltree(T), at_close(S);
}

TEST(splice_internal_findroom) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[5];
    at_Node  *copied;
    int       i;
    for (i = 0; i < 5; ++i)
        at_make_anchor(T, (size_t)(10 + i * 10), 0, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(atK_idx(&C, &T->root, 0) == 0);
    assertok(atN_cc(&T->root) > 1);
    atN_setcc(&T->S->rt[1], 0);
    assertok(atP_reserve(S, &S->nodes, 8) == AT_OK);
    assertok(atD_findroom(&C, T->S->rt, 1) == 0);
    copied = (at_Node *)T->S->rt[1].children[0];
    atN_purge(T, copied, 0, 0, atN_cc(copied));
    atP_free(&S->nodes, copied);
    atN_setcc(&T->S->rt[1], 0);
    at_deltree(T), at_close(S);
}

TEST(splice_internal_backward) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[20];
    size_t    p;
    int       i;
    for (i = 0; i < 20; ++i)
        at_make_anchor(T, (size_t)(10 + i * 10), 0, AT_GROW, &id[i]);
    for (p = 0; p <= T->bytes; ++p) {
        assertok(at_seek(&C, T, p) == AT_OK);
        if (atK_levels(&C) >= 2 && atK_idx(&C, &T->root, 0) > 0
            && atK_idx(&C, atK_parent(&C, 1), 1) < 2)
            break;
    }
    assertok(p <= T->bytes);
    atD_backwardnode(&C, 2, 1);
    at_deltree(T), at_close(S);
}

TEST(splice_internal_stitchtail) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id;
    int       cc;
    at_make_anchor(T, 100, 0, AT_GROW, &id);
    assertok(at_seek(&C, T, 100) == AT_OK);
    cc = atN_cc(&T->root);
    C.off = T->root.bytes[cc - 1], C.poff = 0;
    C.paths[0] = &T->root.children[cc];
    atD_stitchtail(&C);
    assertok(C.paths[0] == &T->root.children[cc - 1]);
    asserteq(C.poff, T->root.bytes[cc - 1]);
    asserteq(C.off, 0);
    at_deltree(T), at_close(S);
}

TEST(splice_internal_findroom_newroot) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[9];
    int       i;
    for (i = 0; i < 9; ++i)
        at_make_anchor(T, (size_t)(10 + i * 10), 0, AT_GROW, &id[i]);
    assertok(at_seek(&C, T, 90) == AT_OK);
    assertok(T->levels == 1);
    assertok(atN_cc(&T->root) == AT_FANOUT);
    assertok(atK_idx(&C, &T->root, 0) == AT_FANOUT - 1);
    assertok(atP_reserve(S, &S->nodes, 32) == AT_OK);
    assertok(atD_findroom(&C, T->S->rt, 1) == 1);
    assertok(T->levels == 2);
    at_deltree(T), at_close(S);
}

TEST(splice_collect_right_oom) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Id     id;
    at_make_anchor(T, 100, 0, AT_GROW, &id);
    oc.oom = 1;
    assertok(atD_collectendpoints(T, 0, 100, &T->origin) == AT_ERRMEM);
    oc.oom = 1 << 20;
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* direct negative shift moves the following edge and its endpoints left */
TEST(splice_shift_negative) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    size_t    x, y;
    at_make_anchor(T, 100, 0, AT_GROW, &a);
    at_make_anchor(T, 200, 0, AT_GROW, &b);
    atI_shift(T, 50, -7);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 93 && y == 93);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 193 && y == 193);
    asserteq(T->bytes, 193);
    at_deltree(T), at_close(S);
}

static int at_splice_random_ok(void) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Model  m[10];
    int       i, step;
    unsigned  seed = 7;
    for (i = 0; i < 10; ++i) {
        assertok(at_seek(&C, T, (size_t)(10 + i * 3)) == AT_OK);
        m[i].gravity = i % 4;
        assertok(
                at_newanchor(&C, (size_t)(i * 2 + 1), 0, m[i].gravity, &m[i].id)
                == AT_OK);
        assertok(at_range(T, m[i].id, &m[i].start, &m[i].end) == AT_OK);
        assertok(at_checktree(T));
        assertok(at_no_empty_edges(T));
    }
    for (step = 0; step < 300; ++step) {
        size_t L, del, ins, bound = at_model_bound(m, 10);
        seed = seed * 1664525u + 1013904223u;
        L = (size_t)((seed >> 8) % (bound + 20u));
        seed = seed * 1664525u + 1013904223u;
        del = (size_t)((seed >> 8) % 25u);
        seed = seed * 1664525u + 1013904223u;
        ins = (size_t)((seed >> 8) % 10u);
        at_model_splice(m, 10, L, del, ins);
        assertok(at_seek(&C, T, L) == AT_OK);
        assertok(at_splice(&C, del, ins) == AT_OK);
        assertok(at_checktree(T));
        assertok(at_no_empty_edges(T));
        assertok(at_checkcursor(&C, L));
        assertok(at_model_ok(T, m, 10));
    }
    at_deltree(T), at_close(S);
    return 1;
}

TEST(splice_random_model) { assertok(at_splice_random_ok()); }

/* P5 object iteration tests */

static int at_collect_ns(at_Tree *T, size_t p, int ns, at_Id *out, int cap) {
    at_Cursor C;
    at_Id     id;
    int       n = 0;
    assertok(at_seek(&C, T, p) == AT_OK);
    for (;;) {
        id = at_next(&C, ns, NULL);
        if (id == AT_NONE) return n;
        assertok(n < cap);
        out[n] = id, n += 1;
    }
}

static int at_collect(at_Tree *T, size_t pos, at_Id *out, int cap) {
    return at_collect_ns(T, pos, 0, out, cap);
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
    size_t    len = 0;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    memset(&D, 0, sizeof(D));
    assertok(at_next(NULL, 0, NULL) == AT_NONE);
    assertok(at_next(&D, 0, NULL) == AT_NONE);
    at_make_anchor(T, 10, 0, AT_GROW, &a);
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(at_next(&C, 0, NULL) == a);
    assertok(at_next(&C, 0, NULL) == AT_NONE);
    assertok(at_next(&C, 0, &len) == AT_NONE);
    asserteq(len, 0);
    at_deltree(T), at_close(S);
}

/* ns filtering keeps START order, skips END and non-matching STARTs */
TEST(iter_ns_filter) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Id     a1, a2, a3, a4, b1, b2, b3, b4, got[8];
    int       n;
    at_make_ns(T, 0, 0, 1, &a1);
    at_make_ns(T, 0, 0, 2, &a2);
    at_make_ns(T, 0, 30, 1, &a3);
    at_make_ns(T, 0, 0, 3, &a4);
    at_make_ns(T, 10, 0, 2, &b1);
    at_make_ns(T, 10, 0, 1, &b2);
    at_make_ns(T, 10, 10, 2, &b3);
    at_make_ns(T, 10, 0, 3, &b4);
    n = at_collect_ns(T, 0, 0, got, 8);
    assertok(n == 8);
    assertok(got[0] == a1 && got[1] == a2 && got[2] == a3 && got[3] == a4);
    assertok(got[4] == b1 && got[5] == b2 && got[6] == b3 && got[7] == b4);
    n = at_collect_ns(T, 0, 1, got, 8);
    assertok(n == 3 && got[0] == a1 && got[1] == a3 && got[2] == b2);
    n = at_collect_ns(T, 0, 2, got, 8);
    assertok(n == 3 && got[0] == a2 && got[1] == b1 && got[2] == b3);
    n = at_collect_ns(T, 0, 3, got, 8);
    assertok(n == 2 && got[0] == a4 && got[1] == b4);
    assertok(at_collect_ns(T, 0, 99, got, 8) == 0);
    at_deltree(T), at_close(S);
}

/* plen reports positive, zero, cross, and exhausted lengths */
TEST(iter_plen) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b, c;
    size_t    len;
    at_make_anchor(T, 0, 5, AT_GROW, &a);
    at_make_anchor(T, 10, 0, AT_GROW, &b);
    at_make_anchor(T, 50, 0, AT_FIXED, &c);
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

/* invalid ns does not exhaust; NULL cursor/tree and NULL plen are safe */
TEST(iter_params) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C, D;
    at_Id     a;
    size_t    len = 99;
    memset(&D, 0, sizeof(D));
    at_make_anchor(T, 0, 3, AT_GROW, &a);
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

/* exhaustion is idempotent; advance(0) preserves, advance(d) restarts */
TEST(iter_exhaust_restart) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    len = 99;
    at_make_ns(T, 0, 0, 1, &a);
    at_make_ns(T, 10, 0, 1, &b);
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

/* ------------------------------------------------------------------ */
/* targeted branch coverage: overflow, OOM orderings, helper misses   */
/* ------------------------------------------------------------------ */

static at_Node *at_lastleaf(at_Tree *T) {
    at_Node *n = &T->root;
    int      l = T->levels;
    while (l > 0) n = n->children[atN_cc(n) - 1], --l;
    return n;
}

static void at_build_full_lastleaf(at_Tree *T) {
    at_Id    id;
    at_Node *last;
    size_t   pos = 10;
    int      guard;
    for (guard = 0; guard < 512; ++guard) {
        at_make_anchor(T, pos, 0, AT_GROW, &id);
        last = at_lastleaf(T);
        if (T->levels >= 2 && atN_cc(last) == AT_FANOUT
            && atN_cc(last->parent) < AT_FANOUT && atN_cc(&T->root) < AT_FANOUT)
            return;
        pos += 10;
    }
    assert(0);
}

/* saturated deletion length takes the overflow branch and clamps at EOF */
TEST(splice_overflow_del) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    size_t    x, y;
    at_make_anchor(T, 10, 5, AT_GROW, &a);
    at_make_anchor(T, 40, 5, AT_GROW, &b);
    assertok(at_seek(&C, T, 20) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_checkcursor(&C, 20));
    assertok(at_range(T, a, &x, &y) == AT_OK);
    assertok(x == 10 && y == 15);
    assertok(at_range(T, b, &x, &y) == AT_OK);
    assertok(x == 20 && y == 20);
    asserteq(T->bytes, 20);
    at_deltree(T), at_close(S);
}

/* deleting almost the whole tree stitches a large remainder back */
TEST(splice_delete_large_span) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[100];
    size_t    x, y;
    int       i;
    for (i = 0; i < 100; ++i)
        at_make_anchor(T, (size_t)(10 + i * 7), (size_t)(i % 5), 2, &id[i]);
    assertok(at_seek(&C, T, 10) == AT_OK);
    assertok(at_splice(&C, (size_t)-1, 0) == AT_OK);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    assertok(at_checkcursor(&C, 10));
    for (i = 0; i < 100; ++i) assertok(at_range(T, id[i], &x, &y) == AT_OK);
    at_deltree(T), at_close(S);
}

/* OOM after the left reserve allocates an empty tail frees it cleanly */
TEST(collect_discard_empty_tail) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    at_make_anchor(T, 100, 0, AT_GROW, &a);
    at_make_anchor(T, 100, 0, AT_GROW, &b);
    oc.oom = 3;
    assertok(atD_collectendpoints(T, 50, 105, &T->origin) == AT_ERRMEM);
    assertok(T->origin.left.tail == NULL);
    assertok(T->origin.right.tail == NULL);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* a reserve failure must keep an existing non-empty tail intact */
TEST(collect_discard_nonempty_tail) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Edge  *le;
    at_Id     a, b;
    at_make_anchor(T, 100, 10, AT_GROW, &a);
    at_make_anchor(T, 100, 10, AT_GROW, &b);
    assertok(atO_findedge(T, 100, &le));
    assertok(le->left.tail != NULL);
    oc.oom = 1;
    assertok(atD_collectendpoints(T, 100, 115, le) == AT_ERRMEM);
    assertok(le->left.tail != NULL);
    asserteq(atO_listcount(&le->left), 2);
    assertok(le->right.tail == NULL);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* OOM on a tail-vector push during endpoint collection propagates */
TEST(collect_tail_push_oom) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Id     id[6];
    int       i;
    for (i = 0; i < 6; ++i) at_make_anchor(T, 100, 10, AT_GROW, &id[i]);
    oc.oom = 1;
    assertok(atD_collectendpoints(T, 50, 105, &T->origin) == AT_ERRMEM);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* OOM inside a child collection propagates through the recursive level */
TEST(collect_recurse_oom) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Id     id[10];
    int       i;
    for (i = 0; i < 10; ++i)
        at_make_anchor(T, (size_t)(10 + i * 10), 0, AT_GROW, &id[i]);
    assertok(T->levels >= 1);
    oc.oom = 0;
    assertok(atD_collectendpoints(T, 0, 1000, &T->origin) == AT_ERRMEM);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* endpoint removal search misses: null tail, scanned tail, role mismatch */
TEST(remove_search_miss) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e1, *e2;
    at_Id     a, b, c;
    at_make_anchor(T, 100, 10, AT_GROW, &a);
    at_make_anchor(T, 100, 10, AT_GROW, &b);
    at_make_anchor(T, 300, 10, AT_GROW, &c);
    assertok(atO_findedge(T, 100, &e1));
    assertok(atO_findedge(T, 300, &e2));
    asserteq(atO_listcount(&e1->left), 2);
    asserteq(atO_listcount(&e2->left), 1);
    atO_remove(T, &e2->left, 999, 0);
    asserteq(atO_listcount(&e2->left), 1);
    atO_remove(T, &e1->left, 999, 0);
    asserteq(atO_listcount(&e1->left), 2);
    atO_remove(T, &e1->left, b, 1);
    asserteq(atO_listcount(&e1->left), 2);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

/* backward node move climbs through zero-index ancestors */
TEST(backward_climb_parent) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[100];
    size_t    p;
    int       i, l;
    for (i = 0; i < 100; ++i)
        at_make_anchor(T, (size_t)(10 + i * 10), 0, AT_GROW, &id[i]);
    for (p = 0; p <= T->bytes; ++p) {
        assertok(at_seek(&C, T, p) == AT_OK);
        l = atK_levels(&C);
        if (l < 2) continue;
        if (atK_idx(&C, atK_parent(&C, l), l) != 0) continue;
        if (atK_idx(&C, atK_parent(&C, l - 1), l - 1) != 0) continue;
        if (atK_idx(&C, atK_parent(&C, l - 2), l - 2) == 0) continue;
        break;
    }
    assertok(p <= T->bytes);
    atD_backwardnode(&C, 1, l);
    assertok(
            atK_idx(&C, atK_parent(&C, l), l) == atN_cc(atK_parent(&C, l)) - 1);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
}

/* OOM while reserving the edge pool aborts before any tree change */
TEST(splice_reserve_edges_oom) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     id[10];
    int       i;
    for (i = 0; i < 10; ++i)
        at_make_anchor(T, (size_t)(10 + i * 10), 0, AT_GROW, &id[i]);
    assertok(atP_reserve(S, &S->nodes, 4 * (T->levels + 2u) + 8u) == AT_OK);
    at_drainpool(&S->edges);
    oc.oom = 0;
    assertok(at_seek(&C, T, 50) == AT_OK);
    assertok(at_splice(&C, 1, 1) == AT_ERRMEM);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* right-boundary insert failure with L at origin cleans nothing extra */
TEST(delete_left_origin_oom) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     a, b;
    at_make_anchor(T, 0, 10, AT_GROW, &a);
    at_make_anchor(T, 100, 10, AT_GROW, &b);
    at_drainpool(&S->edges);
    oc.oom = 0;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(atD_remove(&C, 50) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* collect failure after a new right boundary removes both new edges */
TEST(delete_collect_oom_boundaries) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Drain  d;
    at_Id     a, b, c;
    at_make_anchor(T, 0, 10, AT_GROW, &a);
    at_make_anchor(T, 60, 5, AT_GROW, &b);
    at_make_anchor(T, 200, 10, AT_GROW, &c);
    assertok(atP_reserve(S, &S->edges, 1) == AT_OK);
    d = at_drainpool(&S->edges);
    assertok(d.count > 0);
    *(void **)d.chain = NULL;
    S->edges.freed = d.chain, S->edges.freed_obj = 1;
    oc.oom = 0;
    assertok(at_seek(&C, T, 0) == AT_OK);
    assertok(atD_remove(&C, 70) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* append failure at the last edge skips the shift rollback */
TEST(insert_seam_oom_tail) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Id     a, b;
    at_make_anchor(T, 100, 0, AT_FIXED, &a);
    at_make_anchor(T, 200, 0, AT_FIXED, &b);
    at_drainpool(&S->edges);
    oc.oom = 0;
    assertok(atI_insert(T, 200, 7) == AT_ERRMEM);
    assertok(at_checktree(T));
    assertok(at_no_empty_edges(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* insert-path child split OOM leaves the tree unchanged */
TEST(insert_split_oom_rollback) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Node  *last;
    at_Edge  *e;
    at_Drain  de;
    size_t    pos;
    at_build_full_lastleaf(T);
    last = at_lastleaf(T);
    pos = atO_edgepos(T, atN_edge(last, 0)) + 1;
    assertok(atP_reserve(S, &S->edges, 1) == AT_OK);
    de = at_drainpool(&S->edges);
    *(void **)de.chain = NULL;
    S->edges.freed = de.chain, S->edges.freed_obj = 1;
    at_drainpool(&S->nodes);
    oc.oom = 0;
    assertok(atI_insertedge(T, pos, &e) == AT_ERRMEM);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

/* append-path split OOM propagates without updating parent bytes */
TEST(append_split_oom_rollback) {
    OomCount  oc = {0, 1 << 20};
    at_State *S = at_open(&oomcount_alloc, &oc);
    at_Tree  *T = at_newtree(S);
    at_Edge  *e;
    at_Drain  de;
    size_t    pos;
    at_build_full_lastleaf(T);
    pos = T->bytes + 10;
    assertok(atP_reserve(S, &S->edges, 1) == AT_OK);
    de = at_drainpool(&S->edges);
    *(void **)de.chain = NULL;
    S->edges.freed = de.chain, S->edges.freed_obj = 1;
    at_drainpool(&S->nodes);
    oc.oom = 0;
    assertok(atI_insertedge(T, pos, &e) == AT_ERRMEM);
    assertok(at_checktree(T));
    at_deltree(T), at_close(S);
    asserteq(oc.live, 0);
}

#include "anchortree_test_fanout4.gen.inc"
