/*
 * lc_tests.h — shared test utilities for linecache tests
 *
 * Usage: #include "lc_tests.h" after defining LC_FANOUT / LC_PAGE_SIZE
 *        and after #include "linecache.h".  Define TESTS(X) macro with
 *        all test entries, then call LC_TEST_MAIN().
 */

#ifndef LC_TESTS_H
#define LC_TESTS_H

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define lc_log(...) fprintf(stderr, __VA_ARGS__)

#define lc_check(e, ...)                         \
    do {                                         \
        if (!(e)) return lc_log(__VA_ARGS__), 0; \
    } while (0)

#include "linecache.h"

LC_STATIC void lc_dumptree(const lc_Cache *c, const char *tag);
LC_STATIC void lc_dumpcursor(const lc_Cursor *C, const char *tag);

/* ================================================================ */
/*  allocators                                                       */
/* ================================================================ */

LC_STATIC void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nsize);
}

LC_STATIC void *oom_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    int *cnt = (int *)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    if (!cnt || *cnt <= 0) return NULL;
    (*cnt)--;
    return realloc(ptr, nsize);
}

/* lc_localfill — fill pool freelist with count objects from a local buffer.
 *   pool->freed is set to point to the first object in buf.
 *   buf must hold count * pool->obj_size bytes.
 *   Caller must ensure buf outlives the pool usage. */
LC_STATIC void lc_localfill(lc_Pool *pool, void **op, void *buf, size_t count) {
    size_t i;
    size_t sz = pool->obj_size;
    char  *base = (char *)buf;
    assert(count > 0 && sz > sizeof(void *));
    *op = pool->freed;
    for (i = 1; i < count; ++i)
        *(void **)(base + (i - 1) * sz) = (void *)(base + i * sz);
    *(void **)(base + (count - 1) * sz) = NULL;
    pool->freed = (void *)base;
    lcP_stat(pool->live_obj += count);
}

/* ================================================================ */
/*  tree invariant checker                                           */
/* ================================================================ */

LC_STATIC int lc_checknode(const lc_Node *n, int rl, int mc) {
    size_t bsum, lsum;
    int    i;
    (void)bsum, (void)lsum;
    lc_check(
            n->child_count >= mc, "[chk] N[%p] rl=%d cc=%d<%d\n", (void *)n, rl,
            n->child_count, mc);
    lc_check(
            n->child_count <= LC_FANOUT, "[chk] N[%p] rl=%d cc=%d>%d\n",
            (void *)n, rl, n->child_count, LC_FANOUT);
    for (i = 0; i < (int)n->child_count; ++i) {
        lc_Node *c = n->children[i];
        if (rl == 0) {
            bsum = lcL_sumbytes((lc_Leaf *)c, 0, n->breaks[i]);
            lc_check(
                    n->breaks[i] >= LC_LEAF_FANOUT / 2
                            && n->breaks[i] <= LC_LEAF_FANOUT,
                    "[chk] LEAF rl=%d i=%d cc=%d brs=%zu bytes=%zu leaf=%p\n",
                    rl, i, n->child_count, n->breaks[i], n->bytes[i],
                    (void *)c);
            lc_check(
                    n->bytes[i] == bsum,
                    "[chk] BOTV rl=%d i=%d cc=%d brs=%zu bytes=%zu sum=%zu "
                    "leaf=%p\n",
                    rl, i, n->child_count, n->breaks[i], n->bytes[i], bsum,
                    (void *)c);
        } else {
            if (!lc_checknode(c, rl - 1, mc ? LC_FANOUT / 2 : 0)) return 0;
            bsum = lcN_sumbytes(c, 0, c->child_count);
            lsum = lcN_sumbreaks(c, 0, c->child_count);
            lc_check(
                    n->bytes[i] == bsum && n->breaks[i] == lsum,
                    "[chk] INNER rl=%d i=%d cc=%d bytes=%zu sum=%zu "
                    "brs=%zu sum=%zu node=%p\n",
                    rl, i, n->child_count, n->bytes[i], bsum, n->breaks[i],
                    lsum, (void *)c);
        }
    }
    return 1;
}

LC_STATIC int lc_checktree_allow_empty(const lc_Cache *c, int allow_empty) {
    size_t bsum, lsum;
    if (c->root.child_count == 0) {
        lc_check(
                c->bytes == 0 && c->breaks == 0,
                "[chk] EMPTY tree has bytes=%zu brs=%zu\n", c->bytes,
                c->breaks);
    } else if (c->levels > 0 || c->root.child_count > 1)
        return lc_checknode(&c->root, c->levels, allow_empty ? 0 : 1);
    else {
        lc_Leaf *lf = (lc_Leaf *)c->root.children[0];
        bsum = lcL_sumbytes(lf, 0, c->root.breaks[0]);
        lc_check(
                c->root.bytes[0] == bsum,
                "[chk] SINGLE LEAF tree has breaks=%zu bytes=%zu sum=%zu\n",
                c->root.breaks[0], c->root.bytes[0], bsum);
    }
    bsum = lcN_sumbytes(&c->root, 0, c->root.child_count);
    lsum = lcN_sumbreaks(&c->root, 0, c->root.child_count);
    lc_check(
            c->bytes == bsum && c->breaks == lsum,
            "[chk] ROOT bytes=%zu sum=%zu brs=%zu sum=%zu\n", c->bytes, bsum,
            c->breaks, lsum);
    return 1;
}

LC_STATIC int lc_checktree(const lc_Cache *c) {
    return lc_checktree_allow_empty(c, 0);
}

/* ================================================================ */
/*  cursor invariant checker                                         */
/* ================================================================ */

LC_STATIC int lc_checkcursor(lc_Cursor *C, size_t expected_off) {
    size_t   bsum = 0, lsum = 0;
    int      i, l;
    lc_Node *p;
    lc_check(
            lc_offset(C) == expected_off,
            "[chk] OFFSET mismatch off=%zu expected=%zu\n", lc_offset(C),
            expected_off);
    if (C->tree->root.child_count == 0) {
        lc_check(
                C->off == 0 && C->nu == 0 && C->loff == 0 && C->lnu == 0,
                "[chk] EMPTY off=%zu nu=%zu loff=%zu lnu=%hu\n", C->off, C->nu,
                C->loff, C->lnu);
        lc_check(
                C->paths[0] == &C->tree->root.children[0],
                "[chk] EMPTY paths[0]=%p expected=%p\n", (void *)C->paths[0],
                (void *)&C->tree->root.children[0]);
        return 1;
    }
    for (l = 0; l <= lcK_levels(C); ++l) {
        p = lcK_parent(C, l), i = lcK_idx(C, p, l);
        lc_check(
                i >= 0 && i < (int)p->child_count,
                "[chk] PATHS[%d] invalid idx=%d cc=%u\n", l, i, p->child_count);
        lc_check(
                C->paths[l] == &p->children[i],
                "[chk] PATHS[%d] invalid ptr=%p expected=%p\n", l,
                (void *)C->paths[l], (void *)&p->children[i]);
        bsum += lcN_sumbytes(p, 0, i);
        lsum += lcN_sumbreaks(p, 0, i);
    }
    lc_check(
            C->off == bsum, "[chk] OFF mismatch off=%zu sum=%zu\n", C->off,
            bsum);
    lc_check(
            C->nu == lsum + C->lnu, "[chk] NU mismatch nu=%zu sum=%zu\n", C->nu,
            lsum);
    p = lcK_parent(C, lcK_levels(C)), i = lcK_idx(C, p, lcK_levels(C));
    bsum = lcL_sumbytes(lcK_leaf(C), 0, C->lnu);
    lc_check(
            C->loff == bsum, "[chk] LOFF mismatch loff=%zu sum=%zu lnu=%hu\n",
            C->loff, bsum, C->lnu);
    lc_check(
            C->lnu <= p->breaks[i], "[chk] LNU out of bounds lnu=%hu brs=%zu\n",
            C->lnu, p->breaks[i]);
    lc_check(
            C->lnu == p->breaks[i] || C->col < lcK_leaf(C)->bytes[C->lnu],
            "[chk] COL out of bounds col=%u line_bytes=%u\n", C->col,
            lcK_leaf(C)->bytes[C->lnu]);
    return 1;
}

/* ================================================================ */
/*  tree dump                                                        */
/* ================================================================ */

LC_STATIC void lc_dumpnode(const lc_Node *n, int idx, int l, int levels) {
    unsigned i, cc = n->child_count;
    if (l == 0)
        lc_log("Root(%p) cc=%u", (void *)n, cc);
    else
        lc_log("%*sN%u_%u(%p) cc=%u", l * 2, "", l - 1, idx, (void *)n, cc);
    for (i = 0; i < cc; ++i)
        lc_log(" b[%u]=%zu l[%u]=%zu", i, n->bytes[i], i, n->breaks[i]);
    lc_log("\n");
    if (l == levels || levels == 0) {
        for (i = 0; i < cc; ++i) {
            lc_Leaf *leaf = (lc_Leaf *)n->children[i];
            unsigned s, sc = (unsigned)n->breaks[i];
            lc_log("%*sL%u leaf[%u]=%p segs=%u bytes:", (l + 1) * 2, "", l + 1,
                   i, (void *)leaf, sc);
            for (s = 0; s < sc; ++s) lc_log(" %u", leaf->bytes[s]);
            lc_log("\n");
        }
    } else {
        for (i = 0; i < cc; ++i) lc_dumpnode(n->children[i], i, l + 1, levels);
    }
}

LC_STATIC void lc_dumptree(const lc_Cache *c, const char *tag) {
    lc_log("[TREE]\t %s: levels=%u root.cc=%u bytes=%zu breaks=%zu\n", tag,
           c->levels, c->root.child_count, c->bytes, c->breaks);
    lc_dumpnode(&c->root, -1, 0, c->levels);
}

LC_STATIC void lc_dumpcursor(const lc_Cursor *C, const char *tag) {
    int l;
    lc_log("[CURSOR] %s: off=%zu nu=%zu loff=%zu lnu=%hu col=%u\n", tag, C->off,
           C->nu, C->loff, C->lnu, C->col);
    for (l = 0; l <= lcK_levels(C); ++l) {
        lc_Node *p = lcK_parent(C, l);
        int      i = lcK_idx(C, p, l);
        lc_log("  paths[%d]=%p p(%p)[%d/%u]=%p b=%zu l=%zu\n", l,
               (void *)C->paths[l], (void *)p, i, p->child_count,
               (void *)*C->paths[l], p->bytes[i], p->breaks[i]);
    }
}

/* ================================================================ */
/*  tree comparison                                                  */
/* ================================================================ */

LC_STATIC int lc_comparenode(
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
        } else if (
                !lc_comparenode(a->children[i], b->children[i], l + 1, levels))
            return 0;
    }
    return 1;
}

LC_STATIC int lc_comparetree(const lc_Cache *a, const lc_Cache *b) {
    if (a->levels != b->levels) return 0;
    if (a->bytes != b->bytes) return 0;
    if (a->breaks != b->breaks) return 0;
    if (a->root.child_count != b->root.child_count) return 0;
    if (a->root.child_count == 0) return 1;
    return lc_comparenode(&a->root, &b->root, 0, a->levels);
}

/* ================================================================ */
/*  leaf sequence checker                                            */
/* ================================================================ */
/* Verify leaf break bytes match expected rle pairs {count,value,..,0} */

LC_STATIC int lc_checkleaves_rec(
        const lc_Node *n, int l, int levels, unsigned **brs) {
    int i;
    if (l == levels || levels == 0) {
        for (i = 0; i < (int)n->child_count; i++) {
            const lc_Leaf *lf;
            int            j;
            lf = (const lc_Leaf *)n->children[i];
            for (j = 0; j < (int)n->breaks[i]; j++) {
                unsigned *p;
                p = *brs;
                if (p[0] == 0) return 0;
                if (p[1] != lf->bytes[j]) return 0;
                if (--p[0] == 0) *brs = p + 2;
            }
        }
    } else {
        for (i = 0; i < (int)n->child_count; i++)
            if (!lc_checkleaves_rec(n->children[i], l + 1, levels, brs))
                return 0;
    }
    return 1;
}

LC_STATIC int lc_checkleaves(const lc_Cache *c, unsigned **brs) {
    if (c->root.child_count == 0) return (**brs == 0);
    if (!lc_checkleaves_rec(&c->root, 0, (int)c->levels, brs)) return 0;
    return (**brs == 0);
}

#define checkleavesV(c, ...)                                            \
    do {                                                                \
        unsigned  brs__[] = {__VA_ARGS__, 0};                           \
        unsigned *pbrs__ = brs__;                                       \
        if (!lc_checkleaves((c), &pbrs__)) {                            \
            fprintf(stderr, "checkleavesV FAILED at %s:%d\n", __FILE__, \
                    __LINE__);                                          \
            lc_dumptree((c), "checkleavesV failed");                    \
            assert(0 && "checkleavesV failed");                         \
        }                                                               \
    } while (0)

/* ================================================================ */
/*  tree construction helpers (leafV / botV / innerV / cacheV)       */
/* ================================================================ */

#define leafV(...)  leafV_(S, __VA_ARGS__, 0)
#define botV(...)   botV_(S, __VA_ARGS__, NULL)
#define innerV(...) innerV_(S, __VA_ARGS__, NULL)

LC_STATIC lc_Node *leafV_(lc_State *S, ...) {
    va_list  ap;
    lc_Leaf *l;
    unsigned n = 0, i;
    va_start(ap, S);
    while (va_arg(ap, unsigned) != 0) n++;
    va_end(ap);
    l = (lc_Leaf *)lcP_alloc(S, &S->leaves);
    assert(l && n <= LC_LEAF_FANOUT);
    va_start(ap, S);
    for (i = 0; i < n; i++) l->bytes[i] = va_arg(ap, unsigned);
    if (n < LC_LEAF_FANOUT) l->bytes[n] = 0;
    va_end(ap);
    return (lc_Node *)l;
}

LC_STATIC lc_Node *botV_(lc_State *S, ...) {
    va_list  ap;
    lc_Node *n;
    unsigned cc = 0, i;
    va_start(ap, S);
    while (va_arg(ap, lc_Node *) != NULL) cc++;
    va_end(ap);
    n = (lc_Node *)lcP_alloc(S, &S->nodes);
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

LC_STATIC lc_Node *innerV_(lc_State *S, ...) {
    va_list  ap;
    lc_Node *n;
    unsigned cc = 0, i;
    va_start(ap, S);
    while (va_arg(ap, lc_Node *) != NULL) cc++;
    va_end(ap);
    n = (lc_Node *)lcP_alloc(S, &S->nodes);
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

LC_STATIC lc_Cache *cacheV(lc_State *S, unsigned levels, lc_Node *root) {
    lc_Cache *c = lc_newtree(S);
    unsigned  i;
    assert(c && root->child_count <= LC_FANOUT);
    c->levels = levels;
    c->root = *root;
    lcP_free(&S->nodes, root);
    c->bytes = 0;
    c->breaks = 0;
    for (i = 0; i < c->root.child_count; i++)
        c->bytes += c->root.bytes[i], c->breaks += c->root.breaks[i];
    lc_checktree_allow_empty(c, 1);
    return c;
}

/* ================================================================ */
/*  lc_asserttree — build expected tree and compare                    */
/* ================================================================ */

#define lc_asserttree(c, lvls, ...)                                      \
    do {                                                                 \
        lc_Cache *__d = cacheV(S, lvls, __VA_ARGS__);                    \
        if (!lc_comparetree((c), __d)) {                                 \
            fprintf(stderr, "lc_asserttree FAILED at %s:%d\n", __FILE__, \
                    __LINE__);                                           \
            fprintf(stderr, "Expected:\n");                              \
            lc_dumptree(__d, "expected");                                \
            fprintf(stderr, "Actual:\n");                                \
            lc_dumptree((c), "actual");                                  \
            assert(0 && "lc_asserttree failed");                         \
        }                                                                \
        lc_deltree(S, __d);                                              \
    } while (0)

/* ================================================================ */
/*  scanner utilities                                                */
/* ================================================================ */

#define lc_scanV(c, ...)                                \
    do {                                                \
        unsigned brs[] = {__VA_ARGS__, 0}, *pbrs = brs; \
        int      r = lc_scan(c, lc_scanner, &pbrs);     \
        assert(r == LC_OK);                             \
    } while (0)

#define lc_rscanV(c, ...)                               \
    do {                                                \
        unsigned brs[] = {__VA_ARGS__, 0}, *pbrs = brs; \
        int      r = lc_scan(c, lc_rscanner, &pbrs);    \
        assert(r == LC_OK);                             \
    } while (0)

/* lc_scanner: read unsigned values one by one until 0.
 *   unsigned brs[] = {10, 15, 15, 0}, *p = brs;
 *   lc_scan(c, lc_scanner, &p); */
LC_STATIC unsigned lc_scanner(void *ud, size_t prev) {
    unsigned **p = (unsigned **)ud;
    (void)prev;
    if (!p || !*p) return 0;
    return *(*p)++;
}

/* lc_rscanner: read [count, value] pairs, repeat value count times.
 * Terminates when count == 0.
 *   unsigned brs[] = {10, 1, 0}, *p = brs;
 *   lc_scan(c, lc_rscanner, &p);   // 10 breaks of 1 byte each
 *
 * Note: modifies brs[] array in-place (decrements counts). Between
 * multiple lc_scan calls on the same array, reinitialize p or the
 * array contents. */
LC_STATIC unsigned lc_rscanner(void *ud, size_t prev) {
    unsigned **p = (unsigned **)ud;
    unsigned  *cur;
    (void)prev;
    if (!p || !*p) return 0;
    cur = *p;
    if (cur[0] == 0) return 0;
    if (--cur[0] == 0) *p = cur + 2;
    return cur[1];
}

/* ================================================================ */
/*  test runner                                                      */
/* ================================================================ */
/* Usage: after #defining TESTS(X) with all test entries, write:
 *
 *   #define X(name) {#name, test_##name},
 *   LC_TEST_MAIN("my tests")
 *   #undef X
 *
 * LC_TEST_MAIN constructs the entries table and the main function.  */

typedef struct {
    const char *name;
    void (*fn)(void);
} lc_test_entry;

LC_STATIC int lc_test_main(
        const char *banner, const lc_test_entry *entries, int argc,
        char *argv[]) {
    int i, j;
    lc_log("=== %s ===\n", banner);
    if (argc == 1) {
        const lc_test_entry *e = entries;
        while (e->name) {
            lc_log("- %s\n", e->name);
            e->fn();
            lc_log("  %s OK\n", e->name);
            ++e;
        }
        lc_log("\nAll tests passed!\n");
        return 0;
    }
    for (i = 1; i < argc; ++i) {
        const char *name = argv[i];
        size_t      len = strlen(name);
        int         found = 0, only = *name == '@';
        if (only) name++, len--;
        for (j = 0; entries[j].name; ++j) {
            if (strlen(entries[j].name) >= len
                && strncmp(name, entries[j].name, len) == 0) {
                lc_log("- %s\n", entries[j].name);
                entries[j].fn();
                lc_log("  %s OK\n", entries[j].name);
                found = 1;
                if (only) break;
            }
        }
        if (!found) {
            lc_log("Unknown test: %s\n", name);
            return 1;
        }
    }
    return 0;
}

#define LC_TEST_MAIN(banner)                                    \
    int main(int argc, char *argv[]) {                          \
        static const lc_test_entry _test_entries[] = {          \
                TESTS(X){NULL, NULL},                           \
        };                                                      \
        return lc_test_main(banner, _test_entries, argc, argv); \
    }

#endif /* LC_TESTS_H */
