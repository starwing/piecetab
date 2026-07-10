/*
 * pt_tests.h — shared test utilities for piecetab tests
 *
 * Usage: #include "pt_tests.h" after defining PT_FANOUT / PT_PAGE_SIZE
 *        and after #include "piecetab.h".  Define TESTS(X) macro with
 *        all test entries, then call PT_TEST_MAIN().
 */

#ifndef PT_TESTS_H
#define PT_TESTS_H

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define pt_log(...) fprintf(stderr, __VA_ARGS__)

#define pt_check(e, ...)                         \
    do {                                         \
        if (!(e)) return pt_log(__VA_ARGS__), 0; \
    } while (0)

#include "piecetab.h"

PT_STATIC void pt_dumptree(pt_Blob snap, const char *tag);
PT_STATIC void pt_dumpcursor(const pt_Cursor *C, const char *tag);

/* ================================================================ */
/*  allocators                                                       */
/* ================================================================ */

PT_STATIC void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nsize);
}

PT_STATIC void *oom_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
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

/* pt_localfill — fill pool freelist with count objects from a local buffer.
 *   pool->freed is set to point to the first object in buf.
 *   buf must hold count * pool->obj_size bytes.
 *   Caller must ensure buf outlives the pool usage. */
PT_STATIC void pt_localfill(pt_Pool *pool, void **op, void *buf, size_t count) {
    size_t i;
    size_t sz = pool->obj_size;
    char  *base = (char *)buf;
    assert(count > 0 && sz > sizeof(void *));
    *op = pool->freed;
    for (i = 1; i < count; ++i)
        *(void **)(base + (i - 1) * sz) = (void *)(base + i * sz);
    *(void **)(base + (count - 1) * sz) = NULL;
    pool->freed = (void *)base;
    ptP_stat(pool->live_obj += count);
}

/* ================================================================ */
/*  tree invariant checker                                           */
/* ================================================================ */

PT_STATIC int pt_checknode(const pt_Node *n, int rl, int mc, int *has_hole) {
    int i, hh;
    pt_check(
            n->child_count >= mc, "[chk] N[%p] rl=%d cc=%d<%d\n", (void *)n, rl,
            n->child_count, mc);
    pt_check(
            n->child_count <= PT_FANOUT, "[chk] N[%p] rl=%d cc=%d>%d\n",
            (void *)n, rl, n->child_count, PT_FANOUT);
    *has_hole = 0;
    for (i = 0; i < n->child_count; ++i) {
        if (rl == 0) {
            if (ptM_ishole(n, i)) {
                pt_Hole *hole = (pt_Hole *)n->children[i];
                pt_check(
                        hole->n <= PT_MAX_HOLESIZE,
                        "[chk] HOLE rl=%d i=%d n=%d > %d\n", rl, i,
                        hole->n, (int)PT_MAX_HOLESIZE);
                pt_check(
                        n->bytes[i] == hole->n,
                        "[chk] HOLE rl=%d i=%d bytes=%zu n=%d\n", rl, i,
                        n->bytes[i], hole->n);
                *has_hole = 1;
            } else {
                pt_check(
                        n->bytes[i] > 0, "[chk] LITERAL rl=%d i=%d bytes=%zu\n",
                        rl, i, n->bytes[i]);
            }
        } else {
            pt_Node *c = n->children[i];
            if (!pt_checknode(c, rl - 1, mc ? PT_FANOUT / 2 : 0, &hh)) return 0;
            pt_check(
                    n->bytes[i] == ptN_sumbytes(c, 0, c->child_count),
                    "[chk] INNER rl=%d i=%d bytes=%zu sum=%zu node=%p\n", rl, i,
                    n->bytes[i], ptN_sumbytes(c, 0, c->child_count), (void *)c);
            pt_check(
                    (ptM_ishole(n, i) != 0) == (hh != 0),
                    "[chk] MASK rl=%d i=%d mask=%d has_hole=%d\n", rl, i,
                    ptM_ishole(n, i) != 0, hh);
            if (hh) *has_hole = 1;
        }
    }
    return 1;
}

PT_STATIC int pt_checktree_allow_empty(pt_Blob snap, int allow_empty) {
    int hh = 0;
    if (snap->root.child_count == 0) {
        pt_check(
                snap->bytes == 0, "[chk] EMPTY tree has bytes=%zu\n",
                snap->bytes);
    } else if (snap->levels > 0 || snap->root.child_count > 1)
        return pt_checknode(
                &snap->root, snap->levels, allow_empty ? 0 : 1, &hh);
    else {
        if (ptM_ishole(&snap->root, 0)) {
            pt_Hole *hole = (pt_Hole *)snap->root.children[0];
            pt_check(
                    hole->n <= PT_MAX_HOLESIZE, "[chk] SINGLE HOLE n=%d > %d\n",
                    hole->n, (int)PT_MAX_HOLESIZE);
            pt_check(
                    snap->root.bytes[0] == hole->n,
                    "[chk] SINGLE HOLE bytes=%zu n=%d\n", snap->root.bytes[0],
                    hole->n);
        } else {
            pt_check(
                    snap->root.bytes[0] > 0, "[chk] SINGLE LITERAL bytes=%zu\n",
                    snap->root.bytes[0]);
        }
    }
    pt_check(
            snap->bytes == ptN_sumbytes(&snap->root, 0, snap->root.child_count),
            "[chk] ROOT bytes=%zu sum=%zu\n", snap->bytes,
            ptN_sumbytes(&snap->root, 0, snap->root.child_count));
    return 1;
}

PT_STATIC int pt_checktree(pt_Blob snap) {
    return pt_checktree_allow_empty(snap, 0);
}

/* ================================================================ */
/*  cursor invariant checker                                         */
/* ================================================================ */

PT_STATIC int pt_checkcursor(pt_Cursor *C, size_t expected_off) {
    size_t   bsum = 0;
    int      i, l;
    pt_Node *p;
    pt_check(
            pt_offset(C) == expected_off,
            "[chk] OFFSET mismatch off=%zu expected=%zu\n",
            pt_offset(C), expected_off);
    if (C->tree->root.child_count == 0) {
        pt_check(
                C->poff == 0 && C->off == 0, "[chk] EMPTY poff=%zu off=%zu\n",
                C->poff, C->off);
        pt_check(
                C->paths[0] == &C->tree->root.children[0],
                "[chk] EMPTY paths[0]=%p expected=%p\n", (void *)C->paths[0],
                (void *)&C->tree->root.children[0]);
        return 1;
    }
    for (l = 0; l <= ptK_levels(C); ++l) {
        p = ptK_parent(C, l);
        i = ptK_idx(C, p, l);
        pt_check(
                i >= 0 && i < p->child_count,
                "[chk] PATHS[%d] invalid idx=%d cc=%u\n", l, i, p->child_count);
        pt_check(
                C->paths[l] == &p->children[i],
                "[chk] PATHS[%d] invalid ptr=%p expected=%p\n", l,
                (void *)C->paths[l], (void *)&p->children[i]);
        bsum += ptN_sumbytes(p, 0, i);
    }
    pt_check(
            C->off == bsum, "[chk] OFF mismatch off=%zu sum=%zu\n",
            C->off, bsum);
    p = ptK_parent(C, ptK_levels(C));
    i = ptK_idx(C, p, ptK_levels(C));
    pt_check(
            C->poff <= p->bytes[i],
            "[chk] POFF out of bounds poff=%zu bytes[%d]=%zu\n",
            C->poff, i, p->bytes[i]);
    return 1;
}

/* ================================================================ */
/*  tree dump                                                        */
/* ================================================================ */

PT_STATIC void pt_dumpnode(const pt_Node *n, int idx, int l, int levels) {
    unsigned i, cc = (unsigned)n->child_count;
    if (l == 0)
        pt_log("Root(%p) cc=%u", (void *)n, cc);
    else
        pt_log("%*sN%u_%u(%p) cc=%u", l * 2, "", (unsigned)(l - 1),
               (unsigned)idx, (void *)n, cc);
    for (i = 0; i < cc; ++i) pt_log(" b[%u]=%zu", i, n->bytes[i]);
    pt_log("\n");
    if ((unsigned)l == (unsigned)levels || levels == 0) {
        for (i = 0; i < cc; ++i) {
            if (ptM_ishole(n, i)) {
                const pt_Hole *hole = (const pt_Hole *)n->children[i];
                pt_log("%*sL%u HOLE n=%u\n", (l + 1) * 2, "", i,
                       (unsigned)hole->n);
            } else {
                pt_log("%*sL%u LIT bytes=%zu %.*s\n", (l + 1) * 2, "", i,
                       n->bytes[i], (int)n->bytes[i],
                       (const char *)n->children[i]);
            }
        }
    } else {
        for (i = 0; i < cc; ++i)
            pt_dumpnode(n->children[i], i, l + 1, levels);
    }
}

PT_STATIC void pt_dumptree(pt_Blob snap, const char *tag) {
    pt_log("[TREE]\t %s: levels=%u root.cc=%u bytes=%zu\n", tag, snap->levels,
           snap->root.child_count, snap->bytes);
    pt_dumpnode(&snap->root, -1, 0, snap->levels);
}

PT_STATIC void pt_dumpcursor(const pt_Cursor *C, const char *tag) {
    (void)C;
    (void)tag; /* TODO */
}

/* ================================================================ */
/*  tree comparison                                                  */
/* ================================================================ */

PT_STATIC int pt_comparenode(
        const pt_Node *a, const pt_Node *b, unsigned l, unsigned levels) {
    unsigned i;
    if (a->child_count != b->child_count) return 0;
    for (i = 0; i < (unsigned)a->child_count; ++i) {
        if (a->bytes[i] != b->bytes[i]) return 0;
        if ((ptM_ishole(a, i) != 0) != (ptM_ishole(b, i) != 0)) return 0;
        if (l == levels) {
            if (ptM_ishole(a, i)) {
                const pt_Hole *ha = (const pt_Hole *)a->children[i];
                const pt_Hole *hb = (const pt_Hole *)b->children[i];
                if (ha->n != hb->n) return 0;
                if (memcmp(ha->data, hb->data, ha->n) != 0) return 0;
            } else {
                if (memcmp((const char *)a->children[i],
                           (const char *)b->children[i], a->bytes[i])
                    != 0)
                    return 0;
            }
        } else {
            if (!pt_comparenode(a->children[i], b->children[i], l + 1, levels))
                return 0;
        }
    }
    return 1;
}

PT_STATIC int pt_comparetree(pt_Blob a, pt_Blob b) {
    if (a->levels != b->levels) return 0;
    if (a->bytes != b->bytes) return 0;
    if (a->root.child_count != b->root.child_count) return 0;
    if (a->root.child_count == 0) return 1;
    return pt_comparenode(&a->root, &b->root, 0, a->levels);
}

/* ================================================================ */
/*  leaf sequence checker                                            */
/* ================================================================ */
/* Verify leaf break bytes match expected rle pairs {count,value,..,0} */

PT_STATIC int pt_checkleaves_rec(
        const pt_Node *n, int l, int levels, unsigned **brs) {
    (void)n;
    (void)l;
    (void)levels;
    (void)brs;
    /* TODO */
    return 0;
}

PT_STATIC int pt_checkleaves(const pt_Blob *S, unsigned **brs) {
    (void)S;
    /* TODO */
    return (**brs == 0);
}

#define checkleavesV(c, ...)                                            \
    do {                                                                \
        unsigned  brs__[] = {__VA_ARGS__, 0};                           \
        unsigned *pbrs__ = brs__;                                       \
        if (!pt_checkleaves((c), &pbrs__)) {                            \
            fprintf(stderr, "checkleavesV FAILED at %s:%d\n", __FILE__, \
                    __LINE__);                                          \
            pt_dumptree((c), "checkleavesV failed");                    \
            assert(0 && "checkleavesV failed");                         \
        }                                                               \
    } while (0)

/* ================================================================ */
/*  tree construction helpers (leafV / botV / innerV / cacheV)       */
/* ================================================================ */

#define litV(s)     (s)
#define leafV(...)  leafV_(S, __VA_ARGS__, (const char *)NULL)
#define innerV(...) innerV_(S, __VA_ARGS__, (pt_Node *)NULL)

PT_STATIC pt_Node *leafV_(pt_State *S, ...) {
    va_list     ap;
    unsigned    i, cc = 0;
    pt_Node    *n;
    const char *s;
    va_start(ap, S);
    while (va_arg(ap, const char *) != NULL) cc++;
    va_end(ap);
    n = (pt_Node *)ptP_alloc(S, &S->nodes);
    assert(n && cc <= PT_FANOUT);
    (void)memset(n->mask, 0, sizeof(n->mask));
    n->child_count = (unsigned short)cc, n->version = 0;
    va_start(ap, S);
    for (i = 0; i < cc; i++) {
        s = va_arg(ap, const char *);
        n->children[i] = (pt_Node *)(char *)s;
        n->bytes[i] = strlen(s);
    }
    va_end(ap);
    return n;
}

PT_STATIC pt_Node *innerV_(pt_State *S, ...) {
    va_list  ap;
    unsigned i, cc = 0;
    pt_Node *n, *ch;
    int      w;
    va_start(ap, S);
    while (va_arg(ap, pt_Node *) != NULL) cc++;
    va_end(ap);
    n = (pt_Node *)ptP_alloc(S, &S->nodes);
    assert(n && cc <= PT_FANOUT);
    memset(n->mask, 0, sizeof(n->mask));
    n->child_count = (unsigned short)cc, n->version = 0;
    va_start(ap, S);
    for (i = 0; i < cc; i++) {
        ch = va_arg(ap, pt_Node *);
        n->children[i] = ch;
        n->bytes[i] = ptN_sumbytes(ch, 0, ch->child_count);
        for (w = 0; w < PT_MASK_SIZE && !ch->mask[w]; ++w) continue;
        ptM_sethole(n, i, w != PT_MASK_SIZE);
    }
    va_end(ap);
    return n;
}

PT_STATIC pt_Blob treeV(pt_State *S, unsigned levels, pt_Node *root) {
    pt_Tree *t = (pt_Tree *)pt_from(S, NULL, 0);
    unsigned i;
    assert(t && root->child_count <= PT_FANOUT);
    t->levels = (unsigned short)levels, t->root = *root;
    ptP_free(&S->nodes, root);
    t->bytes = 0;
    for (i = 0; i < t->root.child_count; i++) t->bytes += t->root.bytes[i];
    pt_checktree_allow_empty(t, 1);
    return t;
}

/* ================================================================ */
/*  pt_asserttree — build expected tree and compare                    */
/* ================================================================ */

#define pt_asserttree(c, lvls, ...)                                      \
    do {                                                                 \
        pt_Blob __d = treeV(S, lvls, __VA_ARGS__);                       \
        if (!pt_comparetree((c), __d)) {                                 \
            fprintf(stderr, "pt_asserttree FAILED at %s:%d\n", __FILE__, \
                    __LINE__);                                           \
            fprintf(stderr, "Expected:\n");                              \
            pt_dumptree(__d, "expected");                                \
            fprintf(stderr, "Actual:\n");                                \
            pt_dumptree((c), "actual");                                  \
            assert(0 && "pt_asserttree failed");                         \
        }                                                                \
        pt_release(__d);                                                 \
    } while (0)

/* ================================================================ */
/*  test runner                                                     */
/* ================================================================ */
/* Usage: after #defining TESTS(X) with all test entries, write:
 *
 *   #define X(name) {#name, test_##name},
 *   PT_TEST_MAIN("my tests")
 *   #undef X
 *
 * PT_TEST_MAIN constructs the entries table and the main function.  */

typedef struct {
    const char *name;
    void (*fn)(void);
} pt_test_entry;

PT_STATIC int pt_test_main(
        const char *banner, const pt_test_entry *entries, int argc,
        char *argv[]) {
    int i, j;
    pt_log("=== %s ===\n", banner);
    if (argc == 1) {
        const pt_test_entry *e = entries;
        while (e->name) {
            pt_log("- %s\n", e->name);
            e->fn();
            pt_log("  %s OK\n", e->name);
            ++e;
        }
        pt_log("\nAll tests passed!\n");
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
                pt_log("- %s\n", entries[j].name);
                entries[j].fn();
                pt_log("  %s OK\n", entries[j].name);
                found = 1;
                if (only) break;
            }
        }
        if (!found) {
            pt_log("Unknown test: %s\n", name);
            return 1;
        }
    }
    return 0;
}

#define PT_TEST_MAIN(banner)                                    \
    int main(int argc, char *argv[]) {                          \
        static const pt_test_entry _test_entries[] = {          \
                TESTS(X){NULL, NULL},                           \
        };                                                      \
        return pt_test_main(banner, _test_entries, argc, argv); \
    }

#endif /* pt_TESTS_H */
