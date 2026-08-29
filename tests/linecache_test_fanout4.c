#define LC_LEAF_FANOUT 4
#define LC_FANOUT      4
#define LC_PAGE_SIZE   512
#define LC_STATIC_API
#ifndef LC_POOL_STATS
# define LC_POOL_STATS
#endif

#include "lc_tests.h"

/* T1: lifecycle */

TEST(lifecycle) {
    lc_State *s = lc_open(&test_alloc, NULL);
    lc_Cache *t1, *t2;
    assertok(s);
    t1 = lc_newcache(s);
    assertok(t1);
    asserteq(lc_breaks(t1), 0);
    asserteq(lc_bytes(t1), 0);
    t2 = lc_newcache(s);
    assertok(t2 && t1 != t2);
    lc_delcache(s, t1);
    lc_reset(s);
    t1 = lc_newcache(s);
    assertok(t1);
    asserteq(lc_breaks(t1), 0);
    lc_delcache(s, t1);
    lc_delcache(s, t2);
    lc_close(s);

    lc_reset(NULL);
    lc_close(NULL);

    /* default allocator (allocf == NULL) */
    {
        lc_State *s3 = lc_open(NULL, NULL);
        lc_Cache *c3;
        assertok(s3 != NULL);
        c3 = lc_newcache(s3);
        assertok(c3 != NULL);
        asserteq(lc_breaks(c3), 0);
        lc_delcache(s3, c3);
        lc_close(s3);
    }
    /* lc_open OOM */
    {
        int z = 0;
        asserteq(lc_open(&oom_alloc, &z), NULL);
    }
    /* lc_newcache OOM */
    {
        int       one = 1;
        lc_State *s2 = lc_open(&oom_alloc, &one);
        assertok(s2 != NULL);
        asserteq(lc_newcache(s2), NULL);
        lc_close(s2);
    }
}

TEST(scan_params) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    unsigned  brs[] = {0}, *pbrs = brs;

    asserteq(lc_scan(NULL, lc_scanner, &pbrs), LC_ERRPARAM);
    asserteq(lc_scan(c, NULL, &pbrs), LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(scan_basic) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    unsigned  empty[] = {0}, full[] = {5, 10, 15, 20, 0}, *pbrs;

    /* case 1: empty scanner on empty tree */
    c = lc_newcache(S);
    pbrs = empty;
    asserteq(lc_scan(c, lc_scanner, &pbrs), LC_OK);
    asserteq(lc_breaks(c), 0);
    asserteq(lc_bytes(c), 0);
    assertok(lc_checktree(c));
    lc_delcache(S, c);

    /* case 2: exactly one full leaf (4 breaks) */
    c = lc_newcache(S);
    pbrs = full;
    asserteq(lc_scan(c, lc_scanner, &pbrs), LC_OK);
    asserteq(lc_breaks(c), 4);
    asserteq(lc_bytes(c), 50);
    asserteq(c->levels, 0);
    asserteq(c->root.child_count, 1);
    assertok(lc_checktree(c));
    lc_delcache(S, c);

    lc_close(S);
}

TEST(scan_seek) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 40);
    assertok(lc_checktree(c));

    r = lc_seek(&C, c, 0);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 0);
    asserteq(lc_line(&C), 0);
    asserteq(lc_linelen(&C), 10);
    assertok(lc_checkcursor(&C, 0));

    r = lc_seek(&C, c, 15);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 15);
    asserteq(lc_line(&C), 1);
    assertok(lc_checkcursor(&C, 15));

    r = lc_seek(&C, c, 10);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 10);
    asserteq(lc_line(&C), 1);
    assertok(lc_checkcursor(&C, 10));

    r = lc_seek(&C, c, 25);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 25);
    asserteq(lc_line(&C), 2);
    assertok(lc_checkcursor(&C, 25));

    r = lc_seek(&C, c, 40);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 40);
    asserteq(lc_line(&C), 3);
    assertok(lc_checkcursor(&C, 40));

    /* re-scan: scan into already-populated tree */
    lc_scanV(c, 5, 10);
    asserteq(lc_breaks(c), 5);
    asserteq(lc_bytes(c), 55);
    assertok(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(scan_bulk) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {120, 1, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, lc_rscanner, &pbrs);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 120);
    asserteq(lc_bytes(c), 120);
    assertok(c->levels >= 2);
    assertok(lc_checktree(c));
    lc_seek(&C, c, 0);
    asserteq(lc_offset(&C), 0);
    asserteq(lc_line(&C), 0);
    assertok(lc_checkcursor(&C, 0));

    lc_seek(&C, c, 120);
    asserteq(lc_offset(&C), 120);
    asserteq(lc_line(&C), 120);
    assertok(lc_checkcursor(&C, 120));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(scan_append) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    unsigned  brs_a[] = {4, 10, 0}, *pa = brs_a;
    unsigned  brs_b[] = {5, 20, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, lc_rscanner, &pa);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 4);
    asserteq(lc_bytes(c), 40);
    r = lc_scan(c, lc_rscanner, &pb);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 9);
    asserteq(lc_bytes(c), 140);
    assertok(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(scan_oom_items) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[] = {10, 0}, *pbrs = brs;
    int       r, oom = 2;

    S = lc_open(&oom_alloc, &oom);
    if (!S) return;
    c = lc_newcache(S);
    r = lc_scan(c, lc_scanner, &pbrs);
    asserteq(r, LC_ERRMEM);
    assertok(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(scan_oom_flush) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[] = {170, 10, 0}, *pbrs = brs;
    int       r, oom = 3;
    S = lc_open(&oom_alloc, &oom);
    if (!S) return;
    c = lc_newcache(S);
    r = lc_scan(c, lc_rscanner, &pbrs);
    asserteq(r, LC_ERRMEM);
    assertok(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(scan_oom_build) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[] = {170, 1, 0}, *pbrs = brs;
    int       r, oom = 4;
    S = lc_open(&oom_alloc, &oom);
    if (!S) return;
    c = lc_newcache(S);
    r = lc_scan(c, lc_rscanner, &pbrs);
    asserteq(r, LC_ERRMEM);
    assertok(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

/* scan OOM after makechain: makechain deepens root, then lcL_new fails
 * -> lc_scan returns ERRMEM without fold/rebalance -> underfilled node */
TEST(scan_oom_unfolded) {
    int       oom = 100, r;
    lc_State *S = lc_open(&oom_alloc, &oom);
    lc_Cache *c;
    unsigned  brs[] = {10, 0}, *pbrs = brs;
    lc_Drain  d;

    c = cacheV(
            S, 0,
            botV(leafV(1, 1, 1, 1), leafV(1, 1, 1, 1), leafV(1, 1, 1, 1),
                 leafV(1, 1, 1, 1)));
    assertok(c);
    oom = 0, S->alloc_ud = &oom;
    d = lc_drainpool(&S->leaves);
    r = lc_scan(c, lc_scanner, &pbrs);
    asserteq(r, LC_ERRMEM);
    assertok(lc_checktree(c));
    lc_refillpool(&S->leaves, d);
    lc_delcache(S, c);
    lc_close(S);
}

/* scan beyond full tree: trigger makechain from<0 (root deepen) */
TEST(scan_deepen_root) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);

    lc_rscanV(c, 260, 1);
    asserteq(lc_breaks(c), 260);
    asserteq(lc_bytes(c), 260);
    assertok(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(scan_edge_makechain_empty) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    unsigned  lines[] = {16, 10, 0}, *p = lines;

    asserteq(lc_scan(c, lc_rscanner, &p), LC_OK);
    asserteq(lc_breaks(c), 16);
    asserteq(lc_bytes(c), 160);
    assertok(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(seek_params) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C, C2;
    memset(&C, 0, sizeof(C));

    asserteq(lc_seek(NULL, c, 0), LC_ERRPARAM);
    asserteq(lc_seek(&C, NULL, 0), LC_ERRPARAM);
    asserteq(lc_seekline(NULL, c, 0), LC_ERRPARAM);
    asserteq(lc_seekline(&C, NULL, 0), LC_ERRPARAM);
    lc_seek(&C2, c, 0);
    assertok(lc_checkcursor(&C2, 0));
    asserteq(lc_seekline(&C2, c, 1), LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(seek_pastleaf) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_rscanV(c, 8, 10);
    asserteq(lc_breaks(c), 8);
    assertok(lc_checktree(c));

    /* seek to offset past first leaf (first leaf has 4 breaks, 40 bytes) */
    r = lc_seek(&C, c, 45);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 45);
    asserteq(lc_line(&C), 4);
    assertok(lc_checkcursor(&C, 45));
    ;

    lc_delcache(S, c);
    lc_close(S);
}

TEST(seek_line_leaf) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assertok(lc_checktree(c));

    r = lc_seekline(&C, c, 0);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 0);
    asserteq(lc_line(&C), 0);
    asserteq(lc_linelen(&C), 10);
    assertok(lc_checkcursor(&C, 0));

    r = lc_seekline(&C, c, 1);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 10);
    asserteq(lc_line(&C), 1);
    asserteq(lc_linelen(&C), 15);
    assertok(lc_checkcursor(&C, 10));

    r = lc_seekline(&C, c, 3);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 40);
    asserteq(lc_line(&C), 3);
    assertok(lc_checkcursor(&C, 40));
    ;

    lc_delcache(S, c);
    lc_close(S);
}

TEST(seek_line_pastleaf) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_rscanV(c, 6, 10);
    asserteq(lc_breaks(c), 6);
    assertok(lc_checktree(c));

    r = lc_seekline(&C, c, 4);
    asserteq(r, LC_OK);
    asserteq(lc_line(&C), 4);
    assertok(lc_checkcursor(&C, lc_offset(&C)));
    ;

    lc_delcache(S, c);
    lc_close(S);
}

TEST(seek_edge) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    /* seek past end: locends, col = n - bytes */
    lc_scanV(c, 10, 15, 15);
    asserteq(lc_bytes(c), 40);
    lc_seek(&C, c, 100);
    asserteq(C.col, 60);
    assertok(lc_checkcursor(&C, 100));
    ;
    lc_delcache(S, c);

    /* seekline on empty tree (no breaks) */
    c = lc_newcache(S);
    asserteq(lc_seek(&C, c, 0), LC_OK);
    assertok(lc_checkcursor(&C, 0));
    ;
    asserteq(lc_seekline(&C, c, 0), LC_OK);
    assertok(lc_checkcursor(&C, 0));
    ;
    lc_delcache(S, c);

    lc_close(S);
}

TEST(advance_params) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);

    asserteq(lc_advance(NULL, 1), LC_ERRPARAM);
    {
        lc_Cursor C;
        memset(&C, 0, sizeof(C));
        asserteq(lc_advance(&C, 1), LC_ERRPARAM);
    }

    asserteq(lc_linelen(NULL), 0);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(advance_single) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assertok(lc_checktree(c));

    r = lc_seek(&C, c, 5);
    asserteq(r, LC_OK);
    r = lc_advance(&C, 0);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 5);
    r = lc_advance(&C, 10);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 15);
    asserteq(lc_line(&C), 1);
    assertok(lc_checkcursor(&C, 15));

    r = lc_advline(&C, 1);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 25);
    asserteq(lc_line(&C), 2);
    assertok(lc_checkcursor(&C, 25));

    /* backward within leaf */
    r = lc_advance(&C, -8);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 17);
    asserteq(lc_line(&C), 1);
    assertok(lc_checkcursor(&C, 17));

    /* clamp past end */
    r = lc_advance(&C, 100);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 117);
    assertok(lc_checkcursor(&C, 117));

    /* clamp before start */
    r = lc_advance(&C, -200);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 0);
    assertok(lc_checkcursor(&C, 0));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(advance_cross) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 10, 10);
    asserteq(lc_breaks(c), 10);
    assertok(lc_checktree(c));

    /* advance forward across leaf boundary */
    lc_seek(&cur, c, 35); /* end of first leaf's last gap */
    assertok(lc_checkcursor(&cur, 35));
    r = lc_advance(&cur, 10); /* cross into second leaf */
    asserteq(r, LC_OK);
    asserteq(lc_offset(&cur), 45);
    asserteq(lc_line(&cur), 4);
    assertok(lc_checkcursor(&cur, 45));

    /* advance backward across leaf boundary */
    r = lc_advance(&cur, -10);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&cur), 35);
    asserteq(lc_line(&cur), 3);
    assertok(lc_checkcursor(&cur, 35));

    /* advline forward across leaf boundary */
    lc_seek(&cur, c, 35); /* break 3, line 3 */
    assertok(lc_checkcursor(&cur, 35));
    r = lc_advline(&cur, 2); /* cross to line 5 (in second leaf) */
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 5);
    asserteq(lc_offset(&cur), 50);
    assertok(lc_checkcursor(&cur, 50));

    /* advline backward across leaf boundary */
    r = lc_advline(&cur, -2);
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 3);
    asserteq(lc_offset(&cur), 30);
    assertok(lc_checkcursor(&cur, 30));

    /* advline to start */
    r = lc_advline(&cur, -100);
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 0);
    asserteq(lc_offset(&cur), 0);
    assertok(lc_checkcursor(&cur, 0));

    /* advline to end (covers lcC_forwardline last-line path) */
    r = lc_advline(&cur, 100);
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 10);
    asserteq(lc_offset(&cur), 100);
    assertok(lc_checkcursor(&cur, 100));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(advance_brute) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, delta, dst;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL);
    assertok(S);
    c = lc_newcache(S);
    {
        unsigned  buf[] = {0, 0, 0};
        unsigned *p;
        buf[0] = (unsigned)n, buf[1] = 2;
        p = buf;
        lc_scan(c, lc_rscanner, &p);
    }
    assertok(lc_checktree(c));

    for (pos = 0; pos <= nb + 1; ++pos)
        for (delta = -nb - 1; delta <= nb + 1; ++delta) {
            lc_seek(&C, c, pos);
            lc_advance(&C, delta);
            dst = pos + delta < 0 ? 0 : pos + delta;
            if (!lc_checkcursor(&C, dst)) {
                test_log(
                        "advance pos=%d delta=%d off=%lu exp=%d\n", pos, delta,
                        test_lu(lc_offset(&C)), dst);
                lc_dumpcursor(&C, "after advance");
                abort();
            }
        }
    lc_delcache(S, c);
    lc_close(S);
}

TEST(advance_cov_skip_siblings) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 12, 10);
    asserteq(lc_breaks(c), 12);

    /* advance forward from leaf 0, skipping past leaf 1 entirely */
    lc_seek(&cur, c, 35); /* near end of leaf 0 */
    assertok(lc_checkcursor(&cur, 35));
    r = lc_advance(&cur, 55); /* skip past leaf 1 (40 bytes) into leaf 2 */
    asserteq(r, LC_OK);
    asserteq(lc_offset(&cur), 90);
    asserteq(lc_line(&cur), 9);
    assertok(lc_checkcursor(&cur, 90));

    /* advance backward from leaf 2, skipping past leaf 1 entirely */
    r = lc_advance(&cur, -55); /* skip past leaf 1 backward into leaf 0 */
    asserteq(r, LC_OK);
    asserteq(lc_offset(&cur), 35);
    asserteq(lc_line(&cur), 3);
    assertok(lc_checkcursor(&cur, 35));

    /* advance lines forward, skipping past full leaf */
    lc_seekline(&cur, c, 1); /* line 1, in leaf 0 */
    assertok(lc_checkcursor(&cur, 10));
    r = lc_advline(&cur, 8); /* skip past leaf 0 rest + full leaf 1 */
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 9);
    asserteq(lc_offset(&cur), 90);
    assertok(lc_checkcursor(&cur, 90));

    /* advance lines backward, skipping past full leaf */
    r = lc_advline(&cur, -8);
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 1);
    asserteq(lc_offset(&cur), 10);
    assertok(lc_checkcursor(&cur, 10));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(advline_params) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);

    asserteq(lc_advline(NULL, 1), LC_ERRPARAM);
    {
        lc_Cursor C;
        memset(&C, 0, sizeof(C));
        asserteq(lc_advline(&C, 1), LC_ERRPARAM);
    }
    {
        lc_Cursor C;
        lc_seek(&C, c, 0);
        asserteq(lc_advline(&C, 0), LC_OK);
    }

    lc_delcache(S, c);
    lc_close(S);
}

TEST(advline_cross) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 10);
    asserteq(lc_breaks(c), 8);

    /* advance lines across leaf boundary */
    lc_seekline(&cur, c, 2); /* line 2, offset 20 */
    assertok(lc_checkcursor(&cur, 20));
    r = lc_advline(&cur, 3); /* skip past leaf boundary (4 breaks in leaf 0) */
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 5);
    asserteq(lc_offset(&cur), 50);
    assertok(lc_checkcursor(&cur, 50));

    /* backward across leaf boundary */
    r = lc_advline(&cur, -4);
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 1);
    asserteq(lc_offset(&cur), 10);
    assertok(lc_checkcursor(&cur, 10));

    /* forward to last line */
    lc_seekline(&cur, c, 0);
    assertok(lc_checkcursor(&cur, 0));
    r = lc_advline(&cur, 100);
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 8);
    asserteq(lc_offset(&cur), 80);
    assertok(lc_checkcursor(&cur, 80));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(advline_zero) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 10);
    lc_seek(&cur, c, 23); /* line 2, col=3 */
    assertok(lc_checkcursor(&cur, 23));
    asserteq(lc_line(&cur), 2);
    asserteq(lc_col(&cur), 3);
    r = lc_advline(&cur, 0);
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 2);
    asserteq(lc_col(&cur), 0);
    lc_advance(&cur, 5);
    asserteq(lc_col(&cur), 5);
    asserteq(lc_line(&cur), 2);
    r = lc_advline(&cur, 0);
    asserteq(r, LC_OK);
    asserteq(lc_line(&cur), 2);
    asserteq(lc_col(&cur), 0);
    lc_delcache(S, c);
    lc_close(S);
}

TEST(advline_brute) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, delta, dst;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL);
    assertok(S);
    c = lc_newcache(S);
    {
        unsigned  buf[] = {0, 0, 0};
        unsigned *p;
        buf[0] = (unsigned)n, buf[1] = 2;
        p = buf;
        lc_scan(c, lc_rscanner, &p);
    }
    assertok(lc_checktree(c));

    for (pos = 0; pos <= nb + 1; ++pos)
        for (delta = -nb - 1; delta <= nb + 1; ++delta) {
            lc_seek(&C, c, pos);
            lc_advline(&C, delta);
            dst = (pos + delta * 2) & ~1;
            dst = dst < 0 ? 0 : dst > n * 2 ? n * 2 : dst;
            if (!lc_checkcursor(&C, dst)) {
                test_log(
                        "advance pos=%d delta=%d off=%lu failed exp=%d\n", pos,
                        delta, test_lu(lc_offset(&C)), dst);
                lc_dumpcursor(&C, "after advance");
                abort();
            }
        }
    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_params) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    asserteq(lc_markbreak(NULL, 1), LC_ERRPARAM);
    memset(&C, 0, sizeof(C));
    asserteq(lc_markbreak(&C, 1), LC_ERRPARAM);
    lc_seek(&C, c, 0);
    asserteq(lc_markbreak(&C, 0), LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_basic) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 20);
    asserteq(lc_breaks(c), 2);

    lc_seek(&cur, c, 0);
    r = lc_markbreak(&cur, 5);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_offset(&cur), 5);
    asserteq(lc_line(&cur), 1);
    assertok(lc_checkcursor(&cur, 5));

    /* verify by seeking past new break */
    lc_seek(&cur, c, 6);
    assertok(lc_checkcursor(&cur, 6));
    asserteq(lc_line(&cur), 1);

    /* extend line past next break (br > gap): set line length to 100 */
    lc_seek(&cur, c, 2);
    assertok(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 100);
    asserteq(r, LC_OK); /* cross-break extension: no error */
    assertok(lc_checkcursor(&cur, 102));

    /* null check */
    r = lc_markbreak(NULL, 1);
    asserteq(r, LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_brute) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, ins, r;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL);
    assertok(S);

    for (pos = 0; pos <= nb + 1; ++pos)
        for (ins = 1; ins <= n; ++ins) {
            c = lc_newcache(S);
            lc_rscanV(c, 128, 2); /* 128*2=256 bytes, levels>=2 */
            lc_seek(&C, c, pos);
            r = lc_markbreak(&C, ins);
            asserteq(r, LC_OK);
            if (!lc_checktree(c) || !lc_checkcursor(&C, pos + ins)) {
                test_log("insert pos=%d ins=%d failed\n", pos, ins);
                lc_dumptree(c, "insert brute fail");
                lc_dumpcursor(&C, "insert brute fail");
                abort();
            }
            if (lc_col(&C) != 0) {
                test_log("insert pos=%d ins=%d col=%u\n", pos, ins, lc_col(&C));
                lc_dumptree(c, "insert brute fail");
                lc_dumpcursor(&C, "insert brute fail");
                abort();
            }
            lc_delcache(S, c);
            asserteq(S->leaves.live_obj, 0);
            asserteq(S->nodes.live_obj, 0);
        }
    lc_close(S);
}

TEST(markbreak_empty) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    r = lc_seek(&cur, c, 0);
    asserteq(r, LC_OK);
    assertok(lc_checkcursor(&cur, 0));
    r = lc_markbreak(&cur, 10);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 1);
    asserteq(lc_bytes(c), 10);
    asserteq(lc_offset(&cur), 10);
    asserteq(lc_line(&cur), 1);
    assertok(lc_checkcursor(&cur, 10));
    asserteq(lc_linelen(&cur), 0); /* at break boundary, gap=0 */

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_crossline) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    int       r;

    /* case 1: large gap split at br=10, line1 [1..99) */
    c = lc_newcache(S);
    lc_scanV(c, 1, 99, 100, 100);
    asserteq(lc_breaks(c), 4);
    assertok(lc_checktree(c));
    lc_seekline(&C, c, 1);
    asserteq(lc_offset(&C), 1);
    asserteq(lc_line(&C), 1);
    asserteq(lc_linelen(&C), 99);
    assertok(lc_checkcursor(&C, 1));
    r = lc_markbreak(&C, 10);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 5);
    asserteq(lc_bytes(c), 300);
    asserteq(lc_offset(&C), 11);
    asserteq(lc_line(&C), 2);
    asserteq(lc_linelen(&C), 89);
    assertok(lc_checkcursor(&C, 11));
    lc_delcache(S, c);

    /* case 2: gap split at br=5, line1 offset 10, len=15 */
    c = lc_newcache(S);
    lc_scanV(c, 10, 15, 15);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 40);
    lc_seekline(&C, c, 1);
    asserteq(lc_offset(&C), 10);
    assertok(lc_checkcursor(&C, 10));
    r = lc_markbreak(&C, 5);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_bytes(c), 40);
    asserteq(lc_breaks(c), 4);
    assertok(lc_checkcursor(&C, 15));
    lc_delcache(S, c);

    lc_close(S);
}

TEST(markbreak_trailing) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15);
    asserteq(lc_breaks(c), 3);

    /* trailing gap: splice at end adds virtual bytes, lc_bytes unchanged */
    lc_seek(&cur, c, 40);
    assertok(lc_checkcursor(&cur, 40));
    lc_splice(&cur, 0, 20);
    asserteq(lc_offset(&cur), 60); /* 40 real + 20 virtual in col */
    assertok(lc_checkcursor(&cur, 60));
    asserteq(lc_bytes(c), 40);
    asserteq(lc_line(&cur), 3);

    r = lc_markbreak(&cur, 5);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 4);
    asserteq(lc_offset(&cur), 65);
    asserteq(lc_line(&cur), 4);
    assertok(lc_checkcursor(&cur, 65));
    asserteq(lc_linelen(&cur), 0);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_noop) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;
    lc_scanV(c, 10, 15, 15);
    asserteq(lc_breaks(c), 3);
    lc_seek(&cur, c, 5);
    assertok(lc_checkcursor(&cur, 5));
    assertok(lc_checktree(c));
    r = lc_markbreak(&cur, 10);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 40);
    assertok(lc_checkcursor(&cur, 15));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_brzero) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;

    lc_scanV(c, 10, 15, 15);
    asserteq(lc_breaks(c), 3);
    lc_seek(&cur, c, 5);
    asserteq(lc_markbreak(&cur, 0), LC_ERRPARAM);
    asserteq(lc_breaks(c), 3);
    assertok(lc_checkcursor(&cur, 5));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_crossleaf) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 1);
    asserteq(lc_breaks(c), 8);
    asserteq(lc_bytes(c), 8);
    assertok(lc_checktree(c));

    lc_seek(&cur, c, 0);
    assertok(lc_checkcursor(&cur, 0));
    r = lc_markbreak(&cur, 100);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_bytes(c), 100);
    asserteq(lc_breaks(c), 1);
    assertok(lc_checkcursor(&cur, 100));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_split) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 5, 10);
    asserteq(lc_breaks(c), 5);
    assertok(
            c->root.child_count
            > 1); /* leaf split: root now has 2 leaf children */

    /* add break to first gap in first leaf */
    lc_seek(&cur, c, 2);
    assertok(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 3);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 6);
    assertok(lc_checkcursor(&cur, 5));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_node_split) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 17, 10);
    asserteq(lc_breaks(c), 17);

    /* internal node has 5 children (> LC_FANOUT=4), so levels >= 2 */
    /* markbreak at offset 2: splits leaf, triggers internal node split */
    lc_seek(&cur, c, 2);
    assertok(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 3);
    asserteq(r, LC_OK);
    assertok(lc_checkcursor(&cur, 5));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_root_split) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 70, 10);
    asserteq(lc_breaks(c), 70);

    /* seek to first gap in first leaf, add break to trigger cascade */
    lc_seek(&cur, c, 2);
    assertok(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 3);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 71);
    assertok(lc_checkcursor(&cur, 5));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_root_add) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;
    lc_rscanV(c, 21, 10);
    asserteq(lc_breaks(c), 21);
    lc_seek(&cur, c, 2);
    assertok(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 3);
    asserteq(r, LC_OK);
    assertok(lc_checkcursor(&cur, 5));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_cascade) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       k, r;

    lc_rscanV(c, 200, 5);
    asserteq(lc_breaks(c), 200);

    for (k = 0; k < 24; ++k) {
        lc_seek(&cur, c, 2);
        assertok(lc_checkcursor(&cur, 2));
        r = lc_markbreak(&cur, 2);
        asserteq(r, LC_OK);
        if (k == 0) assertok(lc_checkcursor(&cur, 4));
    }

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_cov_split_right) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 10, 10, 10);
    asserteq(lc_breaks(c), 4);

    /* cursor at offset 25 generates slot=2 (>= mid=2), moves to new leaf */
    lc_seek(&cur, c, 25);
    assertok(lc_checkcursor(&cur, 25));
    r = lc_markbreak(&cur, 3);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 5);
    assertok(lc_checkcursor(&cur, 28));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_cov_child_right) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(3, 3, 3, 3), leafV(3, 3, 3, 3),
                               leafV(3, 3, 3, 3), leafV(3, 3, 3, 3)),
                          botV(leafV(3, 3, 3, 3), leafV(3, 3, 3, 3),
                               leafV(3, 3, 3, 3), leafV(3, 3, 3, 3)),
                          botV(leafV(3, 3, 3, 3), leafV(3, 3, 3, 3),
                               leafV(3, 3, 3, 3), leafV(3, 3, 3, 3)),
                          botV(leafV(3, 3, 3, 3), leafV(3, 3, 3, 3),
                               leafV(3, 3, 3, 3), leafV(3, 3, 3, 3))),
                   innerV(botV(leafV(3, 3, 3, 3)))));
    lc_Cursor C;
    int       r;
    lc_seek(&C, c, 168);
    assertok(lc_checkcursor(&C, 168));
    r = lc_markbreak(&C, 1);
    asserteq(r, LC_OK);
    assertok(lc_checkcursor(&C, 169));
    assertok(lc_checktree_allow_empty(c, 1));
    lc_delcache(S, c);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->leaves.live_obj, 0);
    lc_close(S);
}

TEST(markbreak_fullleaf_pastend) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 10, 10, 10);
    asserteq(lc_breaks(c), 4);
    lc_seek(&C, c, 40);
    asserteq(C.lnu, 4);
    asserteq(C.col, 0);
    r = lc_markbreak(&C, 3);
    asserteq(r, LC_OK);
    assertok(c->root.breaks[0] <= LC_LEAF_FANOUT);
    asserteq(lc_breaks(c), 5);
    asserteq(lc_bytes(c), 43);
    assertok(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(markbreak_fullleaf_brgt) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 10, 10, 10, 10, 10, 10, 10);
    asserteq(lc_breaks(c), 8);
    lc_seek(&C, c, 25);
    asserteq(C.lnu, 2);
    asserteq(C.col, 5);
    r = lc_markbreak(&C, 8);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 8);
    lc_asserttree(c, 0, botV(leafV(10, 10, 13, 7), leafV(10, 10, 10, 10)));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(clearbreaks_basic) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15, 15);
    asserteq(lc_breaks(c), 4);
    asserteq(lc_bytes(c), 55);
    assertok(lc_checktree(c));

    /* len == 0: no-op */
    lc_seek(&C, c, 5);
    r = lc_clearbreaks(&C, 0);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 4);
    asserteq(lc_linelen(&C), 10);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 5));

    /* clear exactly one break at break boundary */
    lc_seek(&C, c, 9);
    r = lc_clearbreaks(&C, 5);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_linelen(&C), 25);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 14));

    /* past end: del clamped, no breaks crossed */
    lc_seek(&C, c, 50);
    r = lc_clearbreaks(&C, 20);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_linelen(&C), 30);
    asserteq(lc_bytes(c), 40);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 70));

    /* clear all remaining breaks */
    lc_seek(&C, c, 5);
    r = lc_clearbreaks(&C, 40);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 0);
    asserteq(lc_linelen(&C), 45);
    asserteq(lc_bytes(c), 0);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 45));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(clearbreaks_cov_slot) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;
    lc_scanV(c, 10, 15, 15);
    asserteq(lc_breaks(c), 3);
    assertok(lc_checktree(c));
    lc_seek(&cur, c, 11);
    assertok(lc_checkcursor(&cur, 11));
    r = lc_clearbreaks(&cur, 16);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 2);
    assertok(lc_checkcursor(&cur, 27));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(remove_params) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C, R, X;
    memset(&C, 0, sizeof(C));
    memset(&X, 0, sizeof(X));

    asserteq(lc_remove(NULL, &R), LC_ERRPARAM);
    asserteq(lc_remove(&C, NULL), LC_ERRPARAM);
    asserteq(lc_remove(&C, &X), LC_ERRPARAM);
    lc_seek(&C, c, 0);
    lc_seek(&R, c, 0);
    asserteq(lc_remove(&C, &X), LC_ERRPARAM); /* X.tree==NULL != c */
    asserteq(lc_remove(&X, &C), LC_ERRPARAM); /* !X->tree */

    /* reversed -> no-op */
    lc_scanV(c, 10, 10);
    lc_seek(&C, c, 5);
    lc_seek(&R, c, 2);
    asserteq(lc_remove(&C, &R), LC_OK);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);

    /* L in trailing region (offset >= bytes) */
    lc_seek(&C, c, lc_bytes(c) + 3);
    lc_seek(&R, c, lc_bytes(c) + 8);
    asserteq(lc_remove(&C, &R), LC_OK);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(remove_basic) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C, R;

    /* remove all */
    c = lc_newcache(S);
    lc_rscanV(c, 100, 10);
    lc_seek(&C, c, 0);
    lc_seek(&R, c, 1000);
    lc_remove(&C, &R);
    asserteq(lc_breaks(c), 0);
    asserteq(lc_bytes(c), 0);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 0));
    lc_delcache(S, c);

    /* remove range -- keep first 11 + last 9 bytes */
    c = lc_newcache(S);
    lc_rscanV(c, 100, 10);
    lc_seek(&C, c, 11);
    lc_seek(&R, c, 991);
    lc_remove(&C, &R);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 11));
    lc_delcache(S, c);

    /* remove within single leaf */
    c = lc_newcache(S);
    lc_scanV(c, 10, 15, 15, 20);
    lc_seek(&C, c, 11);
    lc_seek(&R, c, 26);
    lc_remove(&C, &R);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 11));
    lc_delcache(S, c);

    /* remove across leaves */
    c = lc_newcache(S);
    lc_scanV(c, 5, 5, 5, 5, 5, 5, 5, 5);
    lc_seek(&C, c, 5);
    lc_seek(&R, c, 21);
    lc_remove(&C, &R);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 5));
    lc_delcache(S, c);

    lc_close(S);
}

/* R overshoots past the tree end (lc_advance col overshoot): rmleaf
 * deletes L's line to the leaf end and drops the virtual remainder
 * (fuzz lc_remove_repro.txt) */
TEST(remove_rend_overtail) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C, R;
    lc_scanV(c, 2, 10);
    lc_seek(&C, c, 6);
    R = C, lc_advance(&R, 19);
    asserteq(lc_remove(&C, &R), LC_OK);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 6));
    asserteq(lc_breaks(c), 1);
    asserteq(lc_bytes(c), 2);
    lc_delcache(S, c);
    lc_close(S);
}

/* virtual R across leaves (same container): trimleft's whole-leaf
 * branch and cutrange's breaks==0 skip consume the emptied tail leaf */
TEST(remove_rend_overtail_range) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C, R;
    lc_scanV(c, 5, 5, 5, 5, 5, 5, 5, 5);
    lc_seek(&C, c, 7);
    R = C, lc_advance(&R, 40);
    asserteq(lc_remove(&C, &R), LC_OK);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 7));
    asserteq(lc_breaks(c), 1);
    asserteq(lc_bytes(c), 5);
    lc_asserttree(c, 0, botV(leafV(5)));
    lc_delcache(S, c);
    lc_close(S);
}

/* virtual R across containers: cutrange's per-level loop skips the
 * emptied tail subtree at every level */
TEST(remove_rend_overtail_range2) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C, R;
    lc_scanV(c, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5);
    lc_seek(&C, c, 7);
    R = C, lc_advance(&R, 97);
    asserteq(lc_remove(&C, &R), LC_OK);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 7));
    asserteq(lc_breaks(c), 1);
    asserteq(lc_bytes(c), 5);
    lc_asserttree(c, 0, botV(leafV(5)));
    lc_delcache(S, c);
    lc_close(S);
}

/* leaf fold underfills a root child (cc 2->1): rebalance must fold at
 * the root level too (fuzz seed 7 op 219) */
TEST(remove_foldroot) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(4, 1, 3), leafV(1, 7, 1)),
                   botV(leafV(1, 1, 1, 1), leafV(1, 1, 1, 1))));
    lc_Cursor C, R;
    lc_seek(&C, c, 3);
    R = C, lc_advance(&R, 3);
    asserteq(lc_remove(&C, &R), LC_OK);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 3));
    lc_delcache(S, c);
    lc_close(S);
}

/* OOM before mutation: lc_remove must reserve nodes up front.  Cross-leaf
 * remove with the nodes pool drained makes lcD_stitch's reserve fail; the
 * old code hit that reserve only after trim/cutrange had already mutated
 * the tree, so the assert in lcD_stitch aborted on a half-deleted tree. */
TEST(remove_oom_prereserve) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C, R;
    void     *pn;
    int       oom = 0, r;
    size_t    bytes, breaks, snb;

    lc_rscanV(c, 8, 10); /* two leaves, cross-leaf remove frees no nodes */
    assertok(lc_checktree(c));
    bytes = lc_bytes(c), breaks = lc_breaks(c), snb = S->nodes.live_obj;
    (void)lc_drainpool(&S->nodes);
    pn = S->nodes.pages;
    S->nodes.pages = NULL;
    lc_seek(&C, c, 5);
    R = C, lc_advance(&R, 40);
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    r = lc_remove(&C, &R);
    S->allocf = test_alloc;
    S->alloc_ud = NULL;
    asserteq(r, LC_ERRMEM);
    asserteq(lc_bytes(c), bytes);
    asserteq(lc_breaks(c), breaks);
    asserteq(S->nodes.live_obj, snb);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 5));
    lc_restorepages(&S->nodes, pn);
    lc_delcache(S, c);
    lc_close(S);
}

/* lc_splice must propagate the remove OOM instead of asserting after a
 * partially-deleted tree. */
TEST(splice_oom_remove) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    void     *pn;
    int       oom = 0, r;
    size_t    bytes, breaks;

    lc_rscanV(c, 8, 10);
    assertok(lc_checktree(c));
    bytes = lc_bytes(c), breaks = lc_breaks(c);
    (void)lc_drainpool(&S->nodes);
    pn = S->nodes.pages;
    S->nodes.pages = NULL;
    lc_seek(&C, c, 5);
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    r = lc_splice(&C, 40, 0);
    S->allocf = test_alloc;
    S->alloc_ud = NULL;
    asserteq(r, LC_ERRMEM);
    asserteq(lc_bytes(c), bytes);
    asserteq(lc_breaks(c), breaks);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 5));
    lc_restorepages(&S->nodes, pn);
    lc_delcache(S, c);
    lc_close(S);
}

TEST(splice_params) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    memset(&C, 0, sizeof(C));

    lc_splice(NULL, 1, 1);
    lc_splice(&C, 1, 1);
    lc_seek(&C, c, 0);
    lc_splice(&C, 5, 3);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(splice_basic) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    lc_rscanV(c, 100, 10);
    asserteq(lc_breaks(c), 100);
    asserteq(lc_bytes(c), 1000);

    lc_seek(&C, c, 0);
    lc_splice(&C, 1000, 0); /* delete all */
    asserteq(lc_breaks(c), 0);
    asserteq(lc_bytes(c), 0);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 0));

    /* second scan on cleared tree */
    lc_rscanV(c, 100, 10);
    asserteq(lc_breaks(c), 100);
    asserteq(lc_bytes(c), 1000);
    lc_seek(&C, c, 11);
    lc_splice(&C, 980, 0); /* delete all but first 11 + last 9 */
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 11));

    lc_scanV(c, 5, 15);

    /* simple splice (no break crossing) */
    lc_seek(&C, c, 2);
    lc_splice(&C, 5, 3);
    asserteq(lc_bytes(c), 38);
    asserteq(lc_offset(&C), 5);
    assertok(lc_checkcursor(&C, 5));

    /* splice crossing breaks */
    lc_seek(&C, c, 0);
    lc_splice(&C, 15, 8);
    asserteq(lc_bytes(c), 31); /* 38 - 15 + 8 */
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 8));

    /* splice with del=0, ins=0 (no-op) */
    lc_splice(&C, 0, 0);
    assertok(lc_checktree(c));

    /* null check */
    lc_splice(NULL, 1, 1);
    assertok(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(splice_trailing) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    lc_scanV(c, 10, 15, 15);
    lc_scan(c, lc_scanner, NULL);

    /* after last break (trailing area): slot=3 (==breaks) */
    lc_seek(&C, c, 40);
    assertok(lc_checkcursor(&C, 40));
    lc_splice(&C, 0, 20); /* insert 20 bytes at end */
    assertok(lc_checktree(c));
    asserteq(lc_bytes(c), 40);   /* 40 is the last newline */
    asserteq(lc_offset(&C), 60); /* offset == line start + col */
    assertok(lc_checkcursor(&C, 60));

    /* verify seek within expanded trailing segment */
    lc_seek(&C, c, 45);
    assertok(lc_checkcursor(&C, 45));
    asserteq(lc_line(&C), 3);

    lc_delcache(S, c);

    /* past-end: delete in trailing area should not move col */
    c = lc_newcache(S);
    lc_scanV(c, 10, 15, 15);
    lc_seek(&C, c, 40);
    lc_splice(&C, 5, 0); /* delete 5 past end */
    asserteq(lc_offset(&C), 40);
    assertok(lc_checkcursor(&C, 40));
    lc_delcache(S, c);

    /* past-end: del+ins, only ins affects col */
    c = lc_newcache(S);
    lc_scanV(c, 10, 15, 15);
    lc_seek(&C, c, 40);
    lc_splice(&C, 5, 10); /* del=5 ins=10 past end */
    asserteq(lc_offset(&C), 50);
    assertok(lc_checkcursor(&C, 50));
    lc_delcache(S, c);

    lc_close(S);
}

/* splice_brute: exhaustive pos+del+ins enumeration on multi-level tree.
 * 128 lines of 2 bytes each = 256 total bytes, levels >= 2.
 * pos=0..257 (1 past end -> trailing), del=0..257 (past end -> clamp),
 * ins=0..1 (byte insert). */
TEST(splice_brute) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, del, ins;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL);
    assertok(S);

    for (pos = 0; pos <= nb + 1; ++pos)
        for (del = 0; del <= nb + 1; ++del)
            for (ins = 0; ins <= 1; ++ins) {
                c = lc_newcache(S);
                {
                    unsigned  buf[] = {0, 0, 0};
                    unsigned *p;
                    buf[0] = (unsigned)n, buf[1] = 2;
                    p = buf;
                    lc_scan(c, lc_rscanner, &p);
                }
                assertok(lc_checktree(c));
                lc_seek(&C, c, pos);
                lc_splice(&C, del, ins);
                if (!lc_checktree(c)) {
                    test_log(
                            "splice pos=%d del=%d ins=%d tree\n", pos, del,
                            ins);
                    lc_dumptree(c, "after splice");
                    abort();
                }
                if (!lc_checkcursor(&C, pos + ins)) {
                    test_log(
                            "splice pos=%d del=%d ins=%d off=%lu exp=%d\n", pos,
                            del, ins, test_lu(lc_offset(&C)), pos + ins);
                    lc_dumpcursor(&C, "after splice");
                    abort();
                }
                lc_delcache(S, c);
                asserteq(S->leaves.live_obj, 0);
                asserteq(S->nodes.live_obj, 0);
            }

    lc_close(S);
}

/* brute (pos,len) splices over mixed-shape levels=2 trees
 * (bot cc / leaf breaks 4/3/2 mixes stress fold/stitch/findroom paths) */
static lc_Node *brute3_bot(lc_State *S, const int *shape, int *si) {
    lc_Node *bot = (lc_Node *)lcP_alloc(S, &S->nodes);
    int      ci, li, cc = shape[(*si)++ % 7];
    for (ci = 0; ci < cc; ++ci) {
        lc_Leaf *lf = lcL_new(S);
        int      n = shape[(*si)++ % 7];
        size_t   lb = 0;
        for (li = 0; li < n; ++li)
            lb += lf->bytes[li] = shape[(*si)++ % 7] > 2 ? 2u : 1u;
        bot->children[ci] = (lc_Node *)lf;
        bot->bytes[ci] = lb, bot->breaks[ci] = (size_t)n;
    }
    return lcN_setcc(bot, cc), bot;
}

static lc_Cache *brute3_cache(lc_State *S, const int *shape) {
    lc_Node *root = (lc_Node *)lcP_alloc(S, &S->nodes);
    int      a, b, si = 0;
    for (a = 0; a < 4; ++a) {
        lc_Node *n1 = (lc_Node *)lcP_alloc(S, &S->nodes);
        for (b = 0; b < 4; ++b) {
            lc_Node *bot = brute3_bot(S, shape, &si);
            n1->children[b] = bot;
            n1->bytes[b] = lcN_sumbytes(bot, 0, lcN_cc(bot));
            n1->breaks[b] = lcN_sumbreaks(bot, 0, lcN_cc(bot));
        }
        lcN_setcc(n1, 4), root->children[a] = n1;
        root->bytes[a] = lcN_sumbytes(n1, 0, 4);
        root->breaks[a] = lcN_sumbreaks(n1, 0, 4);
    }
    return lcN_setcc(root, 4), cacheV(S, 2, root);
}

TEST(splice_brute3) {
    static const int shapes[3][7] = {
            {4, 3, 2, 4, 2, 3, 4},
            {2, 2, 3, 2, 4, 2, 2},
            {4, 4, 4, 4, 4, 4, 4}};
    lc_State *S = lc_open(&test_alloc, NULL);
    size_t    pos, len, total;
    int       si;
    for (si = 0; si < 3; ++si) {
        lc_Cache *c0 = brute3_cache(S, shapes[si]);
        total = lc_bytes(c0);
        lc_delcache(S, c0);
        for (pos = 0; pos < total; ++pos)
            for (len = 1; len <= total - pos; ++len) {
                lc_Cache *c = brute3_cache(S, shapes[si]);
                lc_Cursor C;
                size_t    hang;
                lc_seek(&C, c, pos);
                lc_splice(&C, len, 0);
                hang = pos >= lc_bytes(c) ? (size_t)C.col : 0;
                if (!lc_checktree_allow_empty(c, 1)
                    || lc_bytes(c) != total - len - hang
                    || !lc_checkcursor(&C, pos)) {
                    test_log(
                            "FAIL brute3 s=%d pos=%lu len=%lu bytes=%lu "
                            "exp=%lu\n",
                            si, test_lu(pos), test_lu(len),
                            test_lu(lc_bytes(c)), test_lu(total - len - hang));
                    lc_dumptree(c, "after splice");
                    lc_dumpcursor(&C, "after splice");
                    abort();
                }
                lc_delcache(S, c);
            }
    }
    lc_close(S);
}

/* cross-leaf splice with L->col (1) != R->col (0): del [1,4) of
 * lines {3,1 | 2,2} kills line A's break and line B; A's leading
 * byte must merge into line C -> {1+2, 2} */
TEST(splice_cross_col) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(3, 1), leafV(2, 2)));
    lc_Cursor C;
    lc_seek(&C, c, 1);
    lc_splice(&C, 3, 0);
    assertok(lc_checktree_allow_empty(c, 1));
    asserteq(lc_bytes(c), 5);
    asserteq(lc_breaks(c), 2);
    assertok(lc_checkcursor(&C, 1));
    checkleavesV(c, 1, 3, 1, 2);
    lc_delcache(S, c);
    lc_close(S);
}

TEST(splice_cov_rebalance) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2, 2), leafV(2, 2, 2)),
                          botV(leafV(2, 2), leafV(2, 2))),
                   innerV(botV(leafV(2)))));
    lc_Cursor C;
    /* L at offset 0 leaf0[2,2]: splice del=3 -> leaf becomes [1]
     * underfull -> foldleaf merge -> botV0.cc=1 -> rebalance(1)
     * -> foldnode at inner0: cl=1 cr=2 -> merge (returns 1);
     * root-level fold merges inner0+inner1 -> root collapses (levels 1) */
    lc_seek(&C, c, 0);
    lc_splice(&C, 3, 0);
    lc_asserttree(
            c, 1,
            innerV(botV(leafV(1, 2, 2, 2), leafV(2, 2), leafV(2, 2)),
                   botV(leafV(2))));
    assertok(lc_checktree_allow_empty(c, 1));
    assertok(lc_checkcursor(&C, 0));
    ;
    lc_delcache(S, c);
    lc_close(S);
}

/* foldleaf balance cl+cg>4: via cross-leaf splice triggering stitch+foldleaf */
TEST(splice_cov_foldleaf_lr) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10), leafV(10, 10, 10, 10)));
    lc_seek(&C, c, 10);   /* left leaf lnu=1, cross into right leaf */
    lc_splice(&C, 11, 0); /* delete 11 bytes -> cross leaf, trim left */
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 10));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(splice_cov_shiftnode_bal0) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    lc_Cursor C;
    lc_seek(&C, c, 25);
    lc_splice(&C, 16, 0);
    assertok(lc_checktree_allow_empty(c, 1));
    assertok(lc_checkcursor(&C, 25));
    lc_delcache(S, c);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->leaves.live_obj, 0);
    lc_close(S);
}

TEST(splice_cov_trimleaf) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 0), leafV(10, 0), leafV(10, 0)));
    lc_seek(&cur, lc_nonnull(c), 5);
    lc_splice(&cur, 25, 0);
    asserteq(c->root.child_count, 0);
    asserteq(c->bytes, 0);
    asserteq(c->breaks, 0);
    assertok(lc_checktree_allow_empty(c, 1));
    assertok(lc_checkcursor(&cur, 5));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_params) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       v;

    S = lc_open(&test_alloc, NULL);
    assertok(S);
    c = lc_newcache(S);

    asserteq(lc_append(NULL, 0, lc_scanner, &pz), LC_ERRPARAM);
    memset(&C, 0, sizeof(C));
    asserteq(lc_append(&C, 0, lc_scanner, &pz), LC_ERRPARAM);
    lc_seek(&C, c, 0);
    lc_scanV(c, 5, 10);
    lc_seek(&C, c, 3);
    v = lc_append(&C, 0, NULL, NULL);
    asserteq(v, LC_OK);
    asserteq(lc_offset(&C), 3);
    asserteq(lc_bytes(c), 15);
    asserteq(lc_breaks(c), 2);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_leaf) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {3, 3, 0}, *pb = brs;
    int       r;

    lc_scanV(c, 10, 15, 15);

    lc_seek(&C, c, 10);
    r = lc_append(&C, 3, lc_scanner, &pb);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 5);
    asserteq(lc_bytes(c), 49);
    lc_asserttree(c, 0, botV(leafV(10, 3, 3), leafV(18, 15)));
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 19));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_col) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {4, 4, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 4, 7);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 11);

    lc_seek(&C, c, 6);
    asserteq(C.lnu, 1);
    asserteq(C.col, 2);
    r = lc_append(&C, 3, lc_scanner, &pb);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    lc_asserttree(c, 0, botV(leafV(4, 2 + 4, 4, 3 + 5)));
    asserteq(lc_breaks(c), 4);
    asserteq(lc_bytes(c), 22);
    assertok(lc_checkcursor(&C, 17));
    ;
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_basic) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    lc_scanV(c, 10, 10);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);

    lc_seek(&C, c, 5);
    r = lc_append(&C, 7, lc_scanner, &pz);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 27);
    asserteq(lc_linelen(&C), 17);
    assertok(lc_checkcursor(&C, 12));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_many) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {17, 1, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 10, 10, 10);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 30);

    lc_seek(&C, c, 5);
    r = lc_append(&C, 0, lc_rscanner, &pb);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 20);
    asserteq(lc_bytes(c), 47);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 22));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_empty) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs[] = {10, 10, 10, 0}, zero[] = {0}, *pbrs;
    int       r;

    /* case 1: insert with breaks into empty tree */
    c = lc_newcache(S);
    pbrs = brs;
    lc_seek(&C, c, 0);
    r = lc_append(&C, 5, lc_scanner, &pbrs);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 30);
    assertok(lc_checkcursor(&C, 35));
    lc_delcache(S, c);

    /* case 2: empty tree, e=0, scanner returns 0 (no-op) */
    c = lc_newcache(S);
    pbrs = zero;
    lc_seek(&C, c, 0);
    r = lc_append(&C, 0, lc_scanner, &pbrs);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 0);
    asserteq(lc_bytes(c), 0);
    assertok(lc_checkcursor(&C, 0));
    lc_delcache(S, c);

    lc_close(S);
}

TEST(append_sib) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 0, botV(leafV(1, 0), leafV(2, 0), leafV(3, 0), NULL));
    lc_Cursor C;
    unsigned  brs[] = {4, 0}, *p = brs;
    lc_seek(&C, c, 0);
    asserteq(lc_append(&C, 0, lc_scanner, &p), LC_OK);
    assertok(lc_checktree_allow_empty(c, 1));
    assertok(lc_checkcursor(&C, 4));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_deep) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    lc_Node  *bot[8];
    unsigned  brs[] = {1, 0}, *p = brs;
    int       i;

    for (i = 0; i < 8; i++)
        bot[i] = botV(leafV(1), leafV(1), leafV(1), leafV(1));
    c = cacheV(
            S, 2,
            innerV(innerV(bot[0], bot[1], bot[2], bot[3]),
                   innerV(bot[4], bot[5], bot[6], bot[7])));
    assertok(lc_checktree_allow_empty(c, 1));

    lc_seek(&C, c, 1);
    asserteq(lc_append(&C, 0, lc_scanner, &p), LC_OK);
    assertok(lc_checktree_allow_empty(c, 1));
    assertok(lc_checkcursor(&C, 2));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_leaf_split) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {3, 3, 3, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 5, 5, 5);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 15);

    lc_seek(&C, c, 5);
    r = lc_append(&C, 2, lc_scanner, &pb);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 6);
    asserteq(lc_bytes(c), 26);
    assertok(lc_checkcursor(&C, 16));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_stitch_shiftup) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(1), leafV(1), leafV(1), leafV(1)));
    lc_Cursor C;
    unsigned  brs[] = {0}, *p = brs;
    lc_seek(&C, c, 1);
    asserteq(lc_append(&C, 0, lc_scanner, &p), LC_OK);
    lc_asserttree(c, 0, botV(leafV(1), leafV(1, 1), leafV(1)));
    assertok(lc_checkcursor(&C, 1));
    ;
    assertok(lc_checktree_allow_empty(c, 1));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_rootpush) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(1), leafV(1), leafV(1), leafV(1)));
    lc_Cursor C;
    unsigned  brs[] = {2, 2, 2, 2, 2, 0}, *p = brs;
    lc_seek(&C, c, 1);
    asserteq(lc_append(&C, 0, lc_scanner, &p), LC_OK);
    assertok(lc_checktree_allow_empty(c, 1));
    assertok(lc_checkcursor(&C, 11));
    ;
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_findroom_findlevel) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor cur;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    c = cacheV(
            S, 1,
            innerV(botV(leafV(2), leafV(2)),
                   botV(leafV(2), leafV(2), leafV(2), leafV(2)),
                   botV(leafV(2), leafV(2)), botV(leafV(2), leafV(2))));
    assertok(lc_checktree_allow_empty(c, 1));
    lc_seek(&cur, c, 6);
    r = lc_append(&cur, 0, lc_scanner, &pz);
    asserteq(r, LC_OK);
    assertok(lc_checktree_allow_empty(c, 1));
    assertok(lc_checkcursor(&cur, 6));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_noop) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    lc_scanV(c, 10, 10);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);

    lc_seek(&C, c, 5);
    r = lc_append(&C, 0, lc_scanner, &pz);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);
    assertok(lc_checkcursor(&C, 5));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_trailing) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {5, 5, 0}, *pb = brs;
    int       r;

    lc_scanV(c, 10, 10);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);

    lc_seek(&C, c, 25);
    asserteq(lc_offset(&C), 25);
    asserteq(C.col, 5);
    r = lc_append(&C, 7, lc_scanner, &pb);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    lc_asserttree(c, 0, botV(leafV(10, 10, 10, 5)));
    assertok(lc_checkcursor(&C, 35 + 7));
    ;
    asserteq(lc_breaks(c), 4);
    asserteq(lc_bytes(c), 35);
    lc_delcache(S, c);
    lc_close(S);
}

/* scanner: returns `len` for first `*n` calls, then 0. ud = &n (mutable). */
static unsigned brute_scanner(void *ud, size_t pos) {
    int *n = (int *)ud;
    (void)pos;
    if (*n <= 0) return 0;
    return (*n)--, 3;
}

TEST(append_brute) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, ins, e, rem, r;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL);
    assertok(S);

    for (pos = 0; pos <= nb + 1; ++pos)
        for (ins = 0; ins <= n; ++ins)
            for (e = 0; e <= 1; ++e) {
                c = lc_newcache(S);
                lc_rscanV(c, 128, 2); /* 128*2=256 bytes, levels>=2 */
                lc_seek(&C, c, pos);
                rem = ins;
                r = lc_append(&C, e, brute_scanner, &rem);
                asserteq(r, LC_OK);
                if (!lc_checktree(c)
                    || !lc_checkcursor(&C, pos + ins * 3 + e)) {
                    test_log("insert pos=%d ins=%d e=%d failed\n", pos, ins, e);
                    lc_dumptree(c, "insert brute fail");
                    lc_dumpcursor(&C, "insert brute fail");
                    abort();
                }
                lc_delcache(S, c);
                asserteq(S->leaves.live_obj, 0);
                asserteq(S->nodes.live_obj, 0);
            }
    lc_close(S);
}

TEST(append_noscanner) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    size_t    bb, br;

    /* empty tree: e bytes in trailing */
    lc_seek(&C, c, 0);
    asserteq(lc_append(&C, 3, NULL, NULL), LC_OK);
    asserteq(lc_offset(&C), 3);
    asserteq(lc_bytes(c), 0);
    asserteq(lc_breaks(c), 0);

    /* non-empty, valid line: e bytes added to current line */
    lc_scanV(c, 5, 10);
    lc_seek(&C, c, 3);
    asserteq(lc_append(&C, 7, NULL, NULL), LC_OK);
    asserteq(lc_offset(&C), 10);
    asserteq(lc_col(&C), 10);
    asserteq(lc_linelen(&C), 12);
    asserteq(lc_bytes(c), 22);

    /* trailing region: C->col += e, tree unchanged */
    bb = lc_bytes(c), br = lc_breaks(c);
    lc_seek(&C, c, bb + 5);
    asserteq(lc_offset(&C), bb + 5);
    asserteq(lc_append(&C, 8, NULL, NULL), LC_OK);
    asserteq(lc_offset(&C), bb + 5 + 8);
    asserteq(lc_bytes(c), bb);
    asserteq(lc_breaks(c), br);

    /* e=0 at valid line: no-op */
    bb = lc_bytes(c);
    lc_seek(&C, c, 0);
    asserteq(lc_append(&C, 0, NULL, NULL), LC_OK);
    asserteq(lc_offset(&C), 0);
    asserteq(lc_bytes(c), bb);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_oom_brute) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    int       pos, ins, e, oom, rem, r;
    int       cnt = 0;
    int const n = 32;

    assertok(S);
    for (pos = 0; pos <= n * 2 + 1; ++pos)
        for (ins = 0; ins <= n; ++ins)
            for (e = 0; e <= 1; ++e)
                for (oom = 0; oom <= 10; ++oom) {
                    int o = oom;
                    c = lc_newcache(S);
                    lc_rscanV(c, 64, 2);
                    assertok(lc_checktree(c));
                    lc_seek(&C, c, pos);
                    (void)lc_drainpool(&S->nodes);
                    (void)lc_drainpool(&S->leaves);
                    rem = ins;
                    S->allocf = oom_alloc;
                    S->alloc_ud = &o;
                    r = lc_append(&C, e, brute_scanner, &rem);
                    S->allocf = test_alloc;
                    S->alloc_ud = NULL;
                    if (r == LC_ERRMEM) {
                        if (!lc_checktree(c)
                            || !lc_checkcursor(&C, (size_t)pos)) {
                            test_log(
                                    "OOM brute fail pos=%d ins=%d e=%d"
                                    " oom=%d\n",
                                    pos, ins, e, oom);
                            lc_dumptree(c, "oom brute fail");
                            lc_dumpcursor(&C, "oom brute fail");
                            abort();
                        }
                        lc_delcache(S, c);
                        ++cnt;
                        continue;
                    }
                    asserteq(r, LC_OK);
                    lc_delcache(S, c);
                }
    test_log("  test_append_oom_brute: %d OOM cases\n", cnt);
    lc_close(S);
}

TEST(append_oom_trailing) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs[] = {2, 0}, *pbrs = brs;
    int       found = 0, k;
    for (k = 2; k <= 10; ++k) {
        int oom = k;
        S = lc_open(&oom_alloc, &oom);
        if (!S) continue;
        c = lc_newcache(S);
        if (!c) {
            lc_close(S);
            continue;
        }
        {
            size_t slb = S->leaves.live_obj, snb = S->nodes.live_obj;
            lc_seek(&C, c, 0);
            pbrs = brs;
            if (lc_append(&C, 0, lc_scanner, &pbrs) == LC_ERRMEM) {
                assertok(lc_checktree(c));
                assertok(lc_checkcursor(&C, 0));
                asserteq(S->leaves.live_obj, slb);
                asserteq(S->nodes.live_obj, snb);
                found = 1;
            }
        }
        lc_delcache(S, c);
        lc_close(S);
        if (found) break;
    }
    assertok(found);
}

TEST(append_oom_normal) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    lc_Leaf   lfdum;
    int       oom = 0;
    void     *lf;
    lc_Drain  ld, nd;
    void     *pl, *pn;

    assertok(S);
    c = cacheV(S, 0, botV(leafV(5, 5)));
    assertok(lc_checktree(c));

    ld = lc_drainpool(&S->leaves);
    nd = lc_drainpool(&S->nodes);
    pl = S->leaves.pages, pn = S->nodes.pages;
    S->leaves.pages = NULL;
    S->nodes.pages = NULL;
    lc_localfill(&S->leaves, &lf, &lfdum, 1);

    {
        size_t   slb = S->leaves.live_obj, snb = S->nodes.live_obj;
        unsigned brs[] = {17, 1, 0}, *pb = brs;
        S->allocf = oom_alloc, S->alloc_ud = &oom;
        lc_seek(&C, c, 3);
        asserteq(lc_append(&C, 0, lc_rscanner, &pb), LC_ERRMEM);
        assertok(lc_checktree(c));
        assertok(lc_checkcursor(&C, 3));
        asserteq(S->leaves.live_obj, slb);
        asserteq(S->nodes.live_obj, snb);
    }

    (void)lc_drainpool(&S->leaves);
    lc_refillpool(&S->leaves, ld);
    lc_refillpool(&S->nodes, nd);
    lc_restorepages(&S->leaves, pl), lc_restorepages(&S->nodes, pn);
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_oom_col0) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs_b[] = {1, 1, 1, 1, 1, 0}, *pb;
    int       oom = 0;
    void     *head;
    lc_Drain  ld;

    assertok(S);
    c = cacheV(S, 0, botV(leafV(5, 5)));
    assertok(lc_checktree(c));

    ld = lc_drainpool(&S->leaves);
    if (ld.count > 1) {
        int i;
        head = ld.chain;
        for (i = 0; i < (int)ld.count - 1; i++) head = *(void **)head;
        *(void **)head = NULL;
        ld.chain = head;
        ld.count = 1;
    }
    lc_refillpool(&S->leaves, ld);

    lc_seek(&C, c, 0);
    {
        size_t slb = S->leaves.live_obj, snb = S->nodes.live_obj;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        pb = brs_b;
        asserteq(lc_append(&C, 0, lc_scanner, &pb), LC_ERRMEM);
        S->allocf = test_alloc;
        S->alloc_ud = NULL;
        assertok(lc_checktree(c));
        assertok(lc_checkcursor(&C, 0));
        lc_asserttree(c, 0, botV(leafV(5, 5)));
        asserteq(S->leaves.live_obj, slb);
        asserteq(S->nodes.live_obj, snb);
    }

    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_oom_shiftup) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    lc_Leaf   lfdum;
    int       oom = 0;
    void     *lf;
    lc_Drain  ld, nd;
    void     *pl, *pn;

    assertok(S);
    c = cacheV(
            S, 0,
            botV(leafV(1, 0), leafV(2, 0), leafV(3, 0), leafV(4, 0), NULL));
    assertok(lc_checktree_allow_empty(c, 1));

    ld = lc_drainpool(&S->leaves);
    nd = lc_drainpool(&S->nodes);
    pl = S->leaves.pages, pn = S->nodes.pages;
    S->leaves.pages = NULL;
    S->nodes.pages = NULL;
    lc_localfill(&S->leaves, &lf, &lfdum, 1);

    {
        unsigned brs[] = {1, 1, 1, 1, 1, 0}, *p = brs;
        size_t   slb = S->leaves.live_obj, snb = S->nodes.live_obj;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        lc_seek(&C, c, 1);
        asserteq(lc_append(&C, 0, lc_scanner, &p), LC_ERRMEM);
        assertok(lc_checktree_allow_empty(c, 1));
        assertok(lc_checkcursor(&C, 1));
        asserteq(S->leaves.live_obj, slb);
        asserteq(S->nodes.live_obj, snb);
    }

    lc_restorepages(&S->leaves, pl), lc_restorepages(&S->nodes, pn);
    (void)lc_drainpool(&S->leaves);
    lc_refillpool(&S->leaves, ld);
    lc_refillpool(&S->nodes, nd);
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_oom_rootpush) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    lc_Node  *b[4];
    int       oom = 0, i;
    lc_Drain  nd;
    void     *pn;

    assertok(S);
    for (i = 0; i < 4; i++) b[i] = botV(leafV(1), leafV(1), leafV(1), leafV(1));
    c = cacheV(S, 1, innerV(b[0], b[1], b[2], b[3]));
    assertok(lc_checktree_allow_empty(c, 1));

    nd = lc_drainpool(&S->nodes);
    pn = S->nodes.pages;
    S->nodes.pages = NULL;

    {
        unsigned bs[49], *p = (unsigned *)bs;
        size_t   snb = S->nodes.live_obj;
        for (i = 0; i < 48; i++) bs[i] = 1;
        bs[48] = 0;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        lc_seek(&C, c, 1);
        asserteq(lc_append(&C, 0, lc_scanner, &p), LC_ERRMEM);
        assertok(lc_checktree_allow_empty(c, 1));
        assertok(lc_checkcursor(&C, 1));
        asserteq(S->nodes.live_obj, snb);
    }

    lc_restorepages(&S->nodes, pn);
    lc_refillpool(&S->nodes, nd);
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_oom_deroot) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    int       oom = 0;
    lc_Drain  nd;
    void     *pn;

    assertok(S);
    c = cacheV(
            S, 0,
            botV(leafV(1, 0), leafV(1, 0), leafV(1, 0), leafV(1, 0), NULL));
    assertok(lc_checktree_allow_empty(c, 1));
    {
        nd = lc_drainpool(&S->nodes);
        if (nd.count > 1) {
            int   i;
            void *p = nd.chain;
            for (i = 0; i < (int)nd.count - 1; i++) p = *(void **)p;
            *(void **)p = NULL;
            nd.chain = p;
            nd.count = 1;
        }
        lc_refillpool(&S->nodes, nd);
    }
    pn = S->nodes.pages;
    S->nodes.pages = NULL;

    {
        unsigned brs[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0}, *p = brs;
        size_t   snb = S->nodes.live_obj;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        lc_seek(&C, c, 1);
        asserteq(lc_append(&C, 0, lc_scanner, &p), LC_ERRMEM);
        assertok(lc_checktree_allow_empty(c, 1));
        assertok(lc_checkcursor(&C, 1));
        asserteq(S->nodes.live_obj, snb);
    }

    lc_restorepages(&S->nodes, pn);
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_oom_rollback) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    void     *pn;

    assertok(S);
    c = cacheV(S, 0, botV(leafV(1, 0), leafV(1, 0), leafV(1, 0), leafV(1, 0)));

    (void)lc_drainpool(&S->nodes);
    pn = S->nodes.pages;
    S->nodes.pages = NULL;

    {
        unsigned bs[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0}, *p = bs;
        int      oom = 1, r;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        lc_seek(&C, c, 1);
        p = bs;
        r = lc_append(&C, 0, lc_scanner, &p);
        S->allocf = test_alloc;
        S->alloc_ud = NULL;
        assertok(r < 0);
        assertok(lc_checktree_allow_empty(c, 1));
        assertok(lc_checkcursor(&C, 1));
        lc_asserttree(
                c, 0, botV(leafV(1, 0), leafV(1, 0), leafV(1, 0), leafV(1, 0)));
    }

    lc_restorepages(&S->nodes, pn);
    lc_delcache(S, c);
    lc_close(S);
}

/* stitch reserve: full 256-seg tree, seek 254, insert 48*1b.
 * freelists cleared -> every page alloc goes through oom_alloc.
 * O(1) reserve: oom=3 fails (findroom+stitch need 4 allocfs);
 * oom=4 succeeds. */
TEST(append_oom_full) {
    unsigned  ins[] = {45, 1, 0};
    unsigned *p;
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       oom, r;

    /* oom=3: cutleaf(1 leaf) + findroom reserve(1 node) + stitch reserve(2
     * node) = 4. 4th allocf fails -> OOM -> rollback. */
    S = lc_open(&test_alloc, NULL);
    c = lc_newcache(S);
    lc_rscanV(c, 256, 1);
    assertok(lc_checktree(c));
    (void)lc_drainpool(&S->nodes);
    (void)lc_drainpool(&S->leaves);
    oom = 3;
    p = ins;
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    lc_seek(&C, c, 254);
    r = lc_append(&C, 0, lc_rscanner, &p);
    S->allocf = test_alloc, S->alloc_ud = NULL;
    asserteq(r, LC_ERRMEM);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 254));
    lc_delcache(S, c);
    lc_close(S);

    /* oom=4: stitch gets its page -> insert succeeds.
     * New reserve O(1): mix of findroom reserve(3) + stitch reserve(5)
     * needs 4 allocfs total (vs 5 in old O(n) reserve). */
    S = lc_open(&test_alloc, NULL);
    c = lc_newcache(S);
    lc_rscanV(c, 256, 1);
    assertok(lc_checktree(c));
    (void)lc_drainpool(&S->nodes);
    (void)lc_drainpool(&S->leaves);
    oom = 4;
    p = ins;
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    lc_seek(&C, c, 254);
    r = lc_append(&C, 0, lc_scanner, &p);
    S->allocf = test_alloc;
    S->alloc_ud = NULL;
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

/* splice delete all with insertion triggers lcD_reset */
TEST(splice_reset) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    lc_scanV(c, 10, 15, 15);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 40);
    lc_seek(&C, c, 0);
    assertok(lc_checkcursor(&C, 0));
    lc_splice(&C, 40, 0);
    asserteq(lc_breaks(c), 0);
    asserteq(lc_bytes(c), 0);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 0));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(splice_cov_foldleaf_rl) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10), leafV(10, 10, 10, 10)));
    lc_Cursor C;
    lc_seek(&C, (assert(c), c), 20); /* start of right leaf, lnu=0 */
    lc_splice(&C, 10, 0);            /* cl+cr=5 > 4, balance dl<0 */
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 20));
    lc_delcache(S, c);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->leaves.live_obj, 0);
    lc_close(S);
}

/* rebalance early exit: foldnode returns 0 (balance, not merge).
 * botV[0] underfull after foldleaf (1 leaf), botV[1] has 4 leaves,
 * 1+4=5 > 4 -> balance -> foldnode returns 0 -> rebalance returns. */
TEST(splice_rebalance_earlyexit) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2, 2), leafV(2)),
                          botV(leafV(2), leafV(2), leafV(2), leafV(2))),
                   innerV(botV(leafV(2)))));
    lc_Cursor C;
    lc_seek(&C, lc_nonnull(c), 0);
    lc_splice(&C, 2, 0);
    lc_asserttree(
            c, 2,
            innerV(innerV(botV(leafV(2, 2), leafV(2), leafV(2)),
                          botV(leafV(2), leafV(2))),
                   innerV(botV(leafV(2)))));
    assertok(lc_checktree_allow_empty(c, 1));
    assertok(lc_checkcursor(&C, 0));
    lc_delcache(S, c);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->leaves.live_obj, 0);
    lc_close(S);
}

/* markbreak at right-half of fully packed tree -> root split with i>=mid */
TEST(markbreak_cov_rootright) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_rscanV(c, 64, 10);
    asserteq(lc_breaks(c), 64);
    assertok(lc_checktree(c));

    lc_seek(&C, c, 330);
    assertok(lc_checkcursor(&C, 330));
    r = lc_markbreak(&C, 3);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

/* insert with root-deepening to exercise fixsource dl>0 */
/* OOM in lcB_oneline: leaf allocation fails (line 898).
 * Use oom_alloc with counter=2: lc_open+l_newtree succeed, lcL_new fails. */
TEST(markbreak_oom_oneline) {
    int       oom = 2;
    lc_State *S = lc_open(&oom_alloc, &oom);
    lc_Cache *c;
    lc_Cursor C;
    int       r;
    if (!S) return;
    c = lc_newcache(S);
    if (!c) {
        lc_close(S);
        return;
    }
    lc_seek(&C, c, 0);
    r = lc_markbreak(&C, 10);
    asserteq(r, LC_OK); /* oneline return discarded by comma operator */
    asserteq(S->leaves.live_obj, 0);
    asserteq(S->nodes.live_obj, 0);
    lc_delcache(S, c);
    lc_close(S);
}

/* OOM in lcB_makeroom: leaf allocation fails (line 970).
 * Tree with one full leaf, leaves pool drained, markbreak triggers makeroom. */
TEST(markbreak_oom_makeroom) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(3, 3, 3, 3)));
    lc_Cursor C;
    int       r, oom = 0;
    void     *pl;
    assertok(c);
    asserteq(c->breaks, 4);
    pl = S->leaves.pages;
    (void)lc_drainpool(&S->leaves);
    S->leaves.pages = NULL;
    lc_seek(&C, c, 0);
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    r = lc_markbreak(&C, 1);
    S->allocf = test_alloc;
    S->alloc_ud = NULL;
    asserteq(r, LC_ERRMEM);
    lc_restorepages(&S->leaves, pl);
    lc_delcache(S, c);
    lc_close(S);
}

/* OOM in lcB_cutleaf: leaf allocation fails (line 1067).
 * Tree with data, leaves pool drained, insert triggers cutleaf. */
TEST(append_oom_cutleaf) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10)));
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r, oom = 0;
    void     *pl;
    pl = S->leaves.pages;
    (void)lc_drainpool(&S->leaves);
    S->leaves.pages = NULL;
    lc_seek(&C, c, 5);
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    r = lc_append(&C, 0, lc_scanner, &pz);
    S->allocf = test_alloc;
    S->alloc_ud = NULL;
    asserteq(r, LC_ERRMEM);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 5));
    lc_restorepages(&S->leaves, pl);
    lc_delcache(S, c);
    lc_close(S);
}

TEST(append_cov_rootdeep) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {8, 1, 0}, *p = brs;
    int       r;

    lc_rscanV(c, 64, 10);
    asserteq(lc_breaks(c), 64);
    assertok(lc_checktree(c));

    lc_seek(&C, c, 330);
    assertok(lc_checkcursor(&C, 330));
    r = lc_append(&C, 0, lc_rscanner, &p);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

/* lc_insert -- wrapper around lc_append that restores cursor */

TEST(insert_params) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;

    S = lc_open(&test_alloc, NULL);
    assertok(S);
    c = lc_newcache(S);

    asserteq(lc_insert(NULL, 0, lc_scanner, &pz), LC_ERRPARAM);
    memset(&C, 0, sizeof(C));
    asserteq(lc_insert(&C, 0, lc_scanner, &pz), LC_ERRPARAM);
    lc_seek(&C, c, 0);
    asserteq(lc_insert(&C, 0, lc_scanner, &pz), LC_OK);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(insert_basic) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    lc_scanV(c, 10, 10);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 7, lc_scanner, &pz);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 27);
    asserteq(lc_linelen(&C), 17);
    assertok(lc_checkcursor(&C, 5));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(insert_leaf) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {3, 3, 0}, *pb = brs;
    int       r;

    lc_scanV(c, 10, 15, 15);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 40);

    lc_seek(&C, c, 10);
    r = lc_insert(&C, 3, lc_scanner, &pb);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 5);
    asserteq(lc_bytes(c), 49);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 10));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(insert_col) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {4, 4, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 4, 7);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 11);

    lc_seek(&C, c, 6);
    asserteq(C.lnu, 1);
    asserteq(C.col, 2);
    r = lc_insert(&C, 3, lc_scanner, &pb);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 4);
    asserteq(lc_bytes(c), 22);
    assertok(lc_checkcursor(&C, 6));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(insert_empty) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs[] = {10, 10, 10, 0}, zero[] = {0}, *pbrs;
    int       r;

    c = lc_newcache(S);

    pbrs = brs;
    lc_seek(&C, c, 0);
    r = lc_insert(&C, 5, lc_scanner, &pbrs);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 30);
    assertok(lc_checkcursor(&C, 0));
    lc_delcache(S, c);

    c = lc_newcache(S);
    pbrs = zero;
    lc_seek(&C, c, 0);
    r = lc_insert(&C, 0, lc_scanner, &pbrs);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 0);
    asserteq(lc_bytes(c), 0);
    assertok(lc_checkcursor(&C, 0));
    lc_delcache(S, c);

    lc_close(S);
}

TEST(insert_trailing) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {5, 5, 0}, *pb = brs;
    int       r;

    lc_scanV(c, 10, 10);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);

    lc_seek(&C, c, 25);
    asserteq(lc_offset(&C), 25);
    asserteq(C.col, 5);
    r = lc_insert(&C, 7, lc_scanner, &pb);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 4);
    asserteq(lc_bytes(c), 35);
    assertok(lc_checkcursor(&C, 25));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(insert_noop) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    lc_scanV(c, 10, 10);
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 0, lc_scanner, &pz);
    asserteq(r, LC_OK);
    assertok(lc_checktree(c));
    asserteq(lc_breaks(c), 2);
    asserteq(lc_bytes(c), 20);
    assertok(lc_checkcursor(&C, 5));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(insert_many) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {17, 1, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 10, 10, 10);
    asserteq(lc_breaks(c), 3);
    asserteq(lc_bytes(c), 30);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 0, lc_rscanner, &pb);
    asserteq(r, LC_OK);
    asserteq(lc_breaks(c), 20);
    asserteq(lc_bytes(c), 47);
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(&C, 5));
    lc_delcache(S, c);
    lc_close(S);
}

TEST(insert_noscanner) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    size_t    bb, br;
    int       v;

    lc_seek(&C, c, 0);
    asserteq(lc_insert(&C, 3, NULL, NULL), LC_OK);
    asserteq(lc_offset(&C), 0);
    asserteq(lc_bytes(c), 0);
    asserteq(lc_breaks(c), 0);

    lc_scanV(c, 5, 10);
    lc_seek(&C, c, 3);
    v = lc_insert(&C, 7, NULL, NULL);
    asserteq(v, LC_OK);
    asserteq(lc_offset(&C), 3);
    asserteq(lc_linelen(&C), 12);
    asserteq(lc_bytes(c), 22);

    bb = lc_bytes(c), br = lc_breaks(c);
    lc_seek(&C, c, bb + 5);
    asserteq(lc_offset(&C), bb + 5);
    asserteq(lc_insert(&C, 8, NULL, NULL), LC_OK);
    asserteq(lc_offset(&C), bb + 5);
    asserteq(lc_bytes(c), bb);
    asserteq(lc_breaks(c), br);

    bb = lc_bytes(c);
    lc_seek(&C, c, 0);
    asserteq(lc_insert(&C, 0, NULL, NULL), LC_OK);
    asserteq(lc_offset(&C), 0);
    asserteq(lc_bytes(c), bb);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(locate_params) { asserteq(lc_locate(NULL, 0), LC_ERRPARAM); }

TEST(locate_basic) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assertok(lc_checktree(c));

    lc_seek(&C, c, 0);

    r = lc_locate(&C, 0);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 0);
    asserteq(lc_line(&C), 0);
    assertok(lc_checkcursor(&C, 0));

    r = lc_locate(&C, 10);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 10);
    asserteq(lc_line(&C), 1);
    assertok(lc_checkcursor(&C, 10));

    r = lc_locate(&C, 25);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 25);
    asserteq(lc_line(&C), 2);
    assertok(lc_checkcursor(&C, 25));

    r = lc_locate(&C, 40);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 40);
    asserteq(lc_line(&C), 3);
    assertok(lc_checkcursor(&C, 40));

    r = lc_locate(&C, 50);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 50);
    assertok(lc_checkcursor(&C, 50));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(locate_empty) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_seek(&C, lc_nonnull(c), 0);
    r = lc_locate(&C, 0);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 0);
    assertok(lc_checkcursor(&C, 0));

    r = lc_locate(&C, 5);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 5);
    assertok(lc_checkcursor(&C, 5));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(locline_params) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    asserteq(lc_locline(NULL, 0), LC_ERRPARAM);
    lc_seek(&C, c, 0);
    asserteq(lc_locline(&C, 1), LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

TEST(locline_basic) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assertok(lc_checktree(c));

    lc_seek(&C, c, 0);

    r = lc_locline(&C, 0);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 0);
    asserteq(lc_line(&C), 0);
    assertok(lc_checkcursor(&C, 0));

    r = lc_locline(&C, 1);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 10);
    asserteq(lc_line(&C), 1);
    assertok(lc_checkcursor(&C, 10));

    r = lc_locline(&C, 3);
    asserteq(r, LC_OK);
    asserteq(lc_offset(&C), 40);
    asserteq(lc_line(&C), 3);
    assertok(lc_checkcursor(&C, 40));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(locline_empty) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_seek(&C, lc_nonnull(c), 0);
    r = lc_locline(&C, 0);
    asserteq(r, LC_OK);
    asserteq(lc_line(&C), 0);
    assertok(lc_checkcursor(&C, 0));

    lc_delcache(S, c);
    lc_close(S);
}

TEST(locline_crossleaf) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_rscanV(c, 6, 10);
    asserteq(lc_breaks(c), 6);

    lc_seek(&C, c, 0);
    r = lc_locline(&C, 4);
    asserteq(r, LC_OK);
    asserteq(lc_line(&C), 4);
    assertok(lc_checkcursor(&C, lc_offset(&C)));

    lc_delcache(S, c);
    lc_close(S);
}

#include "linecache_test_fanout4.gen.inc"
