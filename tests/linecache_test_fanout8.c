#define LC_LEAF_FANOUT 8
#define LC_FANOUT      8
#define LC_PAGE_SIZE   4096
#undef LC_IMPLEMENTATION
#define LC_IMPLEMENTATION

#include "lc_tests.h"

/* foldleaf cursor switch (dl>0, *ls==o, left->right) */
TEST(foldleaf_cursor_switch) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    lc_rscanV(c, 20, 1);
    assert(lc_checktree(c));
    lc_seek(&C, c, 6);
    lc_splice(&C, 9, 0);
    assert(lc_checkcursor(&C, 6));
    ;
    assert(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

/* Large tree scan test: foldnode cursor adjust via scan-built tree */
TEST(foldnode_cursor_scan) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    lc_rscanV(c, 512, 1, 256, 1);
    assert(lc_checktree(c));
    lc_seek(&C, c, 100);
    lc_splice(&C, 200, 0);
    assert(lc_checkcursor(&C, 100));
    ;
    assert(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

/* foldnode cursor right (dn>0, *ns==o, scan-built tree) */
TEST(foldnode_cursor_right) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    lc_rscanV(c, 768, 1);
    assert(c->levels >= 1);
    assert(lc_checktree(c));
    lc_seek(&C, c, 4);
    lc_splice(&C, 4, 0);
    assert(lc_checktree(c));
    assert(lc_checkcursor(&C, 4));
    ;
    lc_delcache(S, c);
    lc_close(S);
}

/* foldnode cursor left (dn<0, *ns!=o, scan-built tree) */
TEST(foldnode_cursor_left) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;
    lc_rscanV(c, 768, 1);
    assert(c->levels >= 1);
    assert(lc_checktree(c));
    lc_seek(&C, c, 2);
    lc_splice(&C, 8, 0);
    assert(lc_checkcursor(&C, 2));
    ;
    assert(lc_checktree(c));
    lc_delcache(S, c);
    lc_close(S);
}

/* foldnode cursor left (dn<0, *ns!=o, cacheV-built tree) */
TEST(foldnode_cursor_left_cacheV) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2)), botV(leafV(2)), botV(leafV(2)),
                          botV(leafV(2))),
                   innerV(botV(leafV(2)), botV(leafV(2)), botV(leafV(2)),
                          botV(leafV(2)), botV(leafV(2)), botV(leafV(2)),
                          botV(leafV(2)), botV(leafV(2)))));
    lc_Cursor C;
    lc_seek(&C, c, 8);
    lc_splice(&C, 3, 0);
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&C, 8));
    ;
    lc_delcache(S, c);
    lc_close(S);
}

/* foldnode cursor right (dn>0, *ns==o, cacheV-built tree) */
TEST(foldnode_cursor_right_cacheV) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 2,
            innerV(innerV(botV(leafV(2)), botV(leafV(2)), botV(leafV(2)),
                          botV(leafV(2)), botV(leafV(2)), botV(leafV(2)),
                          botV(leafV(2)), botV(leafV(2))),
                   innerV(botV(leafV(2)), botV(leafV(2)))));
    lc_Cursor C;
    lc_seek(&C, c, 12);
    lc_splice(&C, 3, 0);
    assert(lc_checktree_allow_empty(c, 1));
    assert(lc_checkcursor(&C, 12));
    ;
    lc_delcache(S, c);
    lc_close(S);
}


#include "linecache_test_fanout8.gen.inc"
