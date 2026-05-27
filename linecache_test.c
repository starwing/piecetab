#define LC_LEAF_FANOUT 4
#define LC_FANOUT      4
#define LC_PAGE_SIZE   512
#define LC_STATIC_API

#include "linecache.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tree invariant checker */

static size_t check_node(const lc_Node *n, int l, int levels, int allow_empty) {
    int    i;
    size_t bsum = 0, lsum = 0;
    assert(allow_empty || n->child_count > 0);
    assert(n->child_count <= LC_FANOUT);
    for (i = 0; i < (int)n->child_count; ++i) {
        if (l == levels || levels == 0) {
            assert(n->breaks[i] <= LC_LEAF_FANOUT);
            bsum += n->bytes[i], lsum += n->breaks[i];
        } else {
            size_t cb = check_node(n->children[i], l + 1, levels, allow_empty);
            assert(cb == n->bytes[i]);
            bsum += cb, lsum += n->breaks[i];
        }
    }
    return bsum;
}

static void check_tree_allow_empty(const lc_Cache *c, int allow_empty) {
    if (c->root.child_count == 0) {
        assert(c->bytes == 0 && c->breaks == 0);
        return;
    }
    check_node(&c->root, 0, c->levels, allow_empty);
    /* verify c->bytes matches sum of root children bytes */
    assert(c->bytes == lcN_sumbytes(&c->root, 0, c->root.child_count));
    /* verify c->breaks matches sum of root children breaks */
    assert(c->breaks == lcN_sumbreaks(&c->root, 0, c->root.child_count));
}

static void check_tree(const lc_Cache *c) { check_tree_allow_empty(c, 0); }

static void dump_node(const lc_Node *n, int l, int levels) {
    unsigned i, cc = n->child_count;
    fprintf(stderr, "%*sL%u=%p cc=%u", l * 2, "", l, (void *)n, cc);
    for (i = 0; i < cc; ++i)
        fprintf(stderr, " b[%u]=%zu l[%u]=%zu", i, n->bytes[i], i,
                n->breaks[i]);
    fprintf(stderr, "\n");
    if (l == levels || levels == 0) {
        /* children are leaves */
        for (i = 0; i < cc; ++i) {
            lc_Leaf *leaf = (lc_Leaf *)n->children[i];
            unsigned s, sc = (unsigned)n->breaks[i];
            fprintf(stderr, "%*sL%u leaf[%u]=%p segs=%u bytes:", (l + 1) * 2,
                    "", l + 1, i, (void *)leaf, sc);
            for (s = 0; s < sc; ++s) fprintf(stderr, " %u", leaf->bytes[s]);
            fprintf(stderr, "\n");
        }
    } else {
        /* children are internal nodes */
        for (i = 0; i < cc; ++i) dump_node(n->children[i], l + 1, levels);
    }
}

static void dump_tree(const lc_Cache *c, const char *tag) {
    fprintf(stderr,
            "=== dump_tree %s: levels=%u root.cc=%u bytes=%zu breaks=%zu ===\n",
            tag, c->levels, c->root.child_count, c->bytes, c->breaks);
    dump_node(&c->root, 0, c->levels);
}

static int compare_node(
        const lc_Node *a, const lc_Node *b, unsigned l, unsigned levels) {
    unsigned i;
    if (a->child_count != b->child_count) return 0;
    for (i = 0; i < a->child_count; ++i) {
        if (a->bytes[i] != b->bytes[i]) return 0;
        if (a->breaks[i] != b->breaks[i]) return 0;
        if (l == levels || levels == 0) {
            const lc_Leaf *la = (const lc_Leaf *)a->children[i];
            const lc_Leaf *lb = (const lc_Leaf *)b->children[i];
            unsigned       j, sc = (unsigned)a->breaks[i];
            for (j = 0; j < sc; ++j)
                if (la->bytes[j] != lb->bytes[j]) return 0;
        } else if (!compare_node(a->children[i], b->children[i], l + 1, levels))
            return 0;
    }
    return 1;
}

static int compare_tree(const lc_Cache *a, const lc_Cache *b) {
    if (a->levels != b->levels) return 0;
    if (a->bytes != b->bytes) return 0;
    if (a->breaks != b->breaks) return 0;
    if (a->root.child_count != b->root.child_count) return 0;
    if (a->root.child_count == 0) return 1;
    return compare_node(&a->root, &b->root, 0, a->levels);
}

#define assert_tree(c, lvls, ...)                                      \
    do {                                                               \
        lc_Cache *__d = cacheV(S, lvls, __VA_ARGS__);                  \
        if (!compare_tree((c), __d)) {                                 \
            fprintf(stderr, "assert_tree FAILED at %s:%d\n", __FILE__, \
                    __LINE__);                                         \
            fprintf(stderr, "Expected:\n");                            \
            dump_tree(__d, "expected");                                \
            fprintf(stderr, "Actual:\n");                              \
            dump_tree((c), "actual");                                  \
            assert(0 && "assert_tree failed");                         \
        }                                                              \
        lc_deltree(S, __d);                                            \
    } while (0)

/* test allocator */

static void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nsize);
}

/* OOM allocator — fails on the Nth allocation (nsize > 0) */
static int   oom_cnt;
static void *oom_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud, (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    if (--oom_cnt == 0) return NULL;
    return realloc(ptr, nsize);
}

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

static unsigned scanner(void *ud, size_t prev) {
    unsigned **brs = (unsigned **)ud;
    (void)prev;
    return brs ? *(*brs)++ : 0;
}

static void test_scan_seek(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;

    int r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_bytes(c) == 40);

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
    {
        unsigned brs2[3] = {5, 10, 0}, *pbrs2 = brs2;
        r = lc_scan(c, scanner, &pbrs2);
        assert(r == LC_OK && lc_breaks(c) == 5 && lc_bytes(c) == 55);
    }

    lc_deltree(S, c);
    lc_close(S);
}

/* T3: seekline */

static void test_seekline(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    lc_scan(c, scanner, &pbrs);

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
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    lc_scan(c, scanner, &pbrs);

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
    assert(r == LC_OK && lc_offset(&cur) == 40);

    /* clamp before start */
    r = lc_advance(&cur, -100);
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
    unsigned brs[11] = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 0}, *pbrs = brs;
    int      r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 10);
    check_tree(c);

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
    unsigned  brs[3] = {10, 20, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 2);

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
    unsigned  brs[6] = {10, 10, 10, 10, 10, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 5);
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
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 3);

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
    unsigned  brs[121];
    int       i, r;

    for (i = 0; i < 120; ++i) brs[i] = 10;
    brs[120] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK && lc_breaks(c) == 120);
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
    unsigned  brs[5] = {10, 15, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 4);

    /* clear breaks in middle of gap (no breaks to remove) */
    lc_seek(&cur, c, 2);
    r = lc_clearbreaks(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 4); /* no breaks in range */
    check_tree(c);

    /* clear 1 break at break boundary */
    lc_seek(&cur, c, 9);
    r = lc_clearbreaks(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 3);
    check_tree(c); /* [25, 40, 55] */

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
    unsigned  brs[5] = {10, 15, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 55);

    /* len == 0: no-op */
    lc_seek(&cur, c, 5);
    r = lc_clearbreaks(&cur, 0);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_linelen(&cur) == 10);
    check_tree(c);

    /* clear exactly one break: seek to break boundary */
    lc_seek(&cur, c, 9);
    r = lc_clearbreaks(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_linelen(&cur) == 25);
    check_tree(c);

    /* past end of tree: del clamped to remaining (5), ins=20,
       tree had 3 breaks after previous case; no break crossed here */
    r = lc_seek(&cur, c, 50);
    assert(r == LC_OK && lc_linelen(&cur) == 15);
    r = lc_clearbreaks(&cur, 20);
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_linelen(&cur) == 30);
    assert(lc_bytes(c) == 40);
    check_tree(c);

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
    unsigned  brs[18] = {10, 10, 10, 10, 10, 10, 10, 10, 10,
                         10, 10, 10, 10, 10, 10, 10, 10, 0},
              *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 17 && lc_bytes(c) == 170);
    fprintf(stderr, "splice_tmp after 1st scan: levels=%d cc=%d\n", c->levels,
            c->root.child_count);
    check_tree(c);
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 170, 0); /* delete all from 0 */
    fprintf(stderr, "splice_tmp after delete all: levels=%d cc=%d bytes=%zu\n",
            c->levels, c->root.child_count, c->bytes);
    check_tree(c);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);

    /* 重建，从 offset 25 删 140 bytes */
    pbrs = brs;
    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 17 && lc_bytes(c) == 170);
    fprintf(stderr,
            "splice_tmp before splice: levels=%d cc=%d bytes=%zu breaks=%zu\n",
            c->levels, c->root.child_count, c->bytes, c->breaks);
    lc_seek(&cur, c, 25);
    lc_splice(&cur, 140, 0);
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_splice_l2(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[66];
    int       i, r;

    for (i = 0; i < 65; ++i) brs[i] = 10;
    brs[65] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK && lc_breaks(c) == 65 && lc_bytes(c) == 650);

    /* delete all */
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 650, 0);
    check_tree(c);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);

    /* rebuild, partial delete from offset 25 */
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK && lc_breaks(c) == 65 && lc_bytes(c) == 650);
    lc_seek(&cur, c, 25);
    lc_splice(&cur, 600, 0); /* delete 600, keep 25+25=50 */
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* T13: splice */

static void test_splice(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[101];
    int       i, r;

    for (i = 0; i < 100; ++i) brs[i] = 10;
    brs[100] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK && lc_breaks(c) == 100 && lc_bytes(c) == 1000);

    lc_seek(&cur, c, 0);
    lc_splice(&cur, 1000, 0); /* delete all */
    assert(r == LC_OK && lc_breaks(c) == 0 && lc_bytes(c) == 0);
    check_tree(c);

    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK && lc_breaks(c) == 100 && lc_bytes(c) == 1000);
    lc_seek(&cur, c, 11);
    lc_splice(&cur, 980, 0); /* delete all but first 11 + last 9 */
    fprintf(stderr, "splice result: breaks=%zu bytes=%zu\n", lc_breaks(c),
            lc_bytes(c));
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_bytes(c) == 20);
    check_tree(c);

    {
        unsigned brs2[3] = {5, 15, 0}, *pbrs2 = brs2;
        r = lc_scan(c, scanner, &pbrs2);
    }
    assert(r == LC_OK);

    /* simple splice (no break crossing) */
    lc_seek(&cur, c, 2);
    lc_splice(&cur, 5, 3);
    assert(lc_bytes(c) == 38 && lc_offset(&cur) == 5);

    /* splice crossing breaks */
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 15, 8);
    assert(lc_bytes(c) == 31); /* 38 - 15 + 8 */
    check_tree(c);

    /* splice with del=0, ins=0 (no-op) */
    lc_splice(&cur, 0, 0);
    check_tree(c);

    /* null check */
    lc_splice(NULL, 1, 1);
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* T14: splice trailing area */

static void test_splice_trailing(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK);
    lc_scan(c, scanner, NULL);

    /* after last break (trailing area): slot=3 (==breaks) */
    lc_seek(&cur, c, 40);
    lc_splice(&cur, 0, 20); /* insert 20 bytes at end */
    check_tree(c);
    printf("bytes after splice: %zu\n", lc_bytes(c));
    assert(lc_bytes(c) == 40); /* 40 is the last newline */
    printf("offset before splice: 40, after splice: %zu\n", lc_offset(&cur));
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
    unsigned  brs[9] = {10, 10, 10, 10, 10, 10, 10, 10, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 8);

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
    unsigned  brs[9] = {10, 10, 10, 10, 10, 10, 10, 10, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 8);

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
    unsigned  brs[9] = {10, 10, 10, 10, 10, 10, 10, 10, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 8);

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
static void test_cov_forwardline_crossnode(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[34] = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
                         10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
                         10, 10, 10, 10, 10, 10, 10, 10, 10, 0},
              *p = brs;
    int       r;

    r = lc_scan(c, scanner, &p);
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
    unsigned  brs[18] = {10, 10, 10, 10, 10, 10, 10, 10, 10,
                         10, 10, 10, 10, 10, 10, 10, 10, 0},
              *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 17);

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
    unsigned  brs[71];
    int       i, r;

    for (i = 0; i < 70; ++i) brs[i] = 10;
    brs[70] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK && lc_breaks(c) == 70);

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
    unsigned  brs[5] = {10, 10, 10, 10, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 4);

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
    unsigned  brs[5] = {10, 15, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 4);

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
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 3);

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
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 3);

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
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 3);

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
    unsigned  brs[13] = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 0},
              *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 12);

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
    unsigned  brs[22] = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
                         10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 0},
              *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK);

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
    unsigned  brs[151];
    int       i, r;

    for (i = 0; i < 150; ++i) brs[i] = 5;
    brs[150] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK);

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
    unsigned  brs[5] = {10, 10, 10, 10, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 4);

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
    unsigned  brs[18] = {10, 10, 10, 10, 10, 10, 10, 10, 10,
                         10, 10, 10, 10, 10, 10, 10, 10, 0},
              *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 17);

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
    unsigned  brs[7] = {10, 10, 10, 10, 10, 10, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 6);

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
    unsigned  brs[101];
    int       i, r;

    for (i = 0; i < 100; ++i) brs[i] = 5;
    brs[100] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK);

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
    unsigned  brs[23] = {10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
                         10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 0},
              *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 22);

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
    unsigned  brs[81];
    int       i, r;

    for (i = 0; i < 80; ++i) brs[i] = 5;
    brs[80] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK);

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
    unsigned  brs[81];
    int       i, r;

    for (i = 0; i < 80; ++i) brs[i] = 5;
    brs[80] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK);

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
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 3);

    /* seek just after first break (offset 11), clear through second break;
       cursor slot ends up past cleared range */
    lc_seek(&cur, c, 11);
    r = lc_clearbreaks(&cur, 16);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}

/* T36: comprehensive deep tree operations to cover remaining split paths */

static void test_cov_remaining(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[201];
    int       i, k, r;

    for (i = 0; i < 200; ++i) brs[i] = 5;
    brs[200] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK && lc_breaks(c) == 200);

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
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    size_t    orig_bytes, orig_breaks;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 3);
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
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 3);
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
    unsigned  brs[5] = {1, 99, 100, 100, 0}, *pbrs = brs;
    int       r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 4);
    check_tree(c);

    /* line 1 starts at offset 1, len=99 (=100-1). split at br=10 */
    lc_seekline(&cur, c, 1);
    assert(lc_offset(&cur) == 1 && lc_line(&cur) == 1
           && lc_linelen(&cur) == 99);
    r = lc_markbreak(&cur, 10);
    assert(r == LC_OK);
    check_tree(c);
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
    unsigned  brs[4] = {10, 15, 15, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_bytes(c) == 40);
    check_tree(c);

    /* line 1 (offset 10, len=15): split at br=5 within line */
    lc_seekline(&cur, c, 1);
    assert(lc_offset(&cur) == 10);
    r = lc_markbreak(&cur, 5);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_bytes(c) == 40 && lc_breaks(c) == 4);

    lc_deltree(S, c);
    lc_close(S);
}

/* T41: markbreaks — set consecutive line lengths within existing bounds */

static void test_markbreaks(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs_mb[4] = {10, 20, 30, 0}, *pbrs_mb = brs_mb;
    unsigned  brs_scan[5] = {10, 90, 100, 100, 0}, *pbrs_scan = brs_scan;
    int       r = lc_scan(c, scanner, &pbrs_scan);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 300);
    check_tree(c);

    /* split lines 1,2,3 at their midpoints */
    lc_seekline(&cur, c, 1);
    r = lc_markbreaks(&cur, scanner, &pbrs_mb);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_bytes(c) == 300 && lc_breaks(c) == 7);

    lc_deltree(S, c);
    lc_close(S);
}

/* T42: markbreak cross-leaf: extend line across leaf boundary */
static void test_markbreak_crossleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[9] = {1, 1, 1, 1, 1, 1, 1, 1, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 8 && lc_bytes(c) == 8);
    check_tree(c);

    lc_seek(&cur, c, 0);
    r = lc_markbreak(&cur, 100);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_bytes(c) == 100 && lc_breaks(c) == 1);

    lc_deltree(S, c);
    lc_close(S);
}

/* ================================================================ */
/*  Tree construction helpers (leafV / botV / innerV / cacheV)         */
/* ================================================================ */

#define leafV(...)  leafV_(S, __VA_ARGS__, 0)
#define botV(...)   botV_(S, __VA_ARGS__, NULL)
#define innerV(...) innerV_(S, __VA_ARGS__, NULL)

/* leafV(S, seg1, seg2, ..., 0) — create leaf from 0-term segment list. */
static lc_Node *leafV_(lc_State *S, ...) {
    va_list  ap;
    lc_Leaf *l;
    unsigned n = 0, i;
    va_start(ap, S);
    while (va_arg(ap, unsigned) != 0) n++;
    va_end(ap);
    l = (lc_Leaf *)lc_poolalloc(S, &S->leaves);
    assert(l && n <= LC_LEAF_FANOUT);
    va_start(ap, S);
    for (i = 0; i < n; i++) l->bytes[i] = va_arg(ap, unsigned);
    if (n < LC_LEAF_FANOUT) l->bytes[n] = 0;
    va_end(ap);
    return (lc_Node *)l;
}

/* botV(S, child1, child2, ..., NULL) — bottom internal node. Children are
 * leaves; computes bytes[]/breaks[] from leaf segment arrays. */
static lc_Node *botV_(lc_State *S, ...) {
    va_list  ap;
    lc_Node *n;
    unsigned cc = 0, i;
    va_start(ap, S);
    while (va_arg(ap, lc_Node *) != NULL) cc++;
    va_end(ap);
    n = (lc_Node *)lc_poolalloc(S, &S->nodes);
    assert(n && cc <= LC_FANOUT);
    n->child_count = (unsigned short)cc;
    va_start(ap, S);
    for (i = 0; i < cc; i++) {
        lc_Leaf *lf = (lc_Leaf *)va_arg(ap, lc_Node *);
        unsigned j, sc = 0;
        size_t   b = 0;
        n->children[i] = (lc_Node *)lf;
        for (j = 0; j < LC_LEAF_FANOUT && lf->bytes[j] != 0; j++)
            b += lf->bytes[j], sc++;
        n->bytes[i] = b, n->breaks[i] = sc;
    }
    va_end(ap);
    return n;
}

/* innerV(S, child1, child2, ..., NULL) — inner internal node. Children are
 * internal nodes; computes bytes[]/breaks[] by summing child node totals. */
static lc_Node *innerV_(lc_State *S, ...) {
    va_list  ap;
    lc_Node *n;
    unsigned cc = 0, i;
    va_start(ap, S);
    while (va_arg(ap, lc_Node *) != NULL) cc++;
    va_end(ap);
    n = (lc_Node *)lc_poolalloc(S, &S->nodes);
    assert(n && cc <= LC_FANOUT);
    n->child_count = (unsigned short)cc;
    va_start(ap, S);
    for (i = 0; i < cc; i++) {
        lc_Node *ch = va_arg(ap, lc_Node *);
        n->children[i] = ch;
        n->bytes[i] = lcN_sumbytes(ch, 0, (int)ch->child_count);
        n->breaks[i] = lcN_sumbreaks(ch, 0, (int)ch->child_count);
    }
    va_end(ap);
    return n;
}

/* cacheV(S, root, levels) — wrap pre-built root node into a cache. */
static lc_Cache *cacheV(lc_State *S, unsigned levels, lc_Node *root) {
    lc_Cache *c = lc_newtree(S);
    unsigned  i;
    assert(c != NULL && root->child_count <= LC_FANOUT);
    c->levels = levels;
    c->root = *root;
    lc_poolfree(&S->nodes, root); /* root copy moved to cache, free template */
    c->bytes = 0;
    c->breaks = 0;
    for (i = 0; i < c->root.child_count; i++)
        c->bytes += c->root.bytes[i], c->breaks += c->root.breaks[i];
    check_tree_allow_empty(c, 1);
    return c;
}

/* coverage: backwardline outer for iteration (L488).
 * lc_scan creates full leaves (4 breaks each). With a half-tree where the
 * deepest-level parent has only 1 child, the inner for loop is empty and
 * the outer for continues to the next level. */
static void test_cov_backwardline_cross(void) {
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
/*  Phase 1: single-leaf spliceleaf tests (Examples 1-3 from plan)  */
/* ================================================================ */

/* Ex1a: 叶内不跨段 — [10,10], seek(5), del=3 → [7,10], bytes=17 */
static void test_spliceleaf_1a(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10)));
    assert(c);

    lc_seek(&cur, c, 5);
    assert(lc_offset(&cur) == 5);
    lcD_spliceleaf(&cur, 3);
    assert_tree(c, 0, botV(leafV(7, 10)));
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* Ex1b: 叶内跨段 — [10,10], seek(5), del=8 → [12], bytes=12 */
static void test_spliceleaf_1b(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10)));
    assert(c);

    lc_seek(&cur, c, 5);
    lcD_spliceleaf(&cur, 8);
    assert_tree(c, 0, botV(leafV(12)));
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* Ex1c: 整段消失 — [5,10,5], seek(3), del=12 → [8], bytes=8 */
static void test_spliceleaf_1c(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(5, 10, 5)));
    assert(c);

    lc_seek(&cur, c, 3);
    lcD_spliceleaf(&cur, 12);
    assert_tree(c, 0, botV(leafV(8)));
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* spliceleaf_foldright: underfilled leaf after delete → fold right → prune →
 * rebalance shrink. levels=1 innerV(botV(leafV(10), leafV(10,10))), C at
 * offset 0, delete 10 bytes empties leaf0 → right fold → botV becomes root. */
static void test_spliceleaf_foldright(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 1, innerV(botV(leafV(10), leafV(10, 10))));
    assert(c);

    lc_seek(&cur, c, 0);
    lcD_spliceleaf(&cur, 10);
    assert_tree(c, 0, botV(leafV(10, 10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* spliceleaf_foldleft: underfilled leaf after delete → fold left → prune →
 * rebalance shrink. levels=1 innerV(botV(leafV(10), leafV(10,10))), C at
 * offset 10 (start of leaf1), delete 10 bytes → leaf1 breaks=1<2 → left fold
 * into leaf0 → leafV(10,10). */
static void test_spliceleaf_foldleft(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 1, innerV(botV(leafV(10), leafV(10, 10))));
    assert(c);

    lc_seek(&cur, c, 10);
    lcD_spliceleaf(&cur, 10);
    assert_tree(c, 0, botV(leafV(10, 10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* prune: levels=1, C1 in L1, C2 in L4 → delete L2 L3, shrink to [L1,L4] */
static void test_prune_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Node  *p;
    lc_Cache *c = cacheV(
            S, 1, innerV(botV(leafV(10), leafV(10), leafV(10), leafV(10))));
    assert(c);

    lc_seek(&C, c, 3);
    p = lcK_parent(&C, 1);
    assert(p->child_count == 4);

    lcD_prune(&C, 3, 1);
    assert_tree(c, 1, innerV(botV(leafV(10), leafV(10))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* prune: levels=2, C1 in innerV0.botV0, C2 in innerV1.botV0 → delete middle */
static void test_prune_node(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(10)), botV(leafV(10))),
                   innerV(botV(leafV(10))), innerV(botV(leafV(10)))));
    assert(c);

    lc_seek(&C, c, 3);
    lcD_prune(&C, 2, 0);
    assert_tree(
            c, 2,
            innerV(innerV(botV(leafV(10)), botV(leafV(10))),
                   innerV(botV(leafV(10)))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* ================================================================ */
/*  Phase 2: rebalance unit tests                                   */
/* ================================================================ */

/* rebalance_shrink: levels=1 root with 1 botV → shrink to levels=0 */
static void test_rebalance_shrink(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 1, innerV(botV(leafV(10), leafV(20))));
    assert(c);

    lc_seek(&cur, c, 5);
    lcD_rebalance(&cur, 0);
    assert_tree(c, 0, botV(leafV(10), leafV(20)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_rebalance_double(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 2, innerV(innerV(botV(leafV(10)))));
    assert(c);

    lc_seek(&cur, c, 5);
    lcD_rebalance(&cur, 0);
    assert_tree(c, 0, botV(leafV(10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* rebalance_node_merge_right: botV0 underfilled (cc=1<2), folds right into
 * botV1 (1+2=3≤4). innerV0 cc→1 cascades to root fold → root shrink.
 * Tree: innerV0 has 2 botV children (botV needs siblings for assert). */
static void test_rebalance_node_merge_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(10)), botV(leafV(10, 10))),
                   innerV(botV(leafV(10, 10)))));
    assert(c);

    lc_seek(&cur, c, 0);
    lcD_rebalance(&cur, 1);
    assert_tree(
            c, 2,
            innerV(innerV(botV(leafV(10), leafV(10, 10))),
                   innerV(botV(leafV(10, 10)))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* rebalance_node_merge_left: botV1 underfilled (cc=1<2), no right sibling →
 * folds left into botV0 (2+1=3≤4). innerV1 cc→1 cascades to root left fold. */
static void test_rebalance_node_merge_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(10, 10))),
                   innerV(botV(leafV(10, 10)), botV(leafV(10)))));
    assert(c);

    lc_seek(&cur, c, 45);
    lcD_rebalance(&cur, 1);
    assert_tree(
            c, 2,
            innerV(innerV(botV(leafV(10, 10))),
                   innerV(botV(leafV(10, 10), leafV(10)))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* rebalance_node_merge_right_large: botV0 underfilled (cc=1<2), absorbs
 * botV1 (cc=1, 4 segs in one leaf). foldnode: 1+1=2≤4 → succeeds.
 * innerV0 cc→1. 根层不动，levels 保持 2。 */
static void test_rebalance_node_merge_right_large(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(10)), botV(leafV(10, 10, 10, 10))),
                   innerV(botV(leafV(10)), botV(leafV(10)), botV(leafV(10)),
                          botV(leafV(10)))));
    assert(c);

    lc_seek(&cur, c, 0);
    lcD_rebalance(&cur, 1);
    assert_tree(
            c, 2,
            innerV(innerV(botV(leafV(10), leafV(10, 10, 10, 10))),
                   innerV(botV(leafV(10)), botV(leafV(10)), botV(leafV(10)),
                          botV(leafV(10)))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* rebalance_cascade: spliceleaf 删 botV0 叶0(一段)→botV0 breaks 1<2→foldleaf
 * 右折→botV0 cc=1→rebalance(1) foldnode 右折 botV→innerV0 cc=1。
 * levels=2 树，innerV0 有 2 botV 子。rebalance 不处理 l=0，故根层不动。 */
static void test_rebalance_cascade(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(10), leafV(10)), botV(leafV(10, 10))),
                   innerV(botV(leafV(10, 10)))));
    assert(c);

    lc_seek(&cur, c, 0);
    lcD_spliceleaf(&cur, 10);
    assert_tree(
            c, 2,
            innerV(innerV(botV(leafV(10), leafV(10, 10))),
                   innerV(botV(leafV(10, 10)))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* ================================================================ */
/*  Phase 4: trimleaf / trimnode tests                              */
/* ================================================================ */

/* trimleaf_left: leaf[10,10,10,10], C at lidx=1,col=3 → keep [0..0],del rest
 */
static void test_trimleaf_trimright(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10, 10, 10)));
    assert(c);

    lc_seek(&cur, c, 13);
    assert(cur.lidx == 1);
    lcD_trimleaf(&cur, 0);
    assert_tree(c, 0, botV(leafV(10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* trimleaf_right: leaf[10,10,10,10], C at lidx=2,col=2 → del left [0..1]+2
 * bytes. 活段在索引2,3，故不使 assert_tree 较叶级数据 */
static void test_trimleaf_trimleft(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Node  *p;
    lc_Leaf  *leaf;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10, 10, 10)));
    assert(c);

    lc_seek(&cur, c, 22);
    assert(cur.lidx == 2);
    lcD_trimleaf(&cur, 1);
    assert(c->bytes == 18);
    assert(c->breaks == 2);
    p = &cur.tree->root;
    leaf = (lc_Leaf *)p->children[0];
    assert(p->bytes[0] == (size_t)18);
    assert(p->breaks[0] == (size_t)2);
    assert(leaf->bytes[2] == 8u);
    assert(leaf->bytes[3] == 10u);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* trimleaf_mergeleaf: trimleaf(C1,1)+trimleaf(C2,0)+mergeleaf → combine
 * botV(leafV(10,10), leafV(10), leafV(10)), C1=10(lidx=1,col=0),
 * C2=20(lidx=0,col=0) trims leaves then merges — 三叶合为二叶，树形正则可
 * assert_tree */
static void test_trimleaf_mergeleaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10), leafV(10), leafV(10)));
    assert(c);

    lc_seek(&C1, c, 10); /* leaf0 seg1 start: lidx=1 col=0 */
    lc_seek(&C2, c, 20); /* leaf1 start: lidx=0 col=0 */
    lcD_trimleaf(&C1, 0);
    lcD_trimleaf(&C2, 1);
    lcD_shiftleaf(&C1, &C2);
    assert(C1.paths[0] + 2 == C2.paths[0]);
    assert_tree(c, 0, botV(leafV(10, 10), leafV_(S, 0), leafV(10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* trimnode_left: levels=1 root[4 botV], C in botV1 → keep [0..1], del right */
static void test_trimnode_trimright(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10, 10)), botV(leafV(10, 10)),
                   botV(leafV(10, 10)), botV(leafV(10, 10))));

    lc_seek(&cur, c, 25);
    lcD_trimnode(&cur, 0, 0);
    assert_tree(c, 1, innerV(botV(leafV(10, 10)), botV(leafV(10, 10))));
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* trimnode_right: trim before cursor (left=0) → 死子留 NULL 于
 * children[0..i-1]。 child_count 不减（以备 mergenode 算 sr 偏移），不可用
 * assert_tree/check_tree。 */
static void test_trimnode_trimleft(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(11, 12)), botV(leafV(13, 14)),
                   botV(leafV(15, 16)), botV(leafV(17, 18))));

    lc_seek(&cur, c, 55);
    lcD_trimnode(&cur, 0, 1);
    assert(c->root.child_count == 2);
    assert(c->bytes == 15 + 16 + 17 + 18);
    assert(c->breaks == 4);
    assert(c->root.breaks[2] == 2);
    assert(c->root.bytes[2] == 15 + 16);
    assert(c->root.breaks[3] == 2);
    assert(c->root.bytes[3] == 17 + 18);
    assert(c->root.children[2]->child_count == 1);
    assert(c->root.children[3]->child_count == 1);

    lc_deltree(S, c);
    lc_close(S);
}

/* trimnode_mergenode: trimnode(C1,1)+trimnode(C2,0)+mergenode → combine
 * innerV(botV3, botV3) levels=1. C1=5(botV0 leaf0), C2=45(botV1 leaf1).
 * trimnode right then left creates dead leaves in sib; mergenode skips
 * dead with sr>0 then combines — two botVs merge into one. */
static void test_trimnode_mergenode(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    assert(c);

    lc_seek(&C1, c, 5);  /* botV0 leaf0: lidx=0 col=5 */
    lc_seek(&C2, c, 45); /* botV1 leaf1: lidx=0 col=5 */
    lcD_trimnode(&C1, 1, 0);
    lcD_trimnode(&C2, 1, 1);
    lcD_shiftnode(&C1, &C2, 0);
    assert_tree(
            c, 1, innerV(botV(leafV(10), leafV(10), leafV(10)), botV_(S, 0)));
    lc_deltree(S, c);
    lc_close(S);
}

/* mergeleaf_combine: 2 leaves 1 seg each → merge, return 1 */
static void test_mergeleaf_combine(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10), leafV(20)));
    assert(c);

    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 15);
    lcD_shiftleaf(&C1, &C2);
    assert_tree(c, 0, botV(leafV(10, 20), leafV_(S, 0)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergeleaf_sr_nonzero: C2.lidx=1, copies from r->bytes[1].
 * Exercises sr!=0 path; known: pr->bytes includes dead region before sr. */
static void test_mergeleaf_sr_nonzero(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10), leafV(10, 10, 10)));
    assert(c);

    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 25);
    lcD_shiftleaf(&C1, &C2);
    /* tree consistent — mergeleaf copied cr=2 segs from r->bytes[1],
     * leaf becomes [10,10,10]; pr->bytes includes dead seg bytes (known issue)
     */
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergeleaf_cross_subtree: C1 in botV0 C2 in botV1 → pl!=pr, exercise
 * cross-parent path. Each botV has ≥2 leaves so removing one leaf does not
 * empty the node. */
static void test_mergeleaf_cross_subtree(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10), leafV(10)), botV(leafV(20), leafV(10))));
    assert(c);

    assert(c->root.breaks[0] == 2);
    assert(c->root.breaks[1] == 2);
    lc_seek(&C1, c, 5);  /* botV0 leaf0: lidx=0 col=5 */
    lc_seek(&C2, c, 25); /* botV1 leaf0: lidx=0 col=5 (botV0=20 bytes) */
    lcD_shiftleaf(&C1, &C2);
    assert_tree(
            c, 1,
            innerV(botV(leafV(10, 20), leafV(10)),
                   botV(leafV_(S, 0), leafV(10))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergenode_combine: 2 botV 1 leaf each → merge, return 1 */
static void test_mergenode_combine(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(S, 1, innerV(botV(leafV(10)), botV(leafV(20))));
    assert(c);

    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 15);
    lcD_shiftnode(&C1, &C2, 0);
    assert_tree(c, 1, innerV(botV(leafV(10), leafV(20)), botV_(S, 0)));
    check_tree_allow_empty(c, 1);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergenode_sr_nonzero: trimnode creates dead child (sr=1) in sib botV,
 * mergenode combine skips it. Like mergeleaf_sr_nonzero but for nodes. */
static void test_mergenode_sr_nonzero(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10)), botV(leafV(10), leafV(10), leafV(10))));
    assert(c);

    lc_seek(&C1, c, 5);  /* botV0 leaf0: lidx=0 col=5 */
    lc_seek(&C2, c, 25); /* botV1 leaf1: lidx=1 col=5 */
    lcD_trimnode(&C2, 1, 0);
    lcD_shiftnode(&C1, &C2, 0);
    /* C1 botV0 leaf + C2 botV1 leaves[1..2] → 3 leaves total */
    assert_tree(
            c, 1, innerV(botV(leafV(10), leafV(10), leafV(10)), botV_(S, 0)));
    check_tree_allow_empty(c, 1);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergenode_cross_subtree: levels=2 tree, C1 in innerV0.botV0, C2 in
 * innerV1.botV0 → pl!=pr. Absorb innerV1.botV0 into innerV0.botV0. */
static void test_mergenode_cross_subtree(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(10)), botV(leafV(10), leafV(10))),
                   innerV(botV(leafV(20)), botV(leafV(10), leafV(10)))));
    assert(c);
    lc_seek(&C1, c, 5);  /* innerV0.botV0 leaf0: lidx=0 col=5 */
    lc_seek(&C2, c, 35); /* innerV1.botV0 leaf0: lidx=0 col=5 */
    lcD_shiftnode(&C1, &C2, 1);
    check_tree_allow_empty(c, 1);
    lc_deltree(S, c);
    lc_close(S);
}

/* ================================================================ */
/*  Phase 9: splicerange integration tests                          */
/* ================================================================ */

/* splicerange_2leaf: levels=0 2 leaves, C1 in leaf0 C2 in leaf1, col=0 */
static void test_splicerange_2leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10, 10), leafV(10, 10, 10)));
    check_tree(c);
    lc_seek(&C1, c, 20);
    lc_seek(&C2, c, 40);
    lcD_splicerange(&C1, &C2);
    assert_tree(c, 0, botV(leafV(10, 10, 10, 10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* splicerange_prune: levels=0 4 leaves, C1 in leaf0 C2 in leaf3, middle freed
 */
static void test_splicerange_prune(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(
            S, 0,
            botV(leafV(10, 10), leafV(10, 10), leafV(10, 10), leafV(10, 10)));
    check_tree(c);
    lc_seek(&C1, c, 10);
    lc_seek(&C2, c, 70);
    lcD_splicerange(&C1, &C2);
    assert_tree(c, 0, botV(leafV(10, 10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* splicerange_merge_rebalance: levels=2, root has 2 children. L and R
 * diverge at level 1 (l=1, within root's left child). After prune the
 * common parent (left innerV) is underfull (child_count==1 < FANOUT/2).
 * foldnode(L, l-1) at root level merges left with right sibling,
 * then rebalance shrinks the root. */
static void test_splicerange_merge_rebalance(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *left = innerV(
            botV(leafV(2, 0)), botV(leafV(2, 0)), botV(leafV(2, 0)));
    lc_Node  *right = innerV(botV(leafV(2, 0)));
    lc_Node  *root = innerV(left, right);
    lc_Cache *c = cacheV(S, 2, root);
    lc_Cursor C1, C2;
    assert(c);
    assert(lc_bytes(c) == 8 && lc_breaks(c) == 4);
    check_tree(c);

    /* L in left->botV[0], R in left->botV[2], l=1 */
    lc_seek(&C1, c, 0);
    lc_seek(&C2, c, 4);
    assert(C1.paths[0] == C2.paths[0]);
    assert(C1.paths[0] == &c->root.children[0]);
    assert(C1.paths[1] != C2.paths[1]);

    lcD_splicerange(&C1, &C2);
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* splicerange_foldnode_upper: levels=2, L and R in different root children
 * (l=0). After trim/shift, upper node has underfull child → foldnode at
 * level 1. (covers L835) */
static void test_splicerange_foldnode_upper(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *left = innerV(botV(leafV(2, 0)), botV(leafV(2, 0)));
    lc_Node  *mid = innerV(botV(leafV(2, 2, 0)), botV(leafV(2, 0)));
    lc_Node  *rgt = innerV(botV(leafV(2, 0)), botV(leafV(2, 0)));
    lc_Node  *root = innerV(left, mid, rgt);
    lc_Cache *c = cacheV(S, 2, root);
    lc_Cursor C1, C2;
    assert(c);
    check_tree(c);

    /* L in left innerV first botV, R in mid innerV first botV, l=0 */
    lc_seek(&C1, c, 0);
    lc_seek(&C2, c, 4);
    assert(C1.paths[0] != C2.paths[0]);

    lcD_splicerange(&C1, &C2);
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* trimleaf_right_end: splice deletes from mid-leaf to tree end. R goes to
 * locend (lidx >= count of last leaf). trimleaf(R,1) hits L614.
 * After trim, db = p->bytes[li], dl = count. */
static void test_trimleaf_right_end(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 0), leafV(10, 0), leafV(10, 0)));
    assert(c);
    check_tree(c);

    lc_seek(&cur, c, 5);
    lc_splice(&cur, 25, 0);
    check_tree(c);

    assert(c->root.child_count == 0);
    lc_deltree(S, c);
    lc_close(S);
}

/* splicerange_all: delete entire tree via lc_splice */
static void test_splicerange_all(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10), leafV(20)));
    assert(c);

    lc_seek(&cur, c, 0);
    lc_splice(&cur, 30, 0);
    assert(c->bytes == 0 && c->breaks == 0);
    assert(c->root.child_count == 0);
    check_tree(c);
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
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(2, 2, 2, 2), leafV(2, 2, 2)));
    check_tree(c);
    lc_seek(&cur, c, 9); /* leaf1 lidx=0 col=1 */
    lc_splice(&cur, 4, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* splice_mergeleaf_sr: mergeleaf d>0 o==*l lidx>=mid (L749).
 * botV(leafV(2,2,2,2), leafV(2,2,2,2)), C pos 7 lidx=3 col=1, del=5. */
static void test_splice_mergeleaf_sr(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(2, 2, 2, 2), leafV(2, 2, 2, 2)));
    check_tree(c);
    lc_seek(&cur, c, 7);
    lc_splice(&cur, 5, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* splice_mergeleaf_dpos: mergeleaf d>0 o==*l lidx>=mid (L749).
 * L at leaf0 lidx=4 (boundary) → trimleaf early return → cl=4.
 * R at leaf1 lidx=2 → cr=2. cl+cr=6>4, d=4-3=1>0, lidx=4>=mid=3. */
static void test_splice_mergeleaf_dpos(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor L, R;
    lc_Cache *c = cacheV(S, 0, botV(leafV(2, 2, 2, 2), leafV(2, 2, 2, 2)));
    check_tree(c);
    lc_seek(&L, c, 6);
    L.lidx = 4;         /* force past leaf0 end, trimleaf(L,0) returns early */
    lc_seek(&R, c, 12); /* leaf1 lidx=2 col=0, trimleaf(R,1) dl=2 */
    lcD_splicerange(&L, &R);
    check_tree(c);
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
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2, 2))), innerV(botV(leafV(2, 2))),
                   innerV(botV(leafV(2, 2)))));
    check_tree(c);
    lc_seek(&cur, c, 1);
    lc_splice(&cur, 7, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* splice_mergenode_dpos: balancenode d>0 (L762-764) + mergenode cursor
 * adjust (L791) + Phase2 mergenode loop (L888-890).
 * levels=2: inner0 has botV with 2 leaf children (4 segs),
 * inner2 has botV with 1 leaf child (2 segs). After Phase 1,
 * merge at dl=1: cl=2, cr=1, total=3<=4 → foldnode would trigger,
 * so use larger counts: inner0 botV has 4 leaf children,
 * inner2 botV has 2 leaf children → cl+cr=6>4, d>0. */
static void test_splice_mergenode_dpos(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2), leafV(2), leafV(2), leafV(2))),
                   innerV(botV(leafV(2), leafV(2))),
                   innerV(botV(leafV(2), leafV(2)))));
    check_tree(c);
    lc_seek(&cur, c, 3);   /* in inner0, 2nd leaf */
    lc_splice(&cur, 8, 0); /* cross to inner2, R at offset 11 */
    check_tree(c);
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
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2, 2))),
                   innerV(botV(leafV(2), leafV(2), leafV(2), leafV(2)))));
    check_tree(c);
    lc_seek(&cur, c, 2);   /* in inner0 botV leaf, offset 2 */
    lc_splice(&cur, 5, 0); /* R at offset 7, in inner1 */
    check_tree(c);
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
    lc_Cursor cur;
    unsigned  brs[66];
    int       i, r;

    for (i = 0; i < 65; ++i) brs[i] = 10;
    brs[65] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK && lc_breaks(c) == 65);

    /* Delete large middle portion, leaving edges intact.
     * Offset 60, del=500 keeps L and R in different subtrees. */
    lc_seek(&cur, c, 60);
    lc_splice(&cur, 500, 0);
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* empty_tree_reset: lcD_emptytree path — delete all at offset 0
 * (with and without insert) to cover both early return branches
 * and the post-delete empty-tree reset in lc_splice. */
static void test_empty_tree_reset(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c;
    lc_Cursor cur;

    /* delete all (ins=0) */
    c = cacheV(S, 0, botV(leafV(10, 10), leafV(10)));
    assert(c);
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 30, 0);
    assert(c->bytes == 0 && c->breaks == 0);
    check_tree_allow_empty(c, 1);
    lc_deltree(S, c);

    /* delete all + insert (ins>0) to cover C->col += ins in lcD_emptytree */
    c = cacheV(S, 0, botV(leafV(10, 10), leafV(10)));
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 30, 7);
    assert(c->bytes == 0 && c->breaks == 0);
    assert(cur.col == 7);
    check_tree_allow_empty(c, 1);
    lc_deltree(S, c);

    lc_close(S);
}

/* shiftleaf_neg_d: shiftleaf d<0 path (L649-654). L at lidx=2, R at lidx=1
 * in 4-seg leaves → cl=2, cr=3 → cl+cr=5>4, d=-1 → move 1 seg R→L. */
static void test_shiftleaf_neg_d(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10, 10, 10, 10), leafV(10)),
                   botV(leafV(10, 10, 10, 10), leafV(10)), botV(leafV(10))));
    lc_Cursor cur;
    assert(c);
    lc_seek(&cur, c, 20);   /* L at lidx=2 col=0 in botV0.leaf0 */
    lc_splice(&cur, 40, 0); /* R at offset 60, botV1.leaf0 lidx=1 */
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode_redistribute: foldnode redistribute (L778-790).
 * levels=2. spliceleaf del=3 makes leaf underfull → foldleaf merge
 * botV cc→1 → rebalance → foldnode at innerV level.
 * botV0 cc=1 + botV1 cc=4 → 5>4 → redistribute. */
static void test_foldnode_redistribute(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *inner0 = innerV(
            botV(leafV(2, 2), leafV(2, 2, 2)),
            botV(leafV(2), leafV(2), leafV(2), leafV(2)));
    lc_Node  *inner1 = innerV(botV(leafV(2)));
    lc_Node  *root = innerV(inner0, inner1);
    lc_Cache *c = cacheV(S, 2, root);
    lc_Cursor cur;
    assert(c);
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 3, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* trimleaf_left_full: trimleaf left with lidx beyond count (L614). */
static void test_trimleaf_left_full(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1, innerV(botV(leafV(10, 10), leafV(10)), botV(leafV(10, 10))));
    lc_Cursor cur;
    assert(c);
    lc_seek(&cur, c, 20); /* in botV1 leaf0, lidx=0 col=0 */
    lc_splice(
            &cur, 10,
            0); /* R in same leaf? Actually cross-leaf within same botV */
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* foldleaf_redistribute_left: cl=4 cr=2 dl=1>0, cursor in left leaf
 * at lidx=3 (>= cl-dl=3). foldleaf redistributes, cursor data moves
 * from left leaf tail to right leaf head. lidx: 4→3, paths switches. */
static void test_foldleaf_redistribute_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10, 10, 10), leafV(10, 10)));
    lc_Cursor cur;
    assert(c && lc_breaks(c) == 6 && lc_bytes(c) == 60);

    /* cursor in left leaf, lidx=3 (last segment), col=5 */
    lc_seek(&cur, c, 35);
    assert(lc_offset(&cur) == 35 && cur.lidx == 3 && cur.col == 5);
    assert(cur.paths[0] == &c->root.children[0]);

    lcD_foldleaf(&cur);
    check_tree(c);

    assert(lc_breaks(c) == 6 && lc_bytes(c) == 60);
    assert(cur.paths[0] == &c->root.children[1]);
    assert(cur.lidx == 0 && cur.col == 5);
    assert(lc_offset(&cur) == 35);

    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode_redistribute_left: cl=4 cr=2 dl=1>0, cursor in left inner node
 * at child index 3 (>= cl-dl=3). foldnode redistributes, cursor child
 * moves from left inner node to right inner node. Paths must switch. */
static void test_foldnode_redistribute_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *left = innerV(
            botV(leafV(1, 0)), botV(leafV(1, 0)), botV(leafV(1, 0)),
            botV(leafV(1, 0)));
    lc_Node  *right = innerV(botV(leafV(1, 0)), botV(leafV(1, 0)));
    lc_Node  *r = innerV(left, right);
    lc_Cache *c = cacheV(S, 2, r);
    lc_Cursor cur;
    assert(c);
    assert(lc_breaks(c) == 6 && lc_bytes(c) == 6);

    /* seek to last child of left inner node (4th botV, index 3) */
    lc_seek(&cur, c, 3);
    assert(lc_offset(&cur) == 3);
    assert(cur.paths[0] == &c->root.children[0]);

    lcD_foldnode(&cur, 0);
    check_tree(c);

    /* dl=1 child moved left->right, paths[0] must switch to right child */
    assert(lc_breaks(c) == 6 && lc_bytes(c) == 6);
    assert(cur.paths[0] == &c->root.children[1]);
    assert(lc_offset(&cur) == 3);

    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode_redistribute_right: cl=2 cr=4 dl=-1<0, cursor in right inner
 * node at child index 0 (< -dl=1). foldnode redistributes right->left,
 * cursor child moves to left inner node. (covers L747) */
static void test_foldnode_redistribute_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *left = innerV(botV(leafV(1, 0)), botV(leafV(1, 0)));
    lc_Node  *right = innerV(
            botV(leafV(1, 0)), botV(leafV(1, 0)), botV(leafV(1, 0)),
            botV(leafV(1, 0)));
    lc_Node  *r = innerV(left, right);
    lc_Cache *c = cacheV(S, 2, r);
    lc_Cursor cur;
    assert(c);
    assert(lc_breaks(c) == 6 && lc_bytes(c) == 6);

    /* seek to first child of right inner node, offset=2 */
    lc_seek(&cur, c, 2);
    assert(lc_offset(&cur) == 2);
    assert(cur.paths[0] == &c->root.children[1]);

    lcD_foldnode(&cur, 0);
    check_tree(c);

    /* dl=-1 child moved right->left, paths[0] switched to left child */
    assert(lc_breaks(c) == 6 && lc_bytes(c) == 6);
    assert(cur.paths[0] == &c->root.children[0]);
    assert(lc_offset(&cur) == 2);

    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode_redistribute_shift: cl=4 cr=1 dl=1>0, cursor in right inner
 * node. Child stays in right, shifted right by dl. (covers L752) */
static void test_foldnode_redistribute_shift(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *left = innerV(
            botV(leafV(1, 0)), botV(leafV(1, 0)), botV(leafV(1, 0)),
            botV(leafV(1, 0)));
    lc_Node  *right = innerV(botV(leafV(1, 0)));
    lc_Node  *r = innerV(left, right);
    lc_Cache *c = cacheV(S, 2, r);
    lc_Cursor cur;
    assert(c);
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 5);

    /* seek to right innerV's only child, offset=4 */
    lc_seek(&cur, c, 4);
    assert(lc_offset(&cur) == 4);
    assert(cur.paths[0] == &c->root.children[1]);

    lcD_foldnode(&cur, 0);
    check_tree(c);

    /* dl>0, cursor in right: child shifted right, paths[0] unchanged */
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 5);
    assert(cur.paths[0] == &c->root.children[1]);
    assert(lc_offset(&cur) == 4);

    lc_deltree(S, c);
    lc_close(S);
}

/* bulk scan: 0 entries — scanner returns 0 immediately on empty tree */
static void test_scan_no_input(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    unsigned  brs[1] = {0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 0 && lc_bytes(c) == 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* bulk scan: exactly one full leaf (LC_LEAF_FANOUT entries) */
static void test_scan_one_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    unsigned  brs[5] = {5, 10, 15, 20, 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 50);
    assert(c->levels == 0 && c->root.child_count == 1);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* bulk scan: 120 entries → multi-level tree */
static void test_scan_bulk_many(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[121], *pbrs = brs;
    int       i, r;

    for (i = 0; i < 120; ++i) brs[i] = 1;
    brs[120] = 0;
    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 120 && lc_bytes(c) == 120);
    assert(c->levels >= 2);
    check_tree(c);
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
    unsigned  brs_a[5] = {10, 10, 10, 10, 0}, *pa = brs_a;
    unsigned  brs_b[6] = {20, 20, 20, 20, 20, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, scanner, &pa);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 40);
    r = lc_scan(c, scanner, &pb);
    assert(r == LC_OK && lc_breaks(c) == 9 && lc_bytes(c) == 140);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* scan OOM: fail allocating pend_root (node page) during lcB_checkpendroot.
 * alloc order: #1 lc_open, #2 lc_newtree, #3 node page ← fail */
static void test_scan_oom_items(void) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[2] = {10, 0}, *pbrs = brs;
    int       r;

    oom_cnt = 3;
    S = lc_open(&oom_alloc, NULL);
    if (!S) return;
    c = lc_newtree(S);
    r = lc_scan(c, scanner, &pbrs);
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
    unsigned  brs[171], *pbrs = brs;
    int       i, r;

    for (i = 0; i < 170; ++i) brs[i] = 10;
    brs[170] = 0;
    oom_cnt = 5;
    S = lc_open(&oom_alloc, NULL);
    if (!S) return;
    c = lc_newtree(S);
    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_ERRMEM);
    lc_deltree(S, c);
    lc_close(S);
}

/* scan OOM: fail allocating leaf page during lcB_fill.
 * alloc order: #1 lc_open, #2 lc_newtree, #3 node page, #4 leaf page ← fail */
static void test_scan_oom_build(void) {
    lc_State *S;
    lc_Cache *c;
    unsigned  brs[171], *pbrs = brs;
    int       i, r;

    for (i = 0; i < 170; ++i) brs[i] = 1;
    brs[170] = 0;
    oom_cnt = 4;
    S = lc_open(&oom_alloc, NULL);
    if (!S) return;
    c = lc_newtree(S);
    r = lc_scan(c, scanner, &pbrs);
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
    check_tree(c);
    lc_seekline(&cur, c, 2);
    assert(cur.idx == 2);
    assert(lc_linelen(&cur) == 5);
    lc_deltree(S, c);

    /* lcK_forwardline L446: <= -> <.
     * 3 叶: leaf0(2段), leaf1(2段), leaf2(1段). advline(4) 自始=leaf2.
     * <= 误停 leaf1, d=2, findinleaf 耗尽断言崩. */
    c = cacheV(S, 0, botV(leafV(1, 1), leafV(1, 1), leafV(5)));
    check_tree(c);
    lc_seek(&cur, c, 0);
    lc_advline(&cur, 4);
    assert(cur.idx == 4);
    printf("linelen: %u\n", lc_linelen(&cur));
    assert(lc_linelen(&cur) == 5);
    lc_deltree(S, c);

    lc_close(S);
}

/* coverage: shiftnode balance branch (L694-L695).
 * Two adjacent innerV nodes with 3+2=5 > LC_FANOUT=4 children.
 * Call shiftnode directly to avoid splicerange path complexity. */
static void test_cov_shiftnode_balance(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node *innerA = innerV(botV(leafV(10)), botV(leafV(10)), botV(leafV(10)));
    lc_Node *innerB = innerV(botV(leafV(10)), botV(leafV(10)));
    lc_Node *root = innerV(innerA, innerB);
    lc_Cache *c = cacheV(S, 2, root);
    lc_Cursor C1, C2;
    assert(c);
    assert(lc_bytes(c) == 50 && lc_breaks(c) == 5);
    check_tree(c);
    lc_seek(&C1, c, 20); /* innerA last botV */
    lc_seek(&C2, c, 30); /* innerB first botV */
    lcD_shiftnode(&C1, &C2, 0);
    lc_deltree(S, c);
    lc_close(S);
}

/* coverage: foldleaf inmarkbreak right→left cursor move (L718).
 * cl=2 cr=4: right leaf larger, cursor at lidx=0 in right leaf.
 * After balanceleaf, dl=-1, lidx<1 → cursor moves to left leaf (L718). */
static void test_cov_foldleaf_inmarkbreak(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10), leafV(10, 10, 10, 10)));
    lc_Cursor cur;
    assert(c && lc_breaks(c) == 6 && lc_bytes(c) == 60);
    check_tree(c);
    lc_seek(&cur, c, 20);
    assert(lc_offset(&cur) == 20 && cur.lidx == 0 && cur.col == 0);
    assert(cur.paths[0] == &c->root.children[1]);
    lcD_foldleaf(&cur);
    check_tree(c);
    assert(lc_breaks(c) == 6 && lc_bytes(c) == 60);
    assert(cur.paths[0] == &c->root.children[0]);
    assert(cur.lidx == 2 && cur.col == 0);
    assert(lc_offset(&cur) == 20);
    lc_deltree(S, c);
    lc_close(S);
}

/* main */

/* insert into non-empty tree: single leaf, middle insert */
static void test_insert_single_leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs_a[5] = {10, 15, 15, 0}, *pa = brs_a;
    unsigned  brs_b[3] = {3, 3, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, scanner, &pa);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_bytes(c) == 40);

    lc_seek(&C, c, 10);
    r = lc_insert(&C, 3, scanner, &pb);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_breaks(c) == 5 && lc_bytes(c) == 49);
    lc_deltree(S, c);
    lc_close(S);
}

/* insert col>0: split current line, merge first br, prepend e */
static void test_insert_col_mid(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs_a[3] = {4, 7, 0}, *pa = brs_a;
    unsigned  brs_b[3] = {4, 4, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, scanner, &pa);
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_bytes(c) == 11);

    lc_seek(&C, c, 6);
    assert(C.lidx == 1 && C.col == 2);
    r = lc_insert(&C, 3, scanner, &pb);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 22);
    lc_deltree(S, c);
    lc_close(S);
}

/* br==0, e>0: add e to current line */
static void test_insert_no_scanner(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs[3] = {10, 10, 0}, *pbrs = brs;
    unsigned  zero[1] = {0}, *pz = zero;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 7, scanner, &pz);
    assert(r == LC_OK);
    check_tree(c);
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
    unsigned  brs_a[3] = {10, 10, 0}, *pa = brs_a;
    unsigned  brs_b[3] = {5, 5, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, scanner, &pa);
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 20);
    assert(lc_offset(&C) == 20 && C.col == 0);
    C.col = 5;
    r = lc_insert(&C, 7, scanner, &pb);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_breaks(c) == 4 && lc_bytes(c) == 30);
    lc_deltree(S, c);
    lc_close(S);
}

/* insert causing leaf split */
static void test_insert_leaf_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs_a[4] = {5, 5, 5, 0}, *pa = brs_a;
    unsigned  brs_b[4] = {3, 3, 3, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, scanner, &pa);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_bytes(c) == 15);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 2, scanner, &pb);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_breaks(c) == 6 && lc_bytes(c) == 26);
    lc_deltree(S, c);
    lc_close(S);
}

/* br==0, e==0: no-op */
static void test_insert_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs[3] = {10, 10, 0}, *pbrs = brs;
    unsigned  zero[1] = {0}, *pz = zero;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_bytes(c) == 20);

    lc_seek(&C, c, 5);
    r = lc_insert(&C, 0, scanner, &pz);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 20);
    lc_deltree(S, c);
    lc_close(S);
}

/* verify cursor position after col>0 insert */
static void test_insert_cursor_pos(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs_a[3] = {4, 7, 0}, *pa = brs_a;
    unsigned  brs_b[3] = {4, 4, 0}, *pb = brs_b;
    int       r;

    r = lc_scan(c, scanner, &pa);
    assert(r == LC_OK);
    lc_seek(&C, c, 6);
    assert(C.lidx == 1 && C.col == 2);
    r = lc_insert(&C, 3, scanner, &pb);
    assert(r == LC_OK);
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
    unsigned  brs_a[4] = {10, 10, 10, 0}, *pa = brs_a;
    unsigned  brs_b[18], *pb = brs_b;
    int       i, r;

    r = lc_scan(c, scanner, &pa);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_bytes(c) == 30);

    for (i = 0; i < 17; ++i) brs_b[i] = 1;
    brs_b[17] = 0;
    lc_seek(&C, c, 5);
    r = lc_insert(&C, 0, scanner, &pb);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_breaks(c) == 20 && lc_bytes(c) == 47);
    lc_deltree(S, c);
    lc_close(S);
}

/* empty tree insert */
static void test_insert_empty(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  brs[4] = {10, 10, 10, 0}, *pbrs = brs;
    int       r;

    lc_seek(&C, c, 0);
    r = lc_insert(&C, 5, scanner, &pbrs);
    assert(r == LC_OK);
    check_tree(c);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 30);
    lc_deltree(S, c);
    lc_close(S);
}

/* empty tree + no scanner + no e: hits flushat empty-pend path */
static void test_insert_empty_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    unsigned  zero[1] = {0}, *pz = zero;
    int       r;

    lc_seek(&C, c, 0);
    r = lc_insert(&C, 0, scanner, &pz);
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
    unsigned  brs[3] = {2, 0}, *pbrs = brs;
    int       found = 0, k;
    for (k = 3; k <= 10; ++k) {
        oom_cnt = k;
        S = lc_open(&oom_alloc, NULL);
        if (!S) continue;
        c = lc_newtree(S);
        if (!c) {
            lc_close(S);
            continue;
        }
        lc_seek(&C, c, 0);
        pbrs = brs;
        if (lc_insert(&C, 0, scanner, &pbrs) == LC_ERRMEM) found = 1;
        lc_deltree(S, c);
        lc_close(S);
        if (found) break;
    }
    assert(found);
}

/* OOM in driverun for normal path */
static void test_insert_oom_normal(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs_a[3] = {5, 5, 0}, *pa;
    unsigned  brs_b[18], *pb;
    int       i, k, found = 0;

    for (k = 4; k <= 15; ++k) {
        oom_cnt = k;
        S = lc_open(&oom_alloc, NULL);
        if (!S) continue;
        c = lc_newtree(S);
        if (!c) {
            lc_close(S);
            continue;
        }
        pa = brs_a;
        if (lc_scan(c, scanner, &pa) != LC_OK) {
            lc_deltree(S, c);
            lc_close(S);
            continue;
        }
        for (i = 0; i < 17; ++i) brs_b[i] = 1;
        brs_b[17] = 0;
        pb = brs_b;
        lc_seek(&C, c, 3);
        if (lc_insert(&C, 0, scanner, &pb) == LC_ERRMEM) found = 1;
        lc_deltree(S, c);
        lc_close(S);
        if (found) break;
    }
    assert(found);
}

#define TESTS(X)                        \
    X(lifecycle)                        \
    X(scan_seek)                        \
    X(scan_no_input)                    \
    X(scan_one_leaf)                    \
    X(scan_bulk_many)                   \
    X(scan_append_many)                 \
    X(scan_oom_items)                   \
    X(scan_oom_flush)                   \
    X(scan_oom_build)                   \
    X(seekline)                         \
    X(advance_single)                   \
    X(advance_cross)                    \
    X(node_split)                       \
    X(seek_past_leaf)                   \
    X(advance_backward_cross)           \
    X(advline_cross)                    \
    X(advline_backward_within)          \
    X(cov_backwardline_cross)           \
    X(cov_forwardline_crossnode)        \
    X(advance_skip_siblings)            \
    X(node_split_cursor_right)          \
    X(splitleaf_left)                   \
    X(rootsplit_left)                   \
    X(findlines_skip)                   \
    X(rootsplit_left_deep)              \
    X(nodesplit_left)                   \
    X(findlines_skip_deep)              \
    X(backwardoff_skip)                 \
    X(cov_remaining)                    \
    X(clearbreaks)                      \
    X(clearbreaks_edge)                 \
    X(clearbreaks_slot)                 \
    X(splice)                           \
    X(splice_tmp)                       \
    X(splice_l2)                        \
    X(splice_trailing)                  \
    X(splice_cross_breaks)              \
    X(splice_cross_breaks_slot)         \
    X(splice_cross_breaks_mid)          \
    X(spliceleaf_1a)                    \
    X(spliceleaf_1b)                    \
    X(spliceleaf_1c)                    \
    X(spliceleaf_foldright)             \
    X(spliceleaf_foldleft)              \
    X(prune_leaf)                       \
    X(prune_node)                       \
    X(rebalance_shrink)                 \
    X(rebalance_double)                 \
    X(rebalance_node_merge_right)       \
    X(rebalance_node_merge_left)        \
    X(rebalance_node_merge_right_large) \
    X(rebalance_cascade)                \
    X(trimleaf_trimleft)                \
    X(trimleaf_trimright)               \
    X(trimleaf_mergeleaf)               \
    X(trimleaf_right_end)               \
    X(trimnode_trimright)               \
    X(trimnode_trimleft)                \
    X(trimnode_mergenode)               \
    X(mergeleaf_combine)                \
    X(mergeleaf_sr_nonzero)             \
    X(mergeleaf_cross_subtree)          \
    X(mergenode_combine)                \
    X(mergenode_sr_nonzero)             \
    X(mergenode_cross_subtree)          \
    X(splicerange_2leaf)                \
    X(splicerange_prune)                \
    X(splicerange_all)                  \
    X(splicerange_merge_rebalance)      \
    X(splicerange_foldnode_upper)       \
    X(splice_uf_last)                   \
    X(splice_mergeleaf_sr)              \
    X(splice_mergeleaf_dpos)            \
    X(splice_mergenode_fold)            \
    X(splice_mergenode_dpos)            \
    X(splice_mergenode_last)            \
    X(splice_removed2)                  \
    X(empty_tree_reset)                 \
    X(shiftleaf_neg_d)                  \
    X(foldnode_redistribute)            \
    X(trimleaf_left_full)               \
    X(foldleaf_redistribute_left)       \
    X(foldnode_redistribute_left)       \
    X(foldnode_redistribute_right)      \
    X(foldnode_redistribute_shift)      \
    X(boundary_cmp)                     \
    X(markbreak)                        \
    X(markbreaks)                       \
    X(markbreak_split)                  \
    X(markbreak_empty)                  \
    X(markbreak_noop)                   \
    X(markbreak_crossline)              \
    X(markbreak_crossline_end)          \
    X(markbreak_trailing)               \
    X(markbreak_brzero)                 \
    X(markbreak_node_split)             \
    X(markbreak_root_split)             \
    X(markbreak_split_right)            \
    X(markbreak_root_split_on_add)      \
    X(markbreak_crossleaf)              \
    X(cov_shiftnode_balance)            \
    X(cov_foldleaf_inmarkbreak)         \
    X(insert_single_leaf)               \
    X(insert_col_mid)                   \
    X(insert_no_scanner)                \
    X(insert_trailing)                  \
    X(insert_leaf_split)                \
    X(insert_noop)                      \
    X(insert_cursor_pos)                \
    X(insert_many)                      \
    X(insert_empty)                     \
    X(insert_empty_noop)                \
    X(insert_oom_trailing)              \
    X(insert_oom_normal)

int main(int argc, char *argv[]) {
    typedef struct entry {
        const char *name;
        void (*fn)(void);
    } entry_t;
    const entry_t entries[] = {
#define X(name) {#name, test_##name}, /* clang-format off */
        TESTS(X)
#undef  X
        {NULL, NULL}, /* clang-format on */
    };
    int i, j;
    (void)&innerV_; /* used by upcoming merge tests */
    printf("=== linecache tests ===\n");
    if (argc == 1) {
        /* run all tests */
        const entry_t *e = entries;
        while (e->name) {
            printf("- %s\n", e->name);
            e->fn();
            printf("  %s OK\n", e->name);
            ++e;
        }
        printf("\nAll tests passed!\n");
        return 0;
    }
    /* run specified tests */
    for (i = 1; i < argc; ++i) {
        const char *name = argv[i];
        size_t      len = strlen(name);
        int         found = 0, only = *name == '@';
        if (only) name++, len--;
        for (j = 0; entries[j].name; ++j) {
            if (strlen(entries[j].name) >= len
                && strncmp(name, entries[j].name, len) == 0) {
                printf("- %s\n", entries[j].name);
                entries[j].fn();
                printf("  %s OK\n", entries[j].name);
                found = 1;
                if (only) break;
            }
        }
        if (!found) {
            fprintf(stderr, "Unknown test: %s\n", name);
            return 1;
        }
    }
    return 0;
}
