/* at_tests.h -- anchortree-specific test utilities.
 *
 * Shared by anchortree test sources (multi-FANOUT).  Public utilities
 * (runner, asserts, allocators) live in tests.h; only anchortree-specific
 * helpers live here.  Test sources define AT_FANOUT etc. and include
 * ../anchortree.h before this header.
 */

#ifndef AT_TESTS_H
#define AT_TESTS_H

#include "tests.h"

typedef struct {
    at_Edge *edge[512];
    size_t   pos[512];
    int      n;
} at_TestSeq;

/* drain/refill pool freelists, for OOM tests */
typedef struct {
    void  *chain;
    size_t count;
} at_Drain;

TEST_STATIC at_Drain at_drainpool(at_Pool *p) {
    at_Drain d;
    d.chain = p->freed, d.count = p->freed_obj;
    p->freed = NULL, p->freed_obj = 0;
    return d;
}

TEST_STATIC void at_refillpool(at_Pool *p, at_Drain d) {
    void **pp = &d.chain;
    while (*pp) pp = (void **)*pp;
    *pp = p->freed;
    p->freed = d.chain, p->freed_obj += d.count;
}

/* collect all tree edges in coordinate order with their absolute positions */
TEST_STATIC void at_seq_node(
        const at_Node *n, int level, size_t *base, at_TestSeq *s) {
    int i;
    if (level == 0) {
        for (i = 0; i < atN_cc(n); ++i) {
            assertok(s->n < 512);
            s->edge[s->n] = atN_edge(n, i);
            s->pos[s->n] = *base + n->bytes[i];
            s->n += 1, *base += n->bytes[i];
        }
        return;
    }
    for (i = 0; i < atN_cc(n); ++i)
        at_seq_node((at_Node *)n->children[i], level - 1, base, s);
}

TEST_STATIC int at_seq(at_Tree *T, at_TestSeq *s) {
    size_t base = 0;
    s->n = 0;
    at_seq_node(&T->root, T->levels, &base, s);
    return s->n;
}

/* strict cursor invariant checker */
TEST_STATIC int at_checkcursor(const at_Cursor *C, size_t expected_off) {
    size_t   bsum = 0;
    int      i, l;
    at_Node *p;
    check(at_offset(C) == expected_off,
          "[atc] OFFSET mismatch off=%lu expected=%lu\n", test_lu(at_offset(C)),
          test_lu(expected_off));
    if (C->tree->bytes == 0) {
        check(C->off == 0 && C->paths[0] == C->tree->root.children,
              "[atc] EMPTY off=%lu poff=%lu\n", test_lu(C->off),
              test_lu(C->poff));
        check(C->poff == expected_off, "[atc] EMPTY poff=%lu expected=%lu\n",
              test_lu(C->poff), test_lu(expected_off));
        return 1;
    }
    for (l = 0; l <= C->tree->levels; ++l) {
        p = l ? (at_Node *)*(C->paths[l - 1]) : &C->tree->root;
        i = (int)(C->paths[l] - p->children);
        check(i >= 0 && i < atN_cc(p), "[atc] PATHS[%d] invalid idx=%d cc=%d\n",
              l, i, atN_cc(p));
        check(C->paths[l] == &p->children[i],
              "[atc] PATHS[%d] invalid ptr=%p expected=%p\n", l,
              (void *)C->paths[l], (void *)&p->children[i]);
        for (i = 0; i < (int)(C->paths[l] - p->children); ++i)
            bsum += p->bytes[i];
    }
    p = C->tree->levels ? (at_Node *)*(C->paths[C->tree->levels - 1])
                        : &C->tree->root;
    i = (int)(C->paths[C->tree->levels] - p->children);
    check(C->off == bsum, "[atc] OFF mismatch off=%lu sum=%lu\n",
          test_lu(C->off), test_lu(bsum));
    if (expected_off >= C->tree->bytes)
        check(C->poff >= p->bytes[i],
              "[atc] VIRTUAL/END poff=%lu len=%lu off=%lu bytes=%lu\n",
              test_lu(C->poff), test_lu(p->bytes[i]), test_lu(C->off),
              test_lu(C->tree->bytes));
    else
        check(C->poff <= p->bytes[i],
              "[atc] RANGE poff=%lu len=%lu off=%lu bytes=%lu\n",
              test_lu(C->poff), test_lu(p->bytes[i]), test_lu(C->off),
              test_lu(C->tree->bytes));
    return 1;
}

/* tree invariant checker: byte sums, parent/index links, no zero-length
 * leaf slots, and minimum occupancy for every non-root node.  Test-only
 * helper; not part of the anchortree API. */
TEST_STATIC size_t atN_checknode(
        const at_Tree *T, const at_Node *n, int level, int root, int *ok) {
    size_t sum = 0;
    int    i;
    (void)T;
    if (n->child_count > AT_FANOUT || (!root && n->child_count < atN_min())) {
        *ok = 0;
        return 0;
    }
    if (level == 0) {
        for (i = 0; i < atN_cc(n); ++i) {
            at_Edge *e = atN_edge(n, i);
            if (n->bytes[i] == 0 || e->parent != n || e->idx != i) *ok = 0;
            sum += n->bytes[i];
        }
    } else {
        for (i = 0; i < atN_cc(n); ++i) {
            at_Node *c = (at_Node *)n->children[i];
            size_t   cs = atN_checknode(T, c, level - 1, 0, ok);
            if (n->bytes[i] != cs || c->parent != n || c->idx != i) *ok = 0;
            sum += cs;
        }
    }
    return sum;
}

TEST_STATIC int at_checktree(const at_Tree *T) {
    size_t sum;
    int    ok = 1;
    if (T == NULL) return 0;
    if (T->levels >= AT_MAX_LEVEL) return 0;
    sum = atN_checknode(T, &T->root, T->levels, 1, &ok);
    if (T->bytes != sum) ok = 0;
    return ok;
}

#endif /* AT_TESTS_H */
