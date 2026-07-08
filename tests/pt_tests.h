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

PT_STATIC void pt_dumptree(const pt_Snapshot *S, const char *tag);
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
    lcP_stat(pool->live_obj += count);
}

/* ================================================================ */
/*  tree invariant checker                                           */
/* ================================================================ */

PT_STATIC int pt_checknode(const pt_Node *n, int rl, int mc) {
    /* TODO */
    return 0;
}

PT_STATIC int pt_checktree_allow_empty(const pt_Snapshot *S, int allow_empty) {
    /* TODO */
    return 0;
}

PT_STATIC int pt_checktree(const pt_Snapshot *S) {
    return pt_checktree_allow_empty(S, 0);
}

/* ================================================================ */
/*  cursor invariant checker                                         */
/* ================================================================ */

PT_STATIC int pt_checkcursor(pt_Cursor *C, size_t expected_off) {
    /* TODO */
    return 0;
}

/* ================================================================ */
/*  tree dump                                                        */
/* ================================================================ */

PT_STATIC void pt_dumpnode(const pt_Node *n, int idx, int l, int levels) {
    /* TODO */
}

PT_STATIC void pt_dumptree(const pt_Snapshot *S, const char *tag) { /* TODO */ }

PT_STATIC void pt_dumpcursor(const pt_Cursor *C, const char *tag) { /* TODO */ }

/* ================================================================ */
/*  tree comparison                                                  */
/* ================================================================ */

PT_STATIC int pt_comparenode(
        const pt_Node *a, const pt_Node *b, unsigned l, unsigned levels) {
    /* TODO */
    return 0;
}

PT_STATIC int pt_comparetree(const pt_Snapshot *a, const pt_Snapshot *b) {
    /* TODO */
    return 0;
}

/* ================================================================ */
/*  leaf sequence checker                                            */
/* ================================================================ */
/* Verify leaf break bytes match expected rle pairs {count,value,..,0} */

static int pt_checkleaves_rec(
        const pt_Node *n, int l, int levels, unsigned **brs) {
    /* TODO */
    return 0;
}

PT_STATIC int pt_checkleaves(const pt_Snapshot *S, unsigned **brs) {
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

#define leafV(...)  leafV_(S, __VA_ARGS__, 0)
#define botV(...)   botV_(S, __VA_ARGS__, NULL)
#define innerV(...) innerV_(S, __VA_ARGS__, NULL)

PT_STATIC pt_Node *leafV_(pt_State *S, ...) { /* TODO */ }

PT_STATIC pt_Node *botV_(pt_State *S, ...) { /* TODO */ }

PT_STATIC pt_Node *innerV_(pt_State *S, ...) { /* TODO */ }

PT_STATIC pt_Snapshot *cacheV(pt_State *S, unsigned levels, pt_Node *root) {
    /* TODO */
    return NULL;
}

/* ================================================================ */
/*  pt_asserttree — build expected tree and compare                    */
/* ================================================================ */

#define pt_asserttree(c, lvls, ...)                                      \
    do {                                                                 \
        pt_Snapshot *__d = cacheV(S, lvls, __VA_ARGS__);                 \
        if (!pt_comparetree((c), __d)) {                                 \
            fprintf(stderr, "pt_asserttree FAILED at %s:%d\n", __FILE__, \
                    __LINE__);                                           \
            fprintf(stderr, "Expected:\n");                              \
            pt_dumptree(__d, "expected");                                \
            fprintf(stderr, "Actual:\n");                                \
            pt_dumptree((c), "actual");                                  \
            assert(0 && "pt_asserttree failed");                         \
        }                                                                \
        pt_deltree(S, __d);                                              \
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

#define pt_TEST_MAIN(banner)                                    \
    int main(int argc, char *argv[]) {                          \
        static const pt_test_entry _test_entries[] = {          \
                TESTS(X){NULL, NULL},                           \
        };                                                      \
        return pt_test_main(banner, _test_entries, argc, argv); \
    }

#endif /* pt_TESTS_H */
