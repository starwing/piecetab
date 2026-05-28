#define LC_LEAF_FANOUT 8
#define LC_FANOUT      8
#define LC_PAGE_SIZE   4096
#define LC_STATIC_API

#include "lc_tests.h"

/* foldleaf cursor switch (dl>0, *ls==o, left->right) */
static void test_foldleaf_cursor_switch(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    sscan_t   ss = {20, 1};
    lc_Cursor cur;
    lc_scan(c, sscan, &ss);
    check_tree(c);
    lc_seek(&cur, c, 6);
    lc_splice(&cur, 9, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* Large tree scan test: foldnode cursor adjust via scan-built tree */
static void test_foldnode_cursor_scan(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    rscan_t   rs = {{512, 256, 0}, {1, 1, 0}, 0};
    int       r;
    r = lc_scan(c, rscan, &rs);
    assert(r == LC_OK);
    check_tree(c);
    lc_seek(&cur, c, 100);
    lc_splice(&cur, 200, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode cursor right (dn>0, *ns==o, scan-built tree) */
static void test_foldnode_cursor_right(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    rscan_t   rs = {{768, 0}, {1, 0}, 0};
    int       r;
    assert(c);
    r = lc_scan(c, rscan, &rs);
    assert(r == LC_OK);
    assert(c->levels >= 1);
    check_tree(c);
    lc_seek(&cur, c, 4);
    lc_splice(&cur, 4, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode cursor left (dn<0, *ns!=o, scan-built tree) */
static void test_foldnode_cursor_left(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    rscan_t   rs = {{768, 0}, {1, 0}, 0};
    int       r;
    assert(c);
    r = lc_scan(c, rscan, &rs);
    assert(r == LC_OK);
    assert(c->levels >= 1);
    check_tree(c);
    lc_seek(&cur, c, 2);
    lc_splice(&cur, 8, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode cursor left (dn<0, *ns!=o, cacheV-built tree) */
static void test_foldnode_cursor_left_cacheV(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *inner0 = innerV(
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)));
    lc_Node *inner1 = innerV(
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)),
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)));
    lc_Node  *root = innerV(inner0, inner1);
    lc_Cache *c = cacheV(S, 2, root);
    lc_Cursor cur;
    check_tree(c);
    lc_seek(&cur, c, 8);
    lc_splice(&cur, 3, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* foldnode cursor right (dn>0, *ns==o, cacheV-built tree) */
static void test_foldnode_cursor_right_cacheV(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *inner0 = innerV(
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)),
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)));
    lc_Node  *inner1 = innerV(botV(leafV(2)), botV(leafV(2)));
    lc_Node  *root = innerV(inner0, inner1);
    lc_Cache *c = cacheV(S, 2, root);
    lc_Cursor cur;
    check_tree(c);
    lc_seek(&cur, c, 12);
    lc_splice(&cur, 3, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

#define TESTS(X)                      \
    X(foldleaf_cursor_switch)          \
    X(foldnode_cursor_scan)            \
    X(foldnode_cursor_right)           \
    X(foldnode_cursor_left)            \
    X(foldnode_cursor_left_cacheV)     \
    X(foldnode_cursor_right_cacheV)

#define X(name) {#name, test_##name},
LC_TEST_MAIN("linecache large-fanout tests")
#undef X
