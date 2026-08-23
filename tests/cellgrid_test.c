#define _DEFAULT_SOURCE /* glibc: declare snprintf under strict C89 */
#ifndef CG_IMPLEMENTATION
# define CG_IMPLEMENTATION
#endif
#include "cellgrid.h"

#include "tests.h"

/* slice from a string literal or byte array (sizeof-1 excludes NUL) */
#define SL(x) cg_slice((x), sizeof(x) - 1)

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
    d->base.move = dbg_move;
    d->base.style = dbg_style;
    d->base.fill = dbg_fill;
    d->base.put = dbg_put;
    d->base.finish = dbg_finish;
}

static void assert_diff(cg_Grid *G, const char *expected) {
    cg_DebugDiff d;
    char         buf[4096];
    size_t       len = sizeof(buf);
    cg_initdebug(&d, buf, &len);
    asserteq(cg_diff(G, &d.base), CG_OK);
    if (strcmp(buf, expected) != 0)
        test_log(
                "diff mismatch:\n  got:      %s\n  expected: %s\n", buf,
                expected);
    assertstreq(buf, expected);
}

/* ================================================================== */
/*  lifecycle                                                          */
/* ================================================================== */

TEST(init_params) {
    cg_Grid g, *gp = &g;
    asserteq(cg_init(NULL, test_alloc, NULL), CG_ERRPARAM);
    asserteq(cg_init(&g, test_alloc, NULL), CG_OK);
    asserteq(cg_rows(gp), 0);
    asserteq(cg_ncols(gp), 0);
    asserteq(cg_top(gp), 0);
    cg_free(&g);
}

TEST(free_null) { cg_free(NULL); }

TEST(free_empty) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_free(&g);
}

TEST(init_default_alloc) {
    cg_Grid g;
    asserteq(cg_init(&g, NULL, NULL), CG_OK);
    asserteq(cg_begin(&g, 0, 2, 3), CG_OK);
    cg_put(&g, 0, 0, 'X', 0);
    asserteq(cg_cell(&g, 0, 0, NULL), 'X');
    cg_free(&g);
}

/* ================================================================== */
/*  cg_begin                                                           */
/* ================================================================== */

TEST(begin_first) {
    cg_Grid g, *gp = &g;
    cg_init(&g, test_alloc, NULL);
    asserteq(cg_begin(&g, 5, 2, 3), CG_OK);
    asserteq(cg_rows(gp), 2);
    asserteq(cg_ncols(gp), 3);
    asserteq(cg_top(gp), 5);
    cg_free(&g);
}

TEST(begin_zero) {
    cg_Grid g, *gp = &g;
    cg_init(&g, test_alloc, NULL);
    asserteq(cg_begin(&g, 0, 0, 5), CG_ERRPARAM);
    asserteq(cg_begin(&g, 0, 5, 0), CG_ERRPARAM);
    asserteq(cg_rows(gp), 0);
    cg_free(&g);
}

TEST(begin_null) { asserteq(cg_begin(NULL, 0, 2, 3), CG_ERRPARAM); }

TEST(begin_oom_init) {
    int     cnt = 0;
    cg_Grid g, *gp = &g;
    cg_init(&g, oom_alloc, &cnt);
    asserteq(cg_begin(&g, 0, 2, 3), CG_ERRMEM);
    asserteq(cg_rows(gp), 0);
    cg_free(&g);
}

TEST(begin_oom_resize) {
    int     cnt = 0;
    cg_Grid g, *gp = &g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'A', 0);
    g.allocf = oom_alloc, g.ud = &cnt;
    asserteq(cg_begin(&g, 0, 4, 5), CG_ERRMEM);
    asserteq(cg_rows(gp), 2);
    asserteq(cg_ncols(gp), 3);
    asserteq(cg_cell(&g, 0, 0, NULL), 'A');
    cg_free(&g);
}

static int cw_double(void *ud, int cp) {
    (void)ud;
    return cp < 0x80 ? 1 : 2;
}

TEST(begin_resize_grow) {
    cg_Grid g, *gp = &g;
    cg_init(&g, test_alloc, NULL);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'A', 1);
    cg_begin(&g, 1, 4, 5);
    asserteq(cg_rows(gp), 4);
    asserteq(cg_ncols(gp), 5);
    asserteq(cg_top(gp), 1);
    asserteq(cg_cell(&g, 0, 0, NULL), 'A');
    cg_free(&g);
}

TEST(begin_resize_shrink) {
    cg_Grid g, *gp = &g;
    cg_init(&g, test_alloc, NULL);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_begin(&g, 0, 4, 5);
    cg_put(&g, 0, 2, 'B', 1);
    cg_begin(&g, 2, 2, 3);
    asserteq(cg_rows(gp), 2);
    asserteq(cg_ncols(gp), 3);
    asserteq(cg_top(gp), 2);
    asserteq(cg_cell(&g, 0, 2, NULL), 'B');
    cg_free(&g);
}

TEST(begin_same) {
    cg_Grid g, *gp = &g;
    cg_init(&g, test_alloc, NULL);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'X', 1);
    cg_begin(&g, 10, 2, 3);
    asserteq(cg_rows(gp), 2);
    asserteq(cg_top(gp), 10);
    asserteq(cg_cell(&g, 0, 0, NULL), 'X');
    cg_free(&g);
}

TEST(begin_sametop) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'a', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 2, 3);
    asserteq(cg_isdirty(&g, 0, 0), 0);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_setwcwidth / cg_clear                                            */
/* ================================================================== */

TEST(wcwidth_set) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 1);
    asserteq(cg_cell(&g, 0, 0, NULL), 0x4e2d);
    asserteq(cg_cell(&g, 0, 1, NULL), -1);
    cg_free(&g);
}

TEST(clear_set) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_put(&g, 0, 0, 'X', 0);
    cg_clear(&g);
    asserteq(cg_cell(&g, 0, 0, NULL), ' ');
    cg_free(&g);
}

TEST(clear_null) { cg_clear(NULL); }

TEST(clear_empty) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
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
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 1, 'a', 7);
    asserteq(cg_cell(&g, 0, 1, &st), 'a');
    asserteq(st, 7);
    cg_free(&g);
}

TEST(put_widechar) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 2);
    asserteq(cg_cell(&g, 0, 0, NULL), 0x4e2d);
    asserteq(cg_cell(&g, 0, 1, NULL), -1);
    cg_free(&g);
}

TEST(put_truncate) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 2, 0x4e2d, 1);
    asserteq(cg_cell(&g, 0, 2, NULL), '>');
    cg_free(&g);
}

TEST(put_overwrite_left) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 1);
    cg_put(&g, 0, 1, 'a', 1);
    asserteq(cg_cell(&g, 0, 0, NULL), 0x20);
    asserteq(cg_cell(&g, 0, 1, NULL), 'a');
    cg_free(&g);
}

TEST(put_overwrite_right) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 1);
    cg_put(&g, 0, 0, 'a', 1);
    asserteq(cg_cell(&g, 0, 0, NULL), 'a');
    asserteq(cg_cell(&g, 0, 1, NULL), 0x20);
    cg_free(&g);
}

TEST(put_params) {
    cg_Grid g, *gp = NULL;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, -1, 0, 'a', 0);
    cg_put(&g, 2, 0, 'a', 0);
    cg_put(&g, 0, -1, 'a', 0);
    cg_put(&g, 0, 3, 'a', 0);
    cg_put(gp, 0, 0, 'a', 0);
    asserteq(cg_cell(&g, 0, 0, NULL), ' ');
    cg_free(&g);
}

TEST(put_same) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'X', 0);
    cg_freeze(&g);
    cg_put(&g, 0, 0, 'X', 0);
    asserteq(cg_isdirty(&g, 0, 0), 0);
    cg_free(&g);
}

TEST(put_transparent_narrow) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 7);
    cg_put(&g, 0, 0, 'B', CG_TRANSPARENT);
    asserteq(cg_cell(&g, 0, 0, &st), 'B');
    asserteq(st, 7);
    cg_free(&g);
}

TEST(put_transparent_wide) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 7);
    cg_put(&g, 0, 0, 0x4e2d, CG_TRANSPARENT);
    asserteq(cg_cell(&g, 0, 0, &st), 0x4e2d);
    asserteq(st, 7);
    asserteq(cg_cell(&g, 0, 1, &st), -1);
    asserteq(st, 7);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_clearrow                                                        */
/* ================================================================== */

TEST(clearrow_range) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 1, 0, 'a', 1);
    cg_put(&g, 1, 1, 'b', 1);
    cg_put(&g, 1, 2, 'c', 1);
    cg_clearrow(&g, 1, 1, 3);
    asserteq(cg_cell(&g, 1, 0, NULL), 'a');
    asserteq(cg_cell(&g, 1, 1, NULL), ' ');
    asserteq(cg_cell(&g, 1, 2, NULL), ' ');
    asserteq(cg_cell(&g, 1, 3, NULL), ' ');
    cg_free(&g);
}

TEST(clearrow_params) {
    cg_Grid g, *gp = NULL;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_clearrow(&g, -1, 0, 1);
    cg_clearrow(&g, 2, 0, 1);
    cg_clearrow(&g, 0, 1, 1);
    cg_clearrow(&g, 0, 2, 1);
    cg_clearrow(&g, 0, -1, 2);
    cg_clearrow(&g, 0, 2, -1);
    cg_clearrow(&g, 0, 2, 10);
    cg_clearrow(gp, 0, 0, 1);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_fill                                                            */
/* ================================================================== */

TEST(fill_set) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 5);
    cg_fill(&g, 0, 1, 4, '.');
    asserteq(cg_cell(&g, 0, 1, NULL), '.');
    asserteq(cg_cell(&g, 0, 2, NULL), '.');
    asserteq(cg_cell(&g, 0, 3, NULL), '.');
    asserteq(cg_cell(&g, 0, 0, NULL), 0x20);
    cg_free(&g);
}

TEST(fill_params) {
    cg_Grid g, *gp = NULL;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_fill(&g, -1, 0, 2, '.');
    cg_fill(&g, 2, 0, 2, '.');
    cg_fill(&g, 0, -1, 2, '.');
    cg_fill(&g, 0, 5, 10, '.');
    cg_fill(&g, 0, 2, 2, '.');
    cg_fill(&g, 0, 2, -1, '.');
    cg_fill(&g, 0, 3, 2, '.');
    cg_fill(gp, 0, 0, 1, '.');
    cg_free(&g);
}

/* ================================================================== */
/*  cg_span                                                            */
/* ================================================================== */

TEST(span_set) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_span(&g, 0, 1, 3, 3);
    asserteq(cg_cell(&g, 0, 1, &st), 0x20);
    asserteq(st, 3);
    asserteq(cg_cell(&g, 0, 2, &st), 0x20);
    asserteq(st, 3);
    cg_cell(&g, 0, 0, &st);
    asserteq(st, 0);
    cg_free(&g);
}

TEST(span_params) {
    cg_Grid g, *gp = NULL;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_span(&g, -1, 0, 2, 1);
    cg_span(&g, 2, 0, 2, 1);
    cg_span(&g, 0, -1, 2, 1);
    cg_span(&g, 0, 0, 0, 1);
    cg_span(&g, 0, 3, 6, 7);
    cg_span(&g, 0, 2, -1, 1);
    cg_span(&g, 0, 3, 2, 1);
    cg_span(gp, 0, 0, 1, 1);
    cg_free(&g);
}

TEST(span_backmatch) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_span(&g, 0, 0, 2, 5);
    cg_freeze(&g);
    cg_span(&g, 0, 0, 2, 5);
    asserteq(cg_isdirty(&g, 0, 0), 0);
    cg_free(&g);
}

TEST(span_transparent) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 7);
    cg_span(&g, 0, 0, 2, CG_TRANSPARENT);
    asserteq(cg_cell(&g, 0, 0, &st), 'A');
    asserteq(st, 7);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_putslice                                                        */
/* ================================================================== */

TEST(putline_ascii) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    cg_setwcwidth(&g, cw_double, NULL);
    asserteq(cg_putslice(&g, 0, 0, SL("ab"), 1), 2);
    asserteq(cg_cell(&g, 0, 0, NULL), 'a');
    asserteq(cg_cell(&g, 0, 1, NULL), 'b');
    assert_diff(
            &g, "[M 0 0][T 1][P 'a'][M 0 1][P 'b'][M 0 2][T 0][F 4 0x20][F]");
    cg_free(&g);
}

TEST(putline_transparent) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_span(&g, 0, 0, 2, 7);
    asserteq(cg_putslice(&g, 0, 0, SL("BC"), CG_TRANSPARENT), 2);
    asserteq(cg_cell(&g, 0, 0, &st), 'B');
    asserteq(st, 7);
    asserteq(cg_cell(&g, 0, 1, &st), 'C');
    asserteq(st, 7);
    cg_free(&g);
}

TEST(putline_wide) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    cg_setwcwidth(&g, cw_double, NULL);
    asserteq(cg_putslice(&g, 0, 0, SL("\xe4\xb8\xad"), 1), 2);
    asserteq(cg_cell(&g, 0, 0, NULL), 0x4e2d);
    asserteq(cg_cell(&g, 0, 1, NULL), -1);
    assert_diff(&g, "[M 0 0][T 1][P 0x4e2d][M 0 2][T 0][F 4 0x20][F]");
    cg_free(&g);
}

TEST(putline_params) {
    char     buf[4];
    cg_Slice es = { NULL, NULL };
    cg_Grid  g, *gp = NULL;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    asserteq(cg_putslice(&g, 0, 0, es, 1), 0);
    asserteq(cg_putslice(&g, -1, 0, SL("a"), 1), 0);
    asserteq(cg_putslice(&g, 2, 0, SL("a"), 1), 0);
    asserteq(cg_putslice(&g, 0, -1, SL("a"), 1), -1);
    asserteq(cg_putslice(&g, 0, 4, SL("a"), 1), 4);
    asserteq(cg_putslice(gp, 0, 0, SL("a"), 1), 0);
    buf[0] = test_byte(0x80);
    buf[1] = 'a';
    buf[2] = 'b';
    buf[3] = '\0';
    asserteq(cg_putslice(&g, 0, 0, SL(buf), 1), 2);
    asserteq(cg_cell(&g, 0, 0, NULL), 'a');
    asserteq(cg_cell(&g, 0, 1, NULL), 'b');
    cg_free(&g);
}

TEST(putline_skip) {
    char    buf[3];
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    buf[0] = test_byte(0x80);
    buf[1] = 'X';
    buf[2] = '\0';
    asserteq(cg_putslice(&g, 0, 0, SL(buf), 1), 1);
    asserteq(cg_cell(&g, 0, 0, NULL), 'X');
    asserteq(cg_cell(&g, 0, 1, NULL), 0x20);
    assert_diff(
            &g, "[M 0 0][T 1][P 'X'][M 0 1][T 0][P 0x20][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(putline_2byte) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    asserteq(cg_putslice(&g, 0, 0, SL("\xc3\x80"), 1), 2);
    asserteq(cg_cell(&g, 0, 0, NULL), 0xc0);
    asserteq(cg_cell(&g, 0, 1, NULL), -1);
    assert_diff(&g, "[M 0 0][T 1][P 0xc0][M 0 2][T 0][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(putline_4byte) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    asserteq(cg_putslice(&g, 0, 0, SL("\xf0\x90\x80\x80"), 1), 2);
    asserteq(cg_cell(&g, 0, 0, NULL), 0x10000);
    asserteq(cg_cell(&g, 0, 1, NULL), -1);
    assert_diff(&g, "[M 0 0][T 1][P 0x10000][M 0 2][T 0][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(diff_wide_nocont) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 0x4e2d, 1);
    /* continuation cell (0,1) must NOT appear in diff */
    assert_diff(&g, "[M 0 0][T 1][P 0x4e2d][M 0 2][T 0][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(putline_atend) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    asserteq(cg_putslice(&g, 0, 3, SL("abc"), 0), 3);
    cg_free(&g);
}

/* ================================================================== */
/*  getters                                                            */
/* ================================================================== */

TEST(getter_values) {
    cg_Grid g, *gp = &g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 3, 2, 5);
    asserteq(cg_rows(gp), 2);
    asserteq(cg_ncols(gp), 5);
    asserteq(cg_top(gp), 3);
    cg_free(&g);
}

TEST(getter_cell) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 1, 'Z', 4);
    asserteq(cg_cell(&g, 0, 1, &st), 'Z');
    asserteq(st, 4);
    cg_free(&g);
}

TEST(getter_back) {
    unsigned st;
    cg_Grid  g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    asserteq(cg_back(&g, 0, 0, &st), ' ');
    asserteq(st, 0);
    cg_free(&g);
}

TEST(getter_params) {
    unsigned st;
    cg_Grid  g, *gp = NULL;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    asserteq(cg_cell(&g, -1, 0, NULL), 0);
    asserteq(cg_cell(&g, 2, 0, NULL), 0);
    asserteq(cg_cell(&g, 0, -1, NULL), 0);
    asserteq(cg_cell(&g, 0, 3, NULL), 0);
    asserteq(cg_cell(gp, 0, 0, NULL), 0);
    st = 9;
    asserteq(cg_cell(&g, -1, 0, &st), 0);
    asserteq(st, 0);
    asserteq(cg_back(&g, -1, 0, NULL), 0);
    asserteq(cg_back(&g, 2, 0, NULL), 0);
    asserteq(cg_back(&g, 0, -1, NULL), 0);
    asserteq(cg_back(&g, 0, 3, NULL), 0);
    asserteq(cg_back(gp, 0, 0, NULL), 0);
    st = 9;
    asserteq(cg_back(&g, -1, 0, &st), 0);
    asserteq(st, 0);
    asserteq(cg_isdirty(&g, -1, 0), 0);
    asserteq(cg_isdirty(&g, 2, 0), 0);
    asserteq(cg_isdirty(&g, 0, -1), 0);
    asserteq(cg_isdirty(&g, 0, 3), 0);
    asserteq(cg_isdirty(gp, 0, 0), 0);
    cg_free(&g);
}

/* ================================================================== */
/*  cg_isdirty / cg_freeze                                             */
/* ================================================================== */

TEST(dirty_track) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 1, 'a', 0);
    asserteq(cg_isdirty(&g, 0, 1), 1);
    cg_free(&g);
}

TEST(freeze_null) { cg_freeze(NULL); }

TEST(freeze_empty) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_freeze(&g);
    cg_free(&g);
}

TEST(freeze_copies) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'X', 5);
    asserteq(cg_isdirty(&g, 0, 0), 1);
    asserteq(cg_back(&g, 0, 0, NULL), ' ');
    cg_freeze(&g);
    asserteq(cg_isdirty(&g, 0, 0), 0);
    asserteq(cg_back(&g, 0, 0, NULL), 'X');
    cg_free(&g);
}

TEST(freeze_clears_dirty) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'a', 0);
    cg_put(&g, 0, 1, 'b', 0);
    asserteq(cg_isdirty(&g, 0, 0), 1);
    asserteq(cg_isdirty(&g, 0, 1), 1);
    cg_freeze(&g);
    asserteq(cg_isdirty(&g, 0, 0), 0);
    asserteq(cg_isdirty(&g, 0, 1), 0);
    cg_free(&g);
}

/* ================================================================== */
/*  scroll tests                                                       */
/* ================================================================== */

TEST(begin_scroll_down) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_put(&g, 1, 0, 'B', 0);
    cg_put(&g, 2, 0, 'C', 0);
    cg_freeze(&g);
    cg_begin(&g, 1, 3, 4);
    asserteq(cg_cell(&g, 0, 0, NULL), 'B');
    asserteq(cg_cell(&g, 1, 0, NULL), 'C');
    cg_free(&g);
}

TEST(begin_scroll_up) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 1, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_put(&g, 1, 0, 'B', 0);
    cg_put(&g, 2, 0, 'C', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 3, 4);
    asserteq(cg_cell(&g, 1, 0, NULL), 'A');
    asserteq(cg_cell(&g, 2, 0, NULL), 'B');
    cg_free(&g);
}

TEST(scroll_up_expose) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 1, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_put(&g, 1, 0, 'B', 0);
    cg_put(&g, 2, 0, 'C', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 3, 4);
    asserteq(cg_cell(&g, 0, 0, NULL), ' ');
    asserteq(cg_cell(&g, 1, 0, NULL), 'A');
    cg_free(&g);
}

TEST(begin_scroll_then_same_top) {
    /* delta=0 right after a scroll frame must keep the ring offset:
     * the unchanged frame's diff must be empty, not a full redraw */
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_putslice(&g, 0, 0, SL("L1"), 0);
    cg_putslice(&g, 1, 0, SL("L2"), 0);
    cg_putslice(&g, 2, 0, SL("L3"), 0);
    cg_freeze(&g);
    cg_begin(&g, 1, 3, 4); /* scroll down 1 */
    cg_putslice(&g, 0, 0, SL("L2"), 0);
    cg_putslice(&g, 1, 0, SL("L3"), 0);
    cg_putslice(&g, 2, 0, SL("L4"), 0);
    assert_diff(&g, "[S 1 3 -1][M 2 0][P 'L'][M 2 1][P '4'][F]");
    cg_freeze(&g);
    cg_begin(&g, 1, 3, 4); /* same top: nothing may change */
    cg_putslice(&g, 0, 0, SL("L2"), 0);
    cg_putslice(&g, 1, 0, SL("L3"), 0);
    cg_putslice(&g, 2, 0, SL("L4"), 0);
    assert_diff(&g, "[F]");
    cg_free(&g);
}

/* ================================================================== */
/*  diff tests                                                         */
/* ================================================================== */

TEST(diff_empty) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_freeze(&g);
    assert_diff(&g, "[F]");
    cg_free(&g);
}

TEST(diff_emptygrid) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    asserteq(cg_diff(&g, NULL), CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_null_grid) {
    cg_Diff d;
    cg_Grid g;
    memset(&d, 0, sizeof(d));
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 1);
    asserteq(cg_diff(NULL, &d), CG_ERRPARAM);
    asserteq(cg_diff(&g, NULL), CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_all_dirty) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
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

TEST(diff_rep_style) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 3);
    cg_put(&g, 0, 0, 'a', 1);
    cg_put(&g, 0, 1, 'a', 2);
    assert_diff(
            &g,
            "[M 0 0][T 1][P 'a'][M 0 1][T 2][P 'a']"
            "[M 0 2][T 0][P 0x20][F]");
    cg_free(&g);
}

TEST(diff_scroll) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 3, 4);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_freeze(&g);
    cg_begin(&g, 1, 3, 4);
    /* exposed rows are cleared with back: blank grid stays blank */
    assert_diff(&g, "[S 1 3 -1][F]");
    cg_free(&g);
}

TEST(diff_scroll_expose_sd) {
    /* viewport up (delta>0, SD): top row is physically blank after the
     * scroll — every cell must be redrawn, even those matching back */
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 1, 3, 6);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_putslice(&g, 0, 0, SL(" 866"), 0);
    cg_putslice(&g, 1, 0, SL(" 867"), 0);
    cg_putslice(&g, 2, 0, SL(" 868"), 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 3, 6); /* delta = +1 */
    cg_putslice(&g, 0, 0, SL(" 865"), 0);
    cg_putslice(&g, 1, 0, SL(" 866"), 0);
    cg_putslice(&g, 2, 0, SL(" 867"), 0);
    assert_diff(
            &g,
            "[S 1 3 1]"
            "[M 0 0][P 0x20][M 0 1][P '8'][M 0 2][P '6'][M 0 3][P '5']"
            "[F]");
    cg_free(&g);
}

TEST(diff_scroll_expose_su) {
    /* viewport down (delta<0, SU): bottom row is physically blank after
     * the scroll — every cell must be redrawn */
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 3, 6);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_putslice(&g, 0, 0, SL(" 865"), 0);
    cg_putslice(&g, 1, 0, SL(" 866"), 0);
    cg_putslice(&g, 2, 0, SL(" 867"), 0);
    cg_freeze(&g);
    cg_begin(&g, 1, 3, 6); /* delta = -1 */
    cg_putslice(&g, 0, 0, SL(" 866"), 0);
    cg_putslice(&g, 1, 0, SL(" 867"), 0);
    cg_putslice(&g, 2, 0, SL(" 868"), 0);
    assert_diff(
            &g,
            "[S 1 3 -1]"
            "[M 2 0][P 0x20][M 2 1][P '8'][M 2 2][P '6'][M 2 3][P '8']"
            "[F]");
    cg_free(&g);
}

TEST(diff_bigjump) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
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
    cg_init(&g, test_alloc, NULL);
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
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'a', 0);
    cg_freeze(&g);
    cg_clear(&g);
    cg_begin(&g, 0, 2, 3);
    assert_diff(
            &g,
            "[M 0 0][P 0x20][P 0x20][P 0x20]"
            "[M 1 0][P 0x20][P 0x20][P 0x20][F]");
    cg_free(&g);
}

TEST(diff_style_change) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
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
    cg_init(&g, test_alloc, NULL);
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
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    memset(&d, 0, sizeof(d));
    d.fill = err_cb_fill;
    asserteq(cg_diff(&g, &d), CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_null) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    memset(&d, 0, sizeof(d));
    asserteq(cg_diff(&g, &d), CG_OK);
    cg_free(&g);
}

TEST(diff_cb_scroll_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_freeze(&g);
    cg_begin(&g, 1, 2, 3);
    memset(&d, 0, sizeof(d));
    d.scroll = err_cb_sn;
    asserteq(cg_diff(&g, &d), CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_scroll_null) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_freeze(&g);
    cg_begin(&g, 1, 2, 3);
    memset(&d, 0, sizeof(d));
    asserteq(cg_diff(&g, &d), CG_OK);
    cg_free(&g);
}

TEST(diff_cb_move_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 2);
    memset(&d, 0, sizeof(d));
    d.move = err_cb_mv;
    asserteq(cg_diff(&g, &d), CG_ERRPARAM);
    cg_free(&g);
}

static int err_cb_st0(cg_Diff *D, unsigned st) {
    (void)D;
    return st == 0 ? CG_ERRPARAM : CG_OK;
}

TEST(diff_cb_style_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 2);
    cg_put(&g, 0, 0, 'x', 7);
    memset(&d, 0, sizeof(d));
    d.style = err_cb_st;
    asserteq(cg_diff(&g, &d), CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_style_null) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 1);
    cg_put(&g, 0, 0, 'x', 7);
    memset(&d, 0, sizeof(d));
    asserteq(cg_diff(&g, &d), CG_OK);
    cg_free(&g);
}

TEST(diff_cb_put_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 2);
    cg_put(&g, 0, 0, 'x', 0);
    memset(&d, 0, sizeof(d));
    d.put = err_cb_put;
    asserteq(cg_diff(&g, &d), CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_finish_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_freeze(&g);
    memset(&d, 0, sizeof(d));
    d.finish = err_cb;
    asserteq(cg_diff(&g, &d), CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_cb_style0_err) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 1);
    cg_put(&g, 0, 0, 'x', 7);
    memset(&d, 0, sizeof(d));
    d.style = err_cb_st0;
    asserteq(cg_diff(&g, &d), CG_ERRPARAM);
    cg_free(&g);
}

TEST(diff_fill_min_pass) {
    cg_Diff d;
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 1, 6);
    memset(&d, 0, sizeof(d));
    d.fill_min = 2;
    asserteq(cg_diff(&g, &d), CG_OK);
    cg_free(&g);
}

/* ================================================================== */
/*  column conversion (cg_dcol / cg_byte / cg_dcols)                   */
/* ================================================================== */

/* grid with optional width callback; tabstop via cg_settabstop */
static void cc_init(cg_Grid *g, int wide, int ts) {
    cg_init(g, test_alloc, NULL);
    if (wide) cg_setwcwidth(g, cw_double, NULL);
    if (ts > 1) cg_settabstop(g, ts);
}

#define ZH "\xe4\xb8\xad" /* 中 (CJK, width 2 via cw_double) */
#define A2 "\xc3\x80"     /* U+0080 (width 2 via cw_double) */

TEST(cols_ascii) {
    cg_Grid g;
    cc_init(&g, 0, 4);
    asserteq(cg_cols(&g, 0, SL("abc")), 3);
    asserteq(cg_cols(&g, 0, cg_slice("abc", 0)), 0);
    asserteq(cg_cols(&g, 0, cg_slice("abc", 1)), 1);
    asserteq(cg_cols(&g, 0, cg_slice("abc", 2)), 2);
    cg_free(&g);
}

TEST(cols_empty) {
    cg_Grid g;
    cc_init(&g, 0, 4);
    asserteq(cg_cols(&g, 0, SL("")), 0);
    cg_free(&g);
}

TEST(cols_tab) {
    cg_Grid g;
    cc_init(&g, 0, 4);
    asserteq(cg_cols(&g, 0, cg_slice("\ta", 1)), 4);
    asserteq(cg_cols(&g, 0, SL("\ta")), 5);
    asserteq(cg_cols(&g, 0, cg_slice("a\tb", 2)), 4);
    asserteq(cg_cols(&g, 0, cg_slice("ab\tc", 3)), 4);
    asserteq(cg_cols(&g, 0, cg_slice("abc\t", 4)), 4);
    cg_free(&g);
}

TEST(cols_tabfold) {
    cg_Grid g;
    cc_init(&g, 0, 2);
    asserteq(cg_cols(&g, 0, cg_slice("a\tb", 2)), 2);
    cg_settabstop(&g, 1);
    asserteq(cg_cols(&g, 0, cg_slice("a\tb", 2)), 2);
    cg_settabstop(&g, 0);
    asserteq(cg_cols(&g, 0, cg_slice("a\tb", 2)), 2);
    cg_free(&g);
}

TEST(cols_wide) {
    cg_Grid g;
    cc_init(&g, 1, 4);
    asserteq(cg_cols(&g, 0, cg_slice(ZH "a", 3)), 2);
    asserteq(cg_cols(&g, 0, SL(ZH "a")), 3);
    cg_free(&g);
}

TEST(cols_wide_nocb) {
    cg_Grid g;
    cc_init(&g, 0, 4);
    asserteq(cg_cols(&g, 0, cg_slice(ZH "a", 3)), 1);
    cg_free(&g);
}

TEST(cols_cont) {
    char    buf[3];
    cg_Grid g;
    cc_init(&g, 1, 4);
    buf[0] = test_byte(0x80);
    buf[1] = 'a';
    buf[2] = '\0';
    asserteq(cg_cols(&g, 0, cg_slice(buf, 1)), 0); /* stray cont: skipped */
    asserteq(cg_cols(&g, 0, cg_slice(buf, 2)), 1);
    cg_free(&g);
}

TEST(cols_2byte) {
    cg_Grid g;
    cc_init(&g, 1, 4);
    asserteq(cg_cols(&g, 0, cg_slice(A2 "a", 2)), 2);
    asserteq(cg_cols(&g, 0, cg_slice(A2 "a", 3)), 3);
    cg_free(&g);
}

TEST(cols_start) {
    /* start shifts the tab alignment: tab at col 2 -> 2 wide (to 4) */
    cg_Grid g;
    cc_init(&g, 0, 4);
    asserteq(cg_cols(&g, 2, SL("\t")), 4);
    asserteq(cg_cols(&g, 0, SL("\t")), 4);
    asserteq(cg_cols(&g, 3, SL("\tb")), 5);
    cg_free(&g);
}

TEST(byte_ascii) {
    cg_Grid g;
    cc_init(&g, 0, 4);
    asserteq(cg_byte(&g, 0, SL("abc"), 0), 0);
    asserteq(cg_byte(&g, 0, SL("abc"), 2), 2);
    asserteq(cg_byte(&g, 0, SL("abc"), 9), 3);
    asserteq(cg_byte(&g, 0, SL("abc"), -1), 0);
    cg_free(&g);
}

TEST(byte_tab) {
    cg_Grid g;
    cc_init(&g, 0, 4);
    asserteq(cg_byte(&g, 0, SL("a\tb"), 1), 1);
    asserteq(cg_byte(&g, 0, SL("a\tb"), 3), 1); /* inside tab: tab start */
    asserteq(cg_byte(&g, 0, SL("a\tb"), 4), 2);
    asserteq(cg_byte(&g, 0, SL("a\tb"), 5), 3);
    asserteq(cg_byte(&g, 0, SL("a\tb"), 6), 3);
    asserteq(cg_byte(&g, 0, SL("\ta"), 2), 0);  /* lead tab, col 0 */
    asserteq(cg_byte(&g, 0, SL("\ta"), 4), 1);
    cg_settabstop(&g, 1);
    asserteq(cg_byte(&g, 0, SL("\ta"), 1), 1); /* tabstop fold -> 1 */
    cg_free(&g);
}

TEST(byte_wide) {
    cg_Grid g;
    cc_init(&g, 1, 4);
    asserteq(cg_byte(&g, 0, SL(ZH "a"), 1), 0); /* inside wide: char start */
    asserteq(cg_byte(&g, 0, SL(ZH "a"), 2), 3);
    asserteq(cg_byte(&g, 0, SL(ZH "a"), 3), 4); /* past end -> len */
    cg_free(&g);
}

TEST(byte_wide_nocb) {
    cg_Grid g;
    cc_init(&g, 0, 4);
    asserteq(cg_byte(&g, 0, SL(ZH "a"), 1), 3);
    cg_free(&g);
}

TEST(byte_cont) {
    char    buf[3];
    cg_Grid g;
    cc_init(&g, 0, 4);
    buf[0] = test_byte(0x80);
    buf[1] = 'a';
    buf[2] = '\0';
    asserteq(cg_byte(&g, 0, cg_slice(buf, 2), 1), 2); /* 'a' at col 0, col 1 = EOL */
    cg_free(&g);
}

TEST(byte_start) {
    /* col is relative to c: "	b" from col 2 -> tab 2..4, b at 4 */
    cg_Grid g;
    cc_init(&g, 0, 4);
    asserteq(cg_byte(&g, 2, SL("\tb"), 1), 0); /* col 3 inside tab */
    asserteq(cg_byte(&g, 2, SL("\tb"), 2), 1); /* col 4 = b start */
    asserteq(cg_byte(&g, 2, SL("\tb"), 3), 2); /* past end -> len */
    cg_free(&g);
}

/* truncated tail: cg_next advances one byte with width 1 (no stall) */
TEST(byte_trunc) {
    char    buf[3];
    cg_Grid g;
    cc_init(&g, 1, 4);
    buf[0] = test_byte(0xe4);
    buf[1] = test_byte(0xb8);
    buf[2] = '\0';
    asserteq(cg_byte(&g, 0, cg_slice(buf, 2), 5), 2);
    cg_free(&g);
}

TEST(next_ascii) {
    cg_Slice s = SL("abc");
    cg_Grid  g;
    cc_init(&g, 0, 4);
    asserteq(cg_next(&g, 0, &s), 1);
    asserteq(s.s - SL("abc").s, 1);
    asserteq(cg_next(&g, 0, &s), 1);
    asserteq(cg_next(&g, 0, &s), 1);
    asserteq(s.s == s.e, 1);
    asserteq(cg_next(&g, 0, &s), 0); /* empty: no advance */
    cg_free(&g);
}

TEST(next_wide) {
    cg_Slice s = SL(ZH "a");
    cg_Grid  g;
    cc_init(&g, 1, 4);
    asserteq(cg_next(&g, 0, &s), 2); /* 中: width 2, 3 bytes */
    asserteq(s.s - SL(ZH "a").s, 3);
    asserteq(cg_next(&g, 0, &s), 1);
    cg_free(&g);
}

TEST(next_tab) {
    cg_Slice s = SL("\ta");
    cg_Grid  g;
    cc_init(&g, 0, 4);
    asserteq(cg_next(&g, 0, &s), 4); /* tab at col 0: to col 4 */
    asserteq(cg_next(&g, 2, &s), 1);
    s = SL("\t");
    asserteq(cg_next(&g, 2, &s), 2); /* tab at col 2: to col 4 */
    cg_free(&g);
}

TEST(next_cont) {
    cg_Slice s = SL("\x80" "a");
    cg_Grid  g;
    cc_init(&g, 1, 4);
    asserteq(cg_next(&g, 0, &s), 0); /* stray continuation: skipped */
    asserteq(cg_next(&g, 0, &s), 1);
    cg_free(&g);
}

TEST(next_trunc) {
    char    buf[3];
    cg_Slice s;
    cg_Grid  g;
    cc_init(&g, 1, 4);
    buf[0] = test_byte(0xe4);
    buf[1] = test_byte(0xb8);
    buf[2] = '\0';
    s = cg_slice(buf, 2);
    asserteq(cg_next(&g, 0, &s), 1); /* truncated tail: 1 byte, width 1 */
    asserteq(cg_next(&g, 0, &s), 0); /* continuation byte: skipped */
    cg_free(&g);
}

/* iterating cg_next yields the per-char (byte, col) pairs dcols used
 * to provide: "a\t" ZH "b" -> (0,0) (1,1) (2,4) (5,6), end col 7 */
TEST(next_iter) {
    cg_Slice s = SL("a\t" ZH "b"), base = SL("a\t" ZH "b");
    cg_Grid  g;
    int      col = 0, k = 0;
    cc_init(&g, 1, 4);
    while (s.s < s.e) {
        int w = cg_next(&g, col, &s);
        if (k == 0) { asserteq((int)(s.s - base.s), 1); asserteq(col, 0); }
        if (k == 1) { asserteq((int)(s.s - base.s), 2); asserteq(col, 1); }
        if (k == 2) { asserteq((int)(s.s - base.s), 5); asserteq(col, 4); }
        if (k == 3) { asserteq((int)(s.s - base.s), 6); asserteq(col, 6); }
        col += w, k++;
    }
    asserteq(k, 4);
    asserteq(col, 7); /* end-of-line column */
    cg_free(&g);
}

TEST(settabstop_null) { cg_settabstop(NULL, 4); }

TEST(putline_tab) {
    cg_Grid g;
    cc_init(&g, 0, 4);
    cg_begin(&g, 0, 1, 8);
    asserteq(cg_putslice(&g, 0, 0, SL("a\tb"), 1), 5);
    asserteq(cg_cell(&g, 0, 0, NULL), 'a');
    asserteq(cg_cell(&g, 0, 1, NULL), ' ');
    asserteq(cg_cell(&g, 0, 3, NULL), ' ');
    asserteq(cg_cell(&g, 0, 4, NULL), 'b');
    cg_free(&g);
}

TEST(putline_tabfold) {
    cg_Grid g;
    cc_init(&g, 0, 1);
    cg_begin(&g, 0, 1, 8);
    /* tabstop <= 1: tab stays a literal tab char, width 1 (matches
     * the column math fold) */
    asserteq(cg_putslice(&g, 0, 0, SL("a\tb"), 1), 3);
    asserteq(cg_cell(&g, 0, 1, NULL), 9);
    asserteq(cg_cell(&g, 0, 2, NULL), 'b');
    cg_free(&g);
}

TEST(putline_tabedge) {
    cg_Grid g;
    cc_init(&g, 0, 4);
    cg_begin(&g, 0, 1, 5);
    asserteq(cg_putslice(&g, 0, 0, SL("a\tb"), 1), 5);
    asserteq(cg_cell(&g, 0, 4, NULL), 'b');
    /* trailing tab expands within the edge: "ab\t" covers cols 0-3 */
    asserteq(cg_putslice(&g, 0, 0, SL("ab\t"), 0), 4);
    asserteq(cg_cell(&g, 0, 3, NULL), ' ');
    cg_free(&g);
}

TEST(putline_tabmid) {
    /* tab after wide char: width folds to the tab stop from the
     * rendered column (2-wide char advances c by 2) */
    cg_Grid g;
    cc_init(&g, 1, 4);
    cg_begin(&g, 0, 1, 8);
    asserteq(cg_putslice(&g, 0, 0, SL(ZH "\t" "b"), 1), 5);
    asserteq(cg_cell(&g, 0, 4, NULL), 'b');
    cg_free(&g);
}

TEST(putslice_trunc) {
    char    buf[3];
    cg_Grid g;
    cc_init(&g, 1, 4);
    cg_begin(&g, 0, 1, 4);
    buf[0] = test_byte(0xe4);
    buf[1] = test_byte(0xb8);
    buf[2] = '\0';
    /* truncated 0xe4: single byte width 1, continuation skipped */
    asserteq(cg_putslice(&g, 0, 0, cg_slice(buf, 2), 1), 1);
    asserteq(cg_cell(&g, 0, 0, NULL), 0xe4);
    cg_free(&g);
}

#undef ZH
#undef A2

/* ================================================================== */
/*  edge cases                                                         */
/* ================================================================== */

TEST(empty_grid) {
    cg_Grid g;
    cg_init(&g, test_alloc, NULL);
    cg_put(&g, 0, 0, 'a', 0);
    cg_span(&g, 0, 0, 2, 1);
    cg_clearrow(&g, 0, 0, 2);
    cg_fill(&g, 0, 0, 2, '.');
    asserteq(cg_putslice(&g, 0, 0, SL("a"), 0), 0);
    asserteq(cg_cell(&g, 0, 0, NULL), 0);
    asserteq(cg_back(&g, 0, 0, NULL), 0);
    asserteq(cg_isdirty(&g, 0, 0), 0);
    cg_free(&g);
}

TEST(resize_cols) {
    cg_Grid g, *gp = &g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 2, 5);
    asserteq(cg_ncols(gp), 5);
    asserteq(cg_cell(&g, 0, 0, NULL), 'A');
    cg_free(&g);
}

TEST(resize_rows) {
    cg_Grid g, *gp = &g;
    cg_init(&g, test_alloc, NULL);
    cg_begin(&g, 0, 2, 3);
    cg_setwcwidth(&g, cw_double, NULL);
    cg_put(&g, 0, 0, 'A', 0);
    cg_freeze(&g);
    cg_begin(&g, 0, 4, 3);
    asserteq(cg_rows(gp), 4);
    asserteq(cg_cell(&g, 0, 0, NULL), 'A');
    cg_free(&g);
}

#include "cellgrid_test.gen.inc"
