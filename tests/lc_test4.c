#define LC_LEAF_FANOUT 4
#define LC_FANOUT      4
#define LC_PAGE_SIZE   512
#undef LC_IMPLEMENTATION
#define LC_IMPLEMENTATION
#ifndef LC_POOL_STATS
# define LC_POOL_STATS
#endif

#include "lc_tests.h"

/* T1: lifecycle */

static void test_lifecycle(void) {
    lc_State *s = lc_open(&test_alloc, NULL);
    lc_Cache *t1, *t2;
    assert(s);
    t1 = lc_newcache(s);
    assert(t1 && lc_breaks(t1) == 0 && lc_bytes(t1) == 0);
    t2 = lc_newcache(s);
    assert(t2 && t1 != t2);
    lc_delcache(s, t1);
    lc_reset(s);
    t1 = lc_newcache(s);
    assert(t1 && lc_breaks(t1) == 0);
    lc_delcache(s, t1);
    lc_delcache(s, t2);
    lc_close(s);

    lc_reset(NULL);
    lc_close(NULL);

    /* lc_open OOM */
    {
        int z = 0;
        assert(lc_open(&oom_alloc, &z) == NULL);
    }
    /* lc_newcache OOM */
    {
        int       one = 1;
        lc_State *s2 = lc_open(&oom_alloc, &one);
        assert(s2 != NULL);
        assert(lc_newcache(s2) == NULL);
        lc_close(s2);
    }
}

static void test_scan_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    unsigned  brs[] = {0}, *pbrs = brs;

    assert(lc_scan(NULL, lc_scanner, &pbrs) == LC_ERRPARAM);
    assert(lc_scan(c, NULL, &pbrs) == LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_scan_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    unsigned  empty[] = {0}, full[] = {5, 10, 15, 20, 0}, *pbrs;

    /* case 1: empty scanner on empty tree */
    c = lc_newcache(S);
    pbrs = empty;
    assert(lc_scan(c, lc_scanner, &pbrs) == LC_OK);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    assert(lc_checktree(c));
    lc_delcache(S, c);

    /* case 2: exactly one full leaf (4 breaks) */
    c = lc_newcache(S);
    pbrs = full;
    assert(lc_scan(c, lc_scanner, &pbrs) == LC_OK);
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 50);
    assert(c->levels == 0 && c->root.child_count == 1);
    assert(lc_checktree(c));
    lc_delcache(S, c);

    lc_close(S);
}

static void test_scan_seek(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);
    assert(lc_checktree(c));

    r = lc_seek(&C, c, 0);
    assert(r == LC_OK && lc_offset(&C) == 0 && lc_line(&C) == 0);
    assert(lc_linelen(&C) == 10);
    assert(lc_checkcursor(&C, 0));

    r = lc_seek(&C, c, 15);
    assert(r == LC_OK && lc_offset(&C) == 15 && lc_line(&C) == 1);
    assert(lc_checkcursor(&C, 15));

    r = lc_seek(&C, c, 10);
    assert(r == LC_OK && lc_offset(&C) == 10 && lc_line(&C) == 1);
    assert(lc_checkcursor(&C, 10));

    r = lc_seek(&C, c, 25);
    assert(r == LC_OK && lc_offset(&C) == 25 && lc_line(&C) == 2);
    assert(lc_checkcursor(&C, 25));

    r = lc_seek(&C, c, 40);
    assert(r == LC_OK && lc_offset(&C) == 40 && lc_line(&C) == 3);
    assert(lc_checkcursor(&C, 40));

    /* re-scan: scan into already-populated tree */
    lc_scanV(c, 5, 10);
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 55);
    assert(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_scan_bulk(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {120, 1, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, lc_rscanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 120 && lc_bytes(c) == 120);
    assert(c->levels >= 2);
    assert(lc_checktree(c));
    lc_seek(&C, c, 0);
    assert(lc_offset(&C) == 0 && lc_line(&C) == 0);
    assert(lc_checkcursor(&C, 0));

    lc_seek(&C, c, 120);
    assert(lc_offset(&C) == 120 && lc_line(&C) == 120);
    assert(lc_checkcursor(&C, 120));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_scan_append(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    unsigned  brs_a[] = {4, 10, 0}, *pa = brs_a;
    unsigned  brs_b[] = {5, 20, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, lc_rscanner, &pa);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 40);
    r = lc_scan(c, lc_rscanner, &pb);
    assert(r == LC_OK && lc_breaks(c) == 9 && lc_bytes(c) == 140);
    assert(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_scan_oom_items(void) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[] = {10, 0}, *pbrs = brs;
    int       r, oom = 2;

    S = lc_open(&oom_alloc, &oom);
    if (!S) return;
    c = lc_newcache(S);
    r = lc_scan(c, lc_scanner, &pbrs);
    assert(r == LC_ERRMEM);
    lc_delcache(S, c);
    lc_close(S);
}

static void test_scan_oom_flush(void) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[] = {170, 10, 0}, *pbrs = brs;
    int       r, oom = 3;
    S = lc_open(&oom_alloc, &oom);
    if (!S) return;
    c = lc_newcache(S);
    r = lc_scan(c, lc_rscanner, &pbrs);
    assert(r == LC_ERRMEM);
    lc_delcache(S, c);
    lc_close(S);
}

static void test_scan_oom_build(void) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[] = {170, 1, 0}, *pbrs = brs;
    int       r, oom = 4;
    S = lc_open(&oom_alloc, &oom);
    if (!S) return;
    c = lc_newcache(S);
    r = lc_scan(c, lc_rscanner, &pbrs);
    assert(r == LC_ERRMEM);
    lc_delcache(S, c);
    lc_close(S);
}

/* scan beyond full tree: trigger makechain from<0 (root deepen) */
static void test_scan_deepen_root(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);

    lc_rscanV(c, 260, 1);
    assert(lc_breaks(c) == 260 && lc_bytes(c) == 260);
    assert(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_scan_edge_makechain_empty(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    unsigned  lines[] = {16, 10, 0}, *p = lines;

    assert(lc_scan(c, lc_rscanner, &p) == LC_OK);
    assert(lc_breaks(c) == 16);
    assert(lc_bytes(c) == 160);
    assert(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_seek_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C, C2;
    memset(&C, 0, sizeof(C));

    assert(lc_seek(NULL, c, 0) == LC_ERRPARAM);
    assert(lc_seek(&C, NULL, 0) == LC_ERRPARAM);
    assert(lc_seekline(NULL, c, 0) == LC_ERRPARAM);
    assert(lc_seekline(&C, NULL, 0) == LC_ERRPARAM);
    lc_seek(&C2, c, 0);
    assert(lc_checkcursor(&C2, 0));
    assert(lc_seekline(&C2, c, 1) == LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_seek_pastleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_rscanV(c, 8, 10);
    assert(lc_breaks(c) == 8);
    assert(lc_checktree(c));

    /* seek to offset past first leaf (first leaf has 4 breaks, 40 bytes) */
    r = lc_seek(&C, c, 45);
    assert(r == LC_OK && lc_offset(&C) == 45 && lc_line(&C) == 4);
    assert(lc_checkcursor(&C, 45));
    ;

    lc_delcache(S, c);
    lc_close(S);
}

static void test_seek_line_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_checktree(c));

    r = lc_seekline(&C, c, 0);
    assert(r == LC_OK && lc_offset(&C) == 0 && lc_line(&C) == 0);
    assert(lc_linelen(&C) == 10);
    assert(lc_checkcursor(&C, 0));

    r = lc_seekline(&C, c, 1);
    assert(r == LC_OK && lc_offset(&C) == 10 && lc_line(&C) == 1);
    assert(lc_linelen(&C) == 15);
    assert(lc_checkcursor(&C, 10));

    r = lc_seekline(&C, c, 3);
    assert(r == LC_OK && lc_offset(&C) == 40 && lc_line(&C) == 3);
    assert(lc_checkcursor(&C, 40));
    ;

    lc_delcache(S, c);
    lc_close(S);
}

static void test_seek_line_pastleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_rscanV(c, 6, 10);
    assert(lc_breaks(c) == 6);
    assert(lc_checktree(c));

    r = lc_seekline(&C, c, 4);
    assert(r == LC_OK && lc_line(&C) == 4);
    assert(lc_checkcursor(&C, lc_offset(&C)));
    ;

    lc_delcache(S, c);
    lc_close(S);
}

static void test_seek_edge(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    /* seek past end: locends, col = n - bytes */
    lc_scanV(c, 10, 15, 15);
    assert(lc_bytes(c) == 40);
    lc_seek(&C, c, 100);
    assert(C.col == 60);
    assert(lc_checkcursor(&C, 100));
    ;
    lc_delcache(S, c);

    /* seekline on empty tree (no breaks) */
    c = lc_newcache(S);
    assert(lc_seek(&C, c, 0) == LC_OK);
    assert(lc_checkcursor(&C, 0));
    ;
    assert(lc_seekline(&C, c, 0) == LC_OK);
    assert(lc_checkcursor(&C, 0));
    ;
    lc_delcache(S, c);

    lc_close(S);
}

static void test_advance_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);

    assert(lc_advance(NULL, 1) == LC_ERRPARAM);
    {
        lc_Cursor C;
        memset(&C, 0, sizeof(C));
        assert(lc_advance(&C, 1) == LC_ERRPARAM);
    }

    assert(lc_linelen(NULL) == 0);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_advance_single(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_checktree(c));

    r = lc_seek(&C, c, 5);
    assert(r == LC_OK);
    r = lc_advance(&C, 0);
    assert(r == LC_OK && lc_offset(&C) == 5);
    r = lc_advance(&C, 10);
    assert(r == LC_OK && lc_offset(&C) == 15 && lc_line(&C) == 1);
    assert(lc_checkcursor(&C, 15));

    r = lc_advline(&C, 1);
    assert(r == LC_OK && lc_offset(&C) == 25 && lc_line(&C) == 2);
    assert(lc_checkcursor(&C, 25));

    /* backward within leaf */
    r = lc_advance(&C, -8);
    assert(r == LC_OK && lc_offset(&C) == 17 && lc_line(&C) == 1);
    assert(lc_checkcursor(&C, 17));

    /* clamp past end */
    r = lc_advance(&C, 100);
    assert(r == LC_OK && lc_offset(&C) == 117);
    assert(lc_checkcursor(&C, 117));

    /* clamp before start */
    r = lc_advance(&C, -200);
    assert(r == LC_OK && lc_offset(&C) == 0);
    assert(lc_checkcursor(&C, 0));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_advance_cross(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 10, 10);
    assert(lc_breaks(c) == 10);
    assert(lc_checktree(c));

    /* advance forward across leaf boundary */
    lc_seek(&cur, c, 35); /* end of first leaf's last gap */
    assert(lc_checkcursor(&cur, 35));
    r = lc_advance(&cur, 10); /* cross into second leaf */
    assert(r == LC_OK && lc_offset(&cur) == 45 && lc_line(&cur) == 4);
    assert(lc_checkcursor(&cur, 45));

    /* advance backward across leaf boundary */
    r = lc_advance(&cur, -10);
    assert(r == LC_OK && lc_offset(&cur) == 35 && lc_line(&cur) == 3);
    assert(lc_checkcursor(&cur, 35));

    /* advline forward across leaf boundary */
    lc_seek(&cur, c, 35); /* break 3, line 3 */
    assert(lc_checkcursor(&cur, 35));
    r = lc_advline(&cur, 2); /* cross to line 5 (in second leaf) */
    assert(r == LC_OK && lc_line(&cur) == 5 && lc_offset(&cur) == 50);
    assert(lc_checkcursor(&cur, 50));

    /* advline backward across leaf boundary */
    r = lc_advline(&cur, -2);
    assert(r == LC_OK && lc_line(&cur) == 3 && lc_offset(&cur) == 30);
    assert(lc_checkcursor(&cur, 30));

    /* advline to start */
    r = lc_advline(&cur, -100);
    assert(r == LC_OK && lc_line(&cur) == 0 && lc_offset(&cur) == 0);
    assert(lc_checkcursor(&cur, 0));

    /* advline to end (covers lcC_forwardline last-line path) */
    r = lc_advline(&cur, 100);
    assert(r == LC_OK && lc_line(&cur) == 10 && lc_offset(&cur) == 100);
    assert(lc_checkcursor(&cur, 100));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_advance_brute(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, delta, dst;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL), assert(S);
    c = lc_newcache(S);
    lc_rscanV(c, n, 2);
    assert(lc_checktree(c));

    for (pos = 0; pos <= nb + 1; ++pos)
        for (delta = -nb - 1; delta <= nb + 1; ++delta) {
            lc_seek(&C, c, pos);
            lc_advance(&C, delta);
            dst = pos + delta < 0 ? 0 : pos + delta;
            if (!lc_checkcursor(&C, dst)) {
                lc_log("advance pos=%d delta=%d off=%zu exp=%d\n", pos, delta,
                       lc_offset(&C), dst);
                lc_dumpcursor(&C, "after advance");
                abort();
            }
        }
    lc_delcache(S, c);
    lc_close(S);
}

static void test_advance_cov_skip_siblings(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 12, 10);
    assert(lc_breaks(c) == 12);

    /* advance forward from leaf 0, skipping past leaf 1 entirely */
    lc_seek(&cur, c, 35); /* near end of leaf 0 */
    assert(lc_checkcursor(&cur, 35));
    r = lc_advance(&cur, 55); /* skip past leaf 1 (40 bytes) into leaf 2 */
    assert(r == LC_OK && lc_offset(&cur) == 90 && lc_line(&cur) == 9);
    assert(lc_checkcursor(&cur, 90));

    /* advance backward from leaf 2, skipping past leaf 1 entirely */
    r = lc_advance(&cur, -55); /* skip past leaf 1 backward into leaf 0 */
    assert(r == LC_OK && lc_offset(&cur) == 35 && lc_line(&cur) == 3);
    assert(lc_checkcursor(&cur, 35));

    /* advance lines forward, skipping past full leaf */
    lc_seekline(&cur, c, 1); /* line 1, in leaf 0 */
    assert(lc_checkcursor(&cur, 10));
    r = lc_advline(&cur, 8); /* skip past leaf 0 rest + full leaf 1 */
    assert(r == LC_OK && lc_line(&cur) == 9 && lc_offset(&cur) == 90);
    assert(lc_checkcursor(&cur, 90));

    /* advance lines backward, skipping past full leaf */
    r = lc_advline(&cur, -8);
    assert(r == LC_OK && lc_line(&cur) == 1 && lc_offset(&cur) == 10);
    assert(lc_checkcursor(&cur, 10));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_advline_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);

    assert(lc_advline(NULL, 1) == LC_ERRPARAM);
    {
        lc_Cursor C;
        memset(&C, 0, sizeof(C));
        assert(lc_advline(&C, 1) == LC_ERRPARAM);
    }
    {
        lc_Cursor C;
        lc_seek(&C, c, 0);
        assert(lc_advline(&C, 0) == LC_OK);
    }

    lc_delcache(S, c);
    lc_close(S);
}

static void test_advline_cross(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 10);
    assert(lc_breaks(c) == 8);

    /* advance lines across leaf boundary */
    lc_seekline(&cur, c, 2); /* line 2, offset 20 */
    assert(lc_checkcursor(&cur, 20));
    r = lc_advline(&cur, 3); /* skip past leaf boundary (4 breaks in leaf 0) */
    assert(r == LC_OK && lc_line(&cur) == 5 && lc_offset(&cur) == 50);
    assert(lc_checkcursor(&cur, 50));

    /* backward across leaf boundary */
    r = lc_advline(&cur, -4);
    assert(r == LC_OK && lc_line(&cur) == 1 && lc_offset(&cur) == 10);
    assert(lc_checkcursor(&cur, 10));

    /* forward to last line */
    lc_seekline(&cur, c, 0);
    assert(lc_checkcursor(&cur, 0));
    r = lc_advline(&cur, 100);
    assert(r == LC_OK && lc_line(&cur) == 8 && lc_offset(&cur) == 80);
    assert(lc_checkcursor(&cur, 80));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_advline_zero(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 10);
    lc_seek(&cur, c, 23); /* line 2, col=3 */
    assert(lc_checkcursor(&cur, 23) && lc_line(&cur) == 2 && lc_col(&cur) == 3);
    r = lc_advline(&cur, 0);
    assert(r == LC_OK && lc_line(&cur) == 2 && lc_col(&cur) == 0);
    lc_advance(&cur, 5);
    assert(lc_col(&cur) == 5 && lc_line(&cur) == 2);
    r = lc_advline(&cur, 0);
    assert(r == LC_OK && lc_line(&cur) == 2 && lc_col(&cur) == 0);
    lc_delcache(S, c);
    lc_close(S);
}

static void test_advline_brute(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, delta, dst;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL), assert(S);
    c = lc_newcache(S);
    lc_rscanV(c, n, 2);
    assert(lc_checktree(c));

    for (pos = 0; pos <= nb + 1; ++pos)
        for (delta = -nb - 1; delta <= nb + 1; ++delta) {
            lc_seek(&C, c, pos);
            lc_advline(&C, delta);
            dst = (pos + delta * 2) & ~1;
            dst = dst < 0 ? 0 : dst > n * 2 ? n * 2 : dst;
            if (!lc_checkcursor(&C, dst)) {
                lc_log("advance pos=%d delta=%d off=%zu failed exp=%d\n", pos,
                       delta, lc_offset(&C), dst);
                lc_dumpcursor(&C, "after advance");
                abort();
            }
        }
    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    assert(lc_markbreak(NULL, 1) == LC_ERRPARAM);
    memset(&C, 0, sizeof(C));
    assert(lc_markbreak(&C, 1) == LC_ERRPARAM);
    lc_seek(&C, c, 0);
    assert(lc_markbreak(&C, 0) == LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 20);
    assert(lc_breaks(c) == 2);

    lc_seek(&cur, c, 0);
    r = lc_markbreak(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 3);
    assert(lc_offset(&cur) == 5 && lc_line(&cur) == 1);
    assert(lc_checkcursor(&cur, 5));

    /* verify by seeking past new break */
    lc_seek(&cur, c, 6);
    assert(lc_checkcursor(&cur, 6));
    assert(lc_line(&cur) == 1);

    /* extend line past next break (br > gap): set line length to 100 */
    lc_seek(&cur, c, 2);
    assert(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 100);
    assert(r == LC_OK); /* cross-break extension: no error */
    assert(lc_checkcursor(&cur, 102));

    /* null check */
    r = lc_markbreak(NULL, 1);
    assert(r == LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_brute(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, ins, r;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL);
    assert(S);

    for (pos = 0; pos <= nb + 1; ++pos)
        for (ins = 1; ins <= n; ++ins) {
            c = lc_newcache(S);
            lc_rscanV(c, 128, 2); /* 128*2=256 bytes, levels≥2 */
            lc_seek(&C, c, pos);
            r = lc_markbreak(&C, ins);
            assert(r == LC_OK);
            if (!lc_checktree(c) || !lc_checkcursor(&C, pos + ins)) {
                lc_log("insert pos=%d ins=%d failed\n", pos, ins);
                lc_dumptree(c, "insert brute fail");
                lc_dumpcursor(&C, "insert brute fail");
                abort();
            }
            if (lc_col(&C) != 0) {
                lc_log("insert pos=%d ins=%d col=%u\n", pos, ins, lc_col(&C));
                lc_dumptree(c, "insert brute fail");
                lc_dumpcursor(&C, "insert brute fail");
                abort();
            }
            lc_delcache(S, c);
            assert(S->leaves.live_obj == 0 && S->nodes.live_obj == 0);
        }
    lc_close(S);
}

static void test_markbreak_empty(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    r = lc_seek(&cur, c, 0);
    assert(r == LC_OK);
    assert(lc_checkcursor(&cur, 0));
    r = lc_markbreak(&cur, 10);
    assert(r == LC_OK && lc_breaks(c) == 1 && lc_bytes(c) == 10);
    assert(lc_offset(&cur) == 10 && lc_line(&cur) == 1);
    assert(lc_checkcursor(&cur, 10));
    assert(lc_linelen(&cur) == 0); /* at break boundary, gap=0 */

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_crossline(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    int       r;

    /* case 1: large gap split at br=10, line1 [1..99) */
    c = lc_newcache(S);
    lc_scanV(c, 1, 99, 100, 100);
    assert(lc_breaks(c) == 4);
    assert(lc_checktree(c));
    lc_seekline(&C, c, 1);
    assert(lc_offset(&C) == 1 && lc_line(&C) == 1 && lc_linelen(&C) == 99);
    assert(lc_checkcursor(&C, 1));
    r = lc_markbreak(&C, 10);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 300);
    assert(lc_offset(&C) == 11 && lc_line(&C) == 2 && lc_linelen(&C) == 89);
    assert(lc_checkcursor(&C, 11));
    lc_delcache(S, c);

    /* case 2: gap split at br=5, line1 offset 10, len=15 */
    c = lc_newcache(S);
    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);
    lc_seekline(&C, c, 1);
    assert(lc_offset(&C) == 10);
    assert(lc_checkcursor(&C, 10));
    r = lc_markbreak(&C, 5);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_bytes(c) == 40 && lc_breaks(c) == 4);
    assert(lc_checkcursor(&C, 15));
    lc_delcache(S, c);

    lc_close(S);
}

static void test_markbreak_trailing(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);

    /* trailing gap: splice at end adds virtual bytes, lc_bytes unchanged */
    lc_seek(&cur, c, 40);
    assert(lc_checkcursor(&cur, 40));
    lc_splice(&cur, 0, 20);
    assert(lc_offset(&cur) == 60); /* 40 real + 20 virtual in col */
    assert(lc_checkcursor(&cur, 60));
    assert(lc_bytes(c) == 40 && lc_line(&cur) == 3);

    r = lc_markbreak(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 4);
    assert(lc_offset(&cur) == 65 && lc_line(&cur) == 4);
    assert(lc_checkcursor(&cur, 65));
    assert(lc_linelen(&cur) == 0);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;
    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);
    lc_seek(&cur, c, 5);
    assert(lc_checkcursor(&cur, 5));
    assert(lc_checktree(c));
    r = lc_markbreak(&cur, 10);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_bytes(c) == 40);
    assert(lc_checkcursor(&cur, 15));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_brzero(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);
    lc_seek(&cur, c, 5);
    assert(lc_markbreak(&cur, 0) == LC_ERRPARAM);
    assert(lc_breaks(c) == 3);
    assert(lc_checkcursor(&cur, 5));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_crossleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 1);
    assert(lc_breaks(c) == 8 && lc_bytes(c) == 8);
    assert(lc_checktree(c));

    lc_seek(&cur, c, 0);
    assert(lc_checkcursor(&cur, 0));
    r = lc_markbreak(&cur, 100);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_bytes(c) == 100 && lc_breaks(c) == 1);
    assert(lc_checkcursor(&cur, 100));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 5, 10);
    assert(lc_breaks(c) == 5);
    assert(c->root.child_count
           > 1); /* leaf split: root now has 2 leaf children */

    /* add break to first gap in first leaf */
    lc_seek(&cur, c, 2);
    assert(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 6);
    assert(lc_checkcursor(&cur, 5));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_node_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 17, 10);
    assert(lc_breaks(c) == 17);

    /* internal node has 5 children (> LC_FANOUT=4), so levels >= 2 */
    /* markbreak at offset 2: splits leaf, triggers internal node split */
    lc_seek(&cur, c, 2);
    assert(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK);
    assert(lc_checkcursor(&cur, 5));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_root_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 70, 10);
    assert(lc_breaks(c) == 70);

    /* seek to first gap in first leaf, add break to trigger cascade */
    lc_seek(&cur, c, 2);
    assert(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 71);
    assert(lc_checkcursor(&cur, 5));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_root_add(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;
    lc_rscanV(c, 21, 10);
    assert(lc_breaks(c) == 21);
    lc_seek(&cur, c, 2);
    assert(lc_checkcursor(&cur, 2));
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK);
    assert(lc_checkcursor(&cur, 5));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_cascade(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       k, r;

    lc_rscanV(c, 200, 5);
    assert(lc_breaks(c) == 200);

    for (k = 0; k < 24; ++k) {
        lc_seek(&cur, c, 2);
        assert(lc_checkcursor(&cur, 2));
        r = lc_markbreak(&cur, 2);
        assert(r == LC_OK);
        if (k == 0) assert(lc_checkcursor(&cur, 4));
    }

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_cov_split_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 10, 10, 10);
    assert(lc_breaks(c) == 4);

    /* cursor at offset 25 generates slot=2 (>= mid=2), moves to new leaf */
    lc_seek(&cur, c, 25);
    assert(lc_checkcursor(&cur, 25));
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 5);
    assert(lc_checkcursor(&cur, 28));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_cov_child_right(void) {
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
    assert(lc_checkcursor(&C, 168));
    r = lc_markbreak(&C, 1);
    assert(r == LC_OK);
    assert(lc_checkcursor(&C, 169));
    assert(lc_checktree_allow_empty(c, 1));
    lc_delcache(S, c);
    assert(S->nodes.live_obj == 0 && S->leaves.live_obj == 0);
    lc_close(S);
}

static void test_markbreak_fullleaf_pastend(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 10, 10, 10);
    assert(lc_breaks(c) == 4);
    lc_seek(&C, c, 40);
    assert(C.lnu == 4 && C.col == 0);
    r = lc_markbreak(&C, 3);
    assert(r == LC_OK);
    assert(c->root.breaks[0] <= LC_LEAF_FANOUT);
    assert(lc_breaks(c) == 5);
    assert(lc_bytes(c) == 43);
    assert(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_markbreak_fullleaf_brgt(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 10, 10, 10, 10, 10, 10, 10);
    assert(lc_breaks(c) == 8);
    lc_seek(&C, c, 25);
    assert(C.lnu == 2 && C.col == 5);
    r = lc_markbreak(&C, 8);
    assert(r == LC_OK);
    assert(lc_breaks(c) == 8);
    lc_asserttree(c, 0, botV(leafV(10, 10, 13, 7), leafV(10, 10, 10, 10)));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_clearbreaks_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15, 15);
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 55);
    assert(lc_checktree(c));

    /* len == 0: no-op */
    lc_seek(&C, c, 5);
    r = lc_clearbreaks(&C, 0);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_linelen(&C) == 10);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 5));

    /* clear exactly one break at break boundary */
    lc_seek(&C, c, 9);
    r = lc_clearbreaks(&C, 5);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_linelen(&C) == 25);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 14));

    /* past end: del clamped, no breaks crossed */
    lc_seek(&C, c, 50);
    r = lc_clearbreaks(&C, 20);
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_linelen(&C) == 30);
    assert(lc_bytes(c) == 40);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 70));

    /* clear all remaining breaks */
    lc_seek(&C, c, 5);
    r = lc_clearbreaks(&C, 40);
    assert(r == LC_OK && lc_breaks(c) == 0 && lc_linelen(&C) == 45);
    assert(lc_bytes(c) == 0);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 45));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_clearbreaks_cov_slot(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor cur;
    int       r;
    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);
    assert(lc_checktree(c));
    lc_seek(&cur, c, 11);
    assert(lc_checkcursor(&cur, 11));
    r = lc_clearbreaks(&cur, 16);
    assert(r == LC_OK && lc_breaks(c) == 2);
    assert(lc_checkcursor(&cur, 27));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_remove_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C, R, X;
    memset(&C, 0, sizeof(C));
    memset(&X, 0, sizeof(X));

    assert(lc_remove(NULL, &R) == LC_ERRPARAM);
    assert(lc_remove(&C, NULL) == LC_ERRPARAM);
    assert(lc_remove(&C, &X) == LC_ERRPARAM);
    lc_seek(&C, c, 0);
    lc_seek(&R, c, 0);
    assert(lc_remove(&C, &X) == LC_ERRPARAM); /* X.tree==NULL != c */
    assert(lc_remove(&X, &C) == LC_ERRPARAM); /* !X->tree */

    /* reversed → no-op */
    lc_scanV(c, 10, 10);
    lc_seek(&C, c, 5);
    lc_seek(&R, c, 2);
    assert(lc_remove(&C, &R) == LC_OK);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);

    /* L in trailing region (offset >= bytes) */
    lc_seek(&C, c, lc_bytes(c) + 3);
    lc_seek(&R, c, lc_bytes(c) + 8);
    assert(lc_remove(&C, &R) == LC_OK);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_remove_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C, R;

    /* remove all */
    c = lc_newcache(S);
    lc_rscanV(c, 100, 10);
    lc_seek(&C, c, 0);
    lc_seek(&R, c, 1000);
    lc_remove(&C, &R);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 0));
    lc_delcache(S, c);

    /* remove range — keep first 11 + last 9 bytes */
    c = lc_newcache(S);
    lc_rscanV(c, 100, 10);
    lc_seek(&C, c, 11);
    lc_seek(&R, c, 991);
    lc_remove(&C, &R);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 11));
    lc_delcache(S, c);

    /* remove within single leaf */
    c = lc_newcache(S);
    lc_scanV(c, 10, 15, 15, 20);
    lc_seek(&C, c, 11);
    lc_seek(&R, c, 26);
    lc_remove(&C, &R);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 11));
    lc_delcache(S, c);

    /* remove across leaves */
    c = lc_newcache(S);
    lc_scanV(c, 5, 5, 5, 5, 5, 5, 5, 5);
    lc_seek(&C, c, 5);
    lc_seek(&R, c, 21);
    lc_remove(&C, &R);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 5));
    lc_delcache(S, c);

    lc_close(S);
}

static void test_splice_params(void) {
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

static void test_splice_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    lc_rscanV(c, 100, 10);
    assert(lc_breaks(c) == 100 && lc_bytes(c) == 1000);

    lc_seek(&C, c, 0);
    lc_splice(&C, 1000, 0); /* delete all */
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 0));

    /* second scan on cleared tree */
    lc_rscanV(c, 100, 10);
    assert(lc_breaks(c) == 100 && lc_bytes(c) == 1000);
    lc_seek(&C, c, 11);
    lc_splice(&C, 980, 0); /* delete all but first 11 + last 9 */
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 11));

    lc_scanV(c, 5, 15);

    /* simple splice (no break crossing) */
    lc_seek(&C, c, 2);
    lc_splice(&C, 5, 3);
    assert(lc_bytes(c) == 38 && lc_offset(&C) == 5);
    assert(lc_checkcursor(&C, 5));

    /* splice crossing breaks */
    lc_seek(&C, c, 0);
    lc_splice(&C, 15, 8);
    assert(lc_bytes(c) == 31); /* 38 - 15 + 8 */
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 8));

    /* splice with del=0, ins=0 (no-op) */
    lc_splice(&C, 0, 0);
    assert(lc_checktree(c));

    /* null check */
    lc_splice(NULL, 1, 1);
    assert(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_splice_trailing(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    lc_scanV(c, 10, 15, 15);
    lc_scan(c, lc_scanner, NULL);

    /* after last break (trailing area): slot=3 (==breaks) */
    lc_seek(&C, c, 40);
    assert(lc_checkcursor(&C, 40));
    lc_splice(&C, 0, 20); /* insert 20 bytes at end */
    assert(lc_checktree(c));
    assert(lc_bytes(c) == 40);   /* 40 is the last newline */
    assert(lc_offset(&C) == 60); /* offset == line start + col */
    assert(lc_checkcursor(&C, 60));

    /* verify seek within expanded trailing segment */
    lc_seek(&C, c, 45);
    assert(lc_checkcursor(&C, 45));
    assert(lc_line(&C) == 3);

    lc_delcache(S, c);

    /* past-end: delete in trailing area should not move col */
    c = lc_newcache(S);
    lc_scanV(c, 10, 15, 15);
    lc_seek(&C, c, 40);
    lc_splice(&C, 5, 0); /* delete 5 past end */
    assert(lc_offset(&C) == 40);
    assert(lc_checkcursor(&C, 40));
    lc_delcache(S, c);

    /* past-end: del+ins, only ins affects col */
    c = lc_newcache(S);
    lc_scanV(c, 10, 15, 15);
    lc_seek(&C, c, 40);
    lc_splice(&C, 5, 10); /* del=5 ins=10 past end */
    assert(lc_offset(&C) == 50);
    assert(lc_checkcursor(&C, 50));
    lc_delcache(S, c);

    lc_close(S);
}

/* splice_brute: exhaustive pos+del+ins enumeration on multi-level tree.
 * 128 lines of 2 bytes each = 256 total bytes, levels >= 2.
 * pos=0..257 (1 past end → trailing), del=0..257 (past end → clamp),
 * ins=0..1 (byte insert). */
static void test_splice_brute(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, del, ins;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL);
    assert(S);

    for (pos = 0; pos <= nb + 1; ++pos)
        for (del = 0; del <= nb + 1; ++del)
            for (ins = 0; ins <= 1; ++ins) {
                c = lc_newcache(S);
                lc_rscanV(c, n, 2);
                assert(lc_checktree(c));
                lc_seek(&C, c, pos);
                lc_splice(&C, del, ins);
                if (!lc_checktree(c)) {
                    lc_log("splice pos=%d del=%d ins=%d tree\n", pos, del, ins);
                    lc_dumptree(c, "after splice");
                    abort();
                }
                if (!lc_checkcursor(&C, pos + ins)) {
                    lc_log("splice pos=%d del=%d ins=%d off=%zu exp=%d\n", pos,
                           del, ins, lc_offset(&C), pos + ins);
                    lc_dumpcursor(&C, "after splice");
                    abort();
                }
                lc_delcache(S, c);
                assert(S->leaves.live_obj == 0 && S->nodes.live_obj == 0);
            }

    lc_close(S);
}

static void test_splice_cov_rebalance(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2, 2), leafV(2, 2, 2)),
                          botV(leafV(2, 2), leafV(2, 2))),
                   innerV(botV(leafV(2)))));
    lc_Cursor C;
    /* L at offset 0 leaf0[2,2]: splice del=3 → leaf becomes [1]
     * underfull → foldleaf merge → botV0.cc=1 → rebalance(1)
     * → foldnode at inner0: cl=1 cr=2 → merge (returns 1) */
    lc_seek(&C, c, 0);
    lc_splice(&C, 3, 0);
    lc_asserttree(
            c, 2,
            innerV(innerV(botV(leafV(1, 2, 2, 2), leafV(2, 2), leafV(2, 2))),
                   innerV(botV(leafV(2)))));
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&C, 0));
    ;
    lc_delcache(S, c);
    lc_close(S);
}

/* foldleaf balance cl+cg>4: via cross-leaf splice triggering stitch+foldleaf */
static void test_splice_cov_foldleaf_lr(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10), leafV(10, 10, 10, 10)));
    lc_seek(&C, c, 10);   /* left leaf lnu=1, cross into right leaf */
    lc_splice(&C, 11, 0); /* delete 11 bytes → cross leaf, trim left */
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 10));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_splice_cov_shiftnode_bal0(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    lc_Cursor C;
    lc_seek(&C, c, 25);
    lc_splice(&C, 16, 0);
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&C, 25));
    lc_delcache(S, c);
    assert(S->nodes.live_obj == 0 && S->leaves.live_obj == 0);
    lc_close(S);
}

static void test_splice_cov_trimleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 0), leafV(10, 0), leafV(10, 0)));
    lc_seek(&cur, lc_nonnull(c), 5);
    lc_splice(&cur, 25, 0);
    assert(c->root.child_count == 0 && c->bytes == 0 && c->breaks == 0);
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&cur, 5));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_params(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       v;

    S = lc_open(&test_alloc, NULL);
    assert(S);
    c = lc_newcache(S);

    assert(lc_append(NULL, 0, lc_scanner, &pz) == LC_ERRPARAM);
    memset(&C, 0, sizeof(C));
    assert(lc_append(&C, 0, lc_scanner, &pz) == LC_ERRPARAM);
    lc_seek(&C, c, 0);
    lc_scanV(c, 5, 10);
    lc_seek(&C, c, 3);
    v = lc_append(&C, 0, NULL, NULL);
    assert(v == LC_OK && lc_offset(&C) == 3);
    assert(lc_bytes(c) == 15 && lc_breaks(c) == 2);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {3, 3, 0}, *pb = brs;
    int       r;

    lc_scanV(c, 10, 15, 15);

    lc_seek(&C, c, 10);
    r = lc_append(&C, 3, lc_scanner, &pb);
    assert(r == LC_OK && lc_breaks(c) == 5 && lc_bytes(c) == 49);
    lc_asserttree(c, 0, botV(leafV(10, 3, 3), leafV(18, 15)));
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 19));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_col(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {4, 4, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 4, 7);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 11);

    lc_seek(&C, c, 6);
    assert(C.lnu == 1 && C.col == 2);
    r = lc_append(&C, 3, lc_scanner, &pb);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    lc_asserttree(c, 0, botV(leafV(4, 2 + 4, 4, 3 + 5)));
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 22);
    assert(lc_checkcursor(&C, 17));
    ;
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    lc_scanV(c, 10, 10);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 5);
    r = lc_append(&C, 7, lc_scanner, &pz);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 27);
    assert(lc_linelen(&C) == 17);
    assert(lc_checkcursor(&C, 12));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_many(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {17, 1, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 10, 10, 10);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 30);

    lc_seek(&C, c, 5);
    r = lc_append(&C, 0, lc_rscanner, &pb);
    assert(r == LC_OK);
    assert(lc_breaks(c) == 20 && lc_bytes(c) == 47);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 22));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_empty(void) {
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
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 30);
    assert(lc_checkcursor(&C, 35));
    lc_delcache(S, c);

    /* case 2: empty tree, e=0, scanner returns 0 (no-op) */
    c = lc_newcache(S);
    pbrs = zero;
    lc_seek(&C, c, 0);
    r = lc_append(&C, 0, lc_scanner, &pbrs);
    assert(r == LC_OK);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    assert(lc_checkcursor(&C, 0));
    lc_delcache(S, c);

    lc_close(S);
}

static void test_append_sib(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 0, botV(leafV(1, 0), leafV(2, 0), leafV(3, 0), NULL));
    lc_Cursor C;
    unsigned  brs[] = {4, 0}, *p = brs;
    lc_seek(&C, c, 0);
    assert(lc_append(&C, 0, lc_scanner, &p) == LC_OK);
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&C, 4));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_deep(void) {
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
    assert(lc_checktree_allow_empty(c, 1));

    lc_seek(&C, c, 1);
    assert(lc_append(&C, 0, lc_scanner, &p) == LC_OK);
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&C, 2));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_leaf_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {3, 3, 3, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 5, 5, 5);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 15);

    lc_seek(&C, c, 5);
    r = lc_append(&C, 2, lc_scanner, &pb);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 6 && lc_bytes(c) == 26);
    assert(lc_checkcursor(&C, 16));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_stitch_shiftup(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(1), leafV(1), leafV(1), leafV(1)));
    lc_Cursor C;
    unsigned  brs[] = {0}, *p = brs;
    lc_seek(&C, c, 1);
    assert(lc_append(&C, 0, lc_scanner, &p) == LC_OK);
    lc_asserttree(c, 0, botV(leafV(1), leafV(1, 1), leafV(1)));
    assert(lc_checkcursor(&C, 1));
    ;
    assert(lc_checktree_allow_empty(c, 1));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_rootpush(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(1), leafV(1), leafV(1), leafV(1)));
    lc_Cursor C;
    unsigned  brs[] = {2, 2, 2, 2, 2, 0}, *p = brs;
    lc_seek(&C, c, 1);
    assert(lc_append(&C, 0, lc_scanner, &p) == LC_OK);
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&C, 11));
    ;
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_findroom_findlevel(void) {
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
    assert(lc_checktree_allow_empty(c, 1));
    lc_seek(&cur, c, 6);
    r = lc_append(&cur, 0, lc_scanner, &pz);
    assert(r == LC_OK);
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&cur, 6));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    lc_scanV(c, 10, 10);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 5);
    r = lc_append(&C, 0, lc_scanner, &pz);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);
    assert(lc_checkcursor(&C, 5));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_trailing(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {5, 5, 0}, *pb = brs;
    int       r;

    lc_scanV(c, 10, 10);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 25);
    assert(lc_offset(&C) == 25 && C.col == 5);
    r = lc_append(&C, 7, lc_scanner, &pb);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    lc_asserttree(c, 0, botV(leafV(10, 10, 10, 5)));
    assert(lc_checkcursor(&C, 35 + 7));
    ;
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 35);
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

static void test_append_brute(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, ins, e, rem, r;
    int const n = 128, nb = n * 2;

    S = lc_open(&test_alloc, NULL);
    assert(S);

    for (pos = 0; pos <= nb + 1; ++pos)
        for (ins = 0; ins <= n; ++ins)
            for (e = 0; e <= 1; ++e) {
                c = lc_newcache(S);
                lc_rscanV(c, 128, 2); /* 128*2=256 bytes, levels≥2 */
                lc_seek(&C, c, pos);
                rem = ins;
                r = lc_append(&C, e, brute_scanner, &rem);
                assert(r == LC_OK);
                if (!lc_checktree(c)
                    || !lc_checkcursor(&C, pos + ins * 3 + e)) {
                    lc_log("insert pos=%d ins=%d e=%d failed\n", pos, ins, e);
                    lc_dumptree(c, "insert brute fail");
                    lc_dumpcursor(&C, "insert brute fail");
                    abort();
                }
                lc_delcache(S, c);
                assert(S->leaves.live_obj == 0 && S->nodes.live_obj == 0);
            }
    lc_close(S);
}

static void test_append_noscanner(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    size_t    bb, br;

    /* empty tree: e bytes in trailing */
    lc_seek(&C, c, 0);
    assert(lc_append(&C, 3, NULL, NULL) == LC_OK);
    assert(lc_offset(&C) == 3 && lc_bytes(c) == 0 && lc_breaks(c) == 0);

    /* non-empty, valid line: e bytes added to current line */
    lc_scanV(c, 5, 10);
    lc_seek(&C, c, 3);
    assert(lc_append(&C, 7, NULL, NULL) == LC_OK);
    assert(lc_offset(&C) == 10 && lc_col(&C) == 10);
    assert(lc_linelen(&C) == 12 && lc_bytes(c) == 22);

    /* trailing region: C->col += e, tree unchanged */
    bb = lc_bytes(c), br = lc_breaks(c);
    lc_seek(&C, c, bb + 5);
    assert(lc_offset(&C) == bb + 5);
    assert(lc_append(&C, 8, NULL, NULL) == LC_OK);
    assert(lc_offset(&C) == bb + 5 + 8 && lc_bytes(c) == bb
           && lc_breaks(c) == br);

    /* e=0 at valid line: no-op */
    bb = lc_bytes(c);
    lc_seek(&C, c, 0);
    assert(lc_append(&C, 0, NULL, NULL) == LC_OK);
    assert(lc_offset(&C) == 0 && lc_bytes(c) == bb);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_oom_brute(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    int       pos, ins, e, oom, rem, r;
    int       cnt = 0;
    int const n = 32;

    assert(S);
    for (pos = 0; pos <= n * 2 + 1; ++pos)
        for (ins = 0; ins <= n; ++ins)
            for (e = 0; e <= 1; ++e)
                for (oom = 0; oom <= 10; ++oom) {
                    int o = oom;
                    c = lc_newcache(S);
                    lc_rscanV(c, 64, 2);
                    assert(lc_checktree(c));
                    lc_seek(&C, c, pos);
                    S->nodes.freed = S->leaves.freed = NULL;
                    rem = ins;
                    S->allocf = oom_alloc;
                    S->alloc_ud = &o;
                    r = lc_append(&C, e, brute_scanner, &rem);
                    S->allocf = test_alloc;
                    S->alloc_ud = NULL;
                    if (r == LC_ERRMEM) {
                        if (!lc_checktree(c)
                            || !lc_checkcursor(&C, (size_t)pos)) {
                            lc_log("OOM brute fail pos=%d ins=%d e=%d"
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
                    assert(r == LC_OK);
                    lc_delcache(S, c);
                }
    lc_log("  test_append_oom_brute: %d OOM cases\n", cnt);
    lc_close(S);
}

static void test_append_oom_trailing(void) {
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
                assert(lc_checktree(c));
                assert(lc_checkcursor(&C, 0));
                assert(S->leaves.live_obj == slb);
                assert(S->nodes.live_obj == snb);
                found = 1;
            }
        }
        lc_delcache(S, c);
        lc_close(S);
        if (found) break;
    }
    assert(found);
}

static void test_append_oom_normal(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    lc_Leaf   lfdum;
    int       oom = 0;
    void     *lf;

    assert(S);
    c = cacheV(S, 0, botV(leafV(5, 5)));
    assert(lc_checktree(c));

    S->leaves.freed = NULL;
    S->leaves.pages = NULL;
    S->nodes.freed = NULL;
    S->nodes.pages = NULL;
    lc_localfill(&S->leaves, &lf, &lfdum, 1);

    {
        size_t   slb = S->leaves.live_obj, snb = S->nodes.live_obj;
        unsigned brs[] = {17, 1, 0}, *pb = brs;
        S->allocf = oom_alloc, S->alloc_ud = &oom;
        lc_seek(&C, c, 3);
        assert(lc_append(&C, 0, lc_rscanner, &pb) == LC_ERRMEM);
        assert(lc_checktree(c));
        assert(lc_checkcursor(&C, 3));
        assert(S->leaves.live_obj == slb);
        assert(S->nodes.live_obj == snb);
    }

    S->leaves.freed = lf;
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_oom_col0(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs_b[] = {1, 1, 1, 1, 1, 0}, *pb;
    int       count, oom = 0;
    void     *head;

    assert(S);
    c = cacheV(S, 0, botV(leafV(5, 5)));
    assert(lc_checktree(c));

    head = S->leaves.freed;
    count = 0;
    while (head) {
        count++;
        head = *(void **)head;
    }
    if (count > 1) {
        int i;
        head = S->leaves.freed;
        for (i = 0; i < count - 1 && *(void **)head; i++) head = *(void **)head;
        S->leaves.freed = head;
        *(void **)head = NULL;
    }

    lc_seek(&C, c, 0);
    {
        size_t slb = S->leaves.live_obj, snb = S->nodes.live_obj;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        pb = brs_b;
        assert(lc_append(&C, 0, lc_scanner, &pb) == LC_ERRMEM);
        S->allocf = test_alloc;
        S->alloc_ud = NULL;
        assert(lc_checktree(c));
        assert(lc_checkcursor(&C, 0));
        lc_asserttree(c, 0, botV(leafV(5, 5)));
        assert(S->leaves.live_obj == slb);
        assert(S->nodes.live_obj == snb);
    }

    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_oom_shiftup(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    lc_Leaf   lfdum;
    int       oom = 0;
    void     *lf;

    assert(S);
    c = cacheV(
            S, 0,
            botV(leafV(1, 0), leafV(2, 0), leafV(3, 0), leafV(4, 0), NULL));
    assert(lc_checktree_allow_empty(c, 1));

    S->leaves.freed = NULL;
    S->leaves.pages = NULL;
    S->nodes.freed = NULL;
    S->nodes.pages = NULL;
    lc_localfill(&S->leaves, &lf, &lfdum, 1);

    {
        unsigned brs[] = {1, 1, 1, 1, 1, 0}, *p = brs;
        size_t   slb = S->leaves.live_obj, snb = S->nodes.live_obj;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        lc_seek(&C, c, 1);
        assert(lc_append(&C, 0, lc_scanner, &p) == LC_ERRMEM);
        assert(lc_checktree_allow_empty(c, 1));
        assert(lc_checkcursor(&C, 1));
        assert(S->leaves.live_obj == slb);
        assert(S->nodes.live_obj == snb);
    }

    S->leaves.freed = lf;
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_oom_rootpush(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    lc_Node  *b[4];
    int       oom = 0, i;
    void     *nf;

    assert(S);
    for (i = 0; i < 4; i++) b[i] = botV(leafV(1), leafV(1), leafV(1), leafV(1));
    c = cacheV(S, 1, innerV(b[0], b[1], b[2], b[3]));
    assert(lc_checktree_allow_empty(c, 1));

    nf = S->nodes.freed;
    S->nodes.freed = NULL;
    S->nodes.pages = NULL;

    {
        unsigned bs[49], *p = (unsigned *)bs;
        size_t   snb = S->nodes.live_obj;
        for (i = 0; i < 48; i++) bs[i] = 1;
        bs[48] = 0;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        lc_seek(&C, c, 1);
        assert(lc_append(&C, 0, lc_scanner, &p) == LC_ERRMEM);
        assert(lc_checktree_allow_empty(c, 1));
        assert(lc_checkcursor(&C, 1));
        assert(S->nodes.live_obj == snb);
    }

    S->nodes.freed = nf;
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_oom_deroot(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    int       oom = 0;

    assert(S);
    c = cacheV(
            S, 0,
            botV(leafV(1, 0), leafV(1, 0), leafV(1, 0), leafV(1, 0), NULL));
    assert(lc_checktree_allow_empty(c, 1));
    {
        void *head = S->nodes.freed;
        int   count = 0;
        while (head) {
            count++;
            head = *(void **)head;
        }
        if (count > 1) {
            int i;
            head = S->nodes.freed;
            for (i = 0; i < count - 1 && *(void **)head; i++)
                head = *(void **)head;
            S->nodes.freed = head;
            *(void **)head = NULL;
        }
    }
    S->nodes.pages = NULL;

    {
        unsigned brs[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0}, *p = brs;
        size_t   snb = S->nodes.live_obj;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        lc_seek(&C, c, 1);
        assert(lc_append(&C, 0, lc_scanner, &p) == LC_ERRMEM);
        assert(lc_checktree_allow_empty(c, 1));
        assert(lc_checkcursor(&C, 1));
        assert(S->nodes.live_obj == snb);
    }

    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_oom_rollback(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;

    assert(S);
    c = cacheV(S, 0, botV(leafV(1, 0), leafV(1, 0), leafV(1, 0), leafV(1, 0)));

    S->nodes.freed = NULL;
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
        assert(r < 0);
        assert(lc_checktree_allow_empty(c, 1));
        assert(lc_checkcursor(&C, 1));
        lc_asserttree(
                c, 0, botV(leafV(1, 0), leafV(1, 0), leafV(1, 0), leafV(1, 0)));
    }

    lc_delcache(S, c);
    lc_close(S);
}

/* stitch reserve: full 256-seg tree, seek 254, insert 48*1b.
 * freelists cleared → every page alloc goes through oom_alloc.
 * oom=4 fails at stitch reserve; oom=5 succeeds.
 * stitch reserve(l+2 nodes) consumes exactly 1 page = 1 allocf. */
static void test_append_oom_full(void) {
    unsigned  ins[] = {45, 1, 0};
    unsigned *p;
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       oom, r;

    /* oom=4: cutleaf(1 leaf pg) + append + findroom(2 node pgs) = 4.
     * stitch reserve(lv+2 nodes) needs 5th page → OOM → rollback. */
    S = lc_open(&test_alloc, NULL);
    c = lc_newcache(S);
    lc_rscanV(c, 256, 1);
    assert(lc_checktree(c));
    S->nodes.freed = S->leaves.freed = NULL;
    oom = 4;
    p = ins;
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    lc_seek(&C, c, 254);
    r = lc_append(&C, 0, lc_rscanner, &p);
    S->allocf = test_alloc, S->alloc_ud = NULL;
    assert(r == LC_ERRMEM);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 254));
    lc_delcache(S, c);
    lc_close(S);

    /* oom=5: stitch gets its page → insert succeeds.
     * Total: cutleaf 1 + findroom 3 + stitch 1 = 5 allocf calls.
     * stitch reserve takes ceil((lv+2)/objs_per_page) page allocs. */
    S = lc_open(&test_alloc, NULL);
    c = lc_newcache(S);
    lc_rscanV(c, 256, 1);
    assert(lc_checktree(c));
    S->nodes.freed = S->leaves.freed = NULL;
    oom = 5;
    p = ins;
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    lc_seek(&C, c, 254);
    r = lc_append(&C, 0, lc_scanner, &p);
    S->allocf = test_alloc;
    S->alloc_ud = NULL;
    assert(r == LC_OK && lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

/* splice delete all with insertion triggers lcD_reset */
static void test_splice_reset(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);
    lc_seek(&C, c, 0);
    assert(lc_checkcursor(&C, 0));
    lc_splice(&C, 40, 0);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 0));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_splice_cov_foldleaf_rl(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10), leafV(10, 10, 10, 10)));
    lc_Cursor C;
    lc_seek(&C, (assert(c), c), 20); /* start of right leaf, lnu=0 */
    lc_splice(&C, 10, 0);            /* cl+cr=5 > 4, balance dl<0 */
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 20));
    lc_delcache(S, c);
    assert(S->nodes.live_obj == 0 && S->leaves.live_obj == 0);
    lc_close(S);
}

/* rebalance early exit: foldnode returns 0 (balance, not merge).
 * botV[0] underfull after foldleaf (1 leaf), botV[1] has 4 leaves,
 * 1+4=5 > 4 → balance → foldnode returns 0 → rebalance returns. */
static void test_splice_rebalance_earlyexit(void) {
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
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&C, 0));
    lc_delcache(S, c);
    assert(S->nodes.live_obj == 0 && S->leaves.live_obj == 0);
    lc_close(S);
}

/* markbreak at right-half of fully packed tree → root split with i>=mid */
static void test_markbreak_cov_rootright(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_rscanV(c, 64, 10);
    assert(lc_breaks(c) == 64);
    assert(lc_checktree(c));

    lc_seek(&C, c, 330);
    assert(lc_checkcursor(&C, 330));
    r = lc_markbreak(&C, 3);
    assert(r == LC_OK);
    assert(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

/* insert with root-deepening to exercise fixsource dl>0 */
/* OOM in lcB_oneline: leaf allocation fails (line 898).
 * Use oom_alloc with counter=2: lc_open+l_newtree succeed, lcL_new fails. */
static void test_markbreak_oom_oneline(void) {
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
    assert(r == LC_OK); /* oneline return discarded by comma operator */
    assert(S->leaves.live_obj == 0 && S->nodes.live_obj == 0);
    lc_delcache(S, c);
    lc_close(S);
}

/* OOM in lcB_makeroom: leaf allocation fails (line 970).
 * Tree with one full leaf, leaves pool drained, markbreak triggers makeroom. */
static void test_markbreak_oom_makeroom(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(3, 3, 3, 3)));
    lc_Cursor C;
    int       r, oom = 0;
    assert(c && c->breaks == 4);
    S->leaves.freed = NULL;
    S->leaves.pages = NULL;
    lc_seek(&C, c, 0);
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    r = lc_markbreak(&C, 1);
    S->allocf = test_alloc;
    S->alloc_ud = NULL;
    assert(r == LC_ERRMEM);
    lc_delcache(S, c);
    lc_close(S);
}

/* OOM in lcB_cutleaf: leaf allocation fails (line 1067).
 * Tree with data, leaves pool drained, insert triggers cutleaf. */
static void test_append_oom_cutleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10)));
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r, oom = 0;
    S->leaves.freed = NULL;
    S->leaves.pages = NULL;
    lc_seek(&C, c, 5);
    S->allocf = oom_alloc;
    S->alloc_ud = &oom;
    r = lc_append(&C, 0, lc_scanner, &pz);
    S->allocf = test_alloc;
    S->alloc_ud = NULL;
    assert(r == LC_ERRMEM);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 5));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_append_cov_rootdeep(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {8, 1, 0}, *p = brs;
    int       r;

    lc_rscanV(c, 64, 10);
    assert(lc_breaks(c) == 64 && lc_checktree(c));

    lc_seek(&C, c, 330);
    assert(lc_checkcursor(&C, 330));
    r = lc_append(&C, 0, lc_rscanner, &p);
    assert(r == LC_OK);
    assert(lc_checktree(c));

    lc_delcache(S, c);
    lc_close(S);
}

/* lc_insert — wrapper around lc_append that restores cursor */

static void test_insert_params(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;

    S = lc_open(&test_alloc, NULL);
    assert(S);
    c = lc_newcache(S);

    assert(lc_insert(NULL, 0, lc_scanner, &pz) == LC_ERRPARAM);
    memset(&C, 0, sizeof(C));
    assert(lc_insert(&C, 0, lc_scanner, &pz) == LC_ERRPARAM);
    lc_seek(&C, c, 0);
    assert(lc_insert(&C, 0, lc_scanner, &pz) == LC_OK);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_insert_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    lc_scanV(c, 10, 10);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 7, lc_scanner, &pz);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 27);
    assert(lc_linelen(&C) == 17);
    assert(lc_checkcursor(&C, 5));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_insert_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {3, 3, 0}, *pb = brs;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);

    lc_seek(&C, c, 10);
    r = lc_insert(&C, 3, lc_scanner, &pb);
    assert(r == LC_OK);
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 49);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 10));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_insert_col(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {4, 4, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 4, 7);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 11);

    lc_seek(&C, c, 6);
    assert(C.lnu == 1 && C.col == 2);
    r = lc_insert(&C, 3, lc_scanner, &pb);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 22);
    assert(lc_checkcursor(&C, 6));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_insert_empty(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs[] = {10, 10, 10, 0}, zero[] = {0}, *pbrs;
    int       r;

    c = lc_newcache(S);

    pbrs = brs;
    lc_seek(&C, c, 0);
    r = lc_insert(&C, 5, lc_scanner, &pbrs);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 30);
    assert(lc_checkcursor(&C, 0));
    lc_delcache(S, c);

    c = lc_newcache(S);
    pbrs = zero;
    lc_seek(&C, c, 0);
    r = lc_insert(&C, 0, lc_scanner, &pbrs);
    assert(r == LC_OK);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    assert(lc_checkcursor(&C, 0));
    lc_delcache(S, c);

    lc_close(S);
}

static void test_insert_trailing(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs[] = {5, 5, 0}, *pb = brs;
    int       r;

    lc_scanV(c, 10, 10);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 25);
    assert(lc_offset(&C) == 25 && C.col == 5);
    r = lc_insert(&C, 7, lc_scanner, &pb);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 35);
    assert(lc_checkcursor(&C, 25));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_insert_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    lc_scanV(c, 10, 10);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 0, lc_scanner, &pz);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);
    assert(lc_checkcursor(&C, 5));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_insert_many(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  brs_b[] = {17, 1, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 10, 10, 10);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 30);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 0, lc_rscanner, &pb);
    assert(r == LC_OK);
    assert(lc_breaks(c) == 20 && lc_bytes(c) == 47);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 5));
    lc_delcache(S, c);
    lc_close(S);
}

static void test_insert_noscanner(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    size_t    bb, br;
    int       v;

    lc_seek(&C, c, 0);
    assert(lc_insert(&C, 3, NULL, NULL) == LC_OK);
    assert(lc_offset(&C) == 0 && lc_bytes(c) == 0 && lc_breaks(c) == 0);

    lc_scanV(c, 5, 10);
    lc_seek(&C, c, 3);
    v = lc_insert(&C, 7, NULL, NULL);
    assert(v == LC_OK && lc_offset(&C) == 3);
    assert(lc_linelen(&C) == 12 && lc_bytes(c) == 22);

    bb = lc_bytes(c), br = lc_breaks(c);
    lc_seek(&C, c, bb + 5);
    assert(lc_offset(&C) == bb + 5);
    assert(lc_insert(&C, 8, NULL, NULL) == LC_OK);
    assert(lc_offset(&C) == bb + 5 && lc_bytes(c) == bb && lc_breaks(c) == br);

    bb = lc_bytes(c);
    lc_seek(&C, c, 0);
    assert(lc_insert(&C, 0, NULL, NULL) == LC_OK);
    assert(lc_offset(&C) == 0 && lc_bytes(c) == bb);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_locate_params(void) {
    assert(lc_locate(NULL, 0) == LC_ERRPARAM);
}

static void test_locate_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_checktree(c));

    lc_seek(&C, c, 0);

    r = lc_locate(&C, 0);
    assert(r == LC_OK && lc_offset(&C) == 0 && lc_line(&C) == 0);
    assert(lc_checkcursor(&C, 0));

    r = lc_locate(&C, 10);
    assert(r == LC_OK && lc_offset(&C) == 10 && lc_line(&C) == 1);
    assert(lc_checkcursor(&C, 10));

    r = lc_locate(&C, 25);
    assert(r == LC_OK && lc_offset(&C) == 25 && lc_line(&C) == 2);
    assert(lc_checkcursor(&C, 25));

    r = lc_locate(&C, 40);
    assert(r == LC_OK && lc_offset(&C) == 40 && lc_line(&C) == 3);
    assert(lc_checkcursor(&C, 40));

    r = lc_locate(&C, 50);
    assert(r == LC_OK && lc_offset(&C) == 50);
    assert(lc_checkcursor(&C, 50));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_locate_empty(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_seek(&C, lc_nonnull(c), 0);
    r = lc_locate(&C, 0);
    assert(r == LC_OK && lc_offset(&C) == 0);
    assert(lc_checkcursor(&C, 0));

    r = lc_locate(&C, 5);
    assert(r == LC_OK && lc_offset(&C) == 5);
    assert(lc_checkcursor(&C, 5));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_locline_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    assert(lc_locline(NULL, 0) == LC_ERRPARAM);
    lc_seek(&C, c, 0);
    assert(lc_locline(&C, 1) == LC_ERRPARAM);

    lc_delcache(S, c);
    lc_close(S);
}

static void test_locline_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_checktree(c));

    lc_seek(&C, c, 0);

    r = lc_locline(&C, 0);
    assert(r == LC_OK && lc_offset(&C) == 0 && lc_line(&C) == 0);
    assert(lc_checkcursor(&C, 0));

    r = lc_locline(&C, 1);
    assert(r == LC_OK && lc_offset(&C) == 10 && lc_line(&C) == 1);
    assert(lc_checkcursor(&C, 10));

    r = lc_locline(&C, 3);
    assert(r == LC_OK && lc_offset(&C) == 40 && lc_line(&C) == 3);
    assert(lc_checkcursor(&C, 40));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_locline_empty(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_seek(&C, lc_nonnull(c), 0);
    r = lc_locline(&C, 0);
    assert(r == LC_OK && lc_line(&C) == 0);
    assert(lc_checkcursor(&C, 0));

    lc_delcache(S, c);
    lc_close(S);
}

static void test_locline_crossleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    int       r;

    lc_rscanV(c, 6, 10);
    assert(lc_breaks(c) == 6);

    lc_seek(&C, c, 0);
    r = lc_locline(&C, 4);
    assert(r == LC_OK && lc_line(&C) == 4);
    assert(lc_checkcursor(&C, lc_offset(&C)));

    lc_delcache(S, c);
    lc_close(S);
}

#define TESTS(X)                  \
    X(lifecycle)                  \
    X(scan_params)                \
    X(scan_basic)                 \
    X(scan_seek)                  \
    X(scan_bulk)                  \
    X(scan_append)                \
    X(scan_oom_items)             \
    X(scan_oom_flush)             \
    X(scan_oom_build)             \
    X(scan_deepen_root)           \
    X(scan_edge_makechain_empty)  \
    X(seek_params)                \
    X(seek_pastleaf)              \
    X(seek_line_leaf)             \
    X(seek_line_pastleaf)         \
    X(seek_edge)                  \
    X(advance_params)             \
    X(advance_single)             \
    X(advance_cross)              \
    X(advance_brute)              \
    X(advance_cov_skip_siblings)  \
    X(advline_params)             \
    X(advline_cross)              \
    X(advline_zero)               \
    X(advline_brute)              \
    X(markbreak_params)           \
    X(markbreak_basic)            \
    X(markbreak_empty)            \
    X(markbreak_crossline)        \
    X(markbreak_trailing)         \
    X(markbreak_noop)             \
    X(markbreak_brzero)           \
    X(markbreak_crossleaf)        \
    X(markbreak_split)            \
    X(markbreak_node_split)       \
    X(markbreak_root_split)       \
    X(markbreak_root_add)         \
    X(markbreak_cascade)          \
    X(markbreak_fullleaf_pastend) \
    X(markbreak_fullleaf_brgt)    \
    X(markbreak_brute)            \
    X(markbreak_cov_split_right)  \
    X(markbreak_cov_child_right)  \
    X(markbreak_cov_rootright)    \
    X(markbreak_oom_oneline)      \
    X(markbreak_oom_makeroom)     \
    X(clearbreaks_basic)          \
    X(clearbreaks_cov_slot)       \
    X(remove_params)              \
    X(remove_basic)               \
    X(splice_params)              \
    X(splice_basic)               \
    X(splice_reset)               \
    X(splice_trailing)            \
    X(splice_brute)               \
    X(splice_cov_rebalance)       \
    X(splice_cov_shiftnode_bal0)  \
    X(splice_cov_trimleaf)        \
    X(splice_cov_foldleaf_lr)     \
    X(splice_cov_foldleaf_rl)     \
    X(splice_rebalance_earlyexit) \
    X(append_params)              \
    X(append_basic)               \
    X(append_leaf)                \
    X(append_col)                 \
    X(append_many)                \
    X(append_empty)               \
    X(append_sib)                 \
    X(append_deep)                \
    X(append_leaf_split)          \
    X(append_stitch_shiftup)      \
    X(append_rootpush)            \
    X(append_findroom_findlevel)  \
    X(append_noop)                \
    X(append_trailing)            \
    X(append_brute)               \
    X(append_oom_brute)           \
    X(append_noscanner)           \
    X(append_oom_trailing)        \
    X(append_oom_normal)          \
    X(append_oom_col0)            \
    X(append_oom_shiftup)         \
    X(append_oom_rootpush)        \
    X(append_oom_deroot)          \
    X(append_oom_rollback)        \
    X(append_oom_cutleaf)         \
    X(append_oom_full)            \
    X(append_cov_rootdeep)        \
    X(insert_params)              \
    X(insert_basic)               \
    X(insert_leaf)                \
    X(insert_col)                 \
    X(insert_empty)               \
    X(insert_trailing)            \
    X(insert_noop)                \
    X(insert_many)                \
    X(insert_noscanner)           \
    X(locate_params)              \
    X(locate_basic)               \
    X(locate_empty)               \
    X(locline_params)             \
    X(locline_basic)              \
    X(locline_empty)              \
    X(locline_crossleaf)

#define X(name) {#name, test_##name},
LC_TEST_MAIN("linecache tests")
#undef X