/* at_fuzz.c -- seeded random-op stress for anchortree (fanout 4). The
 * full-tree check runs every FZ_CHECK ops in fuzz mode (O(tree size)
 * per call would be quadratic) and every op in replay mode; the op
 * log lands in /tmp/at_oplog.txt so a crash replays with
 * "replay <path>".
 *
 *   ./at_fuzz [seed]        fuzz with the seed (default 1)
 *   ./at_fuzz replay [path] replay the op log, checking each op
 *
 * the model tracks live anchor ranges and gravity after each splice.
 * o->op = rnd % 100 maps through the op table below (weights sum to
 * 100). Cursor checks use the shared tests/at_tests.h helper. */
#define AT_FANOUT 4
#define AT_STATIC_API
#include "anchortree.h"
#include "at_tests.h"
#include "fz.h"

#ifndef FZ_OPS
# define FZ_OPS 1000000
#endif

#define FZ_OPLOG "/tmp/at_oplog.txt"
#define FZ_MAXAN 128

typedef struct {
    size_t s;
    size_t e;
    int    g;
} fz_An;

static fz_An fz_ans[FZ_MAXAN];
static at_Id fz_live[FZ_MAXAN];
static int   fz_nlive;

/* op table: X(NAME, weight) rows; weights sum to 100 */
#define FZ_KIND(X) \
    X(SEEK, 10)    \
    X(NEW, 20)     \
    X(SPLICE, 30)  \
    X(DEL, 20)     \
    X(NEXT, 10)    \
    X(ADV, 10)

FZ_TABLE()

static size_t fz_bound(void) {
    size_t b = 0;
    int    i;
    for (i = 0; i < fz_nlive; ++i) {
        at_Id id = fz_live[i];
        if (fz_ans[id].s > b) b = fz_ans[id].s;
        if (fz_ans[id].e > b) b = fz_ans[id].e;
    }
    return b;
}

static int fz_islive(at_Id id) {
    int i;
    for (i = 0; i < fz_nlive; ++i)
        if (fz_live[i] == id) return 1;
    return 0;
}

static void fz_add(at_Id id) {
    assert(fz_nlive < FZ_MAXAN);
    fz_live[fz_nlive] = id, fz_nlive += 1;
}

static void fz_drop(at_Id id) {
    int i;
    for (i = 0; i < fz_nlive; ++i)
        if (fz_live[i] == id) {
            fz_live[i] = fz_live[fz_nlive - 1], fz_nlive -= 1;
            return;
        }
    abort();
}

static int fz_no_empty(at_Tree *T) {
    at_TestSeq s;
    int        i;
    at_seq(T, &s);
    for (i = 0; i < s.n; ++i)
        if (atO_empty(s.edge[i])) return 0;
    return 1;
}

static void fz_splice_model(size_t L, size_t del, size_t ins) {
    size_t bound = fz_bound(), R;
    int    i;
    if (del > 0 && L < bound) {
        R = del > bound - L ? bound : L + del;
        for (i = 0; i < fz_nlive; ++i) {
            at_Id  id = fz_live[i];
            size_t p = fz_ans[id].s;
            if (p > L && p <= R)
                p = L;
            else if (p > R)
                p -= del;
            fz_ans[id].s = p;
            p = fz_ans[id].e;
            if (p > L && p <= R)
                p = L;
            else if (p > R)
                p -= del;
            fz_ans[id].e = p;
        }
    }
    if (ins > 0) {
        for (i = 0; i < fz_nlive; ++i) {
            at_Id  id = fz_live[i];
            int    s0 = fz_ans[id].g == AT_FIXED || fz_ans[id].g == AT_SHIFT;
            int    s1 = fz_ans[id].g == AT_GROW || fz_ans[id].g == AT_SHIFT;
            size_t p = fz_ans[id].s;
            if (p == L) {
                if (s0) p += ins;
            } else if (p > L)
                p += ins;
            fz_ans[id].s = p;
            p = fz_ans[id].e;
            if (p == L) {
                if (s1) p += ins;
            } else if (p > L)
                p += ins;
            fz_ans[id].e = p;
        }
    }
}

static int fz_iter_ok(at_Tree *T, size_t pos) {
    at_Cursor C;
    at_Id     ids[FZ_MAXAN];
    size_t    x, y, prev;
    int       n = 0, m = 0, i, have = 0;
    char      seen[FZ_MAXAN];
    assertok(at_seek(&C, T, pos) == AT_OK);
    memset(seen, 0, sizeof(seen));
    for (;;) {
        at_Id id = at_next(&C);
        int   j;
        if (id == AT_NONE) break;
        if (id >= FZ_MAXAN || !fz_islive(id)) return 0;
        assertok(at_range(T, id, &x, &y) == AT_OK);
        if (x != fz_ans[id].s || y != fz_ans[id].e) return 0;
        if (x < pos || (have && x < prev)) return 0;
        prev = x, have = 1, ids[n] = id, n += 1;
        for (j = 0; j < m; ++j)
            if (seen[j] && ids[j] == id) return 0;
        seen[n - 1] = 1;
    }
    for (i = 0; i < fz_nlive; ++i) {
        at_Id id = fz_live[i];
        if (fz_ans[id].s >= pos) {
            int j, ok = 0;
            for (j = 0; j < n; ++j)
                if (seen[j] && ids[j] == id) ok = 1;
            if (!ok) return 0;
            m += 1;
        }
    }
    return n == m;
}

static void runop(at_Cursor *C, fz_Op *o, FILE *lf) {
    at_Tree *T = C->T;
    size_t   exp;
    if (lf) fz_write(lf, o);
    ++fz_opno;
    assertok(at_seek(C, T, o->off) == AT_OK);
    exp = o->off;
    switch (fz_opidx(o->op)) {
    case FZ_SEEK: break;
    case FZ_NEW: {
        at_Id  id;
        size_t off = at_offset(C);
        size_t len = o->len % 40;
        int    g = (int)(o->extra % 4);
        if (fz_nlive < FZ_MAXAN - 1) {
            assertok(at_newanchor(C, len, g, &id) == AT_OK);
            if (id >= FZ_MAXAN) abort();
            fz_ans[id].s = off, fz_ans[id].e = off + len;
            fz_ans[id].g = g, fz_add(id);
        }
        break;
    }
    case FZ_SPLICE: {
        size_t del = o->len % 30;
        size_t ins = o->extra % 20;
        assertok(at_splice(C, del, ins) == AT_OK);
        fz_splice_model(o->off, del, ins);
        break;
    }
    case FZ_DEL: {
        at_Id id;
        if (fz_nlive > 0) {
            id = fz_live[o->extra % (unsigned)fz_nlive];
            assertok(at_del(T, id) == AT_OK);
            fz_drop(id);
        }
        break;
    }
    case FZ_NEXT:
        if (!fz_iter_ok(T, o->off)) {
            fprintf(stderr,
                    "iter fail at op %u: %s(off=%lu len=%lu "
                    "extra=%lu)\n",
                    fz_opno, fz_opname(o->op), o->off, o->len, o->extra);
            abort();
        }
        break;
    case FZ_ADV: {
        long d = (long)(o->extra % 41) - 20;
        assertok(at_advance(C, (ptrdiff_t)d) == AT_OK);
        exp = at_offset(C);
        break;
    }
    }
    assertok(at_seek(C, T, exp) == AT_OK);
    if (!at_checkcursor(C, exp)) {
        fprintf(stderr,
                "cursor fail at op %u: %s(off=%lu len=%lu "
                "extra=%lu)\n",
                fz_opno, fz_opname(o->op), o->off, o->len, o->extra);
        abort();
    }
}

int main(int argc, char **argv) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    unsigned  seed = 0;
    FILE     *lf = NULL;
    int       i, n = FZ_OPS;
    assertok(S && T);
    assertok(at_seek(&C, T, 0) == AT_OK);
    if (argc > 1 && strcmp(argv[1], "replay") == 0) {
        fz_Op o;
        assertok(freopen(argc > 2 ? argv[2] : FZ_OPLOG, "r", stdin));
        for (n = 0; fz_read(&o); ++n) {
            runop(&C, &o, NULL);
            if ((n & (FZ_CHECK - 1)) == 0
                && (!at_checktree(T) || !fz_no_empty(T))) {
                fprintf(stderr,
                        "check fail at op %d: %s(off=%lu len=%lu "
                        "extra=%lu)\n",
                        n, fz_opname(o.op), o.off, o.len, o.extra);
                abort();
            }
        }
        printf("replayed %d ops OK\n", n);
    } else {
        fz_Op o;
        seed = fz_seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 1u;
        assertok((lf = fopen(FZ_OPLOG, "w")) != NULL);
        for (i = 0; i < n; ++i) {
            size_t hi = fz_bound() ? fz_bound() + 1 : 20;
            o.op = fz_rnd() % 100;
            o.off = fz_rnd() % hi;
            o.len = fz_rnd() % 30;
            o.extra = fz_rnd() % 64;
            runop(&C, &o, lf);
            if ((i & (FZ_CHECK - 1)) == 0
                && (!at_checktree(T) || !fz_no_empty(T))) {
                fprintf(stderr,
                        "check fail at op %d: %s(off=%lu len=%lu "
                        "extra=%lu)\n",
                        i, fz_opname(o.op), o.off, o.len, o.extra);
                abort();
            }
        }
        printf("fuzz OK (seed %u, ops %d, bytes %lu)\n", seed, n,
               (unsigned long)T->bytes);
    }
    at_deltree(T), at_close(S);
    return 0;
}
