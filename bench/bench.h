#ifndef bench_h
#define bench_h

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BENCH_MAX_CASES 64

typedef struct bench_Params {
    long        seed;
    long        iters;
    long        rounds;
    const char *json;
    const char *cases[BENCH_MAX_CASES];
    int         ncases;
} bench_Params;

typedef struct bench_Case {
    const char *name;
    const char *corpus;
    long        default_iters;
    int         per_round;
    int (*setup)(void **ud, const bench_Params *p);
    int (*run)(void *ud, long iters);
    void (*teardown)(void *ud);
    long (*calls_per_iter)(void *ud);
} bench_Case;

typedef struct bench_Result {
    double ns_per_op;
    double median_ns;
    double min_ns;
    double p10_ns;
    double p90_ns;
    long   calls;
    double amortized_ns;
    long   iters;
    long   rounds;
} bench_Result;

typedef struct bench_Rand {
    unsigned long state;
} bench_Rand;

static void bench_rand_seed(bench_Rand *r, long seed) {
    r->state = (unsigned long)seed + 1;
}

static unsigned long bench_rand(bench_Rand *r) {
    r->state = r->state * 1103515245UL + 12345UL;
    return (r->state / 65536UL) % 32768UL;
}

static long bench_range(bench_Rand *r, long lo, long hi) {
    return lo + (long)(bench_rand(r) % (unsigned long)(hi - lo + 1));
}

static double bench_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void bench_parse(int argc, char **argv, bench_Params *p) {
    int i;
    memset(p, 0, sizeof(*p));
    p->seed = 1;
    p->rounds = 7;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            p->seed = atol(argv[++i]);
        else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
            p->iters = atol(argv[++i]);
        else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
            p->rounds = atol(argv[++i]);
        else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc)
            p->json = argv[++i];
        else if (strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            if (p->ncases < BENCH_MAX_CASES)
                p->cases[p->ncases++] = argv[++i];
            else
                ++i;
        }
    }
}

static int bench_selected(const bench_Params *p, const char *name) {
    int i;
    if (p->ncases == 0) return 1;
    for (i = 0; i < p->ncases; ++i)
        if (strcmp(p->cases[i], name) == 0) return 1;
    return 0;
}

static int bench_cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void bench_stats(double *s, int n, bench_Result *r) {
    double med;
    qsort(s, (size_t)n, sizeof(double), bench_cmp_double);
    med = (n % 2) ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) * 0.5;
    r->min_ns = s[0];
    r->p10_ns = s[n / 10];
    r->median_ns = med;
    r->p90_ns = s[n * 9 / 10];
    r->ns_per_op = med;
}

static void bench_json_str(FILE *out, const char *s) {
    fputc('"', out);
    for (; s && *s; ++s) {
        if (*s == '"' || *s == '\\') fputc('\\', out);
        fputc(*s, out);
    }
    fputc('"', out);
}

static void bench_print_header(FILE *out, const bench_Params *p) {
    fprintf(out, "{\n");
    fprintf(out, "  \"benchmark\": \"pt_fanout_sweep\",\n");
    fprintf(out, "  \"git_commit\": ");
#ifdef BENCH_GIT
    bench_json_str(out, BENCH_GIT);
#else
    fprintf(out, "\"unknown\"");
#endif
    fprintf(out, ",\n");
    fprintf(out, "  \"compiler\": ");
#ifdef __VERSION__
    bench_json_str(out, __VERSION__);
#else
    fprintf(out, "\"unknown\"");
#endif
    fprintf(out, ",\n");
    fprintf(out, "  \"cflags\": ");
#ifdef BENCH_CFLAGS
    bench_json_str(out, BENCH_CFLAGS);
#else
    fprintf(out, "\"unknown\"");
#endif
    fprintf(out, ",\n");
    fprintf(out, "  \"machine\": {\"os\": ");
#ifdef __APPLE__
    fprintf(out, "\"macOS\"");
#elif defined(__linux__)
    fprintf(out, "\"Linux\"");
#else
    fprintf(out, "\"unknown\"");
#endif
    fprintf(out, ", \"cpu\": \"unknown\", \"ram\": \"unknown\"},\n");
#if defined(PT_FANOUT) && defined(PT_MAX_LEVEL)
    fprintf(out, "  \"params\": {\"PT_FANOUT\": %d, \"PT_MAX_LEVEL\": %d, \"seed\": %ld},\n",
            (int)PT_FANOUT, (int)PT_MAX_LEVEL, p->seed);
#elif defined(PT_FANOUT)
    fprintf(out, "  \"params\": {\"PT_FANOUT\": %d, \"seed\": %ld},\n",
            (int)PT_FANOUT, p->seed);
#else
    fprintf(out, "  \"params\": {\"seed\": %ld},\n", p->seed);
#endif
    fprintf(out, "  \"cases\": [\n");
}

static void bench_print_case(FILE *out, const bench_Case *c,
                             const bench_Result *r, const char *corpus,
                             int last) {
    fprintf(out, "    {\n");
    fprintf(out, "      \"name\": ");
    bench_json_str(out, c->name);
    fprintf(out, ",\n");
    fprintf(out, "      \"corpus\": ");
    bench_json_str(out, corpus);
    fprintf(out, ",\n");
    fprintf(out, "      \"iters\": %ld,\n", r->iters);
    fprintf(out, "      \"rounds\": %ld,\n", r->rounds);
    fprintf(out, "      \"ns_per_op\": %.3f,\n", r->ns_per_op);
    fprintf(out, "      \"median_ns\": %.3f,\n", r->median_ns);
    fprintf(out, "      \"min_ns\": %.3f,\n", r->min_ns);
    fprintf(out, "      \"p10_ns\": %.3f,\n", r->p10_ns);
    fprintf(out, "      \"p90_ns\": %.3f,\n", r->p90_ns);
    fprintf(out, "      \"calls\": %ld,\n", r->calls);
    fprintf(out, "      \"amortized_ns\": %.3f\n", r->amortized_ns);
    fprintf(out, "    }%s\n", last ? "" : ",");
}

static int bench_run_one(const bench_Case *c, const bench_Params *p,
                         FILE *out, const char *corpus, int last) {
    bench_Result res;
    void        *ud = NULL;
    double      *s;
    double       t0, t1;
    long         iters = p->iters > 0 ? p->iters : c->default_iters;
    long         i;
    int          ok = 1;

    if (iters <= 0) iters = 1000;
    if (p->rounds <= 0) return 0;
    s = (double *)malloc((size_t)p->rounds * sizeof(double));
    if (s == NULL) return 0;

    if (c->setup && c->setup(&ud, p) == 0) ok = 0;
    if (ok && c->run) {
        long w = iters < 10 ? iters : iters / 10;
        t0 = bench_now();
        c->run(ud, w);
        t1 = bench_now();
        (void)t0;
        (void)t1;
    }
    if (ok && c->per_round && c->teardown) c->teardown(ud);
    if (ok && c->per_round) ud = NULL;

    for (i = 0; ok && i < p->rounds; ++i) {
        if (c->per_round && c->setup && c->setup(&ud, p) == 0) {
            ok = 0;
            break;
        }
        t0 = bench_now();
        if (c->run) c->run(ud, iters);
        t1 = bench_now();
        s[i] = (t1 - t0) * 1e9 / (double)iters;
        if (c->per_round && c->teardown) c->teardown(ud);
        if (c->per_round) ud = NULL;
    }
    if (ok) {
        long   calls = 0;
        double amortized = 0.0;
        memset(&res, 0, sizeof(res));
        bench_stats(s, (int)p->rounds, &res);
        res.iters = iters;
        res.rounds = p->rounds;
        if (!c->per_round && c->calls_per_iter && ud != NULL) {
            calls = c->calls_per_iter(ud);
            if (calls > 0) amortized = res.ns_per_op / (double)calls;
        }
        res.calls = calls;
        res.amortized_ns = amortized;
        bench_print_case(out, c, &res, corpus, last);
    }
    if (ok && !c->per_round && c->teardown) c->teardown(ud);
    free(s);
    return ok;
}

static int bench_run(const bench_Case *cases, int ncases,
                     const bench_Params *p, FILE *out) {
    int i, any = 0;
    bench_print_header(out, p);
    for (i = 0; i < ncases; ++i) {
        int last;
        if (!bench_selected(p, cases[i].name)) continue;
        last = 1;
        {
            int j;
            for (j = i + 1; j < ncases; ++j)
                if (bench_selected(p, cases[j].name)) {
                    last = 0;
                    break;
                }
        }
        if (bench_run_one(&cases[i], p, out,
                          cases[i].corpus ? cases[i].corpus : "fragmented_10k",
                          last))
            any = 1;
    }
    fprintf(out, "  ]\n}\n");
    return any;
}

#endif /* bench_h */
