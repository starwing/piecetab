#define LC_LEAF_FANOUT 4
#define LC_FANOUT      4
#define LC_STATIC_API

#include "linecache.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tree invariant checker */

static size_t check_node(const lc_Node *n, unsigned l, unsigned levels) {
    unsigned i;
    size_t   bsum = 0, lsum = 0;
    assert(n->child_count > 0);
    assert(n->child_count <= LC_FANOUT);
    for (i = 0; i < n->child_count; ++i) {
        if (l == levels || levels == 0) {
            assert(n->breaks[i] <= LC_LEAF_FANOUT);
            bsum += n->bytes[i], lsum += n->breaks[i];
        } else {
            size_t cb = check_node(n->children[i], l + 1, levels);
            assert(cb == n->bytes[i]);
            bsum += cb, lsum += n->breaks[i];
        }
    }
    return bsum;
}

static void check_tree(const lc_Cache *c) {
    size_t st;
    if (c->root.child_count == 0) {
        assert(c->bytes == 0 && c->breaks == 0);
        return;
    }
    check_node(&c->root, 0, c->levels);
    /* verify c->bytes matches sum of root children bytes */
    st = 0;
    {
        unsigned i;
        for (i = 0; i < c->root.child_count; ++i) st += c->root.bytes[i];
    }
    assert(c->bytes == st);
    /* verify c->breaks matches sum of root children breaks */
    st = 0;
    {
        unsigned i;
        for (i = 0; i < c->root.child_count; ++i) st += c->root.breaks[i];
    }
    assert(c->breaks == st);
}

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

typedef struct lc_ScanCtx {
    size_t pos;
    size_t breaks[320];
} lc_ScanCtx;

static lc_ScanCtx init_scanner(int first, ...) {
    va_list    li;
    lc_ScanCtx s;
    size_t     b, i = 0;
    va_start(li, first);
    if (first != 0) {
        s.breaks[i++] = first;
        while ((b = va_arg(li, size_t)) != 0) s.breaks[i++] = b;
    }
    va_end(li);
    s.breaks[i] = 0;
    s.pos = 0;
    return s;
}

static unsigned test_scanner(void *ud, size_t prev) {
    lc_ScanCtx *s = (lc_ScanCtx *)ud;
    if (s == NULL) return 0;
    while (s->breaks[s->pos] != 0 && s->breaks[s->pos] <= prev) s->pos++;
    if (s->breaks[s->pos] == 0) return 0;
    return (unsigned)(s->breaks[s->pos++] - prev);
}

static void test_scan_seek(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);

    int r;

    r = lc_scan(c, &test_scanner, &s);
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
        lc_ScanCtx s2 = init_scanner(45, 55, 0);
        r = lc_scan(c, &test_scanner, &s2);
        assert(r == LC_OK && lc_breaks(c) == 5 && lc_bytes(c) == 55);
    }

    lc_deltree(S, c);
    lc_close(S);
}

/* T3: seekline */

static void test_seekline(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        r;

    lc_scan(c, &test_scanner, &s);

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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        r;

    lc_scan(c, &test_scanner, &s);

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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 10 breaks = 2 full leaves (FANOUT=4) + 2 partial: triggers leaf promotion
     */
    for (i = 0; i < 10; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[10] = 0;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 30, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 4 breaks fills one leaf (FANOUT=4); 5 triggers split */
    for (i = 0; i < 5; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[5] = 0;

    r = lc_scan(c, &test_scanner, &s);
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

/* T9: markbreak in trailing area */

static void test_markbreak_trailing(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 3);

    /* add trailing bytes via splice to create a trailing gap */
    lc_seek(&cur, c, 40);
    lc_splice(&cur, 0, 20); /* add 20 trailing bytes, gap becomes 20 */
    assert(lc_bytes(c) == 60);

    /* now markbreak within trailing gap (slot == count) */
    r = lc_markbreak(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 4);
    assert(lc_offset(&cur) == 65 && lc_line(&cur) == 4);
    assert(lc_linelen(&cur) == 15); /* 20 - 5 = 15 remaining gap */

    lc_deltree(S, c);
    lc_close(S);
}

/* T10: node split and root split (needs many leaves) */

static void test_node_split(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 120 breaks: 30 leaves (FANOUT=4), which forces node splits and root split
     */
    for (i = 0; i < 120; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[120] = 0;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 55, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 4);

    /* clear breaks in middle of gap (no breaks to remove) */
    lc_seek(&cur, c, 2);
    r = lc_clearbreaks(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 4); /* no breaks in range */

    /* clear 1 break at break boundary */
    lc_seek(&cur, c, 10);
    r = lc_clearbreaks(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 3);

    /* null check */
    r = lc_clearbreaks(NULL, 1);
    assert(r == LC_ERRPARAM);

    lc_deltree(S, c);
    lc_close(S);
}

/* T12: clearbreaks with 0-length */

static void test_clearbreaks_edge(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 55, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 55);

    /* len == 0: no-op */
    lc_seek(&cur, c, 5);
    r = lc_clearbreaks(&cur, 0);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_linelen(&cur) == 10);

    /* clear exactly one break: seek to break boundary */
    lc_seek(&cur, c, 10);
    r = lc_clearbreaks(&cur, 5);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_linelen(&cur) == 25);

    /* past end of tree: del clamped to remaining (5), ins=20,
       tree had 3 breaks after previous case; no break crossed here */
    lc_seek(&cur, c, 50);
    assert(lc_linelen(&cur) == 15);
    r = lc_clearbreaks(&cur, 20);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_linelen(&cur) == 30);
    assert(lc_bytes(c) == 70);

    lc_deltree(S, c);
    lc_close(S);
}

/* splice_tmp: 简化跨叶删除，从非零位置删大部分内容 */
static void test_splice_tmp(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 17 breaks → 5 leaves → levels=1 树 */
    for (i = 0; i < 17; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[17] = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 17 && lc_bytes(c) == 170);
    fprintf(stderr, "DEBUG tmp: levels=%u root.cc=%u\n", c->levels,
            c->root.child_count);
    dump_tree(c, "splice_tmp after scan");
    check_tree(c);
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 170, 0); /* delete all from 0 */
    fprintf(stderr, "DEBUG tmp1: breaks=%zu bytes=%zu levels=%u\n",
            lc_breaks(c), lc_bytes(c), c->levels);
    dump_tree(c, "splice_tmp after delete all");
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);

    /* 重建，从 offset 25 删 140 bytes */
    s.pos = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 17 && lc_bytes(c) == 170);
    lc_seek(&cur, c, 25);
    fprintf(stderr,
            "DEBUG tmp2 before splice: off=%zu loff=%zu lidx=%u col=%u "
            "levels=%u\n",
            lc_offset(&cur), (size_t)cur.loff, cur.lidx, cur.col, c->levels);
    lc_splice(&cur, 140, 0);
    fprintf(stderr, "DEBUG tmp2: breaks=%zu bytes=%zu\n", lc_breaks(c),
            lc_bytes(c));
    dump_tree(c, "tmp2 after splice");
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

static void test_splice_l2(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 65 breaks → levels=2 tree, 650 bytes */
    for (i = 0; i < 65; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[65] = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 65 && lc_bytes(c) == 650);
    fprintf(stderr, "DEBUG l2: levels=%u root.cc=%u\n", c->levels,
            c->root.child_count);
    dump_tree(c, "splice_l2 after scan");
    check_tree(c);

    /* delete all */
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 650, 0);
    fprintf(stderr, "DEBUG l2 delall: breaks=%zu bytes=%zu levels=%u\n",
            lc_breaks(c), lc_bytes(c), c->levels);
    assert(lc_breaks(c) == 0 && lc_bytes(c) == 0);

    /* rebuild, partial delete from offset 25 */
    s.pos = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 65 && lc_bytes(c) == 650);
    lc_seek(&cur, c, 25);
    lc_splice(&cur, 600, 0); /* delete 600, keep 25+25=50 */
    fprintf(stderr, "DEBUG l2 partial: breaks=%zu bytes=%zu levels=%u\n",
            lc_breaks(c), lc_bytes(c), c->levels);
    dump_tree(c, "splice_l2 after partial delete");
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* T13: splice */

static void test_splice(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    for (i = 0; i < 100; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[100] = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 100 && lc_bytes(c) == 1000);
    dump_tree(c, "after scan");
    check_tree(c);
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 1000, 0); /* delete all */
    fprintf(stderr, "DEBUG T13: breaks=%zu bytes=%zu\n", lc_breaks(c),
            lc_bytes(c));
    dump_tree(c, "after splice (tree cleanup)");
    assert(r == LC_OK && lc_breaks(c) == 0 && lc_bytes(c) == 0);
    s.pos = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 100 && lc_bytes(c) == 1000);
    lc_seek(&cur, c, 11);
    lc_splice(&cur, 980, 0); /* delete all but first 11 + last 9 */
    printf("DEBUG T13: breaks=%zu bytes=%zu\n", lc_breaks(c), lc_bytes(c));
    dump_tree(c, "T13 after partial delete");
    assert(r == LC_OK && lc_breaks(c) == 2 && lc_bytes(c) == 20);

    s = init_scanner(10, 25, 40, 0);
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK);

    /* simple splice (no break crossing) */
    lc_seek(&cur, c, 2);
    lc_splice(&cur, 5, 3);
    assert(lc_bytes(c) == 38 && lc_offset(&cur) == 5);

    /* splice crossing breaks */
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 15, 8);
    assert(lc_bytes(c) == 31); /* 38 - 15 + 8 */

    /* splice with del=0, ins=0 (no-op) */
    lc_splice(&cur, 0, 0);

    /* null check */
    lc_splice(NULL, 1, 1);

    lc_deltree(S, c);
    lc_close(S);
}

/* T14: splice trailing area */

static void test_splice_trailing(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK);
    lc_scan(c, &test_scanner, NULL);

    /* after last break (trailing area): slot=3 (==breaks) */
    lc_seek(&cur, c, 40);
    lc_splice(&cur, 0, 20);    /* insert 20 bytes at end */
    assert(lc_bytes(c) == 60); /* 40 + 20 */
    assert(lc_offset(&cur) == 60);

    /* now in trailing: seek to trailing, verify */
    lc_seek(&cur, c, 45);
    assert(lc_line(&cur) == 3); /* still after all 3 breaks */

    lc_deltree(S, c);
    lc_close(S);
}

/* T15: seek past first leaf (covers lcC_findpieces skip-child path) */

static void test_seek_past_leaf(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    for (i = 0; i < 8; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[8] = 0;

    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 8);

    /* seek to offset past first leaf (first leaf has 4 breaks, 40 bytes) */
    r = lc_seek(&cur, c, 45);
    assert(r == LC_OK && lc_offset(&cur) == 45 && lc_line(&cur) == 4);

    lc_deltree(S, c);
    lc_close(S);
}

/* T16: backward cross-sibling (covers lcC_backwardoff cross-sibling) */

static void test_advance_backward_cross(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        i, r;

    for (i = 0; i < 8; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[8] = 0;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    for (i = 0; i < 8; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[8] = 0;

    r = lc_scan(c, &test_scanner, &s);
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

/* T18: markbreak triggers internal node split */

static void test_markbreak_node_split(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 17 breaks: 4 full leaves + 1 partial = 5 leaves in internal node */
    for (i = 0; i < 17; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[17] = 0;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 70 breaks → many leaves → deep tree with full internal nodes */
    for (i = 0; i < 70; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[70] = 0;
    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 20, 30, 40, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 55, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    int        r;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 12 breaks = 3 full leaves (4 breaks each), forces promotion */
    for (i = 0; i < 12; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[12] = 0;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 21 breaks: 5 full leaves + 1 partial, fills internal to 5 > FANOUT=4 */
    for (i = 0; i < 21; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[21] = 0;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* many breaks to force deep multi-level tree */
    for (i = 0; i < 150; ++i) s.breaks[i] = (size_t)((i + 1) * 5);
    s.breaks[150] = 0;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 20, 30, 40, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 17 breaks triggers root split; oi=0 puts cursor in left half */
    for (i = 0; i < 17; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[17] = 0;
    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* small tree with 2 leaves: breaks in root are [2, N] */
    for (i = 0; i < 6; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[6] = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 6);

    /* seekline to line 4 (past first child's 2 breaks) */
    r = lc_seekline(&cur, c, 4);
    assert(r == LC_OK && lc_line(&cur) == 4);

    lc_deltree(S, c);
    lc_close(S);
}

/* T31: root split left path via markbreak on deep tree */

static void test_rootsplit_left_deep(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* deep tree with levels >= 2 */
    for (i = 0; i < 100; ++i) s.breaks[i] = (size_t)((i + 1) * 5);
    s.breaks[100] = 0;
    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    for (i = 0; i < 22; ++i) s.breaks[i] = (size_t)((i + 1) * 10);
    s.breaks[22] = 0;
    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* enough breaks for levels >= 2 */
    for (i = 0; i < 80; ++i) s.breaks[i] = (size_t)((i + 1) * 5);
    s.breaks[80] = 0;
    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    for (i = 0; i < 80; ++i) s.breaks[i] = (size_t)((i + 1) * 5);
    s.breaks[80] = 0;
    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
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
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, k, r;

    for (i = 0; i < 200; ++i) s.breaks[i] = (size_t)((i + 1) * 5);
    s.breaks[200] = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 200);

    /* repeatedly markbreak from leftmost leaf to fill and overflow left-side
     * nodes */
    for (k = 0; k < 24; ++k) {
        lc_seek(&cur, c, 2);
        r = lc_markbreak(&cur, 2);
        assert(r == LC_OK);
    }

    lc_deltree(S, c);
    lc_close(S);
}

/* T37: markbreak noop — br == gap should be a no-op */
static void test_markbreak_noop(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    size_t     orig_bytes, orig_breaks;
    int        r;

    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 3);
    lc_seek(&cur, c, 5);
    orig_bytes = lc_bytes(c), orig_breaks = lc_breaks(c);
    r = lc_markbreak(&cur, lc_linelen(&cur)); /* br == gap */
    assert(r == LC_OK && lc_breaks(c) == orig_breaks
           && lc_bytes(c) == orig_bytes);

    lc_deltree(S, c);
    lc_close(S);
}

/* T38: markbreak br=0 — break at cursor position */
static void test_markbreak_brzero(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 3);
    lc_seek(&cur, c, 5); /* gap=5 (in first segment of 10 bytes) */
    r = lc_markbreak(&cur, 0);
    assert(r == LC_OK && lc_breaks(c) == 4);
    assert(lc_offset(&cur) == 5 && lc_line(&cur) == 1);

    lc_deltree(S, c);
    lc_close(S);
}

/* T39: markbreak cross-line — 4 empty lines (single leaf), extend mid line
 * to 100 */
static void test_markbreak_crossline(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    /* 4 lines, each 1 byte, total 4 bytes, 4 breaks (fits in 1 leaf) */
    for (i = 0; i < 4; ++i) s.breaks[i] = (size_t)((i + 1));
    s.breaks[4] = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 4 && lc_bytes(c) == 4);
    check_tree(c);

    /* line 1 (0-based) starts at offset 1, gap=1 */
    lc_seekline(&cur, c, 1);
    assert(lc_offset(&cur) == 1 && lc_line(&cur) == 1);
    r = lc_markbreak(&cur, 100);
    assert(r == LC_OK);
    check_tree(c);
    /* after: breaks at 1, 101; bytes=101 */
    assert(lc_breaks(c) == 2 && lc_bytes(c) == 101);
    assert(lc_offset(&cur) == 101 && lc_line(&cur) == 2);

    lc_deltree(S, c);
    lc_close(S);
}

/* T40: markbreak cross-line past end of file */
static void test_markbreak_crossline_end(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(10, 25, 40, 0);
    int        r;

    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK && lc_breaks(c) == 3 && lc_bytes(c) == 40);
    check_tree(c);

    /* seek to line 1 (offset 10), extend to 1000 bytes (past file end) */
    lc_seekline(&cur, c, 1);
    assert(lc_offset(&cur) == 10);
    r = lc_markbreak(&cur, 1000);
    assert(r == LC_OK);
    check_tree(c);
    /* break at 10 survives, new break at 10+1000=1010 */
    assert(lc_bytes(c) == 1010 && lc_breaks(c) == 2);

    lc_deltree(S, c);
    lc_close(S);
}

/* T41: markbreaks — set consecutive line lengths */
static void test_markbreaks(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    unsigned   brs[3];
    int        r;

    /* start with 3 breaks at 10, 25, 40 (bytes=40, breaks=3) */
    s.breaks[0] = 10, s.breaks[1] = 25, s.breaks[2] = 40, s.breaks[3] = 0;
    r = lc_scan(c, &test_scanner, &s);
    assert(r == LC_OK);
    check_tree(c);

    /* set line 1 length to 100 (crosses into line 2), line 2 to 30, line 3 to
     * 50 */
    lc_seekline(&cur, c, 1);
    brs[0] = 100, brs[1] = 30, brs[2] = 50;
    r = lc_markbreaks(&cur, brs, 3);
    assert(r == LC_OK);
    check_tree(c);
    /* line 0 = 10, line 1 = 100, line 2 = 30, line 3 = 50 → bytes =
     * 10+100+30+50 = 190 */
    assert(lc_bytes(c) == 190 && lc_breaks(c) == 4);

    lc_deltree(S, c);
    lc_close(S);
}

/* T42: markbreak cross-leaf: extend line across leaf boundary */
static void test_markbreak_crossleaf(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cache  *c = lc_newtree(S);
    lc_Cursor  cur;
    lc_ScanCtx s = init_scanner(0);
    int        i, r;

    for (i = 0; i < 8; ++i) s.breaks[i] = (size_t)((i + 1) * 1);
    s.breaks[8] = 0;
    r = lc_scan(c, &test_scanner, &s);
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
    memset(l->bytes, 0, LC_LEAF_FANOUT * sizeof(unsigned));
    va_start(ap, S);
    for (i = 0; i < n; i++) l->bytes[i] = va_arg(ap, unsigned);
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
    assert(c && root->child_count <= LC_FANOUT);
    c->levels = levels;
    c->root = *root;
    lc_poolfree(&S->nodes, root); /* root copy moved to cache, free template */
    c->bytes = 0;
    c->breaks = 0;
    for (i = 0; i < c->root.child_count; i++)
        c->bytes += c->root.bytes[i], c->breaks += c->root.breaks[i];
    return c;
}

/* ================================================================ */
/*  Phase 1: single-leaf spliceleaf tests (Examples 1-3 from plan)  */
/* ================================================================ */

/* Ex1a: 叶内不跨段 — [10,10], seek(5), del=3 → [7,10], bytes=17 */
static void test_spliceleaf_1a(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    size_t    del;

    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10)));
    check_tree(c);
    dump_tree(c, "1a initial");

    lc_seek(&cur, c, 5);
    assert(lc_offset(&cur) == 5);
    del = 3;
    lcD_spliceleaf(&cur, &del);
    fprintf(stderr, "1a del=%zu\n", del);
    dump_tree(c, "1a after spliceleaf");

    assert(del == 0);
    assert_tree(c, 0, botV(leafV(7, 10)));
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* Ex1b: 叶内跨段 — [10,10], seek(5), del=8 → [5,7], bytes=12 */
static void test_spliceleaf_1b(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    size_t    del;

    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10)));
    check_tree(c);

    lc_seek(&cur, c, 5);
    del = 8;
    lcD_spliceleaf(&cur, &del);
    fprintf(stderr, "1b del=%zu\n", del);
    dump_tree(c, "1b after spliceleaf");

    assert(del == 0);
    assert_tree(c, 0, botV(leafV(5, 7)));
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* Ex1c: 整段消失 — [5,10,5], seek(3), del=12 → [3,5], bytes=8 */
static void test_spliceleaf_1c(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    size_t    del;

    lc_Cache *c = cacheV(S, 0, botV(leafV(5, 10, 5)));
    check_tree(c);

    lc_seek(&cur, c, 3);
    del = 12;
    lcD_spliceleaf(&cur, &del);
    fprintf(stderr, "1c del=%zu\n", del);
    dump_tree(c, "1c after spliceleaf");

    assert(del == 0);
    assert_tree(c, 0, botV(leafV(3, 5)));
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* divergence: same leaf, C1/C2 in same leaf → l > levels */
static void test_divergence_same(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cursor  C1, C2;
    lc_Cache  *c = cacheV(S, 0, botV(leafV(10, 10)));

    lc_seek(&C1, c, 3);
    lc_seek(&C2, c, 7);
    assert(lcD_divergence(&C1, &C2) == (int)lcK_levels(&C1) + 1);
    lc_deltree(S, c);
    lc_close(S);
}

/* divergence: different leaves, same parent → diverge at levels */
static void test_divergence_leaf(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cursor  C1, C2;
    lc_Cache  *c = cacheV(S, 0, botV(leafV(10, 10), leafV(10, 10)));

    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 25);
    assert(lcD_divergence(&C1, &C2) == (int)lcK_levels(&C1));
    lc_deltree(S, c);
    lc_close(S);
}

/* divergence: different internal nodes (levels=2) → diverge at l=1 */
static void test_divergence_node(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cursor  C1, C2;
    lc_Cache  *c = cacheV(S, 2,
            innerV(innerV(botV(leafV(10)), botV(leafV(10)))));

    /* C1 in first botV (off=3), C2 in second botV (off=13) */
    lc_seek(&C1, c, 3);
    lc_seek(&C2, c, 13);
    assert(lcD_divergence(&C1, &C2) == 1);
    lc_deltree(S, c);
    lc_close(S);
}

/* prune: levels=1, C1 in L1, C2 in L4 → delete L2 L3, shrink to [L1,L4] */
static void test_prune_leaf(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cursor  C1, C2;
    lc_Node   *p;
    lc_Cache  *c = cacheV(S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10), leafV(10))));

    lc_seek(&C1, c, 3);
    lc_seek(&C2, c, 33);
    p = lcK_parent(&C1, 1);
    assert(p->child_count == 4);

    lcD_prune(&C1, &C2, 1);
    assert_tree(c, 1,
            innerV(botV(leafV(10), leafV(10))));
    assert(lcK_idx(&C2, p, 1) == 1);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* prune: levels=2, C1 in innerV0.botV0, C2 in innerV1.botV0 → delete middle */
static void test_prune_node(void) {
    lc_State  *S = lc_open(&test_alloc, NULL);
    lc_Cursor  C1, C2;
    lc_Cache  *c = cacheV(S, 2,
            innerV(innerV(botV(leafV(10)), botV(leafV(10))),
                   innerV(botV(leafV(10))),
                   innerV(botV(leafV(10)))));

    lc_seek(&C1, c, 3);
    lc_seek(&C2, c, 33);
    lcD_prune(&C1, &C2, 0);
    assert_tree(c, 2,
            innerV(innerV(botV(leafV(10)), botV(leafV(10))),
                   innerV(botV(leafV(10)))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* ================================================================ */
/*  Phase 2: cross-leaf freerange tests (Examples 4-6 from plan)    */
/* ================================================================ */

/* Ex4: 跨叶删除 — leaf0[10,10,10,10]+leaf1[10], seek(15), del=30
 * freerange → no_break=1, mergeleaf → [10,10], bytes=20 */
static void test_freerange_4(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    size_t    del = 30;
    int       no_break = 0;

    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10, 10, 10), leafV(10)));
    check_tree(c);
    dump_tree(c, "ex4 initial");

    lc_seek(&C1, c, 15);
    assert(lc_offset(&C1) == 15);
    C2 = C1;
    lc_advance(&C2, (ptrdiff_t)del);
    fprintf(stderr, "ex4 C1: off=%zu loff=%zu lidx=%u col=%u paths[0]=%p\n",
            lc_offset(&C1), (size_t)C1.loff, C1.lidx, C1.col,
            (void *)C1.paths[0]);
    fprintf(stderr, "ex4 C2: off=%zu loff=%zu lidx=%u col=%u paths[0]=%p\n",
            lc_offset(&C2), (size_t)C2.loff, C2.lidx, C2.col,
            (void *)C2.paths[0]);
    fprintf(stderr, "ex4 root.children[0]=%p [1]=%p\n",
            (void *)c->root.children[0], (void *)c->root.children[1]);

    lcD_freerange(&C1, &C2, &del, &no_break);
    fprintf(stderr, "ex4 after freerange: del=%zu no_break=%d\n", del,
            no_break);
    dump_tree(c, "ex4 after freerange");

    assert(del == 0 && no_break == 1 && c->bytes == 20 && c->breaks == 3);
    check_tree(c);

    lcD_mergeleaf(&C1, no_break);
    dump_tree(c, "ex4 after mergeleaf");
    assert(c->bytes == 20 && c->breaks == 2);
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* Ex5: 右叶首段半消费 — leaf0[10,10,10,10]+leaf1[10], seek(35), del=10
 * freerange → no_break=1, mergeleaf → [10,10,10,10], bytes=40 */
static void test_freerange_5(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    size_t    del = 10;
    int       no_break = 0;

    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10, 10, 10), leafV(10)));
    check_tree(c);

    lc_seek(&C1, c, 35);
    assert(lc_offset(&C1) == 35);
    C2 = C1;
    lc_advance(&C2, (ptrdiff_t)del);

    lcD_freerange(&C1, &C2, &del, &no_break);
    fprintf(stderr, "ex5 after freerange: del=%zu no_break=%d\n", del,
            no_break);
    dump_tree(c, "ex5 after freerange");

    assert(del == 0 && no_break == 1);
    check_tree(c);

    lcD_mergeleaf(&C1, no_break);
    dump_tree(c, "ex5 after mergeleaf");
    assert(c->bytes == 40 && c->breaks == 4);
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* Ex6: 右叶首段全消费+左叶段全消费 — leaf0[10,10,10,10]+leaf1[10],
 * seek(25), del=20 → no_break=1, mergeleaf → [10,10,10], bytes=30 */
static void test_freerange_6(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    size_t    del = 20;
    int       no_break = 0;

    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10, 10, 10), leafV(10)));
    check_tree(c);

    lc_seek(&C1, c, 25);
    assert(lc_offset(&C1) == 25);
    C2 = C1;
    lc_advance(&C2, (ptrdiff_t)del);

    lcD_freerange(&C1, &C2, &del, &no_break);
    fprintf(stderr, "ex6 after freerange: del=%zu no_break=%d\n", del,
            no_break);
    dump_tree(c, "ex6 after freerange");

    assert(del == 0 && no_break == 1);
    check_tree(c);

    lcD_mergeleaf(&C1, no_break);
    dump_tree(c, "ex6 after mergeleaf");
    assert(c->bytes == 30 && c->breaks == 3);
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}

/* ================================================================ */
/*  Phase 3: internal node merge tests                             */
/* ================================================================ */

/* merge_sufficient: 两 botV 各 cc=3 >= LC_FANOUT/2=2，merge 应空操作 */
static void test_merge_sufficient(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    lc_seek(&cur, c, 5);
    lcD_merge(&cur, 0);
    assert_tree(
            c, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    lc_deltree(S, c);
    lc_close(S);
}

/* merge_redistribute: botV0 cc=1 < 2, botV1 cc=4, total=5 > 4 → 均分 */
static void test_merge_redistribute(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10), leafV(10))));
    lc_seek(&cur, c, 5);
    lcD_merge(&cur, 0);
    assert_tree(
            c, 1,
            innerV(botV(leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    lc_deltree(S, c);
    lc_close(S);
}

/* merge_combine: botV0 cc=1, botV1 cc=1, root cc=3, total=2≤4 → 合并 */
static void test_merge_combine(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10)), botV(leafV(20)),
                   botV(leafV(30), leafV(40))));
    lc_seek(&cur, c, 5);
    lcD_merge(&cur, 0);
    assert_tree(
            c, 1,
            innerV(botV(leafV(10), leafV(20)), botV(leafV(30), leafV(40))));
    lc_deltree(S, c);
    lc_close(S);
}

/* merge_shrink_root: root 仅 2 botV 各 cc=1，合并后 root cc=1 → 根收缩 */
static void test_merge_shrink_root(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 1, innerV(botV(leafV(10)), botV(leafV(20))));
    lc_seek(&cur, c, 5);
    lcD_merge(&cur, 0);
    assert_tree(c, 0, botV(leafV(10), leafV(20)));
    lc_deltree(S, c);
    lc_close(S);
}

/* merge_combine_deep: levels=2, L1 级 combine 释内部节点子女 → use-after-free
 */
static void test_merge_combine_deep(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(10))),
                   innerV(botV(leafV(20)), botV(leafV(30))),
                   innerV(botV(leafV(40)), botV(leafV(50)))));
    lc_seek(&cur, c, 5);
    lcD_merge(&cur, 1);
    assert_tree(
            c, 2,
            innerV(innerV(botV(leafV(10)), botV(leafV(20)), botV(leafV(30))),
                   innerV(botV(leafV(40)), botV(leafV(50)))));
    lc_deltree(S, c);
    lc_close(S);
}

/* ================================================================ */
/*  Phase 4: trimnode / trimleaf tests                              */
/* ================================================================ */

/* trimnode_left: levels=1 root[4 botV], C in botV1 → keep [0..1], del right */
static void test_trimnode_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 1,
            innerV(botV(leafV(10,10)), botV(leafV(10,10)),
                   botV(leafV(10,10)), botV(leafV(10,10))));
    check_tree(c);
    lc_seek(&cur, c, 25);
    lcD_trimnode(&cur, 0, 1);
    assert_tree(c, 1, innerV(botV(leafV(10,10)), botV(leafV(10,10))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* trimnode_right: levels=1 root[4 botV], C in botV2 → keep [2..3], del left */
static void test_trimnode_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Node *root;
    lc_Cache *c = cacheV(S, 1,
            innerV(botV(leafV(10,10)), botV(leafV(10,10)),
                   botV(leafV(10,10)), botV(leafV(10,10))));
    check_tree(c);
    lc_seek(&cur, c, 45);
    root = &c->root;
    lcD_trimnode(&cur, 0, 0);
    assert_tree(c, 1, innerV(botV(leafV(10,10)), botV(leafV(10,10))));
    assert(cur.paths[0] == &root->children[0]);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* trimleaf_left: leaf[10,10,10,10], C at lidx=1,col=3 → keep [0..0]+3,del rest */
static void test_trimleaf_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10,10,10,10)));
    check_tree(c);
    lc_seek(&cur, c, 13);
    assert(cur.lidx == 1);
    lcD_trimleaf(&cur, 1);
    assert_tree(c, 0, botV(leafV(10,3)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* trimleaf_right: leaf[10,10,10,10], C at lidx=2,col=2 → del left [0..1]+2 bytes */
static void test_trimleaf_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10,10,10,10)));
    check_tree(c);
    lc_seek(&cur, c, 22);
    assert(cur.lidx == 2);
    lcD_trimleaf(&cur, 0);
    assert_tree(c, 0, botV(leafV(8,10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* lcL_append: append src leaf bytes[0..n-1] to dst at dst_segs */
static void test_lcL_append(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Leaf *src, *dst;
    unsigned n;
    src = (lc_Leaf *)lc_poolalloc(S, &S->leaves);
    dst = (lc_Leaf *)lc_poolalloc(S, &S->leaves);
    memset(src->bytes, 0, LC_LEAF_FANOUT * sizeof(unsigned));
    memset(dst->bytes, 0, LC_LEAF_FANOUT * sizeof(unsigned));
    src->bytes[0] = 10; src->bytes[1] = 20;
    dst->bytes[0] = 5; dst->bytes[1] = 15;
    n = lcL_append(dst, 2, src, 2);
    assert(n == 4);
    assert(dst->bytes[0] == 5 && dst->bytes[1] == 15
           && dst->bytes[2] == 10 && dst->bytes[3] == 20);
    lc_poolfree(&S->leaves, src);
    lc_poolfree(&S->leaves, dst);
    lc_close(S);
}

/* lcN_append: append src node children/bytes/breaks to dst */
static void test_lcN_append(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node *src, *dst;
    src = (lc_Node *)lc_poolalloc(S, &S->nodes);
    dst = (lc_Node *)lc_poolalloc(S, &S->nodes);
    memset(src, 0, sizeof(lc_Node));
    memset(dst, 0, sizeof(lc_Node));
    src->children[0] = (lc_Node *)(size_t)1; src->bytes[0] = 10; src->breaks[0] = 1;
    src->children[1] = (lc_Node *)(size_t)2; src->bytes[1] = 20; src->breaks[1] = 2;
    src->child_count = 2;
    dst->children[0] = (lc_Node *)(size_t)3; dst->bytes[0] = 5; dst->breaks[0] = 1;
    dst->child_count = 1;
    lcN_append(dst, src, 2);
    assert(dst->child_count == 3);
    assert(dst->children[0] == (lc_Node *)(size_t)3);
    assert(dst->bytes[0] == 5 && dst->breaks[0] == 1);
    assert(dst->children[1] == (lc_Node *)(size_t)1);
    assert(dst->bytes[1] == 10 && dst->breaks[1] == 1);
    assert(dst->children[2] == (lc_Node *)(size_t)2);
    assert(dst->bytes[2] == 20 && dst->breaks[2] == 2);
    lc_poolfree(&S->nodes, src);
    lc_poolfree(&S->nodes, dst);
    lc_close(S);
}

/* mergeleaf2_sufficient: 2 leaves with 3 segs each (≥2) → no-op, return 0 */
static void test_mergeleaf2_sufficient(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    int ret;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10,10,10), leafV(10,10,10)));
    check_tree(c);
    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 35);
    ret = lcD_mergeleaf2(&C1, &C2);
    assert(ret == 0);
    assert_tree(c, 0, botV(leafV(10,10,10), leafV(10,10,10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergeleaf2_combine: 2 leaves 1 seg each → merge, return 1 */
static void test_mergeleaf2_combine(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    int ret;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10), leafV(20)));
    check_tree(c);
    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 15);
    ret = lcD_mergeleaf2(&C1, &C2);
    assert(ret == 1);
    assert_tree(c, 0, botV(leafV(10,20)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergeleaf2_redistribute: left 1 seg, right 4 segs → redistribute, return 0 */
static void test_mergeleaf2_redistribute(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    int ret;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10), leafV(10,10,10,10)));
    check_tree(c);
    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 15);
    ret = lcD_mergeleaf2(&C1, &C2);
    assert(ret == 0);
    assert_tree(c, 0, botV(leafV(10,10), leafV(10,10,10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergenode_sufficient: 2 botV with 3 leaves each (≥2) → no-op, return 0 */
static void test_mergenode_sufficient(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    int ret;
    lc_Cache *c = cacheV(S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    check_tree(c);
    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 35);
    ret = lcD_mergenode(&C1, &C2, 0);
    assert(ret == 0);
    assert_tree(c, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergenode_combine: 2 botV 1 leaf each → merge, return 1 */
static void test_mergenode_combine(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    int ret;
    lc_Cache *c = cacheV(S, 1,
            innerV(botV(leafV(10)), botV(leafV(20))));
    check_tree(c);
    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 15);
    ret = lcD_mergenode(&C1, &C2, 0);
    assert(ret == 1);
    assert_tree(c, 1, innerV(botV(leafV(10), leafV(20))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* mergenode_redistribute: left cc=1, right cc=4 → redistribute, return 0 */
static void test_mergenode_redistribute(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    int ret;
    lc_Cache *c = cacheV(S, 1,
            innerV(botV(leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10), leafV(10))));
    check_tree(c);
    lc_seek(&C1, c, 5);
    lc_seek(&C2, c, 15);
    ret = lcD_mergenode(&C1, &C2, 0);
    assert(ret == 0);
    assert_tree(c, 1,
            innerV(botV(leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* rebalance_noop: tree with sufficient children → no change */
static void test_rebalance_noop(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    check_tree(c);
    lc_seek(&cur, c, 5);
    lcD_rebalance(&cur, 0);
    assert_tree(c, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10)),
                   botV(leafV(10), leafV(10), leafV(10))));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* rebalance_shrink: levels=1 root with 1 botV → shrink to levels=0 */
static void test_rebalance_shrink(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 1, innerV(botV(leafV(10), leafV(20))));
    check_tree(c);
    lc_seek(&cur, c, 5);
    lcD_rebalance(&cur, 0);
    assert_tree(c, 0, botV(leafV(10), leafV(20)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* rebalance_double: levels=2 root with 1 child → double shrink to levels=0 */
/* checktree: exercise lcD_checktree to silence -Wunused-function */
static void test_checktree(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(S, 0, botV(leafV(10), leafV(20)));
    lcD_checktree(c, "test_checktree");
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

static void test_rebalance_double(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 2, innerV(innerV(botV(leafV(10)))));
    check_tree(c);
    lc_seek(&cur, c, 5);
    lcD_rebalance(&cur, 0);
    assert_tree(c, 0, botV(leafV(10)));
    check_tree(c);
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
    lc_Cache *c = cacheV(S, 0,
            botV(leafV(10,10,10), leafV(10,10,10)));
    check_tree(c);
    lc_seek(&C1, c, 20);
    lc_seek(&C2, c, 40);
    lcD_splicerange(&C1, &C2);
    assert_tree(c, 0, botV(leafV(10,10), leafV(10,10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* splicerange_prune: levels=0 4 leaves, C1 in leaf0 C2 in leaf3, middle freed */
static void test_splicerange_prune(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(S, 0,
            botV(leafV(10,10), leafV(10,10),
                 leafV(10,10), leafV(10,10)));
    check_tree(c);
    lc_seek(&C1, c, 10);
    lc_seek(&C2, c, 70);
    lcD_splicerange(&C1, &C2);
    assert_tree(c, 0, botV(leafV(10,10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* splicerange_all: delete entire tree via lc_splice */
static void test_splicerange_all(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10), leafV(20)));
    check_tree(c);
    lc_seek(&cur, c, 0);
    lc_splice(&cur, 30, 0);
    assert(c->bytes == 0 && c->breaks == 0);
    assert(c->root.child_count == 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* main */

#define TESTS(X)                   \
    X(lifecycle)                   \
    X(scan_seek)                   \
    X(seekline)                    \
    X(advance_single)              \
    X(advance_cross)               \
    X(node_split)                  \
    X(seek_past_leaf)              \
    X(advance_backward_cross)      \
    X(advline_cross)               \
    X(advline_backward_within)     \
    X(advance_skip_siblings)       \
    X(node_split_cursor_right)     \
    X(splitleaf_left)              \
    X(rootsplit_left)              \
    X(findlines_skip)              \
    X(rootsplit_left_deep)         \
    X(nodesplit_left)              \
    X(findlines_skip_deep)         \
    X(backwardoff_skip)            \
    X(cov_remaining)               \
    X(clearbreaks)                 \
    X(clearbreaks_edge)            \
    X(clearbreaks_slot)            \
    X(splice_tmp)                  \
    X(splice_l2)                   \
    X(splice)                      \
    X(splice_trailing)             \
    X(splice_cross_breaks)         \
    X(splice_cross_breaks_slot)    \
    X(splice_cross_breaks_mid)     \
    X(markbreak)                   \
    X(markbreaks)                  \
    X(markbreak_split)             \
    X(markbreak_empty)             \
    X(markbreak_noop)              \
    X(markbreak_trailing)          \
    X(markbreak_brzero)            \
    X(markbreak_crossline)         \
    X(markbreak_crossline_end)     \
    X(markbreak_node_split)        \
    X(markbreak_root_split)        \
    X(markbreak_split_right)       \
    X(markbreak_root_split_on_add) \
    X(spliceleaf_1a)                \
    X(spliceleaf_1b)                \
    X(spliceleaf_1c)                \
    X(divergence_same)              \
    X(divergence_leaf)              \
    X(divergence_node)              \
    X(prune_leaf)                   \
    X(prune_node)                   \
    X(freerange_4)                 \
    X(freerange_5)                 \
    X(freerange_6)                 \
    X(markbreak_crossleaf)         \
    X(merge_sufficient)            \
    X(merge_redistribute)          \
    X(merge_combine)               \
    X(merge_shrink_root)           \
    X(merge_combine_deep)          \
    X(trimnode_left)               \
    X(trimnode_right)              \
    X(trimleaf_left)               \
    X(trimleaf_right)              \
    X(lcL_append)                  \
    X(lcN_append)                  \
    X(mergeleaf2_sufficient)       \
    X(mergeleaf2_combine)          \
    X(mergeleaf2_redistribute)     \
    X(mergenode_sufficient)        \
    X(mergenode_combine)           \
    X(mergenode_redistribute)       \
    X(checktree)                   \
    X(rebalance_noop)               \
    X(rebalance_shrink)             \
    X(rebalance_double)             \
    X(splicerange_2leaf)           \
    X(splicerange_prune)           \
    X(splicerange_all)

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
