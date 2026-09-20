/* at_fuzz.c -- seeded random-op stress for anchortree (default fanout 4,
 * AT_MAX_LEVEL 64; override with -DAT_FANOUT=n -DAT_MAX_LEVEL=m, e.g. 5/64,
 * 7/41, 9/32 per docs/max_levels.md).  In fuzz mode the full-tree check
 * runs every FZ_CHECK ops (any positive value; no power-of-two assumption);
 * O(tree size) per call would be quadratic.  In replay mode it runs every op
 * so the first inconsistent op is reported exactly.  The op log defaults to
 * /tmp/at_oplog.txt (override with -DFZ_OPLOG=...) so a crash replays with
 * "replay <path>"; odd-fanout builds use a per-fanout log.
 *
 *   ./at_fuzz [seed]        fuzz with the seed (default 1)
 *   ./at_fuzz replay [path] replay the op log, checking each op
 *
 * The shared tests/at_tests.h model tracks live anchor ranges and
 * gravity; the same atT_modelsplice drives tests and fuzz.  o->op =
 * rnd % 100 maps through the op table below (weights sum to 100).
 * Cursor and chain checks use the shared atT_/at_ checkers from
 * tests/at_tests.h. */
#ifndef AT_FANOUT
# define AT_FANOUT 4
#endif
#ifndef AT_MAX_LEVEL
# define AT_MAX_LEVEL 64 /* AT_FANOUT=4: see docs/max_levels.md */
#endif
#define AT_STATIC_API
#include "anchortree.h"
#include "at_tests.h"
#include "fz.h"

#ifndef FZ_OPS
# define FZ_OPS 1000000
#endif

#ifndef FZ_OPLOG
# define FZ_OPLOG "/tmp/at_oplog.txt"
#endif
#define FZ_MAXAN 128

static att_Model fz_models[FZ_MAXAN];
static int       fz_nlive;

/* op table: X(NAME, weight) rows; weights sum to 100 */
#define FZ_KIND(X) \
    X(SEEK, 10)    \
    X(NEW, 20)     \
    X(SPLICE, 30)  \
    X(DEL, 20)     \
    X(NEXT, 10)    \
    X(ADV, 10)

FZ_TABLE()

static att_Model *fz_model(at_Id id) {
    int i;
    for (i = 0; i < fz_nlive; ++i)
        if (fz_models[i].id == id) return &fz_models[i];
    abort();
    return NULL;
}

static int fz_islive(at_Id id) {
    int i;
    for (i = 0; i < fz_nlive; ++i)
        if (fz_models[i].id == id) return 1;
    return 0;
}

static void fz_add(at_Id id, size_t s, size_t e, int g) {
    att_Model *m;
    assert(fz_nlive < FZ_MAXAN);
    m = &fz_models[fz_nlive];
    m->id = id, m->start = s, m->end = e, m->gravity = g;
    fz_nlive += 1;
}

static void fz_drop(at_Id id) {
    int i;
    for (i = 0; i < fz_nlive; ++i)
        if (fz_models[i].id == id) break;
    if (i == fz_nlive) abort();
    fz_models[i] = fz_models[fz_nlive - 1];
    fz_nlive -= 1;
}

static size_t fz_bound(void) { return atT_modelbound(fz_models, fz_nlive); }

static int fz_seen(const char *seen, const at_Id *ids, int n, at_Id id) {
    int i;
    for (i = 0; i < n; ++i)
        if (seen[i] && ids[i] == id) return 1;
    return 0;
}

static int fz_iter_ok(at_Tree *T, size_t pos) {
    at_Cursor  C;
    at_Id      id, ids[FZ_MAXAN];
    att_Model *m;
    size_t     x, y, prev, plen;
    int        i, n = 0, nwant = 0, have = 0;
    char       seen[FZ_MAXAN];
    assertok(at_seek(&C, T, pos) == AT_OK);
    memset(seen, 0, sizeof(seen));
    while ((id = at_next(&C, 0, &plen)) != AT_NONE) {
        if (id >= FZ_MAXAN || !fz_islive(id)) return 0;
        m = fz_model(id);
        assertok(at_range(T, id, &x, &y) == AT_OK);
        if (x != m->start || y != m->end) return 0;
        if (plen != (x <= y ? y - x : 0)) return 0;
        if (x < pos || (have && x < prev)) return 0;
        prev = x, have = 1, ids[n] = id, n += 1;
        seen[n - 1] = 1;
    }
    for (i = 0; i < fz_nlive; ++i) {
        id = fz_models[i].id;
        if (fz_models[i].start >= pos) {
            if (!fz_seen(seen, ids, n, id)) return 0;
            nwant += 1;
        }
    }
    return n == nwant;
}

static void fz_stale_ok(const at_Cursor *stale, size_t pos) {
    at_Cursor S = *stale;
    at_Id     id;
    size_t    len = 99;
    assertok(at_next(&S, 0, &len) == AT_NONE);
    assertok(len == 0);
    assertok(at_advance(&S, 0) == AT_ESTALE);
    assertok(at_newanchor(&S, 0, 0, AT_GROW, &id) == AT_ESTALE);
    assertok(at_splice(&S, 0, 0) == AT_ESTALE);
    assertok(at_locate(&S, pos) == AT_OK);
    assertok(at_checkcursor(&S, pos));
}

static void fz_opnew(at_Cursor *C, const fz_Op *o) {
    at_Id  id;
    size_t off = at_offset(C);
    size_t len = o->len % 40u;
    int    g = (int)(o->extra % 4u);
    if (fz_nlive < FZ_MAXAN - 1) {
        assertok(at_newanchor(C, len, 0, g, &id) == AT_OK);
        if (id >= FZ_MAXAN) abort();
        fz_add(id, off, off + len, g);
    }
}

static void fz_opsplice(at_Cursor *C, const fz_Op *o) {
    att_Splice w;
    w.del = o->len % 30u, w.ins = o->extra % 20u, w.L = o->off;
    assertok(at_splice(C, w.del, w.ins) == AT_OK);
    atT_modelsplice(fz_models, fz_nlive, &w);
}

static void fz_opdel(at_Tree *T, const fz_Op *o) {
    at_Id id;
    if (fz_nlive > 0) {
        id = fz_models[o->extra % (unsigned)fz_nlive].id;
        assertok(at_delanchor(T, id) == AT_OK);
        fz_drop(id);
    }
}

static size_t fz_opadv(at_Cursor *C, const fz_Op *o) {
    long d = (long)(o->extra % 41u) - 20;
    assertok(at_advance(C, (at_Delta)d) == AT_OK);
    return at_offset(C);
}

static void fz_opnext(at_Tree *T, const fz_Op *o) {
    if (!fz_iter_ok(T, o->off)) {
        fprintf(stderr, "iter fail at op %u: %s(off=%lu len=%lu extra=%lu)\n",
                fz_opno, fz_opname(o->op), o->off, o->len, o->extra);
        abort();
    }
}

static void fz_check(const at_Tree *T, int i, const fz_Op *o) {
    if (!atT_checkchains(T) || !atT_noemptyedges(T)) {
        fprintf(stderr, "check fail at op %d: %s(off=%lu len=%lu extra=%lu)\n",
                i, fz_opname(o->op), o->off, o->len, o->extra);
        abort();
    }
}

static void runop(at_Cursor *C, fz_Op *o, FILE *lf) {
    at_Tree  *T = C->tree;
    at_Cursor stale;
    size_t    exp, epoch;
    if (lf) fz_write(lf, o);
    ++fz_opno;
    assertok(at_seek(C, T, o->off) == AT_OK);
    stale = *C;
    exp = o->off, epoch = T->epoch;
    switch (fz_opidx(o->op)) {
    case FZ_SEEK: break;
    case FZ_NEW: fz_opnew(C, o); break;
    case FZ_SPLICE: fz_opsplice(C, o); break;
    case FZ_DEL: fz_opdel(T, o); break;
    case FZ_NEXT: fz_opnext(T, o); break;
    case FZ_ADV: exp = fz_opadv(C, o); break;
    }
    if (T->epoch != epoch) fz_stale_ok(&stale, o->off);
    assertok(at_seek(C, T, exp) == AT_OK);
    if (!at_checkcursor(C, exp)) {
        fprintf(stderr, "cursor fail at op %u: %s(off=%lu len=%lu extra=%lu)\n",
                fz_opno, fz_opname(o->op), o->off, o->len, o->extra);
        abort();
    }
}

static void fz_replay(at_Cursor *C, at_Tree *T, const char *path) {
    fz_Op o;
    int   n;
    assertok(freopen(path, "r", stdin));
    for (n = 0; fz_read(&o); ++n) {
        runop(C, &o, NULL);
        fz_check(T, n, &o);
    }
    printf("replayed %d ops OK\n", n);
}

static void fz_fuzz(at_Cursor *C, at_Tree *T, unsigned seed, int ops) {
    fz_Op o;
    FILE *lf;
    int   i;
    fz_seed = seed;
    assertok((lf = fopen(FZ_OPLOG, "w")) != NULL);
    for (i = 0; i < ops; ++i) {
        size_t hi = fz_bound() ? fz_bound() + 1u : 20u;
        o.op = fz_rnd() % 100u;
        o.off = fz_rnd() % hi;
        o.len = fz_rnd() % 30u;
        o.extra = fz_rnd() % 64u;
        runop(C, &o, lf);
        if (i % FZ_CHECK == 0) fz_check(T, i, &o);
    }
    printf("fuzz OK (seed %u, ops %d, bytes %lu)\n", seed, ops,
           (unsigned long)at_bytes(T));
}

int main(int argc, char **argv) {
    at_State *S = at_open(NULL, NULL);
    at_Tree  *T = at_newtree(S);
    at_Cursor C;
    unsigned  seed = 1;
    assertok(S != NULL && T != NULL);
    assertok(at_seek(&C, T, 0) == AT_OK);
    if (argc > 1 && strcmp(argv[1], "replay") == 0)
        fz_replay(&C, T, argc > 2 ? argv[2] : FZ_OPLOG);
    else {
        seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 1u;
        fz_fuzz(&C, T, seed, FZ_OPS);
    }
    at_deltree(T), at_close(S);
    return 0;
}
