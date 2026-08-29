#define SP_STATIC_API
#include "bench.h"
#include "spantree.h"

#define SP_BENCH_CHUNK    64
#define SP_BENCH_SINGLE   1048576UL
#define SP_BENCH_EDIT_N   10000UL
#define SP_BENCH_EDIT_100K 100000UL
#define SP_BENCH_EDIT_1M  1000000UL
#define SP_BENCH_FRAG_N   10000UL
#define SP_BENCH_FRAG_100K 100000UL
#define SP_BENCH_FRAG_1M  1000000UL
#define SP_BENCH_MAX_EDIT 16UL
#define SP_BENCH_NS       1
#define SP_BENCH_SPARSE_IDS 64UL

typedef struct bench_sp_ud {
    sp_State  *S;
    sp_Tree   *t;
    size_t     total;
    size_t    *offs;
    size_t    *advs;
    long       noffs;
    sp_Cursor  cur;
    sp_Cursor  rcur;
    long       idx;
    long       calls;
    size_t     spans;
    size_t     sink;
    bench_Rand rng;
} bench_sp_ud;

static void bench_sp_ud_free(bench_sp_ud *u);


static sp_Id bench_sp_arb(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    (void)ud;
    (void)old;
    if (mask != NULL) *mask = (sp_Mask)1;
    return id;
}

static sp_Id bench_sp_arb_sparse(void *ud, sp_Id id, sp_Id old,
                                 sp_Mask *mask) {
    (void)ud;
    (void)old;
    if (mask != NULL) *mask = (id == 1) ? (sp_Mask)1 : (sp_Mask)0;
    return id;
}

static int bench_sp_offsets(bench_sp_ud *u, long seed, long n) {
    bench_Rand r;
    long       i;
    u->offs = (size_t *)malloc((size_t)n * sizeof(size_t));
    u->advs = (size_t *)malloc((size_t)n * sizeof(size_t));
    if (u->offs == NULL || u->advs == NULL) return 0;
    bench_rand_seed(&r, seed);
    for (i = 0; i < n; ++i) {
        u->offs[i] = (size_t)bench_range(&r, 0, (long)u->total - 1);
        u->advs[i] = (size_t)bench_range(&r, -512, 512);
    }
    u->noffs = n;
    return 1;
}

static int bench_sp_corpus_fragmented(bench_sp_ud *u, sp_State *S,
                                      long seed, size_t n) {
    bench_Rand r;
    sp_Cursor  cur;
    size_t     i, total = 0;
    u->S = S;
    u->t = sp_newtree(S);
    if (u->t == NULL) return 0;
    sp_setarbiter(u->t, bench_sp_arb, NULL);
    bench_rand_seed(&r, seed);
    for (i = 0; i < n; ++i) {
        size_t len = SP_BENCH_CHUNK + (i % 7);
        sp_Id   id = (sp_Id)(i % 7 + 1);
        if (sp_seek(&cur, u->t, total) != SP_OK) return 0;
        if (sp_fill(&cur, id, len) != SP_OK) return 0;
        total += len;
    }
    u->total = total;
    u->spans = n;
    return bench_sp_offsets(u, seed, 4096);
}

static int bench_sp_corpus_fragmented_sparse(bench_sp_ud *u, sp_State *S,
                                             long seed, size_t n) {
    bench_Rand r;
    sp_Cursor  cur;
    size_t     i, total = 0;
    u->S = S;
    u->t = sp_newtree(S);
    if (u->t == NULL) return 0;
    sp_setarbiter(u->t, bench_sp_arb_sparse, NULL);
    bench_rand_seed(&r, seed);
    for (i = 0; i < n; ++i) {
        size_t len = SP_BENCH_CHUNK + (i % 7);
        sp_Id   id = (sp_Id)(i % SP_BENCH_SPARSE_IDS + 1);
        if (sp_seek(&cur, u->t, total) != SP_OK) return 0;
        if (sp_fill(&cur, id, len) != SP_OK) return 0;
        total += len;
    }
    u->total = total;
    u->spans = n;
    return bench_sp_offsets(u, seed, 4096);
}

static int bench_sp_corpus_viewport(bench_sp_ud *u, sp_State *S,
                                    long seed, size_t n) {
    bench_Rand r;
    sp_Cursor  cur;
    size_t     i, total = 0, lo, hi;
    u->S = S;
    u->t = sp_newtree(S);
    if (u->t == NULL) return 0;
    sp_setarbiter(u->t, bench_sp_arb_sparse, NULL);
    bench_rand_seed(&r, seed);
    lo = n / 10 * 4;
    hi = n / 10 * 6;
    for (i = 0; i < n; ++i) {
        size_t len = SP_BENCH_CHUNK + (i % 7);
        sp_Id   id = (i >= lo && i < hi) ? 1 : 2;
        if (sp_seek(&cur, u->t, total) != SP_OK) return 0;
        if (sp_fill(&cur, id, len) != SP_OK) return 0;
        total += len;
    }
    u->total = total;
    u->spans = n;
    return bench_sp_offsets(u, seed, 4096);
}

static int bench_sp_corpus_scattered(bench_sp_ud *u, sp_State *S,
                                     long seed, size_t n) {
    bench_Rand r;
    sp_Cursor  cur;
    size_t     i, total = 0;
    u->S = S;
    u->t = sp_newtree(S);
    if (u->t == NULL) return 0;
    sp_setarbiter(u->t, bench_sp_arb_sparse, NULL);
    bench_rand_seed(&r, seed);
    for (i = 0; i < n; ++i) {
        size_t len = SP_BENCH_CHUNK + (i % 7);
        sp_Id   id = (i % 1000 == 0) ? 1 : 2;
        if (sp_seek(&cur, u->t, total) != SP_OK) return 0;
        if (sp_fill(&cur, id, len) != SP_OK) return 0;
        total += len;
    }
    u->total = total;
    u->spans = n;
    return bench_sp_offsets(u, seed, 4096);
}

static int ud_open_viewport_n(bench_sp_ud *u, const bench_Params *p,
                              size_t n) {
    u->S = sp_open(NULL, NULL);
    if (u->S == NULL) return 0;
    if (!bench_sp_corpus_viewport(u, u->S, p->seed, n)) return 0;
    return 1;
}

static int setup_viewport_n(void **ud, const bench_Params *p, size_t n) {
    bench_sp_ud *u = (bench_sp_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_viewport_n(u, p, n)) {
        bench_sp_ud_free(u);
        return 0;
    }
    if (sp_seek(&u->cur, u->t, 0) != SP_OK) {
        bench_sp_ud_free(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_viewport(void **ud, const bench_Params *p) {
    return setup_viewport_n(ud, p, SP_BENCH_FRAG_N);
}

static int setup_viewport_100k(void **ud, const bench_Params *p) {
    return setup_viewport_n(ud, p, SP_BENCH_FRAG_100K);
}

static int setup_viewport_1m(void **ud, const bench_Params *p) {
    return setup_viewport_n(ud, p, SP_BENCH_FRAG_1M);
}

static int ud_open_scattered_n(bench_sp_ud *u, const bench_Params *p,
                               size_t n) {
    u->S = sp_open(NULL, NULL);
    if (u->S == NULL) return 0;
    if (!bench_sp_corpus_scattered(u, u->S, p->seed, n)) return 0;
    return 1;
}

static int setup_scattered_n(void **ud, const bench_Params *p, size_t n) {
    bench_sp_ud *u = (bench_sp_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_scattered_n(u, p, n)) {
        bench_sp_ud_free(u);
        return 0;
    }
    if (sp_seek(&u->cur, u->t, 0) != SP_OK) {
        bench_sp_ud_free(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_scattered(void **ud, const bench_Params *p) {
    return setup_scattered_n(ud, p, SP_BENCH_FRAG_N);
}

static int setup_scattered_100k(void **ud, const bench_Params *p) {
    return setup_scattered_n(ud, p, SP_BENCH_FRAG_100K);
}

static int setup_scattered_1m(void **ud, const bench_Params *p) {
    return setup_scattered_n(ud, p, SP_BENCH_FRAG_1M);
}



static void bench_sp_ud_free(bench_sp_ud *u) {
    if (u == NULL) return;
    if (u->t != NULL) sp_freetree(u->t);
    if (u->S != NULL) sp_close(u->S);
    free(u->offs);
    free(u->advs);
    free(u);
}

static int ud_open_frag_n(bench_sp_ud *u, const bench_Params *p, size_t n) {
    u->S = sp_open(NULL, NULL);
    if (u->S == NULL) return 0;
    if (!bench_sp_corpus_fragmented(u, u->S, p->seed, n)) return 0;
    return 1;
}

static int setup_nav_n(void **ud, const bench_Params *p, size_t n) {
    bench_sp_ud *u = (bench_sp_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_frag_n(u, p, n)) {
        bench_sp_ud_free(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_nav(void **ud, const bench_Params *p) {
    return setup_nav_n(ud, p, SP_BENCH_FRAG_N);
}

static int setup_nav_100k(void **ud, const bench_Params *p) {
    return setup_nav_n(ud, p, SP_BENCH_FRAG_100K);
}

static int setup_nav_1m(void **ud, const bench_Params *p) {
    return setup_nav_n(ud, p, SP_BENCH_FRAG_1M);
}

static int setup_edit_n(void **ud, const bench_Params *p, size_t n) {
    bench_sp_ud *u = (bench_sp_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_frag_n(u, p, n)) {
        bench_sp_ud_free(u);
        return 0;
    }
    if (sp_seek(&u->cur, u->t, 0) != SP_OK) {
        bench_sp_ud_free(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_edit(void **ud, const bench_Params *p) {
    return setup_edit_n(ud, p, SP_BENCH_FRAG_N);
}

static int setup_edit_100k(void **ud, const bench_Params *p) {
    return setup_edit_n(ud, p, SP_BENCH_FRAG_100K);
}

static int setup_edit_1m(void **ud, const bench_Params *p) {
    return setup_edit_n(ud, p, SP_BENCH_FRAG_1M);
}

static int ud_open_frag_sparse_n(bench_sp_ud *u, const bench_Params *p,
                                 size_t n) {
    u->S = sp_open(NULL, NULL);
    if (u->S == NULL) return 0;
    if (!bench_sp_corpus_fragmented_sparse(u, u->S, p->seed, n)) return 0;
    return 1;
}

static int setup_edit_sparse_n(void **ud, const bench_Params *p, size_t n) {
    bench_sp_ud *u = (bench_sp_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_frag_sparse_n(u, p, n)) {
        bench_sp_ud_free(u);
        return 0;
    }
    if (sp_seek(&u->cur, u->t, 0) != SP_OK) {
        bench_sp_ud_free(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_edit_sparse(void **ud, const bench_Params *p) {
    return setup_edit_sparse_n(ud, p, SP_BENCH_FRAG_N);
}

static int setup_edit_sparse_100k(void **ud, const bench_Params *p) {
    return setup_edit_sparse_n(ud, p, SP_BENCH_FRAG_100K);
}

static int setup_edit_sparse_1m(void **ud, const bench_Params *p) {
    return setup_edit_sparse_n(ud, p, SP_BENCH_FRAG_1M);
}


static void teardown_nav(void *ud) {
    bench_sp_ud_free((bench_sp_ud *)ud);
}

static int run_seek(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k)
        sp_seek(&u->cur, u->t, u->offs[k % u->noffs]);
    u->idx = k;
    return 1;
}

static int run_locate(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, k = u->idx;
    if (sp_seek(&u->cur, u->t, 0) != SP_OK) return 0;
    for (i = 0; i < iters; ++i, ++k)
        sp_locate(&u->cur, u->offs[k % u->noffs]);
    u->idx = k;
    return 1;
}

static int run_advance(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, k = u->idx;
    if (sp_seek(&u->cur, u->t, u->offs[0]) != SP_OK) return 0;
    for (i = 0; i < iters; ++i, ++k)
        sp_advance(&u->cur, (sp_Delta)u->advs[k % u->noffs]);
    u->idx = k;
    return 1;
}

static int run_style(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t len;
        sp_Id  id;
        if (sp_seek(&u->cur, u->t, u->offs[k % u->noffs]) != SP_OK)
            return 0;
        id = sp_style(&u->cur, &len, NULL);
        u->sink += (size_t)id + len;
    }
    u->idx = k;
    return 1;
}

static int run_next(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, calls = 0;
    for (i = 0; i < iters; ++i) {
        size_t len;
        if (sp_seek(&u->cur, u->t, 0) != SP_OK) return 0;
        while (sp_next(&u->cur, 0, &len) != SP_NONE) {
            u->sink += len;
            ++calls;
        }
    }
    u->calls = iters > 0 ? calls / iters : 0;
    return 1;
}

static int run_prev(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, calls = 0;
    for (i = 0; i < iters; ++i) {
        size_t len;
        if (sp_seek(&u->cur, u->t, u->total) != SP_OK) return 0;
        while (sp_prev(&u->cur, 0, &len) != SP_NONE) {
            u->sink += len;
            ++calls;
        }
    }
    u->calls = iters > 0 ? calls / iters : 0;
    return 1;
}

static int run_next_sparse(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, calls = 0;
    for (i = 0; i < iters; ++i) {
        size_t len;
        if (sp_seek(&u->cur, u->t, 0) != SP_OK) return 0;
        while (sp_next(&u->cur, 1, &len) != SP_NONE) {
            u->sink += len;
            ++calls;
        }
    }
    u->calls = iters > 0 ? calls / iters : 0;
    return 1;
}

static int run_prev_sparse(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, calls = 0;
    for (i = 0; i < iters; ++i) {
        size_t len;
        if (sp_seek(&u->cur, u->t, u->total) != SP_OK) return 0;
        while (sp_prev(&u->cur, 1, &len) != SP_NONE) {
            u->sink += len;
            ++calls;
        }
    }
    u->calls = iters > 0 ? calls / iters : 0;
    return 1;
}

static long calls_scan(void *ud) {
    return ((bench_sp_ud *)ud)->calls;
}

static long calls_spans(void *ud) {
    return (long)((bench_sp_ud *)ud)->spans;
}

static int run_fill(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t off = u->offs[k % u->noffs];
        size_t len = (size_t)((k % 8) + 1);
        sp_Id   id = (sp_Id)((k % 7) + 1);
        if (sp_locate(&u->cur, off) != SP_OK) return 0;
        if (sp_fill(&u->cur, id, len) != SP_OK) return 0;
    }
    u->idx = k;
    return 1;
}

static int run_append(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t len = (size_t)((i % 8) + 1);
        if (sp_seek(&u->cur, u->t, u->total) != SP_OK) return 0;
        if (sp_append(&u->cur, len) != SP_OK) return 0;
        u->total += len;
    }
    return 1;
}

static int run_insert(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t off = u->offs[k % u->noffs];
        size_t len = (size_t)((k % 8) + 1);
        if (sp_locate(&u->cur, off) != SP_OK) return 0;
        if (sp_insert(&u->cur, len) != SP_OK) return 0;
    }
    u->idx = k;
    return 1;
}

static int run_splice(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t off = u->offs[k % u->noffs];
        size_t del = (size_t)(k % 5);
        size_t ins = (size_t)((k % 8) + 1);
        if (sp_locate(&u->cur, off) != SP_OK) return 0;
        if (sp_splice(&u->cur, del, ins) != SP_OK) return 0;
    }
    u->idx = k;
    return 1;
}

static int run_remove(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t off = u->offs[k % u->noffs];
        size_t del = (size_t)((k % 4) + 1);
        size_t end = off + del;
        if (end > u->total) end = u->total;
        if (end <= off) end = off + 1;
        if (sp_locate(&u->cur, off) != SP_OK) return 0;
        if (sp_seek(&u->rcur, u->t, end) != SP_OK) return 0;
        if (sp_remove(&u->cur, &u->rcur) != SP_OK) return 0;
    }
    u->idx = k;
    return 1;
}

static int run_clear(void *ud, long iters) {
    bench_sp_ud *u = (bench_sp_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        sp_Id id = (sp_Id)((i % 7) + 1);
        if (sp_clear(u->t, SP_BENCH_NS, id) != SP_OK) return 0;
    }
    return 1;
}

static bench_Case bench_cases[] = {
    {"sp_seek", "fragmented_10k", 100000, 0, setup_nav, run_seek, teardown_nav, NULL, 0},
    {"sp_seek", "fragmented_100k", 100000, 0, setup_nav_100k, run_seek, teardown_nav, NULL, 0},
    {"sp_seek", "fragmented_1m", 100000, 0, setup_nav_1m, run_seek, teardown_nav, NULL, 0},
    {"sp_locate", "fragmented_10k", 100000, 0, setup_nav, run_locate, teardown_nav, NULL, 0},
    {"sp_locate", "fragmented_100k", 100000, 0, setup_nav_100k, run_locate, teardown_nav, NULL, 0},
    {"sp_locate", "fragmented_1m", 100000, 0, setup_nav_1m, run_locate, teardown_nav, NULL, 0},
    {"sp_advance", "fragmented_10k", 100000, 0, setup_nav, run_advance, teardown_nav, NULL, 0},
    {"sp_advance", "fragmented_100k", 100000, 0, setup_nav_100k, run_advance, teardown_nav, NULL, 0},
    {"sp_advance", "fragmented_1m", 100000, 0, setup_nav_1m, run_advance, teardown_nav, NULL, 0},
    {"sp_style", "fragmented_10k", 100000, 0, setup_nav, run_style, teardown_nav, NULL, 0},
    {"sp_style", "fragmented_100k", 100000, 0, setup_nav_100k, run_style, teardown_nav, NULL, 0},
    {"sp_style", "fragmented_1m", 100000, 0, setup_nav_1m, run_style, teardown_nav, NULL, 0},
    {"sp_next", "fragmented_10k", 20, 0, setup_nav, run_next, teardown_nav, calls_scan, 0},
    {"sp_next", "fragmented_100k", 20, 0, setup_nav_100k, run_next, teardown_nav, calls_scan, 0},
    {"sp_next", "fragmented_1m", 20, 0, setup_nav_1m, run_next, teardown_nav, calls_scan, 0},
    {"sp_prev", "fragmented_10k", 20, 0, setup_nav, run_prev, teardown_nav, calls_scan, 0},
    {"sp_prev", "fragmented_100k", 20, 0, setup_nav_100k, run_prev, teardown_nav, calls_scan, 0},
    {"sp_prev", "fragmented_1m", 20, 0, setup_nav_1m, run_prev, teardown_nav, calls_scan, 0},
    {"sp_next_sparse", "fragmented_sparse_10k", 20, 0,
     setup_edit_sparse, run_next_sparse, teardown_nav, calls_scan, 0},
    {"sp_next_sparse", "fragmented_sparse_100k", 20, 0,
     setup_edit_sparse_100k, run_next_sparse, teardown_nav, calls_scan, 0},
    {"sp_next_sparse", "fragmented_sparse_1m", 20, 0,
     setup_edit_sparse_1m, run_next_sparse, teardown_nav, calls_scan, 0},
    {"sp_prev_sparse", "fragmented_sparse_10k", 20, 0,
     setup_edit_sparse, run_prev_sparse, teardown_nav, calls_scan, 0},
    {"sp_prev_sparse", "fragmented_sparse_100k", 20, 0,
     setup_edit_sparse_100k, run_prev_sparse, teardown_nav, calls_scan, 0},
    {"sp_prev_sparse", "fragmented_sparse_1m", 20, 0,
     setup_edit_sparse_1m, run_prev_sparse, teardown_nav, calls_scan, 0},
    {"sp_next_sparse", "viewport_10k", 20, 0,
     setup_viewport, run_next_sparse, teardown_nav, calls_scan, 0},
    {"sp_next_sparse", "viewport_100k", 20, 0,
     setup_viewport_100k, run_next_sparse, teardown_nav, calls_scan, 0},
    {"sp_next_sparse", "viewport_1m", 20, 0,
     setup_viewport_1m, run_next_sparse, teardown_nav, calls_scan, 0},
    {"sp_prev_sparse", "viewport_10k", 20, 0,
     setup_viewport, run_prev_sparse, teardown_nav, calls_scan, 0},
    {"sp_prev_sparse", "viewport_100k", 20, 0,
     setup_viewport_100k, run_prev_sparse, teardown_nav, calls_scan, 0},
    {"sp_prev_sparse", "viewport_1m", 20, 0,
     setup_viewport_1m, run_prev_sparse, teardown_nav, calls_scan, 0},
    {"sp_clear_sparse", "viewport_10k", 1, 1,
     setup_viewport, run_clear, teardown_nav, calls_spans, 1},
    {"sp_clear_sparse", "viewport_100k", 1, 1,
     setup_viewport_100k, run_clear, teardown_nav, calls_spans, 1},
    {"sp_clear_sparse", "viewport_1m", 1, 1,
     setup_viewport_1m, run_clear, teardown_nav, calls_spans, 1},
    {"sp_next_sparse", "scattered_10k", 20, 0,
     setup_scattered, run_next_sparse, teardown_nav, calls_scan, 0},
    {"sp_next_sparse", "scattered_100k", 20, 0,
     setup_scattered_100k, run_next_sparse, teardown_nav, calls_scan, 0},
    {"sp_next_sparse", "scattered_1m", 20, 0,
     setup_scattered_1m, run_next_sparse, teardown_nav, calls_scan, 0},
    {"sp_prev_sparse", "scattered_10k", 20, 0,
     setup_scattered, run_prev_sparse, teardown_nav, calls_scan, 0},
    {"sp_prev_sparse", "scattered_100k", 20, 0,
     setup_scattered_100k, run_prev_sparse, teardown_nav, calls_scan, 0},
    {"sp_prev_sparse", "scattered_1m", 20, 0,
     setup_scattered_1m, run_prev_sparse, teardown_nav, calls_scan, 0},
    {"sp_clear_sparse", "scattered_10k", 1, 1,
     setup_scattered, run_clear, teardown_nav, calls_spans, 1},
    {"sp_clear_sparse", "scattered_100k", 1, 1,
     setup_scattered_100k, run_clear, teardown_nav, calls_spans, 1},
    {"sp_clear_sparse", "scattered_1m", 1, 1,
     setup_scattered_1m, run_clear, teardown_nav, calls_spans, 1},

    {"sp_fill", "fragmented_10k", 20000, 1, setup_edit, run_fill, teardown_nav, NULL, 0},
    {"sp_fill", "fragmented_100k", 20000, 1, setup_edit_100k, run_fill, teardown_nav, NULL, 0},
    {"sp_fill", "fragmented_1m", 20000, 1, setup_edit_1m, run_fill, teardown_nav, NULL, 0},
    {"sp_append", "fragmented_10k", 20000, 1, setup_edit, run_append, teardown_nav, NULL, 0},
    {"sp_insert", "fragmented_10k", 20000, 1, setup_edit, run_insert, teardown_nav, NULL, 0},
    {"sp_insert", "fragmented_100k", 20000, 1, setup_edit_100k, run_insert, teardown_nav, NULL, 0},
    {"sp_insert", "fragmented_1m", 20000, 1, setup_edit_1m, run_insert, teardown_nav, NULL, 0},
    {"sp_splice", "fragmented_10k", 20000, 1, setup_edit, run_splice, teardown_nav, NULL, 0},
    {"sp_splice", "fragmented_100k", 20000, 1, setup_edit_100k, run_splice, teardown_nav, NULL, 0},
    {"sp_splice", "fragmented_1m", 20000, 1, setup_edit_1m, run_splice, teardown_nav, NULL, 0},
    {"sp_remove", "fragmented_10k", 20000, 1, setup_edit, run_remove, teardown_nav, NULL, 0},
    {"sp_remove", "fragmented_100k", 20000, 1, setup_edit_100k, run_remove, teardown_nav, NULL, 0},
    {"sp_remove", "fragmented_1m", 20000, 1, setup_edit_1m, run_remove, teardown_nav, NULL, 0},
    {"sp_clear", "fragmented_10k", 1, 1, setup_edit, run_clear, teardown_nav, calls_spans, 1},
    {"sp_clear", "fragmented_100k", 1, 1, setup_edit_100k, run_clear, teardown_nav, calls_spans, 1},
    {"sp_clear", "fragmented_1m", 1, 1, setup_edit_1m, run_clear, teardown_nav, calls_spans, 1},
    {"sp_clear_sparse", "fragmented_sparse_10k", 1, 1,
     setup_edit_sparse, run_clear, teardown_nav, calls_spans, 1},
    {"sp_clear_sparse", "fragmented_sparse_100k", 1, 1,
     setup_edit_sparse_100k, run_clear, teardown_nav, calls_spans, 1},
    {"sp_clear_sparse", "fragmented_sparse_1m", 1, 1,
     setup_edit_sparse_1m, run_clear, teardown_nav, calls_spans, 1},
};

int main(int argc, char **argv) {
    bench_Params params;
    FILE        *out = stdout;
    int          ok;
    bench_parse(argc, argv, &params);
    if (params.json != NULL) {
        out = fopen(params.json, "w");
        if (out == NULL) return 1;
    }
    ok = bench_run(bench_cases,
                   (int)(sizeof(bench_cases) / sizeof(bench_cases[0])),
                   &params, out);
    if (out != stdout) fclose(out);
    return ok ? 0 : 1;
}
