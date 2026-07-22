#ifndef CG_IMPLEMENTATION
# define CG_IMPLEMENTATION
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cellgrid.h"

#define cg_log(...) fprintf(stderr, __VA_ARGS__)

static void *cg_test_alloc(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    (void)ud, (void)osize;
    if (nsize == 0) return free(p), NULL;
    np = realloc(p, nsize);
    if (!np) abort();
    return np;
}

static void *oom_alloc(void *ud, void *p, size_t osize, size_t nsize) {
    int *cnt = (int *)ud;
    (void)osize;
    if (nsize == 0) return free(p), NULL;
    if (!cnt || *cnt <= 0) return NULL;
    return (*cnt)--, realloc(p, nsize);
}

typedef struct {
    const char *name;
    void (*fn)(void);
} cg_test_entry;

static int cg_test_main(
        const char *b, const cg_test_entry *e, int argc, char *argv[]) {
    int i, j;
    cg_log("=== %s ===\n", b);
    if (argc == 1) {
        while (e->name) e->fn(), cg_log("  %s OK\n", e->name), ++e;
        return cg_log("\nAll tests passed!\n"), 0;
    }
    for (i = 1; i < argc; ++i) {
        const char *n = argv[i];
        size_t      len = strlen(n);
        int         found = 0, only = *n == '@';
        if (only) n++, len--;
        for (j = 0; e[j].name; ++j)
            if (strlen(e[j].name) >= len && strncmp(n, e[j].name, len) == 0)
                e[j].fn(), cg_log("  %s OK\n", e[j].name), found = 1,
                                                           j = only ? 9999 : j;
        if (!found) return cg_log("Unknown test: %s\n", n), 1;
    }
    return 0;
}

#define CG_TEST_MAIN(banner)                                     \
    int main(int argc, char *argv[]) {                           \
        static const cg_test_entry e[] = {TESTS(X){NULL, NULL}}; \
        return cg_test_main(banner, e, argc, argv);              \
    }

#define X(name)    static void test_##name(void)
#define TEST(name) X(name)

/* ================================================================== */
/*  DebugDiff                                                          */
/* ================================================================== */

typedef struct {
    cg_Diff base;
    char   *buf;
    size_t *plen;
} cg_DebugDiff;

#define dbg_self(D) ((cg_DebugDiff *)(D))
#define dbg_fmt(...)                                                         \
    do {                                                                     \
        int w = snprintf(dbg_self(D)->buf, *dbg_self(D)->plen, __VA_ARGS__); \
        if (w > 0) dbg_self(D)->buf += w, *dbg_self(D)->plen -= (size_t)w;   \
    } while (0)

static int dbg_scroll(cg_Diff *D, int top, int bot, int n) {
    dbg_fmt("[S %d %d %d]", top, bot, n);
    return 0;
}
static int dbg_move(cg_Diff *D, int r, int c) {
    dbg_fmt("[M %d %d]", r, c);
    return 0;
}
static int dbg_style(cg_Diff *D, unsigned st) {
    dbg_fmt("[T %u]", st);
    return 0;
}
static int dbg_fill(cg_Diff *D, int n, int cp) {
    if (cp >= 0x21 && cp <= 0x7e)
        dbg_fmt("[F %d '%c']", n, cp);
    else
        dbg_fmt("[F %d 0x%x]", n, (unsigned)cp);
    return 0;
}
static int dbg_put(cg_Diff *D, int cp) {
    if (cp >= 0x21 && cp <= 0x7e)
        dbg_fmt("[P '%c']", cp);
    else
        dbg_fmt("[P 0x%x]", (unsigned)cp);
    return 0;
}
static int dbg_finish(cg_Diff *D) {
    dbg_fmt("[F]");
    return 0;
}

void cg_initdebug(cg_DebugDiff *d, char *buf, size_t *plen) {
    memset(d, 0, sizeof(*d));
    d->buf = buf, d->plen = plen;
    *buf = '\0';
    d->base.scroll = dbg_scroll;
    d->base.move   = dbg_move;
    d->base.style  = dbg_style;
    d->base.fill   = dbg_fill;
    d->base.put    = dbg_put;
    d->base.finish = dbg_finish;
}

static void assert_diff(cg_Grid *G, const char *expected) {
    cg_DebugDiff d;
    char         buf[4096];
    size_t       len = sizeof(buf);
    cg_initdebug(&d, buf, &len);
    assert(cg_diff(G, &d.base) == CG_OK);
    if (strcmp(buf, expected) != 0)
        cg_log("diff mismatch:\n  got:      %s\n  expected: %s\n", buf,
               expected);
    assert(strcmp(buf, expected) == 0);
}

/* ================================================================== */
/*  lifecycle                                                          */
/* ================================================================== */

TEST(init_params) {
    cg_Grid g;
    assert(cg_init(NULL, cg_test_alloc, NULL) == CG_ERRPARAM);
    assert(cg_init(&g, cg_test_alloc, NULL) == CG_OK);
    assert(cg_rows(&g) == 0);
    assert(cg_cols(&g) == 0);
    assert(cg_top(&g) == 0);
    cg_free(&g);
}

TEST(free_null) { cg_free(NULL); }

TEST(free_empty) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_free(&g);
}

TEST(init_default_alloc) {
    cg_Grid g;
    assert(cg_init(&g, NULL, NULL) == CG_OK);
    assert(cg_begin(&g, 0, 2, 3) == CG_OK);
    cg_put(&g, 0, 0, 'X', 0);
    assert(cg_cell(&g, 0, 0, NULL) == 'X');
    cg_free(&g);
}

/* ================================================================== */
/*  cg_begin                                                           */
/* ================================================================== */

TEST(begin_first) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    assert(cg_begin(&g, 5, 2, 3) == CG_OK);
    assert(cg_rows(&g) == 2);
    assert(cg_cols(&g) == 3);
    assert(cg_top(&g) == 5);
    cg_free(&g);
}

TEST(begin_zero) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    assert(cg_begin(&g, 0, 0, 5) == CG_ERRPARAM);
    assert(cg_begin(&g, 0, 5, 0) == CG_ERRPARAM);
    assert(cg_rows(&g) == 0);
    cg_free(&g);
}

TEST(begin_null) {
    assert(cg_begin(NULL, 0, 2, 3) == CG_ERRPARAM);
}

TEST(begin_oom_init) {
    int      cnt = 0;
    cg_Grid g;
    cg_init(&g, oom_alloc, &cnt);
    assert(cg_begin(&g, 0, 2, 3) == CG_ERRMEM);
    assert(cg_rows(&g) == 0);
    cg_free(&g);
}

TEST(begin_oom_resize) {
    int      cnt = 0;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'A', 0);
    g.allocf = oom_alloc, g.ud = &cnt;
    assert(cg_begin(&g, 0, 4, 5) == CG_ERRMEM);
    assert(cg_rows(&g) == 2);
    assert(cg_cols(&g) == 3);
    assert(cg_cell(&g, 0, 0, NULL) == 'A');
    cg_free(&g);
}

static int cw_double(void *ud, int cp) {
    (void)ud;
    return cp < 0x80 ? 1 : 2;
}

TEST(begin_resize_grow) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'A', 1);
    cg_begin(&g, 1, 4, 5);
    assert(cg_rows(&g) == 4);
    assert(cg_cols(&g) == 5);
    assert(cg_top(&g) == 1);
    assert(cg_cell(&g, 0, 0, NULL) == 'A');
    cg_free(&g);
}

TEST(begin_resize_shrink) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_begin(&g, 0, 4, 5);
    cg_put(&g, 0, 2, 'B', 1);
    cg_begin(&g, 2, 2, 3);
    assert(cg_rows(&g) == 2);
    assert(cg_cols(&g) == 3);
    assert(cg_top(&g) == 2);
    assert(cg_cell(&g, 0, 2, NULL) == 'B');
    cg_free(&g);
}

TEST(begin_same) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'X', 1);
    cg_begin(&g, 10, 2, 3);
    assert(cg_rows(&g) == 2);
    assert(cg_top(&g) == 10);
    assert(cg_cell(&g, 0, 0, NULL) == 'X');
    cg_free(&g);
}

TEST(begin_sametop) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'a', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 2, 3);
    assert(cg_isdirty(&g, 0, 0) == 0);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_setwcwidth / cg_clear                                            */
/* ================================================================== */

TEST(wcwidth_set) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 1);
    assert(cg_cell(&g, 0, 0, NULL) == 0x4e2d);
    assert(cg_cell(&g, 0, 1, NULL) == -1);
    cg_free(&g);
}

TEST(clear_set) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'X', 0);
    cg_clear(&g);
    assert(cg_cell(&g, 0, 0, NULL) == ' ');
    cg_free(&g);
}

TEST(clear_null) { cg_clear(NULL); }

TEST(clear_empty) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_clear(&g);
    cg_free(&g);
}

TEST(wcwidth_null) { cg_setwcwidth(NULL, cw_double, NULL); }

/* ================================================================== */
/*  cg_put                                                             */
/* ================================================================== */

TEST(put_ascii) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 1, 'a', 7);
    assert(cg_cell(&g, 0, 1, &st) == 'a');
    assert(st == 7);
    cg_free(&g);
}

TEST(put_widechar) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 2);
    assert(cg_cell(&g, 0, 0, NULL) == 0x4e2d);
    assert(cg_cell(&g, 0, 1, NULL) == -1);
    cg_free(&g);
}

TEST(put_truncate) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 2, 0x4e2d, 1);
    assert(cg_cell(&g, 0, 2, NULL) == '>');
    cg_free(&g);
}

TEST(put_overwrite_left) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 1);
    cg_put(&g, 0, 1, 'a', 1);
    assert(cg_cell(&g, 0, 0, NULL) == 0x20);
    assert(cg_cell(&g, 0, 1, NULL) == 'a');
    cg_free(&g);
}

TEST(put_overwrite_right) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 1);
    cg_put(&g, 0, 0, 'a', 1);
    assert(cg_cell(&g, 0, 0, NULL) == 'a');
    assert(cg_cell(&g, 0, 1, NULL) == 0x20);
    cg_free(&g);
}

TEST(put_params) {
    cg_Grid g, *gp = NULL;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, -1, 0, 'a', 0);
    cg_put(&g, 2, 0, 'a', 0);
    cg_put(&g, 0, -1, 'a', 0);
    cg_put(&g, 0, 3, 'a', 0);
    cg_put(gp, 0, 0, 'a', 0);
    assert(cg_cell(&g, 0, 0, NULL) == ' ');
    cg_free(&g);
}

TEST(put_same) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'X', 0);
    cg_freeze(&g);
    cg_put(&g, 0, 0, 'X', 0);
    assert(cg_isdirty(&g, 0, 0) == 0);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_clearrow                                                        */
/* ================================================================== */

TEST(clearrow_range) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 1, 0, 'a', 1);
    cg_put(&g, 1, 1, 'b', 1);
    cg_put(&g, 1, 2, 'c', 1);
    cg_clearrow(&g, 1, 1, 3);
    assert(cg_cell(&g, 1, 0, NULL) == 'a');
    assert(cg_cell(&g, 1, 1, NULL) == ' ');
    assert(cg_cell(&g, 1, 2, NULL) == ' ');
    assert(cg_cell(&g, 1, 3, NULL) == ' ');
    cg_free(&g);
}

TEST(clearrow_params) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_clearrow(&g, -1, 0, 1);
    cg_clearrow(&g, 2, 0, 1);
    cg_clearrow(&g, 0, 1, 1);
    cg_clearrow(&g, 0, 2, 1);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_fill                                                            */
/* ================================================================== */

TEST(fill_set) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 5);
    cg_fill(&g, 0, 1, 4, '.');
    assert(cg_cell(&g, 0, 1, NULL) == '.');
    assert(cg_cell(&g, 0, 2, NULL) == '.');
    assert(cg_cell(&g, 0, 3, NULL) == '.');
    assert(cg_cell(&g, 0, 0, NULL) == 0x20);
    cg_free(&g);
}

TEST(fill_params) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_fill(&g, -1, 0, 2, '.');
    cg_fill(&g, 2, 0, 2, '.');
    cg_fill(&g, 0, -1, 2, '.');
    cg_fill(&g, 0, 5, 10, '.');
    cg_fill(&g, 0, 2, 2, '.');
    cg_free(&g);
}

/* ================================================================== */
/*  cg_span                                                            */
/* ================================================================== */

TEST(span_set) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_span(&g, 0, 1, 3, 3);
    assert(cg_cell(&g, 0, 1, &st) == 0x20);
    assert(st == 3);
    assert(cg_cell(&g, 0, 2, &st) == 0x20);
    assert(st == 3);
    cg_cell(&g, 0, 0, &st);
    assert(st == 0);
    cg_free(&g);
}

TEST(span_params) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_span(&g, -1, 0, 2, 1);
    cg_span(&g, 2, 0, 2, 1);
    cg_span(&g, 0, -1, 2, 1);
    cg_span(&g, 0, 0, 0, 1);
    cg_span(&g, 0, 3, 6, 7);
    cg_free(&g);
}

TEST(span_backmatch) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_span(&g, 0, 0, 2, 5);
    cg_freeze(&g);
    cg_span(&g, 0, 0, 2, 5);
    assert(cg_isdirty(&g, 0, 0) == 0);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_putline                                                         */
/* ================================================================== */

TEST(putline_ascii) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    cg_setwcwidth(&g, cw_double, NULL);
    assert(cg_putline(&g, 0, 0, "ab", 1) == 2);
    assert(cg_cell(&g, 0, 0, NULL) == 'a');
    assert(cg_cell(&g, 0, 1, NULL) == 'b');
    assert_diff(&g,
        "[M 0 0][T 1][P 'a'][M 0 1][P 'b'][M 0 2][T 0][F 4 0x20][F]");
    cg_free(&g);
}

TEST(putline_wide) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    cg_setwcwidth(&g, cw_double, NULL);
    assert(cg_putline(&g, 0, 0, "\xe4\xb8\xad", 1) == 2);
    assert(cg_cell(&g, 0, 0, NULL) == 0x4e2d);
    assert(cg_cell(&g, 0, 1, NULL) == -1);
    assert_diff(&g,
        "[M 0 0][T 1][P 0x4e2d][M 0 2][T 0][F 4 0x20][F]");
    cg_free(&g);
}

TEST(putline_params) {
    char     buf[4];
    cg_Grid g, *gp = NULL;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    assert(cg_putline(&g, 0, 0, NULL, 1) == 0);
    assert(cg_putline(&g, -1, 0, "a", 1) == 0);
    assert(cg_putline(&g, 0, 4, "a", 1) == 4);
    assert(cg_putline(gp, 0, 0, "a", 1) == 0);
    buf[0] = (char)0x80;
    buf[1] = 'a';
    buf[2] = 'b';
    buf[3] = '\0';
    assert(cg_putline(&g, 0, 0, buf, 1) == 2);
    assert(cg_cell(&g, 0, 0, NULL) == 'a');
    assert(cg_cell(&g, 0, 1, NULL) == 'b');
    cg_free(&g);
}

TEST(putline_skip) {
    char    buf[3];
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    buf[0] = (char)0x80;
    buf[1] = 'X';
    buf[2] = '\0';
    assert(cg_putline(&g, 0, 0, buf, 1) == 1);
    assert(cg_cell(&g, 0, 0, NULL) == 'X');
    assert(cg_cell(&g, 0, 1, NULL) == 0x20);
    assert_diff(&g,
        "[M 0 0][T 1][P 'X'][M 0 1][T 0][P 0x20][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(putline_2byte) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    assert(cg_putline(&g, 0, 0, "\xc3\x80", 1) == 2);
    assert(cg_cell(&g, 0, 0, NULL) == 0xc0);
    assert(cg_cell(&g, 0, 1, NULL) == -1);
    assert_diff(&g,
        "[M 0 0][T 1][P 0xc0][M 0 2][T 0][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(putline_4byte) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    assert(cg_putline(&g, 0, 0, "\xf0\x90\x80\x80", 1) == 2);
    assert(cg_cell(&g, 0, 0, NULL) == 0x10000);
    assert(cg_cell(&g, 0, 1, NULL) == -1);
    assert_diff(&g,
        "[M 0 0][T 1][P 0x10000][M 0 2][T 0][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(diff_wide_nocont) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 1);
    /* continuation cell (0,1) must NOT appear in diff */
    assert_diff(&g,
        "[M 0 0][T 1][P 0x4e2d][M 0 2][T 0][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(putline_atend) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    assert(cg_putline(&g, 0, 3, "abc", 0) == 3);
    cg_free(&g);
}

/* ================================================================== */
/*  getters                                                            */
/* ================================================================== */

TEST(getter_values) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 3, 2, 5);
    assert(cg_rows(&g) == 2);
    assert(cg_cols(&g) == 5);
    assert(cg_top(&g) == 3);
    cg_free(&g);
}

TEST(getter_cell) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 1, 'Z', 4);
    assert(cg_cell(&g, 0, 1, &st) == 'Z');
    assert(st == 4);
    cg_free(&g);
}

TEST(getter_back) {
    unsigned st;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    assert(cg_back(&g, 0, 0, &st) == ' ');
    assert(st == 0);
    cg_free(&g);
}

TEST(getter_params) {
    cg_Grid g, *gp = NULL;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    assert(cg_cell(&g, -1, 0, NULL) == 0);
    assert(cg_cell(&g, 2, 0, NULL) == 0);
    assert(cg_cell(&g, 0, -1, NULL) == 0);
    assert(cg_cell(&g, 0, 3, NULL) == 0);
    assert(cg_cell(gp, 0, 0, NULL) == 0);
    assert(cg_back(&g, -1, 0, NULL) == 0);
    assert(cg_back(gp, 0, 0, NULL) == 0);
    assert(cg_isdirty(&g, -1, 0) == 0);
    assert(cg_isdirty(&g, 0, 3) == 0);
    assert(cg_isdirty(gp, 0, 0) == 0);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_isdirty / cg_freeze                                             */
/* ================================================================== */

TEST(dirty_track) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 1, 'a', 0);
    assert(cg_isdirty(&g, 0, 1) == 1);
    cg_free(&g);
}

TEST(freeze_null) {
    cg_freeze(NULL);
}

TEST(freeze_empty) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_freeze(&g);
    cg_free(&g);
}

TEST(freeze_copies) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'X', 5);
    assert(cg_isdirty(&g, 0, 0) == 1);
    assert(cg_back(&g, 0, 0, NULL) == ' ');
    cg_freeze(&g);
    assert(cg_isdirty(&g, 0, 0) == 0);
    assert(cg_back(&g, 0, 0, NULL) == 'X');
    cg_free(&g);
}

TEST(freeze_clears_dirty) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'a', 0);
    cg_put(&g, 0, 1, 'b', 0);
    assert(cg_isdirty(&g, 0, 0) == 1);
    assert(cg_isdirty(&g, 0, 1) == 1);
    cg_freeze(&g);
    assert(cg_isdirty(&g, 0, 0) == 0);
    assert(cg_isdirty(&g, 0, 1) == 0);
    cg_free(&g);
}

/* ================================================================== */
/*  scroll tests                                                       */
/* ================================================================== */

TEST(begin_scroll_down) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_put(&g, 1, 0, 'B', 0);
    cg_put(&g, 2, 0, 'C', 0);
    cg_freeze(&g);
    cg_begin(&g, 1, 3, 4);
    assert(cg_cell(&g, 0, 0, NULL) == 'B');
    assert(cg_cell(&g, 1, 0, NULL) == 'C');
    cg_free(&g);
}

TEST(begin_scroll_up) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 1, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_put(&g, 1, 0, 'B', 0);
    cg_put(&g, 2, 0, 'C', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 3, 4);
    assert(cg_cell(&g, 1, 0, NULL) == 'A');
    assert(cg_cell(&g, 2, 0, NULL) == 'B');
    cg_free(&g);
}

TEST(scroll_up_expose) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 1, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_put(&g, 1, 0, 'B', 0);
    cg_put(&g, 2, 0, 'C', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 3, 4);
    assert(cg_cell(&g, 0, 0, NULL) == ' ');
    assert(cg_cell(&g, 1, 0, NULL) == 'A');
    cg_free(&g);
}

/* ================================================================== */
/*  diff tests                                                         */
/* ================================================================== */

TEST(diff_empty) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_freeze(&g);
    assert_diff(&g, "[F]");
    cg_free(&g);
}

TEST(diff_emptygrid) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    assert(cg_diff(&g, NULL) == CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_all_dirty) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'a', 0);
    cg_put(&g, 1, 1, 'b', 1);
    assert_diff(
            &g,
            "[M 0 0][P 'a'][M 0 1][P 0x20][P 0x20]"
            "[M 1 0][P 0x20][M 1 1][T 1][P 'b'][M 1 2][T 0][P 0x20][F]");
    cg_free(&g);
}

TEST(diff_scroll) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_freeze(&g);
    cg_begin(&g, 1, 3, 4);
    assert_diff(&g, "[S 1 3 -1][F]");
    cg_free(&g);
}

TEST(diff_bigjump) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_freeze(&g);
    cg_begin(&g, 10, 3, 4);
    assert_diff(
            &g,
            "[M 0 0][P 'A'][M 0 1][P 0x20][P 0x20][P 0x20]"
            "[M 1 0][F 4 0x20][M 2 0][F 4 0x20][F]");
    cg_free(&g);
}

TEST(diff_twice) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_freeze(&g);
    assert_diff(&g, "[F]");
    cg_put(&g, 0, 0, 'a', 0);
    assert_diff(&g, "[M 0 0][P 'a'][F]");
    cg_free(&g);
}

TEST(diff_clear_dirty) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'a', 0);
    cg_freeze(&g);
    cg_clear(&g);
    cg_begin(&g, 0, 2, 3);
    assert_diff(&g, "[M 0 0][P 0x20][P 0x20][P 0x20]"
                     "[M 1 0][P 0x20][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(diff_style_change) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'x', 0);
    cg_freeze(&g);
    cg_put(&g, 0, 0, 'x', 5);
    assert_diff(&g, "[M 0 0][T 5][P 'x'][T 0][F]");
    cg_free(&g);
}

TEST(diff_resize) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 2, 5);
    assert_diff(&g, "[F]");
    cg_free(&g);
}

static int err_cb(cg_Diff *D) {
    (void)D;
    return CG_ERRPARAM;
}
static int err_cb_sn(cg_Diff *D, int top, int bot, int n) {
    (void)D, (void)top, (void)bot, (void)n;
    return CG_ERRPARAM;
}
static int err_cb_mv(cg_Diff *D, int r, int c) {
    (void)D, (void)r, (void)c;
    return CG_ERRPARAM;
}
static int err_cb_st(cg_Diff *D, unsigned st) {
    (void)D, (void)st;
    return CG_ERRPARAM;
}
static int err_cb_put(cg_Diff *D, int cp) {
    (void)D, (void)cp;
    return CG_ERRPARAM;
}
static int err_cb_fill(cg_Diff *D, int n, int cp) {
    (void)D, (void)n, (void)cp;
    return CG_ERRPARAM;
}

TEST(diff_fill_error) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    memset(&d, 0, sizeof(d));
    d.fill = err_cb_fill;
    assert(cg_diff(&g, &d) == CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_null) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    memset(&d, 0, sizeof(d));
    assert(cg_diff(&g, &d) == CG_OK);
    cg_free(&g);
}

TEST(diff_cb_scroll_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_freeze(&g);
    cg_begin(&g, 1, 2, 3);
    memset(&d, 0, sizeof(d));
    d.scroll = err_cb_sn;
    assert(cg_diff(&g, &d) == CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_move_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 2);
    memset(&d, 0, sizeof(d));
    d.move = err_cb_mv;
    assert(cg_diff(&g, &d) == CG_ERRPARAM);
    cg_free(&g);
}

static int err_cb_st0(cg_Diff *D, unsigned st) {
    (void)D;
    return st == 0 ? CG_ERRPARAM : CG_OK;
}

TEST(diff_cb_style_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 2);
    cg_put(&g, 0, 0, 'x', 7);
    memset(&d, 0, sizeof(d));
    d.style = err_cb_st;
    assert(cg_diff(&g, &d) == CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_put_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 2);
    cg_put(&g, 0, 0, 'x', 0);
    memset(&d, 0, sizeof(d));
    d.put = err_cb_put;
    assert(cg_diff(&g, &d) == CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_finish_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_freeze(&g);
    memset(&d, 0, sizeof(d));
    d.finish = err_cb;
    assert(cg_diff(&g, &d) == CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_style0_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 1);
    cg_put(&g, 0, 0, 'x', 7);
    memset(&d, 0, sizeof(d));
    d.style = err_cb_st0;
    assert(cg_diff(&g, &d) == CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_fill_min_pass) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    memset(&d, 0, sizeof(d));
    d.fill_min = 2;
    assert(cg_diff(&g, &d) == CG_OK);
    cg_free(&g);
}

TEST(tocp_trunc) {
    char buf[8];
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)0xc3;
    cgK_tocp(buf, 1);
    buf[0] = (char)0xe4;
    cgK_tocp(buf, 2);
    buf[0] = (char)0xe4;
    cgK_tocp(buf, 1);
}

/* ================================================================== */
/*  edge cases                                                         */
/* ================================================================== */

TEST(empty_grid) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_put(&g, 0, 0, 'a', 0);
    cg_span(&g, 0, 0, 2, 1);
    cg_clearrow(&g, 0, 0, 2);
    cg_fill(&g, 0, 0, 2, '.');
    assert(cg_putline(&g, 0, 0, "a", 0) == 0);
    assert(cg_cell(&g, 0, 0, NULL) == 0);
    assert(cg_back(&g, 0, 0, NULL) == 0);
    assert(cg_isdirty(&g, 0, 0) == 0);
    cg_free(&g);
}

TEST(resize_cols) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 2, 5);
    assert(cg_cols(&g) == 5);
    assert(cg_cell(&g, 0, 0, NULL) == 'A');
    cg_free(&g);
}

TEST(resize_rows) {
    cg_Grid g;
    cg_init(&g, cg_test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 4, 3);
    assert(cg_rows(&g) == 4);
    assert(cg_cell(&g, 0, 0, NULL) == 'A');
    cg_free(&g);
}

#define TESTS(X)           \
    X(init_params)         \
    X(free_null)           \
    X(free_empty)          \
    X(init_default_alloc)  \
    X(begin_first)         \
    X(begin_zero)          \
    X(begin_null)          \
    X(begin_oom_init)      \
    X(begin_oom_resize)    \
    X(begin_resize_grow)   \
    X(begin_resize_shrink) \
    X(begin_same)          \
    X(begin_sametop)       \
    X(wcwidth_set)         \
    X(clear_set)           \
    X(clear_null)          \
    X(clear_empty)         \
    X(wcwidth_null)        \
    X(put_ascii)           \
    X(put_widechar)        \
    X(put_truncate)        \
    X(put_overwrite_left)  \
    X(put_overwrite_right) \
    X(put_params)          \
    X(put_same)            \
    X(clearrow_range)      \
    X(clearrow_params)     \
    X(fill_set)            \
    X(fill_params)         \
    X(span_set)            \
    X(span_params)         \
    X(span_backmatch)      \
    X(putline_ascii)       \
    X(putline_wide)        \
    X(putline_params)      \
    X(putline_skip)        \
    X(putline_2byte)       \
    X(putline_4byte)       \
    X(putline_atend)       \
    X(getter_values)       \
    X(getter_cell)         \
    X(getter_back)         \
    X(getter_params)       \
    X(dirty_track)         \
    X(freeze_null)         \
    X(freeze_empty)        \
    X(freeze_copies)       \
    X(freeze_clears_dirty) \
    X(begin_scroll_down)   \
    X(begin_scroll_up)     \
    X(scroll_up_expose)    \
    X(diff_empty)          \
    X(diff_emptygrid)      \
    X(diff_all_dirty)      \
    X(diff_scroll)         \
    X(diff_bigjump)        \
    X(diff_twice)          \
    X(diff_clear_dirty)    \
    X(diff_style_change)   \
    X(diff_resize)         \
    X(diff_wide_nocont)    \
    X(diff_fill_error)     \
    X(diff_cb_null)        \
    X(diff_cb_scroll_err)  \
    X(diff_cb_move_err)    \
    X(diff_cb_style_err)   \
    X(diff_cb_style0_err)  \
    X(diff_cb_put_err)     \
    X(diff_cb_finish_err)  \
    X(diff_fill_min_pass)  \
    X(tocp_trunc)          \
    X(empty_grid)          \
    X(resize_cols)         \
    X(resize_rows)

#undef X
#define X(name) {#name, test_##name},
CG_TEST_MAIN("cellgrid tests")
