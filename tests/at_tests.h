/* at_tests.h -- anchortree-specific test utilities.
 *
 * Shared by anchortree test sources (multi-FANOUT) and by fuzz/at_fuzz.c.
 * Public utilities (runner, asserts, allocators) live in tests.h.  Test
 * sources define AT_FANOUT etc. and include ../anchortree.h before this
 * header.
 *
 * Tests drive the public API only.  The helpers below read or write tree
 * internals directly for exact setup and invariant checking; none of
 * them call library-internal functions.
 */

#ifndef AT_TESTS_H
#define AT_TESTS_H

#include "tests.h"

typedef struct {
    size_t L;
    size_t del;
    size_t ins;
} att_Splice;

typedef struct {
    at_Id  id;
    size_t start;
    size_t end;
    int    gravity;
} att_Model;

TEST_STATIC size_t att_model_bound(const att_Model *m, int n) {
    size_t bound = 0;
    int    i;
    for (i = 0; i < n; ++i) {
        if (m[i].start > bound) bound = m[i].start;
        if (m[i].end > bound) bound = m[i].end;
    }
    return bound;
}

TEST_STATIC size_t att_model_collapse(size_t p, size_t L, size_t d, size_t R) {
    if (p > L && p <= R) return L;
    return p > R ? p - d : p;
}

TEST_STATIC size_t att_model_insert(size_t p, size_t L, size_t ins, int right) {
    if (p == L) return right ? p + ins : p;
    return p > L ? p + ins : p;
}

TEST_STATIC void att_model_splice(att_Model *m, int n, const att_Splice *w) {
    size_t R, bound = att_model_bound(m, n);
    int    i;
    if (w->del && w->L < bound) {
        R = w->L + w->del > bound ? bound : w->L + w->del;
        for (i = 0; i < n; ++i) {
            m[i].start = att_model_collapse(m[i].start, w->L, w->del, R);
            m[i].end = att_model_collapse(m[i].end, w->L, w->del, R);
        }
    }
    if (w->ins) {
        for (i = 0; i < n; ++i) {
            int right0 = m[i].gravity == AT_FIXED || m[i].gravity == AT_SHIFT;
            int right1 = m[i].gravity == AT_GROW || m[i].gravity == AT_SHIFT;
            m[i].start = att_model_insert(m[i].start, w->L, w->ins, right0);
            m[i].end = att_model_insert(m[i].end, w->L, w->ins, right1);
        }
    }
}

TEST_STATIC int att_model_ok(at_Tree *T, const att_Model *m, int n) {
    size_t x, y;
    int    i;
    for (i = 0; i < n; ++i) {
        if (at_range(T, m[i].id, &x, &y) != AT_OK) return 0;
        if (x != m[i].start || y != m[i].end) {
            test_log(
                    "model mismatch id=%u gravity=%d got=%lu..%lu "
                    "want=%lu..%lu\n",
                    m[i].id, m[i].gravity, test_lu(x), test_lu(y),
                    test_lu(m[i].start), test_lu(m[i].end));
            return 0;
        }
    }
    return 1;
}

typedef struct {
    void  *chain;
    size_t count;
} at_Drain;

TEST_STATIC at_Drain att_drainpool(at_Pool *p) {
    at_Drain d;
    d.chain = p->freed, d.count = p->freed_obj;
    p->freed = NULL, p->freed_obj = 0;
    return d;
}

/* Checker sequence capacity.  The 512-slot draft aborted on multi-level
 * fanout-32 trees (>512 edges); 4096 covers att_splitchild_edge's
 * 2048-anchor setup budget with 2x headroom.  Each at_Seq costs
 * AT_SEQ_CAP * 24 bytes on the stack, and at most two checker Seq
 * instances nest (att_checkchains -> att_checktree). */
#ifndef AT_SEQ_CAP
#define AT_SEQ_CAP 4096
#endif

typedef struct {
    at_Edge *edge[AT_SEQ_CAP];
    size_t   pos[AT_SEQ_CAP];
    size_t   span[AT_SEQ_CAP];
    int      n;
} at_Seq;

TEST_STATIC void att_seqnode(const at_Node *n, int lv, size_t *b, at_Seq *s) {
    int i;
    if (lv == 0) {
        for (i = 0; i < atN_cc(n); ++i) {
            assertok(s->n < AT_SEQ_CAP);
            s->edge[s->n] = atN_edge(n, i);
            s->span[s->n] = n->bytes[i];
            s->pos[s->n] = *b;
            s->n += 1, *b += n->bytes[i];
        }
    } else {
        for (i = 0; i < atN_cc(n); ++i)
            att_seqnode((at_Node *)n->children[i], lv - 1, b, s);
    }
}

TEST_STATIC int att_seq(const at_Tree *T, at_Seq *s) {
    size_t base = 0;
    s->n = 0;
    att_seqnode(&T->root, T->levels, &base, s);
    return s->n;
}

TEST_STATIC int att_checkcpath(const at_Cursor *C, int l, size_t *sum) {
    const at_Node *p = l ? (const at_Node *)*(C->paths[l - 1]) : &C->tree->root;
    int            j, i = (int)(C->paths[l] - p->children);
    check(i >= 0 && i < atN_cc(p), "[atc] PATHS[%d] invalid idx=%d cc=%d\n", l,
          i, atN_cc(p));
    check(C->paths[l] == &p->children[i],
          "[atc] PATHS[%d] invalid ptr=%p expected=%p\n", l,
          (const void *)C->paths[l], (const void *)&p->children[i]);
    for (j = 0; j < i; ++j) *sum += p->bytes[j];
    return 1;
}

TEST_STATIC int att_checkcpoff(const at_Cursor *C, size_t expected_off) {
    const at_Node *p;
    size_t         bytes = at_bytes(C->tree);
    int            i;
    p = C->tree->levels ? (const at_Node *)*(C->paths[C->tree->levels - 1])
                        : &C->tree->root;
    i = (int)(C->paths[C->tree->levels] - p->children);
    if (expected_off >= bytes)
        check(C->poff >= p->bytes[i],
              "[atc] VIRTUAL/END poff=%lu len=%lu off=%lu bytes=%lu\n",
              test_lu(C->poff), test_lu(p->bytes[i]), test_lu(C->off),
              test_lu(bytes));
    else
        check(C->poff < p->bytes[i],
              "[atc] RANGE poff=%lu len=%lu off=%lu bytes=%lu\n",
              test_lu(C->poff), test_lu(p->bytes[i]), test_lu(C->off),
              test_lu(bytes));
    return 1;
}

TEST_STATIC int att_checkcursor(const at_Cursor *C, size_t expected_off) {
    size_t sum = 0;
    int    l;
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
    for (l = 0; l <= C->tree->levels; ++l)
        check(att_checkcpath(C, l, &sum), "[atc] PATHS[%d] invalid\n", l);
    check(C->off == sum, "[atc] OFF mismatch off=%lu sum=%lu\n",
          test_lu(C->off), test_lu(sum));
    return att_checkcpoff(C, expected_off);
}

TEST_STATIC int att_empty(const at_Edge *e) {
    return e->left.id == 0 && e->right.id == 0;
}

TEST_STATIC int att_side(int g, int end) {
    if (end) return g == AT_GROW || g == AT_SHIFT;
    return g == AT_FIXED || g == AT_SHIFT;
}

TEST_STATIC at_Node *att_lastleaf(const at_Tree *T) {
    at_Node *n = (at_Node *)&T->root;
    int      l;
    for (l = T->levels; l > 0; --l) n = (at_Node *)n->children[atN_cc(n) - 1];
    return n;
}

TEST_STATIC at_Edge *att_leafedge(const at_Tree *T, size_t pos, size_t *poff) {
    const at_Node *n = &T->root;
    size_t         off = pos;
    int            i, l = T->levels;
    if (atN_cc(n) == 0) return NULL;
    if (pos == at_bytes(T)) {
        for (; l > 0; --l) n = n->children[atN_cc(n) - 1];
        i = atN_cc(n) - 1, *poff = n->bytes[i];
        return atN_edge(n, i);
    }
    for (; l > 0; --l) {
        for (i = 0; i < atN_cc(n) && off >= n->bytes[i]; ++i)
            off -= n->bytes[i];
        assert(i < atN_cc(n));
        n = n->children[i];
    }
    for (i = 0; i < atN_cc(n) && off >= n->bytes[i]; ++i) off -= n->bytes[i];
    assert(i < atN_cc(n));
    *poff = off;
    return atN_edge(n, i);
}

TEST_STATIC int att_setup_findedge(const at_Tree *T, size_t p, at_Edge **pe) {
    at_Edge *e;
    size_t   poff = 0;
    if (T == NULL || pe == NULL || atN_cc(&T->root) == 0) return 0;
    if (p > at_bytes(T)) return 0;
    e = att_leafedge(T, p, &poff);
    if (e == NULL || poff != 0) return 0;
    *pe = e;
    return 1;
}

TEST_STATIC int att_setup_edge(at_Tree *T, size_t pos, at_Edge **out) {
    at_Cursor C;
    at_Id     id;
    int       r;
    if (T == NULL || out == NULL) return AT_ERRPARAM;
    if ((r = at_seek(&C, T, pos)) != AT_OK) return r;
    if ((r = at_newanchor(&C, 0, 0, AT_LOCK, &id)) != AT_OK) return r;
    return att_setup_findedge(T, pos, out) ? AT_OK : AT_ERRPARAM;
}

TEST_STATIC int att_setup_dropedge(at_Tree *T, size_t pos) {
    at_Edge *e;
    at_Id    id;
    if (T == NULL || atN_cc(&T->root) == 0 || pos > at_bytes(T))
        return AT_ERRPARAM;
    if (!att_setup_findedge(T, pos, &e)) return AT_ERRPARAM;
    for (id = 1; id < T->idcap; ++id) {
        at_Anchor *a = &T->ids[id];
        if (a->end != NULL && a->start == e && a->end == e)
            return at_delanchor(T, id);
    }
    return AT_ERRPARAM;
}

TEST_STATIC int att_setup_insert(at_Tree *T, size_t pos, size_t ins) {
    at_Cursor C;
    int       r;
    if (T == NULL) return AT_ERRPARAM;
    if ((r = at_seek(&C, T, pos)) != AT_OK) return r;
    return at_splice(&C, 0, ins);
}

TEST_STATIC size_t att_setup_edgepos(const at_Edge *e) {
    const at_Node *n = e->parent;
    size_t         pos = 0;
    int            i;
    for (i = 0; i < (int)e->idx; ++i) pos += n->bytes[i];
    while (n->parent != NULL) {
        const at_Node *p = n->parent;
        for (i = 0; i < (int)n->idx; ++i) pos += p->bytes[i];
        n = p;
    }
    return pos;
}

TEST_STATIC size_t att_checknode(const at_Node *n, int lv, int root, int *ok) {
    size_t sum = 0;
    int    i;
    if (atN_cc(n) > AT_FANOUT || (!root && atN_cc(n) < atN_min())) {
        *ok = 0;
        return 0;
    }
    for (i = 0; i < atN_cc(n); ++i) {
        if (lv == 0) {
            at_Edge *e = atN_edge(n, i);
            if (e->parent != n || e->idx != i) *ok = 0;
            sum += n->bytes[i];
        } else {
            at_Node *c = (at_Node *)n->children[i];
            size_t   cs = att_checknode(c, lv - 1, 0, ok);
            if (n->bytes[i] != cs || c->parent != n || c->idx != i) *ok = 0;
            sum += cs;
        }
    }
    return sum;
}

TEST_STATIC int att_checkleafs(const at_Seq *s, size_t bytes) {
    int i, last = s->n - 1, ok = 1;
    if (s->n == 0) return 0;
    if (s->pos[0] != 0) ok = 0;
    for (i = 0; i < s->n; ++i) {
        if (s->span[i] == 0 && (i != last || att_empty(s->edge[i]))) ok = 0;
        if (i != 0 && s->span[i] != 0 && att_empty(s->edge[i])) ok = 0;
        if (i == 0 && bytes == 0 && att_empty(s->edge[i])) ok = 0;
    }
    return ok;
}

/* structural checker: read-only depth assertion */
TEST_STATIC int att_checkdepth(const at_Tree *T, size_t min_level) {
    return T != NULL && (size_t)T->levels >= min_level;
}

TEST_STATIC int att_checktree(const at_Tree *T) {
    at_Seq s;
    size_t sum;
    int    ok = 1;
    if (T == NULL) return 0;
    if (T->levels >= AT_MAX_LEVEL) return 0;
    sum = att_checknode(&T->root, T->levels, 1, &ok);
    if (at_bytes(T) != sum) ok = 0;
    if (atN_cc(&T->root) == 0) return ok && at_bytes(T) == 0 && T->levels == 0;
    if (at_bytes(T) == 0 && atN_cc(&T->root) > 1) ok = 0;
    if (atN_cc(&T->root) == 1 && T->levels != 0) ok = 0;
    if (!ok) return 0;
    att_seq(T, &s);
    return att_checkleafs(&s, at_bytes(T));
}

TEST_STATIC int att_sideof(const at_Tree *T, at_Edge *e, at_Id id, int end) {
    at_Point p = e->left;
    while (p.id != 0) {
        const at_Anchor *a = &T->ids[p.id];
        if (p.id == id && p.is_end == (unsigned)end) return 0;
        p = p.is_end ? a->enext : a->snext;
    }
    p = e->right;
    while (p.id != 0) {
        const at_Anchor *a = &T->ids[p.id];
        if (p.id == id && p.is_end == (unsigned)end) return 1;
        p = p.is_end ? a->enext : a->snext;
    }
    return -1;
}

TEST_STATIC int att_counttag(const at_Tree *T, at_Point *h, at_Id id, int end) {
    at_Point p = *h;
    int      n = 0;
    while (p.id != 0 && n <= (int)(T->idcap * 2u + 4u)) {
        const at_Anchor *a = &T->ids[p.id];
        if (p.id == id && p.is_end == (unsigned)end) n += 1;
        p = p.is_end ? a->enext : a->snext;
    }
    return n;
}

TEST_STATIC int att_checkbucket(const at_Tree *T, const at_Edge *e, int side) {
    at_Point p = side ? e->right : e->left;
    int      n = 0;
    while (p.id != 0) {
        const at_Anchor *a;
        if (T->ids == NULL || p.id >= T->idcap) return 0;
        a = &T->ids[p.id];
        if (a->end == NULL) return 0;
        if (side != att_side(a->gravity, p.is_end)) return 0;
        if ((p.is_end ? a->end : a->start) != e) return 0;
        p = p.is_end ? a->enext : a->snext;
        n += 1;
        if (n > (int)(T->idcap * 2u + 8u)) return 0;
    }
    return 1;
}

TEST_STATIC int att_checkanchor(const at_Tree *T, at_Id id) {
    const at_Anchor *a = &T->ids[id];
    int              side;
    if (a->end == NULL) return 1;
    if (a->start == NULL) return 0;
    side = att_side(a->gravity, 0);
    if (att_counttag(T, side ? &a->start->right : &a->start->left, id, 0) != 1)
        return 0;
    side = att_side(a->gravity, 1);
    return att_counttag(T, side ? &a->end->right : &a->end->left, id, 1) == 1;
}

TEST_STATIC int att_checkchains(const at_Tree *T) {
    at_Seq s;
    at_Id  id;
    int    i;
    if (T == NULL || !att_checktree(T)) return 0;
    att_seq(T, &s);
    for (i = 0; i < s.n; ++i) {
        if (!att_checkbucket(T, s.edge[i], 0)) return 0;
        if (!att_checkbucket(T, s.edge[i], 1)) return 0;
    }
    if (T->ids == NULL) return 1;
    for (id = 1; id < T->idcap; ++id)
        if (!att_checkanchor(T, id)) return 0;
    return 1;
}

TEST_STATIC int att_no_empty_edges(const at_Tree *T) {
    at_Seq s;
    int    i;
    att_seq(T, &s);
    for (i = 1; i < s.n; ++i)
        if (att_empty(s.edge[i])) return 0;
    return 1;
}

TEST_STATIC void att_anchor(at_Tree *T, size_t p, size_t n, int g, at_Id *id) {
    at_Cursor C;
    assertok(at_seek(&C, T, p) == AT_OK);
    assertok(at_newanchor(&C, n, 0, g, id) == AT_OK);
}

TEST_STATIC void att_mkns(at_Tree *T, size_t p, size_t n, int ns, at_Id *id) {
    at_Cursor C;
    assertok(at_seek(&C, T, p) == AT_OK);
    assertok(at_newanchor(&C, n, ns, AT_GROW, id) == AT_OK);
}

TEST_STATIC at_Id att_anew(at_Tree *T, size_t p, size_t n, int g) {
    at_Cursor C;
    at_Id     id;
    assertok(at_seek(&C, T, p) == AT_OK);
    assertok(at_newanchor(&C, n, 0, g, &id) == AT_OK);
    return id;
}

TEST_STATIC size_t att_lcg(size_t *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed >> 16;
}

TEST_STATIC void att_checkall(at_Tree *T, at_Cursor *C, size_t pos) {
    assertok(att_checktree(T));
    assertok(att_checkchains(T));
    assertok(at_seek(C, T, pos) == AT_OK);
    assertok(att_checkcursor(C, pos));
}

TEST_STATIC void att_randomsplice(at_Cursor *C, size_t *seed) {
    size_t del = att_lcg(seed) % 7u, ins = att_lcg(seed) % 7u;
    assertok(at_splice(C, del, ins) == AT_OK);
}

#endif /* AT_TESTS_H */
