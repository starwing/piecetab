/* fz.h -- shared fuzz scaffolding: seeded RNG, op-log line io and the
 * op table (X-macro rows + FZ_TABLE() helpers).
 *
 * The op log is the crash-reproduction contract: the fuzz writes each
 * op before running it, so a failing run always leaves a replayable
 * log (the failing op is the last line).
 *
 * The op table: each fuzz defines FZ_KIND(X) with one X(NAME, pct) row
 * per op, then expands FZ_TABLE() once at file scope. pct rows are the
 * op weights and o->op = rnd() % 100 maps through fz_opidx, so a new
 * op is one row and a weight change never renumbers anything (the old
 * spantree enum-boundary style forced both, which is why it was
 * dropped). fz_opname prints the row name in crash reports. */
#ifndef FZ_H
#define FZ_H

#include <stdio.h>

typedef struct {
    unsigned     op;
    unsigned long off, len, extra;
} fz_Op;

static unsigned fz_seed;
static unsigned __attribute((unused)) fz_opno;

static unsigned __attribute((unused)) fz_rnd(void) {
    return fz_seed = fz_seed * 1664525u + 1013904223u;
}

static int fz_read(fz_Op *o) {
    return scanf("%u %lu %lu %lu", &o->op, &o->off, &o->len, &o->extra) == 4;
}

static void __attribute((unused)) fz_write(FILE *lf, const fz_Op *o) {
    fprintf(lf, "%u %lu %lu %lu\n", o->op, o->off, o->len, o->extra);
    fflush(lf);
}

/* full-tree check frequency: the tree check is O(tree size), so each
 * fuzz runs it every FZ_CHECK ops and the cursor check per op; pass
 * -DFZ_CHECK=1 for the strongest sweep */
#ifndef FZ_CHECK
# define FZ_CHECK 256
#endif

/* op table rows are X-macro expansions of FZ_KIND: each fuzz defines
 * FZ_KIND(X) with one X(NAME, pct) row per op, then expands FZ_TABLE()
 * once at file scope. o->op = fz_rnd() % 100 maps through fz_opidx by
 * cumulative weight; fz_opname prints the row name for crash reports. */

#define FZ_ENUM(n, p) FZ_##n,
#define FZ_NAME(n, p) #n,
#define FZ_PCT(n, p) p,

#define FZ_TABLE()                                                    \
    enum { FZ_KIND(FZ_ENUM) FZ_KIND_NUM };                            \
    static const char *const fz_opnames[] = { FZ_KIND(FZ_NAME) };     \
    static const unsigned fz_oppcts[] = { FZ_KIND(FZ_PCT) };          \
    static unsigned fz_opidx(unsigned op) {                           \
        unsigned i, acc = 0, n =                                     \
                (unsigned)(sizeof(fz_oppcts) / sizeof(fz_oppcts[0])); \
        for (i = 0; i < n; ++i)                                       \
            if (op < (acc += fz_oppcts[i])) return i;                 \
        return n - 1;                                                 \
    }                                                                 \
    static const char *fz_opname(unsigned op) {                       \
        return fz_opnames[fz_opidx(op)];                              \
    }

#endif /* FZ_H */
