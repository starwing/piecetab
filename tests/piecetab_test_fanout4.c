#define PT_FANOUT         4
#define PT_PAGE_SIZE      512
#define PT_MAX_HOLESIZE   16
#define PT_COMPACT_RANGES 2
#define PT_STATIC_API
#ifndef PT_POOL_STATS
# define PT_POOL_STATS
#endif

#include "../piecetab.h"
#include "pt_tests.h"
#include "tests.h"

/* ================================================================ */
/*  tree dump                                                        */
/* ================================================================ */

PT_STATIC void pt_dumpnode(const pt_Node *n, int idx, int l, int levels) {
    unsigned i, cc = (unsigned)n->child_count;
    if (l == 0)
        test_log("Root(%p) cc=%u", (void *)n, cc);
    else
        test_log(
                "%*sN%u_%u(%p) cc=%u", l * 2, "", (unsigned)(l - 1),
                (unsigned)idx, (void *)n, cc);
    for (i = 0; i < cc; ++i) test_log(" b[%u]=%lu", i, test_lu(n->bytes[i]));
    test_log("\n");
    if ((unsigned)l == (unsigned)levels || levels == 0) {
        for (i = 0; i < cc; ++i) {
            if (ptM_ishole(n, i)) {
                const unsigned char *hd = (const unsigned char *)n->children[i];
                unsigned             ki;
                test_log(
                        "%*sL%u HOLE bytes=%lu data=", (l + 1) * 2, "", i,
                        test_lu(n->bytes[i]));
                for (ki = 0; ki < (unsigned)pt_min(n->bytes[i], 16); ++ki)
                    test_log("%02x", hd[ki]);
                test_log(" '");
                for (ki = 0; ki < (unsigned)pt_min(n->bytes[i], 16); ++ki)
                    test_log(
                            "%c",
                            hd[ki] >= 32 && hd[ki] < 127 ? (char)hd[ki] : '.');
                test_log("'\n");
            } else {
                test_log(
                        "%*sL%u LIT bytes=%lu %.*s\n", (l + 1) * 2, "", i,
                        test_lu(n->bytes[i]), (int)n->bytes[i],
                        (const char *)n->children[i]);
            }
        }
    } else {
        for (i = 0; i < cc; ++i) pt_dumpnode(n->children[i], i, l + 1, levels);
    }
}

PT_STATIC void pt_dumptree(pt_Buffer snap, const char *tag) {
    test_log(
            "[TREE]\t %s: levels=%u root.cc=%u bytes=%lu\n", tag, snap->levels,
            snap->root.child_count, test_lu(snap->bytes));
    pt_dumpnode(&snap->root, -1, 0, snap->levels);
}

PT_STATIC void pt_dumpcursor(const pt_Cursor *C, const char *tag) {
    (void)C;
    (void)tag; /* TODO */
}

/* ================================================================ */
/*  tree comparison                                                  */
/* ================================================================ */

PT_STATIC int pt_comparenode(
        const pt_Node *a, const pt_Node *b, unsigned l, unsigned levels) {
    unsigned i;
    if (a->child_count != b->child_count) return 0;
    for (i = 0; i < (unsigned)a->child_count; ++i) {
        if (a->bytes[i] != b->bytes[i]) return 0;
        if ((ptM_ishole(a, i) != 0) != (ptM_ishole(b, i) != 0)) return 0;
        if (l == levels) {
            if (ptM_ishole(a, i)) {
                const pt_Hole *ha = (const pt_Hole *)a->children[i];
                const pt_Hole *hb = (const pt_Hole *)b->children[i];
                if (memcmp(ha->data, hb->data, a->bytes[i]) != 0) return 0;
            } else {
                if (memcmp((const char *)a->children[i],
                           (const char *)b->children[i], a->bytes[i])
                    != 0)
                    return 0;
            }
        } else {
            if (!pt_comparenode(a->children[i], b->children[i], l + 1, levels))
                return 0;
        }
    }
    return 1;
}

PT_STATIC int pt_comparetree(pt_Buffer a, pt_Buffer b) {
    if (a->levels != b->levels) return 0;
    if (a->bytes != b->bytes) return 0;
    if (a->root.child_count != b->root.child_count) return 0;
    if (a->root.child_count == 0) return 1;
    return pt_comparenode(&a->root, &b->root, 0, a->levels);
}

/* ================================================================ */
/*  leaf sequence checker                                            */
/* ================================================================ */
/* Verify leaf break bytes match expected rle pairs {count,value,..,0} */

PT_STATIC int pt_checkleaves_rec(
        const pt_Node *n, int l, int levels, unsigned **brs) {
    (void)n;
    (void)l;
    (void)levels;
    (void)brs;
    /* TODO */
    return 0;
}

PT_STATIC int pt_checkleaves(const pt_Buffer *S, unsigned **brs) {
    (void)S;
    /* TODO */
    return (**brs == 0);
}

#define checkleavesV(c, ...)                                            \
    do {                                                                \
        unsigned  brs__[] = {__VA_ARGS__, 0};                           \
        unsigned *pbrs__ = brs__;                                       \
        if (!pt_checkleaves((c), &pbrs__)) {                            \
            fprintf(stderr, "checkleavesV FAILED at %s:%d\n", __FILE__, \
                    __LINE__);                                          \
            pt_dumptree((c), "checkleavesV failed");                    \
            abort();                                                    \
        }                                                               \
    } while (0)

/* ================================================================ */
/*  tree construction helpers (leafV / botV / innerV / cacheV)       */
/* ================================================================ */

typedef struct {
    void  *data;
    size_t len;
    int    is_hole;
} pt_LeafValue;

#define innerV(...)    innerV_(S, __VA_ARGS__, NULL)
#define leafV(...)     leafV_(S, __VA_ARGS__, litV_(NULL, 0))
#define litV(s)        litV_("" s, sizeof(s) - 1)
#define holeV(s)       holeV_(S, "" s, sizeof(s) - 1)
#define treeV(l, root) treeV_(S, l, root)

#define pt_nonnull(c) (assert(c), c)

#define editV(c, off, l, root)                 \
    do {                                       \
        pt_Buffer _tv_ = treeV_(S, l, root);   \
        pt_seek((c), pt_nonnull(_tv_), (off)); \
        (c)->dirty = 1;                        \
    } while (0)

static pt_LeafValue litV_(const char *s, size_t len) {
    pt_LeafValue v;
    v.data = (void *)s, v.len = len, v.is_hole = 0;
    return v;
}

static pt_LeafValue holeV_(pt_State *S, const char *s, size_t len) {
    pt_LeafValue v;
    pt_Hole     *h = (pt_Hole *)ptP_alloc(S, &S->holes);
    assertok(len <= PT_MAX_HOLESIZE);
    memcpy(h->data, s, len);
    v.data = (void *)h, v.len = len, v.is_hole = 1;
    return v;
}

PT_STATIC pt_Node *leafV_(pt_State *S, ...) {
    va_list      ap;
    unsigned     i, cc = 0;
    pt_Node     *n;
    pt_LeafValue v;
    va_start(ap, S);
    while (va_arg(ap, pt_LeafValue).data != NULL) cc++;
    va_end(ap);
    n = (pt_Node *)ptP_alloc(S, &S->nodes);
    assertok(n && cc <= PT_FANOUT);
    ptN_setcc(n, cc), n->version = 0, n->mask = 0;
    va_start(ap, S);
    for (i = 0; i < cc; i++) {
        v = va_arg(ap, pt_LeafValue);
        n->children[i] = (pt_Node *)v.data;
        ptM_sethole(n, i, v.is_hole);
        n->bytes[i] = v.len;
    }
    va_end(ap);
    return n;
}

PT_STATIC pt_Node *innerV_(pt_State *S, ...) {
    va_list  ap;
    unsigned i, cc = 0;
    pt_Node *n, *c;
    va_start(ap, S);
    while (va_arg(ap, pt_Node *) != NULL) cc++;
    va_end(ap);
    n = (pt_Node *)ptP_alloc(S, &S->nodes);
    assertok(n && cc <= PT_FANOUT);
    ptN_setcc(n, cc), n->version = 0, n->mask = 0;
    va_start(ap, S);
    for (i = 0; i < cc; i++) {
        c = va_arg(ap, pt_Node *);
        n->children[i] = c, n->bytes[i] = ptN_sumbytes(c, 0, c->child_count);
        ptM_sethole(n, i, (int)c->mask);
    }
    va_end(ap);
    return n;
}

PT_STATIC pt_Buffer treeV_(pt_State *S, unsigned levels, pt_Node *root) {
    pt_Tree *t = (pt_Tree *)pt_from(S, NULL, 0);
    unsigned i;
    assertok(t && root->child_count <= PT_FANOUT);
    t->levels = (unsigned short)levels, t->root = *root;
    ptP_free(&S->nodes, root);
    t->bytes = 0;
    for (i = 0; i < t->root.child_count; i++) t->bytes += t->root.bytes[i];
    pt_checktree_allow_empty(t, 1);
    return assert(t), t;
}

/* ================================================================ */
/*  pt_asserttree -- build expected tree and compare                    */
/* ================================================================ */

#define pt_asserttree(c, lvls, root)                                     \
    do {                                                                 \
        pt_Buffer __d = treeV_(S, lvls, root);                           \
        if (!pt_comparetree((c), __d)) {                                 \
            fprintf(stderr, "pt_asserttree FAILED at %s:%d\n", __FILE__, \
                    __LINE__);                                           \
            fprintf(stderr, "Expected:\n");                              \
            pt_dumptree(__d, "expected");                                \
            fprintf(stderr, "Actual:\n");                                \
            pt_dumptree((c), "actual");                                  \
            abort();                                                     \
        }                                                                \
        pt_release(__d);                                                 \
    } while (0)

/* T1: lifecycle */

TEST(lifecycle) {
    pt_State *S;
    pt_Buffer b, b2;

    S = pt_open(NULL, NULL);
    assertok(S != NULL);

    b = pt_empty(S);
    assertok(b != NULL);
    asserteq(pt_version(b), 0);
    asserteq(pt_bytes(b), 0);

    pt_retain(b);
    pt_release(b);

    b2 = pt_empty(S);
    assertok(b2 != NULL);
    pt_release(b);
    pt_release(b2);

    /* pt_getallocf */
    {
        void     *ud;
        pt_Alloc *af = pt_getallocf(S, &ud);
        asserteq(af, S->allocf);
        asserteq(ud, S->ud);
        af = pt_getallocf(S, NULL);
        asserteq(af, S->allocf);
    }

    pt_close(S);
}

/* T2: seek on empty tree */

TEST(seek_empty) {
    pt_State *S = pt_open(NULL, NULL);
    pt_Cursor c;

    pt_seek(&c, pt_empty(S), 0);
    asserteq(pt_offset(&c), 0);

    pt_seek(&c, pt_empty(S), 100);
    asserteq(pt_offset(&c), 0); /* clamped */

    pt_close(S);
}

/* T3: insert and append */

TEST(insert_basic) {
    pt_State *S = pt_open(NULL, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    int       r;

    pt_seek(&c, b, 0);
    r = pt_insert(&c, "hello", 5);
    asserteq(r, PT_OK);
    asserteq(pt_offset(&c), 0);
    asserteq(pt_bytes(c.tree), 5);

    r = pt_insert(&c, " world", 6);
    asserteq(r, PT_OK);
    asserteq(pt_offset(&c), 0); /* stay */
    asserteq(pt_bytes(c.tree), 11);

    pt_append(&c, "!", 1);
    asserteq(pt_offset(&c), 1); /* append advances cursor past inserted text */
    asserteq(pt_bytes(c.tree), 12);

    pt_release(b);
    pt_close(S);
}

/* T3b: insert positions before / after / mid */

TEST(insert_before) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello", 5);               /* ["hello"], cursor stays pos 0 */
    asserteq(pt_insert(&c, "XX", 2), PT_OK); /* poff==0 -> insert before */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 0));
    pt_asserttree(c.tree, 0, leafV(litV("XX"), litV("hello")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(insert_after) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello", 5);
    pt_advance(&c, 5);                       /* to end, poff==bytes[0] */
    asserteq(pt_insert(&c, "YY", 2), PT_OK); /* insert after */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 5));
    pt_asserttree(c.tree, 0, leafV(litV("hello"), litV("YY")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(insert_mid) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "abcdef", 6);
    pt_advance(&c, 3); /* poff==3, middle of piece */
    asserteq(pt_insert(&c, "XYZ", 3), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 3)); /* cursor stays at insert point */
    pt_asserttree(c.tree, 0, leafV(litV("abc"), litV("XYZ"), litV("def")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3c: split root (fill a levels-0 tree, then insert -> levels 1) */

TEST(insert_split_root) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2);
    pt_append(&c, "bb", 2);
    pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2); /* root full: 4 pieces */
    pt_append(&c, "ee", 2); /* triggers splitroot */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 10));
    pt_asserttree(
            c.tree, 1,
            innerV(leafV(litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"), litV("ee"))));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3d: seek to end (locend) then append after last piece */

TEST(insert_locend) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer a;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "foo", 3);
    pt_append(&c, "bar", 3);
    a = pt_commit(&c);               /* ["foo","bar"] committed */
    pt_seek(&c, a, 999);             /* clamp to end -> locend */
    assertok(pt_checkcursor(&c, 6)); /* off=3, poff=3 (last piece) */
    asserteq(pt_append(&c, "baz", 3), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 9));
    pt_asserttree(c.tree, 0, leafV(litV("foo"), litV("bar"), litV("baz")));
    pt_release(c.tree), pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3e: split a full leaf node (levels 1, no root split) */

TEST(insert_split_leaf) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2);
    pt_append(&c, "bb", 2);
    pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2);
    pt_append(&c, "ee", 2); /* splitroot -> levels 1 */
    pt_append(&c, "ff", 2); /* fills nw leaf to 4 */
    pt_append(&c, "gg", 2); /* splits the full leaf node */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 14));
    pt_asserttree(
            c.tree, 1,
            innerV(leafV(litV("aa"), litV("bb")), leafV(litV("cc"), litV("dd")),
                   leafV(litV("ee"), litV("ff"), litV("gg"))));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3f: deep tree (levels>=2) -- exercises internal splitnode, splitroot,
 * multi-level upbytes, and the 2*levels+3 reserve budget (audit B3). */

TEST(insert_deep) {
    static char buf[300];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = pt_empty(S);
    pt_Cursor   c;
    int         k, n = 60;
    size_t      pos = 0;
    for (k = 0; k < (int)sizeof(buf); ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, b, 0);
    for (k = 0; k < n; ++k) {
        asserteq(pt_append(&c, buf + k * 3, 2), PT_OK);
        pos += 2;
        assertok(pt_checktree(c.tree));
        assertok(pt_checkcursor(&c, pos));
        asserteq(pt_bytes(c.tree), pos);
    }
    assertok(c.tree->levels >= 2);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3g: COW -- editing a committed levels-1 tree copies the path, source stays
 */

TEST(insert_cow) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer a;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2), pt_append(&c, "bb", 2), pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2), pt_append(&c, "ee", 2); /* levels 1 */
    a = pt_commit(&c);
    pt_seek(&c, a, 0);                       /* edit committed source */
    asserteq(pt_insert(&c, "ZZ", 2), PT_OK); /* cowpath copies leaf pp */
    assertok(pt_version(c.tree) != pt_version(a));
    assertok(pt_checktree(c.tree) && pt_checktree(a));
    assertok(pt_checkcursor(&c, 0));
    pt_asserttree(
            a, 1,
            innerV(leafV(litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"), litV("ee"))));
    pt_asserttree(
            c.tree, 1,
            innerV(leafV(litV("ZZ"), litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"), litV("ee"))));
    pt_release(c.tree), pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3h: two cursors fork from one buffer into independent versions */

TEST(insert_multiversion) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer a;
    pt_Cursor c1, c2;
    pt_seek(&c1, b, 0);
    pt_append(&c1, "aa", 2), pt_append(&c1, "bb", 2), pt_append(&c1, "cc", 2);
    pt_append(&c1, "dd", 2), pt_append(&c1, "ee", 2);
    a = pt_commit(&c1);
    pt_seek(&c1, a, 0);
    asserteq(pt_insert(&c1, "11", 2), PT_OK); /* version v1 */
    pt_seek(&c2, a, 10);
    asserteq(pt_insert(&c2, "22", 2), PT_OK); /* version v2, isolated */
    assertok(pt_version(a) != pt_version(c1.tree));
    assertok(pt_version(c1.tree) != pt_version(c2.tree));
    assertok(pt_checktree(a) && pt_checktree(c1.tree) && pt_checktree(c2.tree));
    pt_asserttree(
            a, 1,
            innerV(leafV(litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"), litV("ee"))));
    pt_asserttree(
            c1.tree, 1,
            innerV(leafV(litV("11"), litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"), litV("ee"))));
    pt_asserttree(
            c2.tree, 1,
            innerV(leafV(litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"), litV("ee"), litV("22"))));
    pt_release(c1.tree), pt_release(c2.tree);
    pt_release(a);
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3i: OOM at the reserve / fork allocation points (audit sec.5 rollback) */

TEST(insert_oom_reserve) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    cnt = 0; /* next pool page alloc fails */
    asserteq(pt_insert(&c, "x", 1), PT_ERRMEM);
    assertok(!c.dirty);
    asserteq(pt_bytes(c.tree), 0); /* not forked, tree untouched */
    cnt = 1000;                    /* recover: cursor still usable */
    asserteq(pt_insert(&c, "ok", 2), PT_OK);
    asserteq(pt_bytes(c.tree), 2);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(insert_oom_fork) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    cnt = 0; /* nodes reserve fails (pt_empty is sentinel, no freelist entry;
                reserve OOM achieves fork failure) */
    asserteq(pt_insert(&c, "x", 1), PT_ERRMEM);
    assertok(!c.dirty);
    asserteq(pt_bytes(c.tree), 0);
    cnt = 1000;
    asserteq(pt_insert(&c, "ok", 2), PT_OK);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3j: physical-contiguity merge of adjacent literals (same buffer slices) */

TEST(merge_right) {
    static const char buf[] = "abcdef";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = pt_empty(S);
    pt_Cursor         c;
    pt_seek(&c, b, 0);
    pt_insert(&c, buf + 3, 3);                  /* ["def"] */
    asserteq(pt_insert(&c, buf + 0, 3), PT_OK); /* "abc" before, contiguous */
    assertok(pt_checktree(c.tree));
    pt_asserttree(c.tree, 0, leafV(litV("abcdef")));
    assertok(pt_checkcursor(&c, 0));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(merge_left) {
    static const char buf[] = "abcdef";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = pt_empty(S);
    pt_Cursor         c;
    pt_seek(&c, b, 0);
    pt_insert(&c, buf + 0, 3); /* ["abc"] */
    pt_advance(&c, 3);
    asserteq(pt_insert(&c, buf + 3, 3), PT_OK); /* "def" after, contiguous */
    assertok(pt_checktree(c.tree));
    pt_asserttree(c.tree, 0, leafV(litV("abcdef")));
    assertok(pt_checkcursor(&c, 3)); /* cursor rides merged piece */
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* L936-937: pt_append LEFT merge (poff==0, i>0, prev literal contiguous) */
TEST(append_merge_left) {
    static const char buf[] = "ABCD";
    static const char sep[] = "XY";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Node          *lf = (pt_Node *)ptP_alloc(S, &S->nodes);
    pt_Buffer         b;
    pt_Cursor         c;
    memset(lf, 0, sizeof(pt_Node));
    lf->children[0] = (pt_Node *)(buf + 0), lf->bytes[0] = 2;
    lf->children[1] = (pt_Node *)(sep + 0), lf->bytes[1] = 2;
    lf->child_count = 2;
    b = treeV(0, lf);
    pt_seek(&c, b, 2);
    assertok(pt_checkcursor(&c, 2));
    asserteq(pt_append(&c, buf + 2, 2), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 4));
    asserteq(pt_bytes(c.tree), 6);
    pt_asserttree(c.tree, 0, leafV(litV("ABCD"), litV("XY")));
    {
        char   rd[16];
        size_t nr;
        pt_seek(&c, c.tree, 0);
        nr = pt_read(&c, rd, 6);
        asserteq(nr, 6);
        asserteq(memcmp(rd, "ABCDXY", 6), 0);
    }
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* pt_append into the middle of a literal piece where s+len abuts the
 * piece tail: ptI_fillrt merges the inserted data with the right
 * remainder into one contiguous piece instead of splitting */
TEST(append_fillrt_merge) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = treeV(0, leafV(litV("abcdef")));
    pt_Cursor   c;
    const char *lit;
    char        rd[16];
    size_t      nr;
    pt_seek(&c, b, 2);
    assertok(pt_checkcursor(&c, 2));
    lit = (const char *)b->root.children[0]; /* s+len == lit+2 */
    asserteq(pt_append(&c, lit, 2), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 4));
    asserteq(pt_bytes(c.tree), 8);
    pt_asserttree(c.tree, 0, leafV(litV("ab"), litV("abcdef")));
    pt_seek(&c, c.tree, 0);
    nr = pt_read(&c, rd, 8);
    asserteq(nr, 8);
    asserteq(memcmp(rd, "ababcdef", 8), 0);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3k: differential -- incremental advance must match a fresh pt_seek */

TEST(advance_brute) {
    static char buf[300];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = pt_empty(S);
    pt_Buffer   a;
    pt_Cursor   c, ref;
    int         k, n = 40;
    size_t      from, to, total;
    for (k = 0; k < 300; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, b, 0);
    for (k = 0; k < n; ++k) pt_append(&c, buf + k * 3, 2);
    a = pt_commit(&c);
    total = pt_bytes(a);
    assertok(a->levels >= 2);
    for (from = 0; from <= total; from += 3)
        for (to = 0; to <= total; to += 5) {
            pt_seek(&c, a, from);
            pt_advance(&c, (pt_Delta)to - (pt_Delta)from);
            pt_seek(&ref, a, to);
            asserteq(pt_offset(&c), pt_offset(&ref));
            assertok(pt_checkcursor(&c, to));
        }
    pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3l: defensive NULL-parameter paths + clean (non-dirty) commit */

TEST(null_params) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer got;
    pt_Cursor c;
    asserteq(pt_version(NULL), 0);
    asserteq(pt_bytes(NULL), 0);
    pt_retain(NULL), pt_release(NULL), pt_rollback(NULL);
    asserteq(pt_commit(NULL), NULL);
    asserteq(pt_seek(NULL, b, 0), PT_ERRPARAM);
    asserteq(pt_seek(&c, NULL, 0), PT_ERRPARAM);
    asserteq(pt_advance(NULL, 1), PT_ERRPARAM);
    asserteq(pt_insert(NULL, "x", 1), PT_ERRPARAM);
    asserteq(pt_append(NULL, "x", 1), PT_ERRPARAM);
    pt_seek(&c, b, 0);
    asserteq(pt_advance(&c, 5), PT_OK); /* delta on empty tree */
    asserteq(pt_insert(&c, NULL, 1), PT_ERRPARAM);
    asserteq(pt_insert(&c, "x", 0), PT_OK); /* len==0 early return */
    got = pt_commit(&c);                    /* not dirty -> retain+return */
    asserteq(got, b);
    pt_release(got); /* balance the retain */
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3m: split with cursor landing in the LEFT half (front insert) */

TEST(insert_split_front) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2), pt_append(&c, "bb", 2);
    pt_append(&c, "cc", 2), pt_append(&c, "dd", 2); /* full levels 0 */
    pt_advance(&c, -8);                             /* back to pos 0 */
    asserteq(pt_insert(&c, "ZZ", 2), PT_OK);        /* splitroot, left half */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 0));
    pt_asserttree(
            c.tree, 1,
            innerV(leafV(litV("ZZ"), litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"))));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3n: rollback discards the transient and restores the source */

TEST(rollback) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello", 5);
    assertok(c.dirty);
    asserteq(pt_bytes(c.tree), 5);
    asserteq(pt_rollback(&c), b);
    assertok(!c.dirty);
    asserteq(c.tree, NULL);
    asserteq(pt_bytes(b), 0);
    pt_release(b); /* balance the rollback retain */
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3o: append onto a committed deep tree -- cascade splits COW shared nodes
 * (splitnode's cownode copy branch, audit B3 interaction). */

TEST(insert_committed_split) {
    static char buf[500];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   a, b, e = pt_empty(S);
    pt_Cursor   c;
    int         k;
    for (k = 0; k < 500; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, e, 0);
    for (k = 0; k < 62; ++k) pt_append(&c, buf + k * 3, 2);
    a = pt_commit(&c);
    assertok(a->levels >= 2);
    pt_seek(&c, a, pt_bytes(a)); /* end of committed tree */
    for (k = 0; k < 40; ++k) {
        asserteq(pt_append(&c, buf + 200 + k * 2, 2), PT_OK);
        assertok(pt_checktree(c.tree));
    }
    b = c.tree;
    assertok(pt_checktree(a)); /* source unchanged & valid */
    asserteq(pt_bytes(a), 124);
    pt_release(e), pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3p: COW lifetime -- release the SOURCE buffer while a forked transient
 * still shares its nodes. The `from` field must keep the source alive until
 * the transient is released (exposes the pre-from use-after-free bug). */

TEST(release_order) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer a, b, e = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, e, 0);
    pt_append(&c, "aa", 2), pt_append(&c, "bb", 2), pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2), pt_append(&c, "ee", 2); /* levels 1 */
    a = pt_commit(&c);
    pt_seek(&c, a, 0);
    asserteq(pt_insert(&c, "ZZ", 2), PT_OK); /* transient b shares a's nodes */
    b = c.tree;
    pt_release(a);             /* release source FIRST; from keeps it alive */
    assertok(pt_checktree(b)); /* transient still valid (no use-after-free) */
    pt_release(b);             /* frees b, then chained a, then b-ref */
    pt_release(e);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* T3q: rollback when the source is kept alive only by the transient's `from`
 * (external released it after fork). rollback's return value revives the
 * source: no dangling, caller owns the returned reference. */

TEST(rollback_released_source) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer a, back;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2), pt_append(&c, "bb", 2), pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2), pt_append(&c, "ee", 2);
    a = pt_commit(&c);
    pt_seek(&c, a, 0);
    asserteq(pt_insert(&c, "ZZ", 2), PT_OK); /* fork; retain(a) via from */
    pt_release(a);          /* external drops a; only from holds it */
    back = pt_rollback(&c); /* return value keeps a alive */
    asserteq(back, a);
    asserteq(c.tree, NULL);
    assertok(pt_checktree(back));
    asserteq(pt_bytes(back), 10);
    pt_release(back);
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* release order with two committed buffers (from-chain COW) */

TEST(remove_release_order) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b0 = pt_from(S, "hello world foobar", 18);
    pt_Cursor c;
    pt_Buffer b1;
    pt_seek(&c, b0, 5);
    pt_insert(&c, "XYZ", 3);
    b1 = pt_commit(&c);
    assertok(pt_checktree(b1));
    pt_release(b0); /* release source first; b1 still shares nodes */
    assertok(pt_checktree(b1)); /* b1 not dangling */
    pt_release(b1);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= remove: levels==0 literal deletions ================= */

TEST(remove_same_mid) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("abcdef")));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    asserteq(pt_remove(&c, 2), PT_OK); /* delete "cd" -> split into two */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("ab"), litV("ef")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_same_prefix) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("abcdef")));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 2), PT_OK); /* delete prefix "ab" */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 0));
    pt_asserttree(c.tree, 0, leafV(litV("cdef")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_same_suffix) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("abcdef")));
    pt_Cursor c;
    pt_seek(&c, b, 4);
    asserteq(pt_remove(&c, 2), PT_OK); /* delete suffix "ef" */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 4));
    pt_asserttree(c.tree, 0, leafV(litV("abcd")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_piece_whole) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("aa"), litV("bb"), litV("cc")));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    asserteq(pt_remove(&c, 2), PT_OK); /* delete whole middle piece "bb" */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("aa"), litV("cc")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_cross) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("aaa"), litV("bbb"), litV("ccc")));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    asserteq(pt_remove(&c, 5), PT_OK); /* [2,7): a|bbb|c across 3 pieces */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("aa"), litV("cc")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_to_end) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("aa"), litV("bb"), litV("cc")));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    asserteq(pt_remove(&c, 4), PT_OK); /* delete "bb"+"cc" to end */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("aa")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_all) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("aa"), litV("bb")));
    pt_Cursor c;
    size_t    n;
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 4), PT_OK); /* delete everything */
    assertok(pt_checktree(c.tree));
    asserteq(pt_bytes(c.tree), 0);
    assertok(pt_checkcursor(&c, 0));
    /* The tree is structurally empty; the root slot must be cleared so
       pt_piece/pt_next never read stale data. */
    pt_seek(&c, c.tree, 0);
    asserteq(pt_piece(&c, &n), NULL);
    asserteq(n, 0);
    pt_asserttree(c.tree, 0, leafV(litV_(NULL, 0)));
    asserteq(c.tree->root.children[0], NULL);
    asserteq(c.tree->root.bytes[0], 0);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_across_leaves) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(
            1, innerV(leafV(litV("aaa"), litV("bbb")),
                      leafV(litV("ccc"), litV("ddd"))));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    asserteq(pt_remove(&c, 7), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("aa"), litV("ddd")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_deep_shrink) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(
            2, innerV(innerV(leafV(litV("aa")), leafV(litV("bb"))),
                      innerV(leafV(litV("cc")))));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 4), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 0));
    pt_asserttree(c.tree, 0, leafV(litV("cc")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* sec.8.1 edit_cow: from committed buffer, fork preserves source, hole in
 * transient
 */

TEST(edit_cow) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer a, b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "hello world", 11), PT_OK);
    a = pt_commit(&c);
    assertok(a != NULL && !c.dirty);
    /* fork: edit on committed buffer */
    pt_seek(&c, a, 5);
    asserteq(pt_edit(&c, 0, "XYZ", 3), PT_OK);
    assertok(c.dirty);
    assertok(pt_version(c.tree) != pt_version(a));
    assertok(pt_checktree(c.tree) && pt_checktree(a));
    /* source unchanged */
    pt_asserttree(a, 0, leafV(litV("hello world")));
    /* transient has hole at position 5 */
    asserteq(pt_bytes(c.tree), 14);
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 3);
        assertok(!ptM_ishole(r, 0));
        asserteq(r->bytes[0], 5);
        asserteq(memcmp(r->children[0], "hello", 5), 0);
        assertok(ptM_ishole(r, 1));
        {
            pt_Hole *h = (pt_Hole *)r->children[1];
            asserteq(r->bytes[1], 3);
            asserteq(memcmp(h->data, "XYZ", 3), 0);
        }
        assertok(!ptM_ishole(r, 2));
        asserteq(r->bytes[2], 6);
        asserteq(memcmp(r->children[2], " world", 6), 0);
    }
    /* source has no holes (committed) */
    {
        unsigned i;
        for (i = 0; i < a->root.child_count; ++i)
            assertok(!ptM_ishole(&a->root, i));
    }
    /* independent versions: edit transient, verify source */
    pt_release(c.tree);
    assertok(pt_checktree(a));
    asserteq(pt_bytes(a), 11);
    pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* sec.8.1 edit_rollback: rollback discards holes, returns source buffer */

TEST(edit_rollback) {
    pt_State *S = pt_open(&test_alloc, NULL);

    /* Path 1: source buffer held externally -> rollback returns source */
    {
        pt_Buffer b = pt_empty(S);
        pt_Cursor c;
        pt_seek(&c, b, 0);
        asserteq(pt_edit(&c, 0, "hello", 5), PT_OK);
        assertok(c.dirty);
        asserteq(pt_bytes(c.tree), 5);
        asserteq(pt_rollback(&c), b);
        assertok(!c.dirty);
        asserteq(c.tree, NULL);
        asserteq(pt_bytes(b), 0);
        pt_release(b); /* balance the rollback retain */
        pt_release(b);
        asserteq(S->nodes.live_obj, 0);
        asserteq(S->holes.live_obj, 0);
    }

    /* Path 2: source only via from -> rollback returns sentinel */
    {
        pt_Buffer b = pt_empty(S);
        pt_Cursor c;
        pt_seek(&c, b, 0);
        asserteq(pt_edit(&c, 0, "hello", 5), PT_OK);
        pt_release(b); /* external drops source; sentinel stays alive */
        asserteq(pt_rollback(&c), b); /* sentinel, not NULL */
        assertok(!c.dirty);
        asserteq(c.tree, NULL);
        asserteq(pt_bytes(b), 0);
        pt_release(b);
        asserteq(S->nodes.live_obj, 0);
        asserteq(S->holes.live_obj, 0);
    }

    pt_close(S);
}

/* sec.8.1 edit_oom: reserve failure leaves tree untouched */

TEST(edit_oom) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    int       r;

    pt_seek(&c, b, 0);
    cnt = 0; /* next allocf fails (nodes reserve) */
    r = pt_edit(&c, 0, "x", 1);
    asserteq(r, PT_ERRMEM);
    assertok(!c.dirty);
    asserteq(pt_bytes(c.tree), 0); /* tree untouched */

    cnt = 1000; /* recover */
    asserteq(pt_edit(&c, 0, "ok", 2), PT_OK);
    asserteq(pt_bytes(c.tree), 2);

    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= byte collection helper ================= */

static size_t collect_bytes_r(
        pt_Buffer b, const pt_Node *n, unsigned l, char *buf) {
    unsigned i;
    size_t   off = 0;
    if (l == b->levels) {
        for (i = 0; i < n->child_count; ++i) {
            if (ptM_ishole(n, i)) {
                pt_Hole *h = (pt_Hole *)n->children[i];
                memcpy(buf + off, h->data, n->bytes[i]);
                off += n->bytes[i];
            } else {
                memcpy(buf + off, (const char *)n->children[i], n->bytes[i]);
                off += n->bytes[i];
            }
        }
    } else {
        for (i = 0; i < n->child_count; ++i)
            off += collect_bytes_r(b, n->children[i], l + 1, buf + off);
    }
    return off;
}

static size_t collect_bytes(pt_Buffer b, char *buf, size_t cap) {
    (void)cap;
    if (b->root.child_count == 0) return 0;
    return collect_bytes_r(b, &b->root, 0, buf);
}

/* sec.8.1 edit_brute: position-independent content verification */

/* shared 288-byte reference for maketree-based brute tests:
 * 72 groups of {lit,lit,'#','#'} drawn from pt_srcbuf pairs */
static const char pt_srcbuf
        [] = "abcdefghijklmnopqrstuvwxyz0123456789"
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz0123456789"
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrst";

static void maketree(pt_State *S, pt_Cursor *C, size_t off);

static void makeref(char *ref) {
    int i;
    for (i = 0; i < 72; i++) {
        ref[i * 4 + 0] = pt_srcbuf[i * 2 + 0];
        ref[i * 4 + 1] = pt_srcbuf[i * 2 + 1];
        ref[i * 4 + 2] = '#';
        ref[i * 4 + 3] = '#';
    }
}

TEST(edit_brute) {
    int const nb = 288;
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor C;
    char      ref[288], expected[576], actual[576];
    int       pos;
    size_t    nread;
    makeref(ref);
    for (pos = 0; pos <= nb; ++pos) {
        maketree(S, &C, (size_t)pos);
        asserteq(pt_edit(&C, 0, "##", 2), PT_OK);
        if (!pt_checktree(C.tree)) {
            test_log("edit_brute FAIL pos=%d\n", pos);
            pt_dumptree(C.tree, "after edit");
            pt_checktree(C.tree), abort();
        }
        if (!pt_checkcursor(&C, (size_t)pos + 2)) {
            test_log(
                    "edit_brute cursor pos=%d off=%lu\n", pos,
                    test_lu(pt_offset(&C)));
            abort();
        }
        memcpy(expected, ref, (size_t)pos);
        memcpy(expected + pos, "##", 2);
        memcpy(expected + pos + 2, ref + pos, 288 - (size_t)pos);
        pt_seek(&C, C.tree, 0);
        nread = pt_read(&C, actual, 290);
        if (nread != 290 || memcmp(actual, expected, 290) != 0) {
            test_log(
                    "edit_brute content fail pos=%d nread=%lu\n", pos,
                    test_lu(nread));
            abort();
        }
        pt_release(C.tree);
        asserteq(S->nodes.live_obj, 0);
        asserteq(S->holes.live_obj, 0);
    }
    pt_close(S);
}

/* sec.8.2 insert_brute: position-independent literal insert */

TEST(insert_brute) {
    int const nb = 288;
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor C;
    char      ref[288], expected[576], actual[576];
    int       pos;
    size_t    nread;
    makeref(ref);
    for (pos = 0; pos <= nb; ++pos) {
        maketree(S, &C, (size_t)pos);
        asserteq(pt_insert(&C, "##", 2), PT_OK);
        if (!pt_checktree(C.tree)) {
            test_log("insert_brute FAIL pos=%d\n", pos);
            pt_dumptree(C.tree, "after insert");
            pt_checktree(C.tree), abort();
        }
        if (!pt_checkcursor(&C, (size_t)pos)) {
            test_log(
                    "insert_brute cursor pos=%d off=%lu\n", pos,
                    test_lu(pt_offset(&C)));
            abort();
        }
        memcpy(expected, ref, (size_t)pos);
        memcpy(expected + pos, "##", 2);
        memcpy(expected + pos + 2, ref + pos, 288 - (size_t)pos);
        pt_seek(&C, C.tree, 0);
        nread = pt_read(&C, actual, 290);
        if (nread != 290 || memcmp(actual, expected, 290) != 0) {
            test_log(
                    "insert_brute content fail pos=%d nread=%lu\n", pos,
                    test_lu(nread));
            abort();
        }
        pt_release(C.tree);
        asserteq(S->nodes.live_obj, 0);
        asserteq(S->holes.live_obj, 0);
    }
    pt_close(S);
}

/* ================= pt_scratch tests ================= */

TEST(peekscratch_roundtrip) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    char     *h;
    size_t    cap, used;

    pt_seek(&c, b, 0);

    /* Arena not built yet -> scratch returns NULL */
    cap = 123;
    asserteq(pt_scratch(&c, &cap), NULL);
    asserteq(cap, 0);

    /* Build arena via pt_reserve */
    h = pt_reserve(&c, 0);
    assertok(h != NULL);

    /* First scratch: head at h, cap == PT_ARENA_SIZE */
    cap = 0;
    h = pt_scratch(&c, &cap);
    assertok(h != NULL);
    asserteq(cap, PT_ARENA_SIZE);

    /* Re-scratch: same position, unchanged */
    {
        char  *h2;
        size_t cap2 = 0;
        h2 = pt_scratch(&c, &cap2);
        asserteq(h2, h);
        asserteq(cap2, cap);
    }

    /* Write 10 bytes to scratch area */
    used = 10;
    memcpy(h, "HelloWorld", 10);

    /* Scratch again: still same position */
    {
        char  *h3;
        size_t cap3 = 0;
        h3 = pt_scratch(&c, &cap3);
        asserteq(h3, h);
        asserteq(cap3, cap);
    }

    /* pt_literal: commit 10 bytes, return old head */
    {
        const char *h4 = (pt_reserve(&c, used), pt_literal(&c, used));
        asserteq(h4, h);
        asserteq(used, 10);
    }

    /* Scratch after literal: position advanced by 10 */
    {
        char  *h5;
        size_t cap5 = 0;
        h5 = pt_scratch(&c, &cap5);
        asserteq(h5, h + 10);
        asserteq(cap5, cap - 10);
    }

    /* Insert into tree, verify content */
    {
        char   buf[16];
        size_t blen;
        asserteq(pt_insert(&c, h, 10), PT_OK);
        assertok(pt_checktree(c.tree));
        asserteq(pt_bytes(c.tree), 10);
        blen = collect_bytes(c.tree, buf, sizeof(buf));
        asserteq(blen, 10);
        asserteq(memcmp(buf, "HelloWorld", 10), 0);
    }
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(peekscratch_params) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    cap;

    pt_seek(&c, b, 0); /* init cursor */

    /* NULL cursor */
    cap = 123;
    asserteq(pt_scratch(NULL, &cap), NULL);
    asserteq(cap, 123);

    /* NULL plen */
    asserteq(pt_scratch(&c, NULL), NULL);

    /* Arena not built: scratch returns NULL, *plen set to 0 */
    cap = 456;
    asserteq(pt_scratch(&c, &cap), NULL);
    asserteq(cap, 0);

    pt_release(b);
    pt_close(S);
}

TEST(peekscratch_adjacent) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    char     *h1, *h2;
    size_t    cap, used;

    pt_seek(&c, b, 0);

    /* Build arena and get first scratch */
    pt_reserve(&c, 0);
    cap = 0;
    h1 = pt_scratch(&c, &cap);
    assertok(h1 != NULL);
    asserteq(cap, PT_ARENA_SIZE);

    /* Write 8 bytes, then literal to commit */
    used = 8;
    memcpy(h1, "HelloABC", 8);
    pt_literal(&c, used);

    /* Next scratch: adjacent at h1+8 */
    cap = 0;
    h2 = pt_scratch(&c, &cap);
    asserteq(h2, h1 + 8);

    /* Write 3 bytes, literal to commit */
    used = 3;
    memcpy(h2, "XYZ", 3);
    pt_literal(&c, used);

    /* Insert both into tree, verify content */
    {
        char   buf[16];
        size_t blen;
        asserteq(pt_insert(&c, h1, 8), PT_OK);
        pt_advance(&c, 8);
        asserteq(pt_insert(&c, h2, 3), PT_OK);
        asserteq(pt_bytes(c.tree), 11);
        blen = collect_bytes(c.tree, buf, sizeof(buf));
        asserteq(blen, 11);
        asserteq(memcmp(buf, "HelloABCXYZ", 11), 0);
    }
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= arena tests ================= */

TEST(arena_lazy) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;

    pt_seek(&c, b, 0);

    /* Lazy: arena not built before any edit/reserve */
    asserteq(c.tree->arena.current, NULL);
    asserteq(c.tree->arena.full, NULL);

    /* pt_scratch before any reserve/literal returns NULL, *plen=0 */
    {
        size_t cap = 123;
        asserteq(pt_scratch(&c, &cap), NULL);
        asserteq(cap, 0);
    }

    /* pt_reserve builds the arena */
    assertok(pt_reserve(&c, 0) != NULL);
    assertok(c.tree != b);
    assertok(c.tree->arena.current != NULL);

    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(arena_reserve_full) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;

    pt_seek(&c, b, 0);

    /* First reserve: block allocated with PT_ARENA_SIZE */
    assertok(pt_reserve(&c, 0) != NULL);
    assertok(c.tree->arena.current != NULL);

    /* Fill the block: literal entire capacity -> moves block to full chain */
    {
        size_t n = PT_ARENA_SIZE;
        assertok(pt_literal(&c, n) != NULL);
        asserteq(n, PT_ARENA_SIZE);
    }
    asserteq(c.tree->arena.current, NULL); /* head exhausted */
    assertok(c.tree->arena.full != NULL);  /* moved to full */

    /* Next reserve allocates new block */
    assertok(pt_reserve(&c, 0) != NULL);
    assertok(c.tree->arena.current != NULL);

    /* Custom-sized reserve: block >= max(len, PT_ARENA_SIZE) */
    {
        char *p = pt_reserve(&c, 256);
        assertok(p != NULL);
    }

    /* Oversized reserve: len > PT_ARENA_SIZE takes the other pt_max arm. */
    {
        char *p = pt_reserve(&c, PT_ARENA_SIZE + 1);
        assertok(p != NULL);
    }

    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(arena_dirty_break) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    cap;

    pt_seek(&c, b, 0);
    assertok(!c.dirty);

    /* pt_scratch does NOT trigger dirty (read-only) */
    asserteq(pt_scratch(&c, &cap), NULL);
    assertok(!c.dirty);
    asserteq(c.tree, b); /* no fork */

    /* pt_reserve triggers dirty + builds arena */
    assertok(pt_reserve(&c, 0) != NULL);
    assertok(c.dirty);
    assertok(c.tree != b);

    /* Fork a second cursor from original buffer: independent arena */
    {
        pt_Cursor c2;
        pt_seek(&c2, b, 0);
        assertok(pt_reserve(&c2, 0) != NULL);
        assertok(c2.dirty);
        assertok(c2.tree != c.tree);
        assertok(c2.tree->arena.current != c.tree->arena.current);
        pt_release(c2.tree);
    }

    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(arena_reserve_reuse) {
    /* Exercise pt_reserve for-loop that traverses current chain and
       unlinks a non-head block with enough space (lines 742-750). */
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    n;
    char     *p;

    pt_seek(&c, b, 0);

    /* Build block A (head), consume a little */
    assertok(pt_reserve(&c, 100) != NULL);
    n = 80;
    assertok(pt_literal(&c, n) != NULL);
    asserteq(n, 80);

    /* Block A (size=1024, used=80, rem=944) cannot satisfy len=1000
       -> allocate block B (size=1024, used=0), current = B -> A */
    assertok(pt_reserve(&c, 1000) != NULL);

    /* Consume B heavily so B's remainder < the next request */
    n = 950;
    assertok(pt_literal(&c, n) != NULL);
    asserteq(n, 950);
    /* now current = B(rem=74) -> A(rem=944) */

    /* Request: head B(74) < 200, so loop skips B; A(944) >= 200
       -> A unlinked from chain, moved to head. */
    p = pt_reserve(&c, 200);
    assertok(p != NULL);
    assertok(c.tree->arena.current != NULL);

    /* Verify we can write into it */
    memcpy(p, "test", 4);

    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(arena_literal_cold) {
    /* pt_literal cold-start: arena not built, *plen < PT_ARENA_SIZE */
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    n;
    char     *p;

    pt_seek(&c, b, 0);
    asserteq(c.tree->arena.current, NULL);

    n = 10;
    p = pt_reserve(&c, n);
    assertok(p != NULL);
    p = (char *)pt_literal(&c, n);
    assertok(p != NULL);
    assertok(c.tree->arena.current != NULL);
    asserteq(c.tree->arena.current->size, PT_ARENA_SIZE);
    asserteq(c.tree->arena.current->used, 10);
    memcpy(p, "hello", 5);

    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(arena_literal_params) {
    /* pt_literal parameter validation returning NULL */
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    n;

    pt_seek(&c, b, 0);

    /* NULL cursor */
    n = 10;
    asserteq(pt_literal(NULL, n), NULL);

    /* *plen == 0 */
    n = 0;
    asserteq(pt_literal(&c, n), NULL);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= literal/pt_literal coverage ================= */

/* L750: pt_literal returns NULL when arena.current == NULL */
TEST(literal_arena_empty) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    n;
    pt_seek(&c, b, 0);
    n = 10;
    asserteq(pt_literal(&c, n), NULL);
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* L750: pt_literal returns NULL when arena has insufficient remainder */
TEST(literal_arena_short) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    n;
    pt_seek(&c, b, 0);
    assertok(pt_reserve(&c, 0) != NULL);
    n = PT_ARENA_SIZE - 5;
    assertok(pt_literal(&c, n) != NULL);
    n = 10;
    asserteq(pt_literal(&c, n), NULL);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= hole remove tests ================= */

TEST(remove_params) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    asserteq(pt_remove(NULL, 5), PT_ERRPARAM);
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 0), PT_OK);   /* len==0 */
    asserteq(pt_remove(&c, 100), PT_OK); /* empty tree, clamped to 0 */
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_hole_whole) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "hello", 5), PT_OK); /* fork + hole in dirty tree */
    asserteq(pt_bytes(c.tree), 5);
    assertok(c.dirty);
    pt_locate(&c, 0);
    asserteq(pt_remove(&c, 5), PT_OK); /* remove entire hole */
    assertok(pt_checktree(c.tree));
    asserteq(pt_bytes(c.tree), 0);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_hole_mid) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_edit(&c, 0, "hello world", 11); /* fork + hole in dirty tree */
    asserteq(pt_bytes(c.tree), 11);
    assertok(c.dirty);
    pt_locate(&c, 2);                  /* pos 2: 'l' in "hello world" */
    asserteq(pt_remove(&c, 5), PT_OK); /* delete [2,7): "llo w" from hole */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 2));
    asserteq(pt_bytes(c.tree), 6);
    /* result: hole "heorld" (indices 0-1 + 7-11) = 6 bytes */
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 1);
        assertok(ptM_ishole(r, 0));
        {
            asserteq(r->bytes[0], 6);
            asserteq(memcmp(((pt_Hole *)r->children[0])->data, "heorld", 6), 0);
        }
    }
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_hole_boundary) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    /* build dirty tree: lit("abc") + hole("DEF") + lit("ghi") = 9B */
    pt_seek(&c, b, 0);
    pt_insert(&c, "abc", 3);
    pt_advance(&c, 3);
    pt_edit(&c, 0, "DEF", 3);
    pt_advance(&c, 3);
    pt_insert(&c, "ghi", 3);
    asserteq(pt_bytes(c.tree), 9);
    assertok(c.dirty);
    /* delete [2,6): "c" (tail of lit) + "DEF" (whole hole) */
    pt_locate(&c, 2);
    asserteq(pt_remove(&c, 4), PT_OK);
    assertok(pt_checktree(c.tree));
    /* result: "ab"(lit) + "ghi"(lit) -- no hole left */
    pt_asserttree(c.tree, 0, leafV(litV("ab"), litV("ghi")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_hole_mixed) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    /* build dirty tree: lit("abc") + hole("DEF") + lit("ghi") = 9B */
    pt_seek(&c, b, 0);
    pt_insert(&c, "abc", 3);
    pt_advance(&c, 3);
    pt_edit(&c, 0, "DEF", 3);
    pt_advance(&c, 3);
    pt_insert(&c, "ghi", 3);
    asserteq(pt_bytes(c.tree), 9);
    assertok(c.dirty);
    /* delete [1,5): "bc"(2 from lit") + "DE"(2 from hole) */
    pt_locate(&c, 1);
    asserteq(pt_remove(&c, 4), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 1));
    /* result: "a"(lit) + hole"F"(1) + "ghi"(lit) */
    asserteq(pt_bytes(c.tree), 5);
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 3);
        assertok(!ptM_ishole(r, 0)); /* lit "a" */
        assertok(ptM_ishole(r, 1));  /* hole "F" */
        assertok(!ptM_ishole(r, 2)); /* lit "ghi" */
        {
            pt_Hole *h = (pt_Hole *)r->children[1];
            asserteq(r->bytes[1], 1);
            asserteq(h->data[0], 'F');
        }
    }
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_cow) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer a;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "abcdef", 6);
    a = pt_commit(&c); /* committed buffer */
    pt_seek(&c, a, 2);
    asserteq(pt_remove(&c, 2), PT_OK); /* remove from committed tree */
    assertok(pt_checktree(c.tree));
    assertok(pt_checktree(a)); /* source unchanged */
    asserteq(pt_bytes(a), 6);
    asserteq(pt_bytes(c.tree), 4);
    pt_asserttree(c.tree, 0, leafV(litV("ab"), litV("ef")));
    pt_asserttree(a, 0, leafV(litV("abcdef")));
    pt_release(c.tree), pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_oom) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b = pt_empty(S);
    pt_Buffer a;
    pt_Cursor c;
    int       r;
    /* Create a committed tree with content */
    pt_seek(&c, b, 0);
    asserteq(pt_insert(&c, "abcd", 4), PT_OK);
    a = pt_commit(&c); /* c.tree == a, refcount remains 1 */
    asserteq(pt_bytes(a), 4);
    /* Drain freelist so reserve must allocate a new page */
    {
        pt_Drain d = pt_drainpool(&S->nodes);
        cnt = 0; /* next alloc fails */
        pt_seek(&c, a, 1);
        r = pt_remove(&c, 2);
        asserteq(r, PT_ERRMEM);
        assertok(!c.dirty);
        cnt = 1000;
        pt_refillpool(&S->nodes, d);
    }
    pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_brute) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    total;
    size_t    pos, len;
    /* construct tree with 20 bytes in 2 pieces via two appends */
    pt_seek(&c, b, 0);
    pt_append(&c, "ABCDEFGHIJ", 10);
    pt_append(&c, "abcdefghij", 10);
    total = pt_bytes(c.tree);
    asserteq(total, 20);
    asserteq(c.tree->levels, 0);
    /* brute all (pos,len) that yield positive deletion */
    for (pos = 0; pos < total; ++pos) {
        size_t maxlen = total - pos;
        for (len = 1; len <= maxlen; ++len) {
            pt_Buffer fresh = pt_empty(S);
            pt_Cursor cc;
            /* rebuild tree from scratch via one append */
            pt_seek(&cc, fresh, 0);
            pt_append(&cc, "ABCDEFGHIJabcdefghij", 20);
            pt_release(fresh);
            asserteq(pt_bytes(cc.tree), 20);
            /* remove */
            pt_locate(&cc, pos);
            asserteq(pt_remove(&cc, len), PT_OK);
            if (!pt_checktree(cc.tree)) {
                test_log(
                        "FAIL: remove_brute pos=%lu len=%lu\n", test_lu(pos),
                        test_lu(len));
                abort();
            }
            asserteq(pt_bytes(cc.tree), total - len);
            pt_release(cc.tree);
        }
    }
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_stitch_full) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    /* COW + cross-leaf eraserange: commit tree then remove across leaves */
    pt_Buffer b = treeV(
            1, innerV(leafV(litV("aa"), litV("bb")),
                      leafV(litV("cc"), litV("dd"))));
    pt_seek(&c, b, 2);
    asserteq(pt_remove(&c, 4), PT_OK); /* delete "bb"+"cc" cross-leaf */
    assertok(pt_checktree(c.tree));
    asserteq(pt_bytes(c.tree), 4); /* "aa"+"dd" */
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_fold_balance) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    /* levels=1: inner(leaf("a"), leaf("x","y","z","w"))
       Remove "a" -> left leaf empty, foldnode tries merge but
       totals > FANOUT(4) -> triggers balancenode */
    pt_Buffer b = treeV(
            1, innerV(leafV(litV("a")),
                      leafV(litV("x"), litV("y"), litV("z"), litV("w"))));
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 3), PT_OK);
    assertok(pt_checktree_allow_empty(c.tree, 1));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_hole_trim) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 1, 0, leafV(holeV("abc"), holeV("def")));
    asserteq(pt_remove(&c, 4), PT_OK);
    asserteq(pt_bytes(c.tree), 2);
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* seam merge: delete a non-contiguous piece separating twosame-source buf
   fragments, so thefragments become physically adjacent -> mergeleaf fuses
   them. Covers: same node, cross node, multi-element cross node. */
TEST(remove_merge_literal) {
    static const char buf[] = "abcdef";
    /* --- same node (levels=0): [abc][SEP][def] delete SEP --- */
    {
        pt_State *S = pt_open(&test_alloc, NULL);
        pt_Node  *lf = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Buffer b;
        pt_Cursor c;
        memset(lf, 0, sizeof(pt_Node));
        lf->children[0] = (pt_Node *)(buf + 0), lf->bytes[0] = 3;
        lf->children[1] = (pt_Node *)"SEP", lf->bytes[1] = 3;
        lf->children[2] = (pt_Node *)(buf + 3), lf->bytes[2] = 3;
        lf->child_count = 3;
        b = treeV(0, lf);
        pt_seek(&c, b, 3);
        asserteq(pt_remove(&c, 3), PT_OK);
        assertok(pt_checktree(c.tree) && pt_checkcursor(&c, 3));
        pt_asserttree(c.tree, 0, leafV(litV("abcdef")));
        pt_release(c.tree), pt_release(b);
        asserteq(S->nodes.live_obj, 0);
        asserteq(S->holes.live_obj, 0);
        pt_close(S);
    }
    /* --- cross node (levels=1): [abc,P][Q,def] delete P+Q --- */
    {
        pt_State *S = pt_open(&test_alloc, NULL);
        pt_Node  *l0 = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Node  *l1 = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Buffer b;
        pt_Cursor c;
        memset(l0, 0, sizeof(pt_Node)), memset(l1, 0, sizeof(pt_Node));
        l0->children[0] = (pt_Node *)(buf + 0), l0->bytes[0] = 3;
        l0->children[1] = (pt_Node *)"P", l0->bytes[1] = 1;
        l0->child_count = 2;
        l1->children[0] = (pt_Node *)"Q", l1->bytes[0] = 1;
        l1->children[1] = (pt_Node *)(buf + 3), l1->bytes[1] = 3;
        l1->child_count = 2;
        b = treeV(1, innerV(l0, l1));
        pt_seek(&c, b, 3);
        asserteq(pt_remove(&c, 2), PT_OK);
        assertok(pt_checktree(c.tree) && pt_checkcursor(&c, 3));
        pt_asserttree(c.tree, 0, leafV(litV("abcdef")));
        pt_release(c.tree), pt_release(b);
        asserteq(S->nodes.live_obj, 0);
        asserteq(S->holes.live_obj, 0);
        pt_close(S);
    }
    /* --- multi-element cross node: [abc,X,Y][Z,def] delete X+Y+Z --- */
    {
        pt_State *S = pt_open(&test_alloc, NULL);
        pt_Node  *l0 = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Node  *l1 = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Buffer b;
        pt_Cursor c;
        memset(l0, 0, sizeof(pt_Node)), memset(l1, 0, sizeof(pt_Node));
        l0->children[0] = (pt_Node *)(buf + 0), l0->bytes[0] = 3;
        l0->children[1] = (pt_Node *)"X", l0->bytes[1] = 1;
        l0->children[2] = (pt_Node *)"Y", l0->bytes[2] = 1;
        l0->child_count = 3;
        l1->children[0] = (pt_Node *)"Z", l1->bytes[0] = 1;
        l1->children[1] = (pt_Node *)(buf + 3), l1->bytes[1] = 3;
        l1->child_count = 2;
        b = treeV(1, innerV(l0, l1));
        pt_seek(&c, b, 3);
        asserteq(pt_remove(&c, 3), PT_OK);
        assertok(pt_checktree(c.tree) && pt_checkcursor(&c, 3));
        pt_asserttree(c.tree, 0, leafV(litV("abcdef")));
        pt_release(c.tree), pt_release(b);
        asserteq(S->nodes.live_obj, 0);
        asserteq(S->holes.live_obj, 0);
        pt_close(S);
    }
}

/* === hole merge tests (mergeleaf full+partial merge) === */

TEST(remove_merge_hole_full) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 10, 1,
          innerV(leafV(holeV("AAAAAAAAAA"), litV("X")),
                 leafV(holeV("BBBBBB"))));
    asserteq(pt_remove(&c, 1), PT_OK);
    assertok(pt_checktree(c.tree) && pt_checkcursor(&c, 10));
    asserteq(pt_bytes(c.tree), 16);
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(remove_merge_hole_split) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    /* mergeleaf full merge: hole A(10) + hole B(5) = 15 <= 16 */
    editV(&c, 10, 1,
          innerV(leafV(holeV("AAAAAAAAAA"), litV("X")), leafV(holeV("BBBBB"))));
    asserteq(pt_remove(&c, 1), PT_OK);
    assertok(pt_checktree(c.tree));
    asserteq(pt_bytes(c.tree), 15);
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* L1154 + L1184-1187: mergeleaf partial hole merge + stitch backwardnode.
   Two hole leaves (12+12=24B), delete 4B at boundary.
   mergeleaf: d=min(10,16-10)=6, partial, ptH_remove(rt,0,0,d).
   stitch: d>poff=0 -> backwardnode. */
TEST(remove_merge_hole_partial) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 10, 1,
          innerV(leafV(holeV("aaaaaaaaaaaa")), leafV(holeV("bbbbbbbbbbbb"))));
    asserteq(pt_remove(&c, 4), PT_OK);
    assertok(pt_checktree(c.tree));
    asserteq(pt_bytes(c.tree), 20);
    assertok(pt_checkcursor(&c, 10));
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 2);
        asserteq(r->bytes[0], 16);
        asserteq(r->bytes[1], 4);
    }
    {
        char   buf[32];
        size_t nr;
        pt_seek(&c, c.tree, 0);
        nr = pt_read(&c, buf, 20);
        asserteq(nr, 20);
        asserteq(memcmp(buf, "aaaaaaaaaabbbbbbbbbb", 20), 0);
    }
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* === deep stitch + findroom/backwardnode === */

TEST(remove_stitch_deep) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b;
    pt_Cursor c;
    /* levels=1: root with 3 leaves, each with 2 pieces.
       Delete 6 bytes across all 3 leaves -> triggers stitch+backwardnode. */
    b = treeV(
            1,
            innerV(leafV(litV("aa"), litV("bb")), leafV(litV("cc"), litV("dd")),
                   leafV(litV("ee"), litV("ff"))));
    pt_seek(&c, b, 2);                 /* start of "bb" */
    asserteq(pt_remove(&c, 8), PT_OK); /* delete "bb"+"cc"+"dd"+"ee" = 8B */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 2));
    asserteq(pt_bytes(c.tree), 4); /* "aa"+"ff" remain */
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* === trimright poff==0 + mask === */

TEST(remove_trim_hole) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 0, 1,
          innerV(leafV(holeV("xy"), litV("ab")),
                 leafV(litV("cd"), litV("ef"))));
    asserteq(pt_remove(&c, 4), PT_OK);
    assertok(pt_checktree(c.tree));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* === eraseleaf cross-piece hole === */

TEST(remove_hole_eraseleaf) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_edit(&c, 0, "hole1", 5); /* hole("hole1") at pos 0 */
    pt_insert(&c, "lit", 3);    /* lit at pos 5 */
    pt_advance(&c, 3);          /* to pos 8 */
    pt_edit(&c, 0, "hole2", 5); /* hole("hole2") at pos 8 */
    pt_release(b);
    pt_locate(&c, 4);
    asserteq(pt_remove(&c, 3), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 4));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* === mergelit left-side merge === */

/* ================= stitch findroom test ================= */

/* full levels=2 tree: 4 inners x 4 leaves x 4 pieces, 1B each, from
 * non-contiguous slices of src (stride 2 defeats literal merging) */
static pt_Buffer brute2_tree(pt_State *S, const char *src) {
    pt_Node *root = (pt_Node *)ptP_alloc(S, &S->nodes);
    int      a, b, ci;
    memset(root, 0, sizeof(pt_Node));
    for (a = 0; a < 4; ++a) {
        pt_Node *in = (pt_Node *)ptP_alloc(S, &S->nodes);
        memset(in, 0, sizeof(pt_Node));
        for (b = 0; b < 4; ++b) {
            pt_Node *lf = (pt_Node *)ptP_alloc(S, &S->nodes);
            memset(lf, 0, sizeof(pt_Node));
            for (ci = 0; ci < 4; ++ci) {
                lf->children[ci] = (pt_Node *)(src + 2 * (a * 16 + b * 4 + ci));
                lf->bytes[ci] = 1;
            }
            ptN_setcc(lf, 4);
            in->children[b] = lf, in->bytes[b] = 4;
        }
        ptN_setcc(in, 4);
        root->children[a] = in, root->bytes[a] = 16;
    }
    ptN_setcc(root, 4);
    return treeV_(S, 2, root);
}

/* brute all (pos,len) removals on a full levels=2 tree, checking tree
 * invariants, byte count, content and cursor position after each */
TEST(remove_brute2) {
    static char src[128];
    char        expect[64], rd[64];
    pt_State   *S = pt_open(&test_alloc, NULL);
    size_t      pos, len, k;
    for (k = 0; k < 128; ++k) src[k] = (char)('A' + (int)k % 26);
    for (k = 0; k < 64; ++k) expect[k] = src[2 * k];
    for (pos = 0; pos <= 64; ++pos) {
        for (len = 1; len <= 64 - pos; ++len) {
            pt_Buffer b = brute2_tree(S, src);
            pt_Cursor c;
            pt_seek(&c, b, pos);
            asserteq(pt_remove(&c, len), PT_OK);
            if (!pt_checktree_allow_empty(c.tree, 1)
                || pt_bytes(c.tree) != 64 - len || !pt_checkcursor(&c, pos)) {
                test_log(
                        "FAIL: brute2 pos=%lu len=%lu\n", test_lu(pos),
                        test_lu(len));
                pt_dumptree(c.tree, "brute2");
                abort();
            }
            pt_locate(&c, 0);
            asserteq(pt_read(&c, rd, 64), 64 - len);
            asserteq(memcmp(rd, expect, pos), 0);
            asserteq(memcmp(rd + pos, expect + pos + len, 64 - pos - len), 0);
            pt_release(c.tree), pt_release(b);
        }
    }
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* L1102-1103: tail rmleaf -> rebalance(l-1) -> foldnode balances leaves
   (4+1 > FANOUT so balancenode path, tree stays legal) */
TEST(remove_fold_balance2) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(
            2, innerV(innerV(leafV(litV("a"), litV("b")),
                             leafV(litV("c"), litV("d"))),
                      innerV(leafV(litV("e"), litV("f"), litV("g"), litV("h")),
                             leafV(litV("i"), litV("j")))));
    pt_Cursor c;
    pt_seek(&c, b, 9);
    asserteq(pt_remove(&c, 1), PT_OK); /* erase tail "j": leaf cc 2 -> 1 */
    assertok(pt_checktree(c.tree));
    asserteq(pt_bytes(c.tree), 9);
    assertok(pt_checkcursor(&c, 9));
    pt_asserttree(
            c.tree, 2,
            innerV(innerV(leafV(litV("a"), litV("b")),
                          leafV(litV("c"), litV("d"))),
                   innerV(leafV(litV("e"), litV("f")),
                          leafV(litV("g"), litV("h"), litV("i")))));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* tail rmleaf -> rebalance -> foldnode merges leaves (2+1 <= FANOUT):
   inner cc drops to 1 while root cc == 2; tree must stay legal */
TEST(remove_fold_merge) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(
            2, innerV(innerV(leafV(litV("a"), litV("b")),
                             leafV(litV("c"), litV("d"))),
                      innerV(leafV(litV("g"), litV("h")),
                             leafV(litV("i"), litV("j")))));
    pt_Cursor c;
    pt_seek(&c, b, 7);
    asserteq(pt_remove(&c, 1), PT_OK); /* erase tail "j": leaf cc 2 -> 1 */
    assertok(pt_checktree(c.tree));
    asserteq(pt_bytes(c.tree), 7);
    assertok(pt_checkcursor(&c, 7));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* L1033-1038: ptD_findroom fl>=0 && c>0. */
TEST(remove_findroom) {
    pt_Node  *root, *inner0, *inner1, *leaf0, *leaf1, *leaf2, *leaf3, *leaf4;
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b;
    pt_Cursor c;
    /* levels=2: root -> [inner0, inner1]; inner0 full (4 leaves), inner1: 1
       leaf. Total 18B. Remove 6 at offset 12 -> crosses inner boundary. */
    leaf0 = leafV(litV("aa"), litV("bb"), litV("cc"), litV("dd"));
    leaf1 = leafV(litV("ee"), litV("ff"), litV("gg"), litV("hh"));
    leaf2 = leafV(litV("ii"), litV("jj"), litV("kk"), litV("ll"));
    leaf3 = leafV(litV("mm"), litV("nn"), litV("oo"), litV("pp"));
    leaf4 = leafV(litV("qq"), litV("rr"));
    inner0 = innerV(leaf0, leaf1, leaf2, leaf3);
    inner1 = innerV(leaf4);
    root = innerV(inner0, inner1);
    b = treeV(2, root);
    pt_seek(&c, b, 12);
    asserteq(pt_remove(&c, 6), PT_OK);
    assertok(pt_checktree_allow_empty(c.tree, 1));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* brute (pos,len) removals over mixed-shape levels=2 trees
 * (leaf cc 4/3/2 mixes stress fold/stitch/findroom paths) */
static pt_Buffer brute3_tree(pt_State *S, const char *src, const int *shape) {
    pt_Node *root = (pt_Node *)ptP_alloc(S, &S->nodes);
    int      a, b, ci, pi, si = 0, off = 0;
    memset(root, 0, sizeof(pt_Node));
    for (a = 0; a < 4; ++a) {
        pt_Node *n1 = (pt_Node *)ptP_alloc(S, &S->nodes);
        int      n1b = 0;
        memset(n1, 0, sizeof(pt_Node));
        for (b = 0; b < 4; ++b) {
            pt_Node *lc = (pt_Node *)ptP_alloc(S, &S->nodes);
            int      cc = shape[si++ % 7], lcb = 0;
            memset(lc, 0, sizeof(pt_Node));
            for (ci = 0; ci < cc; ++ci) {
                pi = shape[si++ % 7] > 2 ? 2 : 1;
                lc->children[ci] = (pt_Node *)(src + off);
                lc->bytes[ci] = pi, off += pi + 1, lcb += pi;
            }
            ptN_setcc(lc, cc);
            n1->children[b] = lc, n1->bytes[b] = lcb, n1b += lcb;
        }
        ptN_setcc(n1, 4);
        root->children[a] = n1, root->bytes[a] = n1b;
    }
    ptN_setcc(root, 4);
    return treeV_(S, 2, root);
}
TEST(remove_brute3) {
    static char      src[1024];
    static const int shapes[3][7] = {
            {4, 3, 2, 4, 2, 3, 4},
            {2, 2, 3, 2, 4, 2, 2},
            {4, 4, 4, 4, 4, 4, 4}};
    pt_State *S = pt_open(&test_alloc, NULL);
    size_t    k, pos, len, total;
    int       si;
    for (k = 0; k < 1024; ++k) src[k] = (char)('A' + (int)k % 26);
    for (si = 0; si < 3; ++si) {
        pt_Buffer b0 = brute3_tree(S, src, shapes[si]);
        pt_Cursor c;
        total = pt_bytes(b0);
        pt_release(b0);
        for (pos = 0; pos < total; ++pos) {
            for (len = 1; len <= total - pos; ++len) {
                pt_Buffer b = brute3_tree(S, src, shapes[si]);
                pt_seek(&c, b, pos);
                asserteq(pt_remove(&c, len), PT_OK);
                if (!pt_checktree_allow_empty(c.tree, 1)
                    || pt_bytes(c.tree) != total - len
                    || !pt_checkcursor(&c, pos)) {
                    test_log(
                            "FAIL brute3 s=%d pos=%lu len=%lu\n", si,
                            test_lu(pos), test_lu(len));
                    abort();
                }
                pt_release(c.tree), pt_release(b);
            }
        }
    }
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= splice tests ================= */

TEST(splice_basic) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("hello")));
    pt_Cursor c;
    pt_seek(&c, b, 1);
    asserteq(pt_splice(&c, 3, "XYZ", 3), PT_OK); /* del "ell", ins "XYZ" */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 4)); /* cursor after remove+append: pos 1+3=4 */
    pt_asserttree(c.tree, 0, leafV(litV("h"), litV("XYZ"), litV("o")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(splice_del0) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("abc")));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_splice(&c, 0, "XYZ", 3), PT_OK); /* del=0 -> insert only */
    assertok(pt_checktree(c.tree));
    pt_asserttree(c.tree, 0, leafV(litV("XYZ"), litV("abc")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(splice_null) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("hello")));
    pt_Cursor c;
    pt_seek(&c, b, 1);
    asserteq(pt_splice(&c, 3, NULL, 0), PT_OK); /* del "ell", no insert */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 1));
    pt_asserttree(c.tree, 0, leafV(litV("h"), litV("o")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= stitch coverage: findroom/backwardnode ================= */

TEST(remove_stitch_overflow) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(
            2, innerV(innerV(leafV(litV("a")), leafV(litV("b"))),
                      innerV(leafV(litV("c")), leafV(litV("d"))),
                      innerV(leafV(litV("e")))));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 3), PT_OK);
    assertok(pt_checktree_allow_empty(c.tree, 1));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* levels=1: single-leaf rmleaf folds the root's two leaf children.  The
   leaf boundary holds adjacent literals (legal across leaves, e.g. after
   compact); the fold splice point turns them into an in-leaf adjacency
   that foldnode never absorbs. */
TEST(remove_fold_adjacent_lit) {
    static char mem[20];
    pt_Node    *lfA, *lfB;
    pt_Buffer   b;
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Cursor   c;
    memcpy(mem + 0, "aaaa", 4);
    memcpy(mem + 6, "bbbb", 4);
    memcpy(mem + 10, "cccc", 4);
    memcpy(mem + 16, "dddd", 4);
    lfA = leafV(litV_(mem + 0, 4), litV_(mem + 6, 4));
    lfB = leafV(litV_(mem + 10, 4), litV_(mem + 16, 4));
    b = treeV(1, innerV(lfA, lfB));
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 4), PT_OK);
    assertok(pt_checktree(c.tree));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= seam merge coverage: insert paths =================
   All construct in-leaf adjacent literals that are also physically
   contiguous (same origin buffer), then exercise the merge sites:
   case1 (append bridges prev), case2 (append bridges next),
   splitins po==n (inserted data bridges the following piece). */

TEST(seam_insert_append_left) {
    static const char buf[] = "abcdefXYZ";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = treeV(0, leafV(litV_(buf + 0, 3), litV_(buf + 6, 3)));
    pt_Cursor         c;
    pt_seek(&c, b, 3);                          /* cursor at head of "XYZ" */
    asserteq(pt_append(&c, buf + 3, 3), PT_OK); /* "def" bridges both */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 6));
    pt_asserttree(c.tree, 0, leafV(litV_(buf + 0, 9)));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(seam_insert_append_right) {
    static const char buf[] = "abcdefg";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = treeV(0, leafV(litV_(buf + 0, 3), litV_(buf + 6, 1)));
    pt_Cursor         c;
    pt_seek(&c, b, 3);                          /* cursor at tail of "abc" */
    asserteq(pt_append(&c, buf + 3, 3), PT_OK); /* "def" bridges both */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 6));
    pt_asserttree(c.tree, 0, leafV(litV_(buf + 0, 7)));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(seam_insert_tail_bridge) {
    static const char buf[] = "abcdefgh";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = treeV(0, leafV(litV_(buf + 0, 2), litV_(buf + 5, 3)));
    pt_Cursor         c;
    pt_seek(&c, b, 2);                          /* cursor at tail of "ab" */
    asserteq(pt_append(&c, buf + 3, 2), PT_OK); /* "cd" bridges next */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 4));
    pt_asserttree(c.tree, 0, leafV(litV_(buf + 0, 2), litV_(buf + 3, 5)));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= seam merge coverage: remove paths =================
   hole-erase fuses both sides; head/tail shrink re-points a piece onto
   its neighbor (overlapping origin buffers are legal -- the checker only
   tests adjacency); foldnode pre-merges the orig-left-last with
   orig-right-first at the boundary before the merge/balance decision, so
   a boundary seam drops cL+cR by one and can turn balance into merge. */

TEST(seam_remove_hole_erase) {
    static const char buf[] = "abcdef";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Cursor         c;
    editV(&c, 0, 0, leafV(litV_(buf + 0, 3), holeV("X"), litV_(buf + 3, 3)));
    pt_seek(&c, c.tree, 3);
    asserteq(pt_remove(&c, 1), PT_OK);
    assertok(pt_checktree(c.tree));
    pt_asserttree(c.tree, 0, leafV(litV_(buf + 0, 6)));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(seam_remove_head_shrink) {
    static const char buf[] = "abcdefghi";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = treeV(0, leafV(litV_(buf + 4, 4), litV_(buf + 5, 4)));
    pt_Cursor         c;
    pt_seek(&c, b, 4); /* head of second piece */
    asserteq(pt_remove(&c, 3), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 4));
    pt_asserttree(c.tree, 0, leafV(litV_(buf + 4, 5)));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(seam_remove_tail_shrink) {
    static const char buf[] = "abcdefgh";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = treeV(0, leafV(litV_(buf + 0, 3), litV_(buf + 4, 4)));
    pt_Cursor         c;
    pt_seek(&c, b, 5); /* inside "efgh", 4 bytes remain to the end */
    asserteq(pt_remove(&c, 4), PT_OK); /* clamp: rmleaf tail-shrink last */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 5));
    pt_asserttree(c.tree, 0, leafV(litV_(buf + 0, 3), litV_(buf + 4, 2)));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* append extends a literal whose right neighbor is a hole: seamleaf must
 * not merge the extended literal with the hole. */
TEST(seam_append_skip_hole_right) {
    static const char buf[] = "abcdef";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Cursor         c;
    editV(&c, 3, 0, leafV(litV_(buf + 0, 3), holeV("X")));
    asserteq(pt_append(&c, buf + 3, 3), PT_OK); /* "abc"+"def" contiguous */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 6));
    pt_asserttree(c.tree, 0, leafV(litV_(buf + 0, 6), holeV("X")));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* append extends a literal whose right neighbor is a non-contiguous
 * literal: seamleaf must not merge them. */
TEST(seam_append_skip_noncontig) {
    static const char buf[8] = "abcdef";
    static const char sb[8]  = "def";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Cursor         c;
    editV(&c, 3, 0, leafV(litV_(buf, 3), litV_(sb, 3)));
    asserteq(pt_append(&c, buf + 3, 3), PT_OK); /* extends first literal */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 6));
    pt_asserttree(c.tree, 0, leafV(litV_(buf, 6), litV_(sb, 3)));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* append extends a literal and the next literal is physically contiguous:
 * seamleaf merges them while the cursor stays on the left piece. */
TEST(seam_append_merge_right) {
    static const char buf[] = "abcdefghij";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Cursor         c;
    editV(&c, 3, 0, leafV(litV_(buf + 0, 3), litV_(buf + 6, 3)));
    asserteq(pt_append(&c, buf + 3, 3), PT_OK); /* bridges to next literal */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 6));
    pt_asserttree(c.tree, 0, leafV(litV_(buf + 0, 9)));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* seamleaf bails out when the right neighbor is a hole: tail-shrink of a
 * literal next to a hole must not try to merge them. */
TEST(seam_skip_hole_right) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 1, 0, leafV(litV("abc"), holeV("X")));
    asserteq(pt_remove(&c, 2), PT_OK); /* "abc" -> "a"; next is a hole */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 1));
    pt_asserttree(c.tree, 0, leafV(litV("a"), holeV("X")));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* seamleaf bails out when the two literals are not physically contiguous:
 * tail-shrink must leave them as separate pieces. */
TEST(seam_skip_noncontig) {
    static const char sa[8] = "abc";
    static const char sb[8] = "def";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Cursor         c;
    editV(&c, 1, 0, leafV(litV_(sa, 3), litV_(sb, 3)));
    asserteq(pt_remove(&c, 2), PT_OK); /* "abc" -> "a"; not contiguous */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 1));
    pt_asserttree(c.tree, 0, leafV(litV_(sa, 1), litV_(sb, 3)));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(seam_remove_balance_left) {
    static const char buf[] = "abcdefghij";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = treeV(
            1,
            innerV(leafV(litV("a"), litV_(buf + 0, 3)),
                   leafV(litV_(buf + 3, 3), litV("c"), litV("d"), litV("e"))));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 1), PT_OK); /* drop "a": left leaf underfull */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 0));
    /* seambound pre-merge fuses "abc"+"def" (physically adjacent), cL+cR
     * drops to 4 <= FANOUT so foldnode merges the pair into one leaf. */
    pt_asserttree(
            c.tree, 0,
            leafV(litV_(buf + 0, 6), litV("c"), litV("d"), litV("e")));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(seam_remove_balance_right) {
    static const char buf[] = "abcdefghij";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = treeV(
            1, innerV(leafV(litV("a"), litV("b"), litV("c"), litV_(buf + 0, 3)),
                      leafV(litV("Y"), litV_(buf + 3, 3))));
    pt_Cursor c;
    pt_seek(&c, b, 6);                 /* cursor at head of "Y" */
    asserteq(pt_remove(&c, 1), PT_OK); /* drop "Y": right leaf underfull */
    assertok(pt_checktree(c.tree));
    /* seambound pre-merge fuses "abc"+"def" (physically adjacent), cL+cR
     * drops to 4 <= FANOUT so foldnode merges the pair into one leaf. */
    pt_asserttree(
            c.tree, 0,
            leafV(litV("a"), litV("b"), litV("c"), litV_(buf + 0, 6)));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= fold balance coverage: balancenode ================= */

TEST(remove_foldnode_balance) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(
            3, innerV(innerV(innerV(leafV(litV("a")), leafV(litV("b"))),
                             innerV(leafV(litV("c")), leafV(litV("d")))),
                      innerV(innerV(leafV(litV("e"))))));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 3), PT_OK);
    assertok(pt_checktree_allow_empty(c.tree, 1));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* L1102-1103: ptD_rebalance foldnode path.
   levels=2: inner of 2 leaves where leaf0 has 1 piece.
   Removing that 1 piece -> leaf cc=0 < 2, inner cc=2 > 1 -> foldnode. */
TEST(remove_foldnode) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(
            2, innerV(innerV(leafV(litV("a")),
                             leafV(litV("b"), litV("c"), litV("d"), litV("e"))),
                      innerV(leafV(litV("f")))));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_remove(&c, 1), PT_OK);
    assertok(pt_checktree_allow_empty(c.tree, 1));
    asserteq(pt_bytes(c.tree), 5);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(from_basic) {
    static const char buf[] = "hello world";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = pt_from(S, buf, (size_t)strlen(buf));
    pt_Cursor         c;
    assertok(b);
    asserteq(pt_bytes(b), 11);
    assertok(pt_checktree(b));
    pt_seek(&c, b, 0);
    assertok(pt_checkcursor(&c, 0));
    pt_seek(&c, b, 5);
    asserteq(pt_insert(&c, "!", 1), PT_OK); /* insert at space */
    asserteq(pt_bytes(c.tree), 12);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= pt_edit tests ================= */

TEST(edit_params) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    asserteq(pt_edit(NULL, 0, "x", 1), PT_ERRPARAM);
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "x", PT_MAX_HOLESIZE + 1), PT_ERRPARAM);
    asserteq(pt_edit(&c, 0, NULL, 1), PT_ERRPARAM);
    asserteq(pt_edit(&c, 0, NULL, 0), PT_OK);
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_empty) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "hello", 5), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 5));
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 1);
        assertok(ptM_ishole(r, 0));
        {
            pt_Hole *h = (pt_Hole *)r->children[0];
            asserteq(r->bytes[0], 5);
            asserteq(memcmp(h->data, "hello", 5), 0);
        }
    }
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_fresh_lit_mid) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("abcdef")));
    pt_Cursor c;
    pt_seek(&c, b, 3);
    asserteq(pt_edit(&c, 0, "XYZ", 3), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 6));
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 3);
        assertok(!ptM_ishole(r, 0));
        asserteq(r->bytes[0], 3);
        asserteq(memcmp(r->children[0], "abc", 3), 0);
        assertok(ptM_ishole(r, 1));
        {
            pt_Hole *h = (pt_Hole *)r->children[1];
            asserteq(r->bytes[1], 3);
            asserteq(memcmp(h->data, "XYZ", 3), 0);
        }
        assertok(!ptM_ishole(r, 2));
        asserteq(r->bytes[2], 3);
        asserteq(memcmp(r->children[2], "def", 3), 0);
    }
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_fresh_boundary) {
    pt_State *S = pt_open(&test_alloc, NULL);

    /* poff==0: insert before piece */
    {
        pt_Buffer b = treeV(0, leafV(litV("abcdef")));
        pt_Cursor c;
        pt_seek(&c, b, 0);
        asserteq(pt_edit(&c, 0, "XYZ", 3), PT_OK);
        assertok(pt_checktree(c.tree));
        assertok(pt_checkcursor(&c, 3));
        {
            pt_Node *r = &c.tree->root;
            asserteq(r->child_count, 2);
            assertok(ptM_ishole(r, 0));
            {
                pt_Hole *h = (pt_Hole *)r->children[0];
                asserteq(r->bytes[0], 3);
                asserteq(memcmp(h->data, "XYZ", 3), 0);
            }
            assertok(!ptM_ishole(r, 1));
            asserteq(r->bytes[1], 6);
            asserteq(memcmp(r->children[1], "abcdef", 6), 0);
        }
        pt_release(c.tree), pt_release(b);
    }

    /* poff==len: insert after piece */
    {
        pt_Buffer b = treeV(0, leafV(litV("abcdef")));
        pt_Cursor c;
        pt_seek(&c, b, 6);
        asserteq(pt_edit(&c, 0, "XYZ", 3), PT_OK);
        assertok(pt_checktree(c.tree));
        assertok(pt_checkcursor(&c, 9));
        {
            pt_Node *r = &c.tree->root;
            asserteq(r->child_count, 2);
            assertok(!ptM_ishole(r, 0));
            asserteq(r->bytes[0], 6);
            asserteq(memcmp(r->children[0], "abcdef", 6), 0);
            assertok(ptM_ishole(r, 1));
            {
                pt_Hole *h = (pt_Hole *)r->children[1];
                asserteq(r->bytes[1], 3);
                asserteq(memcmp(h->data, "XYZ", 3), 0);
            }
        }
        pt_release(c.tree), pt_release(b);
    }

    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_append_tail) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "hello", 5), PT_OK);
    assertok(c.dirty);
    pt_release(b);
    pt_locate(&c, 5);
    asserteq(pt_edit(&c, 0, " world", 6), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 11));
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 1);
        assertok(ptM_ishole(r, 0));
        {
            pt_Hole *h = (pt_Hole *)r->children[0];
            asserteq(r->bytes[0], 11);
            asserteq(memcmp(h->data, "hello world", 11), 0);
        }
    }
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_append_full) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    int       i;
    /* hole with 14 bytes (close to PT_MAX_HOLESIZE=16), append 5 -> overflow */
    {
        static char bigbuf[15];
        for (i = 0; i < 14; ++i) bigbuf[i] = 'a';
        bigbuf[14] = '\0';
        pt_seek(&c, b, 0);
        asserteq(pt_edit(&c, 0, bigbuf, 14), PT_OK);
    }
    pt_release(b);
    pt_locate(&c, 14);
    asserteq(pt_edit(&c, 0, "bbbbb", 5), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 19));
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 2);
        assertok(ptM_ishole(r, 0));
        assertok(ptM_ishole(r, 1));
        {
            pt_Hole *ha = (pt_Hole *)r->children[0];
            asserteq(r->bytes[0], 14);
            for (i = 0; i < 14; ++i) asserteq(ha->data[i], 'a');
        }
        {
            pt_Hole *hb = (pt_Hole *)r->children[1];
            asserteq(r->bytes[1], 5);
            asserteq(memcmp(hb->data, "bbbbb", 5), 0);
        }
    }
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_prev_hole) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_edit(&c, 0, "hello", 5); /* hole("hello") at pos 0 */
    pt_insert(&c, "XYZ", 3);    /* lit("XYZ") at pos 5 */
    pt_release(b);
    /* seek to 5: boundary after hole "hello", start of lit "XYZ" */
    pt_locate(&c, 5);
    asserteq(pt_edit(&c, 0, "abc", 3), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 8));
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 2);
        assertok(ptM_ishole(r, 0));
        {
            pt_Hole *h = (pt_Hole *)r->children[0];
            asserteq(r->bytes[0], 8);
            asserteq(memcmp(h->data, "helloabc", 8), 0);
        }
        assertok(!ptM_ishole(r, 1));
        asserteq(r->bytes[1], 3);
        asserteq(memcmp(r->children[1], "XYZ", 3), 0);
    }
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_mid_fit) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "hello world", 11), PT_OK);
    assertok(c.dirty);
    pt_release(b);
    pt_locate(&c, 5);
    asserteq(pt_edit(&c, 0, "XYZ", 3), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 8));
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 1);
        assertok(ptM_ishole(r, 0));
        {
            pt_Hole *h = (pt_Hole *)r->children[0];
            asserteq(r->bytes[0], 14);
            asserteq(memcmp(h->data, "helloXYZ world", 14), 0);
        }
    }
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_mid_split) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    int       i;
    /* hole with 12 bytes, insert 5 at middle -> 17 > CAP -> splitins */
    {
        static char splbuf[13];
        for (i = 0; i < 12; ++i) splbuf[i] = 'a';
        splbuf[12] = '\0';
        pt_seek(&c, b, 0);
        asserteq(pt_edit(&c, 0, splbuf, 12), PT_OK);
    }
    pt_release(b);
    pt_locate(&c, 6);
    asserteq(pt_edit(&c, 0, "bbbbb", 5), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 11));
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 3);
        assertok(ptM_ishole(r, 0));
        assertok(ptM_ishole(r, 1));
        assertok(ptM_ishole(r, 2));
        {
            pt_Hole *hl = (pt_Hole *)r->children[0];
            asserteq(r->bytes[0], 6);
            for (i = 0; i < 6; ++i) asserteq(hl->data[i], 'a');
        }
        {
            pt_Hole *hm = (pt_Hole *)r->children[1];
            asserteq(r->bytes[1], 5);
            asserteq(memcmp(hm->data, "bbbbb", 5), 0);
        }
        {
            pt_Hole *hr = (pt_Hole *)r->children[2];
            asserteq(r->bytes[2], 6);
            for (i = 0; i < 6; ++i) asserteq(hr->data[i], 'a');
        }
    }
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_del_then_ins) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello world", 11);
    pt_release(b);
    pt_locate(&c, 3);
    asserteq(pt_edit(&c, 2, "XYZ", 3), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 6));
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 3);
        assertok(!ptM_ishole(r, 0));
        asserteq(r->bytes[0], 3);
        asserteq(memcmp(r->children[0], "hel", 3), 0);
        assertok(ptM_ishole(r, 1));
        {
            pt_Hole *h = (pt_Hole *)r->children[1];
            asserteq(r->bytes[1], 3);
            asserteq(memcmp(h->data, "XYZ", 3), 0);
        }
        assertok(!ptM_ishole(r, 2));
        asserteq(r->bytes[2], 6);
        asserteq(memcmp(r->children[2], " world", 6), 0);
    }
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_del_only) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello world", 11);
    pt_release(b);
    pt_locate(&c, 3);
    asserteq(pt_edit(&c, 5, NULL, 0), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 3));
    pt_asserttree(c.tree, 0, leafV(litV("hel"), litV("rld")));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_type_sequence) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    int       k, n = 15;
    pt_seek(&c, b, 0);
    for (k = 0; k < n; ++k) {
        char ch = (char)('a' + (k % 26));
        asserteq(pt_edit(&c, 0, &ch, 1), PT_OK);
        assertok(pt_checktree(c.tree));
        assertok(pt_checkcursor(&c, (size_t)(k + 1)));
    }
    /* all 15 chars merged into a single hole via branch A */
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 1);
        assertok(ptM_ishole(r, 0));
        {
            asserteq(r->bytes[0], (size_t)n);
            {
                pt_Hole *h = (pt_Hole *)r->children[0];
                for (k = 0; k < n; ++k)
                    asserteq(h->data[k], (char)('a' + (k % 26)));
            }
        }
    }
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_split_tree) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2);
    pt_append(&c, "bb", 2);
    pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2);
    pt_release(b);
    pt_locate(&c, 2);
    asserteq(pt_edit(&c, 0, "ZZ", 2), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 4));
    /* expected: levels=1, inner(left: lit"aa", hole"ZZ", lit"bb",
       right: lit"cc", lit"dd") */
    {
        pt_Node  *exp_lf = leafV(litV("aa"), holeV("ZZ"), litV("bb"));
        pt_Node  *exp_rf = leafV(litV("cc"), litV("dd"));
        pt_Node  *exp_in = innerV(exp_lf, exp_rf);
        pt_Buffer expected = treeV(1, exp_in);
        assertok(pt_comparetree(c.tree, expected));
        pt_release(expected);
    }
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(edit_upmask) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b;
    pt_Cursor c;
    pt_Node  *r, *leaf0, *leaf1;
    pt_Hole  *hole;
    /* need each leaf to have >=2 children for pt_checktree */
    b = treeV(
            1, innerV(leafV(litV("aa"), litV("bb")),
                      leafV(litV("cc"), litV("dd"))));
    pt_seek(&c, b, 1);
    asserteq(pt_edit(&c, 0, "XYZ", 3), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 4));
    r = &c.tree->root;
    assertok(ptM_ishole(r, 0));
    assertok(!ptM_ishole(r, 1));
    leaf0 = r->children[0];
    asserteq(leaf0->child_count, 4);
    assertok(!ptM_ishole(leaf0, 0));
    asserteq(leaf0->bytes[0], 1);
    asserteq(memcmp(leaf0->children[0], "a", 1), 0);
    assertok(ptM_ishole(leaf0, 1));
    hole = (pt_Hole *)leaf0->children[1];
    asserteq(leaf0->bytes[1], 3);
    asserteq(memcmp(hole->data, "XYZ", 3), 0);
    assertok(!ptM_ishole(leaf0, 2));
    asserteq(leaf0->bytes[2], 1);
    asserteq(memcmp(leaf0->children[2], "a", 1), 0);
    assertok(!ptM_ishole(leaf0, 3));
    asserteq(leaf0->bytes[3], 2);
    asserteq(memcmp(leaf0->children[3], "bb", 2), 0);
    leaf1 = r->children[1];
    asserteq(leaf1->child_count, 2);
    assertok(!ptM_ishole(leaf1, 0));
    asserteq(leaf1->bytes[0], 2);
    asserteq(memcmp(leaf1->children[0], "cc", 2), 0);
    assertok(!ptM_ishole(leaf1, 1));
    asserteq(leaf1->bytes[1], 2);
    asserteq(memcmp(leaf1->children[1], "dd", 2), 0);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= pt_commit tests ================= */

TEST(commit_single_hole) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_Buffer snap;
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "hello", 5), PT_OK);
    assertok(c.dirty);
    snap = pt_commit(&c);
    assertok(snap != NULL);
    assertok(!c.dirty);
    asserteq(c.tree, NULL);
    assertok(pt_checktree(snap));
    {
        const pt_Node *r = &snap->root;
        asserteq(r->child_count, 1);
        assertok(!ptM_ishole(r, 0));
        asserteq(r->bytes[0], 5);
        asserteq(memcmp(r->children[0], "hello", 5), 0);
    }
    pt_release(snap), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* E11: consecutive holes -> adjacent literals frozen contiguously -> merged
 * into a single literal slot */
TEST(commit_merge) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer snap;
    pt_Cursor c;
    int       i;
    pt_seek(&c, b, 0);
    {
        char bigbuf[64];
        for (i = 0; i < 14; ++i) bigbuf[i] = 'a';
        bigbuf[14] = '\0';
        asserteq(pt_edit(&c, 0, bigbuf, 14), PT_OK);
    }
    asserteq(pt_edit(&c, 0, "!!", 2), PT_OK); /* fills to CAP=16 */
    asserteq(pt_edit(&c, 0, "XY", 2), PT_OK); /* over CAP -> 2nd hole */
    {
        pt_Node *r = &c.tree->root;
        asserteq(r->child_count, 2);
        assertok(ptM_ishole(r, 0) && ptM_ishole(r, 1));
    }
    snap = pt_commit(&c);
    assertok(snap != NULL);
    assertok(pt_checktree(snap));
    {
        const pt_Node *r = &snap->root;
        asserteq(r->child_count, 1);
        assertok(!ptM_ishole(r, 0));
        asserteq(r->bytes[0], PT_MAX_HOLESIZE + 2);
    }
    pt_release(snap), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(commit_mixed) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer snap;
    pt_Cursor c;
    int       i;
    pt_seek(&c, b, 0);
    pt_insert(&c, "abc", 3);  /* lit at pos 0 */
    pt_advance(&c, 3);        /* to pos 3 */
    pt_edit(&c, 0, "DEF", 3); /* hole at pos 3 */
    pt_advance(&c, 3);        /* to pos 6 */
    pt_insert(&c, "ghi", 3);  /* lit at pos 6 */
    pt_release(b);
    pt_locate(&c, 0);
    asserteq(pt_edit(&c, 0, "x", 1), PT_OK);
    snap = pt_commit(&c);
    assertok(snap != NULL);
    assertok(pt_checktree(snap));
    {
        const pt_Node *r = &snap->root;
        for (i = 0; i < (int)r->child_count; ++i) assertok(!ptM_ishole(r, i));
    }
    pt_release(snap);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(commit_deep) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer snap;
    pt_Cursor c;
    editV(&c, 0, 1,
          innerV(leafV(holeV("abc"), litV("x")),
                 leafV(litV("def"), litV("ghi"))));
    asserteq(pt_edit(&c, 0, "XYZ", 3), PT_OK);
    snap = pt_commit(&c);
    assertok(snap != NULL);
    assertok(pt_checktree(snap));
    {
        const pt_Node *r = &snap->root;
        int            i;
        for (i = 0; i < (int)r->child_count; ++i) assertok(!ptM_ishole(r, i));
    }
    pt_release(snap);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* commit exactly fills the arena block: the full block must retire to
 * the full list so pt_scratch stays valid afterwards */
TEST(commit_fillblock) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer snap;
    pt_Cursor c;
    size_t    cap;
    pt_seek(&c, b, 0);
    assertok(pt_reserve(&c, 0) != NULL); /* PT_ARENA_SIZE block */
    assertok(pt_scratch(&c, &cap) != NULL);
    assertok(pt_literal(&c, cap - 8) != NULL);      /* leave exactly 8 bytes */
    asserteq(pt_edit(&c, 0, "12345678", 8), PT_OK); /* 8-byte hole */
    snap = pt_commit(&c); /* freeze consumes the last 8 bytes */
    assertok(snap != NULL && !c.dirty);
    assertok(pt_checktree(snap));
    assertok(snap->arena.full != NULL);
    asserteq(snap->arena.current, NULL);
    pt_seek(&c, snap, 0);
    asserteq(pt_scratch(&c, &cap), NULL);
    asserteq(cap, 0);
    pt_release(snap), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* E7: freeze copies holes into scratch, handles page transition */
TEST(commit_freshpage) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    pt_Buffer b = pt_empty(S);
    pt_Buffer snap;
    /* Single edit -> 1 hole; commit copies data into scratch */
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "aaaaaaaaaaaaaaa", 15), PT_OK);
    snap = pt_commit(&c);
    assertok(snap != NULL && !c.dirty);
    assertok(pt_checktree(snap));
    {
        const pt_Node *r = &snap->root;
        asserteq(r->child_count, 1);
        assertok(!ptM_ishole(r, 0));
        asserteq(r->bytes[0], 15);
    }
    pt_release(snap), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(commit_clean) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer snap;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    snap = pt_commit(&c);
    asserteq(snap, b);
    pt_release(snap);
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(commit_then_reseek) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer snap;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "abcdef", 6), PT_OK);
    snap = pt_commit(&c);
    assertok(snap != NULL);
    pt_seek(&c, snap, 3);
    asserteq(pt_offset(&c), 3);
    assertok(pt_checkcursor(&c, 3));
    pt_release(snap), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* E11/E12: bytes/levels unchanged by freeze (single hole, no merge) */
TEST(commit_bytes_invariant) {
    pt_State      *S = pt_open(&test_alloc, NULL);
    pt_Buffer      b = pt_empty(S);
    pt_Buffer      snap;
    pt_Cursor      c;
    size_t         bytes_before;
    unsigned       levels_before;
    unsigned short cc_before;
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "abc", 3), PT_OK); /* hole("abc") at pos 0 */
    asserteq(pt_edit(&c, 0, "DE", 2), PT_OK);  /* appends hole("DE") */
    pt_release(b);
    pt_locate(&c, 0);
    asserteq(pt_edit(&c, 0, "XY", 2), PT_OK); /* front-insert into hole */
    bytes_before = pt_bytes(c.tree);
    levels_before = c.tree->levels;
    cc_before = c.tree->root.child_count;
    snap = pt_commit(&c);
    assertok(snap != NULL);
    asserteq(pt_bytes(snap), bytes_before);
    asserteq(snap->levels, levels_before);
    asserteq(snap->root.child_count, cc_before);
    pt_release(snap);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* multi-page reserve chain (levels=1, 3 leaves with holes); freeze merges
 * the whole run into one literal and collapses the tree */
TEST(commit_reserve_pages) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer snap;
    pt_Cursor c;
    int       i;
    editV(&c, 0, 1,
          innerV(leafV(holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa"),
                       holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa")),
                 leafV(holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa"),
                       holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa")),
                 leafV(holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa"),
                       holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa"))));
    asserteq(pt_edit(&c, 0, ".", 1), PT_OK);
    snap = pt_commit(&c);
    assertok(snap != NULL);
    assertok(pt_checktree(snap));
    asserteq(snap->levels, 0);
    asserteq(snap->root.child_count, 1);
    asserteq(pt_bytes(snap), 193);
    {
        char rd[200];
        pt_seek(&c, snap, 0);
        asserteq(pt_read(&c, rd, 193), 193);
        asserteq(rd[0], '.');
        for (i = 1; i < 193; ++i) asserteq(rd[i], 'a');
    }
    pt_release(snap);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* E12 all-or-nothing: reservescratch allocf fail -> NULL, tree untouched */
TEST(commit_reservebuf_oom) {
    int       cnt = 10000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    bytes_before;
    int       i;
    pt_seek(&c, b, 0);
    /* create a tree with holes via pt_edit */
    {
        char bigbuf[64];
        for (i = 0; i < 15; ++i) bigbuf[i] = 'a';
        bigbuf[15] = '\0';
        asserteq(pt_edit(&c, 0, bigbuf, 15), PT_OK);
    }
    bytes_before = pt_bytes(c.tree);
    assertok(c.dirty);
    asserteq(bytes_before, 15);
    cnt = 0; /* kill allocf -- next alloc (scratch page) fails */
    asserteq(pt_commit(&c), NULL);
    asserteq(c.dirty, 1);                     /* tree not frozen */
    asserteq(pt_bytes(c.tree), bytes_before); /* bytes unchanged */
    assertok(pt_checktree(c.tree));
    /* cleanup: transient still has holes -> release normally */
    pt_release(c.tree);
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* sec.8.3 full round-trip: edit series -> commit -> content matches reference
 */
TEST(edit_commit_roundtrip) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_from(S, "Hello World", 11);
    pt_Cursor c;
    pt_Buffer snap;
    pt_seek(&c, b, 5);
    asserteq(pt_edit(&c, 1, "XY", 2), PT_OK); /* replace " " with "XY" */
    snap = pt_commit(&c);
    assertok(snap != NULL);
    assertok(!c.dirty);
    assertok(pt_checktree(snap));
    asserteq(pt_bytes(snap), 12);
    {
        const pt_Node *r = &snap->root;
        asserteq(r->child_count, 3);
        assertok(!ptM_ishole(r, 0) && !ptM_ishole(r, 1) && !ptM_ishole(r, 2));
        asserteq(r->bytes[0], 5);
        asserteq(r->bytes[1], 2);
        asserteq(r->bytes[2], 5);
        asserteq(memcmp(r->children[0], "Hello", 5), 0);
        asserteq(memcmp(r->children[1], "XY", 2), 0);
        asserteq(memcmp(r->children[2], "World", 5), 0);
    }
    pt_release(snap), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* sec.8.3 commit then seek+edit (new transient), verify independent version */
TEST(edit_commit_edit) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_from(S, "Hello", 5);
    pt_Cursor c;
    pt_Buffer snap, snap2;
    pt_seek(&c, b, 2);
    asserteq(pt_edit(&c, 0, "XY", 2), PT_OK); /* "HeXYlo" */
    snap = pt_commit(&c);
    assertok(snap != NULL && !c.dirty);
    /* second edit on committed snapshot */
    pt_seek(&c, snap, 4);
    asserteq(pt_edit(&c, 0, "!", 1), PT_OK);
    assertok(c.dirty);
    asserteq(pt_bytes(c.tree), 8);
    snap2 = pt_commit(&c);
    assertok(snap2 != NULL);
    assertok(!c.dirty);
    assertok(pt_checktree(snap2));
    asserteq(pt_bytes(snap2), 8);
    {
        const pt_Node *r = &snap2->root;
        int            i;
        size_t         total = 0;
        for (i = 0; i < (int)r->child_count; ++i) {
            assertok(!ptM_ishole(r, i));
            total += r->bytes[i];
        }
        asserteq(total, 8);
    }
    /* first snapshot unchanged (bytes before 2nd edit) */
    asserteq(pt_bytes(snap), 7);
    assertok(pt_checktree(snap));
    /* cleanup: release snap2 (2nd committed) -> cascades to snap, then b */
    pt_release(snap2), pt_release(snap), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= deep commit with levels>=2 and holes ================= */

/* ================= deep commit with levels>=2 and holes ================= */

/* BUG BLOCKER: ptM_upmask in piecetab.h (L865-872) iterates root->leaf
   instead of leaf->root, so for levels>=2 trees the ROOT's mask is never
   updated when a hole is inserted in a leaf.  This means:
   - ptC_holebytes (L452 FALSE) and ptC_freeze (L541 FALSE) never descend
     into inner nodes because the ROOT has no mask bits -> UNREACHABLE.
   - pt_checktree fails on any levels>=2 tree with unfrozen holes.
   Fix: change loop direction in ptM_upmask to descend (l = levels-1 -> 0).
   Once fixed, this test should seek at multiple positions in a levels>=2
   tree, pt_edit to create holes, assert pt_checktree passes, then commit
   and verify all masks cleared. */

TEST(commit_deep2) {
    /* levels>=2 tree with a hole under an inner node: exercises the
       descend (FALSE) branch of ptC_holebytes/ptC_freeze, and verifies
       ptM_upmask propagates the hole bit to the root (leaf->root). */
    static char buf[200], exp0[128], exp1[128], got[128];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = pt_empty(S);
    pt_Buffer   a, a2;
    pt_Cursor   c;
    size_t      len0, len1, gl;
    int         k, n = 30;

    for (k = 0; k < 200; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, b, 0);
    /* stride-3 take-2 keeps pieces non-contiguous (no mergelit) -> deep tree */
    for (k = 0; k < n; ++k) asserteq(pt_append(&c, buf + k * 3, 2), PT_OK);
    assertok(c.tree->levels >= 2);
    a = pt_commit(&c);
    assertok(a != NULL && !c.dirty);
    pt_release(b);
    assertok(pt_checktree(a));
    len0 = collect_bytes(a, exp0, sizeof(exp0)); /* ground truth content */

    /* edit a levels>=2 committed tree: fork + insert a hole under an inner */
    pt_seek(&c, a, 7);
    asserteq(pt_edit(&c, 0, "ZZ", 2), PT_OK);
    assertok(c.tree->levels >= 2);
    assertok(pt_checktree(c.tree)); /* upmask fix: root mask sees the hole */
    memcpy(exp1, exp0, 7);
    exp1[7] = 'Z', exp1[8] = 'Z';
    memcpy(exp1 + 9, exp0 + 7, len0 - 7);
    len1 = len0 + 2;
    gl = collect_bytes(c.tree, got, sizeof(got));
    asserteq(gl, len1);
    asserteq(memcmp(got, exp1, len1), 0);

    a2 = pt_commit(&c); /* freeze: must descend into inner subtree */
    assertok(a2 != NULL && !c.dirty);
    asserteq(S->holes.live_obj, 0); /* every hole frozen -> descend worked */
    assertok(pt_checktree(a2));
    asserteq(pt_bytes(a2), len1);
    gl = collect_bytes(a2, got, sizeof(got));
    asserteq(gl, len1);
    asserteq(memcmp(got, exp1, len1), 0);

    pt_release(a);
    pt_release(a2);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= frozen-into-arena verification ================= */

TEST(commit_reserve_leftover) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer snap;
    pt_Cursor c;
    int       i;

    pt_seek(&c, b, 0);
    /* 31 edits of 16 bytes = 496 total hole bytes */
    for (i = 0; i < 31; ++i) {
        static char bigbuf[17];
        memset(bigbuf, (char)('A' + (i % 26)), 16);
        asserteq(pt_edit(&c, 0, bigbuf, 16), PT_OK);
    }
    assertok(c.dirty);
    /* Commit: freeze hole data into tree arena; the contiguous run
     * merges into a single literal and the tree collapses */
    snap = pt_commit(&c);
    assertok(snap != NULL && !c.dirty);
    assertok(pt_checktree(snap));
    asserteq(snap->levels, 0);
    asserteq(snap->root.child_count, 1);
    assertok(!ptM_ishole(&snap->root, 0));
    asserteq(pt_bytes(snap), 31 * 16);

    /* Verify data content via pt_read */
    {
        char   buf[512];
        size_t n;
        pt_seek(&c, snap, 0);
        n = pt_read(&c, buf, sizeof(buf));
        asserteq(n, 31 * 16);
        for (i = 0; i < 31; ++i) {
            int j;
            for (j = 0; j < 16; ++j)
                asserteq(buf[i * 16 + j], (char)('A' + (i % 26)));
        }
    }

    /* Arena block exists with used == total */
    {
        pt_Block *ab = snap->arena.current;
        assertok(ab != NULL);
        asserteq(ab->used, 31 * 16);
    }

    /* Release: arena freed, live_obj zero */
    pt_release(snap), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= reservescratch multi-page OOM rollback ================= */

TEST(commit_reservebuf_oom_multi) {
    int       cnt = 10000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    int       i;
    size_t    bytes_before;

    pt_seek(&c, b, 0);
    /* 32 edits of 16 bytes = 512 total -> pt_reserve allocs 1 block (1024) */
    for (i = 0; i < 32; ++i) {
        static char bigbuf[17];
        memset(bigbuf, 'x', 16);
        asserteq(pt_edit(&c, 0, bigbuf, 16), PT_OK);
    }
    bytes_before = pt_bytes(c.tree);
    assertok(c.dirty && bytes_before > 0);

    cnt = 0; /* pt_reserve ptA_alloc fails */
    asserteq(pt_commit(&c), NULL);
    /* Tree must be unchanged (E12 all-or-nothing) */
    asserteq(c.dirty, 1);
    asserteq(pt_bytes(c.tree), bytes_before);
    assertok(pt_checktree(c.tree));

    /* Cleanup: tree still has holes, release normally */
    cnt = 10000;
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* freeze fold pulls the right sibling (with unfrozen holes) into the
 * underfull leaf; second fixpoint round freezes them and merges the
 * arena seam into one literal */
TEST(commit_stitch_seam) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer snap;
    pt_Cursor c;
    editV(&c, 0, 1,
          innerV(leafV(holeV("ab"), holeV("cd")),
                 leafV(holeV("ef"), litV("XY"))));
    snap = pt_commit(&c);
    assertok(snap != NULL);
    asserteq(c.tree, NULL);
    assertok(!c.dirty);
    assertok(pt_checktree(snap));
    pt_asserttree(snap, 0, leafV(litV("abcdef"), litV("XY")));
    pt_release(snap);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* holes in the LAST leaf: after merge the underfull tail folds into its
 * left sibling (foldnode picks the left pair) and the root collapses */
TEST(commit_seam_left) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer snap;
    pt_Cursor c;
    editV(&c, 0, 1,
          innerV(leafV(litV("ab"), litV("cd")),
                 leafV(holeV("ef"), holeV("gh"))));
    snap = pt_commit(&c);
    assertok(snap != NULL);
    asserteq(c.tree, NULL);
    assertok(pt_checktree(snap));
    pt_asserttree(snap, 0, leafV(litV("ab"), litV("cd"), litV("efgh")));
    pt_release(snap);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* OOM inside ptC_freeze node reserve: commit returns NULL, tree stays
 * legal and dirty, cursor position survives, retry succeeds */
TEST(commit_freeze_oom) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Cursor c;
    pt_Drain  d;
    editV(&c, 0, 1,
          innerV(leafV(holeV("ab"), holeV("cd")),
                 leafV(holeV("ef"), litV("XY"))));
    pt_locate(&c, 3);
    d = pt_drainpool(&S->nodes);
    cnt = 1; /* arena block alloc ok; freezestep node reserve fails */
    asserteq(pt_commit(&c), NULL);
    assertok(c.dirty && c.tree != NULL);
    assertok(pt_checktree(c.tree));
    asserteq(pt_offset(&c), 3);
    assertok(pt_checkcursor(&c, 3));
    pt_refillpool(&S->nodes, d), cnt = 1000;
    {
        pt_Buffer snap = pt_commit(&c);
        assertok(snap != NULL);
        asserteq(c.tree, NULL);
        assertok(pt_checktree(snap));
        pt_asserttree(snap, 0, leafV(litV("abcdef"), litV("XY")));
        pt_release(snap);
    }
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= read interface tests ================= */

TEST(read_params) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_from(S, "hello", 5);
    pt_Cursor c;
    size_t    n;
    char      buf[8];

    /* pt_piece NULL checks */
    asserteq(pt_piece(NULL, &n), NULL);

    /* pt_next NULL checks */
    asserteq(pt_next(NULL, &n), NULL);

    /* pt_prev NULL checks */
    asserteq(pt_prev(NULL, &n), NULL);

    /* pt_read NULL checks */
    asserteq(pt_read(NULL, buf, 5), 0);
    pt_seek(&c, b, 0);
    asserteq(pt_read(&c, NULL, 5), 0);

    /* pt_next at end (poff==bytes[i]) */
    pt_seek(&c, b, 5);
    {
        const char *p = pt_next(&c, &n);
        asserteq(p, NULL);
        asserteq(n, 0);
    }

    /* pt_prev at start (off==0) */
    pt_seek(&c, b, 0);
    {
        const char *p = pt_prev(&c, &n);
        asserteq(p, NULL);
        asserteq(n, 0);
    }

    /* pt_prev with poff>0 (return current piece start) */
    pt_seek(&c, b, 3);
    {
        const char *p = pt_prev(&c, &n);
        assertok(p != NULL);
        asserteq(n, 3);
        asserteq(memcmp(p, "hel", 3), 0);
        asserteq(pt_offset(&c), 0);
    }

    /* pt_read with len=0 */
    pt_seek(&c, b, 0);
    asserteq(pt_read(&c, buf, 0), 0);

    /* pt_piece with poff past piece len */
    pt_seek(&c, b, 5);
    {
        const char *p = pt_piece(&c, &n);
        asserteq(p, NULL);
        asserteq(n, 0);
    }

    /* C->tree == NULL branches (zeroed cursor) */
    {
        pt_Cursor cz;
        size_t    zn;
        memset(&cz, 0, sizeof(cz));
        asserteq(pt_piece(&cz, &zn), NULL);
        asserteq(pt_next(&cz, &zn), NULL);
        asserteq(pt_prev(&cz, &zn), NULL);
        asserteq(pt_read(&cz, buf, 5), 0);
    }

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(piece_positions) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = treeV(0, leafV(litV("hello"), litV("world"), litV("!!!")));
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Start of first piece */
    pt_seek(&c, pt_nonnull(b), 0);
    p = pt_piece(&c, &n);
    assertok(p != NULL);
    asserteq(n, 5);
    asserteq(memcmp(p, "hello", 5), 0);

    /* Middle of first piece */
    pt_seek(&c, b, 2);
    p = pt_piece(&c, &n);
    assertok(p != NULL);
    asserteq(n, 3);
    asserteq(memcmp(p, "llo", 3), 0);

    /* Boundary between pieces */
    pt_seek(&c, b, 5);
    p = pt_piece(&c, &n);
    assertok(p != NULL);
    asserteq(n, 5);
    asserteq(memcmp(p, "world", 5), 0);

    /* End of last piece */
    pt_seek(&c, b, 10);
    p = pt_piece(&c, &n);
    assertok(p != NULL);
    asserteq(n, 3);
    asserteq(memcmp(p, "!!!", 3), 0);

    /* Past end */
    pt_seek(&c, b, 13);
    p = pt_piece(&c, &n);
    asserteq(p, NULL);
    asserteq(n, 0);

    /* pt_piece with NULL plen */
    pt_seek(&c, b, 0);
    p = pt_piece(&c, NULL);
    assertok(p != NULL);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(prefix_basic) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = treeV(0, leafV(litV("hello"), litV("world"), litV("!!!")));
    pt_Cursor   c;
    const char *p, *base;
    size_t      n, pre, full;

    /* Middle of a piece: prefix + remaining reconstruct the full piece. */
    pt_seek(&c, b, 2);
    p = pt_piece(&c, &n);
    assertok(p != NULL);
    pre = pt_prefix(&c);
    base = p - pre;
    full = n + pre;
    asserteq(pre, 2);
    asserteq(full, 5);
    asserteq(memcmp(base, "hello", 5), 0);
    asserteq(pt_offset(&c) - pre, 0);

    /* Piece start: prefix is zero and base equals pt_piece. */
    pt_seek(&c, b, 5);
    p = pt_piece(&c, &n);
    assertok(p != NULL);
    asserteq(pt_prefix(&c), 0);
    asserteq(p - pt_prefix(&c), p);

    /* Tree tail: pt_piece is empty, prefix still identifies the last piece. */
    pt_seek(&c, b, 13);
    p = pt_piece(&c, &n);
    asserteq(p, NULL);
    asserteq(n, 0);
    pre = pt_prefix(&c);
    asserteq(pre, 3);
    asserteq(pt_offset(&c) - pre, 10);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(next_basic) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = treeV(0, leafV(litV("aa"), litV("bb"), litV("cc")));
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Forward from start: piece then next */
    pt_seek(&c, b, 0);
    p = pt_piece(&c, &n);
    assertok(p != NULL);
    asserteq(n, 2);
    asserteq(memcmp(p, "aa", 2), 0);
    asserteq(pt_offset(&c), 0);

    p = pt_next(&c, &n);
    assertok(p != NULL);
    asserteq(n, 2);
    asserteq(memcmp(p, "bb", 2), 0);
    asserteq(pt_offset(&c), 2);

    p = pt_next(&c, &n);
    assertok(p != NULL);
    asserteq(n, 2);
    asserteq(memcmp(p, "cc", 2), 0);
    asserteq(pt_offset(&c), 4);

    /* No more pieces */
    p = pt_next(&c, &n);
    asserteq(p, NULL);
    asserteq(n, 0);
    asserteq(pt_offset(&c), 6);

    /* Forward from middle */
    pt_seek(&c, b, 2);
    p = pt_next(&c, &n);
    assertok(p != NULL);
    asserteq(n, 2);
    asserteq(memcmp(p, "cc", 2), 0);
    asserteq(pt_offset(&c), 4);

    p = pt_next(&c, &n);
    asserteq(p, NULL);
    asserteq(n, 0);
    asserteq(pt_offset(&c), 6);

    /* pt_next with NULL plen */
    pt_seek(&c, b, 0);
    p = pt_next(&c, NULL);
    assertok(p != NULL);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* next on an emptied tree must not move the cursor: pt_advance guards
 * the empty tree but pt_next read the stale bytes[0] and pushed poff
 * past it into a virtual end -- fuzz seed 3 op 169264 */
TEST(next_emptied_tree) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = pt_empty(S);
    pt_Cursor   c;
    const char *p;
    size_t      n;

    pt_seek(&c, b, 0);
    assertok(pt_append(&c, "abcdefgh", 8) == PT_OK);
    assertok(pt_locate(&c, 0) == PT_OK); /* back to the head */
    assertok(pt_remove(&c, 8) == PT_OK); /* tree empties */
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 0));
    p = pt_next(&c, &n);
    asserteq(p, NULL);
    assertok(pt_checkcursor(&c, 0)); /* cursor must stay put */

    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(prev_basic) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = treeV(0, leafV(litV("aa"), litV("bb"), litV("cc")));
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Backward from end */
    pt_seek(&c, b, 6);
    p = pt_prev(&c, &n);
    assertok(p != NULL);
    asserteq(n, 2);
    asserteq(memcmp(p, "cc", 2), 0);
    asserteq(pt_offset(&c), 4);

    p = pt_prev(&c, &n);
    assertok(p != NULL);
    asserteq(n, 2);
    asserteq(memcmp(p, "bb", 2), 0);
    asserteq(pt_offset(&c), 2);

    p = pt_prev(&c, &n);
    assertok(p != NULL);
    asserteq(n, 2);
    asserteq(memcmp(p, "aa", 2), 0);
    asserteq(pt_offset(&c), 0);

    /* No more previous */
    p = pt_prev(&c, &n);
    asserteq(p, NULL);
    asserteq(n, 0);

    /* pt_prev with NULL plen at piece boundary (poff==0, off>0)
     * to hit the L619 return with plen==NULL */
    pt_seek(&c, b, 4); /* start of "cc", poff==0 */
    p = pt_prev(&c, NULL);
    assertok(p != NULL);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* Single piece tree -- pt_next/pt_prev edges */
TEST(trav_single) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = treeV(0, leafV(litV("hello")));
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Forward: current piece via pt_piece, then next stops */
    pt_seek(&c, b, 0);
    p = pt_piece(&c, &n);
    assertok(p != NULL);
    asserteq(n, 5);
    asserteq(memcmp(p, "hello", 5), 0);

    p = pt_next(&c, &n);
    asserteq(p, NULL);
    asserteq(n, 0);
    asserteq(pt_offset(&c), 5);

    p = pt_next(&c, &n);
    asserteq(p, NULL);
    asserteq(n, 0);

    /* Backward from middle */
    pt_seek(&c, b, 3);
    p = pt_prev(&c, &n);
    assertok(p != NULL);
    asserteq(n, 3);
    asserteq(memcmp(p, "hel", 3), 0);
    asserteq(pt_offset(&c), 0);

    /* Backward from end */
    pt_seek(&c, b, 5);
    p = pt_prev(&c, &n);
    assertok(p != NULL);
    asserteq(n, 5);
    asserteq(memcmp(p, "hello", 5), 0);
    asserteq(pt_offset(&c), 0);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* Levels=1 forward traversal -- exercises walk-up-then-down in pt_next */
TEST(next_levels1) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = pt_empty(S);
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Build levels=1 tree via editing (triggers splitroot) */
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2);
    pt_append(&c, "bb", 2);
    pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2);
    pt_append(&c, "ee", 2);
    pt_release(b);
    asserteq(c.tree->levels, 1);

    /* Full forward */
    pt_seek(&c, c.tree, 0);
    {
        char   fwd[16];
        size_t off = 0;
        while ((p = pt_piece(&c, &n)) != NULL && n > 0) {
            memcpy(fwd + off, p, n), off += n;
            pt_next(&c, &n);
        }
        asserteq(off, 10);
        asserteq(memcmp(fwd, "aabbccddee", 10), 0);
    }

    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* Levels=1 backward traversal */
TEST(prev_levels1) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(
            1, innerV(leafV(litV("aa"), litV("bb")),
                      leafV(litV("cc"), litV("dd"))));
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Full backward */
    pt_seek(&c, b, 8);
    {
        char   rev[16];
        size_t end = 8;
        while ((p = pt_prev(&c, &n)) != NULL) end -= n, memcpy(rev + end, p, n);
        asserteq(end, 0);
        asserteq(memcmp(rev, "aabbccdd", 8), 0);
    }

    /* From "cc" (start of leaf 1), prev jumps to "bb" (leaf 0) */
    pt_seek(&c, b, 4);
    p = pt_prev(&c, &n);
    assertok(p != NULL);
    asserteq(n, 2);
    asserteq(memcmp(p, "bb", 2), 0);
    asserteq(pt_offset(&c), 2);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(read_basic) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("hello world")));
    pt_Cursor c;
    char      buf[32];
    size_t    r;

    /* Read partial from start */
    pt_seek(&c, b, 0);
    r = pt_read(&c, buf, 5);
    asserteq(r, 5);
    asserteq(memcmp(buf, "hello", 5), 0);
    asserteq(pt_offset(&c), 5);

    /* Read remaining */
    r = pt_read(&c, buf, 32);
    asserteq(r, 6);
    asserteq(memcmp(buf, " world", 6), 0);
    asserteq(pt_offset(&c), 11);

    /* Read past end */
    r = pt_read(&c, buf, 32);
    asserteq(r, 0);

    /* Read from middle */
    pt_seek(&c, b, 6);
    r = pt_read(&c, buf, 5);
    asserteq(r, 5);
    asserteq(memcmp(buf, "world", 5), 0);
    asserteq(pt_offset(&c), 11);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(read_cross) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("aaaa"), litV("bbbb"), litV("cccc")));
    pt_Cursor c;
    char      buf[32];
    size_t    r;

    /* Read across first two pieces exactly */
    pt_seek(&c, b, 2);
    r = pt_read(&c, buf, 6);
    asserteq(r, 6);
    asserteq(memcmp(buf, "aabbbb", 6), 0);
    asserteq(pt_offset(&c), 8);

    /* Read across multiple pieces to end */
    pt_seek(&c, b, 6);
    r = pt_read(&c, buf, 32);
    asserteq(r, 6);
    asserteq(memcmp(buf, "bbcccc", 6), 0);

    /* Read full tree */
    pt_seek(&c, b, 0);
    r = pt_read(&c, buf, 32);
    asserteq(r, 12);
    asserteq(memcmp(buf, "aaaabbbbcccc", 12), 0);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(read_full) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    static char ref[128];
    pt_Buffer   b;
    pt_Cursor   c;
    char        buf[256];
    size_t      r;
    int         k;

    for (k = 0; k < 128; ++k) ref[k] = (char)('A' + (k % 26));
    b = pt_from(S, ref, 128);
    pt_seek(&c, b, 0);
    r = pt_read(&c, buf, 256);
    asserteq(r, 128);
    asserteq(memcmp(buf, ref, 128), 0);
    asserteq(pt_offset(&c), 128);
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(read_empty) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    char      buf[8];
    size_t    r;

    pt_seek(&c, b, 0);
    r = pt_read(&c, buf, 5);
    asserteq(r, 0);

    /* pt_next/pt_prev on empty tree */
    {
        const char *p;
        size_t      n;
        p = pt_next(&c, &n);
        asserteq(p, NULL);
        asserteq(n, 0);
        p = pt_prev(&c, &n);
        asserteq(p, NULL);
        asserteq(n, 0);
    }

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(read_cursor_mid) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_from(S, "abcdefghij", 10);
    pt_Cursor c;
    char      buf[16];
    size_t    r;

    /* Read from middle of content (not piece boundary) */
    pt_seek(&c, pt_nonnull(b), 3);
    r = pt_read(&c, buf, 4);
    asserteq(r, 4);
    asserteq(memcmp(buf, "defg", 4), 0);
    asserteq(pt_offset(&c), 7);

    /* Read remaining after mid-read */
    r = pt_read(&c, buf, 10);
    asserteq(r, 3);
    asserteq(memcmp(buf, "hij", 3), 0);
    asserteq(pt_offset(&c), 10);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(trav_deep) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(
            2, innerV(innerV(leafV(litV("aa"), litV("bb")),
                             leafV(litV("cc"), litV("dd"))),
                      innerV(leafV(litV("ee"), litV("ff")),
                             leafV(litV("gg"), litV("hh")))));
    pt_Cursor   c;
    const char *p;
    size_t      n, off = 0, end = 16;
    char        fwd[16], rev[16];
    pt_seek(&c, b, 0);
    for (p = pt_piece(&c, &n); n > 0; p = pt_next(&c, &n))
        memcpy(fwd + off, p, n), off += n;
    asserteq(off, 16);
    asserteq(memcmp(fwd, "aabbccddeeffgghh", 16), 0);
    pt_seek(&c, b, pt_bytes(b));
    while ((p = pt_prev(&c, &n)) != NULL) end -= n, memcpy(rev + end, p, n);
    asserteq(end, 0);
    asserteq(memcmp(rev, "aabbccddeeffgghh", 16), 0);
    asserteq(pt_offset(&c), 0);
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= splice_brute: exhaustive enumeration ================= */

/* levels=3 tree, 288 bytes: root cc=3 over inner cc {4,3,2} ("4-3-2"),
 * each of the 9 inners holds 4 leaves of {lit,hole,lit,hole} 2B pieces */
static void maketree(pt_State *S, pt_Cursor *C, size_t off) {
    static const int rshape[3] = {4, 3, 2};
    pt_Node         *leaves[36], *inners[9], *root;
    int              i, j, idx = 0, ii = 0;

    for (i = 0; i < 36; i++) {
        pt_Node *lf = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Hole *h0 = (pt_Hole *)ptP_alloc(S, &S->holes);
        pt_Hole *h1 = (pt_Hole *)ptP_alloc(S, &S->holes);
        memset(lf, 0, sizeof(pt_Node));
        memset(h0, 0, sizeof(pt_Hole));
        memset(h1, 0, sizeof(pt_Hole));
        h0->data[0] = '#', h0->data[1] = '#';
        h1->data[0] = '#', h1->data[1] = '#';
        lf->child_count = 4;
        lf->children[0] = (pt_Node *)(pt_srcbuf + idx);
        lf->bytes[0] = 2;
        lf->children[1] = (pt_Node *)h0;
        lf->bytes[1] = 2;
        lf->mask |= (pt_Mask)1 << 1;
        lf->children[2] = (pt_Node *)(pt_srcbuf + idx + 2);
        lf->bytes[2] = 2;
        lf->children[3] = (pt_Node *)h1;
        lf->bytes[3] = 2;
        lf->mask |= (pt_Mask)1 << 3;
        idx += 4;
        leaves[i] = lf;
    }

    for (i = 0; i < 9; i++) {
        pt_Node *n = (pt_Node *)ptP_alloc(S, &S->nodes);
        memset(n, 0, sizeof(pt_Node));
        n->child_count = 4;
        for (j = 0; j < 4; j++) {
            n->children[j] = leaves[i * 4 + j];
            n->bytes[j] = ptN_sumbytes(leaves[i * 4 + j], 0, 4);
            if (leaves[i * 4 + j]->mask) n->mask |= (pt_Mask)1 << j;
        }
        inners[i] = n;
    }

    root = (pt_Node *)ptP_alloc(S, &S->nodes);
    memset(root, 0, sizeof(pt_Node));
    root->child_count = 3;
    for (i = 0; i < 3; i++) {
        pt_Node *n = (pt_Node *)ptP_alloc(S, &S->nodes);
        memset(n, 0, sizeof(pt_Node));
        n->child_count = (unsigned short)rshape[i];
        for (j = 0; j < rshape[i]; j++) {
            n->children[j] = inners[ii + j];
            n->bytes[j] = ptN_sumbytes(inners[ii + j], 0, 4);
            if (inners[ii + j]->mask) n->mask |= (pt_Mask)1 << j;
        }
        ii += rshape[i];
        root->children[i] = n;
        root->bytes[i] = ptN_sumbytes(n, 0, rshape[i]);
        if (n->mask) root->mask |= (pt_Mask)1 << i;
    }

    pt_seek(C, treeV(3, root), off);
    C->dirty = 1;
}

TEST(splice_brute) {
    int const nb = 288;
    pt_State *S = pt_open(&test_alloc, NULL);
    int       r, pos, del, ins;
    char      ref[288], expected[576], actual[576];
    pt_Cursor C;
    size_t    epos, edel, expect_len, cursor_exp, nread;
    makeref(ref);

    for (pos = 0; pos <= nb + 1; ++pos)
        for (del = 0; del <= nb + 1; ++del)
            for (ins = 0; ins <= 1; ++ins) {
                maketree(S, &C, (size_t)pos);
                epos = (size_t)pos < 288 ? (size_t)pos : 288;
                edel = (size_t)del < 288 - epos ? (size_t)del : 288 - epos;
                expect_len = 288 - edel + (ins ? 1u : 0u);
                cursor_exp = epos + (ins ? 1u : 0u);

                if (ins)
                    r = pt_splice(&C, (size_t)del, "!", 1);
                else
                    r = pt_splice(&C, (size_t)del, NULL, 0);
                asserteq(r, PT_OK);

                if (!pt_checktree(C.tree)) {
                    test_log("FAIL pos=%d del=%d ins=%d\n", pos, del, ins);
                    pt_dumptree(C.tree, "after splice");
                    pt_checktree(C.tree);
                    abort();
                }

                if (!pt_checkcursor(&C, cursor_exp)) {
                    test_log(
                            "splice pos=%d del=%d ins=%d off=%lu exp=%lu\n",
                            pos, del, ins, test_lu(pt_offset(&C)),
                            test_lu(cursor_exp));
                    abort();
                }

                if (ins) {
                    memcpy(expected, ref, epos);
                    expected[epos] = '!';
                    memcpy(expected + epos + 1, ref + epos + edel,
                           288 - epos - edel);
                } else {
                    memcpy(expected, ref, epos);
                    memcpy(expected + epos, ref + epos + edel,
                           288 - epos - edel);
                }

                pt_seek(&C, C.tree, 0);
                nread = pt_read(&C, actual, expect_len);
                if (nread != expect_len
                    || memcmp(actual, expected, expect_len) != 0) {
                    test_log(
                            "splice pos=%d del=%d ins=%d content fail"
                            " nread=%lu exp=%lu\n",
                            pos, del, ins, test_lu(nread), test_lu(expect_len));
                    abort();
                }

                pt_release(C.tree);
                asserteq(S->nodes.live_obj, 0);
                asserteq(S->holes.live_obj, 0);
            }

    pt_close(S);
}

/* ================================================================
 *  coverage gap fillers
 * ================================================================ */

/* deep tree with holes in multiple inner children;
 * exercises ptC_holebytes k>0 path and ptC_freeze inner-node traversal */
TEST(commit_deep_holes) {
    static char buf[300];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = pt_empty(S);
    pt_Cursor   c;
    pt_Buffer   snap;
    int         k, n = 60;
    for (k = 0; k < 300; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, b, 0);
    for (k = 0; k < n; ++k) pt_append(&c, buf + k * 3, 2);
    assertok(c.tree->levels >= 2);
    pt_locate(&c, 5);
    pt_edit(&c, 0, "AAAA", 4);
    pt_locate(&c, 55);
    pt_edit(&c, 0, "BBBB", 4);
    pt_locate(&c, 100);
    pt_edit(&c, 0, "CCCC", 4);
    assertok(c.dirty);
    snap = pt_commit(&c);
    assertok(snap != NULL && !c.dirty);
    pt_checktree(snap);
    {
        const pt_Node *r = &snap->root;
        for (k = 0; k < (int)r->child_count; ++k) assertok(!ptM_ishole(r, k));
    }
    pt_release(snap), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* read fewer bytes than a piece -- exercises pt_read m<n partial-read branch */
TEST(read_partial) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Buffer a;
    pt_Cursor c;
    char      rd[16];
    size_t    nr;
    pt_seek(&c, b, 0);
    pt_append(&c, "ABCD", 4);
    a = pt_commit(&c);
    pt_seek(&c, a, 1);
    nr = pt_read(&c, rd, 2);
    asserteq(nr, 2);
    asserteq(memcmp(rd, "BC", 2), 0);
    assertok(pt_checkcursor(&c, 3));
    {
        const char *p;
        size_t      n;
        p = pt_piece(&c, &n);
        asserteq(memcmp(p, "D", n), 0);
    }
    pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* pt_prev crossing a multi-level inner-node boundary:
 * cursor at first leaf of its parent triggers the while(--l>=0) ascent */
TEST(prev_cross_level) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b;
    pt_Buffer a;
    pt_Cursor c;
    char      buf[300];
    int       k;
    for (k = 0; k < 300; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, pt_empty(S), 0);
    for (k = 0; k < 40; ++k) pt_append(&c, buf + k * 3, 2);
    a = pt_commit(&c);
    assertok(a->levels >= 2);
    /* position at the first leaf of the second inner child of level 1
     * (inner1 children[0]), then prev crosses back into inner0 */
    {
        const pt_Node *r = &a->root;
        size_t         inner0_bytes = ptN_sumbytes(
                r->children[0], 0, r->children[0]->child_count);
        pt_seek(&c, a, inner0_bytes); /* start of first leaf in inner[1] */
        asserteq(pt_offset(&c), inner0_bytes);
        asserteq(c.poff, 0);
        {
            const char *p;
            size_t      n;
            p = pt_prev(&c, &n);
            assertok(p != NULL && n > 0);
            asserteq(pt_offset(&c), inner0_bytes - n);
        }
    }
    pt_release(a), pt_release(b = pt_empty(S));
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* deep remove that forces ptD_findroom -> ptD_makechain:
 * cutrange empties all right siblings so stitch must make a chain */
TEST(remove_findroom_deep) {
    pt_State *S = pt_open(&test_alloc, NULL);
    /* build a levels=3 tree with small pieces so remove can
     * create the findroom scenario */
    pt_Buffer b = treeV(
            3,
            innerV(innerV(
                    innerV(leafV(litV("a"), litV("b"), litV("c"), litV("d"))),
                    innerV(leafV(litV("e"), litV("f"), litV("g"), litV("h"))),
                    innerV(leafV(litV("i"), litV("j"), litV("k"), litV("l"))),
                    innerV(leafV(
                            litV("m"), litV("n"), litV("o"), litV("p"))))));
    pt_Cursor c;
    pt_seek(&c, b, 1);
    /* remove everything from position 1 to end, leaving "a" */
    asserteq(pt_remove(&c, 15), PT_OK);
    assertok(pt_checktree(c.tree));
    pt_asserttree(c.tree, 0, leafV(litV("a")));
    assertok(pt_checkcursor(&c, 1));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* edit a committed deep tree so the COW path inside ptI_splitchild
 * (nd = ptK_cow) is exercised on a node k levels deep */
TEST(edit_cow_splitchild) {
    static char buf[300];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = pt_empty(S);
    pt_Buffer   a;
    pt_Cursor   c;
    int         k;
    for (k = 0; k < 300; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, b, 0);
    for (k = 0; k < 60; ++k) pt_append(&c, buf + k * 3, 2);
    a = pt_commit(&c);
    assertok(a->levels >= 2);
    /* re-seek in the middle of a full leaf and insert to force splitchild */
    pt_seek(&c, a, 20);
    asserteq(pt_insert(&c, "ZZ", 2), PT_OK);
    pt_checktree(c.tree);
    pt_checktree(a); /* source still valid after COW */
    pt_release(c.tree), pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* error-return paths for public API null / invalid parameters */
TEST(error_paths) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    size_t    cap;

    pt_seek(&c, b, 0);

    /* pt_locate with NULL or bad tree */
    asserteq(pt_locate(NULL, 0), PT_ERRPARAM);
    c.tree = NULL;
    asserteq(pt_locate(&c, 0), PT_ERRPARAM);
    c.tree = (pt_Tree *)b;

    /* pt_reserve with NULL / dirty fail */
    asserteq(pt_reserve(NULL, 0), NULL);
    c.tree = NULL;
    asserteq(pt_reserve(&c, 0), NULL);
    c.tree = (pt_Tree *)b;

    /* pt_scratch with NULL args */
    asserteq(pt_scratch(NULL, &cap), NULL);
    cap = 99;
    asserteq(pt_scratch(&c, NULL), NULL);
    assertok(pt_scratch(&c, &cap) == NULL || cap == 0);

    /* pt_literal with NULL / len==0 */
    asserteq(pt_literal(NULL, 1), NULL);
    asserteq(pt_literal(&c, 0), NULL);

    /* pt_edit param checks */
    asserteq(pt_edit(NULL, 1, "x", 1), PT_ERRPARAM);
    c.tree = NULL;
    asserteq(pt_edit(&c, 1, "x", 1), PT_ERRPARAM);
    c.tree = (pt_Tree *)b;

    /* pt_edit len too large */
    asserteq(pt_edit(&c, 0, "x", PT_MAX_HOLESIZE + 1), PT_ERRPARAM);

    /* pt_append NULL */
    asserteq(pt_append(NULL, "x", 1), PT_ERRPARAM);
    asserteq(pt_append(&c, NULL, 1), PT_ERRPARAM);

    /* pt_remove / pt_splice NULL */
    asserteq(pt_remove(NULL, 1), PT_ERRPARAM);
    c.tree = NULL;
    asserteq(pt_remove(&c, 1), PT_ERRPARAM);
    c.tree = (pt_Tree *)b;
    asserteq(pt_splice(NULL, 1, "x", 1), PT_ERRPARAM);
    c.tree = NULL;
    asserteq(pt_splice(&c, 1, "x", 1), PT_ERRPARAM);
    c.tree = (pt_Tree *)b;

    /* pt_rollback: not dirty -> returns retained buffer, cursor detached */
    asserteq(pt_rollback(&c), b);
    asserteq(c.tree, NULL);
    pt_release(b); /* balance the rollback retain */

    /* pt_piece/pt_next/pt_prev NULL cursor */
    asserteq(pt_piece(NULL, NULL), NULL);
    asserteq(pt_next(NULL, NULL), NULL);
    asserteq(pt_prev(NULL, NULL), NULL);
    asserteq(pt_read(NULL, NULL, 0), 0);

    /* pt_empty NULL */
    asserteq(pt_empty(NULL), NULL);

    /* pt_advance backward overflow: d<0 and |d|>off clamps to start */
    pt_seek(&c, b, 0);
    pt_insert(&c, "abc", 3);
    asserteq(pt_advance(&c, -100), PT_OK); /* clamp to 0 */
    asserteq(pt_offset(&c), 0);

    /* pt_next at end: exhaust pieces then returns NULL */
    {
        const char *p;
        size_t      n;
        pt_locate(&c, 0);
        p = pt_piece(&c, &n); /* "abc" */
        assertok(p);
        asserteq(n, 3);
        p = pt_next(&c, &n); /* past end -> NULL */
        asserteq(p, NULL);
    }

    pt_seek(&c, b, 0);                          /* back to empty */
    asserteq(pt_append(&c, "hello", 5), PT_OK); /* piece for prev test */
    {
        pt_Buffer   a = pt_commit(&c); /* commit to test prev */
        const char *p;
        size_t      n;
        assertok(a != NULL);
        pt_seek(&c, a, 5);   /* end */
        p = pt_prev(&c, &n); /* "hello" */
        assertok(p);
        asserteq(memcmp(p, "hello", 5), 0);
        asserteq(n, 5);
        p = pt_prev(&c, &n); /* before start -> NULL */
        asserteq(p, NULL);
        asserteq(n, 0);
        /* plen=NULL variants for pt_piece/pt_next/pt_prev */
        pt_locate(&c, 0);
        pt_piece(&c, NULL); /* current piece, no len */
        pt_next(&c, NULL);  /* past end -> NULL */
        pt_advance(&c, -1);
        pt_prev(&c, NULL); /* back to "hell", no len */
        pt_release(a);
    }

    /* pt_from NULL S / bad s */
    asserteq(pt_from(NULL, NULL, 0), NULL);
    asserteq(pt_from(S, NULL, 1), NULL);

    /* pt_getallocf */
    asserteq(pt_getallocf(NULL, NULL), NULL);

    /* pt_reset / pt_close NULL (no crash) */
    pt_reset(NULL);
    pt_close(NULL);

    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* remove from a committed deep tree where L and R share root child;
 * exercises ptD_cowpaths inner loop (L1240) */
TEST(remove_cow_deep) {
    static char buf[300];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = pt_empty(S);
    pt_Buffer   a;
    pt_Cursor   c;
    int         k;
    for (k = 0; k < 300; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, b, 0);
    for (k = 0; k < 40; ++k) pt_append(&c, buf + k * 3, 2);
    a = pt_commit(&c);
    assertok(a->levels >= 2);
    /* remove range within first root child only */
    {
        const pt_Node *r = &a->root;
        size_t         child0_bytes = ptN_sumbytes(
                r->children[0], 0, r->children[0]->child_count);
        pt_seek(&c, a, 2);
        asserteq(pt_remove(&c, child0_bytes - 4), PT_OK);
        pt_checktree(c.tree);
        pt_checktree(a);
    }
    pt_release(c.tree), pt_release(a), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* remove mid-piece from a literal in a deeply full tree;
 * triggers ptD_rmleaf splitroot+splitchild path (L1221-1222) */
TEST(remove_literal_mid_fulltree) {
    pt_State *S = pt_open(&test_alloc, NULL);
    /* build tree where every node (root to leaf) is full (cc=PT_FANOUT) */
    pt_Buffer b = treeV(
            2,
            innerV(innerV(leafV(litV("aa"), litV("bb"), litV("cc"), litV("dd")),
                          leafV(litV("ee"), litV("ff"), litV("gg"), litV("hh")),
                          leafV(litV("ii"), litV("jj"), litV("kk"), litV("ll")),
                          leafV(litV("mm"), litV("nn"), litV("oo"),
                                litV("pp"))),
                   innerV(leafV(litV("qa"), litV("qb"), litV("qc"), litV("qd")),
                          leafV(litV("qe"), litV("qf"), litV("qg"), litV("qh")),
                          leafV(litV("qi"), litV("qj"), litV("qk"), litV("ql")),
                          leafV(litV("qm"), litV("qn"), litV("qo"),
                                litV("qp"))),
                   innerV(leafV(litV("ra"), litV("rb"), litV("rc"), litV("rd")),
                          leafV(litV("re"), litV("rf"), litV("rg"), litV("rh")),
                          leafV(litV("ri"), litV("rj"), litV("rk"), litV("rl")),
                          leafV(litV("rm"), litV("rn"), litV("ro"),
                                litV("rp"))),
                   innerV(leafV(litV("sa"), litV("sb"), litV("sc"), litV("sd")),
                          leafV(litV("se"), litV("sf"), litV("sg"), litV("sh")),
                          leafV(litV("si"), litV("sj"), litV("sk"), litV("sl")),
                          leafV(litV("sm"), litV("sn"), litV("so"),
                                litV("sp")))));
    pt_Cursor c;
    pt_seek(&c, b, 3);
    /* remove 1 byte from middle of piece "bb" (position 3 = "b" in "bb") */
    asserteq(pt_remove(&c, 1), PT_OK);
    assertok(pt_checktree(c.tree));
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* cov: ptD_rmleaf mid-literal split in a completely full tree */
TEST(remove_cov_midsplit) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b;
    pt_Cursor c;
    b = treeV(
            1,
            innerV(leafV(litV("aaa"), litV("bbb"), litV("ccc"), litV("ddd")),
                   leafV(litV("eee"), litV("fff"), litV("ggg"), litV("hhh")),
                   leafV(litV("iii"), litV("jjj"), litV("kkk"), litV("lll")),
                   leafV(litV("mmm"), litV("nnn"), litV("ooo"), litV("ppp"))));
    pt_seek(&c, b, 1);
    asserteq(pt_remove(&c, 1), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 1));
    asserteq(pt_bytes(c.tree), 47);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* stitch where right-side pieces were zeroed out: exercises
 * ptD_stitch zero-byte removal path (L1164) and the stitching
 * cursor adjustment (L1175) */
TEST(remove_stitch_zero) {
    pt_State *S = pt_open(&test_alloc, NULL);
    /* remove that creates empty tail pieces that get cleaned up */
    pt_Buffer b = treeV(
            1, innerV(leafV(litV("a"), litV("bb"), litV("ccc"), litV("dddd")),
                      leafV(litV("e"), litV("ff"), litV("ggg"), litV("hhhh"))));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    /* remove exactly the piece "ccc" leaving an empty slot */
    asserteq(pt_remove(&c, 3), PT_OK);
    pt_checktree(c.tree);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* foldnode balance: children moving out of N0_0 must not leave stale
 * mask bits beyond its new cc, or root bit 0 reads as a hole though
 * N0_0 keeps none (fuzz replay pt_holebit_repro.txt) */
TEST(remove_balance_stalemask) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 5, 1,
          innerV(leafV(litV("a"), litV("b"), holeV("c"), holeV("d")),
                 leafV(litV("e"), litV("f"))));
    asserteq(pt_remove(&c, 1), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 5));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* splice with del==0 and NULL s -- triggers early return branch */
TEST(splice_null_del0) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    asserteq(pt_splice(&c, 0, NULL, 0), PT_OK);
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* build a buffer via pt_reserve+pt_literal+pt_append from empty;
 * within one arena block (PT_ARENA_SIZE=1024) all pieces must
 * merge into a single-node tree because literals are contiguous */
TEST(literal_single_node) {
    static char data[1024];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b = pt_empty(S);
    pt_Cursor   c;
    int         k;
    size_t      chunk, total = 0;
    size_t      cap;
    const char *lit;
    char       *buf;

    for (k = 0; k < 1024; ++k) data[k] = (char)('!' + (char)(k % 90));

    pt_seek(&c, b, 0);

    {
        buf = pt_reserve(&c, 1024); /* one arena block */
        assertok(buf != NULL);
        pt_scratch(&c, &cap);
        assertok(cap >= 1024);
    }

    for (k = 0; k < 1024; k += 16) {
        chunk = (size_t)(k + 16 <= 1024 ? 16 : 1024 - k);

        pt_scratch(&c, &cap);
        assertok(cap >= chunk);

        buf = (char *)pt_scratch(&c, &cap);
        assertok(cap >= chunk);
        memcpy(buf, data + k, chunk);

        lit = pt_literal(&c, chunk);
        assertok(lit != NULL);
        asserteq(lit, buf);

        pt_append(&c, lit, chunk);
        total += chunk;
    }

    asserteq(c.tree->levels, 0);
    asserteq(c.tree->root.child_count, 1);
    asserteq(pt_bytes(c.tree), total);
    asserteq(pt_offset(&c), total);
    pt_checktree(c.tree);

    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* cov: parameter/NULL error branches */
TEST(error_cov_params) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b = treeV(0, leafV(litV("ab")));
    pt_Cursor c;
    char      buf[4];
    size_t    n;
    { /* pt_open allocf fails */
        int cnt0 = 0;
        asserteq(pt_open(&oom_alloc, &cnt0), NULL);
    }
    { /* pt_from trees pool page alloc fails */
        int       cnt1 = 1000;
        pt_State *S2 = pt_open(&oom_alloc, &cnt1);
        cnt1 = 0;
        asserteq(pt_from(S2, "x", 1), NULL);
        pt_close(S2);
    }
    asserteq(pt_from(NULL, "x", 1), NULL);
    asserteq(pt_from(S, NULL, 1), NULL);
    asserteq(pt_getallocf(NULL, NULL), NULL);
    memset(&c, 0, sizeof(c));
    asserteq(pt_advance(&c, 1), PT_ERRPARAM);
    asserteq(pt_locate(&c, 0), PT_ERRPARAM);
    asserteq(pt_commit(&c), NULL);
    asserteq(pt_rollback(&c), NULL);
    asserteq(pt_scratch(&c, &n), NULL);
    asserteq(pt_literal(&c, 1), NULL);
    asserteq(pt_reserve(&c, 1), NULL);
    asserteq(pt_append(&c, "x", 1), PT_ERRPARAM);
    asserteq(pt_remove(&c, 1), PT_ERRPARAM);
    asserteq(pt_splice(&c, 1, "x", 1), PT_ERRPARAM);
    asserteq(pt_edit(&c, 1, "x", 1), PT_ERRPARAM);
    asserteq(pt_piece(&c, &n), NULL);
    asserteq(pt_next(&c, &n), NULL);
    asserteq(pt_prev(&c, &n), NULL);
    asserteq(pt_read(&c, buf, 1), 0);
    pt_seek(&c, b, 2);
    asserteq(pt_piece(&c, NULL), NULL);
    asserteq(pt_next(&c, NULL), NULL);
    pt_locate(&c, 0);
    asserteq(pt_prev(&c, NULL), NULL);
    {
        pt_Cursor e;
        pt_seek(&e, pt_empty(S), 0);
        asserteq(pt_piece(&e, NULL), NULL);
        asserteq(pt_next(&e, NULL), NULL);
        asserteq(pt_prev(&e, NULL), NULL);
    }
    asserteq(pt_scratch(&c, NULL), NULL);
    asserteq(pt_literal(&c, 0), NULL);
    asserteq(pt_splice(&c, 0, "x", 0), PT_OK);
    asserteq(pt_splice(&c, 0, NULL, 5), PT_OK);
    asserteq(pt_splice(&c, 1, "x", 0), PT_OK);
    assertok(pt_checktree(c.tree));
    pt_release(pt_rollback(&c)); /* returns retained b */
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* cov: pt_reserve / pt_literal OOM branches */
TEST(reserve_cov_oom) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    cnt = 0;
    asserteq(pt_reserve(&c, 4), NULL);
    asserteq(pt_literal(&c, 1), NULL);
    assertok(!c.dirty);
    cnt = 1000;
    asserteq(pt_insert(&c, "x", 1), PT_OK);
    assertok(c.dirty);
    cnt = 0;
    asserteq(pt_reserve(&c, 4), NULL);
    cnt = 1000;
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* cov: node reserve OK but tree fork (markdirty) fails */
TEST(fork_cov_oom) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b;
    pt_Cursor c;
    pt_Drain  td, nd;
    b = pt_from(S, "abcd", 4);
    pt_seek(&c, b, 0);
    asserteq(pt_insert(&c, "x", 1), PT_OK);
    asserteq(pt_rollback(&c), b);
    pt_release(b); /* balance the rollback retain */
    td = pt_drainpool(&S->trees);
    cnt = 0;
    pt_seek(&c, b, 1);
    asserteq(pt_insert(&c, "y", 1), PT_ERRMEM);
    asserteq(pt_remove(&c, 2), PT_ERRMEM);
    asserteq(pt_splice(&c, 2, "z", 1), PT_ERRMEM);
    assertok(!c.dirty);
    nd = pt_drainpool(&S->nodes);
    asserteq(pt_splice(&c, 2, "z", 1), PT_ERRMEM);
    pt_refillpool(&S->nodes, nd);
    pt_refillpool(&S->trees, td);
    cnt = 1000;
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* cov: pt_edit hole reserve fails after beginedit succeeds */
TEST(edit_cov_holeoom) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b = pt_empty(S);
    pt_Cursor c;
    pt_Drain  d;
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "x", 1), PT_OK);
    d = pt_drainpool(&S->holes);
    cnt = 0;
    asserteq(pt_edit(&c, 0, "y", 1), PT_ERRMEM);
    pt_refillpool(&S->holes, d);
    cnt = 1000;
    asserteq(pt_edit(&c, 0, "y", 1), PT_OK);
    pt_release(c.tree), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* cov: pt_edit prev-hole exists but full (ptH_fit false).
 * Use imperative construction to avoid COW double-free on shared leaf
 * children that occurs when a levels=0 treeV tree is forked. */
TEST(edit_cov_prevfull) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b;
    pt_Cursor c;
    b = pt_empty(S);
    pt_seek(&c, b, 0);
    asserteq(pt_edit(&c, 0, "0123456789abcdef", 16), PT_OK);
    pt_append(&c, "XY", 2);
    pt_release(b); /* empty sentinel, no-op */
    pt_locate(&c, 16);
    asserteq(pt_edit(&c, 0, "zz", 2), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 18));
    pt_asserttree(
            c.tree, 0,
            leafV(holeV("0123456789abcdef"), holeV("zz"), litV("XY")));
    pt_release(c.tree);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* ================= compact ================= */

TEST(compact_params) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_State *S2 = pt_open(&test_alloc, NULL);
    pt_Buffer b = pt_from(S, "hello", 5);
    asserteq(pt_compact(NULL, b), NULL);
    asserteq(pt_compact(S, NULL), NULL);
    asserteq(pt_compact(S2, b), NULL); /* foreign state */
    asserteq(pt_compact(S, pt_empty(S)), pt_empty(S));
    {
        pt_Buffer z = pt_from(S, NULL, 0); /* zero-byte blob */
        asserteq(pt_compact(S, z), pt_empty(S));
        pt_release(z);
    }
    {
        pt_Buffer e = pt_empty(S);
        pt_Cursor c;
        pt_seek(&c, e, 0);
        asserteq(pt_edit(&c, 0, "x", 1), PT_OK);
        asserteq(pt_compact(S, c.tree), NULL); /* transient root has mask */
        pt_release(c.tree);
    }
    pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S2), pt_close(S);
}

TEST(compact_basic) {
    static const char big[] = "0123456789ABCDEF";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b0 = pt_from(S, big, 16), b1, nb;
    pt_Cursor         c;
    const char       *old;

    pt_seek(&c, b0, 8);
    asserteq(pt_edit(&c, 0, "xy", 2), PT_OK);
    b1 = pt_commit(&c);
    assertok(b1 != NULL);
    old = (const char *)b1->root.children[1]; /* internal "xy" */

    nb = pt_compact(S, b1);
    assertok(nb != NULL);
    asserteq(nb->from, &S->empty);
    assertok(pt_checktree(nb));
    pt_asserttree(nb, 0, leafV(litV("01234567"), litV("xy"), litV("89ABCDEF")));
    /* external pieces keep the original pointers (zero-copy) */
    asserteq((const char *)nb->root.children[0], big);
    asserteq((const char *)nb->root.children[2], big + 8);
    /* internal piece migrated into nb's own arena */
    assertok((const char *)nb->root.children[1] != old);
    assertok(nb->arena.current != NULL);

    /* old chain released: new blob must stay intact (ASAN guards) */
    pt_release(b1), pt_release(b0);
    {
        char buf[32];
        asserteq(collect_bytes(nb, buf, 32), 18);
        asserteq(memcmp(buf, "01234567xy89ABCDEF", 18), 0);
    }
    pt_release(nb);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(compact_merge) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b1, b2, nb;
    pt_Cursor c;

    pt_seek(&c, pt_empty(S), 0);
    asserteq(pt_edit(&c, 0, "ab", 2), PT_OK);
    b1 = pt_commit(&c);
    pt_seek(&c, b1, 2);
    asserteq(pt_edit(&c, 0, "cd", 2), PT_OK);
    b2 = pt_commit(&c);
    assertok(b1);
    assertok(b2);
    asserteq(b2->root.child_count, 2);

    /* both pieces internal: copied back-to-back, hence merged */
    nb = pt_compact(S, b2);
    assertok(nb != NULL);
    asserteq(nb->from, &S->empty);
    pt_asserttree(nb, 0, leafV(litV("abcd")));

    pt_release(b2), pt_release(b1);
    {
        char buf[8];
        asserteq(collect_bytes(nb, buf, 8), 4);
        asserteq(memcmp(buf, "abcd", 4), 0);
    }
    pt_release(nb);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* disjoint slices of one buffer: never adjacent, never merged */
static pt_Buffer compact_makewide(pt_State *S, const char *src, int np) {
    pt_Cursor c;
    int       i;
    pt_seek(&c, pt_empty(S), 0);
    for (i = 0; i < np; ++i) asserteq(pt_append(&c, src + 2 * i, 1), PT_OK);
    return pt_commit(&c);
}

TEST(compact_deep) {
    static const char src[] = "a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-p-";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = compact_makewide(S, src, 16), nb;

    nb = pt_compact(S, b); /* full leaves: root deepening, no fold */
    assertok(nb != NULL);
    asserteq(nb->from, &S->empty);
    assertok(pt_checktree(nb));
    asserteq(nb->levels, 1);
    asserteq(nb->root.child_count, 4);
    asserteq(nb->arena.current, NULL); /* all external: zero-copy */
    {
        char buf[20];
        asserteq(collect_bytes(nb, buf, 20), 16);
        asserteq(memcmp(buf, "abcdefghijklmnop", 16), 0);
    }
    pt_release(nb), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(compact_tail_balance) {
    static const char src[] = "a-b-c-d-e-f-";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Buffer         b = compact_makewide(S, src, 6), nb;

    nb = pt_compact(S, b); /* trailing leaf underfull: balanced [3,3] */
    assertok(nb != NULL);
    assertok(pt_checktree(nb));
    pt_asserttree(
            nb, 1,
            innerV(leafV(litV_(src, 1), litV_(src + 2, 1), litV_(src + 4, 1)),
                   leafV(litV_(src + 6, 1), litV_(src + 8, 1),
                         litV_(src + 10, 1))));
    pt_release(nb), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(compact_chain) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Buffer b1, b2, b3, nb;
    pt_Cursor c;

    /* three generations: three arenas, ranges array must grow (cap 2) */
    pt_seek(&c, pt_empty(S), 0);
    asserteq(pt_edit(&c, 0, "aa", 2), PT_OK);
    b1 = pt_commit(&c);
    pt_seek(&c, b1, 2);
    asserteq(pt_edit(&c, 0, "bb", 2), PT_OK);
    b2 = pt_commit(&c);
    pt_seek(&c, b2, 4);
    asserteq(pt_edit(&c, 0, "cc", 2), PT_OK);
    b3 = pt_commit(&c);
    assertok(b1 && b2 && b3);

    nb = pt_compact(S, b3); /* ancestor-arena literals are internal too */
    assertok(nb != NULL);
    asserteq(nb->from, &S->empty);
    pt_asserttree(nb, 0, leafV(litV("aabbcc")));

    pt_release(b3), pt_release(b2), pt_release(b1);
    {
        char buf[8];
        asserteq(collect_bytes(nb, buf, 8), 6);
        asserteq(memcmp(buf, "aabbcc", 6), 0);
    }
    pt_release(nb);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(compact_oom) {
    int       cnt = 1000, k;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer b1, b2, b3, nb;
    pt_Cursor c;
    size_t    pre;

    /* three generations so ranges collect must grow (cap 2) */
    pt_seek(&c, pt_empty(S), 0);
    asserteq(pt_edit(&c, 0, "aa", 2), PT_OK);
    b1 = pt_commit(&c);
    pt_seek(&c, b1, 2);
    asserteq(pt_edit(&c, 0, "bb", 2), PT_OK);
    b2 = pt_commit(&c);
    pt_seek(&c, b2, 4);
    asserteq(pt_edit(&c, 0, "cc", 2), PT_OK);
    b3 = pt_commit(&c);
    assertok(b1 && b2 && b3);

    pre = S->nodes.live_obj;
    for (k = 0;; ++k) {
        char buf[8];
        cnt = k;
        nb = pt_compact(S, b3);
        if (nb != NULL) break;
        assertok(k < 64);
        asserteq(S->nodes.live_obj, pre);       /* failure leaks nothing */
        asserteq(collect_bytes(b3, buf, 8), 6); /* source intact */
        asserteq(memcmp(buf, "aabbcc", 6), 0);
    }
    cnt = 1000;
    pt_asserttree(nb, 0, leafV(litV("aabbcc")));
    pt_release(nb), pt_release(b3), pt_release(b2), pt_release(b1);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(compact_oom_makechain) {
    static const char src[] = "a-b-c-d-e-f-g-h-i-j-k-l-m-n-o-p-";
    int               cnt = 1000;
    pt_State         *S = pt_open(&oom_alloc, &cnt);
    pt_Buffer         b = compact_makewide(S, src, 16), nb;
    pt_Drain          d;
    size_t            pre;

    assertok(b != NULL);
    pre = S->nodes.live_obj;
    d = pt_drainpool(&S->nodes); /* force makechain onto page alloc */
    cnt = 0;
    nb = pt_compact(S, b);
    asserteq(nb, NULL);
    pt_refillpool(&S->nodes, d), cnt = 1000;
    asserteq(S->nodes.live_obj, pre);

    nb = pt_compact(S, b); /* recovery */
    assertok(nb != NULL);
    assertok(pt_checktree(nb));
    {
        char buf[20];
        asserteq(collect_bytes(nb, buf, 20), 16);
        asserteq(memcmp(buf, "abcdefghijklmnop", 16), 0);
    }
    pt_release(nb), pt_release(b);
    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

/* edit whose deletion stops mid-literal (rmleaf splits the piece): the
 * cursor must not rest at a mid-tree piece end, and reads must run
 * through the delete point. See audit_pieceend_pt.md. */
TEST(edit_pieceend_escape) {
    static char f1[] = "abc", f2[] = "abc";
    static char deep[40][8];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Buffer   b, o;
    pt_Cursor   c, d;
    char        rd[17];
    size_t      n, k;
    pt_Node    *p;
    int         i;
    const char *pc;

    /* flat: two pieces in one leaf container (levels == 0) */
    b = pt_from(S, f1, 3);
    pt_seek(&c, b, 3);
    pt_append(&c, f2, 3);
    o = b;
    b = pt_commit(&c);
    pt_release(o);
    pt_seek(&c, b, 1);
    asserteq(pt_edit(&c, 1, "X", 1), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 2));
    p = ptK_parent(&c, ptK_levels(&c)), i = ptK_idx(&c, p, ptK_levels(&c));
    asserteq(c.poff < p->bytes[i], 1); /* never mid-tree at a piece end */
    n = 9;
    pc = pt_piece(&c, &n);
    assertok(pc != NULL);
    asserteq(n, 1);
    asserteq(memcmp(pc, "c", 1), 0);
    n = pt_read(&c, rd, 16);
    rd[n] = '\0';
    assertstreq(rd, "cabc");
    pt_seek(&d, c.tree, 0);
    n = pt_read(&d, rd, 16);
    rd[n] = '\0';
    assertstreq(rd, "aXcabc");
    pt_release(c.tree), pt_release(b);

    /* deep: 40 pieces spanning three inner levels (levels == 3) */
    for (k = 0; k < 40; ++k) memcpy(deep[k], "abc", 3);
    b = pt_from(S, deep[0], 3);
    pt_seek(&c, b, 3);
    for (k = 1; k < 40; ++k) pt_append(&c, deep[k], 3);
    o = b;
    b = pt_commit(&c);
    pt_release(o);
    pt_seek(&c, b, 1);
    asserteq(pt_edit(&c, 1, "X", 1), PT_OK);
    assertok(pt_checktree(c.tree));
    assertok(pt_checkcursor(&c, 2));
    p = ptK_parent(&c, ptK_levels(&c)), i = ptK_idx(&c, p, ptK_levels(&c));
    asserteq(c.poff < p->bytes[i], 1); /* never mid-tree at a piece end */
    n = 9;
    pc = pt_piece(&c, &n);
    assertok(pc != NULL);
    asserteq(n, 1);
    asserteq(memcmp(pc, "c", 1), 0);
    n = pt_read(&c, rd, 16);
    rd[n] = '\0';
    assertstreq(rd, "cabcabcabcabcabc");
    pt_release(c.tree), pt_release(b);

    asserteq(S->nodes.live_obj, 0);
    asserteq(S->holes.live_obj, 0);
    pt_close(S);
}

TEST(close_unreleased_arena) {
    Count     c = {0};
    pt_State *S = pt_open(&count_alloc, &c);
    pt_Buffer b;
    pt_Cursor cur;
    char     *p;

    b = pt_empty(S);
    pt_seek(&cur, b, 0);
    p = pt_reserve(&cur, 0);
    assertok(p != NULL);
    assertok(S->arenas != NULL);
    pt_close(S);
    asserteq(c.live, 0);
}

TEST(close_unreleased_commit) {
    Count     c = {0};
    pt_State *S = pt_open(&count_alloc, &c);
    pt_Buffer b, nb;
    pt_Cursor cur;

    b = pt_empty(S);
    pt_seek(&cur, b, 0);
    assertok(pt_edit(&cur, 0, "hello", 5) == PT_OK);
    nb = pt_commit(&cur);
    assertok(nb != NULL);
    assertok(S->arenas != NULL);
    pt_close(S);
    asserteq(c.live, 0);
}

TEST(reset_unreleased_arena) {
    Count     c = {0};
    pt_State *S = pt_open(&count_alloc, &c);
    pt_Buffer b;
    pt_Cursor cur;

    b = pt_empty(S);
    pt_seek(&cur, b, 0);
    assertok(pt_reserve(&cur, 0) != NULL);
    assertok(S->arenas != NULL);
    pt_reset(S);
    assertok(S->arenas == NULL);
    b = pt_empty(S);
    pt_seek(&cur, b, 0);
    assertok(pt_reserve(&cur, 0) != NULL);
    assertok(S->arenas != NULL);
    pt_close(S);
    asserteq(c.live, 0);
}

TEST(release_unlinks_arena_blocks) {
    Count     c = {0};
    pt_State *S = pt_open(&count_alloc, &c);
    pt_Buffer b, nb;
    pt_Cursor cur;

    b = pt_empty(S);
    pt_seek(&cur, b, 0);
    assertok(pt_edit(&cur, 0, "hello", 5) == PT_OK);
    nb = pt_commit(&cur);
    assertok(nb != NULL);
    assertok(S->arenas != NULL);
    pt_release(nb);
    assertok(S->arenas == NULL);
    pt_close(S);
    asserteq(c.live, 0);
}

#include "piecetab_test_fanout4.gen.inc"
