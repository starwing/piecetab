#define PT_IMPLEMENTATION
#include "bench_data.h"

#define BENCH_READ_LEN 64UL

static const char bench_text[BENCH_MAX_EDIT + 1] = "abcdefghijklmnop";
static const char bench_lit[] = "literal-insert-payload";

typedef struct bench_pt_ud {
    pt_State    *S;
    bench_Corpus corpus;
    pt_Cursor    cur;
    char        *buf;
    size_t       bufcap;
    long         idx;
    long         calls;
    size_t       sink;
    pt_Buffer    work;
    bench_Rand   rng;
} bench_pt_ud;

static int ud_open_frag_n(bench_pt_ud *u, const bench_Params *p, size_t n) {
    u->S = pt_open(NULL, NULL);
    if (u->S == NULL) return 0;
    if (!bench_corpus_fragmented(&u->corpus, u->S, p->seed, n))
        return 0;
    u->buf = (char *)malloc(BENCH_READ_LEN);
    u->bufcap = BENCH_READ_LEN;
    return u->buf != NULL;
}

static int ud_open_edited_n(bench_pt_ud *u, const bench_Params *p, size_t n) {
    u->S = pt_open(NULL, NULL);
    if (u->S == NULL) return 0;
    if (!bench_corpus_edited(&u->corpus, u->S, p->seed, n))
        return 0;
    u->buf = (char *)malloc(BENCH_READ_LEN);
    u->bufcap = BENCH_READ_LEN;
    return u->buf != NULL;
}

static void ud_close(bench_pt_ud *u) {
    if (u == NULL) return;
    if (u->work) pt_release(u->work);
    bench_corpus_free(&u->corpus);
    if (u->S) pt_close(u->S);
    free(u->buf);
    free(u);
}

static void ud_discard_dirty(bench_pt_ud *u) {
    pt_Buffer b;
    if (!pt_valid(&u->cur)) return;
    b = pt_rollback(&u->cur);
    if (b) pt_release(b);
}

static int setup_nav_n(void **ud, const bench_Params *p, size_t n) {
    bench_pt_ud *u = (bench_pt_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_frag_n(u, p, n)) {
        ud_close(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_nav(void **ud, const bench_Params *p) {
    return setup_nav_n(ud, p, BENCH_FRAG_N);
}

static int setup_nav_100k(void **ud, const bench_Params *p) {
    return setup_nav_n(ud, p, BENCH_FRAG_100K);
}

static int setup_nav_1m(void **ud, const bench_Params *p) {
    return setup_nav_n(ud, p, BENCH_FRAG_1M);
}

static int setup_single(void **ud, const bench_Params *p) {
    bench_pt_ud *u = (bench_pt_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    u->S = pt_open(NULL, NULL);
    if (u->S == NULL) {
        free(u);
        return 0;
    }
    if (!bench_corpus_single(&u->corpus, u->S, p->seed, BENCH_SINGLE)) {
        ud_close(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_edited_n(void **ud, const bench_Params *p, size_t n) {
    bench_pt_ud *u = (bench_pt_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_edited_n(u, p, n)) {
        ud_close(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_edited(void **ud, const bench_Params *p) {
    return setup_edited_n(ud, p, BENCH_EDIT_N);
}

static void teardown_nav(void *ud) {
    ud_close((bench_pt_ud *)ud);
}

static void teardown_dirty(void *ud) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    ud_discard_dirty(u);
    ud_close(u);
}

static int run_from(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        pt_Buffer b = pt_from(u->S, u->corpus.base, u->corpus.total);
        if (b) pt_release(b);
    }
    return 1;
}

static int run_seek(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k)
        pt_seek(&u->cur, u->corpus.b, u->corpus.offs[k % u->corpus.noffs]);
    u->idx = k;
    return 1;
}

static int run_locate(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, k = u->idx;
    if (pt_seek(&u->cur, u->corpus.b, 0) != PT_OK) return 0;
    for (i = 0; i < iters; ++i, ++k)
        pt_locate(&u->cur, u->corpus.offs[k % u->corpus.noffs]);
    u->idx = k;
    return 1;
}

static int run_advance(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, k = u->idx;
    if (pt_seek(&u->cur, u->corpus.b, u->corpus.offs[0]) != PT_OK) return 0;
    for (i = 0; i < iters; ++i, ++k)
        pt_advance(&u->cur, (pt_Delta)u->corpus.advs[k % u->corpus.noffs]);
    u->idx = k;
    return 1;
}

static int run_read(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t n;
        if (pt_seek(&u->cur, u->corpus.b, u->corpus.offs[k % u->corpus.noffs])
            != PT_OK)
            return 0;
        n = pt_read(&u->cur, u->buf, u->bufcap);
        u->sink += n;
    }
    u->idx = k;
    return 1;
}

static int run_next(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, calls = 0;
    for (i = 0; i < iters; ++i) {
        size_t n;
        if (pt_seek(&u->cur, u->corpus.b, 0) != PT_OK) return 0;
        while (pt_next(&u->cur, &n) != NULL) {
            u->sink += n;
            ++calls;
        }
    }
    u->calls = iters > 0 ? calls / iters : 0;
    return 1;
}

static int run_prev(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, calls = 0;
    for (i = 0; i < iters; ++i) {
        size_t n;
        if (pt_seek(&u->cur, u->corpus.b, u->corpus.total) != PT_OK) return 0;
        while (pt_prev(&u->cur, &n) != NULL) {
            u->sink += n;
            ++calls;
        }
    }
    u->calls = iters > 0 ? calls / iters : 0;
    return 1;
}

static long calls_scan(void *ud) {
    return ((bench_pt_ud *)ud)->calls;
}

static int setup_edit_n(void **ud, const bench_Params *p, size_t n) {
    bench_pt_ud *u = (bench_pt_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_frag_n(u, p, n)) {
        ud_close(u);
        return 0;
    }
    if (pt_seek(&u->cur, u->corpus.b, 0) != PT_OK) {
        ud_close(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_edit(void **ud, const bench_Params *p) {
    return setup_edit_n(ud, p, BENCH_FRAG_N);
}

static int setup_edit_100k(void **ud, const bench_Params *p) {
    return setup_edit_n(ud, p, BENCH_FRAG_100K);
}

static int setup_edit_1m(void **ud, const bench_Params *p) {
    return setup_edit_n(ud, p, BENCH_FRAG_1M);
}

static int setup_edit_commit_n(void **ud, const bench_Params *p, size_t n) {
    bench_pt_ud *u = (bench_pt_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_frag_n(u, p, n)) {
        ud_close(u);
        return 0;
    }
    pt_retain(u->corpus.b);
    u->work = u->corpus.b;
    if (u->work == NULL) {
        ud_close(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_edit_commit(void **ud, const bench_Params *p) {
    return setup_edit_commit_n(ud, p, BENCH_FRAG_N);
}

static int setup_edit_commit_100k(void **ud, const bench_Params *p) {
    return setup_edit_commit_n(ud, p, BENCH_FRAG_100K);
}

static int setup_edit_commit_1m(void **ud, const bench_Params *p) {
    return setup_edit_commit_n(ud, p, BENCH_FRAG_1M);
}

static int run_edit(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t off = u->corpus.offs[k % u->corpus.noffs];
        size_t del = (size_t)(k % 5);
        size_t ins = (size_t)(k % (long)BENCH_MAX_EDIT);
        if (pt_locate(&u->cur, off) != PT_OK) return 0;
        if (pt_edit(&u->cur, del, bench_text, ins) != PT_OK) return 0;
    }
    u->idx = k;
    return 1;
}

static int run_edit_commit(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        pt_Buffer b;
        size_t    off = u->corpus.offs[k % u->corpus.noffs];
        size_t    del = (size_t)(k % 5);
        size_t    ins = (size_t)(k % (long)BENCH_MAX_EDIT);
        if (pt_seek(&u->cur, u->work, off) != PT_OK) return 0;
        if (pt_edit(&u->cur, del, bench_text, ins) != PT_OK) return 0;
        b = pt_commit(&u->cur);
        if (b == NULL) return 0;
        pt_release(u->work);
        u->work = b;
    }
    u->idx = k;
    return 1;
}

static int run_insert(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t off = u->corpus.offs[k % u->corpus.noffs];
        size_t len = (size_t)((k % 8) + 1);
        if (pt_locate(&u->cur, off) != PT_OK) return 0;
        if (pt_insert(&u->cur, bench_lit, len) != PT_OK) return 0;
    }
    u->idx = k;
    return 1;
}

static int run_append(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        size_t len = (size_t)((i % 8) + 1);
        if (pt_locate(&u->cur, pt_bytes(pt_buffer(&u->cur))) != PT_OK)
            return 0;
        if (pt_append(&u->cur, bench_lit, len) != PT_OK) return 0;
    }
    return 1;
}

static int run_splice(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t off = u->corpus.offs[k % u->corpus.noffs];
        size_t del = (size_t)(k % 5);
        size_t ins = (size_t)((k % 8) + 1);
        if (pt_locate(&u->cur, off) != PT_OK) return 0;
        if (pt_splice(&u->cur, del, bench_lit, ins) != PT_OK) return 0;
    }
    u->idx = k;
    return 1;
}

static int run_remove(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i, k = u->idx;
    for (i = 0; i < iters; ++i, ++k) {
        size_t off = u->corpus.offs[k % u->corpus.noffs];
        size_t del = (size_t)((k % 4) + 1);
        if (pt_locate(&u->cur, off) != PT_OK) return 0;
        if (pt_remove(&u->cur, del) != PT_OK) return 0;
    }
    u->idx = k;
    return 1;
}

static int setup_dirty_n(void **ud, const bench_Params *p, size_t n) {
    bench_pt_ud *u = (bench_pt_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    if (!ud_open_frag_n(u, p, n)) {
        ud_close(u);
        return 0;
    }
    if (pt_seek(&u->cur, u->corpus.b, 0) != PT_OK
        || pt_edit(&u->cur, 1, bench_text, 2) != PT_OK) {
        ud_close(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int setup_dirty(void **ud, const bench_Params *p) {
    return setup_dirty_n(ud, p, BENCH_FRAG_N);
}

static int setup_dirty_100k(void **ud, const bench_Params *p) {
    return setup_dirty_n(ud, p, BENCH_FRAG_100K);
}

static int setup_dirty_1m(void **ud, const bench_Params *p) {
    return setup_dirty_n(ud, p, BENCH_FRAG_1M);
}

static int run_commit(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        pt_Buffer b = pt_commit(&u->cur);
        if (b == NULL) return 0;
        if (u->work) pt_release(u->work);
        u->work = b;
        if (i + 1 < iters && pt_seek(&u->cur, u->work, 0) != PT_OK)
            return 0;
        if (i + 1 < iters && pt_edit(&u->cur, 1, bench_text, 2) != PT_OK)
            return 0;
    }
    return 1;
}

static int run_rollback(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        pt_Buffer b = pt_rollback(&u->cur);
        if (b == NULL) return 0;
        if (u->work) pt_release(u->work);
        u->work = b;
        if (i + 1 < iters && pt_seek(&u->cur, u->work, 0) != PT_OK)
            return 0;
        if (i + 1 < iters && pt_edit(&u->cur, 1, bench_text, 2) != PT_OK)
            return 0;
    }
    return 1;
}

static int setup_compact(void **ud, const bench_Params *p) {
    return setup_edited(ud, p);
}

static int setup_compact_100k(void **ud, const bench_Params *p) {
    return setup_edited_n(ud, p, BENCH_EDIT_100K);
}

static int setup_compact_1m(void **ud, const bench_Params *p) {
    return setup_edited_n(ud, p, BENCH_EDIT_1M);
}

static int run_compact(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        pt_Buffer b = pt_compact(u->S, u->corpus.b);
        if (b == NULL) return 0;
        pt_release(b);
    }
    return 1;
}

static int setup_replay(void **ud, const bench_Params *p) {
    bench_pt_ud *u = (bench_pt_ud *)calloc(1, sizeof(*u));
    if (u == NULL) return 0;
    u->S = pt_open(NULL, NULL);
    if (u->S == NULL) {
        free(u);
        return 0;
    }
    if (!bench_corpus_single(&u->corpus, u->S, p->seed, BENCH_SINGLE)) {
        ud_close(u);
        return 0;
    }
    pt_retain(u->corpus.b);
    u->work = u->corpus.b;
    bench_rand_seed(&u->rng, p->seed + 999);
    if (pt_seek(&u->cur, u->work, 0) != PT_OK) {
        ud_close(u);
        return 0;
    }
    *ud = u;
    return 1;
}

static int replay_reset_work(bench_pt_ud *u, pt_Buffer b) {
    pt_release(u->work);
    u->work = b;
    return pt_seek(&u->cur, u->work, 0) == PT_OK;
}

static int run_replay(void *ud, long iters) {
    bench_pt_ud *u = (bench_pt_ud *)ud;
    long         i;
    for (i = 0; i < iters; ++i) {
        long op = bench_range(&u->rng, 0, 6);
        if (op == 0) {
            size_t len = (size_t)bench_range(&u->rng, 1, 4);
            if (pt_edit(&u->cur, 0, bench_text, len) != PT_OK) return 0;
        } else if (op == 1) {
            size_t del = (size_t)bench_range(&u->rng, 1, 4);
            if (pt_edit(&u->cur, del, bench_text, 0) != PT_OK) return 0;
        } else if (op == 2) {
            size_t off = (size_t)bench_range(&u->rng, 0,
                (long)pt_bytes(pt_buffer(&u->cur)) - 1);
            if (pt_locate(&u->cur, off) != PT_OK) return 0;
        } else if (op == 3) {
            if (pt_edit(&u->cur, 0, "\n", 1) != PT_OK) return 0;
        } else if (op == 4) {
            pt_Buffer b = pt_commit(&u->cur);
            if (b == NULL) return 0;
            if (!replay_reset_work(u, b)) return 0;
        } else if (op == 5) {
            pt_Buffer b = pt_rollback(&u->cur);
            if (b == NULL) return 0;
            if (!replay_reset_work(u, b)) return 0;
        } else {
            pt_Buffer b;
            if (pt_valid(&u->cur) && u->cur.dirty != 0) {
                b = pt_rollback(&u->cur);
                if (b == NULL) return 0;
                if (!replay_reset_work(u, b)) return 0;
            }
            b = pt_compact(u->S, u->work);
            if (b == NULL) return 0;
            if (!replay_reset_work(u, b)) return 0;
        }
    }
    return 1;
}

static bench_Case bench_cases[] = {
    {"pt_from", "single_piece", 100000, 0, setup_single, run_from, teardown_nav, NULL, 0},
    {"pt_seek", "fragmented_10k", 100000, 0, setup_nav, run_seek, teardown_nav, NULL, 0},
    {"pt_seek", "fragmented_100k", 100000, 0, setup_nav_100k, run_seek, teardown_nav, NULL, 0},
    {"pt_seek", "fragmented_1m", 100000, 0, setup_nav_1m, run_seek, teardown_nav, NULL, 0},
    {"pt_locate", "fragmented_10k", 100000, 0, setup_nav, run_locate, teardown_nav, NULL, 0},
    {"pt_locate", "fragmented_100k", 100000, 0, setup_nav_100k, run_locate, teardown_nav, NULL, 0},
    {"pt_locate", "fragmented_1m", 100000, 0, setup_nav_1m, run_locate, teardown_nav, NULL, 0},
    {"pt_advance", "fragmented_10k", 100000, 0, setup_nav, run_advance, teardown_nav, NULL, 0},
    {"pt_advance", "fragmented_100k", 100000, 0, setup_nav_100k, run_advance, teardown_nav, NULL, 0},
    {"pt_advance", "fragmented_1m", 100000, 0, setup_nav_1m, run_advance, teardown_nav, NULL, 0},
    {"pt_read", "fragmented_10k", 100000, 0, setup_nav, run_read, teardown_nav, NULL, 0},
    {"pt_read", "fragmented_100k", 100000, 0, setup_nav_100k, run_read, teardown_nav, NULL, 0},
    {"pt_read", "fragmented_1m", 100000, 0, setup_nav_1m, run_read, teardown_nav, NULL, 0},
    {"pt_next", "fragmented_10k", 20, 0, setup_nav, run_next, teardown_nav, calls_scan, 0},
    {"pt_next", "fragmented_100k", 20, 0, setup_nav_100k, run_next, teardown_nav, calls_scan, 0},
    {"pt_next", "fragmented_1m", 20, 0, setup_nav_1m, run_next, teardown_nav, calls_scan, 0},
    {"pt_prev", "fragmented_10k", 20, 0, setup_nav, run_prev, teardown_nav, calls_scan, 0},
    {"pt_prev", "fragmented_100k", 20, 0, setup_nav_100k, run_prev, teardown_nav, calls_scan, 0},
    {"pt_prev", "fragmented_1m", 20, 0, setup_nav_1m, run_prev, teardown_nav, calls_scan, 0},
    {"pt_edit", "fragmented_10k", 20000, 1, setup_edit, run_edit, teardown_dirty, NULL, 0},
    {"pt_edit", "fragmented_100k", 20000, 1, setup_edit_100k, run_edit, teardown_dirty, NULL, 0},
    {"pt_edit", "fragmented_1m", 20000, 1, setup_edit_1m, run_edit, teardown_dirty, NULL, 0},
    {"pt_edit_commit", "fragmented_10k", 20000, 1, setup_edit_commit, run_edit_commit, teardown_dirty, NULL, 0},
    {"pt_edit_commit", "fragmented_100k", 20000, 1, setup_edit_commit_100k, run_edit_commit, teardown_dirty, NULL, 0},
    {"pt_edit_commit", "fragmented_1m", 20000, 1, setup_edit_commit_1m, run_edit_commit, teardown_dirty, NULL, 0},
    {"pt_insert", "fragmented_10k", 20000, 1, setup_edit, run_insert, teardown_dirty, NULL, 0},
    {"pt_insert", "fragmented_100k", 20000, 1, setup_edit_100k, run_insert, teardown_dirty, NULL, 0},
    {"pt_insert", "fragmented_1m", 20000, 1, setup_edit_1m, run_insert, teardown_dirty, NULL, 0},
    {"pt_append", "fragmented_10k", 20000, 1, setup_edit, run_append, teardown_dirty, NULL, 0},
    {"pt_splice", "fragmented_10k", 20000, 1, setup_edit, run_splice, teardown_dirty, NULL, 0},
    {"pt_splice", "fragmented_100k", 20000, 1, setup_edit_100k, run_splice, teardown_dirty, NULL, 0},
    {"pt_splice", "fragmented_1m", 20000, 1, setup_edit_1m, run_splice, teardown_dirty, NULL, 0},
    {"pt_remove", "fragmented_10k", 20000, 1, setup_edit, run_remove, teardown_dirty, NULL, 0},
    {"pt_remove", "fragmented_100k", 20000, 1, setup_edit_100k, run_remove, teardown_dirty, NULL, 0},
    {"pt_remove", "fragmented_1m", 20000, 1, setup_edit_1m, run_remove, teardown_dirty, NULL, 0},
    {"pt_commit", "fragmented_10k", 1000, 1, setup_dirty, run_commit, teardown_dirty, NULL, 0},
    {"pt_commit", "fragmented_100k", 1000, 1, setup_dirty_100k, run_commit, teardown_dirty, NULL, 0},
    {"pt_commit", "fragmented_1m", 1000, 1, setup_dirty_1m, run_commit, teardown_dirty, NULL, 0},
    {"pt_rollback", "fragmented_10k", 1000, 1, setup_dirty, run_rollback, teardown_dirty, NULL, 0},
    {"pt_rollback", "fragmented_100k", 1000, 1, setup_dirty_100k, run_rollback, teardown_dirty, NULL, 0},
    {"pt_rollback", "fragmented_1m", 1000, 1, setup_dirty_1m, run_rollback, teardown_dirty, NULL, 0},
    {"pt_compact", "edited_10k", 20, 1, setup_compact, run_compact, teardown_nav, NULL, 0},
    {"pt_compact", "edited_100k", 20, 1, setup_compact_100k, run_compact, teardown_nav, NULL, 0},
    {"pt_compact", "edited_1m", 20, 1, setup_compact_1m, run_compact, teardown_nav, NULL, 0},
    {"pt_replay", "editor_script", 20000, 1, setup_replay, run_replay, teardown_dirty, NULL, 0},
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
    ok = bench_run(bench_cases, (int)(sizeof(bench_cases) / sizeof(bench_cases[0])),
                   &params, out);
    if (out != stdout) fclose(out);
    return ok ? 0 : 1;
}
