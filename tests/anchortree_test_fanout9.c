/* anchortree_test_fanout9.c -- odd-fanout anchortree regressions.
 *
 * Only tests that depend on an odd AT_FANOUT live here; the fanout-independent
 * suite runs in anchortree_test_fanout4.c and the default-config stress tests
 * live in anchortree_test_fanout32.c. */
#define AT_FANOUT    9
#define AT_MAX_LEVEL 32 /* AT_FANOUT=9: see docs/max_levels.md */
#define AT_PAGE_SIZE 512
#define AT_STATIC_API

#include "../anchortree.h"
#include "at_tests.h"
#include "tests.h"

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
        assertok(at_checkcursor(&C, pos));
        assertok(at_newanchor(&C, 0, 0, AT_GROW, &ids[i]) == AT_OK);
        atT_checkall(T, &C, pos);
    }
    assertok(atT_checkdepth(T, 1));
    for (i = 0; i < 32; ++i) {
        assertok(at_delanchor(T, ids[i]) == AT_OK);
        atT_checkall(T, &C, 0);
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
        pos = atT_lcg(&seed) % 1000u, op = (int)(atT_lcg(&seed) % 3u);
        if (op == 0 && n < 64) {
            assertok(at_seek(&C, T, pos) == AT_OK);
            assertok(at_newanchor(&C, 0, 0, AT_GROW, &ids[n]) == AT_OK);
            n += 1;
        } else if (op == 1 && n > 0) {
            size_t k = atT_lcg(&seed) % (size_t)n;
            assertok(at_delanchor(T, ids[k]) == AT_OK);
            ids[k] = ids[n - 1], n -= 1;
        } else {
            assertok(at_seek(&C, T, pos) == AT_OK);
            atT_randomsplice(&C, &seed);
        }
        atT_checkall(T, &C, pos);
    }
    for (i = 0; i < n; ++i) assertok(at_delanchor(T, ids[i]) == AT_OK);
    atT_checkall(T, &C, 0);
    at_deltree(T), at_close(S);
    asserteq(c.live, 0);
}

#include "anchortree_test_fanout9.gen.inc"
