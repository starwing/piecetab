/* pt_fuzz.c — seeded random-op stress for piecetab (fanout 4). Every
 * op runs the full tree and cursor invariant checks; the op log lands
 * in /tmp/pt_oplog.txt so a crash replays with "replay <path>".
 *
 *   ./pt_fuzz [seed]        fuzz with the seed (default 1)
 *   ./pt_fuzz replay [path] replay the op log, checking each op
 *
 * op <40 append(len) / <60 insert(len) / <80 splice(extra%20, len) /
 * <92 remove: advance extra then delete len / else edit(extra%20,
 * len%17); the cursor expectation is pos+len for append/splice/edit
 * and the post-advance position for insert/remove. */
#define PT_FANOUT         4
#define PT_PAGE_SIZE      512
#define PT_MAX_HOLESIZE   16
#define PT_COMPACT_RANGES 2
#define PT_STATIC_API
#include "pt_tests.h"
#include "fz.h"

#ifndef FZ_OPS
# define FZ_OPS 400000
#endif

#define FZ_OPLOG "/tmp/pt_oplog.txt"

static char fz_buf[64];

/* structural checker for the fuzz: child-count bounds, byte sums and
 * hole-size caps. The test checker's ADJACENT-literals clause is
 * skipped — zero-copy merging is an edit-point opportunity, not a
 * library invariant (pt_compact exists to coalesce such states). */
static int fz_checknode(const pt_Node *n, int rl, int mc, int *has_hole) {
    int i, hh;
    check(n->child_count >= mc, "[chk] N[%p] rl=%d cc=%d<%d\n", (void *)n, rl,
          n->child_count, mc);
    check(n->child_count <= PT_FANOUT, "[chk] N[%p] rl=%d cc=%d>%d\n",
          (void *)n, rl, n->child_count, PT_FANOUT);
    *has_hole = 0;
    for (i = 0; i < n->child_count; ++i) {
        if (rl == 0) {
            if (ptM_ishole(n, i)) {
                check(n->bytes[i] > 0 && n->bytes[i] <= PT_MAX_HOLESIZE,
                      "[chk] HOLE rl=%d i=%d bytes=%lu > %d\n", rl, i,
                      test_lu(n->bytes[i]), (int)PT_MAX_HOLESIZE);
                *has_hole = 1;
            } else
                check(n->bytes[i] > 0, "[chk] LITERAL rl=%d i=%d bytes=%lu\n",
                      rl, i, test_lu(n->bytes[i]));
        } else {
            pt_Node *c = n->children[i];
            if (!fz_checknode(c, rl - 1, mc ? PT_FANOUT / 2 : 0, &hh)) return 0;
            check(n->bytes[i] == ptN_sumbytes(c, 0, c->child_count),
                  "[chk] INNER rl=%d i=%d bytes=%lu sum=%lu node=%p\n", rl, i,
                  test_lu(n->bytes[i]),
                  test_lu(ptN_sumbytes(c, 0, c->child_count)), (void *)c);
            check((ptM_ishole(n, i) != 0) == (hh != 0),
                  "[chk] MASK rl=%d i=%d mask=%d has_hole=%d\n", rl, i,
                  ptM_ishole(n, i) != 0, hh);
            if (hh) *has_hole = 1;
        }
    }
    return 1;
}

static int fz_checktree(pt_Buffer b) {
    int hh = 0;
    check(b->root.child_count != 0 || b->bytes == 0,
          "[chk] EMPTY root but bytes=%lu\n", test_lu(b->bytes));
    if (b->root.child_count == 0) {
        check(b->bytes == 0, "[chk] EMPTY tree has bytes=%lu\n",
              test_lu(b->bytes));
    } else if (b->levels > 0 || b->root.child_count > 1) {
        if (!fz_checknode(&b->root, b->levels, 1, &hh)) return 0;
    } else
        check(b->root.bytes[0] > 0, "[chk] SINGLE bytes=%lu\n",
              test_lu(b->root.bytes[0]));
    check(b->bytes == ptN_sumbytes(&b->root, 0, b->root.child_count),
          "[chk] ROOT bytes=%lu sum=%lu\n", test_lu(b->bytes),
          test_lu(ptN_sumbytes(&b->root, 0, b->root.child_count)));
    return 1;
}

static void runop(pt_Cursor *C, fz_Op *o, FILE *lf) {
    size_t exp = o->off;
    if (lf) fz_write(lf, o);
    assertok(pt_locate(C, o->off) == PT_OK);
    if (o->op < 40) {
        assertok(pt_append(C, fz_buf, o->len) == PT_OK);
        exp += o->len;
    } else if (o->op < 60) {
        assertok(pt_insert(C, fz_buf, o->len) == PT_OK);
    } else if (o->op < 80) {
        assertok(pt_splice(C, o->extra % 20, fz_buf, o->len) == PT_OK);
        exp += o->len;
    } else if (o->op < 92) {
        assertok(pt_advance(C, (pt_Delta)o->extra) == PT_OK);
        exp = pt_offset(C); /* the advance clamps at the end */
        assertok(pt_remove(C, o->len) == PT_OK);
    } else {
        assertok(pt_edit(C, o->extra % 20, fz_buf, o->len % 17) == PT_OK);
        exp += o->len % 17;
    }
    assertok(fz_checktree(C->tree));
    assertok(pt_checkcursor(C, exp));
}

int main(int argc, char **argv) {
    pt_State *S = pt_open(NULL, NULL);
    pt_Cursor C;
    unsigned  seed = 0;
    FILE     *lf = NULL;
    int       i, n = FZ_OPS;
    assertok(S);
    assertok(pt_seek(&C, pt_empty(S), 0) == PT_OK);
    if (argc > 1 && strcmp(argv[1], "replay") == 0) {
        fz_Op o;
        assertok(freopen(argc > 2 ? argv[2] : FZ_OPLOG, "r", stdin));
        for (n = 0; fz_read(&o); ++n) runop(&C, &o, NULL);
        printf("replayed %d ops OK\n", n);
    } else {
        fz_Op o;
        seed = fz_seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 1u;
        assertok((lf = fopen(FZ_OPLOG, "w")) != NULL);
        for (i = 0; i < n; ++i) {
            o.op = fz_rnd() % 100;
            o.off = fz_rnd()
                    % (pt_bytes(C.tree) ? (unsigned)(pt_bytes(C.tree) + 1) : 1);
            o.len = fz_rnd() % 20;
            o.extra = fz_rnd() % 40;
            runop(&C, &o, lf);
        }
        printf("fuzz OK (seed %u, ops %d, bytes %lu)\n", seed, n,
               (unsigned long)pt_bytes(C.tree));
    }
    pt_close(S);
    return 0;
}
