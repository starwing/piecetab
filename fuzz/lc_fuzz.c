/* lc_fuzz.c -- seeded random-op stress for linecache (fanout 4). Every
 * op runs the full tree and cursor invariant checks; the op log lands
 * in /tmp/lc_oplog.txt so a crash replays with "replay <path>".
 *
 *   ./lc_fuzz [seed]        fuzz with the seed (default 1)
 *   ./lc_fuzz replay [path] replay the op log, checking each op
 *
 * o->op = rnd % 100 maps through the op table below (weights sum to
 * 100); insert appends e=len bytes to the scanned lines (cursor stays
 * at the insert point), append moves past everything it added, splice
 * deletes extra%20 then inserts len, remove advances extra then deletes
 * to the second cursor; markbreak splits the current line at len and
 * seekline/advline move by line. The cursor expectation is pos for
 * insert/remove, pos+len for splice/append/markbreak and the moved
 * position for the line moves. */
#define LC_LEAF_FANOUT 4
#define LC_FANOUT      4
#define LC_PAGE_SIZE   512
#define LC_STATIC_API
#include "fz.h"
#include "lc_tests.h"

#ifndef FZ_OPS
# define FZ_OPS 400000
#endif

#define FZ_OPLOG "/tmp/lc_oplog.txt"

/* op table: X(NAME, weight) rows; weights sum to 100 */
#define FZ_KIND(X)       \
    X(INSERT, 30)        \
    X(APPEND, 20)        \
    X(SPLICE, 25)        \
    X(REMOVE, 15)        \
    X(MARKBREAK, 5)      \
    X(SEEKLINE, 3)       \
    X(ADVLINE, 2)

FZ_TABLE()

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
    ++fz_opno;
    assertok(lc_locate(C, o->off) == LC_OK);
    switch (fz_opidx(o->op)) {
    case FZ_INSERT: {
        unsigned brs[] = {3, 1, 2, 0}, *p = brs;
        assertok(lc_insert(C, (unsigned)o->len, fz_sc, &p) == LC_OK);
        break;
    }
    case FZ_APPEND: {
        unsigned brs[] = {3, 1, 2, 0}, *p = brs;
        assertok(lc_append(C, (unsigned)o->len, fz_sc, &p) == LC_OK);
        exp += 6 + o->len; /* scanned lines + trailing extension */
        break;
    }
    case FZ_SPLICE:
        assertok(lc_splice(C, o->extra % 20, (unsigned)o->len) == LC_OK);
        exp += o->len;
        break;
    case FZ_REMOVE: {
        lc_Cursor R = *C;
        assertok(lc_advance(&R, (lc_Delta)o->extra) == LC_OK);
        if (lc_offset(&R) > lc_offset(C)) assertok(lc_remove(C, &R) == LC_OK);
        break;
    }
    case FZ_MARKBREAK: {
        unsigned len = (unsigned)(o->len % 20) + 1;
        assertok(lc_markbreak(C, len) == LC_OK);
        exp += len;
        break;
    }
    case FZ_SEEKLINE: {
        size_t n = o->off % (lc_breaks(c) + 1);
        assertok(lc_seekline(C, c, n) == LC_OK);
        exp = lc_offset(C);
        break;
    }
    case FZ_ADVLINE:
        assertok(lc_advline(C, (int)(o->extra % 9) - 4) == LC_OK);
        exp = lc_offset(C);
        break;
    }
    if (!lc_checkcursor(C, exp)) {
        fprintf(stderr, "cursor fail at op %u: %s(off=%lu len=%lu "
                        "extra=%lu)\n",
                fz_opno, fz_opname(o->op), o->off, o->len, o->extra);
        abort();
    }
}

int main(int argc, char **argv) {
    lc_State *S = lc_open(NULL, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    unsigned  seed = 0;
    FILE     *lf = NULL;
    int       i, n = FZ_OPS;
    assertok(S && c);
    assertok(lc_seek(&C, c, 0) == LC_OK);
    if (argc > 1 && strcmp(argv[1], "replay") == 0) {
        fz_Op o;
        assertok(freopen(argc > 2 ? argv[2] : FZ_OPLOG, "r", stdin));
        for (n = 0; fz_read(&o); ++n) {
            runop(c, &C, &o, NULL);
            if ((n & (FZ_CHECK - 1)) == 0 && !lc_checktree(c)) {
                fprintf(stderr, "check fail at op %d: %s(off=%lu len=%lu "
                                "extra=%lu)\n",
                        n, fz_opname(o.op), o.off, o.len, o.extra);
                lc_dumptree(c, "fuzz");
                abort();
            }
        }
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
            if ((i & (FZ_CHECK - 1)) == 0 && !lc_checktree(c)) {
                fprintf(stderr, "check fail at op %d: %s(off=%lu len=%lu "
                                "extra=%lu)\n",
                        i, fz_opname(o.op), o.off, o.len, o.extra);
                lc_dumptree(c, "fuzz");
                abort();
            }
        }
        printf("fuzz OK (seed %u, ops %d, bytes %lu)\n", seed, n,
               (unsigned long)lc_bytes(c));
    }
    lc_delcache(S, c), lc_close(S);
    return 0;
}
