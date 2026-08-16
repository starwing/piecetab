/* sp_fuzz.c — seeded random-op stress for spantree (fanout 4). The
 * full-tree check runs every FZ_CHECK ops in fuzz mode (O(tree size)
 * per call would be quadratic) and every op in replay mode; the op
 * log lands in /tmp/sp_oplog.txt so a crash replays with
 * "replay <path>".
 *
 *   ./sp_fuzz [seed]        fuzz with the seed (default 1)
 *   ./sp_fuzz replay [path] replay the op log, checking each op
 *
 * op <40 append(len) / <60 insert(len) / <80 splice(extra%20, len) /
 * <92 remove(advance extra) / <96 next / <98 prev / else fill(extra%8,
 * len); the cursor expectation is pos+len for append/splice/fill, the
 * walked position for next/prev, pos otherwise. */
#define SP_FANOUT 4
#define SP_IMPLEMENTATION
#include "fz.h"
#include "sp_tests.h"

#ifndef FZ_OPS
# define FZ_OPS 400000
#endif

#define FZ_OPLOG "/tmp/sp_oplog.txt"

/* full-tree check frequency: O(tree size) per call, so fuzz mode checks
 * every FZ_CHECK ops and replay mode checks every op (crash pinpointing) */
#define FZ_CHECK 256

static size_t fz_checknode(const sp_Node *n, int rl, int mc, sp_Mask *pmsk) {
    size_t  sum = 0;
    int     i;
    sp_Mask m = 0;
    check(n->child_count <= SP_FANOUT, "[chk] N[%p] rl=%d cc=%d>%d\n",
          (void *)n, rl, n->child_count, SP_FANOUT);
    for (i = 0; i < (int)n->child_count; ++i) {
        if (rl == 0) {
            check(n->bytes[i] > 0, "[chk] ZEROSEG rl=%d i=%d len=%lu\n", rl, i,
                  test_lu(n->bytes[i]));
            sum += n->bytes[i], m |= n->mask[i];
        } else {
            sp_Mask csmsk = 0;
            size_t  cs;
            check(n->child_count >= mc, "[chk] N[%p] rl=%d cc=%d<%d\n",
                  (void *)n, rl, n->child_count, mc);
            cs = fz_checknode(
                    n->children[i], rl - 1, mc ? SP_FANOUT / 2 : 0, &csmsk);
            check(n->bytes[i] == cs,
                  "[chk] INNER rl=%d i=%d bytes=%lu sum=%lu node=%p\n", rl, i,
                  test_lu(n->bytes[i]), test_lu(cs), (void *)n->children[i]);
            check(n->mask[i] == csmsk,
                  "[chk] MASK rl=%d i=%d mask=%lu or=%lu\n", rl, i,
                  test_lu(n->mask[i]), test_lu(csmsk));
            sum += cs, m |= csmsk;
        }
    }
    if (pmsk) *pmsk = m;
    return sum;
}

static int fz_checktree(sp_Tree *T) {
    sp_Mask msk = 0;
    size_t  bsum;
    check(spN_cc(&T->root) != 0 || T->bytes == 0,
          "[chk] EMPTY root but bytes=%lu\n", test_lu(T->bytes));
    if (spN_cc(&T->root) == 0) return 1;
    bsum = fz_checknode(&T->root, T->levels, T->levels ? 1 : 0, &msk);
    check(T->bytes == bsum, "[chk] ROOT bytes=%lu sum=%lu\n", test_lu(T->bytes),
          test_lu(bsum));
    return 1;
}

static void runop(sp_Cursor *C, fz_Op *o, FILE *lf) {
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
    } else if (o->op < 96) {
        size_t n, k = o->extra % 5 + 1;
        while (k && sp_next(C, 0, &n) != 0) --k;
        exp = sp_offset(C);
    } else if (o->op < 98) {
        size_t n, k = o->extra % 5 + 1;
        while (k && sp_prev(C, 0, &n) != 0) --k;
        exp = sp_offset(C);
    } else {
        assertok(sp_fill(C, (sp_Id)(o->extra % 8), o->len) == SP_OK);
        exp += o->len;
    }
    assertok(sp_checkcursor(C, exp));
}

int main(int argc, char **argv) {
    sp_State *S = sp_open(NULL, NULL);
    sp_Tree  *T = sp_newtree(S);
    sp_Cursor C;
    unsigned  seed = 0;
    FILE     *lf = NULL;
    int       i, n = FZ_OPS;
    assertok(S && T);
    assertok(sp_seek(&C, T, 0) == SP_OK);
    if (argc > 1 && strcmp(argv[1], "replay") == 0) {
        fz_Op o;
        assertok(freopen(argc > 2 ? argv[2] : FZ_OPLOG, "r", stdin));
        for (n = 0; fz_read(&o); ++n) {
            runop(&C, &o, NULL);
            assertok(fz_checktree(T)); /* every op: pinpoint the bug */
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
            if ((i & (FZ_CHECK - 1)) == 0) assertok(fz_checktree(T));
        }
        printf("fuzz OK (seed %u, ops %d, bytes %lu)\n", seed, n,
               (unsigned long)sp_bytes(T));
    }
    sp_freetree(T), sp_close(S);
    return 0;
}
