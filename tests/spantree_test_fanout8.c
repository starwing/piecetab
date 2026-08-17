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

/* exhaustive fill over the 144-byte fanout-8 tree */
TEST(fill8_brute) {
    sp_State *S = sp_open(NULL, NULL);
    size_t    pos, len;
    for (pos = 0; pos <= 145; ++pos)
        for (len = 1; len <= 146; ++len) {
            sp_Tree  *t = sp_newtree(S);
            sp_Cursor C;
            SpRef     bref;
            long      counts[SP_REFN];
            mk8tree(S, t);
            memset(&bref, 0, sizeof(SpRef));
            spA_seed(&bref, t), sp_setarbiter(t, spA_ref, &bref);
            assertok(sp_checktree(t));
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_fill(&C, 7, len), SP_OK);
            assertok(sp_checktree(t));
            spA_tally(t, counts);
            assertok(spA_check(&bref, counts));
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
    SpRef     bref;
    long      counts[SP_REFN];
    int       step;
    mk8tree(S, t);
    memset(&bref, 0, sizeof(SpRef));
    spA_seed(&bref, t), sp_setarbiter(t, spA_ref, &bref);
    assertok(sp_checktree(t));
    for (step = 0; step < 60; ++step) {
        size_t pos = (size_t)((step * 37) % 130);
        size_t l1 = (size_t)((step * 11) % 20) + 1;
        switch (step % 4) {
        case 0:
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_append(&C, l1), SP_OK);
            break;
        case 1:
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_insert(&C, l1), SP_OK);
            break;
        case 2:
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_fill(&C, (sp_Id)(step + 1), l1), SP_OK);
            break;
        default:
            asserteq(sp_seek(&C, t, pos), SP_OK);
            asserteq(sp_splice(&C, l1, l1 / 2), SP_OK);
            break;
        }
        assertok(sp_checktree(t));
        spA_tally(t, counts);
        assertok(spA_check(&bref, counts));
    }
    {
        sp_Cursor R;
        asserteq(sp_seek(&C, t, 5), SP_OK);
        asserteq(sp_seek(&R, t, 55), SP_OK);
        asserteq(sp_remove(&C, &R), SP_OK);
        assertok(sp_checktree(t));
        spA_tally(t, counts);
        assertok(spA_check(&bref, counts));
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
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_insert(&C, 1), SP_OK);
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[2:2][3:2][4:1][5:1][6:1][7:1][8:1][9:1][10:1][11:1]"
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
    asserteq(sp_seek(&C, t, 2), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[2:3][3:1][4:1][5:1][6:1][7:1][8:1][9:1][10:1][11:1]"
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
    asserteq(sp_seek(&C, t, 6), SP_OK);
    asserteq(sp_append(&C, 1), SP_OK);
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

/* ns differential on the fanout-8 shape: filtered iteration and
 * prune-clear must agree with a full mask scan under random ops */
static unsigned mrng8(unsigned s) { return s * 1103515245u + 12345u; }

TEST(ns8_differ) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    sp_Id     ids1[256], ids2[256];
    size_t    lens1[256], lens2[256];
    SpRef     nref;
    long      counts[SP_REFN];
    unsigned  seed = 7;
    int       i, k, step, n1, n2;
    mk8tree(S, t);
    memset(&nref, 0, sizeof(SpRef));
    spA_seed(&nref, t), sp_setarbiter(t, spA_nsref, &nref);
    for (step = 0; step < 300; ++step) {
        size_t pos, len, bytes = sp_bytes(t);
        int    op = (int)(seed = mrng8(seed)) % 7;
        pos = (seed = mrng8(seed)) % (bytes + 16);
        len = 1 + (seed = mrng8(seed)) % 10;
        asserteq(sp_seek(&C, t, pos), SP_OK);
        if (op < 3)
            asserteq(
                    sp_fill(&C, (sp_Id)(1 + (seed = mrng8(seed)) % 3), len),
                    SP_OK);
        else if (op == 3)
            asserteq(sp_append(&C, len), SP_OK);
        else if (op == 4)
            asserteq(sp_insert(&C, len), SP_OK);
        else if (op == 5)
            asserteq(sp_splice(&C, sp_min(len, bytes), 0), SP_OK);
        else {
            int ns1 = 1 + (int)((seed = mrng8(seed)) % 3);
            int ns2 = 1 + (int)((seed = mrng8(seed)) % 3);
            asserteq(sp_clear(t, ns1, 0x8000 + (sp_Id)ns2), SP_OK);
        }
        assertok(sp_checktree(t));
        spA_tally(t, counts);
        assertok(spA_check(&nref, counts));
    }
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
            if (sp_next(&C, k, &len) == 0) break;
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

/* fanout-8 twin of remove_mergeleft_foldfirst: the chain node at the
 * fork's first child folds rightward into the full cursor parent */
TEST(remove_mergeleft_foldfirst) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(innerV(innerV(leafV(5, 7, 6, 170), leafV(7, 9),
                                    leafV(8, 9), leafV(9, 9, 2, 1152)),
                             innerV(leafV(2, 3, 2, 82), leafV(3, 142),
                                    leafV(4, 89), leafV(10, 1), leafV(11, 1)),
                             innerV(leafV(12, 1), leafV(13, 1), leafV(14, 1),
                                    leafV(15, 1)),
                             innerV(leafV(16, 1), leafV(17, 1), leafV(18, 1),
                                    leafV(19, 1)),
                             innerV(leafV(20, 1), leafV(21, 1), leafV(22, 1),
                                    leafV(23, 1))),
                      innerV(innerV(leafV(24, 1), leafV(25, 1), leafV(26, 1),
                                    leafV(27, 1)),
                             innerV(leafV(28, 1), leafV(29, 1), leafV(30, 1),
                                    leafV(31, 1)),
                             innerV(leafV(32, 1), leafV(33, 1), leafV(34, 1),
                                    leafV(35, 1)),
                             innerV(leafV(36, 1), leafV(37, 1), leafV(38, 1),
                                    leafV(39, 1)))));
    sp_Cursor C;
    char      buf[256];
    assertok(sp_checktree_allow_unseamedspan(t, 1)); /* pre-merge input */
    asserteq(sp_seek(&C, t, 1356), SP_OK);
    asserteq(sp_splice(&C, 18, 19), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 1375));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[5:7][6:170][7:9][8:9][9:9][2:1238][3:142][4:89]"
            "[10:1][11:1][12:1][13:1][14:1][15:1][16:1][17:1]"
            "[18:1][19:1][20:1][21:1][22:1][23:1][24:1][25:1]"
            "[26:1][27:1][28:1][29:1][30:1][31:1][32:1][33:1]"
            "[34:1][35:1][36:1][37:1][38:1][39:1]");
    sp_freetree(t), sp_close(S);
}

/* fanout-8 twin of remove_mergeleft_idmismatch: the left neighbor's
 * tail id differs from the cut-side head, no pull happens */
TEST(remove_mergeleft_idmismatch) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2,
            innerV(innerV(leafV(1, 1), leafV(2, 1), leafV(3, 1), leafV(40, 1)),
                   innerV(leafV(5, 1, 6, 1, 4, 1), leafV(7, 1), leafV(8, 1),
                          leafV(9, 1))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 4), SP_OK);
    asserteq(sp_seek(&R, t, 6), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 4));
    sp_serialtree(t, buf);
    assertstreq(buf, "[1:1][2:1][3:1][40:1][4:1][7:1][8:1][9:1]");
    sp_freetree(t), sp_close(S);
}

/* fanout-8 twin of remove_mergeleft_healthy: the pull leaves the
 * neighbor's leaf container at cc=4 (healthy), foldbelow returns at
 * the first chain node */
TEST(remove_mergeleft_healthy) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(innerV(innerV(leafV(1, 1), leafV(2, 1), leafV(3, 1),
                                    leafV(4, 1)),
                             innerV(leafV(5, 1), leafV(6, 1), leafV(7, 1),
                                    leafV(8, 1)),
                             innerV(leafV(30, 1), leafV(31, 1), leafV(32, 1),
                                    leafV(33, 1)),
                             innerV(leafV(34, 1), leafV(35, 1), leafV(36, 1),
                                    leafV(8, 1, 9, 1, 10, 1, 11, 1, 12, 1))),
                      innerV(innerV(leafV(13, 1, 14, 1, 15, 1), leafV(16, 1),
                                    leafV(17, 1), leafV(18, 1)),
                             innerV(leafV(12, 1, 19, 1), leafV(20, 1),
                                    leafV(21, 1), leafV(22, 1)),
                             innerV(leafV(40, 1), leafV(41, 1), leafV(42, 1),
                                    leafV(43, 1)),
                             innerV(leafV(44, 1), leafV(45, 1), leafV(46, 1),
                                    leafV(47, 1)))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 20), SP_OK);
    asserteq(sp_seek(&R, t, 26), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 20));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:1][2:1][3:1][4:1][5:1][6:1][7:1][8:1][30:1][31:1]"
            "[32:1][33:1][34:1][35:1][36:1][8:1][9:1][10:1][11:1]"
            "[12:2][19:1][20:1][21:1][22:1][40:1][41:1][42:1]"
            "[43:1][44:1][45:1][46:1][47:1]");
    sp_freetree(t), sp_close(S);
}

/* fanout-8 twin of remove_mergeleft_foldrightbal: the drained fork
 * survivor (cc=3) folds right into the full cursor container (cc=8),
 * forcing the balance path */
TEST(remove_mergeleft_foldrightbal) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(2, 1), leafV(3, 1),
                             leafV(4, 1, 5, 1)),
                      innerV(leafV(6, 1, 7, 1, 5, 1), leafV(8, 1), leafV(9, 1),
                             leafV(10, 1), leafV(11, 1), leafV(12, 1),
                             leafV(13, 1), leafV(14, 1))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_seek(&R, t, 7), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 5));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:1][2:1][3:1][4:1][5:2][8:1][9:1][10:1][11:1][12:1]"
            "[13:1][14:1]");
    sp_freetree(t), sp_close(S);
}

/* fanout-8 twin of remove_stitch_findroom: rt[0] outgrows the cut
 * container's free slots, findroom chains a fresh leaf container */
TEST(remove_stitch_findroom) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 10, 2, 10, 3, 10, 4, 10, 5, 10, 6, 10, 7,
                                   10, 8, 10),
                             leafV(9, 10, 10, 10), leafV(11, 10, 12, 10),
                             leafV(13, 10, 14, 10)),
                      innerV(leafV(15, 10, 16, 10), leafV(17, 10, 18, 10),
                             leafV(19, 10, 20, 10),
                             leafV(21, 10, 22, 10, 23, 10, 24, 10, 25, 10, 26,
                                   10, 27, 10, 28, 10))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 5), SP_OK);
    asserteq(sp_seek(&R, t, 200), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 5));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:5][21:10][22:10][23:10][24:10][25:10][26:10][27:10]"
            "[28:10]");
    sp_freetree(t), sp_close(S);
}

/* random edit sequences on the fanout-8 shape: the three-shape
 * refcount table must equal the segment tally through every step,
 * and freetree zeroes it */
TEST(idref8_differ) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = sp_newtree(S);
    sp_Cursor C;
    SpRef     r;
    long      counts[SP_REFN];
    unsigned  seed = 3;
    int       step;
    assert(t), mk8tree(S, t);
    memset(&r, 0, sizeof(SpRef));
    spA_seed(&r, t), sp_setarbiter(t, spA_ref, &r);
    for (step = 0; step < 300; ++step) {
        size_t pos, len, bytes = sp_bytes(t);
        int    op = (int)(seed = mrng8(seed)) % 8;
        pos = (seed = mrng8(seed)) % (bytes + 16);
        len = 1 + (seed = mrng8(seed)) % 10;
        asserteq(sp_seek(&C, t, pos), SP_OK);
        if (op < 4)
            asserteq(
                    sp_fill(&C, (sp_Id)(1 + (seed = mrng8(seed)) % 8), len),
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
    asserteq(S->nodes.live_obj, 0);
    sp_close(S);
}

/* mergeleft whose pull stops on a full rt[0]: the fork survivor
 * (cc=3) folds left into its sibling (merge path) */
TEST(remove_mergeleft_foldleft) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            1,
            innerV(leafV(1, 1, 2, 1), leafV(3, 1, 4, 1, 5, 1, 7, 1),
                   leafV(5, 1, 6, 1),
                   leafV(7, 1, 8, 1, 9, 1, 10, 1, 11, 1, 12, 1, 13, 1, 14, 1)));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 6), SP_OK);
    asserteq(sp_seek(&R, t, 8), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 6));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:1][2:1][3:1][4:1][5:1][7:2][8:1][9:1][10:1][11:1]"
            "[12:1][13:1][14:1]");
    sp_freetree(t), sp_close(S);
}

/* fanout-8 twin of remove_mergeleft_pullref: one join plus three
 * ride-along pulls, every pulled slot must stay silent */
TEST(remove_mergeleft_pullref) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(9, 1), leafV(10, 1),
                             leafV(2, 1, 3, 1, 4, 1, 5, 1)),
                      innerV(leafV(6, 1, 7, 1), leafV(5, 1, 8, 1), leafV(11, 1),
                             leafV(12, 1))));
    sp_Cursor C, R;
    SpRef     r;
    long      counts[SP_REFN];
    char      buf[256];
    memset(&r, 0, sizeof(SpRef));
    spA_seed(&r, t), sp_setarbiter(t, spA_ref, &r);
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 7), SP_OK);
    asserteq(sp_seek(&R, t, 9), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 7));
    sp_serialtree(t, buf);
    assertstreq(buf, "[1:1][9:1][10:1][2:1][3:1][4:1][5:2][8:1][11:1][12:1]");
    spA_tally(t, counts);
    assertok(spA_check(&r, counts));
    sp_setarbiter(t, NULL, NULL);
    sp_freetree(t), sp_close(S);
}

/* same flow with a full left sibling: the fork fold balances instead
 * of merging (cL + cc > FANOUT) */
TEST(remove_mergeleft_foldleftbal) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            1, innerV(leafV(1, 1, 2, 1, 3, 1, 4, 1, 5, 1, 6, 1, 7, 1),
                      leafV(8, 1, 9, 1, 10, 1, 11, 1), leafV(10, 1, 12, 1),
                      leafV(11, 1, 13, 1, 14, 1, 15, 1, 16, 1, 17, 1, 18, 1, 19,
                            1)));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 11), SP_OK);
    asserteq(sp_seek(&R, t, 13), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 11));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:1][2:1][3:1][4:1][5:1][6:1][7:1][8:1][9:1][10:1]"
            "[11:2][13:1][14:1][15:1][16:1][17:1][18:1][19:1]");
    sp_freetree(t), sp_close(S);
}

/* mergeleft draining the chain leaf at the fork's left subtree:
 * foldbelow folds the underfilled container left into its sibling */
TEST(remove_mergeleft_foldbelow) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(40, 1, 41, 1),
                             leafV(20, 1, 21, 1, 22, 1),
                             leafV(30, 1, 31, 1, 32, 1, 7, 1)),
                      innerV(leafV(5, 1, 6, 1),
                             leafV(7, 1, 8, 1, 9, 1, 10, 1, 11, 1, 12, 1, 13, 1,
                                   14, 1),
                             leafV(50, 1, 51, 1, 52, 1),
                             leafV(53, 1, 54, 1, 55, 1))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 10), SP_OK);
    asserteq(sp_seek(&R, t, 12), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 10));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:1][40:1][41:1][20:1][21:1][22:1][30:1][31:1][32:1]"
            "[7:2][8:1][9:1][10:1][11:1][12:1][13:1][14:1][50:1]"
            "[51:1][52:1][53:1][54:1][55:1]");
    sp_freetree(t), sp_close(S);
}

/* same flow with a nearly full chain sibling: foldbelow balances
 * instead of merging (cL + cc > FANOUT) */
TEST(remove_mergeleft_foldbelowbal) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            2, innerV(innerV(leafV(1, 1), leafV(2, 1),
                             leafV(3, 1, 4, 1, 5, 1, 6, 1, 7, 1, 8, 1, 9, 1),
                             leafV(30, 1, 31, 1, 32, 1, 7, 1)),
                      innerV(leafV(5, 1, 6, 1),
                             leafV(7, 1, 8, 1, 9, 1, 10, 1, 11, 1, 12, 1, 13, 1,
                                   14, 1),
                             leafV(50, 1, 51, 1, 52, 1),
                             leafV(53, 1, 54, 1, 55, 1))));
    sp_Cursor C, R;
    char      buf[256];
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 13), SP_OK);
    asserteq(sp_seek(&R, t, 15), SP_OK);
    asserteq(sp_remove(&C, &R), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 13));
    sp_serialtree(t, buf);
    assertstreq(
            buf,
            "[1:1][2:1][3:1][4:1][5:1][6:1][7:1][8:1][9:1][30:1]"
            "[31:1][32:1][7:2][8:1][9:1][10:1][11:1][12:1][13:1]"
            "[14:1][50:1][51:1][52:1][53:1][54:1][55:1]");
    sp_freetree(t), sp_close(S);
}

/* rmleaf on a 5-leaf container: the rebalance fold loop returns at
 * the first level (the child stays healthy) */
TEST(rebalance_healthy) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(innerV(
                       innerV(leafV(1, 1, 2, 1, 3, 1, 4, 1),
                              leafV(5, 1, 6, 1, 7, 1, 8, 1),
                              leafV(9, 1, 10, 1, 11, 1, 12, 1),
                              leafV(13, 1, 14, 1, 15, 1, 16, 1)),
                       innerV(leafV(17, 1, 18, 1, 19, 1, 20, 1),
                              leafV(21, 1, 22, 1, 23, 1, 24, 1),
                              leafV(25, 1, 26, 1, 27, 1, 28, 1),
                              leafV(29, 1, 30, 1, 31, 1, 32, 1)),
                       innerV(leafV(33, 1, 34, 1, 35, 1, 36, 1),
                              leafV(37, 1, 38, 1, 39, 1, 40, 1),
                              leafV(41, 1, 42, 1, 43, 1, 44, 1),
                              leafV(45, 1, 46, 1, 47, 1, 48, 1)),
                       innerV(leafV(49, 1, 50, 1, 51, 1, 52, 1),
                              leafV(53, 1, 54, 1, 55, 1, 56, 1),
                              leafV(57, 1, 58, 1, 59, 1, 60, 1),
                              leafV(61, 1, 62, 1, 63, 1, 64, 1),
                              leafV(65, 1, 66, 1, 67, 1, 68, 1, 69, 1)))));
    sp_Cursor C;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 68), SP_OK);
    asserteq(sp_splice(&C, 1, 0), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 68));
    asserteq(sp_bytes(t), 68);
    sp_freetree(t), sp_close(S);
}

/* rmleaf dropping a 4-leaf container next to a full one: the rebalance
 * fold balances instead of merging (cL + cR > FANOUT) */
TEST(rebalance_balance) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *t = treeV(
            3, innerV(
                       innerV(innerV(leafV(1, 1, 2, 1, 3, 1, 4, 1),
                                     leafV(5, 1, 6, 1, 7, 1, 8, 1),
                                     leafV(9, 1, 10, 1, 11, 1, 12, 1),
                                     leafV(13, 1, 14, 1, 15, 1, 16, 1)),
                              innerV(leafV(17, 1, 18, 1, 19, 1, 20, 1),
                                     leafV(21, 1, 22, 1, 23, 1, 24, 1),
                                     leafV(25, 1, 26, 1, 27, 1, 28, 1),
                                     leafV(29, 1, 30, 1, 31, 1, 32, 1)),
                              innerV(leafV(33, 1, 34, 1, 35, 1, 36, 1),
                                     leafV(37, 1, 38, 1, 39, 1, 40, 1),
                                     leafV(41, 1, 42, 1, 43, 1, 44, 1),
                                     leafV(45, 1, 46, 1, 47, 1, 48, 1)),
                              innerV(leafV(49, 1, 50, 1, 51, 1, 52, 1),
                                     leafV(53, 1, 54, 1, 55, 1, 56, 1),
                                     leafV(57, 1, 58, 1, 59, 1, 60, 1),
                                     leafV(61, 1, 62, 1, 63, 1, 64, 1, 65, 1,
                                           66, 1, 67, 1, 68, 1),
                                     leafV(69, 1, 70, 1, 71, 1, 72, 1)))));
    sp_Cursor C;
    assertok(sp_checktree(t));
    asserteq(sp_seek(&C, t, 71), SP_OK);
    asserteq(sp_splice(&C, 1, 0), SP_OK);
    assertok(sp_checktree(t));
    assertok(sp_checkcursor(&C, 71));
    asserteq(sp_bytes(t), 71);
    sp_freetree(t), sp_close(S);
}

#include "spantree_test_fanout8.gen.inc"
