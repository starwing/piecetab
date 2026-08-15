/* fz.h — shared fuzz scaffolding: seeded RNG and op-log line io.
 *
 * The op log is the crash-reproduction contract: the fuzz writes each
 * op before running it, so a failing run always leaves a replayable
 * log (the failing op is the last line). */
#ifndef FZ_H
#define FZ_H

#include <stdio.h>

typedef struct {
    unsigned     op;
    unsigned long off, len, extra;
} fz_Op;

static unsigned fz_seed;

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

#endif /* FZ_H */
