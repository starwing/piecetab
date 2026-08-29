#define LC_STATIC_API
#include "bench.h"
#include "linecache.h"

#define LC_BENCH_NLINES      10000UL
#define LC_BENCH_NLINES_100K 100000UL
#define LC_BENCH_NLINES_1M   1000000UL
#define LC_BENCH_MAX_LINE    80UL
#define LC_BENCH_EDIT_N      10000UL
#define LC_BENCH_EDIT_100K   100000UL
#define LC_BENCH_EDIT_1M     1000000UL
#define LC_BENCH_MAX_DEL     8UL
#define LC_BENCH_MAX_INS     16UL
#define LC_BENCH_MAX_JUMP    512L
#define LC_BENCH_MAX_LINE_JUMP 16L

typedef struct lc_bench_ud {
    lc_State  *S;
    lc_Cache  *c;
    unsigned  *lens;
    size_t     nlines;
    size_t     total;
    size_t    *offs;
    size_t    *lines;
    lc_Delta  *advs;
    lc_Delta  *advlines;
    long       noffs;
    lc_Cursor  cur;
    lc_Cursor  rcur;
    long       idx;
    long       calls;
    size_t     sink;
    size_t     next;
    bench_Rand rng;
} lc_bench_ud;

static unsigned lc_bench_scan(void *ud, size_t pos) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    (void)pos;
    if (u->next >= u->nlines) return 0;
    return u->lens[u->next++];
}

static int lc_bench_offsets(lc_bench_ud *u, long seed, long n) {
    bench_Rand r;
    long       i;
    u->offs = (size_t *)malloc((size_t)n * sizeof(size_t));
    u->lines = (size_t *)malloc((size_t)n * sizeof(size_t));
    u->advs = (lc_Delta *)malloc((size_t)n * sizeof(lc_Delta));
    u->advlines = (lc_Delta *)malloc((size_t)n * sizeof(lc_Delta));
    if (u->offs == NULL || u->lines == NULL ||
        u->advs == NULL || u->advlines == NULL)
        return 0;
    bench_rand_seed(&r, seed);
    for (i = 0; i < n; ++i) {
        u->offs[i] = (size_t)bench_range(&r, 0, (long)u->total - 1);
        u->lines[i] = (size_t)bench_range(&r, 0, (long)u->nlines - 1);
        u->advs[i] = (lc_Delta)bench_range(&r, -LC_BENCH_MAX_JUMP,
                                           LC_BENCH_MAX_JUMP);
        u->advlines[i] = (lc_Delta)bench_range(&r, -LC_BENCH_MAX_LINE_JUMP,
                                               LC_BENCH_MAX_LINE_JUMP);
    }
    u->noffs = n;
    return 1;
}

static int lc_bench_build(lc_bench_ud *u, lc_State *S, long seed,
                          size_t nlines) {
    bench_Rand r;
    size_t     i, total = 0;
    u->S = S;
    u->c = lc_newcache(S);
    if (u->c == NULL) return 0;
    u->lens = (unsigned *)malloc(nlines * sizeof(unsigned));
    if (u->lens == NULL) return 0;
    u->nlines = nlines;
    bench_rand_seed(&r, seed);
    for (i = 0; i < nlines; ++i) {
        unsigned len = (unsigned)bench_range(&r, 1, (long)LC_BENCH_MAX_LINE);
        u->lens[i] = len;
        total += len;
    }
    u->total = total;
    u->next = 0;
    if (lc_scan(u->c, lc_bench_scan, u) != LC_OK) return 0;
    return lc_bench_offsets(u, seed + 1, 4096);
}

static int lc_bench_scan_build(lc_bench_ud *u, lc_State *S, long seed,
                               size_t nlines) {
    bench_Rand r;
    size_t     i, total = 0;
    u->S = S;
    u->c = NULL;
    u->lens = (unsigned *)malloc(nlines * sizeof(unsigned));
    if (u->lens == NULL) return 0;
    u->nlines = nlines;
    bench_rand_seed(&r, seed);
    for (i = 0; i < nlines; ++i) {
        unsigned len = (unsigned)bench_range(&r, 1, (long)LC_BENCH_MAX_LINE);
        u->lens[i] = len;
        total += len;
    }
    u->total = total;
    u->noffs = 0;
    return 1;
}

static void lc_bench_ud_free(lc_bench_ud *u) {
    if (u == NULL) return;
    if (u->c != NULL) lc_delcache(u->S, u->c);
    if (u->S != NULL) lc_close(u->S);
    free(u->lens);
    free(u->offs);
    free(u->lines);
    free(u->advs);
    free(u->advlines);
    free(u);
}

static int setup_nav_n(void **ud, const bench_Params *p, size_t nlines) {
    lc_bench_ud *u = (lc_bench_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    u->S = lc_open(NULL, NULL);
    if (u->S == NULL) {
        free(u);
        return 0;
    }
    if (!lc_bench_build(u, u->S, p->seed, nlines)) {
        lc_bench_ud_free(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_nav(void **ud, const bench_Params *p) {
    return setup_nav_n(ud, p, LC_BENCH_NLINES);
}

static int setup_nav_100k(void **ud, const bench_Params *p) {
    return setup_nav_n(ud, p, LC_BENCH_NLINES_100K);
}

static int setup_nav_1m(void **ud, const bench_Params *p) {
    return setup_nav_n(ud, p, LC_BENCH_NLINES_1M);
}

static int setup_scan_n(void **ud, const bench_Params *p, size_t nlines) {
    lc_bench_ud *u = (lc_bench_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    u->S = lc_open(NULL, NULL);
    if (u->S == NULL) {
        free(u);
        return 0;
    }
    if (!lc_bench_scan_build(u, u->S, p->seed, nlines)) {
        lc_bench_ud_free(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_scan(void **ud, const bench_Params *p) {
    return setup_scan_n(ud, p, LC_BENCH_NLINES);
}

static int setup_scan_100k(void **ud, const bench_Params *p) {
    return setup_scan_n(ud, p, LC_BENCH_NLINES_100K);
}

static int setup_scan_1m(void **ud, const bench_Params *p) {
    return setup_scan_n(ud, p, LC_BENCH_NLINES_1M);
}

static void teardown(void *ud) {
    lc_bench_ud_free((lc_bench_ud *)ud);
}

static long lc_bench_calls_scan(void *ud) {
    return (long)((lc_bench_ud *)ud)->nlines;
}

static int run_scan(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        lc_Cache *c = lc_newcache(u->S);
        if (c == NULL) return 0;
        u->next = 0;
        if (lc_scan(c, lc_bench_scan, u) != LC_OK) {
            lc_delcache(u->S, c);
            return 0;
        }
        u->sink += lc_bytes(c);
        lc_delcache(u->S, c);
    }
    return 1;
}

static int run_seek(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t off = u->offs[i % u->noffs];
        if (lc_seek(&u->cur, u->c, off) != LC_OK) return 0;
        u->sink += lc_offset(&u->cur);
    }
    return 1;
}

static int run_seekline(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t line = u->lines[i % u->noffs];
        if (lc_seekline(&u->cur, u->c, line) != LC_OK) return 0;
        u->sink += lc_line(&u->cur);
    }
    return 1;
}

static int run_locate(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    if (lc_seek(&u->cur, u->c, 0) != LC_OK) return 0;
    for (i = 0; i < iters; ++i) {
        size_t off = u->offs[i % u->noffs];
        if (lc_locate(&u->cur, off) != LC_OK) return 0;
        u->sink += lc_offset(&u->cur);
    }
    return 1;
}

static int run_locline(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    if (lc_seekline(&u->cur, u->c, 0) != LC_OK) return 0;
    for (i = 0; i < iters; ++i) {
        size_t line = u->lines[i % u->noffs];
        if (lc_locline(&u->cur, line) != LC_OK) return 0;
        u->sink += lc_line(&u->cur);
    }
    return 1;
}

static int run_advance(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    if (lc_seek(&u->cur, u->c, u->offs[0]) != LC_OK) return 0;
    for (i = 0; i < iters; ++i) {
        if (lc_advance(&u->cur, u->advs[i % u->noffs]) != LC_OK) return 0;
        u->sink += lc_offset(&u->cur);
    }
    return 1;
}

static int run_advline(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    if (lc_seekline(&u->cur, u->c, 0) != LC_OK) return 0;
    for (i = 0; i < iters; ++i) {
        if (lc_advline(&u->cur, u->advlines[i % u->noffs]) != LC_OK) return 0;
        u->sink += lc_line(&u->cur);
    }
    return 1;
}

static int run_linelen(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t line = u->lines[i % u->noffs];
        if (lc_seekline(&u->cur, u->c, line) != LC_OK) return 0;
        u->sink += lc_linelen(&u->cur);
    }
    return 1;
}

static int run_markbreak(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t   off = u->offs[i % u->noffs];
        unsigned len = (unsigned)(1 + (i % (long)LC_BENCH_MAX_LINE));
        if (lc_seek(&u->cur, u->c, off) != LC_OK) return 0;
        if (lc_markbreak(&u->cur, len) != LC_OK) return 0;
        u->sink += lc_line(&u->cur);
    }
    return 1;
}

static int run_splice(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t off = u->offs[i % u->noffs];
        size_t del = (size_t)(i % (long)LC_BENCH_MAX_DEL);
        unsigned ins = (unsigned)(i % (long)LC_BENCH_MAX_INS);
        if (lc_seek(&u->cur, u->c, off) != LC_OK) return 0;
        if (lc_splice(&u->cur, del, ins) != LC_OK) return 0;
        u->sink += lc_offset(&u->cur);
    }
    return 1;
}

static int run_remove(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t off = u->offs[i % u->noffs];
        size_t len = (size_t)(1 + (i % 32));
        if (lc_seek(&u->cur, u->c, off) != LC_OK) return 0;
        if (lc_seek(&u->rcur, u->c, off + len) != LC_OK) return 0;
        if (lc_remove(&u->cur, &u->rcur) != LC_OK) return 0;
        u->sink += lc_offset(&u->cur);
    }
    return 1;
}

static int run_append(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t   off = u->offs[i % u->noffs];
        unsigned e = (unsigned)(i % (long)LC_BENCH_MAX_INS);
        if (lc_seek(&u->cur, u->c, off) != LC_OK) return 0;
        if (lc_append(&u->cur, e, NULL, NULL) != LC_OK) return 0;
        u->sink += lc_offset(&u->cur);
    }
    return 1;
}

static int run_insert(void *ud, long iters) {
    lc_bench_ud *u = (lc_bench_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t   off = u->offs[i % u->noffs];
        unsigned e = (unsigned)(i % (long)LC_BENCH_MAX_INS);
        if (lc_seek(&u->cur, u->c, off) != LC_OK) return 0;
        if (lc_insert(&u->cur, e, NULL, NULL) != LC_OK) return 0;
        u->sink += lc_offset(&u->cur);
    }
    return 1;
}

static bench_Case lc_bench_cases[] = {
    {"lc_scan", "lines_10k", 1, 0, setup_scan, run_scan, teardown,
     lc_bench_calls_scan, 0},
    {"lc_scan", "lines_100k", 1, 0, setup_scan_100k, run_scan, teardown,
     lc_bench_calls_scan, 0},
    {"lc_scan", "lines_1m", 1, 0, setup_scan_1m, run_scan, teardown,
     lc_bench_calls_scan, 0},
    {"lc_seek", "lines_10k", 100000, 0, setup_nav, run_seek, teardown,
     NULL, 0},
    {"lc_seek", "lines_100k", 100000, 0, setup_nav_100k, run_seek, teardown,
     NULL, 0},
    {"lc_seek", "lines_1m", 100000, 0, setup_nav_1m, run_seek, teardown,
     NULL, 0},
    {"lc_seekline", "lines_10k", 100000, 0, setup_nav, run_seekline, teardown,
     NULL, 0},
    {"lc_seekline", "lines_100k", 100000, 0, setup_nav_100k, run_seekline,
     teardown, NULL, 0},
    {"lc_seekline", "lines_1m", 100000, 0, setup_nav_1m, run_seekline,
     teardown, NULL, 0},
    {"lc_locate", "lines_10k", 100000, 0, setup_nav, run_locate, teardown,
     NULL, 0},
    {"lc_locate", "lines_100k", 100000, 0, setup_nav_100k, run_locate,
     teardown, NULL, 0},
    {"lc_locate", "lines_1m", 100000, 0, setup_nav_1m, run_locate, teardown,
     NULL, 0},
    {"lc_locline", "lines_10k", 100000, 0, setup_nav, run_locline, teardown,
     NULL, 0},
    {"lc_locline", "lines_100k", 100000, 0, setup_nav_100k, run_locline,
     teardown, NULL, 0},
    {"lc_locline", "lines_1m", 100000, 0, setup_nav_1m, run_locline, teardown,
     NULL, 0},
    {"lc_advance", "lines_10k", 100000, 0, setup_nav, run_advance, teardown,
     NULL, 0},
    {"lc_advance", "lines_100k", 100000, 0, setup_nav_100k, run_advance,
     teardown, NULL, 0},
    {"lc_advance", "lines_1m", 100000, 0, setup_nav_1m, run_advance, teardown,
     NULL, 0},
    {"lc_advline", "lines_10k", 100000, 0, setup_nav, run_advline, teardown,
     NULL, 0},
    {"lc_advline", "lines_100k", 100000, 0, setup_nav_100k, run_advline,
     teardown, NULL, 0},
    {"lc_advline", "lines_1m", 100000, 0, setup_nav_1m, run_advline, teardown,
     NULL, 0},
    {"lc_linelen", "lines_10k", 100000, 0, setup_nav, run_linelen, teardown,
     NULL, 0},
    {"lc_linelen", "lines_100k", 100000, 0, setup_nav_100k, run_linelen,
     teardown, NULL, 0},
    {"lc_linelen", "lines_1m", 100000, 0, setup_nav_1m, run_linelen, teardown,
     NULL, 0},
    {"lc_markbreak", "lines_10k", 20000, 1, setup_nav, run_markbreak,
     teardown, NULL, 0},
    {"lc_markbreak", "lines_100k", 20000, 1, setup_nav_100k, run_markbreak,
     teardown, NULL, 0},
    {"lc_markbreak", "lines_1m", 20000, 1, setup_nav_1m, run_markbreak,
     teardown, NULL, 0},
    {"lc_splice", "lines_10k", 20000, 1, setup_nav, run_splice, teardown,
     NULL, 0},
    {"lc_splice", "lines_100k", 20000, 1, setup_nav_100k, run_splice,
     teardown, NULL, 0},
    {"lc_splice", "lines_1m", 20000, 1, setup_nav_1m, run_splice, teardown,
     NULL, 0},
    {"lc_remove", "lines_10k", 20000, 1, setup_nav, run_remove, teardown,
     NULL, 0},
    {"lc_remove", "lines_100k", 20000, 1, setup_nav_100k, run_remove,
     teardown, NULL, 0},
    {"lc_remove", "lines_1m", 20000, 1, setup_nav_1m, run_remove, teardown,
     NULL, 0},
    {"lc_append", "lines_10k", 20000, 1, setup_nav, run_append, teardown,
     NULL, 0},
    {"lc_append", "lines_100k", 20000, 1, setup_nav_100k, run_append,
     teardown, NULL, 0},
    {"lc_append", "lines_1m", 20000, 1, setup_nav_1m, run_append, teardown,
     NULL, 0},
    {"lc_insert", "lines_10k", 20000, 1, setup_nav, run_insert, teardown,
     NULL, 0},
    {"lc_insert", "lines_100k", 20000, 1, setup_nav_100k, run_insert,
     teardown, NULL, 0},
    {"lc_insert", "lines_1m", 20000, 1, setup_nav_1m, run_insert, teardown,
     NULL, 0},
};

int main(int argc, char **argv) {
    bench_Params p;
    FILE        *out = stdout;
    int          ncases = (int)(sizeof(lc_bench_cases) / sizeof(lc_bench_cases[0]));
    bench_parse(argc, argv, &p);
    if (p.json != NULL) {
        out = fopen(p.json, "w");
        if (out == NULL) return 1;
    }
    if (!bench_run(lc_bench_cases, ncases, &p, out)) {
        if (out != stdout) fclose(out);
        return 1;
    }
    if (out != stdout) fclose(out);
    return 0;
}
