/* lc_fuzz.c — seeded random-op stress for linecache (fanout 4). Every
 * op runs the full tree and cursor invariant checks; the op log lands
 * in /tmp/lc_oplog.txt so a crash replays with "replay <path>".
 *
 *   ./lc_fuzz [seed]        fuzz with the seed (default 1)
 *   ./lc_fuzz replay [path] replay the op log, checking each op
 *
 * op <33 insert(e=len, scanned lines) / <66 splice(extra%20, len) /
 * else remove: advance extra then delete to the second cursor; the
 * cursor expectation is pos for insert/remove, pos+len for splice. */
#define LC_LEAF_FANOUT 4
#define LC_FANOUT      4
#define LC_PAGE_SIZE   512
#define LC_IMPLEMENTATION
#include "lc_tests.h"
#include "fz.h"

#ifndef FZ_OPS
# define FZ_OPS 400000
#endif

#define FZ_OPLOG "/tmp/lc_oplog.txt"

/* finite scan pattern: new lines 3,1,2 bytes then the extension e
 * rides on the last line */
static unsigned fz_sc(void *ud, size_t pos) {
    unsigned **p = (unsigned **)ud;
    (void)pos;
    return **p ? *(*p)++ : 0;
}

static void runop(lc_Cache *c, lc_Cursor *C, fz_Op *o, FILE *lf) {
    size_t exp = o->off;
    if (lf) fz_write(lf, o);
    assertok(lc_locate(C, o->off) == LC_OK);
    if (o->op < 33) {
        unsigned brs[] = {3, 1, 2, 0}, *p = brs;
        assertok(lc_insert(C, (unsigned)o->len, fz_sc, &p) == LC_OK);
    } else if (o->op < 66) {
        assertok(lc_splice(C, o->extra % 20, (unsigned)o->len) == LC_OK);
        exp += o->len;
    } else {
        lc_Cursor R = *C;
        assertok(lc_advance(&R, (lc_Delta)o->extra) == LC_OK);
        if (lc_offset(&R) > lc_offset(C)) assertok(lc_remove(C, &R) == LC_OK);
    }
    assertok(lc_checktree(c));
    assertok(lc_checkcursor(C, exp));
}

int main(int argc, char **argv) {
    lc_State  *S = lc_open(NULL, NULL);
    lc_Cache  *c = lc_newcache(S);
    lc_Cursor  C;
    unsigned   seed = 0;
    FILE      *lf = NULL;
    int        i, n = FZ_OPS;
    assertok(S && c);
    assertok(lc_seek(&C, c, 0) == LC_OK);
    if (argc > 1 && strcmp(argv[1], "replay") == 0) {
        fz_Op o;
        assertok(freopen(argc > 2 ? argv[2] : FZ_OPLOG, "r", stdin));
        for (n = 0; fz_read(&o); ++n) runop(c, &C, &o, NULL);
        printf("replayed %d ops OK\n", n);
    } else {
        fz_Op o;
        seed = fz_seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 1u;
        assertok((lf = fopen(FZ_OPLOG, "w")) != NULL);
        for (i = 0; i < n; ++i) {
            o.op = fz_rnd() % 100;
            o.off = fz_rnd() % (lc_bytes(c) ? (unsigned)(lc_bytes(c) + 1) : 1);
            o.len = fz_rnd() % 20;
            o.extra = fz_rnd() % 40;
            runop(c, &C, &o, lf);
        }
        printf("fuzz OK (seed %u, ops %d, bytes %lu)\n", seed, n,
               (unsigned long)lc_bytes(c));
    }
    lc_delcache(S, c), lc_close(S);
    return 0;
}
