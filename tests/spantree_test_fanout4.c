#define SP_FANOUT    4
#define SP_PAGE_SIZE 512
#define SP_STATIC_API
#ifndef SP_POOL_STATS
# define SP_POOL_STATS
#endif

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
        assertok(sp_bytes(t) == 0);
        assertok(t->levels == 0);
        assertok(sp_checktree(t));
        sp_freetree(t);
        assertok(S->nodes.live_obj == 0);
        sp_close(S);
    }
    assertok(sp_bytes(NULL) == 0);
    assertok(sp_newtree(NULL) == NULL);
}

/* reserve failure: drained pool + failing page alloc must surface
 * SP_ERRMEM and leave the tree untouched (transactional reserve) */
TEST(reserve_oom) {
    int       cnt = 1 << 20;
    sp_State *S = sp_open(&oom_alloc, &cnt);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C, R;
    sp_Drain  d;
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_append(&C, 16) == SP_OK);
    d = sp_drainpool(&S->nodes);
    cnt = 0;
    assertok(sp_append(&C, 1) == SP_ERRMEM);
    assertok(sp_insert(&C, 1) == SP_ERRMEM);
    assertok(sp_splice(&C, 1, 1) == SP_ERRMEM);
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_seek(&R, t, 3) == SP_OK);
    assertok(sp_remove(&C, &R) == SP_ERRMEM);
    assertok(sp_fill(&C, 9, 2) == SP_ERRMEM);
    assertok(sp_bytes(t) == 16);
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
    assertok(spN_sumbytes(&a, 1, 3) == 5);
    assertok(spN_sumbytes(&a, 0, 4) == 10);
    assertok(spN_sumbytes(&a, 3, 3) == 0);
    /* copy: slot range, children+bytes travel together, cc untouched */
    memset(&b, 0, sizeof(b));
    spN_copy(&b, 1, &a, 0, 2);
    assertok(spN_cc(&b) == 0);
    assertok(spL_id(&b, 1) == 10 && b.bytes[1] == 1);
    assertok(spL_id(&b, 2) == 11 && b.bytes[2] == 2);
    /* move: overlapping shift within one node */
    spN_move(&a, 1, 0, 3);
    assertok(spL_id(&a, 1) == 10 && a.bytes[1] == 1);
    assertok(spL_id(&a, 3) == 12 && a.bytes[3] == 3);
    /* makespace: open n slots at i, cc grows */
    spN_setcc(&a, 2);
    spN_makespace(&a, 1, 2);
    assertok(spN_cc(&a) == 4);
    assertok(spL_id(&a, 0) == 10 && spL_id(&a, 3) == 10 && a.bytes[3] == 1);
    /* remove: close n slots at i, cc shrinks */
    spN_remove(&a, 1, 2);
    assertok(spN_cc(&a) == 2);
    assertok(spL_id(&a, 0) == 10 && spL_id(&a, 1) == 10 && a.bytes[1] == 1);
    /* spL_setid round-trip, id 0 = NULL slot */
    spL_setid(&a, 0, 0);
    assertok(spL_id(&a, 0) == 0);
    spL_setid(&a, 0, 7);
    assertok(spL_id(&a, 0) == 7);
}

/* cursor seek: empty tree, segment boundaries, virtual excess beyond bytes */
TEST(seek) {
    sp_State *S;
    sp_Tree  *t;
    sp_Cursor C;
    size_t    len;
    S = sp_open(NULL, NULL);
    t = sp_newtree(S);
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_style(&C, &len) == NULL && len == 0);
    assertok(sp_offset(&C) == 0);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 0));
    assertok(sp_seek(&C, t, 5) == SP_OK);
    assertok(sp_style(&C, &len) == NULL && len == 0);
    assertok(C.off == 5 && sp_offset(&C) == 5);
    assertok(sp_checkcursor(&C, 5));
    /* leaf-only tree [3,5,2] */
    spL_setid(&t->root, 0, 1), t->root.bytes[0] = 3;
    spL_setid(&t->root, 1, 2), t->root.bytes[1] = 5;
    spL_setid(&t->root, 2, 3), t->root.bytes[2] = 2;
    spN_setcc(&t->root, 3), t->bytes = 10;
    assertok(sp_checktree(t));
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_offset(&C) == 0 && *sp_style(&C, &len) == 1 && len == 3);
    assertok(sp_checkcursor(&C, 0));
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_offset(&C) == 2 && *sp_style(&C, &len) == 1 && len == 1);
    assertok(sp_checkcursor(&C, 2));
    assertok(sp_seek(&C, t, 3) == SP_OK);
    assertok(sp_offset(&C) == 3 && *sp_style(&C, &len) == 2 && len == 5);
    assertok(sp_checkcursor(&C, 3));
    assertok(sp_seek(&C, t, 8) == SP_OK);
    assertok(sp_offset(&C) == 8 && *sp_style(&C, &len) == 3 && len == 2);
    assertok(sp_checkcursor(&C, 8));
    assertok(sp_seek(&C, t, 10) == SP_OK);
    assertok(sp_offset(&C) == 10 && sp_style(&C, &len) == NULL && len == 0);
    assertok(sp_checkcursor(&C, 10));
    assertok(sp_seek(&C, t, 12) == SP_OK);
    assertok(sp_style(&C, &len) == NULL && len == 0);
    assertok(C.off == 12 && sp_offset(&C) == 12);
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
    leaf0 = (sp_Node *)S->allocf(S->alloc_ud, NULL, 0, sizeof(sp_Node));
    leaf1 = (sp_Node *)S->allocf(S->alloc_ud, NULL, 0, sizeof(sp_Node));
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(*sp_next(&C, &len) == 11 && len == 3 && sp_offset(&C) == 2);
    assertok(sp_checkcursor(&C, 2));
    assertok(*sp_next(&C, &len) == 12 && len == 1 && sp_offset(&C) == 5);
    assertok(*sp_next(&C, &len) == 13 && len == 4 && sp_offset(&C) == 6);
    assertok(sp_checkcursor(&C, 6));
    assertok(sp_next(&C, &len) == NULL && len == 0 && sp_offset(&C) == 10);
    assertok(sp_checkcursor(&C, 10));
    assertok(sp_next(&C, &len) == NULL && len == 0);
    /* prev: across segments and leaf containers, NULL at head */
    assertok(sp_seek(&C, t, 10) == SP_OK);
    assertok(*sp_prev(&C, &len) == 13 && len == 4 && sp_offset(&C) == 6);
    assertok(sp_checkcursor(&C, 6));
    assertok(*sp_prev(&C, &len) == 12 && len == 1 && sp_offset(&C) == 5);
    assertok(*sp_prev(&C, &len) == 11 && len == 3 && sp_offset(&C) == 2);
    assertok(*sp_prev(&C, &len) == 10 && len == 2 && sp_offset(&C) == 0);
    assertok(sp_checkcursor(&C, 0));
    assertok(sp_prev(&C, &len) == NULL && len == 0);
    /* prev inside a segment rewinds to its head */
    assertok(sp_seek(&C, t, 7) == SP_OK);
    assertok(*sp_prev(&C, &len) == 13 && len == 1 && sp_offset(&C) == 6);
    assertok(sp_checkcursor(&C, 6));
    assertok(*sp_prev(&C, &len) == 12 && len == 1 && sp_offset(&C) == 5);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_advance(&C, 2) == SP_OK);
    assertok(sp_offset(&C) == 2 && *sp_style(&C, &len) == 1 && len == 1);
    assertok(sp_checkcursor(&C, 2));
    assertok(sp_advance(&C, 1) == SP_OK);
    assertok(sp_offset(&C) == 3 && *sp_style(&C, &len) == 2 && len == 5);
    assertok(sp_advance(&C, 5) == SP_OK);
    assertok(sp_offset(&C) == 8 && *sp_style(&C, &len) == 3 && len == 2);
    assertok(sp_advance(&C, 2) == SP_OK);
    assertok(sp_offset(&C) == 10 && sp_style(&C, &len) == NULL && len == 0);
    assertok(sp_checkcursor(&C, 10));
    assertok(sp_advance(&C, 3) == SP_OK);
    assertok(sp_offset(&C) == 13 && sp_style(&C, &len) == NULL && len == 0);
    assertok(sp_checkcursor(&C, 13));
    /* backward from virtual: stay virtual, then reenter the tree */
    assertok(sp_advance(&C, -1) == SP_OK);
    assertok(sp_offset(&C) == 12 && sp_style(&C, &len) == NULL && len == 0);
    assertok(sp_checkcursor(&C, 12));
    assertok(sp_locate(&C, 13) == SP_OK);
    assertok(sp_offset(&C) == 13 && sp_style(&C, &len) == NULL && len == 0);
    assertok(sp_advance(&C, -4) == SP_OK);
    assertok(sp_offset(&C) == 9 && *sp_style(&C, &len) == 3 && len == 1);
    assertok(sp_checkcursor(&C, 9));
    /* backward: within segment, across segment, clamp to head */
    assertok(sp_seek(&C, t, 9) == SP_OK);
    assertok(sp_advance(&C, -1) == SP_OK);
    assertok(sp_offset(&C) == 8 && *sp_style(&C, &len) == 3 && len == 2);
    assertok(sp_advance(&C, -8) == SP_OK);
    assertok(sp_offset(&C) == 0 && *sp_style(&C, &len) == 1 && len == 3);
    assertok(sp_checkcursor(&C, 0));
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_advance(&C, -5) == SP_OK);
    assertok(sp_offset(&C) == 0);
    sp_freetree(t), sp_close(S);
}

/* ---- insertion: append/insert, inheritance, cursor motion ---- */

/* collect segment stream: ids then lens, from cursor to tree end */
static void collect_stream(sp_Cursor *C, sp_Id *ids, size_t *lens, int *n) {
    size_t       len;
    const sp_Id *id;
    *n = 0;
    sp_seek(C, C->tree, 0);
    while ((id = sp_style(C, &len)) != NULL) {
        ids[*n] = *id, lens[*n] = len, *n += 1;
        sp_next(C, &len);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_append(&C, 5) == SP_OK);
    asserteq(sp_bytes(t), 5);
    assertok(sp_offset(&C) == 5);
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
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_append(&C, 2) == SP_OK);
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
    assertok(sp_seek(&C, t, 3) == SP_OK);
    assertok(sp_append(&C, 4) == SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 7);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    /* tail append inherits last segment */
    assertok(sp_seek(&C, t, 12) == SP_OK);
    assertok(sp_append(&C, 2) == SP_OK);
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
    assertok(sp_seek(&C, t, 3) == SP_OK);
    assertok(sp_insert(&C, 4) == SP_OK);
    assertok(sp_offset(&C) == 3);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 3));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 2);
    asserteq(lens[1], 9);
    /* no-op */
    assertok(sp_append(&C, 0) == SP_OK);
    assertok(sp_insert(&C, 0) == SP_OK);
    assertok(sp_bytes(t) == 12);
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
    assertok(sp_seek(&C, t, 5) == SP_OK);
    assertok(sp_append(&C, 3) == SP_OK);
    asserteq(sp_bytes(t), 8);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 8));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 8);
    /* virtual insert pads and inherits right (none at tail) */
    assertok(sp_seek(&C, t, 12) == SP_OK);
    assertok(sp_insert(&C, 2) == SP_OK);
    asserteq(sp_bytes(t), 14);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 14);
    /* mid-tree edit after pad stays correct */
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
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
    assertok(sp_seek(&L, t, 1) == SP_OK);
    assertok(sp_seek(&R, t, 2) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
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
    assertok(sp_checktree(t));
    assertok(sp_seek(&L, t, 3) == SP_OK);
    assertok(sp_seek(&R, t, 8) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 2);
    asserteq(lens[1], 4);
    /* remove whole tree */
    assertok(sp_seek(&L, t, 0) == SP_OK);
    assertok(sp_seek(&R, t, 7) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
    asserteq(sp_bytes(t), 0);
    assertok(sp_checktree(t));
    sp_seek(&L, t, 0);
    {
        size_t zl;
        assertok(sp_style(&L, &zl) == NULL);
    }
    /* no-op and cross-tree errors */
    assertok(sp_seek(&L, t, 0) == SP_OK);
    assertok(sp_remove(&L, &L) == SP_OK);
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
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_splice(&C, 2, 3) == SP_OK);
    asserteq(sp_bytes(t), 9);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 4);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    /* splice across segments */
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_splice(&C, 4, 1) == SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 0);
    asserteq(lens[0], 1);
    asserteq(ids[1], 2);
    asserteq(lens[1], 5);
    /* tail splice pads the gap */
    assertok(sp_seek(&C, t, 12) == SP_OK);
    assertok(sp_splice(&C, 0, 2) == SP_OK);
    asserteq(sp_bytes(t), 14);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[2], 0);
    asserteq(lens[2], 8);
    /* no-op */
    assertok(sp_splice(&C, 0, 0) == SP_OK);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* fill fully inside one segment: A->ABA / A->AB / A->BA splits */
TEST(fill_leaf) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    /* A->ABA: mid-segment fill splits the leaf into three */
    t = treeV(0, leafV(1, 10));
    assertok(sp_checktree(t));
    assertok(sp_seek(&C, t, 3) == SP_OK);
    assertok(sp_fill(&C, 9, 4) == SP_OK);
    assertok(sp_offset(&C) == 7);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_fill(&C, 9, 7) == SP_OK);
    assertok(sp_offset(&C) == 7);
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
    assertok(sp_seek(&C, t, 3) == SP_OK);
    assertok(sp_fill(&C, 9, 7) == SP_OK);
    assertok(sp_offset(&C) == 10);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 10));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 9);
    asserteq(lens[1], 7);
    sp_freetree(t), sp_close(S);
}

/* ---- fill: arbiter filter over a range ---- */

static unsigned arb_sum(void *ud, sp_Id old, sp_Id in) {
    (void)ud;
    return (unsigned)(old + in);
}

static int      arb_count;
static unsigned arb_counting(void *ud, sp_Id old, sp_Id in) {
    (void)ud, (void)old;
    arb_count += 1;
    return (unsigned)in;
}

static int      arb_notify;
static unsigned arb_notifypad(void *ud, sp_Id old, sp_Id in) {
    (void)ud, (void)old;
    if (in == 0 && old == 0) arb_notify += 1;
    return 0;
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
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_fill(&C, 9, 2) == SP_OK);
    assertok(sp_offset(&C) == 3);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_fill(&C, 9, 2) == SP_OK);
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
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_fill(&C, 9, 1) == SP_OK);
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
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_fill(&C, 9, 4) == SP_OK);
    assertok(sp_checktree(t) && sp_checkcursor(&C, 6));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    asserteq(ids[1], 9);
    asserteq(lens[1], 4);
    asserteq(ids[2], 2);
    asserteq(lens[2], 2);
    asserteq(ids[3], 3);
    asserteq(lens[3], 2);
    /* fill that colors the whole tree: [0,10) -> 7 */
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_fill(&C, 7, 10) == SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 7);
    asserteq(lens[0], 10);
    /* len == 0 and null cursor */
    assertok(sp_fill(&C, 1, 0) == SP_OK);
    assertok(sp_fill(NULL, 1, 1) == SP_ERRPARAM);
    assertok(sp_bytes(t) == 10);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_fill(&C, 5, 3) == SP_OK);
    assertok(sp_offset(&C) == 3);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_fill(&C, 0, 3) == SP_OK);
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
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_fill(&C, 9, 6) == SP_OK);
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
    /* fully virtual: pad + color, the arbiter gets the (0,0) pad
     * notice plus the (0, 0) color of the padded run */
    sp_setarbiter(t, arb_notifypad, NULL);
    assertok(sp_seek(&C, t, 5) == SP_OK);
    assertok(sp_fill(&C, 0, 3) == SP_OK);
    asserteq(arb_notify, 2);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 8);
    /* virtual fill with color: [8,13) -> 7 (bare overwrite) */
    sp_setarbiter(t, NULL, NULL);
    assertok(sp_seek(&C, t, 8) == SP_OK);
    assertok(sp_fill(&C, 7, 5) == SP_OK);
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
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_fill(&C, 9, 8) == SP_OK);
    assertok(sp_offset(&C) == 10);
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
    n0 = (sp_Node *)S->allocf(S->alloc_ud, NULL, 0, sizeof(sp_Node));
    *l0 = lf = (sp_Node *)S->allocf(S->alloc_ud, NULL, 0, sizeof(sp_Node));
    memset(lf, 0, sizeof(sp_Node));
    spL_setid(lf, 0, 1), lf->bytes[0] = 2;
    spL_setid(lf, 1, 2), lf->bytes[1] = 3;
    spN_setcc(lf, 2);
    *l1 = lf = (sp_Node *)S->allocf(S->alloc_ud, NULL, 0, sizeof(sp_Node));
    memset(lf, 0, sizeof(sp_Node));
    spL_setid(lf, 0, 3), lf->bytes[0] = 1;
    spL_setid(lf, 1, 4), lf->bytes[1] = 4;
    spN_setcc(lf, 2);
    *l2 = lf = (sp_Node *)S->allocf(S->alloc_ud, NULL, 0, sizeof(sp_Node));
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
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_fill(&C, 7, 8) == SP_OK);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_fill(&C, 8, 13) == SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 8);
    asserteq(lens[0], 13);
    /* edits after fill still work: remove a tail chunk */
    {
        sp_Cursor R;
        assertok(sp_seek(&C, t, 5) == SP_OK);
        assertok(sp_seek(&R, t, 8) == SP_OK);
        assertok(sp_remove(&C, &R) == SP_OK);
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
static unsigned arb_same(void *ud, sp_Id old, sp_Id in) {
    (void)ud, (void)in;
    return (unsigned)old;
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
    assertok(sp_seek(&C, t, 2) == SP_OK); /* poff == 0 at a segment head */
    live0 = S->nodes.live_obj;
    assertok(sp_fill(&C, 9, 6) == SP_OK);
    assertok(sp_offset(&C) == 8);
    assertok(sp_bytes(t) == 16);
    assertok(S->nodes.live_obj == live0 - 1);
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
    assertok(sp_seek(&C, t, 1) == SP_OK);
    live0 = S->nodes.live_obj;
    assertok(sp_fill(&C, 20, 11) == SP_OK);
    assertok(sp_bytes(t) == 16);
    assertok(S->nodes.live_obj == live0);
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
    assertok(sp_seek(&C, t, 3) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 5);
    asserteq(ids[0], 5);
    asserteq(lens[0], 1);
    asserteq(ids[1], 1);
    asserteq(lens[1], 1);
    asserteq(ids[2], 2);
    asserteq(lens[2], 3);
    asserteq(ids[3], 9);
    asserteq(lens[3], 1);
    asserteq(ids[4], 7);
    asserteq(lens[4], 1);
    assertok(sp_checktree(t));
    sp_freetree(t), sp_close(S);
}

/* mergeleft across a container border absorbs the cursor container's
 * only segment: the emptied shell must leave the tree */
TEST(mergeleft_shell) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(1, 1)), innerV(leafV(2, 1))));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_insert(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 3);
    asserteq(ids[1], 2);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* mergeright across a container border absorbs the right neighbor's
 * only segment: the emptied shell must leave the tree */
TEST(mergeright_shell) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 2), leafV(1, 1)));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 4);
    assertok(sp_checktree(t));
    sp_asserttree(t, 0, leafV(1, 4));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    /* neighbor shell with siblings left: the fork keeps cc >= 2, the
     * fold branch stays off */
    t = treeV(1, innerV(leafV(1, 2), leafV(1, 1), leafV(2, 1)));
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 1);
    asserteq(lens[0], 4);
    asserteq(ids[1], 2);
    assertok(sp_checktree(t));
    sp_asserttree(t, 0, leafV(1, 4, 2, 1));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    /* deep shell: the merge climbs past the fork level, exercising the
     * multi-level byte fixup loop */
    t = treeV(2, innerV(innerV(leafV(2, 1), leafV(1, 2)), innerV(leafV(1, 1))));
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 2);
    asserteq(ids[0], 2);
    asserteq(lens[0], 1);
    asserteq(ids[1], 1);
    asserteq(lens[1], 4);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    /* single-child chain: the neighbor's parent holds one child, the
     * climb reaches the fork level and the run collapses */
    t = treeV(2, innerV(innerV(leafV(1, 2)), innerV(leafV(1, 2))));
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 5);
    assertok(sp_checktree(t));
    sp_asserttree(t, 0, leafV(1, 5));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    /* deep single-child chain: three climb steps before the fold */
    t = treeV(
            4, innerV(innerV(innerV(innerV(leafV(1, 2)))),
                      innerV(innerV(innerV(leafV(1, 2))))));
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 5);
    assertok(sp_checktree(t));
    sp_asserttree(t, 0, leafV(1, 5));
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
    assertok(sp_open(&oom_alloc, &cnt) == NULL);
    sp_close(NULL);
    cnt = 1;
    S2 = sp_open(&oom_alloc, &cnt);
    assertok(S2 != NULL);
    cnt = 0;
    assertok(sp_newtree(S2) == NULL);
    sp_close(S2);
    S = sp_open(NULL, NULL);
    assertok(S != NULL);
    sp_freetree(NULL);
    sp_setarbiter(NULL, NULL, NULL);
    /* tree and cursor param checks */
    t = sp_newtree(S);
    assertok(t != NULL);
    assertok(sp_seek(NULL, t, 0) == SP_ERRPARAM);
    assertok(sp_seek(&C, NULL, 0) == SP_ERRPARAM);
    memset(&B, 0, sizeof(sp_Cursor));
    assertok(sp_advance(NULL, 1) == SP_ERRPARAM);
    assertok(sp_advance(&B, 1) == SP_ERRPARAM);
    assertok(sp_style(NULL, &len) == NULL);
    assertok(sp_next(NULL, &len) == NULL);
    assertok(sp_prev(NULL, &len) == NULL);
    assertok(sp_remove(NULL, &R) == SP_ERRPARAM);
    assertok(sp_remove(&C, NULL) == SP_ERRPARAM);
    assertok(sp_remove(&C, &B) == SP_ERRPARAM);
    assertok(sp_remove(&B, &C) == SP_ERRPARAM);
    assertok(sp_append(NULL, 1) == SP_ERRPARAM);
    assertok(sp_insert(NULL, 1) == SP_ERRPARAM);
    assertok(sp_splice(NULL, 0, 1) == SP_ERRPARAM);
    assertok(sp_fill(NULL, 1, 1) == SP_ERRPARAM);
    assertok(sp_fill(&B, 1, 1) == SP_ERRPARAM);
    sp_freetree(t), sp_close(S);
}

/* traversal edge states: next/prev/advance at segment tails, the tree
 * tail and virtual positions, always also with a NULL plen */
TEST(traversal_edges) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 2, 2, 2), leafV(3, 2, 4, 2)));
    sp_Cursor C;
    assertok(sp_checktree(t));
    /* advance 0 on a non-empty tree, and on an empty tree */
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_advance(&C, 0) == SP_OK);
    assertok(sp_offset(&C) == 0);
    {
        sp_Tree *e = sp_newtree(S);
        assertok(sp_seek(&C, e, 0) == SP_OK);
        assertok(sp_advance(&C, 5) == SP_OK);
        assertok(sp_offset(&C) == 0);
        sp_freetree(e);
    }
    /* sp_style with a NULL plen: inside a segment and at the tail */
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_style(&C, NULL) != NULL);
    assertok(sp_seek(&C, t, 8) == SP_OK);
    assertok(sp_style(&C, NULL) == NULL);
    /* sp_next with NULL plen: segment tail, tree tail, last-segment
     * tail (the l < 0 climb-out) */
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_next(&C, NULL) != NULL);
    assertok(sp_offset(&C) == 2);
    assertok(sp_seek(&C, t, 8) == SP_OK);
    assertok(sp_next(&C, NULL) == NULL);
    assertok(sp_seek(&C, t, 7) == SP_OK);
    assertok(sp_next(&C, NULL) == NULL);
    assertok(sp_offset(&C) == 8);
    /* sp_prev with NULL plen: mid-segment, tree head, successful */
    assertok(sp_seek(&C, t, 6) == SP_OK);
    assertok(sp_prev(&C, NULL) != NULL);
    assertok(sp_offset(&C) == 4);
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_prev(&C, NULL) == NULL);
    assertok(sp_seek(&C, t, 8) == SP_OK);
    assertok(sp_prev(&C, NULL) != NULL);
    assertok(sp_offset(&C) == 6);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_splice(&C, 0, 3) == SP_OK);
    assertok(sp_bytes(t) == 3);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 0);
    asserteq(lens[0], 3);
    /* wipe the whole tree then insert: remove empties, then onepiece */
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_splice(&C, 3, 2) == SP_OK);
    assertok(sp_bytes(t) == 2);
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
    assertok(sp_style(&B, &len) == NULL);
    assertok(sp_next(&B, &len) == NULL);
    assertok(sp_prev(&B, &len) == NULL);
    assertok(sp_append(&B, 1) == SP_ERRPARAM);
    assertok(sp_insert(&B, 1) == SP_ERRPARAM);
    assertok(sp_splice(&B, 0, 1) == SP_ERRPARAM);
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_seek(&R, t2, 0) == SP_OK);
    assertok(sp_remove(&C, &R) == SP_ERRPARAM);
    /* remove with the L cursor at the tree tail clamps to nothing */
    assertok(sp_seek(&C, t2, 0) == SP_OK);
    assertok(sp_append(&C, 4) == SP_OK);
    assertok(sp_seek(&C, t2, 4) == SP_OK);
    assertok(sp_seek(&R, t2, 6) == SP_OK);
    assertok(sp_remove(&C, &R) == SP_OK);
    asserteq(sp_bytes(t2), 4);
    /* both cursors beyond the tree end: the offset guard fires */
    assertok(sp_seek(&C, t2, 8) == SP_OK);
    assertok(sp_seek(&R, t2, 10) == SP_OK);
    assertok(sp_remove(&C, &R) == SP_OK);
    asserteq(sp_bytes(t2), 4);
    sp_freetree(t2), sp_freetree(t), sp_close(S);
}

/* inheritance edge states: append at a container-head segment boundary
 * climbs to the previous container's last segment; insert at a segment
 * tail inherits nothing (id 0) */
TEST(inherit_edges) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(1, innerV(leafV(1, 2, 2, 2), leafV(3, 2)));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    assertok(sp_checktree(t));
    /* append at the head of a container's first segment: inherits the
     * left container's last segment id */
    assertok(sp_seek(&C, t, 4) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    asserteq(ids[1], 2);
    asserteq(lens[1], 3);
    asserteq(ids[2], 3);
    asserteq(lens[2], 2);
    /* insert at the tree tail (poff == len): no right neighbor, the
     * inserted run inherits id 0 */
    assertok(sp_seek(&C, t, 7) == SP_OK);
    assertok(sp_insert(&C, 2) == SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
    assertok(sp_append(&C, 20) == SP_OK);
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
    assertok(sp_seek(&C, t, 10) == SP_OK);
    assertok(sp_splice(&C, 5, 3) == SP_ERRMEM);
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
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
    assertok(sp_seek(&C, t, 7) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    assertok(sp_checktree(t));
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[3], 3);
    asserteq(lens[3], 3);
    sp_freetree(t), sp_close(S);
}

/* mergeright empties a 3-container node: the fold chain's first link
 * lands on a full parent and the while loop exits on cc >= 2 */
TEST(mergeright_fold_done) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(1, 1)),
                      innerV(leafV(1, 1), leafV(1, 1), leafV(2, 1))));
    sp_Cursor C;
    sp_Id     ids[16];
    size_t    lens[16];
    int       n;
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 4);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 1);
    asserteq(lens[1], 3);
    asserteq(ids[2], 1);
    asserteq(lens[2], 1);
    asserteq(ids[3], 2);
    asserteq(lens[3], 1);
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
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_next(&C, &len) != NULL);
    assertok(sp_offset(&C) == 2);
    assertok(*sp_style(&C, &len) == 3 && len == 1);
    assertok(sp_checktree(t));
    /* empty-tree prev */
    {
        sp_Tree  *e = sp_newtree(S);
        sp_Cursor D;
        assertok(sp_seek(&D, e, 0) == SP_OK);
        assertok(sp_prev(&D, &len) == NULL);
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
    assertok(sp_seek(&L, t, 2) == SP_OK);
    assertok(sp_seek(&R, t, 3) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
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
    assertok(sp_seek(&L, t, 7) == SP_OK);
    assertok(sp_seek(&R, t, 8) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
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
    assertok(sp_seek(&C, t, 9) == SP_OK);
    assertok(sp_insert(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 15);
    asserteq(ids[7], 4);
    asserteq(lens[7], 3);
    asserteq(ids[8], 9);
    asserteq(lens[8], 1);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    /* four-level mergeright: the append absorbs the right chain's
     * first container whole */
    t = treeV(
            4, innerV(innerV(innerV(innerV(leafV(1, 1), leafV(3, 1)),
                                    innerV(leafV(3, 2), leafV(4, 1))),
                             innerV(innerV(leafV(5, 1), leafV(6, 1)),
                                    innerV(leafV(7, 1), leafV(4, 2)))),
                      innerV(innerV(innerV(leafV(4, 1), leafV(9, 1)),
                                    innerV(leafV(10, 1), leafV(11, 1))),
                             innerV(innerV(leafV(12, 1), leafV(13, 1)),
                                    innerV(leafV(14, 1), leafV(15, 1))))));
    assertok(sp_seek(&C, t, 9) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 15);
    asserteq(ids[7], 4);
    asserteq(lens[7], 4);
    asserteq(ids[8], 9);
    asserteq(lens[8], 1);
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
    assertok(sp_seek(&L, t, 2) == SP_OK);
    assertok(sp_seek(&R, t, 4) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
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
    assertok(sp_seek(&L, t, 2) == SP_OK);
    assertok(sp_seek(&R, t, 4) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
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
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_fill(&C, 7, len) == SP_OK);
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
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_insert(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 2);
    asserteq(lens[1], 3);
    asserteq(ids[2], 3);
    asserteq(lens[2], 1);
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    /* deeper: three levels, the fold chain climbs through the layer-0
     * node before the root shrinks */
    t = treeV(
            3, innerV(
                       innerV(innerV(leafV(1, 1), leafV(2, 1)),
                              innerV(leafV(2, 1), leafV(3, 1)))));
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_insert(&C, 1) == SP_OK);
    collect_stream(&C, ids, lens, &n);
    asserteq(n, 3);
    asserteq(ids[0], 1);
    asserteq(lens[0], 1);
    asserteq(ids[1], 2);
    asserteq(lens[1], 3);
    asserteq(ids[2], 3);
    asserteq(lens[2], 1);
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
    assertok(sp_seek(&C, t, 3) == SP_OK);
    assertok(sp_prev(&C, &len) != NULL);
    assertok(sp_offset(&C) == 2);
    assertok(*sp_style(&C, &len) == 2 && len == 1);
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
    assertok(sp_seek(&L, t, 2) == SP_OK);
    assertok(sp_seek(&R, t, 5) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
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
    assertok(sp_seek(&L, t, 2) == SP_OK);
    assertok(sp_seek(&R, t, 3) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
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
    assertok(sp_seek(&L, t, 5) == SP_OK);
    assertok(sp_seek(&R, t, 6) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
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
    assertok(sp_seek(&C, t, 1) == SP_OK);
    assertok(sp_splice(&C, 1, 0) == SP_OK);
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
    assertok(sp_seek(&L, t, 2) == SP_OK);
    assertok(sp_seek(&R, t, 4) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
    collect_stream(&L, ids, lens, &n);
    asserteq(n, 1);
    asserteq(ids[0], 1);
    asserteq(lens[0], 2);
    assertok(sp_checkcursor(&L, 2));
    assertok(sp_checktree(t));
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
    assertok(sp_seek(&L, t, 1) == SP_OK);
    assertok(sp_seek(&R, t, 2) == SP_OK);
    assertok(sp_remove(&L, &R) == SP_OK);
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
    mseg_t tmp[MMAX];
    size_t acc = 0, end = pos + len;
    int    i, w = 0;
    if (len == 0) return;
    if (end > mbytes) mpad(end);
    for (i = 0; i < mn; ++i) {
        size_t a = acc, b = acc + msegs[i].len;
        size_t lo = a > pos ? a : pos;
        size_t hi = b < end ? b : end;
        if (lo < hi) {
            if (lo > a) tmp[w].len = lo - a, tmp[w].id = msegs[i].id, w++;
            tmp[w].len = hi - lo, tmp[w].id = (sp_Id)arb(NULL, msegs[i].id, in),
            w++;
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
    for (i = 0; i < mn && i < 20; ++i)
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
    sp_Cursor    C;
    const sp_Id *id;
    size_t       len;
    int          i = 0;
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
    assertok(sp_seek(&C, t, 0) == SP_OK);
    while ((id = sp_style(&C, &len)) != NULL) {
        if (i >= mn || *id != msegs[i].id || len != msegs[i].len) {
            test_log(
                    "DIFFER step %d: seg %d tree=(%lu,%lu) model=(%lu,%lu)\n",
                    step, i, test_lu(len), test_lu(*id), test_lu(msegs[i].len),
                    test_lu(msegs[i].id));
            mops_dump();
            sp_dumptree(t, "TREE");
            mdump();
            abort();
        }
        i += 1;
        sp_next(&C, &len);
    }
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
static unsigned arb_overwrite(void *ud, sp_Id old, sp_Id in) {
    (void)ud, (void)old;
    return (unsigned)in;
}

static unsigned arb_uniq(void *ud, sp_Id old, sp_Id in) {
    (void)ud;
    return (unsigned)(old * 100 + in + 10000);
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

static void mserial(char *buf) {
    int i, r = 0;
    buf[0] = '\0';
    for (i = 0; i < mn; ++i)
        r += snprintf(
                buf + r, 4096 - (size_t)r, "[%lu:%lu]", test_lu(msegs[i].id),
                test_lu(msegs[i].len));
}

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
                mkfilltree(S, t);
                mn = 144, mbytes = 144;
                for (i = 0; i < 144; ++i)
                    msegs[i].len = 1, msegs[i].id = (sp_Id)(i + 1);
                sp_setarbiter(t, arbs[a], NULL);
                mfill(pos, 7, len, arbs[a]);
                assertok(sp_seek(&C, t, pos) == SP_OK);
                assertok(sp_fill(&C, 7, len) == SP_OK);
                mserial(want), sp_serialtree(t, got);
                if (!sp_checktree(t) || strcmp(want, got) != 0
                    || sp_offset(&C) != pos + len) {
                    test_log(
                            "fill_brute a=%d pos=%lu len=%lu off=%lu\n", a,
                            test_lu(pos), test_lu(len), test_lu(sp_offset(&C)));
                    test_log("want=%s\ngot =%s\n", want, got);
                    sp_dumptree(t, "TREE");
                    mdump();
                    abort();
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
            assertok(sp_seek(&C, t, pos) == SP_OK);
            {
                sp_Cursor R;
                assertok(sp_seek(&R, t, pos + l1) == SP_OK);
                assertok(sp_remove(&C, &R) == SP_OK);
            }
            mcompare(t, step);
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
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_append(&C, l1) == SP_OK);
            break;
        case 1:
            minsert(pos, l1, 0);
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_insert(&C, l1) == SP_OK);
            break;
        case 2:
            mremove(pos, l1);
            assertok(sp_seek(&C, t, pos) == SP_OK);
            {
                sp_Cursor R;
                assertok(sp_seek(&R, t, pos + l1) == SP_OK);
                assertok(sp_remove(&C, &R) == SP_OK);
            }
            break;
        case 3:
            mremove(pos, l1);
            minsert(pos, l2, 1);
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_splice(&C, l1, l2) == SP_OK);
            break;
        default:
            mfill(pos, in, l1, arb_sum);
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_fill(&C, in, l1) == SP_OK);
            break;
        }
        mcompare(t, step);
    }
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

#include "spantree_test_fanout4.gen.inc"
