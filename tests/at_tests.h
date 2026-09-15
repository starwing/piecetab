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
    size_t   span[512];
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
            s->span[s->n] = n->bytes[i];
            s->pos[s->n] = *base;
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
    if (atN_cc(&C->tree->root) == 0) {
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
        check(C->poff < p->bytes[i],
              "[atc] RANGE poff=%lu len=%lu off=%lu bytes=%lu\n",
              test_lu(C->poff), test_lu(p->bytes[i]), test_lu(C->off),
              test_lu(C->tree->bytes));
    return 1;
}

/* tree invariant checker: byte sums, parent/index links, at most one
 * zero-length leaf slot (the tail sentinel, always last), and minimum
 * occupancy for every non-root node.  Test-only helper. */
TEST_STATIC size_t atN_checknode(
        const at_Tree *T, const at_Node *n, int level, int root, int *ok) {
    size_t sum = 0;
    int    i;
    (void)T;
    if (atN_cc(n) > AT_FANOUT || (!root && atN_cc(n) < atN_min())) {
        *ok = 0;
        return 0;
    }
    for (i = 0; i < atN_cc(n); ++i) {
        if (level == 0) {
            at_Edge *e = atN_edge(n, i);
            if (e->parent != n || e->idx != i) *ok = 0;
            sum += n->bytes[i];
        } else {
            at_Node *c = (at_Node *)n->children[i];
            size_t   cs = atN_checknode(T, c, level - 1, 0, ok);
            if (n->bytes[i] != cs || c->parent != n || c->idx != i) *ok = 0;
            sum += cs;
        }
    }
    return sum;
}

TEST_STATIC int at_checktree(const at_Tree *T) {
    at_TestSeq s;
    size_t     sum;
    int        i, ok = 1;
    if (T == NULL) return 0;
    if (T->levels >= AT_MAX_LEVEL) return 0;
    sum = atN_checknode(T, &T->root, T->levels, 1, &ok);
    if (T->bytes != sum) ok = 0;
    if (atN_cc(&T->root) == 0 && T->bytes != 0) ok = 0;
    if (T->bytes == 0 && atN_cc(&T->root) > 1) ok = 0;
    if (atN_cc(&T->root) == 1 && T->levels != 0) ok = 0;
    if (!ok) return 0;
    at_seq((at_Tree *)T, &s);
    for (i = 0; i + 1 < s.n; ++i)
        if (s.span[i] == 0) ok = 0;
    return ok;
}

/* endpoint-chain helpers */

TEST_STATIC int atO_listcount(at_Tree *T, at_Point *head) {
    at_Point p = *head;
    int      n = 0;
    while (p.id != 0 && n <= (int)(T->idcap * 2u + 4u)) {
        at_Anchor *a = &T->ids[p.id];
        p = p.is_end ? a->enext : a->snext;
        n += 1;
    }
    return n;
}

TEST_STATIC int atO_push(
        at_Tree *T, at_Point *head, at_Id id, int ns, int end) {
    at_Anchor *a = &T->ids[id];
    at_Point   slot;
    slot.id = id, slot.is_end = (unsigned)end;
    a->ns = (unsigned)ns & 0x1fffffffu;
    if (end)
        a->enext = *head;
    else
        a->snext = *head;
    *head = slot;
    return AT_OK;
}

TEST_STATIC void atO_remove(at_Tree *T, at_Point *head, at_Id id, int end) {
    at_Point   p = *head, prev, next;
    at_Anchor *a, *pa;
    prev.id = 0, prev.is_end = 0;
    while (p.id != 0 && (p.id != id || p.is_end != (unsigned)end)) {
        a = &T->ids[p.id];
        prev = p;
        p = p.is_end ? a->enext : a->snext;
    }
    if (p.id == 0) return;
    a = &T->ids[p.id];
    next = p.is_end ? a->enext : a->snext;
    if (prev.id == 0) {
        *head = next;
        return;
    }
    pa = &T->ids[prev.id];
    if (prev.is_end)
        pa->enext = next;
    else
        pa->snext = next;
}

TEST_STATIC int atO_sideof(at_Tree *T, at_Edge *e, at_Id id, int end) {
    at_Point p = e->left;
    while (p.id != 0) {
        at_Anchor *a = &T->ids[p.id];
        if (p.id == id && p.is_end == (unsigned)end) return 0;
        p = p.is_end ? a->enext : a->snext;
    }
    p = e->right;
    while (p.id != 0) {
        at_Anchor *a = &T->ids[p.id];
        if (p.id == id && p.is_end == (unsigned)end) return 1;
        p = p.is_end ? a->enext : a->snext;
    }
    return -1;
}

TEST_STATIC int at_counttag(at_Tree *T, at_Point *head, at_Id id, int end) {
    at_Point p = *head;
    int      n = 0;
    while (p.id != 0 && n <= (int)(T->idcap * 2u + 4u)) {
        at_Anchor *a = &T->ids[p.id];
        if (p.id == id && p.is_end == (unsigned)end) n += 1;
        p = p.is_end ? a->enext : a->snext;
    }
    return n;
}

TEST_STATIC int at_checkbucket(at_Tree *T, at_Edge *e, int side) {
    at_Point p = side ? e->right : e->left;
    int      n = 0;
    while (p.id != 0) {
        at_Anchor *a;
        if (p.id == 0 || T->ids == NULL || p.id >= T->idcap) return 0;
        a = &T->ids[p.id];
        if (a->end == NULL) return 0;
        if (side != atO_side(a->gravity, p.is_end)) return 0;
        if ((p.is_end ? a->end : a->start) != e) return 0;
        p = p.is_end ? a->enext : a->snext;
        n += 1;
        if (n > (int)(T->idcap * 2u + 8u)) return 0;
    }
    return 1;
}

TEST_STATIC int at_checkchains(at_Tree *T) {
    at_TestSeq s;
    at_Id      id;
    int        i, side;
    if (T == NULL || !at_checktree(T)) return 0;
    at_seq(T, &s);
    for (i = 0; i < s.n; ++i) {
        if (!at_checkbucket(T, s.edge[i], 0)) return 0;
        if (!at_checkbucket(T, s.edge[i], 1)) return 0;
    }
    if (T->ids == NULL) return 1;
    for (id = 1; id < T->idcap; ++id) {
        at_Anchor *a = &T->ids[id];
        if (a->end == NULL) continue;
        if (a->start == NULL || a->end == NULL) return 0;
        side = atO_side(a->gravity, 0);
        if (at_counttag(T, side ? &a->start->right : &a->start->left, id, 0)
            != 1)
            return 0;
        side = atO_side(a->gravity, 1);
        if (at_counttag(T, side ? &a->end->right : &a->end->left, id, 1) != 1)
            return 0;
    }
    return 1;
}

#endif /* AT_TESTS_H */
