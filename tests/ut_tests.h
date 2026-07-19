/*
 * ut_tests.h — shared test utilities for undotree tests
 *
 * Usage: #include "ut_tests.h".  Define TESTS(X) macro with
 *        all test entries, then call UT_TEST_MAIN().
 */

#ifndef UT_TESTS_H
#define UT_TESTS_H

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ut_log(...) fprintf(stderr, __VA_ARGS__)
#define ut_lu(x)    ((unsigned long)(x))

#define ut_check(e, ...)                         \
    do {                                         \
        if (!(e)) return ut_log(__VA_ARGS__), 0; \
    } while (0)

#define UT_POOL_STATS
#include "../undotree.h"

UT_STATIC void *ut_test_alloc(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    (void)ud, (void)osize;
    if (nsize == 0) return free(p), NULL;
    np = realloc(p, nsize);
    if (!np) abort();
    return np;
}

UT_STATIC void *oom_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
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

typedef struct {
    void *chain;
} ut_Drain;

UT_STATIC ut_Drain ut_drainpool(ut_Pool *p) {
    ut_Drain d;
    d.chain = p->freed, p->freed = NULL;
    return d;
}

UT_STATIC void ut_refillpool(ut_Pool *p, ut_Drain d) {
    void **pp;
    if (d.chain == NULL) return;
    for (pp = &d.chain; *pp; pp = (void **)*pp) continue;
    *pp = p->freed, p->freed = d.chain;
}

/* ================================================================ */
/*  test runner                                                      */
/* ================================================================ */

typedef struct {
    const char *name;
    void (*fn)(void);
} ut_test_entry;

UT_STATIC int ut_test_main(
        const char *banner, const ut_test_entry *entries, int argc,
        char *argv[]) {
    int i, j;
    ut_log("=== %s ===\n", banner);
    if (argc == 1) {
        const ut_test_entry *e = entries;
        while (e->name) {
            ut_log("- %s\n", e->name);
            e->fn();
            ut_log("  %s OK\n", e->name);
            ++e;
        }
        ut_log("\nAll tests passed!\n");
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
                ut_log("- %s\n", entries[j].name);
                entries[j].fn();
                ut_log("  %s OK\n", entries[j].name);
                found = 1;
                if (only) break;
            }
        }
        if (!found) {
            ut_log("Unknown test: %s\n", name);
            return 1;
        }
    }
    return 0;
}

#define UT_TEST_MAIN(banner)                                    \
    int main(int argc, char *argv[]) {                          \
        static const ut_test_entry _test_entries[] = {          \
                TESTS(X){NULL, NULL},                           \
        };                                                      \
        return ut_test_main(banner, _test_entries, argc, argv); \
    }

#endif /* UT_TESTS_H */
