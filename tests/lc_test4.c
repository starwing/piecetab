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

/* T2: scan & seek */

static void test_scan_seek(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);

    r = lc_seek(&cur, c, 0);
    assert(r == LC_OK && lc_offset(&cur) == 0 && lc_line(&cur) == 0);
    assert(lc_linelen(&cur) == 10);

    r = lc_seek(&cur, c, 15);
    assert(r == LC_OK && lc_offset(&cur) == 15 && lc_line(&cur) == 1);

    /* seek to exact break positions */
    r = lc_seek(&cur, c, 10);
    assert(r == LC_OK && lc_offset(&cur) == 10);
    r = lc_seek(&cur, c, 25);
    assert(r == LC_OK && lc_offset(&cur) == 25);

    /* seek to end */
    r = lc_seek(&cur, c, 40);
    assert(r == LC_OK && lc_offset(&cur) == 40);

    /* re-scan: scan into already-populated tree */
    lc_scanV(c, 5, 10);
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 55);

    lc_deltree(S, c);
    lc_close(S);
}

/* T3: seekline */

static void test_seekline(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15);

    r = lc_seekline(&cur, c, 0);
    assert(r == LC_OK && lc_offset(&cur) == 0 && lc_line(&cur) == 0);
    assert(lc_linelen(&cur) == 10);

    r = lc_seekline(&cur, c, 1);
    assert(r == LC_OK && lc_offset(&cur) == 10 && lc_line(&cur) == 1);
    assert(lc_linelen(&cur) == 15);

    r = lc_seekline(&cur, c, 3);
    assert(r == LC_OK && lc_offset(&cur) == 40 && lc_line(&cur) == 3);

    /* null check */
    r = lc_seekline(NULL, c, 0);
    assert(r == LC_ERRPARAM);

    /* out of bounds */
    r = lc_seekline(&cur, c, 99);
    assert(r == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

/* T4: advance & advline within single leaf */

static void test_advance_single(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15);

    r = lc_seek(&cur, c, 5);
    assert(r == LC_OK);
    r = lc_advance(&cur, 10);
    assert(r == LC_OK && lc_offset(&cur) == 15);
    r = lc_advline(&cur, 1);
    assert(r == LC_OK && lc_line(&cur) == 2);

    /* backward within leaf */
    r = lc_advance(&cur, -8);
    assert(r == LC_OK && lc_offset(&cur) == 17);

    /* clamp past end */
    r = lc_advance(&cur, 100);
    assert(r == LC_OK && lc_offset(&cur) == 117);

    /* clamp before start */
    r = lc_advance(&cur, -200);
    assert(r == LC_OK && lc_offset(&cur) == 0);

    /* null/empty checks */
    r = lc_advance(NULL, 1);
    assert(r == LC_ERRPARAM);
    r = lc_advline(NULL, 1);
    assert(r == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

/* T5: cross-leaf navigation (needs 2+ leaves) */

static void test_advance_cross(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 10, 10);
    assert(lc_breaks(c) == 10);
    lc_checktree(c);

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

/* T6: markbreak */

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

/* T7: leaf split via markbreak (needs full leaf) */

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

/* T8: markbreak on empty tree */

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

/* T9: markbreak within trailing gap, virtual extension tracked in col */
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

/* T10: node split and root split (needs many leaves) */

static void test_node_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;

    lc_rscanV(c, 120, 10);
    assert(lc_breaks(c) == 120);
    assert(c->levels >= 1); /* root split created internal level */

    /* navigate in multi-level tree */
    lc_seekline(&cur, c, 60);
    assert(lc_line(&cur) == 60 && lc_offset(&cur) == 600);

    lc_seek(&cur, c, 1200);
    assert(lc_line(&cur) == 120 && lc_offset(&cur) == 1200);

    lc_deltree(S, c);
    lc_close(S);
}

/* T11: clearbreaks */

static void test_clearbreaks(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15, 15);
    assert(lc_breaks(c) == 4);

    /* clear breaks in middle of gap (no breaks to remove) */
    lc_seek(&cur, c, 2);
    r = lc_clearbreaks(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 4); /* no breaks in range */
    lc_checktree(c);

    /* clear 1 break at break boundary */
    lc_seek(&cur, c, 9);
    r = lc_clearbreaks(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 3);
    lc_checktree(c); /* [25, 40, 55] */

    /* null check */
    r = lc_clearbreaks(NULL, 1);
    assert(r == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

/* T12: clearbreaks with 0-length */

static void test_clearbreaks_edge(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15, 15);
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 55);

    /* len == 0: no-op */
    lc_seek(&cur, c, 5);
    r = lc_clearbreaks(&cur, 0);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_linelen(&cur) == 10);
    lc_checktree(c);

    /* clear exactly one break: seek to break boundary */
    lc_seek(&cur, c, 9);
    r = lc_clearbreaks(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_linelen(&cur) == 25);
    lc_checktree(c);

    /* past end of tree: del clamped to remaining (5), ins=20,
       tree had 3 breaks after previous case; no break crossed here */
    r = lc_seek(&cur, c, 50);
    assert(r == LC_OK && lc_linelen(&cur) == 15);
    r = lc_clearbreaks(&cur, 20);
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_linelen(&cur) == 30);
    assert(lc_bytes(c) == 40);
    lc_checktree(c);

    r = lc_seek(&cur, c, 5);
    assert(r == LC_OK && lc_offset(&cur) == 5);
    r = lc_clearbreaks(&cur, 40);
    assert(r == LC_OK && lc_breaks(c) == 0 && lc_linelen(&cur) == 45);
    assert(lc_bytes(c) == 0);

    lc_deltree(S, c);
    lc_close(S);
}

/* splice_tmp: 简化跨叶删除，从非零位置删大部分内容 */
static void test_splice_tmp(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;

    lc_rscanV(c, 17, 10);
    assert(lc_breaks(c) == 17 && lc_bytes(c) == 170);
    lc_checktree(c);
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 170, 0); /* delete all from 0 */
    lc_checktree(c);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);

    /* 重建，从 offset 25 删 140 bytes */
    lc_rscanV(c, 17, 10);
    assert(lc_breaks(c) == 17 && lc_bytes(c) == 170);
    lc_seek(&cur, c, 25);
    lc_splice(&cur, 140, 0);
    lc_checktree(c);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_splice_l2(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;

    lc_rscanV(c, 65, 10);
    assert(lc_breaks(c) == 65 && lc_bytes(c) == 650);

    /* delete all */
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 650, 0);
    lc_checktree(c);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);

    /* rebuild, partial delete from offset 25 */
    lc_rscanV(c, 65, 10);
    assert(lc_breaks(c) == 65 && lc_bytes(c) == 650);
    lc_seek(&cur, c, 25);
    lc_splice(&cur, 600, 0); /* delete 600, keep 25+25=50 */
    lc_checktree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* T13: splice */

static void test_splice(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;

    lc_rscanV(c, 100, 10);
    assert(lc_breaks(c) == 100 && lc_bytes(c) == 1000);

    lc_seek(&cur, c, 0);
    lc_splice(&cur, 1000, 0); /* delete all */
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    lc_checktree(c);

    /* 第二次扫描（树已清空） */
    lc_rscanV(c, 100, 10);
    assert(lc_breaks(c) == 100 && lc_bytes(c) == 1000);
    lc_seek(&cur, c, 11);
    lc_splice(&cur, 980, 0); /* delete all but first 11 + last 9 */
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);
    lc_checktree(c);

    lc_scanV(c, 5, 15);

    /* simple splice (no break crossing) */
    lc_seek(&cur, c, 2);
    lc_splice(&cur, 5, 3);
    assert(lc_bytes(c) == 38 && lc_offset(&cur) == 5);

    /* splice crossing breaks */
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 15, 8);
    assert(lc_bytes(c) == 31); /* 38 - 15 + 8 */
    lc_checktree(c);

    /* splice with del=0, ins=0 (no-op) */
    lc_splice(&cur, 0, 0);
    lc_checktree(c);

    /* null check */
    lc_splice(NULL, 1, 1);
    lc_checktree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* T14: splice trailing area */

static void test_splice_trailing(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    lc_scanV(c, 10, 15, 15);
    lc_scan(c, lc_scanner, NULL);

    /* after last break (trailing area): slot=3 (==breaks) */
    lc_seek(&cur, c, 40);
    lc_splice(&cur, 0, 20); /* insert 20 bytes at end */
    lc_checktree(c);
    assert(lc_bytes(c) == 40);     /* 40 is the last newline */
    assert(lc_offset(&cur) == 60); /* offset == line start + col */

    /* verify seek within expanded trailing segment */
    lc_seek(&cur, c, 45);
    assert(lc_line(&cur) == 3);

    lc_deltree(S, c);
    lc_close(S);
}

/* T15: seek past first leaf (covers lcC_findpieces skip-child path) */

static void test_seek_past_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 10);
    assert(lc_breaks(c) == 8);

    /* seek to offset past first leaf (first leaf has 4 breaks, 40 bytes) */
    r = lc_seek(&cur, c, 45);
    assert(r == LC_OK && lc_offset(&cur) == 45 && lc_line(&cur) == 4);

    lc_deltree(S, c);
    lc_close(S);
}

/* T16: backward cross-sibling (covers lcC_backwardoff cross-sibling) */

static void test_advance_backward_cross(void) {
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

/* T17: line move cross-sibling (covers lcC_forwardline/backwardline cross
 * paths) */

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

/* coverage: backward cross-leaf (L490 in lcK_backwardline) */
/* coverage: forward cross-node (L466 in lcK_forwardline via lcK_findline) */
static void test_forwardline_crossnode(void) {
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

/* T18: markbreak triggers internal node split */

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

/* T19: markbreak triggers root split */

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

/* T20: markbreak leaf split, cursor moves to new leaf */

static void test_markbreak_split_right(void) {
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

/* T21: backward line within same leaf (d < C->slot) */

static void test_advline_backward_within(void) {
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

/* T22: splice that deletes across break boundaries */

static void test_splice_cross_breaks(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);

    /* splice from start, deleting across first break: del=15, ins=8 */
    /* starts at offset 0 (slot=0), deletes past gap (10) into next gap */
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 15, 8);
    assert(lc_bytes(c) == 33); /* 40 - 15 + 8 */

    lc_deltree(S, c);
    lc_close(S);
}

/* T23: splice at non-zero slot crossing breaks (slot > 0) */

static void test_splice_cross_breaks_slot(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);

    /* delete from offset 15 (slot=1), past break at 25 into third gap */
    lc_seek(&cur, c, 15);
    lc_splice(&cur, 15, 5);
    assert(lc_bytes(c) == 30); /* 40 - 15 + 5 */
    /* cross-break deletion at slot>0 exercised */

    lc_deltree(S, c);
    lc_close(S);
}

/* T24: splice at non-zero slot crossing breaks */

static void test_splice_cross_breaks_mid(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);

    /* delete from offset 2 (slot=0) past break at 10 into second gap */
    lc_seek(&cur, c, 2);
    lc_splice(&cur, 13, 5);
    assert(lc_bytes(c) == 32); /* 40 - 13 + 5 */
    /* cross-break deletion code exercised */

    lc_deltree(S, c);
    lc_close(S);
}

/* T25: forward/backward skipping multiple siblings */

static void test_advance_skip_siblings(void) {
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

/* T26: node split with cursor moving to right half */

static void test_node_split_cursor_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 21, 10);

    /* seek near end of leaf 2 (in right half of first internal node),
       then markbreak to trigger leaf split → internal node overflow → split.
       cursor should be in right half of the split */
    lc_seek(&cur, c, 85); /* offset 85 in leaf 2 */
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}

/* T27: multi-level tree markbreak (exercises internal node structure) */

static void test_markbreak_root_split_on_add(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 150, 5);

    /* markbreak in first gap to trigger cascading splits */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 2);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}

/* T28: splitleaf cursor stays in old leaf (slot < mid) */

static void test_splitleaf_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 10, 10, 10);
    assert(lc_breaks(c) == 4);

    /* markbreak in first gap (slot=0), stays in old leaf (0 < mid=2) */
    lc_seek(&cur, c, 3);
    r = lc_markbreak(&cur, 2);
    assert(r == LC_OK && lc_breaks(c) == 5);

    lc_deltree(S, c);
    lc_close(S);
}

/* T29: root split cursor in left half */

static void test_rootsplit_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 17, 10);
    assert(lc_breaks(c) == 17);

    /* markbreak at first leaf (oi=0 < mid) triggers root split with left path
     */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 18);

    lc_deltree(S, c);
    lc_close(S);
}

/* T30: findlines skip-child (pline > breaks[i]) */

static void test_findlines_skip(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 6, 10);
    assert(lc_breaks(c) == 6);

    /* seekline to line 4 (past first child's 2 breaks) */
    r = lc_seekline(&cur, c, 4);
    assert(r == LC_OK && lc_line(&cur) == 4);

    lc_deltree(S, c);
    lc_close(S);
}

/* T31: root split left path via markbreak on deep tree */

static void test_rootsplit_left_deep(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 100, 5);

    /* seek to first leaf, add break to trigger cascade from left side */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 2);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}

/* T32: node split cursor stays in old node (paths[level+1] < mid) */

static void test_nodesplit_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 22, 10);
    assert(lc_breaks(c) == 22);

    /* seek to first leaf, add break → leaf split triggers internal node split,
       cursor stays in old node (slot < mid) */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 23);

    lc_deltree(S, c);
    lc_close(S);
}

/* T33: findlines skip-child in deep multi-level tree */

static void test_findlines_skip_deep(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 80, 5);

    /* seek to near-end → advline back by many lines → backwardline traverses
     * deep
     */
    lc_seekline(&cur, c, 0);
    r = lc_advline(&cur, 75);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}

/* T34: backwardoff skip-child in deep tree */

static void test_backwardoff_skip(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 80, 5);

    /* seek to near-end, advance backward across many children */
    lc_seek(&cur, c, lc_bytes(c));
    r = lc_advance(&cur, -200);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}

/* T35: clearbreaks edge: slot > last, and clamping */

static void test_clearbreaks_slot(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);

    /* seek just after first break (offset 11), clear through second break;
       cursor slot ends up past cleared range */
    lc_seek(&cur, c, 11);
    r = lc_clearbreaks(&cur, 16);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}

/* T36: comprehensive deep tree operations to cover remaining split paths */

static void test_split_cascade(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       k, r;

    lc_rscanV(c, 200, 5);
    assert(lc_breaks(c) == 200);

    /* repeatedly markbreak from leftmost leaf to fill and overflow left-side
     * nodes */
    for (k = 0; k < 24; ++k) {
        lc_seek(&cur, c, 2);
        r = lc_markbreak(&cur, 2);
        assert(r == LC_OK);
    }

    assert(lc_col(NULL) == 0);

    lc_deltree(S, c);
    lc_close(S);
}

/* T37: markbreak noop — br == gap should be a no-op */
static void test_markbreak_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    size_t    orig_bytes, orig_breaks;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3);
    lc_seek(&cur, c, 5);
    orig_bytes = lc_bytes(c), orig_breaks = lc_breaks(c);
    r = lc_markbreak(&cur, lc_linelen(&cur) - lc_col(&cur)); /* br == gap */
    assert(r == LC_OK && lc_breaks(c) == orig_breaks
           && lc_bytes(c) == orig_bytes);

    lc_deltree(S, c);
    lc_close(S);
}

/* T38: markbreak br=0 — break at cursor position */
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

/* T39: markbreak cross-line within single leaf */
/* TODO: br > remain path needs cursor-state fix before testing extension */

static void test_markbreak_crossline(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 1, 99, 100, 100);
    assert(lc_breaks(c) == 4);
    lc_checktree(c);

    /* line 1 starts at offset 1, len=99 (=100-1). split at br=10 */
    lc_seekline(&cur, c, 1);
    assert(lc_offset(&cur) == 1 && lc_line(&cur) == 1
           && lc_linelen(&cur) == 99);
    r = lc_markbreak(&cur, 10);
    assert(r == LC_OK);
    lc_checktree(c);
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 300);
    assert(lc_offset(&cur) == 11 && lc_line(&cur) == 2
           && lc_linelen(&cur) == 89);

    lc_deltree(S, c);
    lc_close(S);
}

/* T40: markbreak cross-line within line */
static void test_markbreak_crossline_end(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);
    lc_checktree(c);

    /* line 1 (offset 10, len=15): split at br=5 within line */
    lc_seekline(&cur, c, 1);
    assert(lc_offset(&cur) == 10);
    r = lc_markbreak(&cur, 5);
    assert(r == LC_OK);
    lc_checktree(c);
    assert(lc_bytes(c) == 40 && lc_breaks(c) == 4);

    lc_deltree(S, c);
    lc_close(S);
}

/* T42: markbreak cross-leaf: extend line across leaf boundary */
static void test_markbreak_crossleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;

    lc_rscanV(c, 8, 1);
    assert(lc_breaks(c) == 8 && lc_bytes(c) == 8);
    lc_checktree(c);

    lc_seek(&cur, c, 0);
    r = lc_markbreak(&cur, 100);
    assert(r == LC_OK);
    lc_checktree(c);
    assert(lc_bytes(c) == 100 && lc_breaks(c) == 1);

    lc_deltree(S, c);
    lc_close(S);
}

/* splitchild cursor in right half of node split */
static void test_markbreak_splitchild_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r;
    lc_rscanV(c, 31, 1);
    lc_seek(&cur, c, 65);
    r = lc_markbreak(&cur, 7);
    assert(r == LC_OK);
    lc_deltree(S, c);
    lc_close(S);
}

/* coverage: backwardline outer for iteration (L488).
 * lc_scan creates full leaves (4 breaks each). With a half-tree where the
 * deepest-level parent has only 1 child, the inner for loop is empty and
 * the outer for continues to the next level. */
static void test_backwardline_cross(void) {
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

/* ================================================================ */
/*  Phase 10: splice coverage tests (public API only)               */
/* ================================================================ */

/* splice_uf_last: splice makes last leaf underfull → mergeleaf backtrack.
 * botV(leafV(2,2,2,2), leafV(2,2,2)). C at pos 9 (leaf1, lidx=0 col=1),
 * delete 4 bytes within leaf1 → underfull → foldleaf fails (4+1>4) → mergeleaf
 * backtracks to leaf0 → balanceleaf d>0 + L749 o!=*l + L846 mergeleaf. */
static void test_splice_uf_last(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(S, 0, botV(leafV(2, 2, 2, 2), leafV(2, 2, 2)));
    assert(c);
    lc_seek(&C, c, 9); /* leaf1 lidx=0 col=1 */
    lc_splice(&C, 4, 0);
    lc_asserttree(c, 0, botV(leafV(2, 2, 2), leafV(2, 2)));
    lc_checktree(c);
    lc_checkcursor(&C, 9);
    lc_deltree(S, c);
    lc_close(S);
}

/* splice_mergeleaf_sr: mergeleaf d>0 o==*l lidx>=mid (L749).
 * botV(leafV(2,2,2,2), leafV(2,2,2,2)), C pos 7 lidx=3 col=1, del=5. */
static void test_splice_mergeleaf_sr(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(S, 0, botV(leafV(2, 2, 2, 2), leafV(2, 2, 2, 2)));
    assert(c);
    lc_seek(&C, c, 7);
    lc_splice(&C, 5, 0);
    lc_asserttree(c, 0, botV(leafV(2, 2, 2), leafV(3, 2)));
    lc_checktree(c);
    lc_checkcursor(&C, 7);
    lc_deltree(S, c);
    lc_close(S);
}

/* splice_mergenode_fold: mergenode foldnode fallback (L780-781).
 * levels=2 with 3 innerV children (cc=3). seek(1) in inner0,
 * del=7 → R at offset 8 (start of inner2). After Phase 1
 * prune removes inner1 (middle), R still in parent. Phase 2
 * mergenode at dl=1 merges inner0+inner2 botV children. */
static void test_splice_mergenode_fold(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2, 2))), innerV(botV(leafV(2, 2))),
                   innerV(botV(leafV(2, 2)))));
    assert(c);
    lc_seek(&C, c, 1);
    lc_splice(&C, 7, 0);
    lc_asserttree(c, 0, botV(leafV(3, 2)));
    lc_checktree(c);
    lc_checkcursor(&C, 1);
    lc_deltree(S, c);
    lc_close(S);
}

/* splice_mergenode_last: mergenode last-child backtrack (L775-777)
 * + o!=*n cursor adjust (L789) + rebalance mergenode (L804-805).
 * levels=2: inner0 botV with 1 leaf, inner1 botV with 2 leaves.
 * splice from inner0 into inner1, Phase1 merges into inner0 side.
 * After prune removed==1, rebalance at l-1=0 triggers foldnode
 * which fails (cl+cr>FANOUT), then mergenode. */
static void test_splice_mergenode_last(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2, 2))),
                   innerV(botV(leafV(2), leafV(2), leafV(2), leafV(2)))));
    assert(c);
    lc_seek(&C, c, 2);   /* in inner0 botV leaf, offset 2 */
    lc_splice(&C, 5, 0); /* R at offset 7, in inner1 */
    lc_asserttree(c, 0, botV(leafV(2, 1, 2), leafV(2)));
    lc_checktree(c);
    lc_checkcursor(&C, 2);
    lc_deltree(S, c);
    lc_close(S);
}

/* splice_removed2: splicerange removed>1 foldnode success (L900-901)
 * + removed>1 mergenode (L903).
 * levels=2: 4 innerV children. L in inner0, R in inner3.
 * l=0, prune removes inner1+inner2 (removed=2).
 * foldnode at l-1=-1 ... hmm, l=0 so removed>1
 * but l==0 → takes rebalance(L,0) shortcut at L898!
 * Need l>0: levels=3 or change strategy.
 * levels=3: root→innerV→innerV→botV→leaf.
 * innerV(innerV(botV(...), ...), ...) with 4 innerV level-1 children.
 * L in child0, R in child3, l=1, prune removes child1+child2. */
static void test_splice_removed2(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    lc_rscanV(c, 65, 10);
    assert(lc_breaks(c) == 65);

    /* Delete large middle portion, leaving edges intact.
     * Offset 60, del=500 keeps L and R in different subtrees. */
    lc_seek(&C, c, 60);
    lc_splice(&C, 500, 0);
    lc_checktree(c);
    lc_checkcursor(&C, 60);

    lc_deltree(S, c);
    lc_close(S);
}

/* empty_tree_reset: lcD_emptytree path — delete all at offset 0
 * (with and without insert) to cover both early return branches
 * and the post-delete empty-tree reset in lc_splice. */
static void test_empty_tree_reset(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;

    /* delete all (ins=0) */
    c = cacheV(S, 0, botV(leafV(10, 10), leafV(10)));
    assert(c);
    lc_seek(&C, c, 0);
    lc_splice(&C, 30, 0);
    assert(c->bytes == 0 && c->breaks == 0);
    assert(lc_offset(&C) == 0);
    lc_checktree(c);
    lc_deltree(S, c);

    /* delete all + insert (ins>0) to cover C->col += ins in lcD_emptytree */
    c = cacheV(S, 0, botV(leafV(10, 10), leafV(10)));
    lc_seek(&C, c, 0);
    lc_splice(&C, 30, 7);
    assert(c->bytes == 0 && c->breaks == 0);
    assert(C.col == 7);
    lc_checktree_allow_empty(c, 1);
    lc_deltree(S, c);

    lc_close(S);
}

/* bulk scan: 0 entries — scanner returns 0 immediately on empty tree */
static void test_scan_no_input(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    unsigned  brs[] = {0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, lc_scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 0 && lc_bytes(c) == 0);
    lc_checktree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* bulk scan: exactly one full leaf (LC_LEAF_FANOUT entries) */
static void test_scan_one_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    unsigned  brs[] = {5, 10, 15, 20, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, lc_scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 50);
    assert(c->levels == 0 && c->root.child_count == 1);
    lc_checktree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* bulk scan: 120 entries → multi-level tree */
static void test_scan_bulk_many(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[] = {120, 1, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, lc_rscanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 120 && lc_bytes(c) == 120);
    assert(c->levels >= 2);
    lc_checktree(c);
    lc_seek(&cur, c, 0);
    assert(lc_offset(&cur) == 0);
    lc_seek(&cur, c, 120);
    assert(lc_offset(&cur) == 120);
    lc_deltree(S, c);
    lc_close(S);
}

/* scan append: 2nd scan into non-empty tree */
static void test_scan_append_many(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    unsigned  brs_a[] = {4, 10, 0}, *pa = brs_a;
    unsigned  brs_b[] = {5, 20, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, lc_rscanner, &pa);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 40);
    r = lc_scan(c, lc_rscanner, &pb);
    assert(r == LC_OK && lc_breaks(c) == 9 && lc_bytes(c) == 140);
    lc_checktree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* scan OOM: fail allocating pend_root (node page) during lcB_checkpendroot.
 * alloc order: #1 lc_open, #2 lc_newtree, #3 node page ← fail */
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

/* scan OOM: fail during flush cascade when pend has internal nodes.
 * LC_PAGE_SIZE=512 → node page holds 4 nodes; leaf page 30 leaves.
 * With 170 segments the cascade exhausts node page → allocf #5 triggers OOM.
 * This exercises lcB_disposepend freeing internal-node children via
 * lcN_freechildren. */
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

/* scan OOM: fail allocating leaf page during lcB_fill.
 * alloc order: #1 lc_open, #2 lc_newtree, #3 node page, #4 leaf page ← fail */
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

/* boundary comparison: verify < vs <= at child traversal edges.
 * forward traversals must use < so boundary values land in next child;
 * backward traversals correctly use <= so boundary values stay in current. */
static void test_boundary_cmp(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c;

    /* lcK_findline L359: <= -> <.
     * leaf0: 2 segs 1B, leaf1: 1 seg 5B. seekline(2)=leaf1 首行.
     * <= 误停 leaf0, *pline=2, findinleaf 耗尽双段断言崩. */
    c = cacheV(S, 0, botV(leafV(1, 1), leafV(5)));
    lc_checktree(c);
    lc_seekline(&cur, c, 2);
    assert(cur.idx == 2);
    assert(lc_linelen(&cur) == 5);
    lc_deltree(S, c);

    /* lcK_forwardline L446: <= -> <.
     * 3 叶: leaf0(2段), leaf1(2段), leaf2(1段). advline(4) 自始=leaf2.
     * <= 误停 leaf1, d=2, findinleaf 耗尽断言崩. */
    c = cacheV(S, 0, botV(leafV(1, 1), leafV(1, 1), leafV(5)));
    lc_checktree(c);
    lc_seek(&cur, c, 0);
    lc_advline(&cur, 4);
    assert(cur.idx == 4);
    assert(lc_linelen(&cur) == 5);
    lc_deltree(S, c);

    lc_close(S);
}

/* main */

/* insert into non-empty tree: single leaf, middle insert */
static void test_insert_single_leaf(void) {
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
    lc_checktree(c);
    lc_checkcursor(&C, 19);
    lc_deltree(S, c);
    lc_close(S);
}

/* insert col>0: split current line, merge first br, prepend e */
static void test_insert_col_mid(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs_b[] = {4, 4, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 4, 7);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 11);

    lc_seek(&C, c, 6);
    assert(C.lidx == 1 && C.col == 2);
    r = lc_insert(&C, 3, lc_scanner, &pb);
    assert(r == LC_OK);
    lc_checktree(c);
    lc_asserttree(c, 0, botV(leafV(4, 2 + 4, 4, 3 + 5)));
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 22);
    lc_deltree(S, c);
    lc_close(S);
}

/* br==0, e>0: add e to current line */
static void test_insert_no_scanner(void) {
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
    lc_checktree(c);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 27);
    assert(lc_linelen(&C) == 17);
    lc_deltree(S, c);
    lc_close(S);
}

/* trailing area insert */
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
    lc_checktree(c);
    lc_asserttree(c, 0, botV(leafV(10, 10, 10, 5)));
    lc_checkcursor(&C, 35 + 7);
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 35);
    lc_deltree(S, c);
    lc_close(S);
}

/* insert causing leaf split */
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
    lc_checktree(c);
    assert(lc_breaks(c) == 6 && lc_bytes(c) == 26);
    lc_deltree(S, c);
    lc_close(S);
}

/* br==0, e==0: no-op */
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
    lc_checktree(c);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);
    lc_deltree(S, c);
    lc_close(S);
}

/* verify cursor position after col>0 insert */
static void test_insert_cursor_pos(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs_b[] = {4, 4, 0}, *pb = brs_b;
    int       r;

    lc_scanV(c, 4, 7);
    lc_seek(&C, c, 6);
    assert(C.lidx == 1 && C.col == 2);
    r = lc_insert(&C, 3, lc_scanner, &pb);
    assert(r == LC_OK);
    lc_asserttree(c, 0, botV(leafV(4, 2 + 4, 4, 3 + 5)));
    lc_checktree(c);
    lc_checkcursor(&C, 6 + 4 + 4 + 3);
    assert(C.idx == 3 && C.col == 3);
    assert(lc_offset(&C) == 17);
    lc_deltree(S, c);
    lc_close(S);
}

/* insert enough data to trigger driverun while-loop (fill->flush->refill) */
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
    lc_checktree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* empty tree insert */
static void test_insert_empty(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs[] = {10, 10, 10, 0}, *pbrs = brs;
    int       r;

    lc_seek(&C, c, 0);
    r = lc_insert(&C, 5, lc_scanner, &pbrs);
    assert(r == LC_OK);
    lc_checktree(c);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 30);
    lc_deltree(S, c);
    lc_close(S);
}

/* empty tree + no scanner + no e: hits flushat empty-pend path */
static void test_insert_empty_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  zero[] = {0}, *pz = zero;
    int       r;

    lc_seek(&C, c, 0);
    r = lc_insert(&C, 0, lc_scanner, &pz);
    assert(r == LC_OK);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);
    lc_deltree(S, c);
    lc_close(S);
}

/* OOM in driverun for trailing path */
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

/* OOM in driverun for normal path */
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
    lc_checktree(c);

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

/* OOM in lcB_applyfirst col==0 path (white-box: drain leaf freelist to 1) */
static void test_insert_oom_col0(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs_b[] = {1, 1, 1, 1, 1, 0}, *pb;
    int       count, oom = 0;
    void     *head;

    assert(S);
    c = cacheV(S, 0, botV(leafV(5, 5)));
    lc_checktree(c);

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

/* insert with NULL parameters */
static void test_insert_param_null(void) {
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

/* cov: splice deletes to tree end, R at locend triggers lcD_trimleaf line 615
 */
static void test_splice_trimleaf_locend(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 0), leafV(10, 0), leafV(10, 0)));
    assert(c);
    lc_seek(&cur, c, 5);
    lc_splice(&cur, 25, 0);
    assert(c->root.child_count == 0 && c->bytes == 0 && c->breaks == 0);
    lc_checktree_allow_empty(c, 1);
    lc_deltree(S, c);
    lc_close(S);
}

/* cov: spliceleaf underfills leaf → foldleaf merge → rebalance → foldnode
 * balance d<0 (lines 671-674, 680, 745-755) */
static void test_balancenode_dneg(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2, 2), leafV(2, 2, 2)),
                          botV(leafV(2), leafV(2), leafV(2), leafV(2))),
                   innerV(botV(leafV(2)))));
    lc_Cursor C;
    assert(c);
    lc_seek(&C, c, 0);
    lc_splice(&C, 3, 0);
    lc_asserttree(
            c, 2,
            innerV(innerV(botV(leafV(1, 2, 2, 2), leafV(2), leafV(2)),
                          botV(leafV(2), leafV(2))),
                   innerV(botV(leafV(2)))));
    lc_checktree_allow_empty(c, 1);
    lc_checkcursor(&C, 0);
    lc_deltree(S, c);
    lc_close(S);
}

/* cov: spliceleaf underfills right leaf → foldleaf merge → botV cc=1 →
 * rebalance → foldnode at inner level with cl=4 cr=1 d=1>0 */
static void test_balancenode_dpos(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2), leafV(2), leafV(2), leafV(2)),
                          botV(leafV(2, 2, 2, 2), leafV(2))),
                   innerV(botV(leafV(2)))));
    lc_Cursor C;
    assert(c);
    lc_seek(&C, c, 8);
    lc_splice(&C, 7, 0);
    lc_asserttree(
            c, 2,
            innerV(innerV(botV(leafV(2), leafV(2)),
                          botV(leafV(2), leafV(2), leafV(1, 2))),
                   innerV(botV(leafV(2)))));
    lc_checktree_allow_empty(c, 1);
    lc_checkcursor(&C, 8);
    lc_deltree(S, c);
    lc_close(S);
}

/* cov: splicerange triggers foldleaf with dl>0 and cursor switch right
 * (line 723). levels=1 cross-leaf splice with balance redistribution. */
static void test_foldleaf_switch(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(2, 2, 2, 2), leafV(2, 2, 2)),
                   botV(leafV(2, 2, 2, 2), leafV(2))));
    assert(c);
    lc_seek(&C, c, 3);
    lc_splice(&C, 10, 0);
    lc_checkcursor(&C, 3);
    lc_checktree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* cov: foldnode cursor switch left (line 750). dn<0, cursor in right
 * child at moved position. root has botV(cc=2) + botV(cc=4).
 * Splice crosses botVs, Phase 2 foldnode balances at root level. */
static void test_foldnode_cursor_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(2), leafV(2)),
                   botV(leafV(2), leafV(2), leafV(2), leafV(2))));
    lc_Cursor C;
    assert(c);
    lc_seek(&C, c, 2);
    lc_splice(&C, 3, 0);
    lc_checktree(c);
    lc_checkcursor(&C, 2);
    lc_deltree(S, c);
    lc_close(S);
}

/* cov: spliceleaf underfills leaf → foldleaf merge → botV.cc=1 →
 * rebalance → foldnode merge (cl+cr≤4, returns 1) → line 765. */
static void test_rebalance_merge(void) {
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
     * → foldnode at inner0: cl=1 cr=2 → merge (returns 1) → line 765 */
    lc_seek(&C, c, 0);
    lc_splice(&C, 3, 0);
    lc_asserttree(
            c, 2,
            innerV(innerV(botV(leafV(1, 2, 2, 2), leafV(2, 2), leafV(2, 2))),
                   innerV(botV(leafV(2)))));
    lc_checktree_allow_empty(c, 1);
    lc_checkcursor(&C, 0);
    lc_deltree(S, c);
    lc_close(S);
}

/* foldleaf dl<0 *ls!=o lidx<-dl: cursor in right leaf, data moves
 * right->left, cursor among moved segments → switches to left leaf. */
static void test_foldleaf_switch_rl(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(S, 0, botV(leafV(2, 2), leafV(2, 2, 2, 2)));
    assert(c);
    lc_seek(&C, c, 4); /* leaf1 lidx=0 col=0, off=4 loff=0 */
    lc_checktree(c);
    lcD_foldleaf(&C);
    C.loff = lcL_sumbytes(lcK_leaf(&C), 0, C.lidx);
    lc_asserttree(c, 0, botV(leafV(2, 2, 2), leafV(2, 2, 2)));
    lc_checktree(c);
    lc_checkcursor(&C, 4);
    lc_deltree(S, c);
    lc_close(S);
}

/* foldleaf dl>0 *ls==o lidx>=cl-dl: cursor in left leaf, data moves
 * left->right, cursor among moved segments → switches to right leaf. */
static void test_foldleaf_switch_lr(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(S, 0, botV(leafV(2, 2, 2, 2), leafV(2, 2)));
    assert(c);
    lc_seek(&C, c, 6); /* leaf0 lidx=3 col=0, off=0 loff=6 */
    lc_checktree(c);
    lcD_foldleaf(&C);
    C.loff = lcL_sumbytes(lcK_leaf(&C), 0, C.lidx);
    lc_asserttree(c, 0, botV(leafV(2, 2, 2), leafV(2, 2, 2)));
    lc_checktree(c);
    lc_checkcursor(&C, 6);
    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode dn<0 *ns!=o pathidx<-dn: cursor in right botV, data moves
 * right->left, cursor among moved children → switches to left botV. */
static void test_foldnode_switch_rl(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *root = innerV(
            botV(leafV(2, 2), leafV(2, 2)),
            botV(leafV(2, 2), leafV(2, 2), leafV(2, 2), leafV(2, 2)));
    lc_Cache *c = cacheV(S, 1, root);
    lc_Cursor C;
    assert(c);
    lc_seek(&C, c, 8);
    lc_checktree(c);
    lcD_foldnode(&C, 0);
    /* expected: off=0 loff=8, cursor in botV0 child[2] */
    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode dn>0 *ns==o pathidx>=cl-dn: cursor in left botV, data moves
 * left->right, cursor among moved children → switches to right botV. */
static void test_foldnode_switch_lr(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *root = innerV(
            botV(leafV(2, 2), leafV(2, 2), leafV(2, 2), leafV(2, 2)),
            botV(leafV(2, 2), leafV(2, 2)));
    lc_Cache *c = cacheV(S, 1, root);
    lc_Cursor C;
    assert(c);
    lc_seek(&C, c, 12);
    lc_checktree(c);
    lcD_foldnode(&C, 0);
    /* expected: off=24 loff=0, cursor in botV1 child[0] */
    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode *ns!=o fallback: cursor in right botV, pathidx not in moved
 * range → stays in right botV with adjusted path index. */
static void test_foldnode_fallback_r(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *root = innerV(
            botV(leafV(2, 2), leafV(2, 2), leafV(2, 2), leafV(2, 2)),
            botV(leafV(2, 2), leafV(2, 2)));
    lc_Cache *c = cacheV(S, 1, root);
    lc_Cursor C;
    assert(c);
    lc_seek(&C, c, 16);
    lc_checktree(c);
    lcD_foldnode(&C, 0);
    /* expected: off=? loff=?, cursor in botV1 with pathidx+=dn */
    lc_deltree(S, c);
    lc_close(S);
}

/* cov: remaining fold cursor adjust branches (lines 723, 750, 752).
 * Build multi-level tree via lc_scan, then splice crossing multiple leaves.
 * Phase 2 foldleaf/foldnode rebalancing triggers cursor adjustments. */
static void test_fold_cursor_scan(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    lc_rscanV(c, 25, 10);
    assert(lc_breaks(c) == 25);
    lc_checktree(c);
    lc_seek(&cur, c, 5);
    lc_splice(&cur, 85, 0);
    lc_checktree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* splice triggers lcD_foldleaf dl>0 with cursor in left leaf moved right */
static void test_splice_foldleaf_curright(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1, innerV(botV(leafV(2, 2, 2, 2), leafV(2)), botV(leafV(2))));
    lc_Cursor C;
    assert(c);
    lc_seek(&C, c, 5);
    lc_splice(&C, 2, 0);
    lc_asserttree(c, 1, innerV(botV(leafV(2, 2, 2), leafV(2)), botV(leafV(2))));
    lc_checktree_allow_empty(c, 1);
    lc_checkcursor(&C, 5);
    lc_deltree(S, c);
    lc_close(S);
}

/* ================================================================ */
/*  insert coverage: multi-leaf, shiftup, stitch, fixcursor         */
/* ================================================================ */

/* insert where right side spans multiple leaves → lcB_cutleaf cnt>0 */
static void test_insert_multi_sib(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 0, botV(leafV(1, 0), leafV(2, 0), leafV(3, 0), NULL));
    lc_Cursor C;
    unsigned  brs[] = {4, 0}, *p = brs;
    lc_seek(&C, c, 0);
    assert(lc_insert(&C, 0, lc_scanner, &p) == LC_OK);
    lc_checktree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* insert triggers stitch→shiftup (parent full during stitch) */
static void test_insert_stitch_shiftup(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(1), leafV(1), leafV(1), leafV(1)));
    lc_Cursor C;
    unsigned  brs[] = {0}, *p = brs;
    lc_seek(&C, c, 1);
    assert(lc_insert(&C, 0, lc_scanner, &p) == LC_OK);
    lc_asserttree(c, 0, botV(leafV(1), leafV(1, 1), leafV(1)));
    lc_checkcursor(&C, 1);
    lc_checktree_allow_empty(c, 1);
    lc_deltree(S, c);
    lc_close(S);
}

/* insert OOM in lcB_fillrt → lcB_oom path */
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
    lc_checktree(c);

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

/* insert triggers stitch→rootpush → fixcursor diff>0 */
static void test_insert_rootpush(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(1), leafV(1), leafV(1), leafV(1)));
    lc_Cursor C;
    unsigned  brs[] = {2, 2, 2, 2, 2, 0}, *p = brs;
    lc_seek(&C, c, 1);
    assert(lc_insert(&C, 0, lc_scanner, &p) == LC_OK);
    lc_checktree_allow_empty(c, 1);
    lc_checkcursor(&C, 11);
    lc_deltree(S, c);
    lc_close(S);
}

/* insert into 2-level tree: root→bottom→leaf, trigger fillrt in append */
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
    lc_checktree(c);

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

/* OOM after rootpush: deroot loop (L1247-1251) */
static void test_insert_oom_deroot(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor C;
    int       oom = 0;

    assert(S);
    c = cacheV(
            S, 0,
            botV(leafV(1, 0), leafV(1, 0), leafV(1, 0), leafV(1, 0), NULL));
    lc_checktree(c);
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

/* deep chain: mid full, root not → fillrt fl=0 dl=2 → chain loop enters */
static void test_insert_deep_chain(void) {
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
    lc_checktree(c);

    lc_seek(&C, c, 1);
    assert(lc_insert(&C, 0, lc_scanner, &p) == LC_OK);
    lc_checktree_allow_empty(c, 1);
    lc_deltree(S, c);
    lc_close(S);
}

/*
 * fillrt findlevel bug: root has 4 children (full), insert through idx=1
 * (not the last child).  After demotion fills inner2 to 4 children,
 * stitch calls fillrt.  The old findlevel checks child_count < FANOUT,
 * sees root is full, and rootpushes -- when moving right siblings
 * (inner3, inner4) would suffice, preserving the original structural order.
 */
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
    lc_checktree(c);
    lc_seek(&cur, c, 6);
    r = lc_insert(&cur, 0, lc_scanner, &pz);
    assert(r == LC_OK);
    lc_checktree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* insert brute: full tree (>=3 layers), insert 10 breaks at all positions */
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
    lc_checktree(c);
    lc_deltree(S, c);
    assert(S->leaves.live_obj == 0 && S->nodes.live_obj == 0);

    for (pos = 0; pos <= 128; ++pos) {
        c = lc_newtree(S);
        lc_rscanV(c, 128, 1);
        lc_seek(&C, c, pos);
        pbrs = brs;
        assert(lc_insert(&C, 0, lc_scanner, &pbrs) == LC_OK);
        lc_checktree(c);
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
        lc_checkcursor(&C, pos + 20);
        lc_deltree(S, c);
        assert(S->leaves.live_obj == 0 && S->nodes.live_obj == 0);
    }

    lc_close(S);
}

#define TESTS(X)                   \
    X(lifecycle)                   \
    X(scan_seek)                   \
    X(scan_no_input)               \
    X(scan_one_leaf)               \
    X(scan_bulk_many)              \
    X(scan_append_many)            \
    X(scan_oom_items)              \
    X(scan_oom_flush)              \
    X(scan_oom_build)              \
    X(seekline)                    \
    X(advance_single)              \
    X(advance_cross)               \
    X(node_split)                  \
    X(seek_past_leaf)              \
    X(advance_backward_cross)      \
    X(advline_cross)               \
    X(advline_backward_within)     \
    X(backwardline_cross)          \
    X(forwardline_crossnode)       \
    X(advance_skip_siblings)       \
    X(node_split_cursor_right)     \
    X(splitleaf_left)              \
    X(rootsplit_left)              \
    X(findlines_skip)              \
    X(rootsplit_left_deep)         \
    X(nodesplit_left)              \
    X(findlines_skip_deep)         \
    X(backwardoff_skip)            \
    X(split_cascade)               \
    X(clearbreaks)                 \
    X(clearbreaks_edge)            \
    X(clearbreaks_slot)            \
    X(splice)                      \
    X(splice_tmp)                  \
    X(splice_l2)                   \
    X(splice_trailing)             \
    X(splice_cross_breaks)         \
    X(splice_cross_breaks_slot)    \
    X(splice_cross_breaks_mid)     \
    X(splice_uf_last)              \
    X(splice_mergeleaf_sr)         \
    X(splice_mergenode_fold)       \
    X(splice_mergenode_last)       \
    X(splice_removed2)             \
    X(empty_tree_reset)            \
    X(splice_trimleaf_locend)      \
    X(splice_foldleaf_curright)    \
    X(balancenode_dneg)            \
    X(balancenode_dpos)            \
    X(foldleaf_switch)             \
    X(foldleaf_switch_rl)          \
    X(foldleaf_switch_lr)          \
    X(foldnode_cursor_left)        \
    X(foldnode_switch_rl)          \
    X(foldnode_switch_lr)          \
    X(foldnode_fallback_r)         \
    X(rebalance_merge)             \
    X(fold_cursor_scan)            \
    X(boundary_cmp)                \
    X(markbreak)                   \
    X(markbreak_split)             \
    X(markbreak_empty)             \
    X(markbreak_noop)              \
    X(markbreak_crossline)         \
    X(markbreak_crossline_end)     \
    X(markbreak_trailing)          \
    X(markbreak_brzero)            \
    X(markbreak_node_split)        \
    X(markbreak_root_split)        \
    X(markbreak_split_right)       \
    X(markbreak_root_split_on_add) \
    X(markbreak_crossleaf)         \
    X(markbreak_splitchild_right)  \
    X(insert_single_leaf)          \
    X(insert_col_mid)              \
    X(insert_no_scanner)           \
    X(insert_trailing)             \
    X(insert_leaf_split)           \
    X(insert_noop)                 \
    X(insert_cursor_pos)           \
    X(insert_many)                 \
    X(insert_empty)                \
    X(insert_empty_noop)           \
    X(insert_oom_trailing)         \
    X(insert_oom_normal)           \
    X(insert_oom_col0)             \
    X(insert_param_null)           \
    X(insert_multi_sib)            \
    X(insert_stitch_shiftup)       \
    X(insert_oom_shiftup)          \
    X(insert_rootpush)             \
    X(insert_oom_rootpush)         \
    X(insert_oom_deroot)           \
    X(insert_deep_chain)           \
    X(insert_fillrt_findlevel)     \
    X(insert_brute)

#define X(name) {#name, test_##name},
LC_TEST_MAIN("linecache tests")
#undef X
