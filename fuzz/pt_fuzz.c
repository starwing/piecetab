/* pt_fuzz.c — seeded random-op stress for piecetab (fanout 4). Every
 * op runs the full tree and cursor invariant checks; the op log lands
 * in /tmp/pt_oplog.txt so a crash replays with "replay <path>".
 *
 *   ./pt_fuzz [seed]        fuzz with the seed (default 1)
 *   ./pt_fuzz replay [path] replay the op log, checking each op
 *
 * op probabilities (percent): append / insert / splice(extra%20) /
 * remove(advance extra) / next / prev / read / edit(extra%20, len%17);
 * the cursor expectation is pos+len for append/splice/edit and the
 * post-advance position for insert/remove. */
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

/* full-tree check frequency: O(tree size) per call, so fuzz mode checks
 * every FZ_CHECK ops and replay mode checks every op (crash pinpointing) */
#define FZ_CHECK 256

#define FZ_KIND(X)      \
    X(APPEND, 40)       \
    X(INSERT, 20)       \
    X(SPLICE, 20)       \
    X(REMOVE, 10)       \
    X(NEXT, 3)          \
    X(PREV, 3)          \
    X(READ, 2)          \
    X(EDIT, 2)

FZ_TABLE()

static char fz_pool[1 << 24];
static size_t fz_used;
static unsigned fz_dseed; /* independent stream so replay reproduces */

/* FZ_SEAM_PCT% of slices are placed immediately after the previous one,
 * producing physically-adjacent literals that exercise the seam-merge /
 * bridge paths (append merge, splitins bridge, remove exposure).  A gap
 * of 16 bytes keeps the rest non-adjacent (the pre-seam-fix behaviour). */
#ifndef FZ_SEAM_PCT
# define FZ_SEAM_PCT 100
#endif

static char *fz_data(size_t len) {
    size_t off = fz_used;
    fz_dseed = fz_dseed * 1664525u + 1013904223u;
    fz_used += len + ((fz_dseed % 100) < FZ_SEAM_PCT ? 0 : 16);
    return fz_pool + off;
}

static char fz_rdbuf[64]; /* read target, never inserted into the tree */

static void __attribute((unused)) fz_dumpnode(const pt_Node *n, int l) {
    int i;
    for (i = 0; i < n->child_count; ++i) {
        if (l == 0) {
            if (!ptM_ishole(n, i))
                printf("L[%d] lit=%p bytes=%lu\n", i,
                       (const void *)n->children[i],
                       (unsigned long)n->bytes[i]);
        } else {
            printf("N l=%d cc=%u\n", l, n->child_count);
            fz_dumpnode(n->children[i], l - 1);
        }
    }
}

static void runop(pt_Cursor *C, fz_Op *o, FILE *lf) {
    size_t exp = o->off, k, n;
    if (lf) fz_write(lf, o);
    ++fz_opno;
    assertok(pt_locate(C, o->off) == PT_OK);
    switch (fz_opidx(o->op)) {
    case FZ_APPEND:
        assertok(pt_append(C, fz_data(o->len), o->len) == PT_OK);
        exp += o->len;
        break;
    case FZ_INSERT:
        assertok(pt_insert(C, fz_data(o->len), o->len) == PT_OK);
        break;
    case FZ_SPLICE:
        assertok(pt_splice(C, o->extra % 20, fz_data(o->len), o->len)
                 == PT_OK);
        exp += o->len;
        break;
    case FZ_REMOVE:
        assertok(pt_advance(C, (pt_Delta)o->extra) == PT_OK);
        exp = pt_offset(C); /* the advance clamps at the end */
        assertok(pt_remove(C, o->len) == PT_OK);
        break;
    case FZ_NEXT:
        k = o->extra % 5 + 1;
        while (k && pt_next(C, &n) != NULL) --k;
        exp = pt_offset(C);
        break;
    case FZ_PREV:
        k = o->extra % 5 + 1;
        while (k && pt_prev(C, &n) != NULL) --k;
        exp = pt_offset(C);
        break;
    case FZ_READ:
        pt_read(C, fz_rdbuf, o->len % 17);
        exp = pt_offset(C);
        break;
    case FZ_EDIT:
        assertok(pt_edit(C, o->extra % 20, fz_data(o->len % 17), o->len % 17) == PT_OK);
        exp += o->len % 17;
        break;
    }
    if (!pt_checkcursor(C, exp)) {
        int      li, ll;
        pt_Node *pp;
        fprintf(stderr, "cursor fail at op %u: %s(off=%lu len=%lu "
                        "extra=%lu) offset=%lu\n",
                fz_opno, fz_opname(o->op), o->off, o->len, o->extra,
                (unsigned long)pt_offset(C));
        for (ll = 0; ll <= ptK_levels(C); ++ll) {
            pp = ptK_parent(C, ll);
            li = ptK_idx(C, pp, ll);
            fprintf(stderr, "  l%d idx=%d cc=%u cum=%lu bytes=%lu\n", ll, li,
                    pp->child_count, (unsigned long)ptN_sumbytes(pp, 0, li),
                    (unsigned long)pp->bytes[li]);
        }
        abort();
    }
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
        for (n = 0; fz_read(&o); ++n) {
            runop(&C, &o, NULL);
            if ((n & (FZ_CHECK - 1)) == 0 && !pt_checktree(C.tree)) {
                fprintf(stderr, "op %d: %s(off=%lu len=%lu extra=%lu)\n", n,
                        fz_opname(o.op), o.off, o.len, o.extra);
                fz_dumpnode(&C.tree->root, C.tree->levels);
                abort();
            }
        }
        printf("replayed %d ops OK\n", n);
        fz_dumpnode(&C.tree->root, C.tree->levels);
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
            if ((i & (FZ_CHECK - 1)) == 0 && !pt_checktree(C.tree)) {
                fprintf(stderr, "check fail at op %d: %s(off=%lu len=%lu "
                                "extra=%lu)\n",
                        i, fz_opname(o.op), o.off, o.len, o.extra);
                abort();
            }
        }
        printf("fuzz OK (seed %u, ops %d, bytes %lu)\n", seed, n,
               (unsigned long)pt_bytes(C.tree));
    }
    pt_close(S);
    return 0;
}
