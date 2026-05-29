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

#include "linecache.h"

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

LC_STATIC int oom_cnt;

LC_STATIC void *oom_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    if (--oom_cnt == 0) return NULL;
    return realloc(ptr, nsize);
}

/* ================================================================ */
/*  tree invariant checker                                           */
/* ================================================================ */

LC_STATIC size_t lc_checknode(
        const lc_Node *n, int l, int levels, int allow_empty) {
    int    i;
    size_t bsum = 0, lsum = 0;
    assert(allow_empty || n->child_count > 0);
    assert(n->child_count <= LC_FANOUT);
    for (i = 0; i < (int)n->child_count; ++i) {
        if (l == levels || levels == 0) {
            assert(n->breaks[i] <= LC_LEAF_FANOUT);
            bsum += n->bytes[i], lsum += n->breaks[i];
        } else {
            size_t cb = lc_checknode(
                    n->children[i], l + 1, levels, allow_empty);
            assert(cb == n->bytes[i]);
            bsum += cb, lsum += n->breaks[i];
        }
    }
    (void)lsum;
    return bsum;
}

LC_STATIC void lc_checktree_allow_empty(const lc_Cache *c, int allow_empty) {
    if (c->root.child_count == 0) {
        assert(c->bytes == 0 && c->breaks == 0);
        return;
    }
    lc_checknode(&c->root, 0, c->levels, allow_empty);
    assert(c->bytes == lcN_sumbytes(&c->root, 0, c->root.child_count));
    assert(c->breaks == lcN_sumbreaks(&c->root, 0, c->root.child_count));
}

LC_STATIC void lc_checktree(const lc_Cache *c) {
    lc_checktree_allow_empty(c, 0);
}

/* ================================================================ */
/*  tree dump                                                        */
/* ================================================================ */

LC_STATIC void lc_dumpnode(const lc_Node *n, int l, int levels) {
    unsigned i, cc = n->child_count;
    fprintf(stderr, "%*sL%u=%p cc=%u", l * 2, "", l, (void *)n, cc);
    for (i = 0; i < cc; ++i)
        fprintf(stderr, " b[%u]=%zu l[%u]=%zu", i, n->bytes[i], i,
                (size_t)n->breaks[i]);
    fprintf(stderr, "\n");
    if (l == levels || levels == 0) {
        for (i = 0; i < cc; ++i) {
            lc_Leaf *leaf = (lc_Leaf *)n->children[i];
            unsigned s, sc = (unsigned)n->breaks[i];
            fprintf(stderr, "%*sL%u leaf[%u]=%p segs=%u bytes:", (l + 1) * 2,
                    "", l + 1, i, (void *)leaf, sc);
            for (s = 0; s < sc; ++s) fprintf(stderr, " %u", leaf->bytes[s]);
            fprintf(stderr, "\n");
        }
    } else {
        for (i = 0; i < cc; ++i) lc_dumpnode(n->children[i], l + 1, levels);
    }
}

LC_STATIC void lc_dumptree(const lc_Cache *c, const char *tag) {
    fprintf(stderr,
            "=== lc_dumptree %s: levels=%u root.cc=%u bytes=%zu breaks=%zu "
            "===\n",
            tag, c->levels, c->root.child_count, c->bytes, c->breaks);
    lc_dumpnode(&c->root, 0, c->levels);
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
    l = (lc_Leaf *)lc_poolalloc(S, &S->leaves);
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

LC_STATIC lc_Node *innerV_(lc_State *S, ...) {
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

LC_STATIC lc_Cache *cacheV(lc_State *S, unsigned levels, lc_Node *root) {
    lc_Cache *c = lc_newtree(S);
    unsigned  i;
    assert(c && root->child_count <= LC_FANOUT);
    c->levels = levels;
    c->root = *root;
    lc_poolfree(&S->nodes, root);
    c->bytes = 0;
    c->breaks = 0;
    for (i = 0; i < c->root.child_count; i++)
        c->bytes += c->root.bytes[i], c->breaks += c->root.breaks[i];
    lc_checktree_allow_empty(c, 1);
    return c;
}

/* ================================================================ */
/*  assert_tree — build expected tree and compare                    */
/* ================================================================ */

#define assert_tree(c, lvls, ...)                                      \
    do {                                                               \
        lc_Cache *__d = cacheV(S, lvls, __VA_ARGS__);                  \
        if (!lc_comparetree((c), __d)) {                               \
            fprintf(stderr, "assert_tree FAILED at %s:%d\n", __FILE__, \
                    __LINE__);                                         \
            fprintf(stderr, "Expected:\n");                            \
            lc_dumptree(__d, "expected");                              \
            fprintf(stderr, "Actual:\n");                              \
            lc_dumptree((c), "actual");                                \
            assert(0 && "assert_tree failed");                         \
        }                                                              \
        lc_deltree(S, __d);                                            \
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
    printf("=== %s ===\n", banner);
    if (argc == 1) {
        const lc_test_entry *e = entries;
        while (e->name) {
            printf("- %s\n", e->name);
            e->fn();
            printf("  %s OK\n", e->name);
            ++e;
        }
        printf("\nAll tests passed!\n");
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

#define LC_TEST_MAIN(banner)                                    \
    int main(int argc, char *argv[]) {                          \
        static const lc_test_entry _test_entries[] = {          \
                TESTS(X){NULL, NULL},                           \
        };                                                      \
        return lc_test_main(banner, _test_entries, argc, argv); \
    }

#endif /* LC_TESTS_H */
