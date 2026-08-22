#ifndef bench_data_h
#define bench_data_h

#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "piecetab.h"

#define BENCH_CHUNK     64
#define BENCH_SINGLE    1048576UL
#define BENCH_EDIT_N    10000UL
#define BENCH_EDIT_100K 100000UL
#define BENCH_EDIT_1M   1000000UL
#define BENCH_FRAG_N    10000UL
#define BENCH_FRAG_100K 100000UL
#define BENCH_FRAG_1M   1000000UL
#define BENCH_MAX_EDIT  16UL

typedef struct bench_Corpus {
    pt_State *S;
    pt_Buffer b;
    char    **chunks;
    size_t   *lens;
    size_t    nchunks;
    size_t    total;
    char     *base;
    size_t   *offs;
    size_t   *advs;
    long      noffs;
} bench_Corpus;

static int bench_corpus_offsets(bench_Corpus *c, long seed, long n) {
    bench_Rand r;
    long       i;
    c->offs = (size_t *)malloc((size_t)n * sizeof(size_t));
    c->advs = (size_t *)malloc((size_t)n * sizeof(size_t));
    if (c->offs == NULL || c->advs == NULL) return 0;
    bench_rand_seed(&r, seed);
    for (i = 0; i < n; ++i) {
        c->offs[i] = (size_t)bench_range(&r, 0, (long)c->total - 1);
        c->advs[i] = (size_t)bench_range(&r, -512, 512);
    }
    c->noffs = n;
    return 1;
}

static int bench_corpus_single(bench_Corpus *c, pt_State *S, long seed,
                               size_t len) {
    size_t i;
    memset(c, 0, sizeof(*c));
    c->S = S;
    c->base = (char *)malloc(len ? len : 1);
    if (c->base == NULL) return 0;
    for (i = 0; i < len; ++i) c->base[i] = (char)('a' + (i % 26));
    c->b = pt_from(S, c->base, len);
    if (c->b == NULL) {
        free(c->base);
        c->base = NULL;
        return 0;
    }
    c->total = len;
    c->nchunks = 0;
    return bench_corpus_offsets(c, seed, 4096);
}

static int bench_corpus_fragmented(bench_Corpus *c, pt_State *S, long seed,
                                   size_t n) {
    bench_Rand r;
    pt_Cursor  cur;
    size_t     i;
    memset(c, 0, sizeof(*c));
    c->S = S;
    c->nchunks = n;
    c->chunks = (char **)malloc(n * sizeof(char *));
    c->lens = (size_t *)malloc(n * sizeof(size_t));
    if (c->chunks == NULL || c->lens == NULL) return 0;
    bench_rand_seed(&r, seed);
    for (i = 0; i < n; ++i) {
        size_t len = BENCH_CHUNK + (i % 7);
        c->chunks[i] = (char *)malloc(len);
        if (c->chunks[i] == NULL) return 0;
        memset(c->chunks[i], (int)('a' + (i % 26)), len);
        c->lens[i] = len;
        c->total += len;
    }
    if (pt_seek(&cur, pt_empty(S), 0) != PT_OK) return 0;
    for (i = 0; i < n; ++i) {
        if (pt_append(&cur, c->chunks[i], c->lens[i]) != PT_OK) return 0;
    }
    c->b = pt_commit(&cur);
    if (c->b == NULL) return 0;
    return bench_corpus_offsets(c, seed + 1, 4096);
}

static int bench_corpus_edited(bench_Corpus *c, pt_State *S, long seed,
                               size_t n) {
    static const char text[BENCH_MAX_EDIT + 1] = "abcdefghijklmnop";
    bench_Rand r;
    pt_Cursor  cur;
    size_t     i, total = BENCH_SINGLE;
    memset(c, 0, sizeof(*c));
    c->S = S;
    c->base = (char *)malloc(total);
    if (c->base == NULL) return 0;
    for (i = 0; i < total; ++i) c->base[i] = (char)('a' + (i % 26));
    c->b = pt_from(S, c->base, total);
    if (c->b == NULL) {
        free(c->base);
        c->base = NULL;
        return 0;
    }
    c->total = total;
    bench_rand_seed(&r, seed);
    if (pt_seek(&cur, c->b, 0) != PT_OK) return 0;
    for (i = 0; i < n; ++i) {
        size_t off = (size_t)bench_range(&r, 0, (long)total - 1);
        size_t del = (size_t)bench_range(&r, 0, 4);
        size_t ins = (size_t)bench_range(&r, 0, BENCH_MAX_EDIT);
        if (pt_locate(&cur, off) != PT_OK) return 0;
        if (pt_edit(&cur, del, text, ins) != PT_OK) return 0;
        total = total - del + ins;
    }
    c->b = pt_commit(&cur);
    if (c->b == NULL) return 0;
    c->total = total;
    c->nchunks = 0;
    return bench_corpus_offsets(c, seed + 2, 4096);
}

static void bench_corpus_free(bench_Corpus *c) {
    size_t i;
    if (c->b) pt_release(c->b);
    for (i = 0; i < c->nchunks; ++i) free(c->chunks[i]);
    free(c->chunks);
    free(c->lens);
    free(c->base);
    free(c->offs);
    free(c->advs);
    memset(c, 0, sizeof(*c));
}

#endif /* bench_data_h */
