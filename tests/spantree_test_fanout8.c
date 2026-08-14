#define SP_FANOUT    8
#define SP_PAGE_SIZE 1024
#define SP_STATIC_API
#ifndef SP_POOL_STATS
# define SP_POOL_STATS
#endif

#include "../spantree.h"
#include "sp_tests.h"
#include "tests.h"

/* spantree tests: fanout-8 — larger fanout shifts the split/merge
 * thresholds (FANOUT/2 = 4), reaching branch combinations that a
 * fanout-4 tree cannot (balance-at-boundary, full-neighbor folds).
 * Tree shapes here are built with leafV/innerV/treeV, so levels and
 * shape expectations are fanout-8 specific. */

/* build 144 one-byte segments in a two-level tree: root(3) ->
 * 3 x layer-0 (cc=6) -> 18 x leaf containers (cc=8) -> 144 segments */
static void mk8tree(sp_State *S, sp_Tree *t) {
    sp_Node *cont[18], *n, *lf;
    int      i, j;
    for (i = 0; i < 18; ++i) {
        lf = (sp_Node *)spP_alloc(S, &S->nodes);
        memset(lf, 0, sizeof(sp_Node));
        for (j = 0; j < 8; ++j)
            spL_setid(lf, j, (sp_Id)(8 * i + j + 1)), lf->bytes[j] = 1;
        spN_setcc(lf, 8), cont[i] = lf;
    }
    for (i = 0; i < 3; ++i) {
        n = (sp_Node *)spP_alloc(S, &S->nodes);
        memset(n, 0, sizeof(sp_Node));
        for (j = 0; j < 6; ++j)
            n->children[j] = cont[i * 6 + j], n->bytes[j] = 8;
        spN_setcc(n, 6);
        t->root.children[i] = n, t->root.bytes[i] = 48;
    }
    spN_setcc(&t->root, 3), t->levels = 2, t->bytes = 144;
}

/* arbiter that overwrites: the covered run collapses into one run */
static unsigned arb8_overwrite(void *ud, sp_Id old, sp_Id in) {
    (void)ud, (void)old;
    return (unsigned)in;
}

/* exhaustive fill over the 144-byte fanout-8 tree */
TEST(fill8_brute) {
    sp_State *S = sp_open(NULL, NULL);
    size_t    pos, len;
    for (pos = 0; pos <= 145; ++pos)
        for (len = 1; len <= 146; ++len) {
            sp_Tree  *t = sp_newtree(S);
            sp_Cursor C;
            mk8tree(S, t);
            assertok(sp_checktree(t));
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_fill(&C, 7, len) == SP_OK);
            assertok(sp_checktree(t));
            sp_freetree(t);
            asserteq(S->nodes.live_obj, 0);
        }
    sp_close(S);
}

/* arbiter set + repeated edits on the big tree */
TEST(edit8_sequence) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    int       step;
    mk8tree(S, t);
    sp_setarbiter(t, arb8_overwrite, NULL);
    assertok(sp_checktree(t));
    for (step = 0; step < 60; ++step) {
        size_t pos = (size_t)((step * 37) % 130);
        size_t l1 = (size_t)((step * 11) % 20) + 1;
        switch (step % 4) {
        case 0:
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_append(&C, l1) == SP_OK);
            break;
        case 1:
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_insert(&C, l1) == SP_OK);
            break;
        case 2:
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_fill(&C, (sp_Id)(step + 1), l1) == SP_OK);
            break;
        default:
            assertok(sp_seek(&C, t, pos) == SP_OK);
            assertok(sp_splice(&C, l1, l1 / 2) == SP_OK);
            break;
        }
        assertok(sp_checktree(t));
    }
    {
        sp_Cursor R;
        assertok(sp_seek(&C, t, 5) == SP_OK);
        assertok(sp_seek(&R, t, 55) == SP_OK);
        assertok(sp_remove(&C, &R) == SP_OK);
        assertok(sp_checktree(t));
    }
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* deep mergeleft on a three-level fanout-8 tree: the fork sits below
 * the root, the byte fixup loop runs while the fold chain climbs */
/* deep mergeleft on a three-level fanout-8 tree: interior nodes hold
 * four children (FANOUT/2), the fold chain keeps them legal */
TEST(merge8_left) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3,
            innerV(innerV(
                    innerV(leafV(2, 1), leafV(2, 1), leafV(3, 1), leafV(4, 1)),
                    innerV(leafV(5, 1), leafV(6, 1), leafV(7, 1), leafV(8, 1)),
                    innerV(leafV(9, 1), leafV(10, 1), leafV(11, 1),
                           leafV(12, 1)),
                    innerV(leafV(13, 1), leafV(14, 1), leafV(15, 1),
                           leafV(16, 1)))));
    sp_Cursor C;
    char      buf[128];
    assertok(sp_checktree(t));
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_insert(&C, 1) == SP_OK);
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[2:1][2:1][3:2][4:1][5:1][6:1][7:1][8:1][9:1][10:1][11:1]"
            "[12:1][13:1][14:1][15:1][16:1]");
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* deep mergeright on a three-level fanout-8 tree */
TEST(merge8_right) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3,
            innerV(innerV(
                    innerV(leafV(2, 1), leafV(2, 1), leafV(3, 1), leafV(4, 1)),
                    innerV(leafV(5, 1), leafV(6, 1), leafV(7, 1), leafV(8, 1)),
                    innerV(leafV(9, 1), leafV(10, 1), leafV(11, 1),
                           leafV(12, 1)),
                    innerV(leafV(13, 1), leafV(14, 1), leafV(15, 1),
                           leafV(16, 1)))));
    sp_Cursor C;
    char      buf[128];
    assertok(sp_checktree(t));
    assertok(sp_seek(&C, t, 2) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[2:1][2:2][3:1][4:1][5:1][6:1][7:1][8:1][9:1][10:1][11:1]"
            "[12:1][13:1][14:1][15:1][16:1]");
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* mergeright empties a five-container node: the fold chain's first
 * link lands on a full parent and the while loop exits on cc >= 4 */
TEST(merge8_fold_done) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(
                       innerV(innerV(leafV(1, 1), leafV(2, 1), leafV(3, 1),
                                     leafV(4, 1), leafV(5, 2)),
                              innerV(leafV(5, 1), leafV(6, 1), leafV(7, 1),
                                     leafV(8, 1), leafV(9, 1)),
                              innerV(leafV(10, 1), leafV(11, 1), leafV(12, 1),
                                     leafV(13, 1)),
                              innerV(leafV(14, 1), leafV(15, 1), leafV(16, 1),
                                     leafV(17, 1)))));
    sp_Cursor C;
    char      buf[128];
    assertok(sp_checktree(t));
    assertok(sp_seek(&C, t, 6) == SP_OK);
    assertok(sp_append(&C, 1) == SP_OK);
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:1][2:1][3:1][4:1][5:4][6:1][7:1][8:1][9:1][10:1][11:1]"
            "[12:1][13:1][14:1][15:1][16:1][17:1]");
    assertok(sp_checktree(t));
    sp_freetree(t);
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

#include "spantree_test_fanout8.gen.inc"
