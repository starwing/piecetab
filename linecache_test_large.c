#define LC_LEAF_FANOUT 8
#define LC_FANOUT      8
#define LC_PAGE_SIZE   4096
#define LC_STATIC_API

#include "linecache.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) { free(ptr); return NULL; }
    return realloc(ptr, nsize);
}

/* stateful scanner: returns 'val' for next 'count' calls, then 0 */
typedef struct { int cnt; unsigned val; } sscan_t;

static unsigned sscan(void *ud, size_t prev) {
    sscan_t *s = (sscan_t *)ud;
    (void)prev;
    if (s->cnt <= 0) return 0;
    s->cnt--;
    return s->val;
}

/* tree invariant checker */
static size_t check_node(
        const lc_Node *n, int l, int levels, int allow_empty) {
    int    i;
    size_t bsum = 0;
    assert(allow_empty || n->child_count > 0);
    assert(n->child_count <= LC_FANOUT);
    for (i = 0; i < (int)n->child_count; ++i) {
        if (l == levels || levels == 0)
            bsum += n->bytes[i];
        else {
            size_t cb = check_node(n->children[i], l + 1, levels, allow_empty);
            assert(cb == n->bytes[i]);
            bsum += cb;
        }
    }
    return bsum;
}

static void check_tree_allow_empty(const lc_Cache *c, int allow_empty) {
    if (c->root.child_count == 0) {
        assert(c->bytes == 0 && c->breaks == 0);
        return;
    }
    check_node(&c->root, 0, c->levels, allow_empty);
    assert(c->bytes == lcN_sumbytes(&c->root, 0, c->root.child_count));
    assert(c->breaks == lcN_sumbreaks(&c->root, 0, c->root.child_count));
}

static void check_tree(const lc_Cache *c) { check_tree_allow_empty(c, 0); }

/* cache builders for precise tree construction */
static lc_Node *leafV_(lc_State *S, unsigned head, ...) {
    lc_Leaf *leaf = (lc_Leaf *)lc_poolalloc(S, &S->leaves);
    va_list  ap;
    unsigned v = head, i = 0;
    assert(leaf);
    va_start(ap, head);
    do {
        assert(i < LC_LEAF_FANOUT);
        leaf->bytes[i++] = v;
        v = va_arg(ap, unsigned);
    } while (v != 0);
    va_end(ap);
    {
        lc_Node *n = (lc_Node *)lc_poolalloc(S, &S->nodes);
        assert(n);
        n->children[0] = (lc_Node *)leaf;
        n->bytes[0] = lcL_sumbytes(leaf, 0, i);
        n->breaks[0] = i;
        n->child_count = 1;
        return n;
    }
}
#define leafV(...) leafV_(S, __VA_ARGS__, 0)

static lc_Node *botV_(lc_State *S, lc_Node *head, ...) {
    lc_Node *n = (lc_Node *)lc_poolalloc(S, &S->nodes);
    va_list  ap;
    lc_Node *child = head;
    int      i = 0;
    assert(n);
    va_start(ap, head);
    do {
        assert(i < LC_FANOUT && child);
        n->children[i] = child;
        n->bytes[i] = child->bytes[0];
        n->breaks[i] = child->breaks[0];
        n->child_count = (unsigned short)(++i);
        child = va_arg(ap, lc_Node *);
    } while (child != NULL);
    va_end(ap);
    return n;
}
#define botV(...) botV_(S, __VA_ARGS__, NULL)

static lc_Node *innerV_(lc_State *S, lc_Node *head, ...) {
    lc_Node *n = (lc_Node *)lc_poolalloc(S, &S->nodes);
    va_list  ap;
    lc_Node *child = head;
    int      i = 0;
    assert(n);
    va_start(ap, head);
    do {
        assert(i < LC_FANOUT && child);
        n->children[i] = child;
        n->bytes[i] = lcN_sumbytes(child, 0, child->child_count);
        n->breaks[i] = lcN_sumbreaks(child, 0, child->child_count);
        n->child_count = (unsigned short)(++i);
        child = va_arg(ap, lc_Node *);
    } while (child != NULL);
    va_end(ap);
    return n;
}
#define innerV(...) innerV_(S, __VA_ARGS__, NULL)

static lc_Cache *cacheV(lc_State *S, unsigned levels, lc_Node *root) {
    lc_Cache *c = lc_newtree(S);
    unsigned  i;
    assert(c && root->child_count <= LC_FANOUT);
    c->levels = levels;
    c->root = *root;
    lc_poolfree(&S->nodes, root);
    c->bytes = 0; c->breaks = 0;
    for (i = 0; i < c->root.child_count; i++)
        c->bytes += c->root.bytes[i], c->breaks += c->root.breaks[i];
    check_tree_allow_empty(c, 1);
    return c;
}

/* ================================================================ */
/*  Test: foldleaf cursor switch (line 723, dl>0)                   */
/* ================================================================ */
static void test_cov_l723(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    sscan_t   ss = { 20, 1 };
    lc_Cursor cur;
    lc_scan(c, sscan, &ss);
    check_tree(c);
    lc_seek(&cur, c, 6);
    lc_splice(&cur, 9, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* scanner that returns alternating values for precise tree shape */
typedef struct {
    int       cnt[4];
    unsigned  val[4];
    int       phase;
} rscan_t;

static unsigned rscan(void *ud, size_t prev) {
    rscan_t *r = (rscan_t *)ud;
    (void)prev;
    for (; r->phase < 4; r->phase++) {
        if (r->cnt[r->phase] > 0) {
            r->cnt[r->phase]--;
            return r->val[r->phase];
        }
    }
    return 0;
}

/* Large tree scan test: 512+256=768 segments → innerV0+innerV1 */
static void test_cov_l750_l752_scan(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    rscan_t   rs = { { 512, 256, 0 }, { 1, 1, 0 }, 0 };
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

static void test_cov_l752(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    rscan_t   rs = { { 768, 0 }, { 1, 0 }, 0 };
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

static void test_cov_l750(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    rscan_t   rs = { { 768, 0 }, { 1, 0 }, 0 };
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

/* ================================================================ */
/*  foldnode cursor adjust: line 750+752 (dn<0, *ns!=o)             */
/*  inner0=4 botVs, inner1=8 botVs. L at inner1 botV0 offset 8     */
/*  del=3 → R at inner1 botV1 → both under inner1, div=1.          */
/*  Phase 1: botV0+bV1 merge → inner1.cc=7.                        */
/*  Phase 2 foldnode(L,0): i=1=c_c-1 → ns-=1. *ns=inner0, o=inner1.*/
/*  cl=4, cr=7, dn=4-6=-2<0. path=0 < -dn=2 → line 750/751/752.   */
static void test_cov_l750_cacheV(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *inner0 = innerV(
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)));
    lc_Node  *inner1 = innerV(
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)),
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)));
    lc_Node  *root  = innerV(inner0, inner1);
    lc_Cache *c     = cacheV(S, 2, root);
    lc_Cursor cur;
    check_tree(c);
    lc_seek(&cur, c, 8);
    lc_splice(&cur, 3, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

/* ================================================================ */
/*  foldnode cursor adjust: line 753+754 (dn>0, *ns==o)             */
/*  inner0=8 botVs, inner1=2 botVs. L at inner0 botV6 offset 12    */
/*  del=3 → R at inner0 botV7 → both under inner0, div=1.          */
/*  Phase 1: botV6+bV7 merge → inner0.cc=7.                        */
/*  Phase 2 foldnode(L,0): i=0. *ns=o=inner0.                      */
/*  cl=7, cr=2, dn=7-5=2>0. path=6 >= cl-dn=5 → line 753/754.     */
static void test_cov_l752_cacheV(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *inner0 = innerV(
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)),
            botV(leafV(2)), botV(leafV(2)), botV(leafV(2)), botV(leafV(2)));
    lc_Node  *inner1 = innerV(botV(leafV(2)), botV(leafV(2)));
    lc_Node  *root  = innerV(inner0, inner1);
    lc_Cache *c     = cacheV(S, 2, root);
    lc_Cursor cur;
    check_tree(c);
    lc_seek(&cur, c, 12);
    lc_splice(&cur, 3, 0);
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}

#define TESTS(X)                \
    X(cov_l723)                 \
    X(cov_l750_l752_scan)       \
    X(cov_l752)                 \
    X(cov_l750)                 \
    X(cov_l750_cacheV)          \
    X(cov_l752_cacheV)

int main(int argc, char *argv[]) {
    typedef struct entry { const char *name; void (*fn)(void); } entry_t;
    const entry_t entries[] = {
#define X(name) {#name, test_##name},
        TESTS(X)
#undef  X
        {NULL, NULL},
    };
    int i, j;
    printf("=== linecache large-fanout tests ===\n");
    if (argc == 1) {
        const entry_t *e = entries;
        while (e->name) { e->fn(); printf("  %s OK\n", e->name); ++e; }
        printf("\nAll tests passed!\n");
        return 0;
    }
    for (i = 1; i < argc; ++i) {
        const char *name = argv[i];
        size_t      len = strlen(name);
        int         found = 0, only = *name == '@';
        if (only) name++, len--;
        for (j = 0; entries[j].name; ++j) {
            if (strlen(entries[j].name) >= len
                && strncmp(name, entries[j].name, len) == 0) {
                printf("- %s\n", entries[j].name);
                entries[j].fn();
                printf("  %s OK\n", entries[j].name);
                found = 1;
                if (only) break;
            }
        }
        if (!found) { fprintf(stderr, "Unknown test: %s\n", name); return 1; }
    }
    return 0;
}
