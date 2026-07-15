#define PT_FANOUT       4
#define PT_PAGE_SIZE    512
#define PT_MAX_HOLESIZE 16
#define PT_STATIC_API
#ifndef PT_POOL_STATS
# define PT_POOL_STATS
#endif

#include "pt_tests.h"

/* T1: lifecycle */

static void test_lifecycle(void) {
    pt_State *S;
    pt_Blob   b, b2;

    S = pt_open(NULL, NULL);
    assert(S != NULL);

    b = pt_empty(S);
    assert(b != NULL);
    assert(pt_version(b) == 0);
    assert(pt_bytes(b) == 0);

    pt_retain(b);
    pt_release(b);

    b2 = pt_empty(S);
    assert(b2 != NULL);
    pt_release(b);
    pt_release(b2);

    pt_close(S);
}

/* T2: seek on empty tree */

static void test_seek_empty(void) {
    pt_State *S = pt_open(NULL, NULL);
    pt_Cursor c;

    pt_seek(&c, pt_empty(S), 0);
    assert(pt_offset(&c) == 0);

    pt_seek(&c, pt_empty(S), 100);
    assert(pt_offset(&c) == 0); /* clamped */

    pt_close(S);
}

/* T3: insert and append */

static void test_insert_basic(void) {
    pt_State *S = pt_open(NULL, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    int       r;

    pt_seek(&c, b, 0);
    r = pt_insert(&c, "hello", 5);
    assert(r == PT_OK);
    assert(pt_offset(&c) == 0);
    assert(pt_bytes(c.tree) == 5);

    r = pt_insert(&c, " world", 6);
    assert(r == PT_OK);
    assert(pt_offset(&c) == 0); /* stay */
    assert(pt_bytes(c.tree) == 11);

    pt_append(&c, "!", 1);
    assert(pt_offset(&c) == 1); /* append advances cursor past inserted text */
    assert(pt_bytes(c.tree) == 12);

    pt_release(b);
    pt_close(S);
}

/* T3b: insert positions before / after / mid */

static void test_insert_before(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello", 5);               /* ["hello"], cursor stays pos 0 */
    assert(pt_insert(&c, "XX", 2) == PT_OK); /* poff==0 -> insert before */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 0));
    pt_asserttree(c.tree, 0, leafV(litV("XX"), litV("hello")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_insert_after(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello", 5);
    pt_advance(&c, 5);                       /* to end, poff==bytes[0] */
    assert(pt_insert(&c, "YY", 2) == PT_OK); /* insert after */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 5));
    pt_asserttree(c.tree, 0, leafV(litV("hello"), litV("YY")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_insert_mid(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "abcdef", 6);
    pt_advance(&c, 3); /* poff==3, middle of piece */
    assert(pt_insert(&c, "XYZ", 3) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 3)); /* cursor stays at insert point */
    pt_asserttree(c.tree, 0, leafV(litV("abc"), litV("XYZ"), litV("def")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3c: split root (fill a levels-0 tree, then insert -> levels 1) */

static void test_insert_split_root(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2);
    pt_append(&c, "bb", 2);
    pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2); /* root full: 4 pieces */
    pt_append(&c, "ee", 2); /* triggers splitroot */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 10));
    pt_asserttree(
            c.tree, 1,
            innerV(leafV(litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"), litV("ee"))));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3d: seek to end (locend) then append after last piece */

static void test_insert_locend(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Blob   a;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "foo", 3);
    pt_append(&c, "bar", 3);
    a = pt_commit(&c);             /* ["foo","bar"] committed */
    pt_seek(&c, a, 999);           /* clamp to end -> locend */
    assert(pt_checkcursor(&c, 6)); /* off=3, poff=3 (last piece) */
    assert(pt_append(&c, "baz", 3) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 9));
    pt_asserttree(c.tree, 0, leafV(litV("foo"), litV("bar"), litV("baz")));
    pt_release(c.tree), pt_release(a), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3e: split a full leaf node (levels 1, no root split) */

static void test_insert_split_leaf(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2);
    pt_append(&c, "bb", 2);
    pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2);
    pt_append(&c, "ee", 2); /* splitroot -> levels 1 */
    pt_append(&c, "ff", 2); /* fills nw leaf to 4 */
    pt_append(&c, "gg", 2); /* splits the full leaf node */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 14));
    pt_asserttree(
            c.tree, 1,
            innerV(leafV(litV("aa"), litV("bb")), leafV(litV("cc"), litV("dd")),
                   leafV(litV("ee"), litV("ff"), litV("gg"))));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3f: deep tree (levels>=2) — exercises internal splitnode, splitroot,
 * multi-level upbytes, and the 2*levels+3 reserve budget (audit B3). */

static void test_insert_deep(void) {
    static char buf[300];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Blob     b = pt_empty(S);
    pt_Cursor   c;
    int         k, n = 60;
    size_t      pos = 0;
    for (k = 0; k < (int)sizeof(buf); ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, b, 0);
    for (k = 0; k < n; ++k) {
        assert(pt_append(&c, buf + k * 3, 2) == PT_OK);
        pos += 2;
        assert(pt_checktree(c.tree));
        assert(pt_checkcursor(&c, pos));
        assert(pt_bytes(c.tree) == pos);
    }
    assert(c.tree->levels >= 2);
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3g: COW — editing a committed levels-1 tree copies the path, source stays */

static void test_insert_cow(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Blob   a;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2), pt_append(&c, "bb", 2), pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2), pt_append(&c, "ee", 2); /* levels 1 */
    a = pt_commit(&c);
    pt_seek(&c, a, 0);                       /* edit committed source */
    assert(pt_insert(&c, "ZZ", 2) == PT_OK); /* cowpath copies leaf pp */
    assert(pt_version(c.tree) != pt_version(a));
    assert(pt_checktree(c.tree) && pt_checktree(a));
    assert(pt_checkcursor(&c, 0));
    pt_asserttree(
            a, 1,
            innerV(leafV(litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"), litV("ee"))));
    pt_asserttree(
            c.tree, 1,
            innerV(leafV(litV("ZZ"), litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"), litV("ee"))));
    pt_release(c.tree), pt_release(a), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3h: two cursors fork from one blob into independent versions */

static void test_insert_multiversion(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Blob   a;
    pt_Cursor c1, c2;
    pt_seek(&c1, b, 0);
    pt_append(&c1, "aa", 2), pt_append(&c1, "bb", 2), pt_append(&c1, "cc", 2);
    pt_append(&c1, "dd", 2), pt_append(&c1, "ee", 2);
    a = pt_commit(&c1);
    pt_seek(&c1, a, 0);
    assert(pt_insert(&c1, "11", 2) == PT_OK); /* version v1 */
    pt_seek(&c2, a, 10);
    assert(pt_insert(&c2, "22", 2) == PT_OK); /* version v2, isolated */
    assert(pt_version(a) != pt_version(c1.tree));
    assert(pt_version(c1.tree) != pt_version(c2.tree));
    assert(pt_checktree(a) && pt_checktree(c1.tree) && pt_checktree(c2.tree));
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
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3i: OOM at the reserve / fork allocation points (audit §5 rollback) */

static void test_insert_oom_reserve(void) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    cnt = 0; /* next pool page alloc fails */
    assert(pt_insert(&c, "x", 1) == PT_ERRMEM);
    assert(!c.dirty && pt_bytes(c.tree) == 0); /* not forked, tree untouched */
    cnt = 1000; /* recover: cursor still usable */
    assert(pt_insert(&c, "ok", 2) == PT_OK);
    assert(pt_bytes(c.tree) == 2);
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_insert_oom_fork(void) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    cnt = 0; /* nodes reserve fails (pt_empty is sentinel, no freelist entry;
                reserve OOM achieves fork failure) */
    assert(pt_insert(&c, "x", 1) == PT_ERRMEM);
    assert(!c.dirty && pt_bytes(c.tree) == 0);
    cnt = 1000;
    assert(pt_insert(&c, "ok", 2) == PT_OK);
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3j: physical-contiguity merge of adjacent literals (same buffer slices) */

static void test_merge_right(void) {
    static const char buf[] = "abcdef";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Blob           b = pt_empty(S);
    pt_Cursor         c;
    pt_seek(&c, b, 0);
    pt_insert(&c, buf + 3, 3);                  /* ["def"] */
    assert(pt_insert(&c, buf + 0, 3) == PT_OK); /* "abc" before, contiguous */
    assert(pt_checktree(c.tree));
    pt_asserttree(c.tree, 0, leafV(litV("abc"), litV("def")));
    assert(pt_checkcursor(&c, 0));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_merge_left(void) {
    static const char buf[] = "abcdef";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Blob           b = pt_empty(S);
    pt_Cursor         c;
    pt_seek(&c, b, 0);
    pt_insert(&c, buf + 0, 3); /* ["abc"] */
    pt_advance(&c, 3);
    assert(pt_insert(&c, buf + 3, 3) == PT_OK); /* "def" after, contiguous */
    assert(pt_checktree(c.tree));
    pt_asserttree(c.tree, 0, leafV(litV("abcdef")));
    assert(pt_checkcursor(&c, 3)); /* cursor rides merged piece */
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3k: differential — incremental advance must match a fresh pt_seek */

static void test_advance_brute(void) {
    static char buf[300];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Blob     b = pt_empty(S);
    pt_Blob     a;
    pt_Cursor   c, ref;
    int         k, n = 40;
    size_t      from, to, total;
    for (k = 0; k < 300; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, b, 0);
    for (k = 0; k < n; ++k) pt_append(&c, buf + k * 3, 2);
    a = pt_commit(&c);
    total = pt_bytes(a);
    assert(a->levels >= 2);
    for (from = 0; from <= total; from += 3)
        for (to = 0; to <= total; to += 5) {
            pt_seek(&c, a, from);
            pt_advance(&c, (pt_Delta)to - (pt_Delta)from);
            pt_seek(&ref, a, to);
            assert(pt_offset(&c) == pt_offset(&ref));
            assert(pt_checkcursor(&c, to));
        }
    pt_release(a), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3l: defensive NULL-parameter paths + clean (non-dirty) commit */

static void test_null_params(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Blob   got;
    pt_Cursor c;
    assert(pt_version(NULL) == 0 && pt_bytes(NULL) == 0);
    pt_retain(NULL), pt_release(NULL), pt_rollback(NULL);
    assert(pt_commit(NULL) == NULL);
    assert(pt_seek(NULL, b, 0) == PT_ERRPARAM);
    assert(pt_seek(&c, NULL, 0) == PT_ERRPARAM);
    assert(pt_advance(NULL, 1) == PT_ERRPARAM);
    assert(pt_insert(NULL, "x", 1) == PT_ERRPARAM);
    assert(pt_append(NULL, "x", 1) == PT_ERRPARAM);
    pt_seek(&c, b, 0);
    assert(pt_advance(&c, 5) == PT_OK); /* delta on empty tree */
    assert(pt_insert(&c, NULL, 1) == PT_ERRPARAM);
    assert(pt_insert(&c, "x", 0) == PT_OK); /* len==0 early return */
    got = pt_commit(&c);                    /* not dirty -> retain+return */
    assert(got == b);
    pt_release(got); /* balance the retain */
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3m: split with cursor landing in the LEFT half (front insert) */

static void test_insert_split_front(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2), pt_append(&c, "bb", 2);
    pt_append(&c, "cc", 2), pt_append(&c, "dd", 2); /* full levels 0 */
    pt_advance(&c, -8);                             /* back to pos 0 */
    assert(pt_insert(&c, "ZZ", 2) == PT_OK);        /* splitroot, left half */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 0));
    pt_asserttree(
            c.tree, 1,
            innerV(leafV(litV("ZZ"), litV("aa"), litV("bb")),
                   leafV(litV("cc"), litV("dd"))));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3n: rollback discards the transient and restores the source */

static void test_rollback(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello", 5);
    assert(c.dirty && pt_bytes(c.tree) == 5);
    pt_rollback(&c);
    assert(!c.dirty && c.tree == b && pt_bytes(c.tree) == 0);
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3o: append onto a committed deep tree — cascade splits COW shared nodes
 * (splitnode's cownode copy branch, audit B3 interaction). */

static void test_insert_committed_split(void) {
    static char buf[500];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Blob     a, b, e = pt_empty(S);
    pt_Cursor   c;
    int         k;
    for (k = 0; k < 500; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, e, 0);
    for (k = 0; k < 62; ++k) pt_append(&c, buf + k * 3, 2);
    a = pt_commit(&c);
    assert(a->levels >= 2);
    pt_seek(&c, a, pt_bytes(a)); /* end of committed tree */
    for (k = 0; k < 40; ++k) {
        assert(pt_append(&c, buf + 200 + k * 2, 2) == PT_OK);
        assert(pt_checktree(c.tree));
    }
    b = c.tree;
    assert(pt_checktree(a)); /* source unchanged & valid */
    assert(pt_bytes(a) == 124);
    pt_release(e), pt_release(a), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3p: COW lifetime — release the SOURCE blob while a forked transient
 * still shares its nodes. The `from` field must keep the source alive until
 * the transient is released (exposes the pre-from use-after-free bug). */

static void test_release_order(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   a, b, e = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, e, 0);
    pt_append(&c, "aa", 2), pt_append(&c, "bb", 2), pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2), pt_append(&c, "ee", 2); /* levels 1 */
    a = pt_commit(&c);
    pt_seek(&c, a, 0);
    assert(pt_insert(&c, "ZZ", 2) == PT_OK); /* transient b shares a's nodes */
    b = c.tree;
    pt_release(a);           /* release source FIRST; from keeps it alive */
    assert(pt_checktree(b)); /* transient still valid (no use-after-free) */
    pt_release(b);           /* frees b, then chained a, then b-ref */
    pt_release(e);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* T3q: rollback when the source is kept alive only by the transient's `from`
 * (external released it after fork). rollback must release cleanly and leave
 * the cursor invalidated (b==NULL) — no dangling into the freed source. */

static void test_rollback_released_source(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Blob   a;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2), pt_append(&c, "bb", 2), pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2), pt_append(&c, "ee", 2);
    a = pt_commit(&c);
    pt_seek(&c, a, 0);
    assert(pt_insert(&c, "ZZ", 2) == PT_OK); /* fork; retain(a) via from */
    pt_release(a);          /* external drops a; only from holds it */
    pt_rollback(&c);        /* transient + a released; cursor invalidated */
    assert(c.tree == NULL); /* no dangling: cursor no longer borrows a */
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* release order with two committed blobs (from-chain COW) */

static void test_remove_release_order(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b0 = pt_from(S, "hello world foobar", 18);
    pt_Cursor c;
    pt_Blob   b1;
    pt_seek(&c, b0, 5);
    pt_insert(&c, "XYZ", 3);
    b1 = pt_commit(&c);
    assert(pt_checktree(b1));
    pt_release(b0);           /* release source first; b1 still shares nodes */
    assert(pt_checktree(b1)); /* b1 not dangling */
    pt_release(b1);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= remove: levels==0 literal deletions ================= */

static void test_remove_same_mid(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("abcdef")));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    assert(pt_remove(&c, 2) == PT_OK); /* delete "cd" -> split into two */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("ab"), litV("ef")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_same_prefix(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("abcdef")));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_remove(&c, 2) == PT_OK); /* delete prefix "ab" */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 0));
    pt_asserttree(c.tree, 0, leafV(litV("cdef")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_same_suffix(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("abcdef")));
    pt_Cursor c;
    pt_seek(&c, b, 4);
    assert(pt_remove(&c, 2) == PT_OK); /* delete suffix "ef" */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 4));
    pt_asserttree(c.tree, 0, leafV(litV("abcd")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_piece_whole(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("aa"), litV("bb"), litV("cc")));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    assert(pt_remove(&c, 2) == PT_OK); /* delete whole middle piece "bb" */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("aa"), litV("cc")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_cross(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("aaa"), litV("bbb"), litV("ccc")));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    assert(pt_remove(&c, 5) == PT_OK); /* [2,7): a|bbb|c across 3 pieces */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("aa"), litV("cc")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_to_end(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("aa"), litV("bb"), litV("cc")));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    assert(pt_remove(&c, 4) == PT_OK); /* delete "bb"+"cc" to end */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("aa")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_all(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("aa"), litV("bb")));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_remove(&c, 4) == PT_OK); /* delete everything */
    assert(pt_checktree(c.tree));
    assert(pt_bytes(c.tree) == 0);
    assert(pt_checkcursor(&c, 0));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_across_leaves(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(
            1, innerV(leafV(litV("aaa"), litV("bbb")),
                      leafV(litV("ccc"), litV("ddd"))));
    pt_Cursor c;
    pt_seek(&c, b, 2);
    assert(pt_remove(&c, 7) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 2));
    pt_asserttree(c.tree, 0, leafV(litV("aa"), litV("ddd")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_deep_shrink(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(
            2, innerV(innerV(leafV(litV("aa")), leafV(litV("bb"))),
                      innerV(leafV(litV("cc")))));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_remove(&c, 4) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 0));
    pt_asserttree(c.tree, 0, leafV(litV("cc")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* §8.1 edit_cow: from committed blob, fork preserves source, hole in transient
 */

static void test_edit_cow(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   a, b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "hello world", 11) == PT_OK);
    a = pt_commit(&c);
    assert(a != NULL && !c.dirty);
    /* fork: edit on committed blob */
    pt_seek(&c, a, 5);
    assert(pt_edit(&c, 0, "XYZ", 3) == PT_OK);
    assert(c.dirty);
    assert(pt_version(c.tree) != pt_version(a));
    assert(pt_checktree(c.tree) && pt_checktree(a));
    /* source unchanged */
    pt_asserttree(a, 0, leafV(litV("hello world")));
    /* transient has hole at position 5 */
    assert(pt_bytes(c.tree) == 14);
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 3);
        assert(!ptM_ishole(r, 0));
        assert(r->bytes[0] == 5 && memcmp(r->children[0], "hello", 5) == 0);
        assert(ptM_ishole(r, 1));
        {
            pt_Hole *h = (pt_Hole *)r->children[1];
            assert(r->bytes[1] == 3 && memcmp(h->data, "XYZ", 3) == 0);
        }
        assert(!ptM_ishole(r, 2));
        assert(r->bytes[2] == 6 && memcmp(r->children[2], " world", 6) == 0);
    }
    /* source has no holes (committed) */
    {
        unsigned i;
        for (i = 0; i < a->root.child_count; ++i)
            assert(!ptM_ishole(&a->root, i));
    }
    /* independent versions: edit transient, verify source */
    pt_release(c.tree);
    assert(pt_checktree(a) && pt_bytes(a) == 11);
    pt_release(a), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* §8.1 edit_rollback: rollback discards holes, cursor returns to source */

static void test_edit_rollback(void) {
    pt_State *S = pt_open(&test_alloc, NULL);

    /* Path 1: source blob held externally → rollback lands on source */
    {
        pt_Blob   b = pt_empty(S);
        pt_Cursor c;
        pt_seek(&c, b, 0);
        assert(pt_edit(&c, 0, "hello", 5) == PT_OK);
        assert(c.dirty && pt_bytes(c.tree) == 5);
        pt_rollback(&c);
        assert(!c.dirty && c.tree == b && pt_bytes(c.tree) == 0);
        pt_release(b);
        assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    }

    /* Path 2: source only via from → rollback returns to sentinel */
    {
        pt_Blob   b = pt_empty(S);
        pt_Cursor c;
        pt_seek(&c, b, 0);
        assert(pt_edit(&c, 0, "hello", 5) == PT_OK);
        pt_release(b); /* external drops source; sentinel stays alive */
        pt_rollback(&c);
        assert(c.tree == b); /* sentinel, not NULL */
        assert(!c.dirty && pt_bytes(c.tree) == 0);
        assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    }

    pt_close(S);
}

/* §8.1 edit_oom: reserve failure leaves tree untouched */

static void test_edit_oom(void) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    int       r;

    pt_seek(&c, b, 0);
    cnt = 0; /* next allocf fails (nodes reserve) */
    r = pt_edit(&c, 0, "x", 1);
    assert(r == PT_ERRMEM);
    assert(!c.dirty && pt_bytes(c.tree) == 0); /* tree untouched */

    cnt = 1000; /* recover */
    assert(pt_edit(&c, 0, "ok", 2) == PT_OK);
    assert(pt_bytes(c.tree) == 2);

    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= byte collection helper ================= */

static size_t collect_bytes_r(
        pt_Blob b, const pt_Node *n, unsigned l, char *buf) {
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

static size_t collect_bytes(pt_Blob b, char *buf, size_t cap) {
    (void)cap;
    if (b->root.child_count == 0) return 0;
    return collect_bytes_r(b, &b->root, 0, buf);
}

/* §8.1 edit_brute: position-independent content verification */

static void maketree(pt_State *S, pt_Cursor *C, size_t off);

static void test_edit_brute(void) {
    static const char alphabet
            [] = "abcdefghijklmnopqrstuvwxyz0123456789"
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                 "abcdefghijklmnopqrstuvwxyz0123456789"
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcd";
    int const nb = 256;
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor C;
    char      ref[256], expected[512], actual[512];
    int       i, pos;
    size_t    nread;
    for (i = 0; i < 64; i++) {
        ref[i * 4 + 0] = alphabet[i * 2 + 0];
        ref[i * 4 + 1] = alphabet[i * 2 + 1];
        ref[i * 4 + 2] = '#';
        ref[i * 4 + 3] = '#';
    }
    for (pos = 0; pos <= nb; ++pos) {
        maketree(S, &C, (size_t)pos);
        assert(pt_edit(&C, 0, "##", 2) == PT_OK);
        if (!pt_checktree(C.tree)) {
            pt_log("edit_brute FAIL pos=%d\n", pos);
            pt_dumptree(C.tree, "after edit");
            pt_checktree(C.tree), assert(0);
        }
        if (!pt_checkcursor(&C, (size_t)pos + 2)) {
            pt_log("edit_brute cursor pos=%d off=%zu\n", pos, pt_offset(&C));
            assert(0);
        }
        memcpy(expected, ref, (size_t)pos);
        memcpy(expected + pos, "##", 2);
        memcpy(expected + pos + 2, ref + pos, 256 - (size_t)pos);
        pt_seek(&C, C.tree, 0);
        nread = pt_read(&C, actual, 258);
        if (nread != 258 || memcmp(actual, expected, 258) != 0) {
            pt_log("edit_brute content fail pos=%d nread=%zu\n", pos, nread);
            assert(0);
        }
        pt_release(C.tree);
        assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    }
    pt_close(S);
}

/* §8.2 insert_brute: position-independent literal insert */

static void test_insert_brute(void) {
    static const char alphabet
            [] = "abcdefghijklmnopqrstuvwxyz0123456789"
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                 "abcdefghijklmnopqrstuvwxyz0123456789"
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcd";
    int const nb = 256;
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor C;
    char      ref[256], expected[512], actual[512];
    int       i, pos;
    size_t    nread;
    for (i = 0; i < 64; i++) {
        ref[i * 4 + 0] = alphabet[i * 2 + 0];
        ref[i * 4 + 1] = alphabet[i * 2 + 1];
        ref[i * 4 + 2] = '#';
        ref[i * 4 + 3] = '#';
    }
    for (pos = 0; pos <= nb; ++pos) {
        maketree(S, &C, (size_t)pos);
        assert(pt_insert(&C, "##", 2) == PT_OK);
        if (!pt_checktree(C.tree)) {
            pt_log("insert_brute FAIL pos=%d\n", pos);
            pt_dumptree(C.tree, "after insert");
            pt_checktree(C.tree), assert(0);
        }
        if (!pt_checkcursor(&C, (size_t)pos)) {
            pt_log("insert_brute cursor pos=%d off=%zu\n", pos, pt_offset(&C));
            assert(0);
        }
        memcpy(expected, ref, (size_t)pos);
        memcpy(expected + pos, "##", 2);
        memcpy(expected + pos + 2, ref + pos, 256 - (size_t)pos);
        pt_seek(&C, C.tree, 0);
        nread = pt_read(&C, actual, 258);
        if (nread != 258 || memcmp(actual, expected, 258) != 0) {
            pt_log("insert_brute content fail pos=%d nread=%zu\n", pos, nread);
            assert(0);
        }
        pt_release(C.tree);
        assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    }
    pt_close(S);
}

/* ================= pt_scratch tests ================= */

static void test_peekscratch_roundtrip(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    char     *h;
    size_t    cap, used;

    pt_seek(&c, b, 0);

    /* Arena not built yet → scratch returns NULL */
    cap = 123;
    assert(pt_scratch(&c, &cap) == NULL);
    assert(cap == 0);

    /* Build arena via pt_reserve */
    h = pt_reserve(&c, 0);
    assert(h != NULL);

    /* First scratch: head at h, cap == PT_ARENA_SIZE */
    cap = 0;
    h = pt_scratch(&c, &cap);
    assert(h != NULL);
    assert(cap == PT_ARENA_SIZE);

    /* Re-scratch: same position, unchanged */
    {
        char  *h2;
        size_t cap2 = 0;
        h2 = pt_scratch(&c, &cap2);
        assert(h2 == h);
        assert(cap2 == cap);
    }

    /* Write 10 bytes to scratch area */
    used = 10;
    memcpy(h, "HelloWorld", 10);

    /* Scratch again: still same position */
    {
        char  *h3;
        size_t cap3 = 0;
        h3 = pt_scratch(&c, &cap3);
        assert(h3 == h);
        assert(cap3 == cap);
    }

    /* pt_literal: commit 10 bytes, return old head */
    {
        const char *h4 = (pt_reserve(&c, used), pt_literal(&c, used));
        assert(h4 == h);
        assert(used == 10);
    }

    /* Scratch after literal: position advanced by 10 */
    {
        char  *h5;
        size_t cap5 = 0;
        h5 = pt_scratch(&c, &cap5);
        assert(h5 == h + 10);
        assert(cap5 == cap - 10);
    }

    /* Insert into tree, verify content */
    {
        char   buf[16];
        size_t blen;
        assert(pt_insert(&c, h, 10) == PT_OK);
        assert(pt_checktree(c.tree));
        assert(pt_bytes(c.tree) == 10);
        blen = collect_bytes(c.tree, buf, sizeof(buf));
        assert(blen == 10);
        assert(memcmp(buf, "HelloWorld", 10) == 0);
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_peekscratch_params(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    size_t    cap;

    pt_seek(&c, b, 0); /* init cursor */

    /* NULL cursor */
    cap = 123;
    assert(pt_scratch(NULL, &cap) == NULL);
    assert(cap == 123);

    /* NULL plen */
    assert(pt_scratch(&c, NULL) == NULL);

    /* Arena not built: scratch returns NULL, *plen set to 0 */
    cap = 456;
    assert(pt_scratch(&c, &cap) == NULL);
    assert(cap == 0);

    pt_release(b);
    pt_close(S);
}

static void test_peekscratch_adjacent(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    char     *h1, *h2;
    size_t    cap, used;

    pt_seek(&c, b, 0);

    /* Build arena and get first scratch */
    pt_reserve(&c, 0);
    cap = 0;
    h1 = pt_scratch(&c, &cap);
    assert(h1 != NULL);
    assert(cap == PT_ARENA_SIZE);

    /* Write 8 bytes, then literal to commit */
    used = 8;
    memcpy(h1, "HelloABC", 8);
    pt_literal(&c, used);

    /* Next scratch: adjacent at h1+8 */
    cap = 0;
    h2 = pt_scratch(&c, &cap);
    assert(h2 == h1 + 8);

    /* Write 3 bytes, literal to commit */
    used = 3;
    memcpy(h2, "XYZ", 3);
    pt_literal(&c, used);

    /* Insert both into tree, verify content */
    {
        char   buf[16];
        size_t blen;
        assert(pt_insert(&c, h1, 8) == PT_OK);
        pt_advance(&c, 8);
        assert(pt_insert(&c, h2, 3) == PT_OK);
        assert(pt_bytes(c.tree) == 11);
        blen = collect_bytes(c.tree, buf, sizeof(buf));
        assert(blen == 11);
        assert(memcmp(buf, "HelloABCXYZ", 11) == 0);
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= arena tests ================= */

static void test_arena_lazy(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;

    pt_seek(&c, b, 0);

    /* Lazy: arena not built before any edit/reserve */
    assert(c.tree->arena.current == NULL);
    assert(c.tree->arena.full == NULL);

    /* pt_scratch before any reserve/literal returns NULL, *plen=0 */
    {
        size_t cap = 123;
        assert(pt_scratch(&c, &cap) == NULL);
        assert(cap == 0);
    }

    /* pt_reserve builds the arena */
    assert(pt_reserve(&c, 0) != NULL);
    assert(c.tree != b);
    assert(c.tree->arena.current != NULL);

    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_arena_reserve_full(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;

    pt_seek(&c, b, 0);

    /* First reserve: block allocated with PT_ARENA_SIZE */
    assert(pt_reserve(&c, 0) != NULL);
    assert(c.tree->arena.current != NULL);

    /* Fill the block: literal entire capacity → moves block to full chain */
    {
        size_t n = PT_ARENA_SIZE;
        assert(pt_literal(&c, n) != NULL);
        assert(n == PT_ARENA_SIZE);
    }
    assert(c.tree->arena.current == NULL); /* head exhausted */
    assert(c.tree->arena.full != NULL);    /* moved to full */

    /* Next reserve allocates new block */
    assert(pt_reserve(&c, 0) != NULL);
    assert(c.tree->arena.current != NULL);

    /* Custom-sized reserve: block >= max(len, PT_ARENA_SIZE) */
    {
        char *p = pt_reserve(&c, 256);
        assert(p != NULL);
    }

    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_arena_dirty_break(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    size_t    cap;

    pt_seek(&c, b, 0);
    assert(!c.dirty);

    /* pt_scratch does NOT trigger dirty (read-only) */
    assert(pt_scratch(&c, &cap) == NULL);
    assert(!c.dirty);
    assert(c.tree == b); /* no fork */

    /* pt_reserve triggers dirty + builds arena */
    assert(pt_reserve(&c, 0) != NULL);
    assert(c.dirty);
    assert(c.tree != b);

    /* Fork a second cursor from original blob: independent arena */
    {
        pt_Cursor c2;
        pt_seek(&c2, b, 0);
        assert(pt_reserve(&c2, 0) != NULL);
        assert(c2.dirty);
        assert(c2.tree != c.tree);
        assert(c2.tree->arena.current != c.tree->arena.current);
        pt_release(c2.tree);
    }

    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_arena_reserve_reuse(void) {
    /* Exercise pt_reserve for-loop that traverses current chain and
       unlinks a non-head block with enough space (lines 742-750). */
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    size_t    n;
    char     *p;

    pt_seek(&c, b, 0);

    /* Build block A (head), consume a little */
    assert(pt_reserve(&c, 100) != NULL);
    n = 80;
    assert(pt_literal(&c, n) != NULL);
    assert(n == 80);

    /* Block A (size=1024, used=80, rem=944) cannot satisfy len=1000
       → allocate block B (size=1024, used=0), current = B → A */
    assert(pt_reserve(&c, 1000) != NULL);

    /* Consume B heavily so B's remainder < the next request */
    n = 950;
    assert(pt_literal(&c, n) != NULL);
    assert(n == 950);
    /* now current = B(rem=74) → A(rem=944) */

    /* Request: head B(74) < 200, so loop skips B; A(944) >= 200
       → A unlinked from chain, moved to head. */
    p = pt_reserve(&c, 200);
    assert(p != NULL);
    assert(c.tree->arena.current != NULL);

    /* Verify we can write into it */
    memcpy(p, "test", 4);

    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_arena_literal_cold(void) {
    /* pt_literal cold-start: arena not built, *plen < PT_ARENA_SIZE */
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    size_t    n;
    char     *p;

    pt_seek(&c, b, 0);
    assert(c.tree->arena.current == NULL);

    n = 10;
    p = pt_reserve(&c, n), assert(p != NULL);
    p = (char *)pt_literal(&c, n), assert(p != NULL);
    assert(c.tree->arena.current != NULL);
    assert(c.tree->arena.current->size == PT_ARENA_SIZE);
    assert(c.tree->arena.current->used == 10);
    memcpy(p, "hello", 5);

    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_arena_literal_params(void) {
    /* pt_literal parameter validation returning NULL */
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    size_t    n;

    pt_seek(&c, b, 0);

    /* NULL cursor */
    n = 10;
    assert(pt_literal(NULL, n) == NULL);

    /* *plen == 0 */
    n = 0;
    assert(pt_literal(&c, n) == NULL);

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= hole remove tests ================= */

static void test_remove_params(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    assert(pt_remove(NULL, 5) == PT_ERRPARAM);
    pt_seek(&c, b, 0);
    assert(pt_remove(&c, 0) == PT_OK);   /* len==0 */
    assert(pt_remove(&c, 100) == PT_OK); /* empty tree, clamped to 0 */
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_hole_whole(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "hello", 5) == PT_OK); /* fork + hole in dirty tree */
    assert(pt_bytes(c.tree) == 5 && c.dirty);
    pt_locate(&c, 0);
    assert(pt_remove(&c, 5) == PT_OK); /* remove entire hole */
    assert(pt_checktree(c.tree));
    assert(pt_bytes(c.tree) == 0);
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_hole_mid(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_edit(&c, 0, "hello world", 11); /* fork + hole in dirty tree */
    assert(pt_bytes(c.tree) == 11 && c.dirty);
    pt_locate(&c, 2);                  /* pos 2: 'l' in "hello world" */
    assert(pt_remove(&c, 5) == PT_OK); /* delete [2,7): "llo w" from hole */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 2));
    assert(pt_bytes(c.tree) == 6);
    /* result: hole "heorld" (indices 0-1 + 7-11) = 6 bytes */
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 1);
        assert(ptM_ishole(r, 0));
        {
            assert(r->bytes[0] == 6);
            assert(memcmp(((pt_Hole *)r->children[0])->data, "heorld", 6) == 0);
        }
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_hole_boundary(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    /* build dirty tree: lit("abc") + hole("DEF") + lit("ghi") = 9B */
    pt_seek(&c, b, 0);
    pt_insert(&c, "abc", 3);
    pt_advance(&c, 3);
    pt_edit(&c, 0, "DEF", 3);
    pt_advance(&c, 3);
    pt_insert(&c, "ghi", 3);
    assert(pt_bytes(c.tree) == 9 && c.dirty);
    /* delete [2,6): "c" (tail of lit) + "DEF" (whole hole) */
    pt_locate(&c, 2);
    assert(pt_remove(&c, 4) == PT_OK);
    assert(pt_checktree(c.tree));
    /* result: "ab"(lit) + "ghi"(lit) — no hole left */
    pt_asserttree(c.tree, 0, leafV(litV("ab"), litV("ghi")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_hole_mixed(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    /* build dirty tree: lit("abc") + hole("DEF") + lit("ghi") = 9B */
    pt_seek(&c, b, 0);
    pt_insert(&c, "abc", 3);
    pt_advance(&c, 3);
    pt_edit(&c, 0, "DEF", 3);
    pt_advance(&c, 3);
    pt_insert(&c, "ghi", 3);
    assert(pt_bytes(c.tree) == 9 && c.dirty);
    /* delete [1,5): "bc"(2 from lit") + "DE"(2 from hole) */
    pt_locate(&c, 1);
    assert(pt_remove(&c, 4) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 1));
    /* result: "a"(lit) + hole"F"(1) + "ghi"(lit) */
    assert(pt_bytes(c.tree) == 5);
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 3);
        assert(!ptM_ishole(r, 0)); /* lit "a" */
        assert(ptM_ishole(r, 1));  /* hole "F" */
        assert(!ptM_ishole(r, 2)); /* lit "ghi" */
        {
            pt_Hole *h = (pt_Hole *)r->children[1];
            assert(r->bytes[1] == 1 && h->data[0] == 'F');
        }
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_cow(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Blob   a;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "abcdef", 6);
    a = pt_commit(&c); /* committed blob */
    pt_seek(&c, a, 2);
    assert(pt_remove(&c, 2) == PT_OK); /* remove from committed tree */
    assert(pt_checktree(c.tree));
    assert(pt_checktree(a)); /* source unchanged */
    assert(pt_bytes(a) == 6);
    assert(pt_bytes(c.tree) == 4);
    pt_asserttree(c.tree, 0, leafV(litV("ab"), litV("ef")));
    pt_asserttree(a, 0, leafV(litV("abcdef")));
    pt_release(c.tree), pt_release(a), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_oom(void) {
    int       cnt = 1000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Blob   b = pt_empty(S);
    pt_Blob   a;
    pt_Cursor c;
    int       r;
    /* Create a committed tree with content */
    pt_seek(&c, b, 0);
    assert(pt_insert(&c, "abcd", 4) == PT_OK);
    a = pt_commit(&c); /* c.tree == a, refcount remains 1 */
    assert(pt_bytes(a) == 4);
    /* Clear freelist so reserve must allocate a new page */
    {
        void *saved_freed = S->nodes.freed;
        S->nodes.freed = NULL;
        cnt = 0; /* next alloc fails */
        pt_seek(&c, a, 1);
        r = pt_remove(&c, 2);
        assert(r == PT_ERRMEM);
        assert(!c.dirty);
        cnt = 1000;
        S->nodes.freed = saved_freed; /* restore freelist */
    }
    pt_release(a), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_brute(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    size_t    total;
    size_t    pos, len;
    /* construct tree with 20 bytes in 2 pieces via two appends */
    pt_seek(&c, b, 0);
    pt_append(&c, "ABCDEFGHIJ", 10);
    pt_append(&c, "abcdefghij", 10);
    total = pt_bytes(c.tree);
    assert(total == 20);
    assert(c.tree->levels == 0);
    /* brute all (pos,len) that yield positive deletion */
    for (pos = 0; pos < total; ++pos) {
        size_t maxlen = total - pos;
        for (len = 1; len <= maxlen; ++len) {
            pt_Blob   fresh = pt_empty(S);
            pt_Cursor cc;
            /* rebuild tree from scratch via one append */
            pt_seek(&cc, fresh, 0);
            pt_append(&cc, "ABCDEFGHIJabcdefghij", 20);
            pt_release(fresh);
            assert(pt_bytes(cc.tree) == 20);
            /* remove */
            pt_locate(&cc, pos);
            assert(pt_remove(&cc, len) == PT_OK);
            if (!pt_checktree(cc.tree)) {
                pt_log("FAIL: remove_brute pos=%zu len=%zu\n", pos, len);
                assert(0);
            }
            assert(pt_bytes(cc.tree) == total - len);
            pt_release(cc.tree);
        }
    }
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_stitch_full(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    /* COW + cross-leaf eraserange: commit tree then remove across leaves */
    pt_Blob b = treeV(
            1, innerV(leafV(litV("aa"), litV("bb")),
                      leafV(litV("cc"), litV("dd"))));
    pt_seek(&c, b, 2);
    assert(pt_remove(&c, 4) == PT_OK); /* delete "bb"+"cc" cross-leaf */
    assert(pt_checktree(c.tree));
    assert(pt_bytes(c.tree) == 4); /* "aa"+"dd" */
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_fold_balance(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    /* levels=1: inner(leaf("a"), leaf("x","y","z","w"))
       Remove "a" -> left leaf empty, foldnode tries merge but
       totals > FANOUT(4) -> triggers balancenode */
    pt_Blob b = treeV(
            1, innerV(leafV(litV("a")),
                      leafV(litV("x"), litV("y"), litV("z"), litV("w"))));
    pt_seek(&c, b, 0);
    assert(pt_remove(&c, 3) == PT_OK);
    assert(pt_checktree_allow_empty(c.tree, 1));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_hole_trim(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 1, 0, leafV(holeV("abc"), holeV("def")));
    assert(pt_remove(&c, 4) == PT_OK);
    assert(pt_bytes(c.tree) == 2);
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* seam merge: delete a non-contiguous piece separating two同源buf残片,
   so the残片s become physically adjacent → mergeleaf fuses them.
   Covers: same node, cross node, multi-element cross node. */
static void test_remove_merge_literal(void) {
    static const char buf[] = "abcdef";
    /* --- same node (levels=0): [abc][SEP][def] delete SEP --- */
    {
        pt_State *S = pt_open(&test_alloc, NULL);
        pt_Node  *lf = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Blob   b;
        pt_Cursor c;
        memset(lf, 0, sizeof(pt_Node));
        lf->children[0] = (pt_Node *)(buf + 0), lf->bytes[0] = 3;
        lf->children[1] = (pt_Node *)"SEP", lf->bytes[1] = 3;
        lf->children[2] = (pt_Node *)(buf + 3), lf->bytes[2] = 3;
        lf->child_count = 3;
        b = treeV(0, lf);
        pt_seek(&c, b, 3);
        assert(pt_remove(&c, 3) == PT_OK);
        assert(pt_checktree(c.tree) && pt_checkcursor(&c, 3));
        pt_asserttree(c.tree, 0, leafV(litV("abcdef")));
        pt_release(c.tree), pt_release(b);
        assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
        pt_close(S);
    }
    /* --- cross node (levels=1): [abc,P][Q,def] delete P+Q --- */
    {
        pt_State *S = pt_open(&test_alloc, NULL);
        pt_Node  *l0 = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Node  *l1 = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Blob   b;
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
        assert(pt_remove(&c, 2) == PT_OK);
        assert(pt_checktree(c.tree) && pt_checkcursor(&c, 3));
        pt_asserttree(c.tree, 0, leafV(litV("abcdef")));
        pt_release(c.tree), pt_release(b);
        assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
        pt_close(S);
    }
    /* --- multi-element cross node: [abc,X,Y][Z,def] delete X+Y+Z --- */
    {
        pt_State *S = pt_open(&test_alloc, NULL);
        pt_Node  *l0 = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Node  *l1 = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Blob   b;
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
        assert(pt_remove(&c, 3) == PT_OK);
        assert(pt_checktree(c.tree) && pt_checkcursor(&c, 3));
        pt_asserttree(c.tree, 0, leafV(litV("abcdef")));
        pt_release(c.tree), pt_release(b);
        assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
        pt_close(S);
    }
}

/* === hole merge tests (mergeleaf full+partial merge) === */

static void test_remove_merge_hole_full(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 10, 1,
          innerV(leafV(holeV("AAAAAAAAAA"), litV("X")),
                 leafV(holeV("BBBBBB"))));
    assert(pt_remove(&c, 1) == PT_OK);
    assert(pt_checktree(c.tree) && pt_checkcursor(&c, 10));
    assert(pt_bytes(c.tree) == 16);
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_remove_merge_hole_split(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    /* mergeleaf full merge: hole A(10) + hole B(5) = 15 ≤ 16 */
    editV(&c, 10, 1,
          innerV(leafV(holeV("AAAAAAAAAA"), litV("X")), leafV(holeV("BBBBB"))));
    assert(pt_remove(&c, 1) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_bytes(c.tree) == 15);
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* === deep stitch + findroom/backwardnode === */

static void test_remove_stitch_deep(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b;
    pt_Cursor c;
    /* levels=1: root with 3 leaves, each with 2 pieces.
       Delete 6 bytes across all 3 leaves → triggers stitch+backwardnode. */
    b = treeV(
            1,
            innerV(leafV(litV("aa"), litV("bb")), leafV(litV("cc"), litV("dd")),
                   leafV(litV("ee"), litV("ff"))));
    pt_seek(&c, b, 2);                 /* start of "bb" */
    assert(pt_remove(&c, 8) == PT_OK); /* delete "bb"+"cc"+"dd"+"ee" = 8B */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 2));
    assert(pt_bytes(c.tree) == 4); /* "aa"+"ff" remain */
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* === trimright poff==0 + mask === */

static void test_remove_trim_hole(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 0, 1,
          innerV(leafV(holeV("xy"), litV("ab")),
                 leafV(litV("cd"), litV("ef"))));
    assert(pt_remove(&c, 4) == PT_OK);
    assert(pt_checktree(c.tree));
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* === eraseleaf cross-piece hole === */

static void test_remove_hole_eraseleaf(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_edit(&c, 0, "hole1", 5); /* hole("hole1") at pos 0 */
    pt_insert(&c, "lit", 3);    /* lit at pos 5 */
    pt_advance(&c, 3);          /* to pos 8 */
    pt_edit(&c, 0, "hole2", 5); /* hole("hole2") at pos 8 */
    pt_release(b);
    pt_locate(&c, 4);
    assert(pt_remove(&c, 3) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 4));
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* === mergelit left-side merge === */

/* ================= splice tests ================= */

static void test_splice_basic(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("hello")));
    pt_Cursor c;
    pt_seek(&c, b, 1);
    assert(pt_splice(&c, 3, "XYZ", 3) == PT_OK); /* del "ell", ins "XYZ" */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 4)); /* cursor after remove+append: pos 1+3=4 */
    pt_asserttree(c.tree, 0, leafV(litV("h"), litV("XYZ"), litV("o")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_splice_del0(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("abc")));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_splice(&c, 0, "XYZ", 3) == PT_OK); /* del=0 → insert only */
    assert(pt_checktree(c.tree));
    pt_asserttree(c.tree, 0, leafV(litV("XYZ"), litV("abc")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_splice_null(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("hello")));
    pt_Cursor c;
    pt_seek(&c, b, 1);
    assert(pt_splice(&c, 3, NULL, 0) == PT_OK); /* del "ell", no insert */
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 1));
    pt_asserttree(c.tree, 0, leafV(litV("h"), litV("o")));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= stitch coverage: findroom/backwardnode ================= */

static void test_remove_stitch_overflow(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(
            2, innerV(innerV(leafV(litV("a")), leafV(litV("b"))),
                      innerV(leafV(litV("c")), leafV(litV("d"))),
                      innerV(leafV(litV("e")))));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_remove(&c, 3) == PT_OK);
    assert(pt_checktree_allow_empty(c.tree, 1));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= fold balance coverage: balancenode ================= */

static void test_remove_foldnode_balance(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(
            3, innerV(innerV(innerV(leafV(litV("a")), leafV(litV("b"))),
                             innerV(leafV(litV("c")), leafV(litV("d")))),
                      innerV(innerV(leafV(litV("e"))))));
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_remove(&c, 3) == PT_OK);
    assert(pt_checktree_allow_empty(c.tree, 1));
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_from_basic(void) {
    static const char buf[] = "hello world";
    pt_State         *S = pt_open(&test_alloc, NULL);
    pt_Blob           b = pt_from(S, buf, (size_t)strlen(buf));
    pt_Cursor         c;
    assert(b && pt_bytes(b) == 11);
    assert(pt_checktree(b));
    pt_seek(&c, b, 0);
    assert(pt_checkcursor(&c, 0));
    pt_seek(&c, b, 5);
    assert(pt_insert(&c, "!", 1) == PT_OK); /* insert at space */
    assert(pt_bytes(c.tree) == 12);
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= pt_edit tests ================= */

static void test_edit_params(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    assert(pt_edit(NULL, 0, "x", 1) == PT_ERRPARAM);
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "x", PT_MAX_HOLESIZE + 1) == PT_ERRPARAM);
    assert(pt_edit(&c, 0, NULL, 1) == PT_ERRPARAM);
    assert(pt_edit(&c, 0, NULL, 0) == PT_OK);
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_empty(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "hello", 5) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 5));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 1);
        assert(ptM_ishole(r, 0));
        {
            pt_Hole *h = (pt_Hole *)r->children[0];
            assert(r->bytes[0] == 5 && memcmp(h->data, "hello", 5) == 0);
        }
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_fresh_lit_mid(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("abcdef")));
    pt_Cursor c;
    pt_seek(&c, b, 3);
    assert(pt_edit(&c, 0, "XYZ", 3) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 6));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 3);
        assert(!ptM_ishole(r, 0));
        assert(r->bytes[0] == 3 && memcmp(r->children[0], "abc", 3) == 0);
        assert(ptM_ishole(r, 1));
        {
            pt_Hole *h = (pt_Hole *)r->children[1];
            assert(r->bytes[1] == 3 && memcmp(h->data, "XYZ", 3) == 0);
        }
        assert(!ptM_ishole(r, 2));
        assert(r->bytes[2] == 3 && memcmp(r->children[2], "def", 3) == 0);
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_fresh_boundary(void) {
    pt_State *S = pt_open(&test_alloc, NULL);

    /* poff==0: insert before piece */
    {
        pt_Blob   b = treeV(0, leafV(litV("abcdef")));
        pt_Cursor c;
        pt_seek(&c, b, 0);
        assert(pt_edit(&c, 0, "XYZ", 3) == PT_OK);
        assert(pt_checktree(c.tree));
        assert(pt_checkcursor(&c, 3));
        {
            pt_Node *r = &c.tree->root;
            assert(r->child_count == 2);
            assert(ptM_ishole(r, 0));
            {
                pt_Hole *h = (pt_Hole *)r->children[0];
                assert(r->bytes[0] == 3 && memcmp(h->data, "XYZ", 3) == 0);
            }
            assert(!ptM_ishole(r, 1));
            assert(r->bytes[1] == 6
                   && memcmp(r->children[1], "abcdef", 6) == 0);
        }
        pt_release(c.tree), pt_release(b);
    }

    /* poff==len: insert after piece */
    {
        pt_Blob   b = treeV(0, leafV(litV("abcdef")));
        pt_Cursor c;
        pt_seek(&c, b, 6);
        assert(pt_edit(&c, 0, "XYZ", 3) == PT_OK);
        assert(pt_checktree(c.tree));
        assert(pt_checkcursor(&c, 9));
        {
            pt_Node *r = &c.tree->root;
            assert(r->child_count == 2);
            assert(!ptM_ishole(r, 0));
            assert(r->bytes[0] == 6
                   && memcmp(r->children[0], "abcdef", 6) == 0);
            assert(ptM_ishole(r, 1));
            {
                pt_Hole *h = (pt_Hole *)r->children[1];
                assert(r->bytes[1] == 3 && memcmp(h->data, "XYZ", 3) == 0);
            }
        }
        pt_release(c.tree), pt_release(b);
    }

    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_append_tail(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "hello", 5) == PT_OK);
    assert(c.dirty);
    pt_release(b);
    pt_locate(&c, 5);
    assert(pt_edit(&c, 0, " world", 6) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 11));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 1);
        assert(ptM_ishole(r, 0));
        {
            pt_Hole *h = (pt_Hole *)r->children[0];
            assert(r->bytes[0] == 11
                   && memcmp(h->data, "hello world", 11) == 0);
        }
    }
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_append_full(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    int       i;
    /* hole with 14 bytes (close to PT_MAX_HOLESIZE=16), append 5 → overflow */
    {
        static char bigbuf[15];
        for (i = 0; i < 14; ++i) bigbuf[i] = 'a';
        bigbuf[14] = '\0';
        pt_seek(&c, b, 0);
        assert(pt_edit(&c, 0, bigbuf, 14) == PT_OK);
    }
    pt_release(b);
    pt_locate(&c, 14);
    assert(pt_edit(&c, 0, "bbbbb", 5) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 19));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 2);
        assert(ptM_ishole(r, 0));
        assert(ptM_ishole(r, 1));
        {
            pt_Hole *ha = (pt_Hole *)r->children[0];
            assert(r->bytes[0] == 14);
            for (i = 0; i < 14; ++i) assert(ha->data[i] == 'a');
        }
        {
            pt_Hole *hb = (pt_Hole *)r->children[1];
            assert(r->bytes[1] == 5 && memcmp(hb->data, "bbbbb", 5) == 0);
        }
    }
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_prev_hole(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_edit(&c, 0, "hello", 5); /* hole("hello") at pos 0 */
    pt_insert(&c, "XYZ", 3);    /* lit("XYZ") at pos 5 */
    pt_release(b);
    /* seek to 5: boundary after hole "hello", start of lit "XYZ" */
    pt_locate(&c, 5);
    assert(pt_edit(&c, 0, "abc", 3) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 8));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 2);
        assert(ptM_ishole(r, 0));
        {
            pt_Hole *h = (pt_Hole *)r->children[0];
            assert(r->bytes[0] == 8 && memcmp(h->data, "helloabc", 8) == 0);
        }
        assert(!ptM_ishole(r, 1));
        assert(r->bytes[1] == 3 && memcmp(r->children[1], "XYZ", 3) == 0);
    }
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_mid_fit(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "hello world", 11) == PT_OK);
    assert(c.dirty);
    pt_release(b);
    pt_locate(&c, 5);
    assert(pt_edit(&c, 0, "XYZ", 3) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 8));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 1);
        assert(ptM_ishole(r, 0));
        {
            pt_Hole *h = (pt_Hole *)r->children[0];
            assert(r->bytes[0] == 14);
            assert(memcmp(h->data, "helloXYZ world", 14) == 0);
        }
    }
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_mid_split(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    int       i;
    /* hole with 12 bytes, insert 5 at middle → 17 > CAP → splitins */
    {
        static char splbuf[13];
        for (i = 0; i < 12; ++i) splbuf[i] = 'a';
        splbuf[12] = '\0';
        pt_seek(&c, b, 0);
        assert(pt_edit(&c, 0, splbuf, 12) == PT_OK);
    }
    pt_release(b);
    pt_locate(&c, 6);
    assert(pt_edit(&c, 0, "bbbbb", 5) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 11));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 3);
        assert(ptM_ishole(r, 0));
        assert(ptM_ishole(r, 1));
        assert(ptM_ishole(r, 2));
        {
            pt_Hole *hl = (pt_Hole *)r->children[0];
            assert(r->bytes[0] == 6);
            for (i = 0; i < 6; ++i) assert(hl->data[i] == 'a');
        }
        {
            pt_Hole *hm = (pt_Hole *)r->children[1];
            assert(r->bytes[1] == 5 && memcmp(hm->data, "bbbbb", 5) == 0);
        }
        {
            pt_Hole *hr = (pt_Hole *)r->children[2];
            assert(r->bytes[2] == 6);
            for (i = 0; i < 6; ++i) assert(hr->data[i] == 'a');
        }
    }
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_del_then_ins(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello world", 11);
    pt_release(b);
    pt_locate(&c, 3);
    assert(pt_edit(&c, 2, "XYZ", 3) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 6));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 3);
        assert(!ptM_ishole(r, 0));
        assert(r->bytes[0] == 3 && memcmp(r->children[0], "hel", 3) == 0);
        assert(ptM_ishole(r, 1));
        {
            pt_Hole *h = (pt_Hole *)r->children[1];
            assert(r->bytes[1] == 3 && memcmp(h->data, "XYZ", 3) == 0);
        }
        assert(!ptM_ishole(r, 2));
        assert(r->bytes[2] == 6);
        assert(memcmp(r->children[2], " world", 6) == 0);
    }
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_del_only(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_insert(&c, "hello world", 11);
    pt_release(b);
    pt_locate(&c, 3);
    assert(pt_edit(&c, 5, NULL, 0) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 3));
    pt_asserttree(c.tree, 0, leafV(litV("hel"), litV("rld")));
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_type_sequence(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    int       k, n = 15;
    pt_seek(&c, b, 0);
    for (k = 0; k < n; ++k) {
        char ch = (char)('a' + (k % 26));
        assert(pt_edit(&c, 0, &ch, 1) == PT_OK);
        assert(pt_checktree(c.tree));
        assert(pt_checkcursor(&c, (size_t)(k + 1)));
    }
    /* all 15 chars merged into a single hole via branch A */
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 1);
        assert(ptM_ishole(r, 0));
        {
            assert(r->bytes[0] == (size_t)n);
            {
                pt_Hole *h = (pt_Hole *)r->children[0];
                for (k = 0; k < n; ++k)
                    assert(h->data[k] == (char)('a' + (k % 26)));
            }
        }
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_split_tree(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_seek(&c, b, 0);
    pt_append(&c, "aa", 2);
    pt_append(&c, "bb", 2);
    pt_append(&c, "cc", 2);
    pt_append(&c, "dd", 2);
    pt_release(b);
    pt_locate(&c, 2);
    assert(pt_edit(&c, 0, "ZZ", 2) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 4));
    /* expected: levels=1, inner(left: lit"aa", hole"ZZ", lit"bb",
       right: lit"cc", lit"dd") */
    {
        pt_Node *exp_lf = leafV(litV("aa"), holeV("ZZ"), litV("bb"));
        pt_Node *exp_rf = leafV(litV("cc"), litV("dd"));
        pt_Node *exp_in = innerV(exp_lf, exp_rf);
        pt_Blob  expected = treeV(1, exp_in);
        assert(pt_comparetree(c.tree, expected));
        pt_release(expected);
    }
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_edit_upmask(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b;
    pt_Cursor c;
    pt_Node  *r, *leaf0, *leaf1;
    pt_Hole  *hole;
    /* need each leaf to have ≥2 children for pt_checktree */
    b = treeV(
            1, innerV(leafV(litV("aa"), litV("bb")),
                      leafV(litV("cc"), litV("dd"))));
    pt_seek(&c, b, 1);
    assert(pt_edit(&c, 0, "XYZ", 3) == PT_OK);
    assert(pt_checktree(c.tree));
    assert(pt_checkcursor(&c, 4));
    r = &c.tree->root;
    assert(ptM_ishole(r, 0));
    assert(!ptM_ishole(r, 1));
    leaf0 = r->children[0];
    assert(leaf0->child_count == 4);
    assert(!ptM_ishole(leaf0, 0));
    assert(leaf0->bytes[0] == 1 && memcmp(leaf0->children[0], "a", 1) == 0);
    assert(ptM_ishole(leaf0, 1));
    hole = (pt_Hole *)leaf0->children[1];
    assert(leaf0->bytes[1] == 3 && memcmp(hole->data, "XYZ", 3) == 0);
    assert(!ptM_ishole(leaf0, 2));
    assert(leaf0->bytes[2] == 1 && memcmp(leaf0->children[2], "a", 1) == 0);
    assert(!ptM_ishole(leaf0, 3));
    assert(leaf0->bytes[3] == 2 && memcmp(leaf0->children[3], "bb", 2) == 0);
    leaf1 = r->children[1];
    assert(leaf1->child_count == 2);
    assert(!ptM_ishole(leaf1, 0));
    assert(leaf1->bytes[0] == 2 && memcmp(leaf1->children[0], "cc", 2) == 0);
    assert(!ptM_ishole(leaf1, 1));
    assert(leaf1->bytes[1] == 2 && memcmp(leaf1->children[1], "dd", 2) == 0);
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= pt_commit tests ================= */

static void test_commit_single_hole(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    pt_Blob   snap;
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "hello", 5) == PT_OK);
    assert(c.dirty);
    snap = pt_commit(&c);
    assert(snap != NULL);
    assert(!c.dirty);
    assert(pt_checktree(snap));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 1);
        assert(!ptM_ishole(r, 0));
        assert(r->bytes[0] == 5);
        assert(memcmp(r->children[0], "hello", 5) == 0);
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* E11: consecutive holes → adjacent literals, NOT merged, child_count unchanged
 */
static void test_commit_no_merge(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    int       i;
    pt_seek(&c, b, 0);
    {
        char bigbuf[64];
        for (i = 0; i < 14; ++i) bigbuf[i] = 'a';
        bigbuf[14] = '\0';
        assert(pt_edit(&c, 0, bigbuf, 14) == PT_OK);
    }
    assert(pt_edit(&c, 0, "!!", 2) == PT_OK); /* fills to CAP=16 */
    assert(pt_edit(&c, 0, "XY", 2) == PT_OK); /* over CAP → 2nd hole */
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 2);
        assert(ptM_ishole(r, 0) && ptM_ishole(r, 1));
    }
    assert(pt_commit(&c) != NULL);
    assert(pt_checktree(c.tree));
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 2);
        assert(!ptM_ishole(r, 0) && !ptM_ishole(r, 1));
        assert(r->bytes[0] == PT_MAX_HOLESIZE);
        assert(r->bytes[1] == 2);
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_commit_mixed(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
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
    assert(pt_edit(&c, 0, "x", 1) == PT_OK);
    assert(pt_commit(&c) != NULL);
    assert(pt_checktree(c.tree));
    {
        pt_Node *r = &c.tree->root;
        for (i = 0; i < (int)r->child_count; ++i) assert(!ptM_ishole(r, i));
    }
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_commit_deep(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 0, 1,
          innerV(leafV(holeV("abc")), leafV(litV("def"), litV("ghi"))));
    assert(pt_edit(&c, 0, "XYZ", 3) == PT_OK);
    assert(pt_commit(&c) != NULL);
    assert(pt_checktree(c.tree));
    {
        pt_Node *r = &c.tree->root;
        int      i;
        for (i = 0; i < (int)r->child_count; ++i) assert(!ptM_ishole(r, i));
    }
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* commit exactly fills the arena block: the full block must retire to
 * the full list so pt_scratch stays valid afterwards */
static void test_commit_fillblock(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    size_t    cap;
    pt_seek(&c, b, 0);
    assert(pt_reserve(&c, 0) != NULL); /* PT_ARENA_SIZE block */
    assert(pt_scratch(&c, &cap) != NULL);
    assert(pt_literal(&c, cap - 8) != NULL);        /* leave exactly 8 bytes */
    assert(pt_edit(&c, 0, "12345678", 8) == PT_OK); /* 8-byte hole */
    assert(pt_commit(&c) != NULL); /* freeze consumes the last 8 bytes */
    assert(pt_checktree(c.tree) && !c.dirty);
    assert(c.tree->arena.full != NULL && c.tree->arena.current == NULL);
    assert(pt_scratch(&c, &cap) == NULL && cap == 0);
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* E7: freeze copies holes into scratch, handles page transition */
static void test_commit_freshpage(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    pt_Blob   b = pt_empty(S);
    /* Single edit → 1 hole; commit copies data into scratch */
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "aaaaaaaaaaaaaaa", 15) == PT_OK);
    assert(pt_commit(&c) != NULL);
    assert(pt_checktree(c.tree) && !c.dirty);
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 1);
        assert(!ptM_ishole(r, 0));
        assert(r->bytes[0] == 15);
    }
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_commit_clean(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Blob   snap;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    snap = pt_commit(&c);
    assert(snap == b);
    pt_release(snap);
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_commit_then_reseek(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Blob   snap;
    pt_Cursor c;
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "abcdef", 6) == PT_OK);
    snap = pt_commit(&c);
    assert(snap != NULL);
    pt_seek(&c, snap, 3);
    assert(pt_offset(&c) == 3);
    assert(pt_checkcursor(&c, 3));
    pt_release(snap), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* E11/E12: bytes/levels/child_count unchanged by freeze */
static void test_commit_bytes_invariant(void) {
    pt_State      *S = pt_open(&test_alloc, NULL);
    pt_Blob        b = pt_empty(S);
    pt_Cursor      c;
    size_t         bytes_before;
    unsigned       levels_before;
    unsigned short cc_before;
    pt_seek(&c, b, 0);
    assert(pt_edit(&c, 0, "abc", 3) == PT_OK); /* hole("abc") at pos 0 */
    assert(pt_edit(&c, 0, "DE", 2) == PT_OK);  /* appends hole("DE") */
    pt_release(b);
    pt_locate(&c, 0);
    assert(pt_edit(&c, 0, "XY", 2) == PT_OK); /* add hole("XY") at front */
    bytes_before = pt_bytes(c.tree);
    levels_before = c.tree->levels;
    cc_before = c.tree->root.child_count;
    assert(pt_commit(&c) != NULL);
    assert(pt_bytes(c.tree) == bytes_before);
    assert(c.tree->levels == levels_before);
    assert(c.tree->root.child_count == cc_before);
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* multi-page reserve chain (levels=1, 3 leaves with holes) */
static void test_commit_reserve_pages(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Cursor c;
    editV(&c, 0, 1,
          innerV(leafV(holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa"),
                       holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa")),
                 leafV(holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa"),
                       holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa")),
                 leafV(holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa"),
                       holeV("aaaaaaaaaaaaaaaa"), holeV("aaaaaaaaaaaaaaaa"))));
    assert(pt_edit(&c, 0, ".", 1) == PT_OK);
    assert(pt_commit(&c) != NULL);
    assert(pt_checktree(c.tree));
    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* E12 all-or-nothing: reservescratch allocf fail → NULL, tree untouched */
static void test_commit_reservebuf_oom(void) {
    int       cnt = 10000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    size_t    bytes_before;
    int       i;
    pt_seek(&c, b, 0);
    /* create a tree with holes via pt_edit */
    {
        char bigbuf[64];
        for (i = 0; i < 15; ++i) bigbuf[i] = 'a';
        bigbuf[15] = '\0';
        assert(pt_edit(&c, 0, bigbuf, 15) == PT_OK);
    }
    bytes_before = pt_bytes(c.tree);
    assert(c.dirty && bytes_before == 15);
    cnt = 0; /* kill allocf — next alloc (scratch page) fails */
    assert(pt_commit(&c) == NULL);
    assert(c.dirty == 1);                     /* tree not frozen */
    assert(pt_bytes(c.tree) == bytes_before); /* bytes unchanged */
    assert(pt_checktree(c.tree));
    /* cleanup: transient still has holes → release normally */
    pt_release(c.tree);
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* §8.3 full round-trip: edit series → commit → content matches reference */
static void test_edit_commit_roundtrip(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_from(S, "Hello World", 11);
    pt_Cursor c;
    pt_Blob   snap;
    pt_seek(&c, b, 5);
    assert(pt_edit(&c, 1, "XY", 2) == PT_OK); /* replace " " with "XY" */
    snap = pt_commit(&c);
    assert(snap != NULL);
    assert(!c.dirty);
    assert(pt_checktree(snap));
    assert(pt_bytes(snap) == 12);
    {
        pt_Node *r = &c.tree->root;
        assert(r->child_count == 3);
        assert(!ptM_ishole(r, 0) && !ptM_ishole(r, 1) && !ptM_ishole(r, 2));
        assert(r->bytes[0] == 5);
        assert(r->bytes[1] == 2);
        assert(r->bytes[2] == 5);
        assert(memcmp(r->children[0], "Hello", 5) == 0);
        assert(memcmp(r->children[1], "XY", 2) == 0);
        assert(memcmp(r->children[2], "World", 5) == 0);
    }
    pt_release(snap), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* §8.3 commit then seek+edit (new transient), verify independent version */
static void test_edit_commit_edit(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_from(S, "Hello", 5);
    pt_Cursor c;
    pt_Blob   snap;
    pt_seek(&c, b, 2);
    assert(pt_edit(&c, 0, "XY", 2) == PT_OK); /* "HeXYlo" */
    snap = pt_commit(&c);
    assert(snap != NULL && !c.dirty);
    /* second edit on committed snapshot */
    pt_seek(&c, snap, 4);
    assert(pt_edit(&c, 0, "!", 1) == PT_OK);
    assert(c.dirty && pt_bytes(c.tree) == 8);
    assert(pt_commit(&c) != NULL);
    assert(!c.dirty && pt_checktree(c.tree) && pt_bytes(c.tree) == 8);
    {
        pt_Node *r = &c.tree->root;
        int      i;
        size_t   total = 0;
        for (i = 0; i < (int)r->child_count; ++i) {
            assert(!ptM_ishole(r, i));
            total += r->bytes[i];
        }
        assert(total == 8);
    }
    /* first snapshot unchanged (bytes before 2nd edit) */
    assert(pt_bytes(snap) == 7 && pt_checktree(snap));
    /* cleanup: release c.tree (2nd committed) → cascades to snap, then b */
    pt_release(c.tree), pt_release(snap), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= deep commit with levels>=2 and holes ================= */

/* ================= deep commit with levels>=2 and holes ================= */

/* BUG BLOCKER: ptM_upmask in piecetab.h (L865-872) iterates root→leaf
   instead of leaf→root, so for levels≥2 trees the ROOT's mask is never
   updated when a hole is inserted in a leaf.  This means:
   - ptC_holebytes (L452 FALSE) and ptC_freeze (L541 FALSE) never descend
     into inner nodes because the ROOT has no mask bits → UNREACHABLE.
   - pt_checktree fails on any levels≥2 tree with unfrozen holes.
   Fix: change loop direction in ptM_upmask to descend (l = levels-1 → 0).
   Once fixed, this test should seek at multiple positions in a levels≥2
   tree, pt_edit to create holes, assert pt_checktree passes, then commit
   and verify all masks cleared. */

static void test_commit_deep2(void) {
    /* levels>=2 tree with a hole under an inner node: exercises the
       descend (FALSE) branch of ptC_holebytes/ptC_freeze, and verifies
       ptM_upmask propagates the hole bit to the root (leaf->root). */
    static char buf[200], exp0[128], exp1[128], got[128];
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Blob     b = pt_empty(S);
    pt_Blob     a, a2;
    pt_Cursor   c;
    size_t      len0, len1, gl;
    int         k, n = 30;

    for (k = 0; k < 200; ++k) buf[k] = (char)('!' + (k % 90));
    pt_seek(&c, b, 0);
    /* stride-3 take-2 keeps pieces non-contiguous (no mergelit) -> deep tree */
    for (k = 0; k < n; ++k) assert(pt_append(&c, buf + k * 3, 2) == PT_OK);
    assert(c.tree->levels >= 2);
    a = pt_commit(&c);
    assert(a != NULL && !c.dirty);
    pt_release(b);
    assert(pt_checktree(a));
    len0 = collect_bytes(a, exp0, sizeof(exp0)); /* ground truth content */

    /* edit a levels>=2 committed tree: fork + insert a hole under an inner */
    pt_seek(&c, a, 7);
    assert(pt_edit(&c, 0, "ZZ", 2) == PT_OK);
    assert(c.tree->levels >= 2);
    assert(pt_checktree(c.tree)); /* upmask fix: root mask sees the hole */
    memcpy(exp1, exp0, 7);
    exp1[7] = 'Z', exp1[8] = 'Z';
    memcpy(exp1 + 9, exp0 + 7, len0 - 7);
    len1 = len0 + 2;
    gl = collect_bytes(c.tree, got, sizeof(got));
    assert(gl == len1 && memcmp(got, exp1, len1) == 0);

    a2 = pt_commit(&c); /* freeze: must descend into inner subtree */
    assert(a2 != NULL && !c.dirty);
    assert(S->holes.live_obj == 0); /* every hole frozen -> descend worked */
    assert(pt_checktree(a2));
    assert(pt_bytes(a2) == len1);
    gl = collect_bytes(a2, got, sizeof(got));
    assert(gl == len1 && memcmp(got, exp1, len1) == 0);

    pt_release(a);
    pt_release(a2);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= frozen-into-arena verification ================= */

static void test_commit_reserve_leftover(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    int       i;

    pt_seek(&c, b, 0);
    /* 31 edits of 16 bytes = 496 total hole bytes */
    for (i = 0; i < 31; ++i) {
        static char bigbuf[17];
        memset(bigbuf, (char)('A' + (i % 26)), 16);
        assert(pt_edit(&c, 0, bigbuf, 16) == PT_OK);
    }
    assert(c.dirty);
    /* Commit: freeze hole data into tree arena */
    assert(pt_commit(&c) != NULL);
    assert(!c.dirty);

    /* Verify: all holes frozen to literals, mask=0 */
    {
        pt_Node *r = &c.tree->root;
        int      j;
        for (j = 0; j < (int)r->child_count; ++j) {
            assert(!ptM_ishole(r, j));
        }
    }
    assert(pt_checktree(c.tree));
    assert(pt_bytes(c.tree) == 31 * 16);

    /* Verify data content via pt_read */
    {
        char   buf[512];
        size_t n;
        pt_seek(&c, c.tree, 0);
        n = pt_read(&c, buf, sizeof(buf));
        assert(n == 31 * 16);
        for (i = 0; i < 31; ++i) {
            int j;
            for (j = 0; j < 16; ++j)
                assert(buf[i * 16 + j] == (char)('A' + (i % 26)));
        }
    }

    /* Arena block exists with used == total */
    {
        pt_Block *ab = c.tree->arena.current;
        assert(ab != NULL);
        assert(ab->used == 31 * 16);
    }

    /* Release: arena freed, live_obj zero */
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= reservescratch multi-page OOM rollback ================= */

static void test_commit_reservebuf_oom_multi(void) {
    int       cnt = 10000;
    pt_State *S = pt_open(&oom_alloc, &cnt);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    int       i;
    size_t    bytes_before;

    pt_seek(&c, b, 0);
    /* 32 edits of 16 bytes = 512 total → pt_reserve allocs 1 block (1024) */
    for (i = 0; i < 32; ++i) {
        static char bigbuf[17];
        memset(bigbuf, 'x', 16);
        assert(pt_edit(&c, 0, bigbuf, 16) == PT_OK);
    }
    bytes_before = pt_bytes(c.tree);
    assert(c.dirty && bytes_before > 0);

    cnt = 0; /* pt_reserve ptA_alloc fails */
    assert(pt_commit(&c) == NULL);
    /* Tree must be unchanged (E12 all-or-nothing) */
    assert(c.dirty == 1);
    assert(pt_bytes(c.tree) == bytes_before);
    assert(pt_checktree(c.tree));

    /* Cleanup: tree still has holes, release normally */
    cnt = 10000;
    pt_release(c.tree), pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= read interface tests ================= */

static void test_read_params(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_from(S, "hello", 5);
    pt_Cursor c;
    size_t    n;
    char      buf[8];

    /* pt_piece NULL checks */
    assert(pt_piece(NULL, &n) == NULL);

    /* pt_next NULL checks */
    assert(pt_next(NULL, &n) == NULL);

    /* pt_prev NULL checks */
    assert(pt_prev(NULL, &n) == NULL);

    /* pt_read NULL checks */
    assert(pt_read(NULL, buf, 5) == 0);
    pt_seek(&c, b, 0);
    assert(pt_read(&c, NULL, 5) == 0);

    /* pt_next at end (poff==bytes[i]) */
    pt_seek(&c, b, 5);
    {
        const char *p = pt_next(&c, &n);
        assert(p == NULL && n == 0);
    }

    /* pt_prev at start (off==0) */
    pt_seek(&c, b, 0);
    {
        const char *p = pt_prev(&c, &n);
        assert(p == NULL && n == 0);
    }

    /* pt_prev with poff>0 (return current piece start) */
    pt_seek(&c, b, 3);
    {
        const char *p = pt_prev(&c, &n);
        assert(p != NULL && n == 3);
        assert(memcmp(p, "hel", 3) == 0);
        assert(pt_offset(&c) == 0);
    }

    /* pt_read with len=0 */
    pt_seek(&c, b, 0);
    assert(pt_read(&c, buf, 0) == 0);

    /* pt_piece with poff past piece len */
    pt_seek(&c, b, 5);
    {
        const char *p = pt_piece(&c, &n);
        assert(p == NULL && n == 0);
    }

    /* C->tree == NULL branches (zeroed cursor) */
    {
        pt_Cursor cz;
        size_t    zn;
        memset(&cz, 0, sizeof(cz));
        assert(pt_piece(&cz, &zn) == NULL);
        assert(pt_next(&cz, &zn) == NULL);
        assert(pt_prev(&cz, &zn) == NULL);
        assert(pt_read(&cz, buf, 5) == 0);
    }

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_piece_positions(void) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Blob     b = treeV(0, leafV(litV("hello"), litV("world"), litV("!!!")));
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Start of first piece */
    pt_seek(&c, pt_nonnull(b), 0);
    p = pt_piece(&c, &n);
    assert(p != NULL && n == 5 && memcmp(p, "hello", 5) == 0);

    /* Middle of first piece */
    pt_seek(&c, b, 2);
    p = pt_piece(&c, &n);
    assert(p != NULL && n == 3 && memcmp(p, "llo", 3) == 0);

    /* Boundary between pieces */
    pt_seek(&c, b, 5);
    p = pt_piece(&c, &n);
    assert(p != NULL && n == 5 && memcmp(p, "world", 5) == 0);

    /* End of last piece */
    pt_seek(&c, b, 10);
    p = pt_piece(&c, &n);
    assert(p != NULL && n == 3 && memcmp(p, "!!!", 3) == 0);

    /* Past end */
    pt_seek(&c, b, 13);
    p = pt_piece(&c, &n);
    assert(p == NULL && n == 0);

    /* pt_piece with NULL plen */
    pt_seek(&c, b, 0);
    p = pt_piece(&c, NULL);
    assert(p != NULL);

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_next_basic(void) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Blob     b = treeV(0, leafV(litV("aa"), litV("bb"), litV("cc")));
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Forward from start: piece then next */
    pt_seek(&c, b, 0);
    p = pt_piece(&c, &n);
    assert(p != NULL && n == 2 && memcmp(p, "aa", 2) == 0);
    assert(pt_offset(&c) == 0);

    p = pt_next(&c, &n);
    assert(p != NULL && n == 2 && memcmp(p, "bb", 2) == 0);
    assert(pt_offset(&c) == 2);

    p = pt_next(&c, &n);
    assert(p != NULL && n == 2 && memcmp(p, "cc", 2) == 0);
    assert(pt_offset(&c) == 4);

    /* No more pieces */
    p = pt_next(&c, &n);
    assert(p == NULL && n == 0);
    assert(pt_offset(&c) == 6);

    /* Forward from middle */
    pt_seek(&c, b, 2);
    p = pt_next(&c, &n);
    assert(p != NULL && n == 2 && memcmp(p, "cc", 2) == 0);
    assert(pt_offset(&c) == 4);

    p = pt_next(&c, &n);
    assert(p == NULL && n == 0);
    assert(pt_offset(&c) == 6);

    /* pt_next with NULL plen */
    pt_seek(&c, b, 0);
    p = pt_next(&c, NULL);
    assert(p != NULL);

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_prev_basic(void) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Blob     b = treeV(0, leafV(litV("aa"), litV("bb"), litV("cc")));
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Backward from end */
    pt_seek(&c, b, 6);
    p = pt_prev(&c, &n);
    assert(p != NULL && n == 2 && memcmp(p, "cc", 2) == 0);
    assert(pt_offset(&c) == 4);

    p = pt_prev(&c, &n);
    assert(p != NULL && n == 2 && memcmp(p, "bb", 2) == 0);
    assert(pt_offset(&c) == 2);

    p = pt_prev(&c, &n);
    assert(p != NULL && n == 2 && memcmp(p, "aa", 2) == 0);
    assert(pt_offset(&c) == 0);

    /* No more previous */
    p = pt_prev(&c, &n);
    assert(p == NULL && n == 0);

    /* pt_prev with NULL plen */
    pt_seek(&c, b, 6);
    p = pt_prev(&c, NULL);
    assert(p != NULL);

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* Single piece tree — pt_next/pt_prev edges */
static void test_trav_single(void) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Blob     b = treeV(0, leafV(litV("hello")));
    pt_Cursor   c;
    const char *p;
    size_t      n;

    /* Forward: current piece via pt_piece, then next stops */
    pt_seek(&c, b, 0);
    p = pt_piece(&c, &n);
    assert(p != NULL && n == 5 && memcmp(p, "hello", 5) == 0);

    p = pt_next(&c, &n);
    assert(p == NULL && n == 0);
    assert(pt_offset(&c) == 5);

    p = pt_next(&c, &n);
    assert(p == NULL && n == 0);

    /* Backward from middle */
    pt_seek(&c, b, 3);
    p = pt_prev(&c, &n);
    assert(p != NULL && n == 3 && memcmp(p, "hel", 3) == 0);
    assert(pt_offset(&c) == 0);

    /* Backward from end */
    pt_seek(&c, b, 5);
    p = pt_prev(&c, &n);
    assert(p != NULL && n == 5 && memcmp(p, "hello", 5) == 0);
    assert(pt_offset(&c) == 0);

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* Levels=1 forward traversal — exercises walk-up-then-down in pt_next */
static void test_next_levels1(void) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    pt_Blob     b = pt_empty(S);
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
    assert(c.tree->levels == 1);

    /* Full forward */
    pt_seek(&c, c.tree, 0);
    {
        char   fwd[16];
        size_t off = 0;
        while ((p = pt_piece(&c, &n)) != NULL && n > 0) {
            memcpy(fwd + off, p, n), off += n;
            pt_next(&c, &n);
        }
        assert(off == 10 && memcmp(fwd, "aabbccddee", 10) == 0);
    }

    pt_release(c.tree);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* Levels=1 backward traversal */
static void test_prev_levels1(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(
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
        assert(end == 0 && memcmp(rev, "aabbccdd", 8) == 0);
    }

    /* From "cc" (start of leaf 1), prev jumps to "bb" (leaf 0) */
    pt_seek(&c, b, 4);
    p = pt_prev(&c, &n);
    assert(p != NULL && n == 2 && memcmp(p, "bb", 2) == 0);
    assert(pt_offset(&c) == 2);

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_read_basic(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("hello world")));
    pt_Cursor c;
    char      buf[32];
    size_t    r;

    /* Read partial from start */
    pt_seek(&c, b, 0);
    r = pt_read(&c, buf, 5);
    assert(r == 5 && memcmp(buf, "hello", 5) == 0);
    assert(pt_offset(&c) == 5);

    /* Read remaining */
    r = pt_read(&c, buf, 32);
    assert(r == 6 && memcmp(buf, " world", 6) == 0);
    assert(pt_offset(&c) == 11);

    /* Read past end */
    r = pt_read(&c, buf, 32);
    assert(r == 0);

    /* Read from middle */
    pt_seek(&c, b, 6);
    r = pt_read(&c, buf, 5);
    assert(r == 5 && memcmp(buf, "world", 5) == 0);
    assert(pt_offset(&c) == 11);

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_read_cross(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(0, leafV(litV("aaaa"), litV("bbbb"), litV("cccc")));
    pt_Cursor c;
    char      buf[32];
    size_t    r;

    /* Read across first two pieces exactly */
    pt_seek(&c, b, 2);
    r = pt_read(&c, buf, 6);
    assert(r == 6 && memcmp(buf, "aabbbb", 6) == 0);
    assert(pt_offset(&c) == 8);

    /* Read across multiple pieces to end */
    pt_seek(&c, b, 6);
    r = pt_read(&c, buf, 32);
    assert(r == 6 && memcmp(buf, "bbcccc", 6) == 0);

    /* Read full tree */
    pt_seek(&c, b, 0);
    r = pt_read(&c, buf, 32);
    assert(r == 12);
    assert(memcmp(buf, "aaaabbbbcccc", 12) == 0);

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_read_full(void) {
    pt_State   *S = pt_open(&test_alloc, NULL);
    static char ref[128];
    pt_Blob     b;
    pt_Cursor   c;
    char        buf[256];
    size_t      r;
    int         k;

    for (k = 0; k < 128; ++k) ref[k] = (char)('A' + (k % 26));
    b = pt_from(S, ref, 128);
    pt_seek(&c, b, 0);
    r = pt_read(&c, buf, 256);
    assert(r == 128 && memcmp(buf, ref, 128) == 0);
    assert(pt_offset(&c) == 128);
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_read_empty(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_empty(S);
    pt_Cursor c;
    char      buf[8];
    size_t    r;

    pt_seek(&c, b, 0);
    r = pt_read(&c, buf, 5);
    assert(r == 0);

    /* pt_next/pt_prev on empty tree */
    {
        const char *p;
        size_t      n;
        p = pt_next(&c, &n);
        assert(p == NULL && n == 0);
        p = pt_prev(&c, &n);
        assert(p == NULL && n == 0);
    }

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_read_cursor_mid(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = pt_from(S, "abcdefghij", 10);
    pt_Cursor c;
    char      buf[16];
    size_t    r;

    /* Read from middle of content (not piece boundary) */
    pt_seek(&c, pt_nonnull(b), 3);
    r = pt_read(&c, buf, 4);
    assert(r == 4 && memcmp(buf, "defg", 4) == 0);
    assert(pt_offset(&c) == 7);

    /* Read remaining after mid-read */
    r = pt_read(&c, buf, 10);
    assert(r == 3 && memcmp(buf, "hij", 3) == 0);
    assert(pt_offset(&c) == 10);

    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

static void test_trav_deep(void) {
    pt_State *S = pt_open(&test_alloc, NULL);
    pt_Blob   b = treeV(
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
    assert(off == 16 && memcmp(fwd, "aabbccddeeffgghh", 16) == 0);
    pt_seek(&c, b, pt_bytes(b));
    while ((p = pt_prev(&c, &n)) != NULL) end -= n, memcpy(rev + end, p, n);
    assert(end == 0 && memcmp(rev, "aabbccddeeffgghh", 16) == 0);
    assert(pt_offset(&c) == 0);
    pt_release(b);
    assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
    pt_close(S);
}

/* ================= splice_brute: exhaustive enumeration ================= */

static void maketree(pt_State *S, pt_Cursor *C, size_t off) {
    static const char litbuf
            [] = "abcdefghijklmnopqrstuvwxyz0123456789"
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                 "abcdefghijklmnopqrstuvwxyz0123456789"
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcd";
    pt_Node *leaves[32], *inners[8], *inner1, *inner2, *inner3, *root;
    int      i, j, idx = 0;

    for (i = 0; i < 32; i++) {
        pt_Node *lf = (pt_Node *)ptP_alloc(S, &S->nodes);
        pt_Hole *h0 = (pt_Hole *)ptP_alloc(S, &S->holes);
        pt_Hole *h1 = (pt_Hole *)ptP_alloc(S, &S->holes);
        memset(lf, 0, sizeof(pt_Node));
        memset(h0, 0, sizeof(pt_Hole));
        memset(h1, 0, sizeof(pt_Hole));
        h0->data[0] = '#', h0->data[1] = '#';
        h1->data[0] = '#', h1->data[1] = '#';
        lf->child_count = 4;
        lf->children[0] = (pt_Node *)(litbuf + idx);
        lf->bytes[0] = 2;
        lf->children[1] = (pt_Node *)h0;
        lf->bytes[1] = 2;
        lf->mask |= (1u << 1);
        lf->children[2] = (pt_Node *)(litbuf + idx + 2);
        lf->bytes[2] = 2;
        lf->children[3] = (pt_Node *)h1;
        lf->bytes[3] = 2;
        lf->mask |= (1u << 3);
        idx += 4;
        leaves[i] = lf;
    }

    for (i = 0; i < 8; i++) {
        pt_Node *n = (pt_Node *)ptP_alloc(S, &S->nodes);
        memset(n, 0, sizeof(pt_Node));
        n->child_count = 4;
        for (j = 0; j < 4; j++) {
            n->children[j] = leaves[i * 4 + j];
            n->bytes[j] = ptN_sumbytes(leaves[i * 4 + j], 0, 4);
            if (leaves[i * 4 + j]->mask) n->mask |= (1u << j);
        }
        inners[i] = n;
    }

    inner1 = (pt_Node *)ptP_alloc(S, &S->nodes);
    memset(inner1, 0, sizeof(pt_Node));
    inner1->child_count = 4;
    for (j = 0; j < 4; j++) {
        inner1->children[j] = inners[j];
        inner1->bytes[j] = ptN_sumbytes(inners[j], 0, 4);
        if (inners[j]->mask) inner1->mask |= (1u << j);
    }

    inner2 = (pt_Node *)ptP_alloc(S, &S->nodes);
    memset(inner2, 0, sizeof(pt_Node));
    inner2->child_count = 2;
    for (j = 0; j < 2; j++) {
        inner2->children[j] = inners[4 + j];
        inner2->bytes[j] = ptN_sumbytes(inners[4 + j], 0, 4);
        if (inners[4 + j]->mask) inner2->mask |= (1u << j);
    }

    inner3 = (pt_Node *)ptP_alloc(S, &S->nodes);
    memset(inner3, 0, sizeof(pt_Node));
    inner3->child_count = 2;
    for (j = 0; j < 2; j++) {
        inner3->children[j] = inners[6 + j];
        inner3->bytes[j] = ptN_sumbytes(inners[6 + j], 0, 4);
        if (inners[6 + j]->mask) inner3->mask |= (1u << j);
    }

    root = (pt_Node *)ptP_alloc(S, &S->nodes);
    memset(root, 0, sizeof(pt_Node));
    root->child_count = 3;
    root->children[0] = inner1;
    root->bytes[0] = ptN_sumbytes(inner1, 0, 4);
    if (inner1->mask) root->mask |= (1u << 0);
    root->children[1] = inner2;
    root->bytes[1] = ptN_sumbytes(inner2, 0, 2);
    if (inner2->mask) root->mask |= (1u << 1);
    root->children[2] = inner3;
    root->bytes[2] = ptN_sumbytes(inner3, 0, 2);
    if (inner3->mask) root->mask |= (1u << 2);

    pt_seek(C, treeV(3, root), off);
    C->dirty = 1;
}

static void test_splice_brute(void) {
    int const         nb = 256;
    pt_State         *S = pt_open(&test_alloc, NULL);
    int               r, i, pos, del, ins;
    char              ref[256], expected[512], actual[512];
    pt_Cursor         C;
    size_t            epos, edel, expect_len, cursor_exp, nread;
    static const char alphabet
            [] = "abcdefghijklmnopqrstuvwxyz0123456789"
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                 "abcdefghijklmnopqrstuvwxyz0123456789"
                 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcd";
    for (i = 0; i < 64; i++) {
        ref[i * 4 + 0] = alphabet[i * 2 + 0];
        ref[i * 4 + 1] = alphabet[i * 2 + 1];
        ref[i * 4 + 2] = '#';
        ref[i * 4 + 3] = '#';
    }

    for (pos = 0; pos <= nb + 1; ++pos)
        for (del = 0; del <= nb + 1; ++del)
            for (ins = 0; ins <= 1; ++ins) {
                maketree(S, &C, (size_t)pos);
                epos = (size_t)pos < 256 ? (size_t)pos : 256;
                edel = (size_t)del < 256 - epos ? (size_t)del : 256 - epos;
                expect_len = 256 - edel + (ins ? 1u : 0u);
                cursor_exp = epos + (ins ? 1u : 0u);

                if (ins)
                    r = pt_splice(&C, (size_t)del, "!", 1);
                else
                    r = pt_splice(&C, (size_t)del, NULL, 0);
                assert(r == PT_OK);

                if (!pt_checktree(C.tree)) {
                    pt_log("FAIL pos=%d del=%d ins=%d\n", pos, del, ins);
                    pt_dumptree(C.tree, "after splice");
                    pt_checktree(C.tree);
                    assert(0);
                }

                if (!pt_checkcursor(&C, cursor_exp)) {
                    pt_log("splice pos=%d del=%d ins=%d off=%zu exp=%zu\n", pos,
                           del, ins, pt_offset(&C), cursor_exp);
                    assert(0);
                }

                if (ins) {
                    memcpy(expected, ref, epos);
                    expected[epos] = '!';
                    memcpy(expected + epos + 1, ref + epos + edel,
                           256 - epos - edel);
                } else {
                    memcpy(expected, ref, epos);
                    memcpy(expected + epos, ref + epos + edel,
                           256 - epos - edel);
                }

                pt_seek(&C, C.tree, 0);
                nread = pt_read(&C, actual, expect_len);
                if (nread != expect_len
                    || memcmp(actual, expected, expect_len) != 0) {
                    pt_log("splice pos=%d del=%d ins=%d content fail"
                           " nread=%zu exp=%zu\n",
                           pos, del, ins, nread, expect_len);
                    assert(0);
                }

                pt_release(C.tree);
                assert(S->nodes.live_obj == 0 && S->holes.live_obj == 0);
            }

    pt_close(S);
}

#define TESTS(X)                   \
    X(advance_brute)               \
    X(arena_dirty_break)           \
    X(arena_lazy)                  \
    X(arena_literal_cold)          \
    X(arena_literal_params)        \
    X(arena_reserve_full)          \
    X(arena_reserve_reuse)         \
    X(commit_bytes_invariant)      \
    X(commit_clean)                \
    X(commit_deep)                 \
    X(commit_deep2)                \
    X(commit_fillblock)            \
    X(commit_freshpage)            \
    X(commit_mixed)                \
    X(commit_no_merge)             \
    X(commit_reserve_leftover)     \
    X(commit_reserve_pages)        \
    X(commit_reservebuf_oom)       \
    X(commit_reservebuf_oom_multi) \
    X(commit_single_hole)          \
    X(commit_then_reseek)          \
    X(edit_append_full)            \
    X(edit_append_tail)            \
    X(edit_brute)                  \
    X(edit_commit_edit)            \
    X(edit_commit_roundtrip)       \
    X(edit_cow)                    \
    X(edit_del_only)               \
    X(edit_del_then_ins)           \
    X(edit_empty)                  \
    X(edit_fresh_boundary)         \
    X(edit_fresh_lit_mid)          \
    X(edit_mid_fit)                \
    X(edit_mid_split)              \
    X(edit_oom)                    \
    X(edit_params)                 \
    X(edit_prev_hole)              \
    X(edit_rollback)               \
    X(edit_split_tree)             \
    X(edit_type_sequence)          \
    X(edit_upmask)                 \
    X(from_basic)                  \
    X(insert_after)                \
    X(insert_basic)                \
    X(insert_before)               \
    X(insert_committed_split)      \
    X(insert_cow)                  \
    X(insert_deep)                 \
    X(insert_locend)               \
    X(insert_mid)                  \
    X(insert_multiversion)         \
    X(insert_oom_fork)             \
    X(insert_oom_reserve)          \
    X(insert_split_front)          \
    X(insert_split_leaf)           \
    X(insert_split_root)           \
    X(insert_brute)                \
    X(lifecycle)                   \
    X(merge_left)                  \
    X(merge_right)                 \
    X(next_basic)                  \
    X(next_levels1)                \
    X(null_params)                 \
    X(peekscratch_adjacent)        \
    X(peekscratch_params)          \
    X(peekscratch_roundtrip)       \
    X(piece_positions)             \
    X(prev_basic)                  \
    X(read_cursor_mid)             \
    X(prev_levels1)                \
    X(read_basic)                  \
    X(read_cross)                  \
    X(read_empty)                  \
    X(read_full)                   \
    X(read_params)                 \
    X(release_order)               \
    X(remove_across_leaves)        \
    X(remove_all)                  \
    X(remove_brute)                \
    X(remove_cow)                  \
    X(remove_cross)                \
    X(remove_deep_shrink)          \
    X(remove_fold_balance)         \
    X(remove_foldnode_balance)     \
    X(remove_hole_boundary)        \
    X(remove_hole_eraseleaf)       \
    X(remove_hole_mid)             \
    X(remove_hole_mixed)           \
    X(remove_hole_trim)            \
    X(remove_hole_whole)           \
    X(remove_merge_hole_full)      \
    X(remove_merge_hole_split)     \
    X(remove_merge_literal)        \
    X(remove_oom)                  \
    X(remove_params)               \
    X(remove_piece_whole)          \
    X(remove_release_order)        \
    X(remove_same_mid)             \
    X(remove_same_prefix)          \
    X(remove_same_suffix)          \
    X(remove_stitch_deep)          \
    X(remove_stitch_full)          \
    X(remove_stitch_overflow)      \
    X(remove_to_end)               \
    X(remove_trim_hole)            \
    X(rollback)                    \
    X(rollback_released_source)    \
    X(seek_empty)                  \
    X(splice_basic)                \
    X(splice_del0)                 \
    X(splice_brute)                \
    X(splice_null)                 \
    X(trav_deep)                   \
    X(trav_single)

#define X(name) {#name, test_##name},
PT_TEST_MAIN("piecetab tests")
#undef X
