#define LC_LEAF_FANOUT 4
#define LC_FANOUT      4
#define LC_PAGE_SIZE   512
#define LC_STATIC_API
#ifndef LC_POOL_STATS
# define LC_POOL_STATS
#endif

#include "lc_tests.h"

/* T1: lifecycle */

static void test_lifecycle(void) {
    lc_State *s = lc_open(&test_alloc, NULL);
    lc_Cache *t1, *t2;
    assert(s);
    t1 = lc_newtree(s);
    assert(t1 && lc_breaks(t1) == 0 && lc_bytes(t1) == 0);
    t2 = lc_newtree(s);
    assert(t2 && t1 != t2);
    lc_deltree(s, t1);
    lc_reset(s);
    t1 = lc_newtree(s);
    assert(t1 && lc_breaks(t1) == 0);
    lc_deltree(s, t1);
    lc_deltree(s, t2);
    lc_close(s);
}

static void test_scan_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    unsigned  brs[] = {0}, *pbrs = brs;

    assert(lc_scan(NULL, lc_scanner, &pbrs) == LC_ERRPARAM);
    assert(lc_scan(c, NULL, &pbrs) == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_scan_basic(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    unsigned  empty[] = {0}, full[] = {5, 10, 15, 20, 0}, *pbrs;

    /* case 1: empty scanner on empty tree */
    c = lc_newtree(S);
    pbrs = empty;
    assert(lc_scan(c, lc_scanner, &pbrs) == LC_OK);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    assert(lc_checktree(c));
    lc_deltree(S, c);

    /* case 2: exactly one full leaf (4 breaks) */
    c = lc_newtree(S);
    pbrs = full;
    assert(lc_scan(c, lc_scanner, &pbrs) == LC_OK);
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 50);
    assert(c->levels == 0 && c->root.child_count == 1);
    assert(lc_checktree(c));
    lc_deltree(S, c);

    lc_close(S);
}

static void test_scan_seek(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);
    assert(lc_checktree(c));

    r = lc_seek(&C, c, 0);
    assert(r == LC_OK && lc_offset(&C) == 0 && lc_line(&C) == 0);
    assert(lc_linelen(&C) == 10);

    r = lc_seek(&C, c, 15);
    assert(r == LC_OK && lc_offset(&C) == 15 && lc_line(&C) == 1);

    /* seek to exact break positions */
    r = lc_seek(&C, c, 10);
    assert(r == LC_OK && lc_offset(&C) == 10);
    r = lc_seek(&C, c, 25);
    assert(r == LC_OK && lc_offset(&C) == 25);

    /* seek to end */
    r = lc_seek(&C, c, 40);
    assert(r == LC_OK && lc_offset(&C) == 40);

    /* re-scan: scan into already-populated tree */
    lc_scanV(c, 5, 10);
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 55);
    assert(lc_checktree(c));

    lc_deltree(S, c);
    lc_close(S);
}

static void test_scan_bulk(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs[] = {120, 1, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, lc_rscanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 120 && lc_bytes(c) == 120);
    assert(c->levels >= 2);
    assert(lc_checktree(c));
    lc_seek(&C, c, 0);
    assert(lc_offset(&C) == 0);
    lc_seek(&C, c, 120);
    assert(lc_offset(&C) == 120);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_scan_append(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    unsigned  brs_a[] = {4, 10, 0}, *pa = brs_a;
    unsigned  brs_b[] = {5, 20, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, lc_rscanner, &pa);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 40);
    r = lc_scan(c, lc_rscanner, &pb);
    assert(r == LC_OK && lc_breaks(c) == 9 && lc_bytes(c) == 140);
    assert(lc_checktree(c));
    lc_deltree(S, c);
    lc_close(S);
}

static void test_scan_oom_items(void) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[] = {10, 0}, *pbrs = brs;
    int       r, oom = 2;

    S = lc_open(&oom_alloc, &oom);
    if (!S) return;
    c = lc_newtree(S);
    r = lc_scan(c, lc_scanner, &pbrs);
    assert(r == LC_ERRMEM);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_scan_oom_flush(void) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[] = {170, 10, 0}, *pbrs = brs;
    int       r, oom = 3;
    S = lc_open(&oom_alloc, &oom);
    if (!S) return;
    c = lc_newtree(S);
    r = lc_scan(c, lc_rscanner, &pbrs);
    assert(r == LC_ERRMEM);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_scan_oom_build(void) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[] = {170, 1, 0}, *pbrs = brs;
    int       r, oom = 4;
    S = lc_open(&oom_alloc, &oom);
    if (!S) return;
    c = lc_newtree(S);
    r = lc_scan(c, lc_rscanner, &pbrs);
    assert(r == LC_ERRMEM);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_seek_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C, C2;
    memset(&C, 0, sizeof(C));

    assert(lc_seek(NULL, c, 0) == LC_ERRPARAM);
    assert(lc_seek(&C, NULL, 0) == LC_ERRPARAM);
    assert(lc_seekline(NULL, c, 0) == LC_ERRPARAM);
    assert(lc_seekline(&C, NULL, 0) == LC_ERRPARAM);
    lc_seek(&C2, c, 0);
    assert(lc_seekline(&C2, c, 1) == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_seek_pastleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
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

    lc_deltree(S, c);
    lc_close(S);
}

static void test_seek_line_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_checktree(c));

    r = lc_seekline(&C, c, 0);
    assert(r == LC_OK && lc_offset(&C) == 0 && lc_line(&C) == 0);
    assert(lc_linelen(&C) == 10);

    r = lc_seekline(&C, c, 1);
    assert(r == LC_OK && lc_offset(&C) == 10 && lc_line(&C) == 1);
    assert(lc_linelen(&C) == 15);

    r = lc_seekline(&C, c, 3);
    assert(r == LC_OK && lc_offset(&C) == 40 && lc_line(&C) == 3);
    assert(lc_checkcursor(&C, 40));
    ;

    lc_deltree(S, c);
    lc_close(S);
}

static void test_seek_line_pastleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    int       r;

    lc_rscanV(c, 6, 10);
    assert(lc_breaks(c) == 6);
    assert(lc_checktree(c));

    r = lc_seekline(&C, c, 4);
    assert(r == LC_OK && lc_line(&C) == 4);
    assert(lc_checkcursor(&C, lc_offset(&C)));
    ;

    lc_deltree(S, c);
    lc_close(S);
}

static void test_seek_edge(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;

    /* seek past end: locends, col = n - bytes */
    lc_scanV(c, 10, 15, 15);
    assert(lc_bytes(c) == 40);
    lc_seek(&C, c, 100);
    assert(C.col == 60);
    assert(lc_checkcursor(&C, 100));
    ;
    lc_deltree(S, c);

    /* seekline on empty tree (no breaks) */
    c = lc_newtree(S);
    assert(lc_seek(&C, c, 0) == LC_OK);
    assert(lc_checkcursor(&C, 0));
    ;
    assert(lc_seekline(&C, c, 0) == LC_OK);
    assert(lc_checkcursor(&C, 0));
    ;
    lc_deltree(S, c);

    lc_close(S);
}

static void test_advance_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);

    assert(lc_advance(NULL, 1) == LC_ERRPARAM);
    {
        lc_Cursor C;
        memset(&C, 0, sizeof(C));
        assert(lc_advance(&C, 1) == LC_ERRPARAM);
    }

    lc_deltree(S, c);
    lc_close(S);
}

static void test_advance_single(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_checktree(c));

    r = lc_seek(&C, c, 5);
    assert(r == LC_OK);
    r = lc_advance(&C, 10);
    assert(r == LC_OK && lc_offset(&C) == 15);
    r = lc_advline(&C, 1);
    assert(r == LC_OK && lc_line(&C) == 2);

    /* backward within leaf */
    r = lc_advance(&C, -8);
    assert(r == LC_OK && lc_offset(&C) == 17);

    /* clamp past end */
    r = lc_advance(&C, 100);
    assert(r == LC_OK && lc_offset(&C) == 117);

    /* clamp before start */
    r = lc_advance(&C, -200);
    assert(r == LC_OK && lc_offset(&C) == 0);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_advance_cross(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 10, 10);
    assert(lc_breaks(c) == 10);
    assert(lc_checktree(c));

    /* advance forward across leaf boundary */
    lc_seek(&cur, c, 35);     /* end of first leaf's last gap */
    r = lc_advance(&cur, 10); /* cross into second leaf */
    assert(r == LC_OK && lc_offset(&cur) == 45);

    /* advance backward across leaf boundary */
    r = lc_advance(&cur, -10);
    assert(r == LC_OK && lc_offset(&cur) == 35);

    /* advline forward across leaf boundary */
    lc_seek(&cur, c, 35);    /* break 3, line 3 */
    r = lc_advline(&cur, 2); /* cross to line 5 (in second leaf) */
    assert(r == LC_OK && lc_line(&cur) == 5);

    /* advline backward across leaf boundary */
    r = lc_advline(&cur, -2);
    assert(r == LC_OK && lc_line(&cur) == 3);

    /* advline to start */
    r = lc_advline(&cur, -100);
    assert(r == LC_OK && lc_line(&cur) == 0);

    /* advline to end (covers lcC_forwardline last-line path) */
    r = lc_advline(&cur, 100);
    assert(r == LC_OK && lc_line(&cur) == 10);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_advance_cov_backward_cross(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 10);
    assert(lc_breaks(c) == 8);

    /* seek to second leaf, move backward across boundary into first leaf */
    lc_seek(&cur, c, 42);
    r = lc_advance(&cur, -5);
    assert(r == LC_OK && lc_offset(&cur) == 37);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_advance_cov_skip_siblings(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 12, 10);
    assert(lc_breaks(c) == 12);

    /* advance forward from leaf 0, skipping past leaf 1 entirely */
    lc_seek(&cur, c, 35);     /* near end of leaf 0 */
    r = lc_advance(&cur, 55); /* skip past leaf 1 (40 bytes) into leaf 2 */
    assert(r == LC_OK && lc_offset(&cur) == 90);

    /* advance backward from leaf 2, skipping past leaf 1 entirely */
    r = lc_advance(&cur, -55); /* skip past leaf 1 backward into leaf 0 */
    assert(r == LC_OK && lc_offset(&cur) == 35);

    /* advance lines forward, skipping past full leaf */
    lc_seekline(&cur, c, 1); /* line 1, in leaf 0 */
    r = lc_advline(&cur, 8); /* skip past leaf 0 rest + full leaf 1 */
    assert(r == LC_OK && lc_line(&cur) == 9);

    /* advance lines backward, skipping past full leaf */
    r = lc_advline(&cur, -8);
    assert(r == LC_OK && lc_line(&cur) == 1);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_advance_cov_backwardoff(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 80, 5);

    /* seek to near-end, advance backward across many children */
    lc_seek(&cur, c, lc_bytes(c));
    r = lc_advance(&cur, -200);
    assert(r == LC_OK);

    r = lc_clearbreaks(&cur, 16);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_advline_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);

    assert(lc_advline(NULL, 1) == LC_ERRPARAM);
    {
        lc_Cursor C;
        memset(&C, 0, sizeof(C));
        assert(lc_advline(&C, 1) == LC_ERRPARAM);
    }

    lc_deltree(S, c);
    lc_close(S);
}

static void test_advline_cross(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 10);
    assert(lc_breaks(c) == 8);

    /* advance lines across leaf boundary */
    lc_seekline(&cur, c, 2); /* line 2, offset 20 */
    r = lc_advline(&cur, 3); /* skip past leaf boundary (4 breaks in leaf 0) */
    assert(r == LC_OK && lc_line(&cur) == 5);

    /* backward across leaf boundary */
    r = lc_advline(&cur, -4);
    assert(r == LC_OK && lc_line(&cur) == 1);

    /* forward to last line */
    lc_seekline(&cur, c, 0);
    r = lc_advline(&cur, 100);
    assert(r == LC_OK && lc_line(&cur) == 8);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_advline_cov_backward_within(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15, 15);
    assert(lc_breaks(c) == 4);

    /* seek to slot=2, advance backward by 1 (d=1 < slot=2 triggers within-leaf
     * path) */
    lc_seek(&cur, c, 30); /* slot=2, in third gap */
    r = lc_advline(&cur, -1);
    assert(r == LC_OK && lc_line(&cur) == 1 && lc_offset(&cur) == 10);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_advline_cov_backward_cross(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10)), botV(leafV(10))));
    lc_Cursor cur;
    int       r;
    assert(c && lc_breaks(c) == 4 && lc_bytes(c) == 40);
    lc_seekline(&cur, c, 3);
    r = lc_advline(&cur, -1);
    assert(r == LC_OK && lc_line(&cur) == 2);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_advline_cov_forward_cross(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 33, 10);
    lc_seekline(&cur, c, 15);
    r = lc_advline(&cur, 1);
    assert(r == LC_OK && lc_line(&cur) == 16);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 20);
    assert(lc_breaks(c) == 2);

    lc_seek(&cur, c, 0);
    r = lc_markbreak(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 3);
    assert(lc_offset(&cur) == 5 && lc_line(&cur) == 1);

    /* verify by seeking past new break */
    lc_seek(&cur, c, 6);
    assert(lc_line(&cur) == 1);

    /* extend line past next break (br > gap): set line length to 100 */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 100);
    assert(r == LC_OK); /* cross-break extension: no error */

    /* null check */
    r = lc_markbreak(NULL, 1);
    assert(r == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;

    assert(lc_markbreak(NULL, 1) == LC_ERRPARAM);
    memset(&C, 0, sizeof(C));
    assert(lc_markbreak(&C, 1) == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_empty(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    r = lc_seek(&cur, c, 0);
    assert(r == LC_OK);
    r = lc_markbreak(&cur, 10);
    assert(r == LC_OK && lc_breaks(c) == 1 && lc_bytes(c) == 10);
    assert(lc_offset(&cur) == 10 && lc_line(&cur) == 1);
    assert(lc_linelen(&cur) == 0); /* at break boundary, gap=0 */

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_crossline(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    int       r;

    /* case 1: large gap split at br=10, line1 [1..99) */
    c = lc_newtree(S);
    lc_scanV(c, 1, 99, 100, 100);
    assert(lc_breaks(c) == 4);
    assert(lc_checktree(c));
    lc_seekline(&C, c, 1);
    assert(lc_offset(&C) == 1 && lc_line(&C) == 1 && lc_linelen(&C) == 99);
    r = lc_markbreak(&C, 10);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 300);
    assert(lc_offset(&C) == 11 && lc_line(&C) == 2 && lc_linelen(&C) == 89);
    lc_deltree(S, c);

    /* case 2: gap split at br=5, line1 offset 10, len=15 */
    c = lc_newtree(S);
    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);
    lc_seekline(&C, c, 1);
    assert(lc_offset(&C) == 10);
    r = lc_markbreak(&C, 5);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_bytes(c) == 40 && lc_breaks(c) == 4);
    lc_deltree(S, c);

    lc_close(S);
}

static void test_markbreak_trailing(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);

    /* trailing gap: splice at end adds virtual bytes, lc_bytes unchanged */
    lc_seek(&cur, c, 40);
    lc_splice(&cur, 0, 20);
    assert(lc_bytes(c) == 40);
    assert(lc_offset(&cur) == 60); /* 40 real + 20 virtual in col */
    assert(lc_line(&cur) == 3);

    r = lc_markbreak(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 4);
    assert(lc_offset(&cur) == 65 && lc_line(&cur) == 4);
    assert(lc_linelen(&cur) == 0);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;
    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);
    lc_seek(&cur, c, 5);
    assert(lc_checktree(c));
    r = lc_markbreak(&cur, 10);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_bytes(c) == 40);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_brzero(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);
    lc_seek(&cur, c, 5); /* gap=5 (in first segment of 10 bytes) */
    r = lc_markbreak(&cur, 0);
    assert(r == LC_OK && lc_breaks(c) == 4);
    assert(lc_offset(&cur) == 5 && lc_line(&cur) == 1);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_crossleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 1);
    assert(lc_breaks(c) == 8 && lc_bytes(c) == 8);
    assert(lc_checktree(c));

    lc_seek(&cur, c, 0);
    r = lc_markbreak(&cur, 100);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_bytes(c) == 100 && lc_breaks(c) == 1);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 5, 10);
    assert(lc_breaks(c) == 5);
    assert(c->root.child_count
           > 1); /* leaf split: root now has 2 leaf children */

    /* add break to first gap in first leaf */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 6);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_node_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 17, 10);
    assert(lc_breaks(c) == 17);

    /* internal node has 5 children (> LC_FANOUT=4), so levels >= 2 */
    /* markbreak at offset 2: splits leaf, triggers internal node split */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_root_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 70, 10);
    assert(lc_breaks(c) == 70);

    /* seek to first gap in first leaf, add break to trigger cascade */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 71);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_root_add(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;
    lc_rscanV(c, 21, 10);
    assert(lc_breaks(c) == 21);
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_cascade(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       k, r;

    lc_rscanV(c, 200, 5);
    assert(lc_breaks(c) == 200);

    for (k = 0; k < 24; ++k) {
        lc_seek(&cur, c, 2);
        r = lc_markbreak(&cur, 2);
        assert(r == LC_OK);
    }

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_cov_split_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 10, 10, 10);
    assert(lc_breaks(c) == 4);

    /* cursor at offset 25 generates slot=2 (>= mid=2), moves to new leaf */
    lc_seek(&cur, c, 25);
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 5);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_markbreak_cov_child_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(1, 1, 1, 1), leafV(1, 1, 1, 1),
                               leafV(1, 1, 1, 1), leafV(1, 1, 1, 1)),
                          botV(leafV(1, 1, 1, 1), leafV(1, 1, 1, 1),
                               leafV(1, 1, 1, 1), leafV(1, 1, 1, 1)),
                          botV(leafV(1, 1, 1, 1), leafV(1, 1, 1, 1),
                               leafV(1, 1, 1, 1), leafV(1, 1, 1, 1)),
                          botV(leafV(1, 1, 1, 1), leafV(1, 1, 1, 1),
                               leafV(1, 1, 1, 1), leafV(1, 1, 1, 1))),
                   innerV(botV(leafV(1, 1, 1, 1)))));
    lc_Cursor C;
    int       r;
    assert(c);
    lc_seek(&C, c, 56);
    r = lc_markbreak(&C, 0);
    assert(r == LC_OK);
    assert(lc_checktree_allow_empty(c, 1));
    lc_deltree(S, c);
    assert(S->nodes.live_obj == 0 && S->leaves.live_obj == 0);
    lc_close(S);
}

static void test_clearbreaks_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    memset(&C, 0, sizeof(C));

    assert(lc_clearbreaks(NULL, 1) == LC_ERRPARAM);
    assert(lc_clearbreaks(&C, 1) == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_clearbreaks(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
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

    /* clear exactly one break at break boundary */
    lc_seek(&C, c, 9);
    r = lc_clearbreaks(&C, 5);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_linelen(&C) == 25);
    assert(lc_checktree(c));

    /* past end: del clamped, no breaks crossed */
    lc_seek(&C, c, 50);
    r = lc_clearbreaks(&C, 20);
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_linelen(&C) == 30);
    assert(lc_bytes(c) == 40);
    assert(lc_checktree(c));

    /* clear all remaining breaks */
    lc_seek(&C, c, 5);
    r = lc_clearbreaks(&C, 40);
    assert(r == LC_OK && lc_breaks(c) == 0 && lc_linelen(&C) == 45);
    assert(lc_bytes(c) == 0);
    assert(lc_checktree(c));

    lc_deltree(S, c);
    lc_close(S);
}

static void test_clearbreaks_cov_slot(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;
    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);
    assert(lc_checktree(c));
    lc_seek(&cur, c, 11);
    r = lc_clearbreaks(&cur, 16);
    assert(r == LC_OK && lc_breaks(c) == 2);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_splice_params(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    memset(&C, 0, sizeof(C));

    lc_splice(NULL, 1, 1);
    lc_splice(&C, 1, 1);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_splice(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;

    lc_rscanV(c, 100, 10);
    assert(lc_breaks(c) == 100 && lc_bytes(c) == 1000);

    lc_seek(&C, c, 0);
    lc_splice(&C, 1000, 0); /* delete all */
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    assert(lc_checktree(c));

    /* second scan on cleared tree */
    lc_rscanV(c, 100, 10);
    assert(lc_breaks(c) == 100 && lc_bytes(c) == 1000);
    lc_seek(&C, c, 11);
    lc_splice(&C, 980, 0); /* delete all but first 11 + last 9 */
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);
    assert(lc_checktree(c));

    lc_scanV(c, 5, 15);

    /* simple splice (no break crossing) */
    lc_seek(&C, c, 2);
    lc_splice(&C, 5, 3);
    assert(lc_bytes(c) == 38 && lc_offset(&C) == 5);

    /* splice crossing breaks */
    lc_seek(&C, c, 0);
    lc_splice(&C, 15, 8);
    assert(lc_bytes(c) == 31); /* 38 - 15 + 8 */
    assert(lc_checktree(c));

    /* splice with del=0, ins=0 (no-op) */
    lc_splice(&C, 0, 0);
    assert(lc_checktree(c));

    /* null check */
    lc_splice(NULL, 1, 1);
    assert(lc_checktree(c));

    lc_deltree(S, c);
    lc_close(S);
}

static void test_splice_trailing(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    lc_scanV(c, 10, 15, 15);
    lc_scan(c, lc_scanner, NULL);

    /* after last break (trailing area): slot=3 (==breaks) */
    lc_seek(&C, c, 40);
    lc_splice(&C, 0, 20); /* insert 20 bytes at end */
    assert(lc_checktree(c));
    assert(lc_bytes(c) == 40);   /* 40 is the last newline */
    assert(lc_offset(&C) == 60); /* offset == line start + col */

    /* verify seek within expanded trailing segment */
    lc_seek(&C, c, 45);
    assert(lc_line(&C) == 3);

    lc_deltree(S, c);
    lc_close(S);
}

/* splice_brute: exhaustive pos+del enumeration on multi-level tree */
static void test_splice_brute(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos, del;
    int const n = 128;

    S = lc_open(&test_alloc, NULL);
    assert(S);
    c = lc_newtree(S);
    lc_rscanV(c, n, 1);
    assert((int)c->levels >= 2);
    assert(lc_checktree(c));
    lc_deltree(S, c);
    assert(S->leaves.live_obj == 0 && S->nodes.live_obj == 0);

    for (pos = 0; pos <= n; ++pos)
        for (del = 0; del <= n - pos; ++del) {
            c = lc_newtree(S);
            lc_rscanV(c, n, 1);
            lc_seek(&C, c, pos);
            fprintf(stderr, "splice pos=%d del=%d\n", pos, del);
            lc_splice(&C, del, 0);
            if (!lc_checktree(c) || !lc_checkcursor(&C, pos)) {
                lc_dumptree(c, "after slice");
                lc_dumpcursor(&C, "after slice");
                abort();
            }
            if (lc_bytes(c) != (size_t)(n - del)) {
                lc_log("splice pos=%d del=%d: bytes=%zu expected=%zu\n", pos,
                       del, lc_bytes(c), (size_t)(n - del));
                abort();
            }
            lc_deltree(S, c);
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
    assert(c);
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
    lc_deltree(S, c);
    lc_close(S);
}

static void test_splice_cov_foldleaf_lr(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(S, 0, botV(leafV(2, 2, 2, 2), leafV(2, 2)));
    assert(c);
    lc_seek(&C, c, 6); /* leaf0 lidx=3 col=0, off=0 loff=6 */
    assert(lc_checktree(c));
    lcD_foldleaf(&C);
    C.loff = lcL_sumbytes(lcK_leaf(&C), 0, C.lnu);
    lc_asserttree(c, 0, botV(leafV(2, 2, 2), leafV(2, 2, 2)));
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 6));
    ;
    lc_deltree(S, c);
    lc_close(S);
}

static void test_splice_cov_shiftnode_bal0(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    lc_Cursor C;
    assert(c);
    lc_seek(&C, c, 25);
    lc_splice(&C, 16, 0);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 25));
    ;
    lc_deltree(S, c);
    assert(S->nodes.live_obj == 0 && S->leaves.live_obj == 0);
    lc_close(S);
}

static void test_splice_cov_trimleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 0), leafV(10, 0), leafV(10, 0)));
    assert(c);
    lc_seek(&cur, c, 5);
    lc_splice(&cur, 25, 0);
    assert(c->root.child_count == 0 && c->bytes == 0 && c->breaks == 0);
    assert(lc_checktree_allow_empty(c, 1));
    lc_deltree(S, c);
    lc_close(S);
}

static void test_boundary_cmp(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c;

    /* lcK_findline: <= -> <. seekline(2) must land in leaf1, not leaf0. */
    c = cacheV(S, 0, botV(leafV(1, 1), leafV(5)));
    assert(lc_checktree(c));
    lc_seekline(&cur, c, 2);
    assert(cur.nu == 2);
    assert(lc_linelen(&cur) == 5);
    lc_deltree(S, c);

    /* lcK_forwardline: <= -> <. advline(4) must skip to leaf2, not stop at
     * leaf1. */
    c = cacheV(S, 0, botV(leafV(1, 1), leafV(1, 1), leafV(5)));
    assert(lc_checktree(c));
    lc_seek(&cur, c, 0);
    lc_advline(&cur, 4);
    assert(cur.nu == 4);
    assert(lc_linelen(&cur) == 5);
    lc_deltree(S, c);

    lc_close(S);
}

static void test_insert_params(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;

    S = lc_open(&test_alloc, NULL);
    assert(S);
    c = lc_newtree(S);
    assert(c);

    assert(lc_insert(NULL, 0, lc_scanner, &pz) == LC_ERRPARAM);
    memset(&C, 0, sizeof(C));
    assert(lc_insert(&C, 0, lc_scanner, &pz) == LC_ERRPARAM);
    lc_seek(&C, c, 0);
    assert(lc_insert(&C, 0, NULL, &pz) == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs[] = {3, 3, 0}, *pb = brs;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(c);

    lc_seek(&C, c, 10);
    r = lc_insert(&C, 3, lc_scanner, &pb);
    assert(r == LC_OK && lc_breaks(c) == 5 && lc_bytes(c) == 49);
    lc_asserttree(c, 0, botV(leafV(10, 3, 3), leafV(18, 15)));
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 19));
    ;
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_col(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
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
    lc_asserttree(c, 0, botV(leafV(4, 2 + 4, 4, 3 + 5)));
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 22);
    assert(lc_checkcursor(&C, 17));
    ;
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_append(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
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
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_many(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
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
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_empty(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs[] = {10, 10, 10, 0}, zero[] = {0}, *pbrs;
    int       r;

    /* case 1: insert with breaks into empty tree */
    c = lc_newtree(S);
    pbrs = brs;
    lc_seek(&C, c, 0);
    r = lc_insert(&C, 5, lc_scanner, &pbrs);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 30);
    lc_deltree(S, c);

    /* case 2: empty tree, e=0, scanner returns 0 (no-op) */
    c = lc_newtree(S);
    pbrs = zero;
    lc_seek(&C, c, 0);
    r = lc_insert(&C, 0, lc_scanner, &pbrs);
    assert(r == LC_OK);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    lc_deltree(S, c);

    lc_close(S);
}

static void test_insert_sib(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 0, botV(leafV(1, 0), leafV(2, 0), leafV(3, 0), NULL));
    lc_Cursor C;
    unsigned  brs[] = {4, 0}, *p = brs;
    lc_seek(&C, c, 0);
    assert(lc_insert(&C, 0, lc_scanner, &p) == LC_OK);
    assert(lc_checktree(c));
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_deep(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    lc_Node  *bot[8], *mid[2];
    unsigned  brs[] = {1, 0}, *p = brs;
    int       i;

    for (i = 0; i < 8; i++)
        bot[i] = botV(leafV(1), leafV(1), leafV(1), leafV(1));
    mid[0] = innerV(bot[0], bot[1], bot[2], bot[3]);
    mid[1] = innerV(bot[4], bot[5], bot[6], bot[7]);
    c = cacheV(S, 2, innerV(mid[0], mid[1]));
    assert(lc_checktree(c));

    lc_seek(&C, c, 1);
    assert(lc_insert(&C, 0, lc_scanner, &p) == LC_OK);
    assert(lc_checktree_allow_empty(c, 1));
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_leaf_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs_b[] = {3, 3, 3, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 5, 5, 5);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 15);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 2, lc_scanner, &pb);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    assert(lc_breaks(c) == 6 && lc_bytes(c) == 26);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_stitch_shiftup(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(1), leafV(1), leafV(1), leafV(1)));
    lc_Cursor C;
    unsigned  brs[] = {0}, *p = brs;
    lc_seek(&C, c, 1);
    assert(lc_insert(&C, 0, lc_scanner, &p) == LC_OK);
    lc_asserttree(c, 0, botV(leafV(1), leafV(1, 1), leafV(1)));
    assert(lc_checkcursor(&C, 1));
    ;
    assert(lc_checktree_allow_empty(c, 1));
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_rootpush(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(1), leafV(1), leafV(1), leafV(1)));
    lc_Cursor C;
    unsigned  brs[] = {2, 2, 2, 2, 2, 0}, *p = brs;
    lc_seek(&C, c, 1);
    assert(lc_insert(&C, 0, lc_scanner, &p) == LC_OK);
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&C, 11));
    ;
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_fillrt_findlevel(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor cur;
    lc_Node  *inner1, *inner2, *inner3, *inner4, *root;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    inner1 = botV(leafV(2), leafV(2));
    inner2 = botV(leafV(2), leafV(2), leafV(2), leafV(2));
    inner3 = botV(leafV(2), leafV(2));
    inner4 = botV(leafV(2), leafV(2));
    root = innerV(inner1, inner2, inner3, inner4);
    c = cacheV(S, 1, root);
    assert(lc_checktree(c));
    lc_seek(&cur, c, 6);
    r = lc_insert(&cur, 0, lc_scanner, &pz);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
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
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_trailing(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs[] = {5, 5, 0}, *pb = brs;
    int       r;
    assert(c);

    lc_scanV(c, 10, 10);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 25);
    assert(lc_offset(&C) == 25 && C.col == 5);
    r = lc_insert(&C, 7, lc_scanner, &pb);
    assert(r == LC_OK);
    assert(lc_checktree(c));
    lc_asserttree(c, 0, botV(leafV(10, 10, 10, 5)));
    assert(lc_checkcursor(&C, 35 + 7));
    ;
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 35);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_brute(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    int       pos;
    unsigned  brs[] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0}, *pbrs;

    S = lc_open(&test_alloc, NULL);
    assert(S);
    c = lc_newtree(S);
    lc_rscanV(c, 128, 1);
    assert((int)c->levels >= 2);
    assert(lc_checktree(c));
    lc_deltree(S, c);
    assert(S->leaves.live_obj == 0 && S->nodes.live_obj == 0);

    for (pos = 0; pos <= 128; ++pos) {
        c = lc_newtree(S);
        lc_rscanV(c, 128, 1);
        lc_seek(&C, c, pos);
        pbrs = brs;
        printf("insert pos=%d\n", pos);
        assert(lc_insert(&C, 0, lc_scanner, &pbrs) == LC_OK);
        if (!lc_checktree(c)) {
            fprintf(stderr, "tree check failed after insert at pos=%d\n", pos);
            lc_dumptree(c, "insert");
            abort();
        }
        assert(lc_breaks(c) == 138);
        assert(lc_bytes(c) == 148);
        if (pos == 0) {
            unsigned brs[] = {10, 2, 128, 1, 0}, *pbrs = brs;
            assert(lc_checkleaves(c, &pbrs));
        } else if (pos == 128) {
            unsigned brs[] = {128, 1, 10, 2, 0}, *pbrs = brs;
            assert(lc_checkleaves(c, &pbrs));
        } else {
            unsigned brs[] = {0, 1, 10, 2, 0, 1, 0}, *pbrs = brs;
            brs[0] = pos, brs[4] = 128 - pos;
            assert(lc_checkleaves(c, &pbrs));
        }
        assert(lc_checkcursor(&C, pos + 20));
        ;
        lc_deltree(S, c);
        assert(S->leaves.live_obj == 0 && S->nodes.live_obj == 0);
    }

    lc_close(S);
}

static void test_insert_oom_trailing(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs[] = {2, 0}, *pbrs = brs;
    int       found = 0, k;
    for (k = 2; k <= 10; ++k) {
        int oom = k;
        S = lc_open(&oom_alloc, &oom);
        if (!S) continue;
        c = lc_newtree(S);
        if (!c) {
            lc_close(S);
            continue;
        }
        {
            size_t slb = S->leaves.live_obj, snb = S->nodes.live_obj;
            lc_seek(&C, c, 0);
            pbrs = brs;
            if (lc_insert(&C, 0, lc_scanner, &pbrs) == LC_ERRMEM) {
                assert(S->leaves.live_obj == slb);
                assert(S->nodes.live_obj == snb);
                found = 1;
            }
        }
        lc_deltree(S, c);
        lc_close(S);
        if (found) break;
    }
    assert(found);
}

static void test_insert_oom_normal(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs_b[] = {17, 1, 0}, *pb;
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
        size_t slb = S->leaves.live_obj, snb = S->nodes.live_obj;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        lc_seek(&C, c, 3);
        pb = brs_b;
        assert(lc_insert(&C, 0, lc_rscanner, &pb) == LC_ERRMEM);
        assert(S->leaves.live_obj == slb);
        assert(S->nodes.live_obj == snb);
    }

    S->leaves.freed = lf;
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_oom_col0(void) {
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
        assert(lc_insert(&C, 0, lc_scanner, &pb) == LC_ERRMEM);
        assert(S->leaves.live_obj == slb);
        assert(S->nodes.live_obj == snb);
    }

    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_oom_shiftup(void) {
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
    assert(lc_checktree(c));

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
        assert(lc_insert(&C, 0, lc_scanner, &p) == LC_ERRMEM);
        assert(S->leaves.live_obj == slb);
        assert(S->nodes.live_obj == snb);
    }

    S->leaves.freed = lf;
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_oom_rootpush(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    lc_Node  *b[4];
    int       oom = 0, i;
    void     *nf;

    assert(S);
    for (i = 0; i < 4; i++) b[i] = botV(leafV(1), leafV(1), leafV(1), leafV(1));
    c = cacheV(S, 1, innerV(b[0], b[1], b[2], b[3]));
    assert(lc_checktree(c));

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
        assert(lc_insert(&C, 0, lc_scanner, &p) == LC_ERRMEM);
        assert(S->nodes.live_obj == snb);
    }

    S->nodes.freed = nf;
    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_oom_deroot(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    int       oom = 0;

    assert(S);
    c = cacheV(
            S, 0,
            botV(leafV(1, 0), leafV(1, 0), leafV(1, 0), leafV(1, 0), NULL));
    assert(lc_checktree(c));
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
        assert(lc_insert(&C, 0, lc_scanner, &p) == LC_ERRMEM);
        assert(S->nodes.live_obj == snb);
    }

    lc_deltree(S, c);
    lc_close(S);
}

static void test_insert_oom_rollback(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;

    assert(S);
    c = cacheV(S, 0, botV(leafV(1, 0), leafV(1, 0), leafV(1, 0), leafV(1, 0)));
    assert(c);

    S->nodes.freed = NULL;
    S->nodes.pages = NULL;

    {
        unsigned bs[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0}, *p = bs;
        int      oom = 1, r;
        S->allocf = oom_alloc;
        S->alloc_ud = &oom;
        lc_seek(&C, c, 1);
        p = bs;
        r = lc_insert(&C, 0, lc_scanner, &p);
        assert(r < 0);
    }

    lc_deltree(S, c);
    lc_close(S);
}

#define TESTS(X)                   \
    X(lifecycle)                   \
    X(scan_params)                 \
    X(scan_basic)                  \
    X(scan_seek)                   \
    X(scan_bulk)                   \
    X(scan_append)                 \
    X(scan_oom_items)              \
    X(scan_oom_flush)              \
    X(scan_oom_build)              \
    X(seek_params)                 \
    X(seek_pastleaf)               \
    X(seek_line_leaf)              \
    X(seek_line_pastleaf)          \
    X(seek_edge)                   \
    X(advance_params)              \
    X(advance_single)              \
    X(advance_cross)               \
    X(advance_cov_backward_cross)  \
    X(advance_cov_skip_siblings)   \
    X(advance_cov_backwardoff)     \
    X(advline_params)              \
    X(advline_cross)               \
    X(advline_cov_backward_within) \
    X(advline_cov_backward_cross)  \
    X(advline_cov_forward_cross)   \
    X(markbreak)                   \
    X(markbreak_params)            \
    X(markbreak_empty)             \
    X(markbreak_crossline)         \
    X(markbreak_trailing)          \
    X(markbreak_noop)              \
    X(markbreak_brzero)            \
    X(markbreak_crossleaf)         \
    X(markbreak_split)             \
    X(markbreak_node_split)        \
    X(markbreak_root_split)        \
    X(markbreak_root_add)          \
    X(markbreak_cascade)           \
    X(markbreak_cov_split_right)   \
    X(markbreak_cov_child_right)   \
    X(clearbreaks_params)          \
    X(clearbreaks)                 \
    X(clearbreaks_cov_slot)        \
    X(splice_params)               \
    X(splice)                      \
    X(splice_trailing)             \
    X(splice_brute)                \
    X(splice_cov_rebalance)        \
    X(splice_cov_foldleaf_lr)      \
    X(splice_cov_shiftnode_bal0)   \
    X(splice_cov_trimleaf)         \
    X(boundary_cmp)                \
    X(insert_params)               \
    X(insert_leaf)                 \
    X(insert_col)                  \
    X(insert_append)               \
    X(insert_many)                 \
    X(insert_empty)                \
    X(insert_sib)                  \
    X(insert_deep)                 \
    X(insert_leaf_split)           \
    X(insert_stitch_shiftup)       \
    X(insert_rootpush)             \
    X(insert_fillrt_findlevel)     \
    X(insert_noop)                 \
    X(insert_trailing)             \
    X(insert_brute)                \
    X(insert_oom_trailing)         \
    X(insert_oom_normal)           \
    X(insert_oom_col0)             \
    X(insert_oom_shiftup)          \
    X(insert_oom_rootpush)         \
    X(insert_oom_deroot)           \
    X(insert_oom_rollback)

#define X(name) {#name, test_##name},
LC_TEST_MAIN("linecache tests")
#undef X