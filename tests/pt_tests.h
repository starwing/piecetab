/* pt_tests.h — piecetab-specific test utilities.
 *
 * Shared by piecetab test sources and fuzz tools. Public utilities
 * (runner, asserts, allocators) live in tests.h; only piecetab-specific
 * helpers live here.  Test sources define PT_FANOUT etc. before include.
 */

#ifndef PT_TESTS_H
#define PT_TESTS_H

#include "piecetab.h"
#include "tests.h"

/* pt_localfill — fill pool freelist with count objects from a local buffer.
 *   pool->freed is set to point to the first object in buf.
 *   buf must hold count * pool->obj_size bytes.
 *   Caller must ensure buf outlives the pool usage. */
PT_STATIC void pt_localfill(pt_Pool *pool, void **op, void *buf, size_t count) {
    size_t i;
    size_t sz = pool->obj_size;
    char  *base = (char *)buf;
    assertok(count > 0 && sz > sizeof(void *));
    *op = pool->freed;
    for (i = 1; i < count; ++i)
        *(void **)(base + (i - 1) * sz) = (void *)(base + i * sz);
    *(void **)(base + (count - 1) * sz) = NULL;
    pool->freed = (void *)base;
    pool->freed_obj = count; /* old chain parked in *op, not reachable */
    ptP_stat(pool->live_obj += count);
}

/* pt_drainpool / pt_refillpool — detach the entire freelist (with its
 * count) so the next ptP_alloc must take the page-alloc path (combine
 * with oom_alloc cnt=0 to force failure).  Refill splices the detached
 * chain back in front of anything freed meanwhile. */
typedef struct {
    void  *chain;
    size_t count;
} pt_Drain;

PT_STATIC pt_Drain pt_drainpool(pt_Pool *p) {
    pt_Drain d;
    d.chain = p->freed, d.count = p->freed_obj;
    p->freed = NULL, p->freed_obj = 0;
    return d;
}

PT_STATIC void pt_refillpool(pt_Pool *p, pt_Drain d) {
    void **pp = &d.chain;
    while (*pp) pp = (void **)*pp;
    *pp = p->freed;
    p->freed = d.chain, p->freed_obj += d.count;
}

/* ================================================================ */
/*  tree invariant checker                                           */
/* ================================================================ */

PT_STATIC int pt_checknode(const pt_Node *n, int rl, int mc, int *has_hole) {
    int i, hh;
    check(n->child_count >= mc, "[chk] N[%p] rl=%d cc=%d<%d\n", (void *)n, rl,
          n->child_count, mc);
    check(n->child_count <= PT_FANOUT, "[chk] N[%p] rl=%d cc=%d>%d\n",
          (void *)n, rl, n->child_count, PT_FANOUT);
    *has_hole = 0;
    for (i = 0; i < n->child_count; ++i) {
        if (rl == 0) {
            if (ptM_ishole(n, i)) {
                check(n->bytes[i] > 0 && n->bytes[i] <= PT_MAX_HOLESIZE,
                      "[chk] HOLE rl=%d i=%d bytes=%lu > %d\n", rl, i,
                      test_lu(n->bytes[i]), (int)PT_MAX_HOLESIZE);
                *has_hole = 1;
            } else {
                check(n->bytes[i] > 0, "[chk] LITERAL rl=%d i=%d bytes=%lu\n",
                      rl, i, test_lu(n->bytes[i]));
                if (i > 0 && !ptM_ishole(n, i - 1)) {
                    check(ptN_lit(n, i - 1) + n->bytes[i - 1] != ptN_lit(n, i),
                          "[chk] ADJACENT literals i=%d,%d node=%p\n", i - 1, i,
                          (void *)n);
                }
            }
        } else {
            pt_Node *c = n->children[i];
            if (!pt_checknode(c, rl - 1, mc ? PT_FANOUT / 2 : 0, &hh)) return 0;
            check(n->bytes[i] == ptN_sumbytes(c, 0, c->child_count),
                  "[chk] INNER rl=%d i=%d bytes=%lu sum=%lu node=%p\n", rl, i,
                  test_lu(n->bytes[i]),
                  test_lu(ptN_sumbytes(c, 0, c->child_count)), (void *)c);
            check((ptM_ishole(n, i) != 0) == (hh != 0),
                  "[chk] MASK rl=%d i=%d mask=%d has_hole=%d\n", rl, i,
                  ptM_ishole(n, i) != 0, hh);
            if (hh) *has_hole = 1;
        }
    }
    return 1;
}

PT_STATIC int pt_checktree_allow_empty(pt_Buffer snap, int allow_empty) {
    int hh = 0;
    check(snap->root.child_count != 0 || snap->bytes == 0,
          "[chk] EMPTY root but bytes=%lu\n", test_lu(snap->bytes));
    if (snap->root.child_count == 0) {
        check(snap->bytes == 0, "[chk] EMPTY tree has bytes=%lu\n",
              test_lu(snap->bytes));
    } else if (snap->levels > 0 || snap->root.child_count > 1)
        return pt_checknode(
                &snap->root, snap->levels, allow_empty ? 0 : 1, &hh);
    else {
        if (ptM_ishole(&snap->root, 0)) {
            check(snap->root.bytes[0] > 0
                          && snap->root.bytes[0] <= PT_MAX_HOLESIZE,
                  "[chk] SINGLE HOLE bytes=%lu > %d\n",
                  test_lu(snap->root.bytes[0]), (int)PT_MAX_HOLESIZE);
        } else {
            check(snap->root.bytes[0] > 0, "[chk] SINGLE LITERAL bytes=%lu\n",
                  test_lu(snap->root.bytes[0]));
        }
    }
    check(snap->bytes == ptN_sumbytes(&snap->root, 0, snap->root.child_count),
          "[chk] ROOT bytes=%lu sum=%lu\n", test_lu(snap->bytes),
          test_lu(ptN_sumbytes(&snap->root, 0, snap->root.child_count)));
    return 1;
}

PT_STATIC int pt_checktree(pt_Buffer snap) {
    return pt_checktree_allow_empty(snap, 0);
}

/* ================================================================ */
/*  cursor invariant checker                                         */
/* ================================================================ */

PT_STATIC int pt_checkcursor(pt_Cursor *C, size_t expected_off) {
    size_t   bsum = 0;
    int      i, l;
    pt_Node *p;
    check(pt_offset(C) == expected_off,
          "[chk] OFFSET mismatch off=%lu expected=%lu\n", test_lu(pt_offset(C)),
          test_lu(expected_off));
    if (C->tree->root.child_count == 0) {
        check(C->poff == 0 && C->off == 0, "[chk] EMPTY poff=%lu off=%lu\n",
              test_lu(C->poff), test_lu(C->off));
        check(C->paths[0] == &C->tree->root.children[0],
              "[chk] EMPTY paths[0]=%p expected=%p\n", (void *)C->paths[0],
              (void *)&C->tree->root.children[0]);
        return 1;
    }
    for (l = 0; l <= ptK_levels(C); ++l) {
        p = ptK_parent(C, l);
        i = ptK_idx(C, p, l);
        check(i >= 0 && i < p->child_count,
              "[chk] PATHS[%d] invalid idx=%d cc=%u\n", l, i, p->child_count);
        check(C->paths[l] == &p->children[i],
              "[chk] PATHS[%d] invalid ptr=%p expected=%p\n", l,
              (void *)C->paths[l], (void *)&p->children[i]);
        bsum += ptN_sumbytes(p, 0, i);
    }
    check(C->off == bsum, "[chk] OFF mismatch off=%lu sum=%lu\n",
          test_lu(C->off), test_lu(bsum));
    p = ptK_parent(C, ptK_levels(C));
    i = ptK_idx(C, p, ptK_levels(C));
    check(C->poff <= p->bytes[i],
          "[chk] POFF out of bounds poff=%lu bytes[%d]=%lu\n", test_lu(C->poff),
          i, test_lu(p->bytes[i]));
    check(C->poff < p->bytes[i] || pt_offset(C) == ptK_bytes(C),
          "[chk] PIECE END mid-tree poff=%lu len=%lu off=%lu bytes=%lu\n",
          test_lu(C->poff), test_lu(p->bytes[i]), test_lu(pt_offset(C)),
          test_lu(ptK_bytes(C)));
    return 1;
}

#endif /* PT_TESTS_H */
