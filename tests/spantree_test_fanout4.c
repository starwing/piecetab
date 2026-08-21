#define SP_FANOUT    4
#define SP_PAGE_SIZE 512
#define SP_STATIC_API
#ifndef SP_POOL_STATS
# define SP_POOL_STATS
#endif

/* spantree.h debug builds may use fprintf (stdio must precede it) */
#include <stdio.h>

#include "../spantree.h"
#include "sp_tests.h"
#include "tests.h"

/* spantree tests: fanout-4 forces splits/merges on small inputs.
 * Skeleton — tests added per implementation block
 * (notes/plans/spantree_impl.md). */

/* state: open/newtree/freetree/close lifecycle; pool stats drain to zero. */
TEST(state) {
    int       i;
    sp_State *S;
    sp_Tree  *t;
    for (i = 0; i < 3; ++i) {
        S = sp_open(NULL, NULL);
        assertok(S != NULL);
        t = sp_newtree(S);
        assertok(t != NULL);
        asserteq(sp_bytes(t), 0);
        asserteq(t->levels, 0);
        assertok(sp_checktree(t));
        sp_freetree(t);
        asserteq(S->nodes.live_obj, 0);
        sp_close(S);
    }
    asserteq(sp_bytes(NULL), 0);
    asserteq(sp_newtree(NULL), NULL);
}

/* reserve failure: drained pool + failing page alloc must surface
 * SP_ERRMEM and leave the tree untouched (transactional reserve) */
TEST(reserve_oom) {
    int       cnt = 1 << 20;
    sp_State *S = sp_open(&oom_alloc, &cnt);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C, R;
    sp_Drain  d;
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 16), SP_OK);
    d = sp_drainpool(&S->nodes);
    cnt = 0;
    asserteq(sp_append(&C, 1), SP_ERRMEM);
    asserteq(sp_insert(&C, 1), SP_ERRMEM);
    asserteq(sp_splice(&C, 1, 1), SP_ERRMEM);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_seek(&R, t, 3), SP_OK);
    asserteq(sp_remove(&C, &R), SP_ERRMEM);
    asserteq(sp_fill(&C, 9, 2), SP_ERRMEM);
    asserteq(sp_bytes(t), 16);
    sp_refillpool(&S->nodes, d);
    sp_freetree(t), sp_close(S);
}

/* node ops: makespace/copy/move/remove/sumbytes on stack nodes. */
TEST(nodes) {
    sp_Node a, b;
    int     i;
    memset(&a, 0, sizeof(a));
    spN_setcc(&a, 4);
    for (i = 0; i < 4; ++i) {
        spL_setid(&a, i, (sp_Id)(10 + i));
        a.bytes[i] = (size_t)(i + 1);
    }
    /* sumbytes: half-open [i, end) over bytes[] */
    asserteq(spN_sumbytes(&a, 1, 3), 5);
    asserteq(spN_sumbytes(&a, 0, 4), 10);
    asserteq(spN_sumbytes(&a, 3, 3), 0);
    /* copy: slot range, children+bytes travel together, cc untouched */
    memset(&b, 0, sizeof(b));
    spN_copy(&b, 1, &a, 0, 2);
    asserteq(spN_cc(&b), 0);
    asserteq(spL_id(&b, 1), 10);
    asserteq(b.bytes[1], 1);
    asserteq(spL_id(&b, 2), 11);
    asserteq(b.bytes[2], 2);
    /* move: overlapping shift within one node */
    spN_move(&a, 1, 0, 3);
    asserteq(spL_id(&a, 1), 10);
    asserteq(a.bytes[1], 1);
    asserteq(spL_id(&a, 3), 12);
    asserteq(a.bytes[3], 3);
    /* makespace: open n slots at i, cc grows */
    spN_setcc(&a, 2);
    spN_makespace(&a, 1, 2);
    asserteq(spN_cc(&a), 4);
    asserteq(spL_id(&a, 0), 10);
    asserteq(spL_id(&a, 3), 10);
    asserteq(a.bytes[3], 1);
    /* remove: close n slots at i, cc shrinks */
    spN_move(&a, 1, 3, 1), spN_setcc(&a, 2);
    asserteq(spN_cc(&a), 2);
    asserteq(spL_id(&a, 0), 10);
    asserteq(spL_id(&a, 1), 10);
    asserteq(a.bytes[1], 1);
    /* spL_setid round-trip, id 0 = NULL slot */
    spL_setid(&a, 0, 0);
    asserteq(spL_id(&a, 0), 0);
    spL_setid(&a, 0, 7);
    asserteq(spL_id(&a, 0), 7);
}

/* cursor seek: empty tree, segment boundaries, virtual excess beyond bytes */
TEST(seek) {
    sp_State *S;
    sp_Tree  *t;
    sp_Cursor C;
    size_t    len;
    S = sp_open(NULL, NULL);
    t = sp_newtree(S);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_offset(&C), 0);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 0));
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(len, 0);
    asserteq(C.off, 0);
    asserteq(C.poff, 5);
    asserteq(sp_offset(&C), 5);
    assertok(sp_checkcursor(&C, 5));
    /* leaf-only tree [3,5,2] */
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_offset(&C), 0);
    asserteq(sp_style(&C, &len, NULL), 1);
    asserteq(len, 3);
    assertok(sp_checkcursor(&C, 0));
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_offset(&C), 2);
    asserteq(sp_style(&C, &len, NULL), 1);
    asserteq(len, 1);
    assertok(sp_checkcursor(&C, 2));
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq(sp_offset(&C), 3);
    asserteq(sp_style(&C, &len, NULL), 2);
    asserteq(len, 5);
    assertok(sp_checkcursor(&C, 3));
    asserteq(sp_seek(&C, t, 8), SP_OK);
    asserteq(sp_offset(&C), 8);
    asserteq(sp_style(&C, &len, NULL), 3);
    asserteq(len, 2);
    assertok(sp_checkcursor(&C, 8));
    asserteq(sp_seek(&C, t, 10), SP_OK);
    asserteq(sp_offset(&C), 10);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(len, 0);
    assertok(sp_checkcursor(&C, 10));
    asserteq(sp_seek(&C, t, 12), SP_OK);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(len, 0);
    asserteq(C.off, 8);
    asserteq(C.poff, 4);
    asserteq(sp_offset(&C), 12);
    assertok(sp_checkcursor(&C, 12));
    sp_freetree(t), sp_close(S);
}

/* traversal: next/prev across segments and leaf containers, ends NULL */
TEST(traverse) {
    sp_State *S;
    sp_Tree  *t;
    sp_Node  *leaf0, *leaf1;
    sp_Cursor C;
    size_t    len;
    S = sp_open(NULL, NULL);
    t = sp_newtree(S);
    leaf0 = (sp_Node *)spP_alloc(S, &S->nodes);
    leaf1 = (sp_Node *)spP_alloc(S, &S->nodes);
    memset(leaf0, 0, sizeof(sp_Node)), memset(leaf1, 0, sizeof(sp_Node));
    spL_setid(leaf0, 0, 10), leaf0->bytes[0] = 2;
    spL_setid(leaf0, 1, 11), leaf0->bytes[1] = 3;
    spN_setcc(leaf0, 2);
    spL_setid(leaf1, 0, 12), leaf1->bytes[0] = 1;
    spL_setid(leaf1, 1, 13), leaf1->bytes[1] = 4;
    spN_setcc(leaf1, 2);
    t->root.children[0] = leaf0, t->root.children[1] = leaf1;
    t->root.bytes[0] = 5, t->root.bytes[1] = 5;
    spN_setcc(&t->root, 2), t->levels = 1, t->bytes = 10;
    assertok(sp_checktree(t));
    /* next: within leaf container, across leaf containers, NULL at tail */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_next(&C, 0, &len), 11);
    asserteq(len, 3);
    asserteq(sp_offset(&C), 2);
    assertok(sp_checkcursor(&C, 2));
    asserteq(sp_next(&C, 0, &len), 12);
    asserteq(len, 1);
    asserteq(sp_offset(&C), 5);
    asserteq(sp_next(&C, 0, &len), 13);
    asserteq(len, 4);
    asserteq(sp_offset(&C), 6);
    assertok(sp_checkcursor(&C, 6));
    asserteq(sp_next(&C, 0, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_offset(&C), 10);
    assertok(sp_checkcursor(&C, 10));
    asserteq(sp_next(&C, 0, &len), SP_NONE);
    asserteq(len, 0);
    /* prev: across segments and leaf containers, NULL at head */
    asserteq(sp_seek(&C, t, 10), SP_OK);
    asserteq(sp_prev(&C, 0, &len), 13);
    asserteq(len, 4);
    asserteq(sp_offset(&C), 6);
    assertok(sp_checkcursor(&C, 6));
    asserteq(sp_prev(&C, 0, &len), 12);
    asserteq(len, 1);
    asserteq(sp_offset(&C), 5);
    asserteq(sp_prev(&C, 0, &len), 11);
    asserteq(len, 3);
    asserteq(sp_offset(&C), 2);
    asserteq(sp_prev(&C, 0, &len), 10);
    asserteq(len, 2);
    asserteq(sp_offset(&C), 0);
    assertok(sp_checkcursor(&C, 0));
    asserteq(sp_prev(&C, 0, &len), SP_NONE);
    asserteq(len, 0);
    /* prev inside a segment rewinds to its head */
    asserteq(sp_seek(&C, t, 7), SP_OK);
    asserteq(sp_prev(&C, 0, &len), 13);
    asserteq(len, 1);
    asserteq(sp_offset(&C), 6);
    assertok(sp_checkcursor(&C, 6));
    asserteq(sp_prev(&C, 0, &len), 12);
    asserteq(len, 1);
    asserteq(sp_offset(&C), 5);
    sp_freetree(t), sp_close(S);
}

/* advance: forward within/across, tail clamp, virtual excess, backward */
TEST(advance) {
    sp_State *S;
    sp_Tree  *t;
    sp_Cursor C;
    size_t    len;
    S = sp_open(NULL, NULL);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    assertok(sp_checktree(t));
    /* forward: within segment, across segment, to tail, virtual excess */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_advance(&C, 2), SP_OK);
    asserteq(sp_offset(&C), 2);
    asserteq(sp_style(&C, &len, NULL), 1);
    asserteq(len, 1);
    assertok(sp_checkcursor(&C, 2));
    asserteq(sp_advance(&C, 1), SP_OK);
    asserteq(sp_offset(&C), 3);
    asserteq(sp_style(&C, &len, NULL), 2);
    asserteq(len, 5);
    asserteq(sp_advance(&C, 5), SP_OK);
    asserteq(sp_offset(&C), 8);
    asserteq(sp_style(&C, &len, NULL), 3);
    asserteq(len, 2);
    asserteq(sp_advance(&C, 2), SP_OK);
    asserteq(sp_offset(&C), 10);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(len, 0);
    assertok(sp_checkcursor(&C, 10));
    asserteq(sp_advance(&C, 3), SP_OK);
    asserteq(sp_offset(&C), 13);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(len, 0);
    assertok(sp_checkcursor(&C, 13));
    /* backward from virtual: stay virtual, then reenter the tree */
    asserteq(sp_advance(&C, -1), SP_OK);
    asserteq(sp_offset(&C), 12);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(len, 0);
    assertok(sp_checkcursor(&C, 12));
    asserteq(sp_locate(&C, 13), SP_OK);
    asserteq(sp_offset(&C), 13);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_advance(&C, -4), SP_OK);
    asserteq(sp_offset(&C), 9);
    asserteq(sp_style(&C, &len, NULL), 3);
    asserteq(len, 1);
    assertok(sp_checkcursor(&C, 9));
    /* backward: within segment, across segment, clamp to head */
    asserteq(sp_seek(&C, t, 9), SP_OK);
    asserteq(sp_advance(&C, -1), SP_OK);
    asserteq(sp_offset(&C), 8);
    asserteq(sp_style(&C, &len, NULL), 3);
    asserteq(len, 2);
    asserteq(sp_advance(&C, -8), SP_OK);
    asserteq(sp_offset(&C), 0);
    asserteq(sp_style(&C, &len, NULL), 1);
    asserteq(len, 3);
    assertok(sp_checkcursor(&C, 0));
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_advance(&C, -5), SP_OK);
    asserteq(sp_offset(&C), 0);
    sp_freetree(t), sp_close(S);
}

/* backwardoff must climb from a leaf with no previous sibling to its parent */
TEST(backwardoff_climb) {
    sp_State *S;
    sp_Tree  *t;
    sp_Node  *leaf0, *leaf1;
    sp_Cursor C;
    size_t    len;
    S = sp_open(NULL, NULL);
    t = sp_newtree(S);
    leaf0 = (sp_Node *)spP_alloc(S, &S->nodes);
    leaf1 = (sp_Node *)spP_alloc(S, &S->nodes);
    memset(leaf0, 0, sizeof(sp_Node)), memset(leaf1, 0, sizeof(sp_Node));
    spL_setid(leaf0, 0, 10), leaf0->bytes[0] = 2;
    spL_setid(leaf0, 1, 11), leaf0->bytes[1] = 3;
    spN_setcc(leaf0, 2);
    spL_setid(leaf1, 0, 12), leaf1->bytes[0] = 1;
    spL_setid(leaf1, 1, 13), leaf1->bytes[1] = 4;
    spN_setcc(leaf1, 2);
    t->root.children[0] = leaf0, t->root.children[1] = leaf1;
    t->root.bytes[0] = 5, t->root.bytes[1] = 5;
    spN_setcc(&t->root, 2), t->levels = 1, t->bytes = 10;
    assertok(sp_checktree(t));
    /* start of leaf1: one byte backward must climb to previous leaf */
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_offset(&C), 5);
    asserteq(sp_advance(&C, -1), SP_OK);
    asserteq(sp_offset(&C), 4);
    asserteq(sp_style(&C, &len, NULL), 11);
    asserteq(len, 1);
    assertok(sp_checkcursor(&C, 4));
    sp_freetree(t), sp_close(S);
}

static void collect_stream(sp_Cursor *C, sp_Id *ids, size_t *lens, int *n);

/* locate inside an existing tree (non-virtual branch) */
TEST(locate_in_tree) {
    sp_State *S;
    sp_Tree  *t;
    sp_Cursor C;
    size_t    len;
    S = sp_open(NULL, NULL);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_locate(&C, 5), SP_OK);
    asserteq(sp_offset(&C), 5);
    asserteq(sp_style(&C, &len, NULL), 2);
    asserteq(len, 3);
    assertok(sp_checkcursor(&C, 5));
    asserteq(sp_locate(&C, 0), SP_OK);
    asserteq(sp_offset(&C), 0);
    asserteq(sp_style(&C, &len, NULL), 1);
    assertok(sp_checkcursor(&C, 0));
    sp_freetree(t), sp_close(S);
}

/* rebalance on a leaf-only tree: the root-children fold guard with
 * levels == 0 is a no-op */
TEST(rebalance_leaf) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spN_setcc(&t->root, 2), t->bytes = 8;
    asserteq(sp_seek(&C, t, 0), SP_OK);
    spD_rebalance(&C, 0);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 0));
    sp_freetree(t), sp_close(S);
}

/* lochead with a NULL plen: internal head helper null out-param */
TEST(lochead_null) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    size_t    len;
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spN_setcc(&t->root, 2), t->bytes = 8;
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(spK_lochead(&C, NULL), 1);
    asserteq(sp_offset(&C), 0);
    assertok(sp_checkcursor(&C, 0));
    asserteq(spK_lochead(&C, &len), 1);
    asserteq(len, 3);
    asserteq(sp_offset(&C), 0);
    assertok(sp_checkcursor(&C, 0));
    sp_freetree(t), sp_close(S);
}

/* seamleaf: merge adjacent same-id leaf segments in both cursor
 * positions (right merge and left merge) */
TEST(seamleaf_merge) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[8];
    size_t    lens[8];
    int       n;
    /* merge right into left: cursor on first of two same-id segments */
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 2;
    spL_setid(&t->root, 1, 1), t->root.bytes[1] = 3;
    spL_setid(&t->root, 2, 2), t->root.bytes[2] = 4;
    spN_setcc(&t->root, 3), t->bytes = 9;
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(spK_seamleaf(&C, 1), 1);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 0));
    sp_seek(&C, t, 0);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 5);
    asserteq(ids[1], 2);
    asserteq(lens[1], 4);
    sp_freetree(t);
    /* merge left into current: cursor on second of two same-id segments */
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 2;
    spL_setid(&t->root, 1, 1), t->root.bytes[1] = 3;
    spL_setid(&t->root, 2, 2), t->root.bytes[2] = 4;
    spN_setcc(&t->root, 3), t->bytes = 9;
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(spK_seamleaf(&C, 0), 1);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 2));
    sp_seek(&C, t, 0);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 5);
    asserteq(ids[1], 2);
    asserteq(lens[1], 4);
    sp_freetree(t), sp_close(S);
}

/* append at tree head when the first segment is already id 0 grows it */
TEST(append_head_zero) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[8];
    size_t    lens[8];
    int       n;
    spL_setid(&t->root, 0, 0), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 1), t->root.bytes[1] = 2;
    spN_setcc(&t->root, 2), t->bytes = 5;
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 2), SP_OK);
    asserteq(sp_bytes(t), 7);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 2));
    sp_seek(&C, t, 0);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 0);
    asserteq(lens[0], 5);
    asserteq(ids[1], 1);
    asserteq(lens[1], 2);
    sp_freetree(t), sp_close(S);
}

/* ---- insertion: append/insert, inheritance, cursor motion ---- */

/* collect segment stream: ids then lens, from cursor to tree end */
static void collect_stream(sp_Cursor *C, sp_Id *ids, size_t *lens, int *n) {
    size_t len;
    sp_Id  id;
    *n = 0;
    sp_seek(C, C->tree, 0);
    for (;;) {
        id = sp_style(C, &len, NULL);
        if (len == 0) break; /* segment ids can be 0: plen marks the end */
        ids[*n] = id, lens[*n] = len, *n += 1;
        sp_next(C, 0, &len);
    }
}

TEST(insert_append) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    /* empty tree append */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 5), SP_OK);
    asserteq(sp_bytes(t), 5);
    asserteq(sp_offset(&C), 5);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 5));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 5);
    /* mid-segment append: [3,5,2] ids [1,2,3], insert 2 at off 1 */
    sp_freetree(t);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_append(&C, 2), SP_OK);
    asserteq(sp_bytes(t), 12);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 3));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 5);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    asserteq(ids[2], 3);
    asserteq(lens[2], 2);
    /* segment-boundary append: at off 3 (seg 2 start), inherits seg 1 */
    sp_freetree(t);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spN_setcc(&t->root, 2), t->bytes = 8;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq(sp_append(&C, 4), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 7);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    /* tail append inherits last segment */
    asserteq(sp_seek(&C, t, 12), SP_OK);
    asserteq(sp_append(&C, 2), SP_OK);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 14));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 7);
    asserteq(ids[1], 2);
    asserteq(lens[1], 7);
    /* insert at segment start inherits right (current) segment, cursor
     * stays at insertion point */
    sp_freetree(t);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spN_setcc(&t->root, 2), t->bytes = 8;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq(sp_insert(&C, 4), SP_OK);
    asserteq(sp_offset(&C), 3);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 3));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 2);
    asserteq(lens[1], 9);
    /* no-op */
    asserteq(sp_append(&C, 0), SP_OK);
    asserteq(sp_insert(&C, 0), SP_OK);
    asserteq(sp_bytes(t), 12);
    sp_freetree(t), sp_close(S);
}

TEST(padding) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    /* virtual seek then append pads [0, 5) with id 0 */
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_append(&C, 3), SP_OK);
    asserteq(sp_bytes(t), 8);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 8));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 8);
    /* virtual insert pads and inherits right (none at tail) */
    asserteq(sp_seek(&C, t, 12), SP_OK);
    asserteq(sp_insert(&C, 2), SP_OK);
    asserteq(sp_bytes(t), 14);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 14);
    /* mid-tree edit after pad stays correct */
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    asserteq(sp_bytes(t), 15);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 2));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 15);
    sp_freetree(t), sp_close(S);
}

/* ---- removal: sp_remove double cursor, splice ---- */

TEST(remove) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    /* mid-segment remove shrinks the segment (adjacent ids merge) */
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&L, t, 1), SP_OK);
    asserteq(sp_seek(&R, t, 2), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    asserteq(sp_bytes(t), 9);
    assertok(sp_checktree(t));
    sp_seek(&L, t, 0);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    asserteq(ids[2], 3);
    asserteq(lens[2], 2);
    /* remove across two segments: boundary merge when ids equal */
    sp_freetree(t);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 2), t->root.bytes[2] = 4;
    spN_setcc(&t->root, 3), t->bytes = 12;
    assertok(sp_checktree_allow_unseamedspan(t, 1)); /* pre-merge input */
    asserteq(sp_seek(&L, t, 3), SP_OK);
    asserteq(sp_seek(&R, t, 8), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 2);
    asserteq(lens[1], 4);
    /* remove whole tree */
    asserteq(sp_seek(&L, t, 0), SP_OK);
    asserteq(sp_seek(&R, t, 7), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    asserteq(sp_bytes(t), 0);
    assertok(sp_checktree(t));
    sp_seek(&L, t, 0);
    {
        size_t zl;
        asserteq(sp_style(&L, &zl, NULL), SP_NONE);
    }
    /* no-op and cross-tree errors */
    asserteq(sp_seek(&L, t, 0), SP_OK);
    asserteq(sp_remove(&L, &L), SP_OK);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

TEST(splice) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    /* mid-segment splice: delete 2 insert 3 */
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spN_setcc(&t->root, 2), t->bytes = 8;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_splice(&C, 2, 3), SP_OK);
    asserteq(sp_bytes(t), 9);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 4);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    /* splice across segments */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_splice(&C, 4, 1), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 0);
    asserteq(lens[0], 1);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    /* tail splice pads the gap */
    asserteq(sp_seek(&C, t, 12), SP_OK);
    asserteq(sp_splice(&C, 0, 2), SP_OK);
    asserteq(sp_bytes(t), 14);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[2], 0);
    asserteq(lens[2], 8);
    /* no-op */
    asserteq(sp_splice(&C, 0, 0), SP_OK);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* fill fully inside one segment: A->ABA / A->AB / A->BA splits */
TEST(fill_leaf) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t;
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    /* A->ABA: mid-segment fill splits the leaf into three */
    t = treeV(0, leafV(1, 10));
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq(sp_fill(&C, 9, 4), SP_OK);
    asserteq(sp_offset(&C), 7);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 7));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 9);
    asserteq(lens[1], 4);
    asserteq(ids[2], 1);
    asserteq(lens[2], 3);
    /* A->AB: fill from the segment head */
    sp_freetree(t);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 10;
    spN_setcc(&t->root, 1), t->bytes = 10;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 9, 7), SP_OK);
    asserteq(sp_offset(&C), 7);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 7));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 9);
    asserteq(lens[0], 7);
    asserteq(ids[1], 1);
    asserteq(lens[1], 3);
    /* A->BA: fill from the segment head to the tree tail */
    sp_freetree(t);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 10;
    spN_setcc(&t->root, 1), t->bytes = 10;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq(sp_fill(&C, 9, 7), SP_OK);
    asserteq(sp_offset(&C), 10);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 10));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 9);
    asserteq(lens[1], 7);
    sp_freetree(t), sp_close(S);
}

/* fill with the same id inside a segment: the early-return path keeps
 * the tree unchanged and cancels the split-protection refcount */
TEST(fill_same_inside) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    SpRef     r;
    sp_Id     ids[8];
    size_t    lens[8];
    int       n;
    memset(&r, 0, sizeof(r));
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 10;
    spN_setcc(&t->root, 1), t->bytes = 10;
    sp_setarbiter(t, spA_ref, &r);
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(spF_filterleaf(&C, 3, 1, 1), 0);
    asserteq(sp_offset(&C), 5);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 5));
    sp_seek(&C, t, 0);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 10);
    sp_freetree(t), sp_close(S);
}

/* ---- fill: arbiter filter over a range ---- */

static sp_Id arb_sum(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud, (void)mask;
    return old + id;
}

static int   arb_count;
static sp_Id arb_counting(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud, (void)mask;
    if (id && old) arb_count += 1; /* merge-shaped calls only */
    return id;
}

static int   arb_notify;
static sp_Id arb_notifypad(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud, (void)old, (void)mask;
    if (id == 0 && old == 0) arb_notify += 1;
    return 0;
}

/* append inside a segment extends the current segment directly; the
 * arbiter is not consulted for a pure length grow */
TEST(append_inside_expands) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spN_setcc(&t->root, 1), t->bytes = 3;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 2));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 4);
    sp_freetree(t), sp_close(S);
}

TEST(fill_basic) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    /* segment-internal fill: [1,3) in id1 -> 9 splits into three */
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_fill(&C, 9, 2), SP_OK);
    asserteq(sp_offset(&C), 3);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 3));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 9);
    asserteq(lens[1], 2);
    asserteq(ids[2], 2);
    asserteq(lens[2], 5);
    asserteq(ids[3], 3);
    asserteq(lens[3], 2);
    /* segment-head fill: [0,2) -> 9; both segments and the tail of the
     * second merge into one 9-run */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 9, 2), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 9);
    asserteq(lens[0], 3);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    asserteq(ids[2], 3);
    asserteq(lens[2], 2);
    /* whole segment then neighbor merge: [2,3) -> 9 joins the 9-run */
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_fill(&C, 9, 1), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 9);
    asserteq(lens[0], 3);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    asserteq(ids[2], 3);
    asserteq(lens[2], 2);
    /* cross-segment fill: [2,6) spans id1/id2 -> 9 */
    sp_freetree(t);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_fill(&C, 9, 4), SP_OK);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 6));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4); /* adjacent same-id 9-runs merge into one run */
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    asserteq(ids[1], 9);
    asserteq(lens[1], 4);
    asserteq(ids[2], 2);
    asserteq(lens[2], 2);
    asserteq(ids[3], 3);
    asserteq(lens[3], 2);
    /* fill that colors the whole tree: [0,10) -> 7 */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 7, 10), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 7);
    asserteq(lens[0], 10);
    /* len == 0 and null cursor */
    asserteq(sp_fill(&C, 1, 0), SP_OK);
    asserteq(sp_fill(NULL, 1, 1), SP_ERRPARAM);
    asserteq(sp_bytes(t), 10);
    sp_freetree(t), sp_close(S);
}

TEST(fill_arb) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    /* arbiter sum: old + in; absorb direction merges left */
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    assertok(sp_checktree(t));
    sp_setarbiter(t, arb_sum, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 5, 3), SP_OK);
    asserteq(sp_offset(&C), 3);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 3));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 6);
    asserteq(lens[0], 3);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    asserteq(ids[2], 3);
    asserteq(lens[2], 2);
    /* arbiter clearing: fill 0 -> id 0, merges with neighbors */
    sp_setarbiter(t, arb_notifypad, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 0, 3), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 0);
    asserteq(lens[0], 3);
    /* one arbiter call per covered segment */
    sp_freetree(t);
    t = sp_newtree(S);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    assertok(sp_checktree(t));
    sp_setarbiter(t, arb_counting, NULL);
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_fill(&C, 9, 6), SP_OK);
    assertok(sp_checktree(t));
    asserteq(arb_count, 2);
    sp_freetree(t), sp_close(S);
}

TEST(fill_virtual) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    /* fully virtual: pad + color are both id0, no arbiter event */
    sp_setarbiter(t, arb_notifypad, NULL);
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_fill(&C, 0, 3), SP_OK);
    asserteq(arb_notify, 0);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 8);
    /* virtual fill with color: [8,13) -> 7 (bare overwrite) */
    sp_setarbiter(t, NULL, NULL);
    asserteq(sp_seek(&C, t, 8), SP_OK);
    asserteq(sp_fill(&C, 7, 5), SP_OK);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 13));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 0);
    asserteq(lens[0], 8);
    asserteq(ids[1], 7);
    asserteq(lens[1], 5);
    /* R virtual: real prefix colored, virtual run appended */
    sp_freetree(t);
    t = sp_newtree(S);
    sp_setarbiter(t, NULL, NULL);
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spN_setcc(&t->root, 2), t->bytes = 8;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_fill(&C, 9, 8), SP_OK);
    asserteq(sp_offset(&C), 10);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 10));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    asserteq(ids[1], 9);
    asserteq(lens[1], 8);
    sp_freetree(t), sp_close(S);
}

/* build a two-level tree: root -> n0 -> leaves l0..l2 */
static void mk2level(
        sp_State *S, sp_Tree *t, sp_Node **l0, sp_Node **l1, sp_Node **l2) {
    sp_Node *n0, *lf;
    n0 = (sp_Node *)spP_alloc(S, &S->nodes);
    *l0 = lf = (sp_Node *)spP_alloc(S, &S->nodes);
    memset(lf, 0, sizeof(sp_Node));
    spL_setid(lf, 0, 1), lf->bytes[0] = 2;
    spL_setid(lf, 1, 2), lf->bytes[1] = 3;
    spN_setcc(lf, 2);
    *l1 = lf = (sp_Node *)spP_alloc(S, &S->nodes);
    memset(lf, 0, sizeof(sp_Node));
    spL_setid(lf, 0, 3), lf->bytes[0] = 1;
    spL_setid(lf, 1, 4), lf->bytes[1] = 4;
    spN_setcc(lf, 2);
    *l2 = lf = (sp_Node *)spP_alloc(S, &S->nodes);
    memset(lf, 0, sizeof(sp_Node));
    spL_setid(lf, 0, 5), lf->bytes[0] = 2;
    spL_setid(lf, 1, 6), lf->bytes[1] = 1;
    spN_setcc(lf, 2);
    memset(n0, 0, sizeof(sp_Node));
    n0->children[0] = *l0, n0->children[1] = *l1, n0->children[2] = *l2;
    n0->bytes[0] = 5, n0->bytes[1] = 5, n0->bytes[2] = 3;
    spN_setcc(n0, 3);
    t->root.children[0] = n0, t->root.bytes[0] = 13;
    spN_setcc(&t->root, 1), t->levels = 2, t->bytes = 13;
}

TEST(fill_multi) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[32];
    size_t    lens[32];
    sp_Node  *l0, *l1, *l2;
    int       n;
    mk2level(S, t, &l0, &l1, &l2);
    assertok(sp_checktree(t));
    /* cross-leaf fill [1,9): l0 tail, whole l1 head/mid, l2 head -> 7;
     * segs sit at [0,2)[2,5)[5,6)[6,10)[10,12)[12,13) */
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_fill(&C, 7, 8), SP_OK);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 9));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 5);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 7);
    asserteq(lens[1], 8);
    asserteq(ids[2], 4);
    asserteq(lens[2], 1);
    asserteq(ids[3], 5);
    asserteq(lens[3], 2);
    asserteq(ids[4], 6);
    asserteq(lens[4], 1);
    /* whole-tree fill -> single run (the tree shrinks to one level) */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 8, 13), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 8);
    asserteq(lens[0], 13);
    /* edits after fill still work: remove a tail chunk */
    {
        sp_Cursor R;
        asserteq(sp_seek(&C, t, 5), SP_OK);
        asserteq(sp_seek(&R, t, 8), SP_OK);
        asserteq(sp_remove(&C, &R), SP_OK);
        assertok(sp_checktree(t));
        collect_stream(&C, ids, lens, &n);
        asserteq(n, 1);
        asserteq(ids[0], 8);
        asserteq(lens[0], 10);
    }
    sp_freetree(t), sp_close(S);
}

/* arbiter that keeps the old id: fill recolors nothing, every peeled
 * segment absorbs into its left neighbor */
static sp_Id arb_same(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud, (void)id, (void)mask;
    return old;
}

/* fill across leaf containers on a full 2-level tree (fl > 0):
 * poff==0 border, same-id absorb path, and a recolor that restores
 * the full structure; byte counts and live objects stay conserved */
TEST(fill_right) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t;
    sp_Cursor C;
    sp_Node  *want;
    size_t    live0;
    /* full tree: root -> n0 (4 children) -> 4 leaf containers, each
     * with 4 one-byte segments; fl = 1 for [2, 8) */
    t = treeV(
            2, innerV(
                       innerV(leafV(1, 1, 2, 1, 3, 1, 4, 1),
                              leafV(5, 1, 6, 1, 7, 1, 8, 1),
                              leafV(9, 1, 10, 1, 11, 1, 12, 1),
                              leafV(13, 1, 14, 1, 15, 1, 16, 1))));
    assertok(sp_checktree(t));
    /* same-id arbiter: peel + absorb restores the segment stream; the
     * stitch ends with shrink-root (rebalance folds a single-child
     * root), so the full 2-level tree settles at 1 level, live - 1 */
    sp_setarbiter(t, arb_same, NULL);
    asserteq(sp_seek(&C, t, 2), SP_OK); /* poff == 0 at a segment head */
    live0 = S->nodes.live_obj;
    asserteq(sp_fill(&C, 9, 6), SP_OK);
    asserteq(sp_offset(&C), 8);
    asserteq(sp_bytes(t), 16);
    asserteq(S->nodes.live_obj, live0 - 1);
    sp_asserttree(
            t, 1,
            innerV(leafV(1, 1, 2, 1, 3, 1, 4, 1), leafV(5, 1, 6, 1, 7, 1, 8, 1),
                   leafV(9, 1, 10, 1, 11, 1, 12, 1),
                   leafV(13, 1, 14, 1, 15, 1, 16, 1)));
    assertok(sp_checkcursor(&C, 8));
    assertok(sp_checktree(t));
    /* sum arbiter: [1, 12) recolors eleven segments; the tree stays
     * 1 level (root keeps 4 children, no second shrink) */
    sp_setarbiter(t, arb_sum, NULL);
    asserteq(sp_seek(&C, t, 1), SP_OK);
    live0 = S->nodes.live_obj;
    asserteq(sp_fill(&C, 20, 11), SP_OK);
    asserteq(sp_bytes(t), 16);
    asserteq(S->nodes.live_obj, live0);
    want = innerV(
            leafV(1, 1, 22, 1, 23, 1, 24, 1), leafV(25, 1, 26, 1, 27, 1, 28, 1),
            leafV(29, 1, 30, 1, 31, 1, 32, 1),
            leafV(13, 1, 14, 1, 15, 1, 16, 1));
    sp_asserttree(t, 1, want);
    assertok(sp_checkcursor(&C, 12));
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* append across a right container border on a 3-level tree: the merge
 * climbs past the fork level (multi-level byte fixup loop) */
TEST(merge_right_deep) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(
                       innerV(innerV(leafV(5, 1), leafV(1, 1, 2, 1)),
                              innerV(leafV(2, 1, 9, 1), leafV(7, 1)))));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 6); /* grow left: the previous 2-run absorbs the append */
    asserteq(ids[0], 5);
    asserteq(lens[0], 1);
    asserteq(ids[1], 1);
    asserteq(lens[1], 1);
    asserteq(ids[2], 2);
    asserteq(lens[2], 2);
    asserteq(ids[3], 2);
    asserteq(lens[3], 1);
    asserteq(ids[4], 9);
    asserteq(lens[4], 1);
    asserteq(ids[5], 7);
    asserteq(lens[5], 1);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* insert inheriting the in-leaf id merges inside the container; the
 * left neighbor keeps its run across the container seam (in-leaf only) */
TEST(mergeleft_shell) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(1, 1)),
                      innerV(leafV(2, 2), leafV(3, 1))));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_insert(&C, 1), SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 1);
    asserteq(lens[1], 2);
    asserteq(ids[2], 2);
    asserteq(lens[2], 2);
    asserteq(ids[3], 3);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* append inside a segment merges the in-leaf neighbors (fillrt rt-merge
 * + seamleaf); the other container keeps its run across the seam */
TEST(mergeright_shell) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 2), leafV(1, 1)));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 1);
    asserteq(lens[1], 1);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    /* neighbor with siblings: the fork keeps cc >= 3, no fold */
    t = treeV(1, innerV(leafV(1, 2), leafV(1, 1), leafV(2, 1)));
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 1);
    asserteq(lens[1], 1);
    asserteq(ids[2], 2);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* API boundary paths: NULL params, OOM construction, and the leaf
 * container edge cases of next/prev/advance with a NULL plen */
TEST(api_param) {
    int       cnt = 0;
    sp_State *S, *S2;
    sp_Tree  *t;
    sp_Cursor C, R, B;
    size_t    len;
    /* construction OOM and NULL paths */
    asserteq(sp_open(&oom_alloc, &cnt), NULL);
    sp_close(NULL);
    cnt = 1;
    S2 = sp_open(&oom_alloc, &cnt);
    assertok(S2 != NULL);
    cnt = 0;
    asserteq(sp_newtree(S2), NULL);
    sp_close(S2);
    S = sp_open(NULL, NULL);
    assertok(S != NULL);
    sp_freetree(NULL);
    sp_setarbiter(NULL, NULL, NULL);
    /* tree and cursor param checks */
    t = sp_newtree(S), assert(t);
    assertok(t != NULL);
    asserteq(sp_seek(NULL, t, 0), SP_ERRPARAM);
    asserteq(sp_seek(&C, NULL, 0), SP_ERRPARAM);
    memset(&B, 0, sizeof(sp_Cursor));
    asserteq(sp_advance(NULL, 1), SP_ERRPARAM);
    asserteq(sp_advance(&B, 1), SP_ERRPARAM);
    asserteq(sp_style(NULL, &len, NULL), SP_NONE);
    asserteq(sp_next(NULL, 0, &len), SP_NONE);
    asserteq(sp_prev(NULL, 0, &len), SP_NONE);
    asserteq(sp_remove(NULL, &R), SP_ERRPARAM);
    asserteq(sp_remove(&C, NULL), SP_ERRPARAM);
    asserteq(sp_remove(&C, &B), SP_ERRPARAM);
    asserteq(sp_remove(&B, &C), SP_ERRPARAM);
    asserteq(sp_append(NULL, 1), SP_ERRPARAM);
    asserteq(sp_insert(NULL, 1), SP_ERRPARAM);
    asserteq(sp_splice(NULL, 0, 1), SP_ERRPARAM);
    asserteq(sp_fill(NULL, 1, 1), SP_ERRPARAM);
    asserteq(sp_fill(&B, 1, 1), SP_ERRPARAM);
    sp_freetree(t), sp_close(S);
}

/* traversal edge states: next/prev/advance at segment tails, the tree
 * tail and virtual positions, always also with a NULL plen */
TEST(traversal_edges) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 2, 2, 2), leafV(3, 2, 4, 2)));
    sp_Cursor C;
    assertok(sp_checktree(t));
    /* advance 0 is idempotent; on an empty tree advance enters the
     * virtual state like seek (advance is a relative seek) */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_advance(&C, 0), SP_OK);
    asserteq(sp_offset(&C), 0);
    {
        sp_Tree *e = sp_newtree(S);
        asserteq(sp_seek(&C, e, 0), SP_OK);
        asserteq(sp_advance(&C, 5), SP_OK);
        asserteq(sp_offset(&C), 5);
        asserteq(sp_advance(&C, -2), SP_OK);
        asserteq(sp_offset(&C), 3);
        asserteq(sp_advance(&C, -10), SP_OK);
        asserteq(sp_offset(&C), 0);
        sp_freetree(e);
    }
    /* sp_style with a NULL plen: inside a segment and at the tail */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    assertok(sp_style(&C, NULL, NULL) != 0);
    asserteq(sp_seek(&C, t, 8), SP_OK);
    asserteq(sp_style(&C, NULL, NULL), SP_NONE);
    /* sp_next with NULL plen: segment tail, tree tail, last-segment
     * tail (the l < 0 climb-out) */
    asserteq(sp_seek(&C, t, 1), SP_OK);
    assertok(sp_next(&C, 0, NULL) != 0);
    asserteq(sp_offset(&C), 2);
    asserteq(sp_seek(&C, t, 8), SP_OK);
    asserteq(sp_next(&C, 0, NULL), SP_NONE);
    asserteq(sp_seek(&C, t, 7), SP_OK);
    asserteq(sp_next(&C, 0, NULL), SP_NONE);
    asserteq(sp_offset(&C), 8);
    /* sp_prev with NULL plen: mid-segment, tree head, successful */
    asserteq(sp_seek(&C, t, 6), SP_OK);
    assertok(sp_prev(&C, 0, NULL) != 0);
    asserteq(sp_offset(&C), 4);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_prev(&C, 0, NULL), SP_NONE);
    asserteq(sp_seek(&C, t, 8), SP_OK);
    assertok(sp_prev(&C, 0, NULL) != 0);
    asserteq(sp_offset(&C), 6);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* splice into an empty tree and splice that empties the whole tree
 * before re-inserting (onepiece paths in sp_splice) */
TEST(splice_edges) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_splice(&C, 0, 3), SP_OK);
    asserteq(sp_bytes(t), 3);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 3);
    /* wipe the whole tree then insert: remove empties, then onepiece */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_splice(&C, 3, 2), SP_OK);
    asserteq(sp_bytes(t), 2);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 2);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* remaining cursor-bound param checks: a cursor whose tree pointer is
 * NULL (construction checks only), and remove across two trees */
TEST(api_param2) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Tree  *t2 = sp_newtree(S);
    sp_Cursor C, R, B;
    size_t    len;
    memset(&B, 0, sizeof(sp_Cursor));
    asserteq(sp_style(&B, &len, NULL), SP_NONE);
    asserteq(sp_next(&B, 0, &len), SP_NONE);
    asserteq(sp_prev(&B, 0, &len), SP_NONE);
    asserteq(sp_append(&B, 1), SP_ERRPARAM);
    asserteq(sp_insert(&B, 1), SP_ERRPARAM);
    asserteq(sp_splice(&B, 0, 1), SP_ERRPARAM);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_seek(&R, t2, 0), SP_OK);
    asserteq(sp_remove(&C, &R), SP_ERRPARAM);
    /* remove with the L cursor at the tree tail clamps to nothing */
    asserteq(sp_seek(&C, t2, 0), SP_OK);
    asserteq(sp_append(&C, 4), SP_OK);
    asserteq(sp_seek(&C, t2, 4), SP_OK);
    asserteq(sp_seek(&R, t2, 6), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    asserteq(sp_bytes(t2), 4);
    /* both cursors beyond the tree end: the offset guard fires */
    asserteq(sp_seek(&C, t2, 8), SP_OK);
    asserteq(sp_seek(&R, t2, 10), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    asserteq(sp_bytes(t2), 4);
    sp_freetree(t2), sp_freetree(t), sp_close(S);
}

/* inheritance edge states: append at a container-head segment boundary
 * climbs to the previous container's last segment; insert at a mid-tree
 * edit tail (poff == len) inherits the right segment; the tree tail has
 * no right neighbor (id 0) */
TEST(inherit_edges) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 2, 2, 2), leafV(3, 2)));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    assertok(sp_checktree(t));
    /* append at the head of a container's first segment grows the left
     * container's last segment; then insert at the edit tail
     * (poff == len, mid-tree): the right segment lives in the next
     * container, the insert merges into it */
    asserteq(sp_seek(&C, t, 4), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    asserteq(sp_insert(&C, 1), SP_OK);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 5));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    asserteq(ids[1], 2);
    asserteq(lens[1], 3);
    asserteq(ids[2], 3);
    asserteq(lens[2], 3);
    /* multi-level edit tail: the append grows the left container's last
     * segment and the insert grows the current right segment */
    sp_freetree(t);
    t = treeV(1, innerV(leafV(1, 2), leafV(2, 2, 4, 2, 5, 2)));
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    asserteq(sp_insert(&C, 1), SP_OK);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 3));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 2);
    asserteq(lens[1], 3);
    asserteq(ids[2], 4);
    asserteq(lens[2], 2);
    asserteq(ids[3], 5);
    asserteq(lens[3], 2);
    /* same-container edit tail: the right segment is the next slot */
    sp_freetree(t);
    t = treeV(0, leafV(1, 3, 2, 2, 3, 2));
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK); /* cursor at poff == len of [1(4)] */
    asserteq(sp_offset(&C), 4);
    asserteq(sp_insert(&C, 1), SP_OK);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 4));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 4);
    asserteq(ids[1], 2);
    asserteq(lens[1], 3);
    asserteq(ids[2], 3);
    asserteq(lens[2], 2);
    /* insert at the tree tail (poff == len): no right neighbor, the
     * inserted run inherits id 0 */
    asserteq(sp_seek(&C, t, 9), SP_OK);
    asserteq(sp_insert(&C, 2), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 4);
    asserteq(ids[1], 2);
    asserteq(lens[1], 3);
    asserteq(ids[2], 3);
    asserteq(lens[2], 2);
    asserteq(ids[3], 0);
    asserteq(lens[3], 2);
    sp_freetree(t), sp_close(S);
}

/* splice whose remove step runs out of memory: the first reserve
 * succeeds, the remove's deeper reserve must fail and surface
 * SP_ERRMEM (del > 0 && r != SP_OK path in sp_splice) */
TEST(splice_oom) {
    OomCount  oc = {0, 1 << 20};
    sp_State *S = sp_open(&oomcount_alloc, &oc);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Drain  d;
    size_t    keep;
    assertok(t != NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    asserteq(sp_append(&C, 20), SP_OK);
    assertok(sp_checktree(t));
    d = sp_drainpool(&S->nodes);
    keep = 3 * (size_t)t->levels + 4;
    {
        size_t i;
        void  *cur = d.chain;
        for (i = 0; i < keep && cur; ++i) cur = *(void **)cur;
        if (cur) *(void **)cur = NULL; /* detach the tail we keep */
        S->nodes.freed = d.chain, S->nodes.freed_obj = keep;
    }
    oc.oom = 0;
    asserteq(sp_seek(&C, t, 10), SP_OK);
    asserteq(sp_splice(&C, 5, 3), SP_ERRMEM);
    sp_freetree(t), sp_close(S);
}

/* append at the tree head inherits nothing (id 0); append at the
 * tree tail (poff == len) inherits the current segment id */
TEST(inherit_head_tail) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 2, 2, 2), leafV(3, 2)));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 0);
    asserteq(lens[0], 1);
    asserteq(ids[1], 1);
    asserteq(lens[1], 2);
    asserteq(ids[2], 2);
    asserteq(lens[2], 2);
    asserteq(ids[3], 3);
    asserteq(lens[3], 2);
    /* tail append inherits the last segment */
    asserteq(sp_seek(&C, t, 7), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[3], 3);
    asserteq(lens[3], 3);
    sp_freetree(t), sp_close(S);
}

/* append at a container head grows the left seam segment directly; the
 * current container's head stays unchanged */
TEST(growleft_fold_done) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(1, 1)),
                      innerV(leafV(1, 1), leafV(1, 1), leafV(2, 1))));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 5);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 1);
    asserteq(lens[1], 2);
    asserteq(ids[2], 1);
    asserteq(lens[2], 1);
    asserteq(ids[3], 1);
    asserteq(lens[3], 1);
    asserteq(ids[4], 2);
    asserteq(lens[4], 1);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* next climbing two levels: the cursor leaves the last segment of
 * the last container under a fork node, and the down-climb loop in
 * sp_next runs twice */
TEST(next_deep) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(innerV(innerV(leafV(1, 1), leafV(2, 1)),
                             innerV(leafV(3, 1), leafV(4, 1))),
                      innerV(innerV(leafV(5, 1), leafV(6, 1)),
                             innerV(leafV(7, 1), leafV(8, 1)))));
    sp_Cursor C;
    size_t    len;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 1), SP_OK);
    assertok(sp_next(&C, 0, &len) != 0);
    asserteq(sp_offset(&C), 2);
    asserteq(sp_style(&C, &len, NULL), 3);
    asserteq(len, 1);
    assertok(sp_checktree(t));
    /* empty-tree prev */
    {
        sp_Tree  *e = sp_newtree(S);
        sp_Cursor D;
        asserteq(sp_seek(&D, e, 0), SP_OK);
        asserteq(sp_prev(&D, 0, &len), SP_NONE);
        sp_freetree(e);
    }
    sp_freetree(t), sp_close(S);
}

/* remove between two segments of the same leaf container: the cut
 * loop of spD_cutrange is skipped (fl == levels) */
TEST(remove_same_container) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(0, leafV(1, 2, 2, 1, 3, 1, 4, 2));
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&L, t, 2), SP_OK);
    asserteq(sp_seek(&R, t, 3), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    asserteq(ids[1], 3);
    asserteq(lens[1], 1);
    asserteq(ids[2], 4);
    asserteq(lens[2], 2);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* remove the last whole segment of the last container: rmleaf empties
 * the container, rebalance folds it into the left sibling, and the
 * single-child root shrinks */
TEST(rmleaf_rebalance) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 4, 2, 1), leafV(3, 1)),
                      innerV(leafV(3, 1), leafV(3, 1))));
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&L, t, 7), SP_OK);
    asserteq(sp_seek(&R, t, 8), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 4);
    asserteq(ids[1], 2);
    asserteq(lens[1], 1);
    asserteq(ids[2], 3);
    asserteq(lens[2], 1);
    asserteq(ids[3], 3);
    asserteq(lens[3], 1);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* four-level mergeleft: the fork sits at the root, the byte fixup
 * and fold loops run three levels */
TEST(mergeleft_4level) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            4, innerV(innerV(innerV(innerV(leafV(1, 1), leafV(3, 1)),
                                    innerV(leafV(3, 2), leafV(4, 1))),
                             innerV(innerV(leafV(5, 1), leafV(6, 1)),
                                    innerV(leafV(7, 1), leafV(4, 1)))),
                      innerV(innerV(innerV(leafV(4, 1), leafV(9, 1)),
                                    innerV(leafV(10, 1), leafV(11, 1))),
                             innerV(innerV(leafV(12, 1), leafV(13, 1)),
                                    innerV(leafV(14, 1), leafV(15, 1))))));
    sp_Cursor C;
    sp_Id     ids[32];
    size_t    lens[32];
    int       n;
    asserteq(sp_seek(&C, t, 9), SP_OK);
    asserteq(sp_insert(&C, 1), SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 16);
    asserteq(ids[7], 4);
    asserteq(lens[7], 1);
    asserteq(ids[8], 4);
    asserteq(lens[8], 2);
    asserteq(ids[9], 9);
    asserteq(lens[9], 1);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    /* four-level append inside a segment: the run merges in-leaf */
    t = treeV(
            4, innerV(innerV(innerV(innerV(leafV(1, 1), leafV(3, 1)),
                                    innerV(leafV(3, 2), leafV(4, 1))),
                             innerV(innerV(leafV(5, 1), leafV(6, 1)),
                                    innerV(leafV(7, 1), leafV(4, 2)))),
                      innerV(innerV(innerV(leafV(4, 1), leafV(9, 1)),
                                    innerV(leafV(10, 1), leafV(11, 1))),
                             innerV(innerV(leafV(12, 1), leafV(13, 1)),
                                    innerV(leafV(14, 1), leafV(15, 1))))));
    asserteq(sp_seek(&C, t, 9), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 16);
    asserteq(ids[7], 4);
    asserteq(lens[7], 3);
    asserteq(ids[8], 4);
    asserteq(lens[8], 1);
    asserteq(ids[9], 9);
    asserteq(lens[9], 1);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* remove that empties the middle leaf container: the L segment trims
 * to zero, stitch drops it, and the cut partner climbs left */
TEST(remove_empty_mid) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(2, 1), leafV(2, 2)),
                      innerV(leafV(2, 1), leafV(3, 1))));
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&L, t, 2), SP_OK);
    asserteq(sp_seek(&R, t, 4), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 2);
    asserteq(lens[1], 2);
    asserteq(ids[2], 3);
    asserteq(lens[2], 1);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* remove that empties a deep mid container: the stitch mergeleft
 * climbs two levels to find the cut partner */
TEST(remove_empty_deep) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(
                       innerV(innerV(leafV(1, 1), leafV(2, 1), leafV(2, 2)),
                              innerV(leafV(2, 1), leafV(3, 1)))));
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&L, t, 2), SP_OK);
    asserteq(sp_seek(&R, t, 4), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 2);
    asserteq(lens[1], 2);
    asserteq(ids[2], 3);
    asserteq(lens[2], 1);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* exhaustive fill over a four-level tree: every (pos, len) must
 * yield a valid tree and return every node to the pool */
TEST(fill_4level) {
    sp_State *S = sp_open(NULL, NULL);
    size_t    pos, len;
    for (pos = 0; pos <= 17; ++pos)
        for (len = 1; len <= 18; ++len) {
            sp_Tree *t = treeV(
                    4,
                    innerV(innerV(innerV(innerV(leafV(1, 1), leafV(3, 1)),
                                         innerV(leafV(3, 2), leafV(4, 1))),
                                  innerV(innerV(leafV(5, 1), leafV(6, 1)),
                                         innerV(leafV(7, 1), leafV(8, 1)))),
                           innerV(innerV(innerV(leafV(9, 1), leafV(10, 1)),
                                         innerV(leafV(11, 1), leafV(12, 1))),
                                  innerV(innerV(leafV(13, 1), leafV(14, 1)),
                                         innerV(leafV(15, 1), leafV(16, 1))))));
            sp_Cursor C;
            assertok(sp_checktree(t));
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_fill(&C, 7, len), SP_OK);
            assertok(sp_checktree(t));
            sp_freetree(t);
            asserteq(S->nodes.live_obj, 0);
        }
    sp_close(S);
}

/* deep mergeleft: insert at the second container's head absorbs the
 * left chain's last segment and folds the emptied shells up the chain */
TEST(mergeleft_deep) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(2, 1)),
                      innerV(leafV(2, 1), leafV(3, 1))));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_insert(&C, 1), SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 2);
    asserteq(lens[1], 1);
    asserteq(ids[2], 2);
    asserteq(lens[2], 2);
    asserteq(ids[3], 3);
    asserteq(lens[3], 1);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    /* deeper: three levels, the seam neighbors stay apart */
    t = treeV(
            3, innerV(
                       innerV(innerV(leafV(1, 1), leafV(2, 1)),
                              innerV(leafV(2, 1), leafV(3, 1)))));
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_insert(&C, 1), SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 2);
    asserteq(lens[1], 1);
    asserteq(ids[2], 2);
    asserteq(lens[2], 2);
    asserteq(ids[3], 3);
    asserteq(lens[3], 1);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* prev climbing: the cursor at a mid-tree container head whose parent
 * chain is all first-slots until the fork level */
TEST(prev_deep) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(innerV(innerV(leafV(1, 1), leafV(2, 1)),
                             innerV(leafV(2, 1), leafV(3, 1))),
                      innerV(innerV(leafV(4, 1), leafV(5, 1)),
                             innerV(leafV(6, 1), leafV(7, 1)))));
    sp_Cursor C;
    size_t    len;
    assertok(sp_checktree(t));
    /* segment 4 heads container B0, whose chain up to the fork is all
     * first slots: prev must climb two levels before descending */
    asserteq(sp_seek(&C, t, 3), SP_OK);
    assertok(sp_prev(&C, 0, &len) != 0);
    asserteq(sp_offset(&C), 2);
    asserteq(sp_style(&C, &len, NULL), 2);
    asserteq(len, 1);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* remove whose L segment sits at a container head: the L container
 * empties, the cut partner climbs to the left neighbor (mergeleft),
 * and stitch re-anchors the cursor at the container tail */
TEST(remove_head) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(0, leafV(26, 2, 22, 2, 34, 1, 26, 4));
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&L, t, 2), SP_OK);
    asserteq(sp_seek(&R, t, 5), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 26);
    asserteq(lens[0], 6);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* single-segment container removed whole at the tree tail: rmleaf
 * empties the container and rebalance folds it into the left neighbor */
TEST(remove_drain) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 2), leafV(2, 1)));
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&L, t, 2), SP_OK);
    asserteq(sp_seek(&R, t, 3), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* underfull container next to a full sibling: rebalance balances the
 * pair instead of folding (balancenode shift, foldnode break path) */
TEST(fold_balance) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            1, innerV(leafV(1, 1, 2, 1, 3, 1, 4, 1), leafV(5, 1, 6, 1)));
    sp_Cursor L, R;
    asserteq(sp_seek(&L, t, 5), SP_OK);
    asserteq(sp_seek(&R, t, 6), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    sp_asserttree(t, 1, innerV(leafV(1, 1, 2, 1), leafV(3, 1, 4, 1, 5, 1)));
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* foldnode merge path: the merged-away sibling shell must return to
 * the pool (fill_brute caught a one-node leak here) */
TEST(foldnode_free) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree *t = treeV(2, innerV(innerV(leafV(1, 1, 2, 1), leafV(3, 1, 4, 1))));
    sp_Cursor C;
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_splice(&C, 1, 0), SP_OK);
    assertok(sp_checktree(t));
    sp_asserttree(t, 0, leafV(1, 1, 3, 1, 4, 1));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* remove to the tree tail empties rt[0] (R segment trims to zero); the
 * stitch tail re-anchors the cursor at the container tail */
TEST(remove_tail) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(0, leafV(1, 2, 2, 2));
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&L, t, 2), SP_OK);
    asserteq(sp_seek(&R, t, 4), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    assertok(sp_checkcursor(&L, 2));
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* cross-container remove to the tree tail: the stitch tail re-anchors
 * the cursor at the container tail (i == cc branch) */
TEST(remove_range_tail) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 2, 2, 2), leafV(3, 2)));
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&L, t, 2), SP_OK);
    asserteq(sp_seek(&R, t, 6), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&L, 2));
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    sp_freetree(t), sp_close(S);
}

/* single-child root under rebalance: the parent cc<2 break path, then
 * shrink-root folds the level away */
TEST(rebalance_root) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 1, 2, 1)));
    sp_Cursor L, R;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    asserteq(sp_seek(&L, t, 1), SP_OK);
    asserteq(sp_seek(&R, t, 2), SP_OK);
    asserteq(sp_remove(&L, &R), SP_OK);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* ---- differential: random ops vs a naive segment-array model ---- */

typedef struct {
    size_t len;
    sp_Id  id;
} mseg_t;

#define MMAX 4096

static mseg_t msegs[MMAX];
static int    mn;
static size_t mbytes;

static unsigned mrng(unsigned s) { return s * 1103515245u + 12345u; }

/* merge adjacent same-id segments (canonical form) */
static void mmaintain(void) {
    int i, w = 0;
    for (i = 0; i < mn; ++i) {
        if (w > 0 && msegs[w - 1].id == msegs[i].id)
            msegs[w - 1].len += msegs[i].len;
        else
            msegs[w++] = msegs[i];
    }
    mn = w;
}

/* locate pos: (segment index, offset within it); pos == bytes -> mn */
static void mseek(size_t pos, int *idx, size_t *off) {
    size_t acc = 0;
    int    i;
    for (i = 0; i < mn && pos >= acc + msegs[i].len; ++i) acc += msegs[i].len;
    *idx = i, *off = pos - acc;
}

/* pad [bytes, end) as an id-0 segment (merges with an id-0 tail) */
static void mpad(size_t end) {
    size_t gap = end - mbytes;
    if (gap == 0) return;
    if (mn > 0 && msegs[mn - 1].id == 0)
        msegs[mn - 1].len += gap;
    else
        msegs[mn].len = gap, msegs[mn].id = 0, mn += 1;
    mbytes = end;
}

/* insert ins bytes at pos, inheriting the left (left=1) or right
 * segment id like the tree does (virtual positions pad first) */
static void minsert(size_t pos, size_t ins, int left) {
    int    idx;
    size_t off, tail;
    sp_Id  id, orig;
    if (ins == 0) return;
    if (pos > mbytes) mpad(pos);
    mseek(pos, &idx, &off);
    if (mn == 0)
        id = 0;
    else if (off > 0)
        id = msegs[idx].id;
    else if (left)
        id = idx > 0 ? msegs[idx - 1].id : 0;
    else
        id = idx < mn ? msegs[idx].id : 0;
    if (off > 0) {
        orig = msegs[idx].id;
        tail = msegs[idx].len - off;
        msegs[idx].len = off;
        memmove(&msegs[idx + 3], &msegs[idx + 1],
                (size_t)(mn - idx - 1) * sizeof(mseg_t));
        msegs[idx + 1].len = ins, msegs[idx + 1].id = id;
        msegs[idx + 2].len = tail, msegs[idx + 2].id = orig;
        mn += 2;
    } else {
        memmove(&msegs[idx + 1], &msegs[idx],
                (size_t)(mn - idx) * sizeof(mseg_t));
        msegs[idx].len = ins, msegs[idx].id = id;
        mn += 1;
    }
    mbytes += ins;
    mmaintain();
}

/* delete del bytes at pos (clamped); same-id neighbors merge */
static void mremove(size_t pos, size_t del) {
    mseg_t tmp[MMAX];
    size_t acc = 0;
    int    i, w = 0;
    if (del == 0 || pos >= mbytes) return;
    if (pos + del > mbytes) del = mbytes - pos;
    for (i = 0; i < mn; ++i) {
        size_t a = acc, b = acc + msegs[i].len;
        size_t lo = a > pos ? a : pos;
        size_t hi = b < pos + del ? b : pos + del;
        if (lo < hi) {
            if (lo > a) tmp[w].len = lo - a, tmp[w].id = msegs[i].id, w++;
            if (hi < b) tmp[w].len = b - hi, tmp[w].id = msegs[i].id, w++;
        } else
            tmp[w++] = msegs[i];
        acc = b;
    }
    memcpy(msegs, tmp, (size_t)w * sizeof(mseg_t));
    mn = w, mbytes -= del;
    mmaintain();
}

/* fill: run the arbiter over every covered byte range; virtual tail
 * pads first, the whole virtual run goes through one arb(0, in) */
static void mfill(size_t pos, sp_Id in, size_t len, sp_Arbiterf *arb) {
    mseg_t  tmp[MMAX];
    sp_Mask m = 0;
    size_t  acc = 0, end = pos + len;
    int     i, w = 0;
    if (len == 0) return;
    if (end > mbytes) mpad(end);
    for (i = 0; i < mn; ++i) {
        size_t a = acc, b = acc + msegs[i].len;
        size_t lo = a > pos ? a : pos;
        size_t hi = b < end ? b : end;
        if (lo < hi) {
            if (lo > a) tmp[w].len = lo - a, tmp[w].id = msegs[i].id, w++;
            tmp[w].len = hi - lo,
            tmp[w].id = (sp_Id)arb(NULL, in, msegs[i].id, &m), w++;
            if (hi < b) tmp[w].len = b - hi, tmp[w].id = msegs[i].id, w++;
        } else
            tmp[w++] = msegs[i];
        acc = b;
    }
    memcpy(msegs, tmp, (size_t)w * sizeof(mseg_t));
    mn = w;
    mmaintain();
}

static void mdump(void) {
    int i;
    test_log("MODEL bytes=%lu n=%d:", test_lu(mbytes), mn);
    for (i = 0; i < mn; ++i)
        test_log(" (%lu,%lu)", test_lu(msegs[i].len), test_lu(msegs[i].id));
    test_log("\n");
}

/* full-stream comparison: tree segments vs the model, plus checktree */
/* op history for failure replay (step, op, pos, l1, l2, in) */
static int    mops_n;
static size_t mops[4096][6];

static void mops_add(
        int step, int op, size_t pos, size_t l1, size_t l2, size_t in) {
    if (mops_n < 4096)
        mops[mops_n][0] = (size_t)step, mops[mops_n][1] = (size_t)op,
        mops[mops_n][2] = pos, mops[mops_n][3] = l1, mops[mops_n][4] = l2,
        mops[mops_n][5] = in, mops_n += 1;
}

static void mops_dump(void) {
    int i;
    test_log("OPS %d:", mops_n);
    for (i = 0; i < mops_n; ++i)
        test_log(
                " s%lu:o%lu p%lu l1%lu l2%lu in%lu", test_lu(mops[i][0]),
                test_lu(mops[i][1]), test_lu(mops[i][2]), test_lu(mops[i][3]),
                test_lu(mops[i][4]), test_lu(mops[i][5]));
    test_log("\n");
}

static void mcompare(sp_Tree *t, int step) {
    sp_Cursor C;
    sp_Id     id, lid = 0;
    size_t    len, run = 0;
    int       i = 0;
    if (!sp_checktree(t)) {
        test_log("DIFFER step %d: checktree failed\n", step);
        mops_dump();
        sp_dumptree(t, "TREE");
        mdump();
        abort();
    }
    if (sp_bytes(t) != mbytes) {
        test_log(
                "DIFFER step %d: bytes %lu != model %lu\n", step,
                test_lu(sp_bytes(t)), test_lu(mbytes));
        mops_dump();
        sp_dumptree(t, "TREE");
        mdump();
        abort();
    }
    asserteq(sp_seek(&C, t, 0), SP_OK);
    for (;;) {
        id = sp_style(&C, &len, NULL);
        if (len == 0) break;  /* segment ids can be 0: plen marks the end */
        if (id == lid && run) /* seam runs merge for the model compare */
            run += len;
        else {
            if (run && (i >= mn || lid != msegs[i].id || run != msegs[i].len)) {
                test_log(
                        "DIFFER step %d: seg %d tree=(%lu,%lu)"
                        " model=(%lu,%lu)\n",
                        step, i, test_lu(run), test_lu(lid),
                        test_lu(msegs[i].len), test_lu(msegs[i].id));
                mops_dump();
                sp_dumptree(t, "TREE");
                mdump();
                {
                    sp_Cursor D;
                    sp_Id     tid;
                    size_t    tlen;
                    int       ti = 0;
                    test_log("TREE:");
                    asserteq(sp_seek(&D, t, 0), SP_OK);
                    while ((tid = sp_style(&D, &tlen, NULL)), tlen) {
                        test_log(" (%lu,%lu)", test_lu(tlen), test_lu(tid));
                        sp_next(&D, 0, &tlen);
                        ti += 1;
                    }
                    test_log(" n=%d\n", ti);
                }
                abort();
            }
            if (run) i += 1;
            lid = id, run = len;
        }
        sp_next(&C, 0, &len);
    }
    if (run && (i >= mn || lid != msegs[i].id || run != msegs[i].len)) {
        test_log(
                "DIFFER step %d: seg %d tree=(%lu,%lu) model=(%lu,%lu)\n", step,
                i, test_lu(run), test_lu(lid), test_lu(msegs[i].len),
                test_lu(msegs[i].id));
        mops_dump();
        sp_dumptree(t, "TREE");
        mdump();
        abort();
    }
    if (run) i += 1;
    if (i != mn) {
        test_log("DIFFER step %d: %d segments != model %d\n", step, i, mn);
        mops_dump();
        sp_dumptree(t, "TREE");
        mdump();
        abort();
    }
}

/* ---- fill_brute: exhaustive (pos,len) over the 4+3+2 shape ---- */

/* arbiter that overwrites: the covered run collapses into one in-run
 * (maximal merge); arb_uniq maps every (old, in) to a value no other
 * segment can carry (deterministic, so model and tree agree) */
static sp_Id arb_overwrite(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud, (void)old, (void)mask;
    return id;
}

static sp_Id arb_uniq(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud, (void)mask;
    return old * 100 + id + 10000;
}

/* root(3) -> L0 cc {4,3,2} (first child a full 64-leaf subtree) ->
 * 9 L1 nodes cc=4 -> 36 leaf containers cc=4 -> 144 one-byte segments
 * with distinct ids */
static void mkfilltree(sp_State *S, sp_Tree *t) {
    static const int rshape[3] = {4, 3, 2};
    sp_Node         *cont[36], *l1[9], *root, *n, *lf;
    int              i, j, ii = 0;
    for (i = 0; i < 36; ++i) {
        lf = (sp_Node *)spP_alloc(S, &S->nodes);
        memset(lf, 0, sizeof(sp_Node));
        for (j = 0; j < 4; ++j)
            spL_setid(lf, j, (sp_Id)(4 * i + j + 1)), lf->bytes[j] = 1;
        spN_setcc(lf, 4), cont[i] = lf;
    }
    for (i = 0; i < 9; ++i) {
        n = (sp_Node *)spP_alloc(S, &S->nodes);
        memset(n, 0, sizeof(sp_Node));
        for (j = 0; j < 4; ++j)
            n->children[j] = cont[i * 4 + j], n->bytes[j] = 4;
        spN_setcc(n, 4), l1[i] = n;
    }
    root = (sp_Node *)spP_alloc(S, &S->nodes);
    memset(root, 0, sizeof(sp_Node));
    for (i = 0; i < 3; ++i) {
        n = (sp_Node *)spP_alloc(S, &S->nodes);
        memset(n, 0, sizeof(sp_Node));
        for (j = 0; j < rshape[i]; ++j)
            n->children[j] = l1[ii + j], n->bytes[j] = 16;
        spN_setcc(n, (unsigned short)rshape[i]), ii += rshape[i];
        root->children[i] = n, root->bytes[i] = (size_t)(16 * rshape[i]);
    }
    spN_setcc(root, 3), t->levels = 3, t->root = *root;
    spP_free(&S->nodes, root), t->bytes = 144;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
static void mserial(char *buf) {
    int i, r = 0;
    buf[0] = '\0';
    for (i = 0; i < mn; ++i)
        r += sprintf(
                buf + r, "[%lu:%lu]", test_lu(msegs[i].id),
                test_lu(msegs[i].len));
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/* every (pos, len) from a virtual pad through the tree tail, under a
 * fully-merging and a never-merging arbiter, must yield the model's
 * segment stream, a valid tree, and the cursor at pos + len */
TEST(fill_brute) {
    sp_State    *S = sp_open(NULL, NULL);
    sp_Arbiterf *arbs[2];
    char         want[4096], got[4096];
    size_t       pos, len;
    int          a, i;
    arbs[0] = arb_overwrite, arbs[1] = arb_uniq;
    for (a = 0; a < 2; ++a)
        for (pos = 0; pos <= 145; ++pos)
            for (len = 1; len <= 146; ++len) {
                sp_Tree  *t = sp_newtree(S);
                sp_Cursor C;
                SpRef     bref;
                long      counts[SP_REFN];
                mkfilltree(S, t);
                mn = 144, mbytes = 144;
                for (i = 0; i < 144; ++i)
                    msegs[i].len = 1, msegs[i].id = (sp_Id)(i + 1);
                if (a == 0) {
                    memset(&bref, 0, sizeof(SpRef));
                    spA_seed(&bref, t), sp_setarbiter(t, spA_ref, &bref);
                } else
                    sp_setarbiter(t, arbs[a], NULL);
                mfill(pos, 7, len, arbs[a]);
                asserteq(sp_seek(&C, t, pos), SP_OK);
                asserteq(sp_fill(&C, 7, len), SP_OK);
                mserial(want), sp_serialtree(t, got);
                if (!sp_checktree(t) || strcmp(want, got) != 0
                    || !sp_checkcursor(&C, pos + len)) {
                    test_log(
                            "fill_brute a=%d pos=%lu len=%lu off=%lu\n", a,
                            test_lu(pos), test_lu(len), test_lu(sp_offset(&C)));
                    test_log("want=%s\ngot =%s\n", want, got);
                    sp_dumptree(t, "TREE");
                    mdump();
                    abort();
                }
                if (a == 0) {
                    spA_tally(t, counts);
                    if (!spA_check(&bref, counts)) {
                        test_log(
                                "fill_brute REF a=%d pos=%lu len=%lu\n", a,
                                test_lu(pos), test_lu(len));
                        abort();
                    }
                }
                sp_freetree(t);
                if (S->nodes.live_obj != 0) {
                    test_log(
                            "fill_brute LEAK a=%d pos=%lu len=%lu live=%lu\n",
                            a, test_lu(pos), test_lu(len),
                            test_lu(S->nodes.live_obj));
                    abort();
                }
            }
    sp_close(S);
}

TEST(differ) {
    sp_State   *S = sp_open(NULL, NULL);
    sp_Tree    *t = sp_newtree(S);
    sp_Cursor   C;
    unsigned    seed = 24680;
    const char *se = getenv("SP_DIFFER_SEED");
    int         step, maxstep = 2000;
    const char *lim = getenv("SP_DIFFER_STEPS");
    mn = 0, mbytes = 0, mops_n = 0;
    if (lim) maxstep = atoi(lim);
    if (se) seed = (unsigned)atoi(se);
    sp_setarbiter(t, arb_sum, NULL);
    for (step = 0; step < maxstep; ++step) {
        int    op = (int)(seed = mrng(seed)) % 5;
        size_t pos, l1, l2;
        sp_Id  in;
        if (mbytes > 900) {
            pos = (seed = mrng(seed)) % (mbytes / 2);
            l1 = mbytes / 2;
            mops_add(step, 9, pos, l1, 0, 0);
            mremove(pos, l1);
            asserteq(sp_seek(&C, t, pos), SP_OK);
            {
                sp_Cursor R;
                asserteq(sp_seek(&R, t, pos + l1), SP_OK);
                asserteq(sp_remove(&C, &R), SP_OK);
            }
            mcompare(t, step);
            assertok(sp_checkcursor(&C, pos));
            continue;
        }
        pos = (seed = mrng(seed)) % (mbytes + 21);
        l1 = 1 + (seed = mrng(seed)) % 12;
        l2 = 1 + (seed = mrng(seed)) % 12;
        in = (sp_Id)((seed = mrng(seed)) % 9);
        mops_add(step, op, pos, l1, l2, in);
        switch (op) {
        case 0:
            minsert(pos, l1, 1);
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_append(&C, l1), SP_OK);
            break;
        case 1:
            minsert(pos, l1, 0);
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_insert(&C, l1), SP_OK);
            break;
        case 2:
            mremove(pos, l1);
            asserteq(sp_seek(&C, t, pos), SP_OK);
            {
                sp_Cursor R;
                asserteq(sp_seek(&R, t, pos + l1), SP_OK);
                asserteq(sp_remove(&C, &R), SP_OK);
            }
            break;
        case 3:
            mremove(pos, l1);
            minsert(pos, l2, 1);
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_splice(&C, l1, l2), SP_OK);
            break;
        default:
            mfill(pos, in, l1, arb_sum);
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_fill(&C, in, l1), SP_OK);
            break;
        }
        mcompare(t, step);
        /* the cursor ends at the edit tail (append/splice/fill) or
         * stays at pos (insert/remove) */
        if (!sp_checkcursor(
                    &C, pos
                                + (op == 1 || op == 2 ? 0
                                   : op == 3          ? l2
                                                      : l1))) {
            test_log(
                    "checkcursor FAIL step=%d op=%d pos=%lu l1=%lu l2=%lu\n",
                    step, op, test_lu(pos), test_lu(l1), test_lu(l2));
            sp_dumptree(t, "differ-tree");
            abort();
        }
    }
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* ---- ns: mask ops, filtered iteration, prune-clear ---- */

TEST(ns_params) {
    sp_Mask m = 0;
    asserteq(sp_addns(&m, 1), SP_OK);
    asserteq(m, 1);
    asserteq(sp_hasns(&m, 1), 1);
    asserteq(sp_hasns(&m, 2), 0);
    asserteq(sp_addns(&m, 3), SP_OK);
    asserteq(m, 5);
    asserteq(sp_hasns(&m, 3), 1);
    asserteq(sp_addns(&m, 64), SP_OK);
    asserteq(sp_delns(&m, 3), SP_OK);
    asserteq(m, 1 + ((sp_Mask)1 << 63));
    asserteq(sp_hasns(&m, 3), 0);
    asserteq(sp_hasns(&m, 64), 1);
    asserteq(sp_delns(&m, 64), SP_OK);
    asserteq(m, 1);
    asserteq(sp_delns(&m, 1), SP_OK);
    asserteq(m, 0);
    asserteq(sp_hasns(&m, 1), 0);
    asserteq(sp_addns(NULL, 1), SP_ERRPARAM);
    asserteq(sp_addns(&m, 0), SP_ERRPARAM);
    asserteq(sp_addns(&m, -1), SP_ERRPARAM);
    asserteq(sp_addns(&m, 65), SP_ERRPARAM);
    asserteq(sp_delns(&m, 0), SP_ERRPARAM);
    asserteq(sp_delns(&m, -1), SP_ERRPARAM);
    asserteq(sp_delns(&m, 66), SP_ERRPARAM);
    asserteq(sp_delns(NULL, 1), SP_ERRPARAM);
    asserteq(sp_hasns(NULL, 1), 0);
    asserteq(sp_hasns(&m, 0), 0);
    asserteq(sp_hasns(&m, -1), 0);
    asserteq(sp_hasns(&m, 65), 0);
    asserteq(sp_addns(&m, 64), SP_OK);
    asserteq(sp_delns(&m, 64), SP_OK);
    asserteq(m, 0);
}

/* arbiter that lies: clears the id while reporting a live mask */
static sp_Id arb_liemask(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud, (void)id, (void)old;
    return *mask = 3, 0;
}

/* arbiter that keeps the old id/mask: sp_clear must still visit each
 * matching slot exactly once, even when no compaction happens */
static sp_Id arb_noop(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud, (void)id, (void)mask;
    sp_ns_calls += 1;
    return old;
}

TEST(ns_basic) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    size_t    len;
    sp_Id     id;
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 100), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 1, 20), SP_OK);
    asserteq(sp_seek(&C, t, 10), SP_OK);
    asserteq(sp_fill(&C, 2, 10), SP_OK);
    assertok(sp_checktree(t));
    /* filtered forward (exclusive): the peeked segment is consumed by
     * the caller; next skips it and prunes to the following match */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 1);
        asserteq(len, 10);
        assertok((m & 1));
    }
    asserteq((id = sp_next(&C, 1, &len)), 3);
    asserteq(len, 10);
    asserteq(sp_offset(&C), 10);
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    asserteq(len, 0);
    /* ns2 lands on the merged segment only: peek first, then next */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 1);
        asserteq(len, 10);
        assertok(!(m & 2));
    }
    asserteq((id = sp_next(&C, 2, &len)), 3);
    asserteq(len, 10);
    asserteq(sp_next(&C, 2, &len), SP_NONE);
    /* delete ns1: [10,20) keeps ns2 alone (no false positives) */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 0x8000 + 1, 100), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 0);
        asserteq(len, 10);
        assertok(!(m & 2));
    }
    asserteq((id = sp_next(&C, 2, &len)), 2);
    asserteq(len, 10);
    asserteq(sp_next(&C, 2, &len), SP_NONE);
    assertok(sp_checktree(t));
    /* id==0 axiom: a lying arbiter's mask is forced to 0 */
    sp_setarbiter(t, arb_liemask, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 9, 10), SP_OK);
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    asserteq(len, 0);
    /* the untouched ns2 segment keeps its contribution */
    asserteq(sp_seek(&C, t, 10), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 2);
        asserteq(len, 10);
        assertok((m & 2));
    }
    asserteq(sp_next(&C, 2, &len), SP_NONE);
    sp_freetree(t), sp_close(S);
}

TEST(ns_filter) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    size_t    len, pos;
    sp_Id     id;
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 30), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 1, 10), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 2, 10), SP_OK);
    asserteq(sp_seek(&C, t, 20), SP_OK);
    asserteq(sp_fill(&C, 4, 10), SP_OK);
    /* exclusive next from mid-segment: the peeked segment is consumed
     * by the caller; next skips it and prunes to the following match */
    asserteq(sp_seek(&C, t, 3), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 3);
        asserteq(len, 7);
        assertok((m & 1));
    }
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    asserteq(len, 0);
    /* prev from mid-segment on a match: plen = poff, cursor to head */
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq((id = sp_prev(&C, 1, &len)), 3);
    asserteq(len, 3);
    asserteq(sp_offset(&C), 0);
    /* prev from mid-segment on a non-match steps backward */
    asserteq(sp_seek(&C, t, 13), SP_OK);
    asserteq((id = sp_prev(&C, 2, &len)), 3);
    asserteq(len, 10);
    asserteq(sp_offset(&C), 0);
    /* out-of-domain ns = empty query: 0, plen 0, cursor unmoved */
    asserteq(sp_seek(&C, t, 8), SP_OK);
    pos = sp_offset(&C);
    asserteq(sp_next(&C, 65, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_offset(&C), pos);
    asserteq(sp_prev(&C, -1, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_offset(&C), pos);
    /* ns==0 stays unfiltered (old semantics): id-0 segments returned */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_next(&C, 0, &len), 0);
    asserteq(len, 10);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

TEST(ns_edit) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    size_t    len;
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 10), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 1, 10), SP_OK);
    /* append inherits the left segment's id and mask (merged run) */
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_append(&C, 5), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 1);
        asserteq(len, 15);
        assertok((m & 1));
    }
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    asserteq(len, 0);
    /* insert at a boundary inherits the right segment (id 0 here) */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_insert(&C, 3), SP_OK);
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 3), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 1);
        asserteq(len, 15);
        assertok((m & 1));
    }
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    asserteq(len, 0);
    /* remove keeps the mask on the surviving run */
    {
        sp_Cursor R;
        asserteq(sp_seek(&C, t, 5), SP_OK);
        asserteq(sp_seek(&R, t, 10), SP_OK);
        asserteq(sp_remove(&C, &R), SP_OK);
    }
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 0), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 1);
        asserteq(len, 13);
        assertok((m & 1));
    }
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    sp_freetree(t), sp_close(S);
}

TEST(ns_clear) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    size_t    len;
    int       i, calls;
    sp_Id     id;
    sp_setarbiter(t, spA_nscount, NULL);
    /* 180 bytes of one id-0 run, then paint every 3rd 3-byte segment
     * ns1 (20 painted segments spread across leaf containers) */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 180), SP_OK);
    for (i = 0; i < 60; i += 3) {
        asserteq(sp_seek(&C, t, (size_t)(i * 3)), SP_OK);
        asserteq(sp_fill(&C, 1, 3), SP_OK);
    }
    assertok(sp_checktree(t));
    sp_ns_calls = 0;
    asserteq(sp_clear(t, 1, 0x8000 + 1), SP_OK);
    asserteq(sp_ns_calls, 20);
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    asserteq(len, 0);
    /* a second pass finds nothing and never calls the arbiter */
    calls = sp_ns_calls;
    asserteq(sp_clear(t, 1, 0x8000 + 1), SP_OK);
    asserteq(sp_ns_calls, calls);
    /* absorb chain: [ns2][ns1][ns2] clears to one merged run; one
     * clear decision on the matching leaf plus the two compact-merge
     * deaths (arb(0, ns2)) of the run collapse */
    sp_freetree(t);
    t = sp_newtree(S);
    sp_setarbiter(t, spA_nscount, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 9), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 2, 3), SP_OK);
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq(sp_fill(&C, 3, 3), SP_OK);
    asserteq(sp_seek(&C, t, 6), SP_OK);
    asserteq(sp_fill(&C, 2, 3), SP_OK);
    sp_ns_calls = 0;
    asserteq(sp_clear(t, 1, 0x8000 + 1), SP_OK);
    asserteq(sp_ns_calls, 3);
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 0), SP_OK);
    {
        sp_Mask m;
        asserteq((id = sp_style(&C, &len, &m)), 2);
        asserteq(len, 9);
        assertok((m & 2));
    }
    asserteq(sp_next(&C, 2, &len), SP_NONE);
    /* a no-op clear (arbiter keeps the id) still visits each leaf once */
    sp_freetree(t);
    t = sp_newtree(S);
    sp_setarbiter(t, spA_nscount, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 9), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 1, 9), SP_OK);
    sp_ns_calls = 0;
    asserteq(sp_clear(t, 1, 0), SP_OK);
    asserteq(sp_ns_calls, 1);
    assertok(sp_checktree(t));
    /* parameter and empty-tree guards */
    asserteq(sp_clear(t, 0, 1), SP_ERRPARAM);
    asserteq(sp_clear(t, 65, 1), SP_ERRPARAM);
    asserteq(sp_clear(NULL, 1, 1), SP_ERRPARAM);
    sp_freetree(t), sp_close(S);
}

TEST(ns_differ) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids1[256], ids2[256];
    size_t    lens1[256], lens2[256];
    SpRef     nref;
    long      counts[SP_REFN];
    unsigned  seed = 1;
    int       i, k, step, n1, n2;
    memset(&nref, 0, sizeof(SpRef));
    sp_setarbiter(t, spA_nsref, &nref);
    for (step = 0; step < 400; ++step) {
        size_t pos, len, bytes = sp_bytes(t);
        int    op = (int)(seed = mrng(seed)) % 8;
        pos = (seed = mrng(seed)) % (bytes + 16);
        len = 1 + (seed = mrng(seed)) % 10;
        asserteq(sp_seek(&C, t, pos), SP_OK);
        if (op < 4)
            asserteq(
                    sp_fill(&C, (sp_Id)(1 + (seed = mrng(seed)) % 3), len),
                    SP_OK);
        else if (op == 4)
            asserteq(sp_append(&C, len), SP_OK);
        else if (op == 5)
            asserteq(sp_insert(&C, len), SP_OK);
        else if (op == 6)
            asserteq(sp_splice(&C, sp_min(len, bytes), 0), SP_OK);
        else {
            int ns1 = 1 + (int)((seed = mrng(seed)) % 3);
            int ns2 = 1 + (int)((seed = mrng(seed)) % 3);
            asserteq(sp_clear(t, ns1, 0x8000 + (sp_Id)ns2), SP_OK);
        }
        assertok(sp_checktree(t));
        /* fill/append land at the run tail, insert/remove at pos */
        if (op != 7) assertok(sp_checkcursor(&C, pos + (op < 5 ? len : 0)));
        spA_tally(t, counts);
        assertok(spA_check(&nref, counts));
    }
    /* differential: filtered iteration vs full scan, every ns */
    for (k = 1; k <= 3; ++k) {
        size_t len;
        n1 = sp_ns_collect(t, (sp_Mask)1 << (k - 1), ids1, lens1, 256);
        asserteq(sp_seek(&C, t, 0), SP_OK);
        n2 = 0;
        for (;;) {
            sp_Mask m;
            sp_Id   s = sp_style(&C, &len, &m);
            if (len == 0) break;
            if (m & ((sp_Mask)1 << (k - 1))) {
                assertok(n2 < 256);
                ids2[n2] = s, lens2[n2] = len, n2 += 1;
            }
            if (sp_next(&C, k, &len) == SP_NONE) break;
        }
        asserteq(n1, n2);
        for (i = 0; i < n1; ++i) {
            asserteq(ids1[i], ids2[i]);
            asserteq(lens1[i], lens2[i]);
        }
        /* backward pass: sp_prev from the tail must yield the reverse
         * stream (full lengths, one segment per call) */
        asserteq(sp_seek(&C, t, sp_bytes(t)), SP_OK);
        n2 = 0;
        while (n2 < 256) {
            sp_Id id = sp_prev(&C, k, &len);
            if (len == 0) break;
            ids2[n2] = id, lens2[n2] = len, n2 += 1;
        }
        asserteq(n1, n2);
        for (i = 0; i < n1; ++i) {
            asserteq(ids1[n1 - 1 - i], ids2[i]);
            asserteq(lens1[n1 - 1 - i], lens2[i]);
        }
    }
    sp_freetree(t), sp_close(S);
}

TEST(ns_edges) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    size_t    len;
    asserteq(sp_seek(&C, t, 0), SP_OK);
    /* NULL guards and out-of-domain ns */
    asserteq(sp_next(NULL, 1, &len), SP_NONE);
    asserteq(sp_prev(NULL, 1, &len), SP_NONE);
    asserteq(sp_style(NULL, &len, NULL), SP_NONE);
    asserteq(sp_next(NULL, 1, NULL), SP_NONE);
    asserteq(sp_prev(NULL, 1, NULL), SP_NONE);
    asserteq(sp_next(&C, -1, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_next(&C, -1, NULL), SP_NONE);
    asserteq(sp_prev(&C, -1, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_prev(&C, -1, NULL), SP_NONE);
    asserteq(sp_prev(&C, SP_MASK_BITS + 1, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_prev(&C, 0, NULL), SP_NONE);
    C.tree = NULL;
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    asserteq(sp_prev(&C, 1, &len), SP_NONE);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(sp_next(&C, 1, NULL), SP_NONE);
    asserteq(sp_prev(&C, 1, NULL), SP_NONE);
    asserteq(sp_style(&C, NULL, NULL), SP_NONE);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_append(&C, 9), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 1, 9), SP_OK);
    /* plen NULL on the peeked match; backward no-match walks to the
     * tree head */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 1);
        asserteq(len, 9);
        assertok((m & 1));
    }
    asserteq(sp_next(&C, 1, NULL), SP_NONE);
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_prev(&C, 2, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_offset(&C), 0);
    /* pruned descend past a non-matching first child */
    sp_freetree(t);
    t = sp_newtree(S);
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 9), SP_OK);
    asserteq(sp_seek(&C, t, 4), SP_OK);
    asserteq(sp_fill(&C, 1, 5), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_next(&C, 1, &len), 1);
    asserteq(len, 5);
    asserteq(sp_offset(&C), 4);
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    /* backward no-match across multiple levels */
    sp_freetree(t);
    t = sp_newtree(S);
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 150), SP_OK);
    asserteq(sp_seek(&C, t, 60), SP_OK);
    asserteq(sp_fill(&C, 2, 10), SP_OK);
    asserteq(sp_seek(&C, t, 100), SP_OK);
    asserteq(sp_prev(&C, 1, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_offset(&C), 0);
    asserteq(sp_seek(&C, t, 100), SP_OK);
    asserteq(sp_prev(&C, 1, NULL), SP_NONE);
    asserteq(sp_offset(&C), 0);
    /* filtered prev landing after skipping non-matching containers */
    sp_freetree(t);
    t = sp_newtree(S);
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 150), SP_OK);
    asserteq(sp_seek(&C, t, 20), SP_OK);
    asserteq(sp_fill(&C, 2, 10), SP_OK);
    asserteq(sp_seek(&C, t, 80), SP_OK);
    asserteq(sp_fill(&C, 1, 5), SP_OK);
    asserteq(sp_seek(&C, t, 140), SP_OK);
    asserteq(sp_prev(&C, 1, &len), 1);
    asserteq(len, 5);
    asserteq(sp_offset(&C), 80);
    /* prev descend skipping rightmost non-matching leaves in another
     * container */
    sp_freetree(t);
    t = sp_newtree(S);
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 50), SP_OK);
    asserteq(sp_seek(&C, t, 10), SP_OK);
    asserteq(sp_fill(&C, 1, 5), SP_OK);
    asserteq(sp_seek(&C, t, 15), SP_OK);
    asserteq(sp_fill(&C, 2, 5), SP_OK);
    asserteq(sp_seek(&C, t, 20), SP_OK);
    asserteq(sp_fill(&C, 4, 5), SP_OK);
    asserteq(sp_seek(&C, t, 40), SP_OK);
    asserteq(sp_prev(&C, 1, &len), 1);
    asserteq(len, 5);
    asserteq(sp_offset(&C), 10);
    /* filtered next descending through multiple skipped levels */
    sp_freetree(t);
    t = sp_newtree(S);
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_append(&C, 150), SP_OK);
    asserteq(sp_seek(&C, t, 100), SP_OK);
    asserteq(sp_fill(&C, 1, 3), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_next(&C, 1, &len), 1);
    asserteq(len, 3);
    asserteq(sp_offset(&C), 100);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* remove that fully trims L's seg and pulls the cut partner into the
 * left neighbor across the gap: the merged seg lands in the emptied
 * container (the cut side) so the cursor stays on the cut chain
 * (mergeleft regression: the cursor used to dangle after the merge) */
TEST(remove_mergeleft_gap) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(0, 42), leafV(3, 18), leafV(0, 61),
                             leafV(12, 7)),
                      innerV(leafV(9, 10), leafV(11, 5), leafV(13, 5),
                             leafV(14, 5))));
    sp_Cursor C;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 42), SP_OK);
    asserteq(sp_splice(&C, 18, 19), SP_OK);
    assertok(sp_checktree(t));
    asserteq(sp_offset(&C), 61);
    sp_serialtree(t, buf);
    assertstreq(buf, "[0:122][12:7][9:10][11:5][13:5][14:5]");
    sp_freetree(t), sp_close(S);
}

/* mergeleft whose left-neighbor chain ends underfilled at the fork's
 * first child: the chain node must fold rightward into the cursor's
 * container parent (foldnode checks the cursor child — the healthy
 * sibling — and no-ops; regression: the cc=1 survivor broke the
 * half-full invariant) */
TEST(remove_mergeleft_foldfirst) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(innerV(innerV(leafV(5, 7, 6, 170), leafV(7, 9, 2, 1152)),
                             innerV(leafV(2, 3, 2, 82), leafV(3, 142),
                                    leafV(4, 89)),
                             innerV(leafV(11, 1), leafV(12, 1))),
                      innerV(innerV(leafV(13, 1), leafV(14, 1)),
                             innerV(leafV(15, 1), leafV(16, 1)))));
    sp_Cursor C;
    char      buf[256];
    assertok(sp_checktree_allow_unseamedspan(t, 1)); /* pre-merge input */
    asserteq(sp_seek(&C, t, 1338), SP_OK);
    asserteq(sp_splice(&C, 18, 19), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 1357));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[5:7][6:170][7:9][2:1238][3:142][4:89][11:1][12:1]"
            "[13:1][14:1][15:1][16:1]");
    sp_freetree(t), sp_close(S);
}

/* mergeleft pull with rt[0] full: the ride-along pull must stop and
 * let dropleftchain fold the neighbor's survivor rightward (fuzz seed
 * 9 spN_makespace assert on the extra pull) */
TEST(remove_mergeleft_rtfull) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1, 8, 1), leafV(9, 1, 10, 1),
                             leafV(8, 2, 11, 1, 12, 1, 13, 1)),
                      innerV(leafV(14, 1, 15, 1), leafV(16, 1, 17, 1))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_seek(&R, t, 5), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 2));
    sp_serialtree(t, buf);
    assertstreq(buf, "[1:1][8:2][11:1][12:1][13:1][14:1][15:1][16:1][17:1]");
    sp_freetree(t), sp_close(S);
}

/* mergeleft whose left-neighbor tail id differs from the cut-side
 * head: no pull, the rt content stitches straight into the emptied
 * container (id-mismatch early return) */
TEST(remove_mergeleft_idmismatch) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1, 2, 1), leafV(3, 1, 40, 1)),
                      innerV(leafV(5, 1, 6, 1, 4, 1), leafV(7, 1, 8, 1),
                             leafV(9, 1, 10, 1), leafV(11, 1, 12, 1))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 4), SP_OK);
    asserteq(sp_seek(&R, t, 6), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 4));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:1][2:1][3:1][40:1][4:1][7:1][8:1][9:1][10:1]"
            "[11:1][12:1]");
    sp_freetree(t), sp_close(S);
}

/* mergeleft whose pull leaves the neighbor's leaf container healthy:
 * foldbelow stops at the first healthy chain node (cc >= FANOUT/2) */
TEST(remove_mergeleft_healthy) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(innerV(innerV(leafV(1, 1), leafV(2, 1)),
                             innerV(leafV(3, 1), leafV(4, 1, 5, 1, 6, 1))),
                      innerV(innerV(leafV(7, 1), leafV(9, 1)),
                             innerV(leafV(6, 1), leafV(8, 1)))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 6), SP_OK);
    asserteq(sp_seek(&R, t, 8), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 6));
    sp_serialtree(t, buf);
    assertstreq(buf, "[1:1][2:1][3:1][4:1][5:1][6:2][8:1]");
    sp_freetree(t), sp_close(S);
}

/* mergeleft whose fork-side survivor is underfilled and the cursor
 * container is full: foldright must balance (cN + cc > FANOUT) */
TEST(remove_mergeleft_foldrightbal) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1, 2, 1), leafV(3, 1, 4, 1)),
                      innerV(leafV(5, 1, 6, 1, 4, 1), leafV(7, 1, 8, 1),
                             leafV(9, 1, 10, 1), leafV(11, 1, 12, 1))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 4), SP_OK);
    asserteq(sp_seek(&R, t, 6), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 4));
    sp_serialtree(t, buf);
    assertstreq(buf, "[1:1][2:1][3:1][4:2][7:1][8:1][9:1][10:1][11:1][12:1]");
    sp_freetree(t), sp_close(S);
}

/* stitch whose rt[0] outgrows the cut container's free slots: findroom
 * chains a fresh leaf container for the remainder */
TEST(remove_stitch_findroom) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 10, 2, 10), leafV(3, 10, 4, 10)),
                      innerV(leafV(5, 10, 6, 10, 11, 10),
                             leafV(7, 10, 8, 10, 9, 10, 10, 10))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_seek(&R, t, 70), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 5));
    sp_serialtree(t, buf);
    assertstreq(buf, "[1:5][7:10][8:10][9:10][10:10]");
    sp_freetree(t), sp_close(S);
}

/* mergeleft whose pull stops on a full rt[0] (survivor cc=1): the
 * underfilled chain leaf folds left into its sibling below the fork */
TEST(remove_mergeleft_foldleft) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(5, 1, 6, 1, 7, 1),
                             leafV(8, 1, 9, 1)),
                      innerV(leafV(13, 1), leafV(9, 1, 10, 1, 11, 1, 12, 1),
                             leafV(14, 1), leafV(15, 1))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 6), SP_OK);
    asserteq(sp_seek(&R, t, 7), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 6));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:1][5:1][6:1][7:1][8:1][9:2][10:1][11:1][12:1]"
            "[14:1][15:1]");
    sp_freetree(t), sp_close(S);
}

/* mergeleft pull-more moves the neighbor's tail slot into rt[0] (it
 * survives the stitch): the pull must stay silent or the refcount
 * table drifts below the segment tally */
TEST(remove_mergeleft_pullref) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1, 2, 1), leafV(9, 1, 3, 1)),
                      innerV(leafV(4, 1, 5, 1), leafV(3, 1, 6, 1))));
    sp_Cursor C, R;
    SpRef     r;
    long      counts[SP_REFN];
    char      buf[256];
    memset(&r, 0, sizeof(SpRef));
    spA_seed(&r, t), sp_setarbiter(t, spA_ref, &r);
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 4), SP_OK);
    asserteq(sp_seek(&R, t, 6), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 4));
    sp_serialtree(t, buf);
    assertstreq(buf, "[1:1][2:1][9:1][3:2][6:1]");
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    sp_setarbiter(t, NULL, NULL);
    sp_freetree(t), sp_close(S);
}

/* sp_clear arbiter keeps the id but changes the mask: the aggregate
 * chain must refresh or later ns queries hit stale prunes */
typedef struct {
    int calls;
    int add; /* 1 = report ns 2, 0 = drop ns 1 */
} ClearCfg;

static sp_Id arb_clearmask(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    ClearCfg *cfg = (ClearCfg *)ud;
    (void)id;
    assertok(++cfg->calls <= 2); /* one decision per matching leaf */
    if (cfg->add)
        sp_addns(mask, 2);
    else
        sp_delns(mask, 1);
    return old;
}

TEST(clear_maskrefresh) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t;
    ClearCfg  cfg;
    sp_Cursor C;
    size_t    len;

    /* drop: cleared leaves must vanish from later ns 1 queries */
    cfg.calls = 0, cfg.add = 0;
    t = treeV(1, innerV(leafV(5, 10), leafV(6, 10)));
    sp_addns(&t->root.mask[0], 1), sp_addns(&t->root.mask[1], 1);
    sp_addns(&t->root.children[0]->mask[0], 1);
    sp_addns(&t->root.children[1]->mask[0], 1);
    sp_setarbiter(t, &arb_clearmask, &cfg);
    asserteq(sp_clear(t, 1, 7), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_next(&C, 1, &len), SP_NONE);
    asserteq(cfg.calls, 2);
    sp_setarbiter(t, NULL, NULL);
    sp_freetree(t);

    /* add: the reported ns must appear in later ns 2 queries */
    cfg.calls = 0, cfg.add = 1;
    t = treeV(1, innerV(leafV(5, 10), leafV(6, 10)));
    sp_addns(&t->root.mask[0], 1), sp_addns(&t->root.mask[1], 1);
    sp_addns(&t->root.children[0]->mask[0], 1);
    sp_addns(&t->root.children[1]->mask[0], 1);
    sp_setarbiter(t, &arb_clearmask, &cfg);
    asserteq(sp_clear(t, 1, 7), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    {
        sp_Mask m;
        asserteq(sp_style(&C, &len, &m), 5);
        asserteq(len, 10);
        assertok(m & 2);
    }
    asserteq(sp_next(&C, 2, &len), 6);
    asserteq(len, 10);
    asserteq(sp_next(&C, 2, &len), SP_NONE);
    asserteq(cfg.calls, 2);
    sp_setarbiter(t, NULL, NULL);
    sp_freetree(t);
    sp_close(S);
}

/* sp_clear prunes a whole leaf container; the resulting leaf must not
 * fall below the half-full minimum when the tree has a sibling to
 * rebalance with (piecetab commit freezes and rebalances, sp_clear
 * should do the same) */
TEST(clear_underfull) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t;
    int       i;

    t = treeV(1, innerV(leafV(1, 1, 2, 1, 3, 1, 4, 1),
                        leafV(5, 1, 6, 1, 7, 1, 8, 1)));
    sp_addns(&t->root.mask[0], 1);
    for (i = 0; i < 4; ++i) sp_addns(&t->root.children[0]->mask[i], 1);
    sp_setarbiter(t, spA_nsset, NULL);
    asserteq(sp_clear(t, 1, 0x8000 + 1), SP_OK);
    assertok(sp_checktree(t));
    for (i = 0; i < (int)t->root.child_count; ++i)
        assertok(spN_cc(t->root.children[i]) >= SP_FANOUT / 2);
    sp_setarbiter(t, NULL, NULL);
    sp_freetree(t), sp_close(S);
}

/* no-compaction clear: each matching slot is visited exactly once even
 * when several matching slots share one leaf container */
TEST(clear_nochange_multi) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(0, leafV(1, 1, 2, 1, 1, 1, 2, 1));
    sp_Cursor C;
    size_t    len;
    sp_addns(&t->root.mask[0], 1), sp_addns(&t->root.mask[2], 1);
    sp_setarbiter(t, arb_noop, NULL);
    sp_ns_calls = 0;
    asserteq(sp_clear(t, 1, 0x8000 + 1), SP_OK);
    asserteq(sp_ns_calls, 2);
    assertok(sp_checktree(t));
    /* the tree is unchanged by the no-op clear */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_style(&C, &len, NULL), 1);
    asserteq(len, 1);
    asserteq(sp_next(&C, 0, &len), 2);
    asserteq(len, 1);
    asserteq(sp_next(&C, 0, &len), 1);
    asserteq(len, 1);
    asserteq(sp_next(&C, 0, &len), 2);
    asserteq(len, 1);
    asserteq(sp_next(&C, 0, &len), SP_NONE);
    sp_setarbiter(t, NULL, NULL);
    sp_freetree(t), sp_close(S);
}

/* sp_style exact ns mask out-param: NULL skips the mask lookup */
TEST(stylemask) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Mask   m;
    size_t    len;
    /* empty tree: 0 / len 0 / mask 0 */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_style(&C, &len, &m), SP_NONE);
    asserteq(len, 0);
    asserteq(m, 0);
    /* two-segment tree with ns bits on the second segment only */
    spL_setid(&t->root, 0, 5), t->root.bytes[0] = 4;
    spL_setid(&t->root, 1, 6), t->root.bytes[1] = 3;
    spN_setcc(&t->root, 2), t->bytes = 7;
    sp_addns(&t->root.mask[1], 1), sp_addns(&t->root.mask[1], 3);
    asserteq(sp_seek(&C, t, 1), SP_OK);
    asserteq(sp_style(&C, &len, &m), 5);
    asserteq(len, 3); /* remaining, sp_style semantics */
    asserteq(m, 0);
    asserteq(sp_seek(&C, t, 6), SP_OK);
    asserteq(sp_style(&C, &len, &m), 6);
    asserteq(len, 1);
    asserteq(m, ((sp_Mask)1 << 0) | ((sp_Mask)1 << 2));
    /* tree end: 0 / len 0 / mask 0 */
    asserteq(sp_seek(&C, t, 7), SP_OK);
    asserteq(sp_style(&C, &len, &m), SP_NONE);
    asserteq(len, 0);
    asserteq(m, 0);
    /* null out-params and null cursor */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_style(&C, NULL, NULL), 5);
    asserteq(sp_style(NULL, &len, &m), SP_NONE);
    asserteq(len, 0);
    asserteq(m, 0);
    asserteq(sp_style(&C, NULL, &m), 5);
    asserteq(m, 0);
    /* locate guard arms: null cursor and a cursor without a tree */
    asserteq(sp_locate(NULL, 0), SP_ERRPARAM);
    {
        sp_Cursor z;
        memset(&z, 0, sizeof(sp_Cursor));
        asserteq(sp_locate(&z, 0), SP_ERRPARAM);
    }
    sp_freetree(t), sp_close(S);
}

/* ---- id lifecycle: three-shape arbiter refcounts ---- */

TEST(idref_fill) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C, R;
    SpRef     r;
    long      counts[SP_REFN];
    int       i;
    memset(&r, 0, sizeof(SpRef));
    assert(t), sp_setarbiter(t, spA_ref, &r);
    /* birth over uncolored: the pad notice plus arb(1, 0) */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 1, 10), SP_OK);
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[1], 1);
    /* whole overwrite: 1 dies, 2 born */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 2, 10), SP_OK);
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[1], 0);
    asserteq(r.ref[2], 1);
    /* partial k=2: [2][3][2] (protect + exit + post piece) */
    asserteq(sp_seek(&C, t, 4), SP_OK);
    asserteq(sp_fill(&C, 3, 2), SP_OK);
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[2], 2);
    asserteq(r.ref[3], 1);
    /* partial k=1 from the head: [0,1)4 keeps [1,4)2 */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 4, 1), SP_OK);
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[2], 2);
    asserteq(r.ref[4], 1);
    /* same-id early return k=2: protection + cancel net zero */
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_fill(&C, 2, 1), SP_OK);
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[2], 2);
    /* fill(0) whole clear: death */
    asserteq(sp_seek(&C, t, 4), SP_OK);
    asserteq(sp_fill(&C, 0, 2), SP_OK);
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[3], 0);
    /* remove across segments: whole deaths for 4 and 2, the 0 run is
     * silent; the surviving tail keeps 2 alive */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_seek(&R, t, 6), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checkcursor(&C, 0));
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[2], 1);
    asserteq(r.ref[4], 0);
    /* freetree: purge deaths zero everything */
    sp_freetree(t);
    for (i = 1; i < SP_REFN; ++i) asserteq(r.ref[i], 0);
    sp_close(S);
}

TEST(idref_edit) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C, R;
    SpRef     r;
    long      counts[SP_REFN];
    memset(&r, 0, sizeof(SpRef));
    assert(t), sp_setarbiter(t, spA_ref, &r);
    /* [0,5)1 [5,10)2 */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 1, 5), SP_OK);
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_fill(&C, 2, 5), SP_OK);
    /* append mid-segment: inherit splits + merges net zero */
    asserteq(sp_seek(&C, t, 3), SP_OK);
    asserteq(sp_append(&C, 3), SP_OK);
    assertok(sp_checkcursor(&C, 6));
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[1], 1);
    asserteq(r.ref[2], 1);
    /* insert mid-segment: same */
    asserteq(sp_seek(&C, t, 4), SP_OK);
    asserteq(sp_insert(&C, 2), SP_OK);
    assertok(sp_checkcursor(&C, 4));
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[1], 1);
    asserteq(r.ref[2], 1);
    /* splice with delete: [2,6) removed then 6 inherited, still net 0 */
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_splice(&C, 4, 6), SP_OK);
    assertok(sp_checkcursor(&C, 8));
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[1], 1);
    asserteq(r.ref[2], 1);
    /* remove the 1 run: whole death; the 2 tail survives */
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_seek(&R, t, 12), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checkcursor(&C, 0));
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[1], 0);
    asserteq(r.ref[2], 1);
    sp_setarbiter(t, NULL, NULL);
    sp_freetree(t), sp_close(S);
}

TEST(idref_clear) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    SpRef     r;
    long      counts[SP_REFN];
    memset(&r, 0, sizeof(SpRef));
    assert(t), sp_setarbiter(t, spA_ref, &r);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_fill(&C, 5, 10), SP_OK);
    asserteq(sp_seek(&C, t, 10), SP_OK);
    asserteq(sp_fill(&C, 6, 10), SP_OK);
    /* tag both leaves ns1 by hand: the ref arbiter passes masks through */
    sp_addns(&t->root.mask[0], 1), sp_addns(&t->root.mask[1], 1);
    asserteq(sp_clear(t, 1, 7), SP_OK);
    /* two clear decisions (5 and 6 die, 7 born twice) then the compact
     * merge of the [7][7] run dies once */
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    asserteq(r.ref[5], 0);
    asserteq(r.ref[6], 0);
    asserteq(r.ref[7], 1);
    sp_setarbiter(t, NULL, NULL);
    sp_freetree(t), sp_close(S);
}

/* random edit sequences: the three-shape refcount table must always
 * equal the tree's segment tally */
TEST(idref_differ) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    SpRef     r;
    long      counts[SP_REFN];
    unsigned  seed = 9;
    int       step;
    memset(&r, 0, sizeof(SpRef));
    sp_setarbiter(t, spA_ref, &r);
    for (step = 0; step < 500; ++step) {
        size_t pos, len, bytes = sp_bytes(t);
        int    op = (int)(seed = mrng(seed)) % 8;
        pos = (seed = mrng(seed)) % (bytes + 16);
        len = 1 + (seed = mrng(seed)) % 10;
        asserteq(sp_seek(&C, (assert(t), t), pos), SP_OK);
        if (op < 4)
            asserteq(
                    sp_fill(&C, (sp_Id)(1 + (seed = mrng(seed)) % 8), len),
                    SP_OK);
        else if (op == 4)
            asserteq(sp_append(&C, len), SP_OK);
        else if (op == 5)
            asserteq(sp_insert(&C, len), SP_OK);
        else if (op == 6)
            asserteq(sp_splice(&C, sp_min(len, bytes), len), SP_OK);
        else
            asserteq(sp_fill(&C, 0, len), SP_OK);
        assertok(sp_checktree(t));
        assertok(sp_checkcursor(&C, pos + (op == 5 ? 0 : len)));
        spA_tally(t, counts);
        assertok(spA_check(&r, counts));
    }
    sp_freetree(t);
    for (step = 1; step < SP_REFN; ++step) asserteq(r.ref[step], 0);
    sp_close(S);
}

/* SP_NONE is the end-of-iteration sentinel; 0 remains a valid id. */
TEST(sp_none) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    size_t    len;
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_style(&C, &len, NULL), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_next(&C, 0, &len), SP_NONE);
    asserteq(len, 0);
    asserteq(sp_prev(&C, 0, &len), SP_NONE);
    asserteq(len, 0);
    /* SP_NONE is forbidden as tree content */
    asserteq(sp_fill(&C, SP_NONE, 1), SP_ERRPARAM);
    asserteq(sp_clear(t, 1, SP_NONE), SP_ERRPARAM);
    /* id 0 is valid content: style/next return it, only the end is SP_NONE */
    asserteq(sp_fill(&C, 5, 2), SP_OK);
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_fill(&C, 0, 3), SP_OK);
    asserteq(sp_seek(&C, t, 0), SP_OK);
    asserteq(sp_style(&C, &len, NULL), 5);
    asserteq(len, 2);
    asserteq(sp_next(&C, 0, &len), 0);
    asserteq(len, 3);
    asserteq(sp_next(&C, 0, &len), SP_NONE);
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_style(&C, &len, NULL), 0);
    asserteq(len, 3);
    sp_freetree(t), sp_close(S);
}

#include "spantree_test_fanout4.gen.inc"
