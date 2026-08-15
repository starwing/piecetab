/* sp_fuzz.c — seeded random-op stress for spantree (fanout 4). Every
 * op runs the full tree and cursor invariant checks; the op log lands
 * in /tmp/sp_oplog.txt so a crash replays with "replay <path>".
 *
 *   ./sp_fuzz [seed]        fuzz with the seed (default 1)
 *   ./sp_fuzz replay [path] replay the op log, checking each op
 *
 * op <40 append(len) / <60 insert(len) / <80 splice(extra%20, len) /
 * <92 remove(advance extra) / else fill(extra%8, len); the cursor
 * expectation is pos+len for append/splice/fill, pos otherwise. */
#define SP_FANOUT 4
#define SP_IMPLEMENTATION
#include "sp_tests.h"
#include "fz.h"

#ifndef FZ_OPS
# define FZ_OPS 400000
#endif

#define FZ_OPLOG "/tmp/sp_oplog.txt"

static void runop(sp_Tree *T, sp_Cursor *C, fz_Op *o, FILE *lf) {
    size_t exp = o->off;
    if (lf) fz_write(lf, o);
    assertok(sp_locate(C, o->off) == SP_OK);
    if (o->op < 40) {
        assertok(sp_append(C, o->len) == SP_OK);
        exp += o->len;
    } else if (o->op < 60) {
        assertok(sp_insert(C, o->len) == SP_OK);
    } else if (o->op < 80) {
        assertok(sp_splice(C, o->extra % 20, o->len) == SP_OK);
        exp += o->len;
    } else if (o->op < 92) {
        sp_Cursor R = *C;
        sp_advance(&R, (sp_Delta)o->extra);
        if (sp_offset(&R) > sp_offset(C)) assertok(sp_remove(C, &R) == SP_OK);
    } else {
        assertok(sp_fill(C, (sp_Id)(o->extra % 8), o->len) == SP_OK);
        exp += o->len;
    }
    assertok(sp_checktree(T));
    assertok(sp_checkcursor(C, exp));
}

int main(int argc, char **argv) {
    sp_State  *S = sp_open(NULL, NULL);
    sp_Tree   *T = sp_newtree(S);
    sp_Cursor  C;
    unsigned   seed = 0;
    FILE      *lf = NULL;
    int        i, n = FZ_OPS;
    assertok(S && T);
    assertok(sp_seek(&C, T, 0) == SP_OK);
    if (argc > 1 && strcmp(argv[1], "replay") == 0) {
        fz_Op o;
        assertok(freopen(argc > 2 ? argv[2] : FZ_OPLOG, "r", stdin));
        for (n = 0; fz_read(&o); ++n) runop(T, &C, &o, NULL);
        printf("replayed %d ops OK\n", n);
    } else {
        fz_Op o;
        seed = fz_seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 1u;
        assertok((lf = fopen(FZ_OPLOG, "w")) != NULL);
        for (i = 0; i < n; ++i) {
            o.op = fz_rnd() % 100;
            o.off = fz_rnd() % (sp_bytes(T) ? (unsigned)(sp_bytes(T) + 1) : 1);
            o.len = fz_rnd() % 20;
            o.extra = fz_rnd() % 40;
            runop(T, &C, &o, lf);
        }
        printf("fuzz OK (seed %u, ops %d, bytes %lu)\n", seed, n,
               (unsigned long)sp_bytes(T));
    }
    sp_freetree(T), sp_close(S);
    return 0;
}
