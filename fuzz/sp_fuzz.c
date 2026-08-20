/* sp_fuzz.c — seeded random-op stress for spantree (fanout 4). The
 * full-tree check runs every FZ_CHECK ops in fuzz mode (O(tree size)
 * per call would be quadratic) and every op in replay mode; the op
 * log lands in /tmp/sp_oplog.txt so a crash replays with
 * "replay <path>".
 *
 *   ./sp_fuzz [seed]        fuzz with the seed (default 1)
 *   ./sp_fuzz replay [path] replay the op log, checking each op
 *
 * the fuzz arbiter stamps every id with ns = id % SP_MASK_BITS + 1 so
 * sp_clear has matches to prune. o->op = rnd % 100 maps through the
 * op table below (weights sum to 100); the cursor expectation is
 * pos+len for append/splice/fill, the walked position for
 * next/prev/style-walk, pos for insert/remove and the seek target
 * otherwise. */
#define SP_FANOUT 4
#define SP_STATIC_API
#include "fz.h"
#include "sp_tests.h"

#ifndef FZ_OPS
# define FZ_OPS 1000000
#endif

#define FZ_OPLOG "/tmp/sp_oplog.txt"

/* op table: X(NAME, weight) rows; weights sum to 100 */
#define FZ_KIND(X)     \
    X(APPEND, 34)      \
    X(INSERT, 12)      \
    X(SPLICE, 12)      \
    X(REMOVE, 10)      \
    X(NEXT, 4)         \
    X(PREV, 4)         \
    X(NEXTNS, 4)       \
    X(PREVNS, 4)       \
    X(STYLE, 4)        \
    X(SEEK, 4)         \
    X(FILL, 4)         \
    X(CLEAR, 4)

FZ_TABLE()

static sp_Id fz_arb(void *ud, sp_Id in, sp_Id old, sp_Mask *m) {
    (void)ud, (void)old;
    *m = 0;
    if (in) sp_addns(m, (int)(in % SP_MASK_BITS) + 1);
    return in;
}

static void runop(sp_Cursor *C, fz_Op *o, FILE *lf) {
    size_t exp = o->off;
    if (lf) fz_write(lf, o);
    ++fz_opno;
    assertok(sp_locate(C, o->off) == SP_OK);
    switch (fz_opidx(o->op)) {
    case FZ_APPEND:
        assertok(sp_append(C, o->len) == SP_OK);
        exp += o->len;
        break;
    case FZ_INSERT:
        assertok(sp_insert(C, o->len) == SP_OK);
        break;
    case FZ_SPLICE:
        assertok(sp_splice(C, o->extra % 20, o->len) == SP_OK);
        exp += o->len;
        break;
    case FZ_REMOVE: {
        sp_Cursor R = *C;
        sp_advance(&R, (sp_Delta)o->extra);
        if (sp_offset(&R) > sp_offset(C)) assertok(sp_remove(C, &R) == SP_OK);
        break;
    }
    case FZ_NEXT: {
        size_t n, k = o->extra % 5 + 1;
        while (k && sp_next(C, 0, &n) != SP_NONE) --k;
        exp = sp_offset(C);
        break;
    }
    case FZ_PREV: {
        size_t n, k = o->extra % 5 + 1;
        while (k && sp_prev(C, 0, &n) != SP_NONE) --k;
        exp = sp_offset(C);
        break;
    }
    case FZ_NEXTNS: {
        size_t n, k = o->extra % 5 + 1, ns = o->extra % 8 + 1;
        while (k && sp_next(C, (int)ns, &n) != SP_NONE) --k;
        exp = sp_offset(C);
        break;
    }
    case FZ_PREVNS: {
        size_t n, k = o->extra % 5 + 1, ns = o->extra % 8 + 1;
        while (k && sp_prev(C, (int)ns, &n) != SP_NONE) --k;
        exp = sp_offset(C);
        break;
    }
    case FZ_STYLE: {
        size_t  n, len;
        sp_Mask m;
        for (;;) {
            sp_style(C, &len, &m);
            if (len == 0) break;
            if (m) sp_hasns(&m, 1), sp_delns(&m, 1);
            sp_next(C, 0, &n);
        }
        sp_advance(C, -(sp_Delta)(o->extra % (sp_bytes(C->tree) + 1)));
        exp = sp_offset(C);
        break;
    }
    case FZ_SEEK:
        assertok(sp_seek(C, C->tree, o->off) == SP_OK);
        exp = o->off;
        break;
    case FZ_FILL:
        assert(C->tree);
        assertok(sp_fill(C, (sp_Id)(o->extra % 8), o->len) == SP_OK);
        exp += o->len;
        break;
    case FZ_CLEAR:
        assertok(sp_clear(C->tree, (int)(o->extra % 8) + 1, 0) == SP_OK);
        assertok(sp_locate(C, o->off) == SP_OK); /* the tree moved */
        break;
    }
    if (!sp_checkcursor(C, exp)) {
        fprintf(stderr, "cursor fail at op %u: %s(off=%lu len=%lu "
                        "extra=%lu)\n",
                fz_opno, fz_opname(o->op), o->off, o->len, o->extra);
        abort();
    }
}

int main(int argc, char **argv) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *T = sp_newtree(S);
    sp_Cursor C;
    unsigned  seed = 0;
    FILE     *lf = NULL;
    int       i, n = FZ_OPS;
    assertok(S && T);
    sp_setarbiter(T, &fz_arb, NULL);
    assertok(sp_seek(&C, T, 0) == SP_OK);
    if (argc > 1 && strcmp(argv[1], "replay") == 0) {
        fz_Op o;
        assertok(freopen(argc > 2 ? argv[2] : FZ_OPLOG, "r", stdin));
        for (n = 0; fz_read(&o); ++n) {
            runop(&C, &o, NULL);
            if ((n & (FZ_CHECK - 1)) == 0 && !sp_checktree(T)) {
                fprintf(stderr, "check fail at op %d: %s(off=%lu len=%lu "
                                "extra=%lu)\n",
                        n, fz_opname(o.op), o.off, o.len, o.extra);
                sp_dumptree(T, "fuzz");
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
            o.off = fz_rnd() % (sp_bytes(T) ? (unsigned)(sp_bytes(T) + 1) : 1);
            o.len = fz_rnd() % 20;
            o.extra = fz_rnd() % 40;
            runop(&C, &o, lf);
            if ((i & (FZ_CHECK - 1)) == 0 && !sp_checktree(T)) {
                fprintf(stderr, "check fail at op %d: %s(off=%lu len=%lu "
                                "extra=%lu)\n",
                        i, fz_opname(o.op), o.off, o.len, o.extra);
                sp_dumptree(T, "fuzz");
                abort();
            }
        }
        printf("fuzz OK (seed %u, ops %d, bytes %lu)\n", seed, n,
               (unsigned long)sp_bytes(T));
    }
    sp_freetree(T), sp_close(S);
    return 0;
}
