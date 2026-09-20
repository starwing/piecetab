#ifndef AT_FANOUT
#define AT_FANOUT 5
#endif
#ifndef AT_MAX_LEVEL
#define AT_MAX_LEVEL 64 /* odd fanout override: see docs/max_levels.md */
#endif
#define AT_PAGE_SIZE 512
#define AT_STATIC_API

#include "../anchortree.h"
#include "at_cases.h"
#include "at_tests.h"
#include "tests.h"

/* odd-fanout runner: default fanout 5, overridable via AT_FANOUT. */

/* shared M1 regression; fanout4/8 register the same wrapper. */
TEST(iter_exhaust_commit) { att_case_iter_exhaust_commit(); }

TEST(odd_split_leaf) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     ids[32];
    size_t    pos;
    int       i;
    assertok(S != NULL && T != NULL);
    for (i = 0; i < 32; ++i) {
        pos = (size_t)i * 7u;
        assertok(at_seek(&C, T, pos) == AT_OK);
        assertok(att_checkcursor(&C, pos));
        assertok(at_newanchor(&C, 0, 0, AT_GROW, &ids[i]) == AT_OK);
        att_checkall(T, &C, pos);
    }
    assertok(att_checkdepth(T, 1));
    for (i = 0; i < 32; ++i) {
        assertok(at_delanchor(T, ids[i]) == AT_OK);
        att_checkall(T, &C, 0);
    }
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(odd_random_ops) {
    Count     c = {0};
    at_State *S = at_open(&count_alloc, &c);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    at_Id     ids[64];
    size_t    seed = 12345u, pos = 0;
    int       i, op, n = 0;
    assertok(S != NULL && T != NULL);
    for (i = 0; i < 400; ++i) {
        pos = att_lcg(&seed) % 1000u, op = (int)(att_lcg(&seed) % 3u);
        if (op == 0 && n < 64) {
            assertok(at_seek(&C, T, pos) == AT_OK);
            assertok(at_newanchor(&C, 0, 0, AT_GROW, &ids[n]) == AT_OK);
            n += 1;
        } else if (op == 1 && n > 0) {
            size_t k = att_lcg(&seed) % (size_t)n;
            assertok(at_delanchor(T, ids[k]) == AT_OK);
            ids[k] = ids[n - 1], n -= 1;
        } else {
            assertok(at_seek(&C, T, pos) == AT_OK);
            att_randomsplice(&C, &seed);
        }
        att_checkall(T, &C, pos);
    }
    for (i = 0; i < n; ++i) assertok(at_delanchor(T, ids[i]) == AT_OK);
    att_checkall(T, &C, 0);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

TEST(remove_params) { att_case_remove_params(); }
TEST(remove_cursor) { att_case_remove_cursor(); }
TEST(insert_cursor) { att_case_insert_cursor(); }
TEST(insert_cursor_growleft) { att_case_insert_cursor_growleft(); }
TEST(insert_cursor_splitmove) { att_case_insert_cursor_splitmove(); }
TEST(append_cursor) { att_case_append_cursor(); }
TEST(splice_atomic_oom) { att_case_splice_atomic_oom(); }
TEST(remove_append_equivalence) { att_case_remove_append_equivalence(); }
TEST(stitch_delete_cross_leaf) { att_case_stitch_delete_cross_leaf(); }
TEST(stitch_delete_multi_level) { att_case_stitch_delete_multi_level(); }
TEST(stitch_delete_tail) { att_case_stitch_delete_tail(); }
TEST(stitch_delete_same_leaf) { att_case_stitch_delete_same_leaf(); }
TEST(stitch_seam_cleared) { att_case_stitch_seam_cleared(); }
TEST(stitch_rt_cleared) { att_case_stitch_rt_cleared(); }
TEST(stitch_random_edit) { att_case_stitch_random_edit(); }
TEST(stitch_delete_boundary) { att_case_stitch_delete_boundary(); }
TEST(stitch_firstedge_keep) { att_case_stitch_firstedge_keep(); }
TEST(stitch_tail_seam_anchors) { att_case_stitch_tail_seam_anchors(); }
TEST(purge_insertion) { att_case_purge_insertion(); }
TEST(purge_delanchor) { att_case_purge_delanchor(); }
TEST(anchor_query) { att_case_anchor_query(); }

#include "anchortree_test_fanout5.gen.inc"
