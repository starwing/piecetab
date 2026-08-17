/* sp_tests.h — spantree-specific test utilities.
 *
 * Shared by spantree test sources (multi-FANOUT).  Public utilities
 * (runner, asserts, allocators) live in tests.h; only spantree-specific
 * helpers live here.  Test sources define SP_FANOUT etc. before include.
 */

#ifndef SP_TESTS_H
#define SP_TESTS_H

#include "spantree.h"
#include "tests.h"

SP_STATIC void sp_dumptree(const sp_Tree *t, const char *tag);

/* sp_drainpool / sp_refillpool — detach the entire freelist (with its
 * count) so the next spP_alloc must take the page-alloc path (combine
 * with oom_alloc cnt=0 to force failure). Refill splices the detached
 * chain back in front of anything freed meanwhile. */
typedef struct {
    void  *chain;
    size_t count;
} sp_Drain;

SP_STATIC sp_Drain sp_drainpool(sp_Pool *p) {
    sp_Drain d;
    d.chain = p->freed, d.count = p->freed_obj;
    p->freed = NULL, p->freed_obj = 0;
    return d;
}

SP_STATIC void sp_refillpool(sp_Pool *p, sp_Drain d) {
    void **pp = &d.chain;
    while (*pp) pp = (void **)*pp;
    *pp = p->freed;
    p->freed = d.chain, p->freed_obj += d.count;
}

/* tree invariant checker: byte sums consistent, child counts within
 * fanout bounds (root exempt from the half-minimum), no zero-length
 * segments in a settled tree. Returns the subtree byte sum (0 also
 * signals failure — a legal node cannot sum to 0) and the OR of the
 * leaf masks so parents verify bytes[i]/mask[i] without a second pass. */

SP_STATIC size_t sp_checknode(
        const sp_Node *n, int rl, int mc, sp_Mask *pmsk, int allow_unseamed) {
    size_t  sum = 0;
    int     i;
    sp_Mask m = 0;
    check(n->child_count <= SP_FANOUT, "[chk] N[%p] rl=%d cc=%d>%d\n",
          (void *)n, rl, n->child_count, SP_FANOUT);
    for (i = 0; i < (int)n->child_count; ++i) {
        if (rl == 0) {
            check(n->bytes[i] > 0, "[chk] ZEROSEG rl=%d i=%d cc=%d len=%lu\n",
                  rl, i, n->child_count, test_lu(n->bytes[i]));
            if (!allow_unseamed)
                check(i == 0 || spL_id(n, i - 1) != spL_id(n, i),
                      "[chk] SEGMERGE rl=%d i=%d id=%lu\n", rl, i,
                      test_lu(spL_id(n, i)));
            sum += n->bytes[i], m |= n->mask[i];
        } else {
            sp_Mask csmsk = 0;
            size_t  cs;
            check(n->child_count >= mc, "[chk] N[%p] rl=%d cc=%d<%d\n",
                  (void *)n, rl, n->child_count, mc);
            cs = sp_checknode(
                    n->children[i], rl - 1, mc ? SP_FANOUT / 2 : 0, &csmsk,
                    allow_unseamed);
            check(n->bytes[i] == cs,
                  "[chk] INNER rl=%d i=%d cc=%d bytes=%lu sum=%lu node=%p\n",
                  rl, i, n->child_count, test_lu(n->bytes[i]), test_lu(cs),
                  (void *)n->children[i]);
            check(n->mask[i] == csmsk,
                  "[chk] MASK rl=%d i=%d mask=%lu or=%lu\n", rl, i,
                  test_lu(n->mask[i]), test_lu(csmsk));
            sum += cs, m |= csmsk;
        }
    }
    if (pmsk) *pmsk = m;
    return sum;
}

/* allow_unseamed: hand-built trees in tests legitimately carry adjacent
 * segments with equal ids (the op under test merges them); the strict
 * check demands a settled tree, so construction points use the allow
 * variant and post-op checks keep the strict one */
SP_STATIC int sp_checktree_allow_unseamedspan(const sp_Tree *t, int allow) {
    sp_Mask msk = 0;
    size_t  bsum;
    if (spN_cc(&t->root) == 0) {
        check(t->bytes == 0, "[chk] EMPTY tree has bytes=%lu\n",
              test_lu(t->bytes));
        return 1;
    }
    bsum = sp_checknode(&t->root, t->levels, t->levels ? 1 : 0, &msk, allow);
    check(t->bytes == bsum, "[chk] ROOT bytes=%lu sum=%lu\n", test_lu(t->bytes),
          test_lu(bsum));
    return 1;
}

SP_STATIC int sp_checktree(const sp_Tree *t) {
    return sp_checktree_allow_unseamedspan(t, 0);
}

/* cursor invariant checker: paths valid, off equals the byte sum of the
 * segments before the current one; virtual states (offset >= bytes)
 * keep off at the last segment's start and push the excess into poff
 * (poff >= segment length); only an empty tree has off == bytes */

SP_STATIC int sp_checkcursor(sp_Cursor *C, size_t expected_off) {
    size_t   bsum = 0;
    int      i, l;
    sp_Node *p;
    check(sp_offset(C) == expected_off,
          "[chk] OFFSET mismatch off=%lu expected=%lu\n", test_lu(sp_offset(C)),
          test_lu(expected_off));
    if (C->tree->bytes == 0) {
        check(C->off == 0 && C->paths[0] == C->tree->root.children,
              "[chk] EMPTY off=%lu poff=%lu\n", test_lu(C->off),
              test_lu(C->poff));
        check(C->poff == expected_off, "[chk] EMPTY poff=%lu expected=%lu\n",
              test_lu(C->poff), test_lu(expected_off));
        return 1;
    }
    for (l = 0; l <= spK_levels(C); ++l) {
        p = spK_parent(C, l), i = spK_idx(C, p, l);
        check(i >= 0 && i < spN_cc(p), "[chk] PATHS[%d] invalid idx=%d cc=%d\n",
              l, i, spN_cc(p));
        check(C->paths[l] == &p->children[i],
              "[chk] PATHS[%d] invalid ptr=%p expected=%p\n", l,
              (void *)C->paths[l], (void *)&p->children[i]);
        bsum += spN_sumbytes(p, 0, i);
    }
    check(C->off == bsum, "[chk] OFF mismatch off=%lu sum=%lu\n",
          test_lu(C->off), test_lu(bsum));
    p = spK_parent(C, spK_levels(C)), i = spK_idx(C, p, spK_levels(C));
    if (sp_offset(C) >= C->tree->bytes)
        check(C->poff >= p->bytes[i],
              "[chk] VIRTUAL poff=%lu len=%lu off=%lu bytes=%lu\n",
              test_lu(C->poff), test_lu(p->bytes[i]), test_lu(C->off),
              test_lu(C->tree->bytes));
    else
        check(C->poff < p->bytes[i],
              "[chk] SPAN END mid-tree poff=%lu len=%lu off=%lu bytes=%lu\n",
              test_lu(C->poff), test_lu(p->bytes[i]), test_lu(sp_offset(C)),
              test_lu(C->tree->bytes));
    return 1;
}

/* tree dump */

SP_STATIC void sp_dumpnode(const sp_Node *n, int idx, int l, int levels) {
    unsigned i, cc = n->child_count;
    if (l == 0)
        test_log("Root(%p) cc=%u", (void *)n, cc);
    else
        test_log("%*sN%u_%u(%p) cc=%u", l * 2, "", l - 1, idx, (void *)n, cc);
    for (i = 0; i < cc; ++i) test_log(" b[%u]=%lu", i, test_lu(n->bytes[i]));
    test_log("\n");
    if (l == levels || levels == 0) {
        for (i = 0; i < cc; ++i)
            test_log(
                    "%*sL%u len=%lu id=%lu\n", (l + 1) * 2, "", i,
                    test_lu(n->bytes[i]), test_lu(spL_id(n, i)));
    } else {
        for (i = 0; i < cc; ++i) sp_dumpnode(n->children[i], i, l + 1, levels);
    }
}

SP_STATIC void sp_dumptree(const sp_Tree *t, const char *tag) {
    test_log(
            "[TREE]\t %s: levels=%u root.cc=%u bytes=%lu\n", tag, t->levels,
            t->root.child_count, test_lu(t->bytes));
    sp_dumpnode(&t->root, -1, 0, t->levels);
}

/* ================================================================ */
/*  tree construction helpers (leafV / innerV / treeV)               */
/*  leafV takes (id, len) pairs, zero length terminates; ids are     */
/*  size_t so varargs are type-consistent                             */
/* ================================================================ */

#define leafV(...)     leafV_(S, __VA_ARGS__, 0, 0)
#define innerV(...)    innerV_(S, __VA_ARGS__, NULL)
#define treeV(l, root) treeV_(S, l, root)

SP_STATIC sp_Node *leafV_(sp_State *S, ...) {
    va_list  ap;
    sp_Node *n;
    unsigned cc = 0, i;
    size_t   id, len;
    va_start(ap, S);
    while (va_arg(ap, size_t), (len = va_arg(ap, size_t)) != 0) cc++;
    va_end(ap);
    n = (sp_Node *)spP_alloc(S, &S->nodes);
    assertok(n && cc <= SP_FANOUT);
    spN_setcc(n, cc);
    va_start(ap, S);
    for (i = 0; i < cc; i++) {
        id = va_arg(ap, size_t), len = va_arg(ap, size_t);
        spL_setid(n, i, id), n->bytes[i] = len, n->mask[i] = 0;
    }
    va_end(ap);
    return n;
}

SP_STATIC sp_Node *innerV_(sp_State *S, ...) {
    va_list  ap;
    sp_Node *n, *c;
    unsigned cc = 0, i;
    va_start(ap, S);
    while (va_arg(ap, sp_Node *) != NULL) cc++;
    va_end(ap);
    n = (sp_Node *)spP_alloc(S, &S->nodes);
    assertok(n && cc <= SP_FANOUT);
    spN_setcc(n, cc);
    va_start(ap, S);
    for (i = 0; i < cc; i++) {
        c = va_arg(ap, sp_Node *);
        n->children[i] = c, n->bytes[i] = spN_sumbytes(c, 0, spN_cc(c));
        n->mask[i] = spM_sumns(c);
    }
    va_end(ap);
    return n;
}

SP_STATIC sp_Tree *treeV_(sp_State *S, unsigned levels, sp_Node *root) {
    sp_Tree *t = sp_newtree(S);
    unsigned i;
    assertok(t && root->child_count <= SP_FANOUT);
    t->levels = (unsigned short)levels, t->root = *root;
    spP_free(&S->nodes, root);
    t->bytes = 0;
    for (i = 0; i < t->root.child_count; i++) t->bytes += t->root.bytes[i];
    return assert(t), t;
}

/* ---- ns test utilities ---- */

/* arbiter model: ids are ns bitsets (id == mask); ids >= 0x8000 clear
 * the bit (id - 0x8000), returning 0 when the last contribution goes;
 * in == 0 is the death shape (three-shape contract, must return 0) */
SP_STATIC sp_Id spA_nsset(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    sp_Mask m;
    (void)ud;
    if (id == 0) return (*mask = 0, (sp_Id)0);
    if (id >= 0x8000) {
        m = *mask & ~((sp_Mask)1 << ((int)id - 0x8000 - 1));
        return m ? (*mask = m, old & m) : (*mask = 0, (sp_Id)0);
    }
    m = (sp_Mask)old | (sp_Mask)id;
    return *mask = m, (sp_Id)m;
}

SP_STATIC int   sp_ns_calls;
SP_STATIC sp_Id spA_nscount(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    sp_ns_calls += 1;
    return spA_nsset(ud, id, old, mask);
}

/* collect every segment whose leaf mask holds bit (full scan with
 * internal mask access — the differential ground truth) */
SP_STATIC int sp_ns_collect(
        sp_Tree *t, sp_Mask bit, sp_Id *ids, size_t *lens, int max) {
    sp_Cursor C;
    sp_Node  *p;
    int       n = 0, i;
    size_t    len;
    sp_seek(&C, t, 0);
    for (;;) {
        sp_Id id = sp_style(&C, &len, NULL);
        if (len == 0) break;
        i = spK_idx(&C, p = spK_parent(&C, spK_levels(&C)), spK_levels(&C));
        if (p->mask[i] & bit) {
            assertok(n < max);
            ids[n] = id, lens[n] = len, n += 1;
        }
        sp_next(&C, 0, &len);
    }
    return n;
}

/* recursive shape compare: child counts, byte sums and leaf ids */
SP_STATIC int sp_comparenode(
        const sp_Node *a, const sp_Node *b, int l, int levels) {
    int i;
    if (a->child_count != b->child_count) return 0;
    for (i = 0; i < (int)a->child_count; ++i) {
        if (a->bytes[i] != b->bytes[i]) return 0;
        if (l == levels) {
            if (spL_id(a, i) != spL_id(b, i)) return 0;
        } else if (
                !sp_comparenode(a->children[i], b->children[i], l + 1, levels))
            return 0;
    }
    return 1;
}

SP_STATIC int sp_comparetree(const sp_Tree *a, const sp_Tree *b) {
    if (a->levels != b->levels || a->bytes != b->bytes) return 0;
    if (a->root.child_count != b->root.child_count) return 0;
    if (a->root.child_count == 0) return 1;
    return sp_comparenode(&a->root, &b->root, 0, a->levels);
}

/* build the expected tree with leafV/innerV/treeV, compare against t
 * (levels, bytes, cc, leaf ids), dump both on mismatch, free expected */
#define sp_asserttree(t, lvls, root)                                         \
    do {                                                                     \
        sp_Tree *__d = treeV(lvls, root);                                    \
        if (!sp_comparetree((t), __d)) {                                     \
            test_log("sp_asserttree FAILED at %s:%d\n", __FILE__, __LINE__); \
            sp_dumptree(__d, "expected");                                    \
            sp_dumptree((t), "actual");                                      \
            abort();                                                         \
        }                                                                    \
        sp_freetree(__d);                                                    \
    } while (0)

/* serialize the segment stream as "[id:len][id:len]..." for content
 * comparison against a naive model. Adjacent same-id segments merge in
 * the output (in-leaf merges are mandatory, seam neighbors are allowed
 * to stay apart, so only the user-level stream is comparable). */
SP_STATIC void sp_serialtree(sp_Tree *t, char *buf) {
    sp_Cursor C;
    sp_Id     id, lid = 0;
    size_t    len, run = 0;
    int       r = 0;
    buf[0] = '\0';
    assertok(sp_seek(&C, t, 0) == SP_OK);
    for (;;) {
        id = sp_style(&C, &len, NULL);
        if (len == 0) break; /* segment ids can be 0: plen marks the end */
        if (id == lid && run)
            run += len;
        else {
            if (run)
                r += sprintf(buf + r, "[%lu:%lu]", test_lu(lid), test_lu(run));
            lid = id, run = len;
        }
        sp_next(&C, 0, &len);
    }
    if (run) r += sprintf(buf + r, "[%lu:%lu]", test_lu(lid), test_lu(run));
}

/* ---- id lifecycle (three-shape arbiter) utilities ---- */

#define SP_REFN 256

typedef struct {
    long ref[SP_REFN];
    int  calls;
} SpRef;

/* overwrite arbiter applying the unified three-shape refcount rule
 * (ret == id always: in==0 death, old==0 birth, both = overwrite) */
SP_STATIC sp_Id spA_ref(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    SpRef *r = (SpRef *)ud;
    (void)mask;
    assert(id < SP_REFN && old < SP_REFN);
    r->calls += 1;
    if (id != 0) r->ref[id] += 1;
    if (old != 0) r->ref[old] -= 1;
    return id;
}

/* ground truth: segment tally over the tree (sp_style + sp_next
 * pairing: sp_next alone is exclusive and skips the first segment) */
SP_STATIC void spA_tally(sp_Tree *t, long *counts) {
    sp_Cursor C;
    size_t    len;
    sp_Id     id;
    int       i;
    for (i = 0; i < SP_REFN; ++i) counts[i] = 0;
    assertok(sp_seek(&C, t, 0) == SP_OK);
    for (;;) {
        id = sp_style(&C, &len, NULL);
        if (len == 0) break;
        counts[id] += 1;
        sp_next(&C, 0, &len);
    }
}

SP_STATIC int spA_check(const SpRef *r, const long *counts) {
    int i;
    for (i = 1; i < SP_REFN; ++i)
        if (r->ref[i] != counts[i]) {
            test_log("ref[%d]: %ld != %ld", i, r->ref[i], counts[i]);
            return 0;
        }
    return 1;
}

/* apply the unified three-shape exit rule to any base arbiter's
 * return: the tree's event plumbing is what the differential
 * validates, independent of the merge choice */
SP_STATIC sp_Id spA_apply(
        SpRef *r, sp_Arbiterf *arb, void *aud, sp_Id id, sp_Id old,
        sp_Mask *mask) {
    sp_Id ret = arb(aud, id, old, mask);
    assert(ret < SP_REFN && old < SP_REFN);
    if (ret != 0) r->ref[ret] += 1;
    if (old != 0) r->ref[old] -= 1;
    return ret;
}

/* ref-counting wrapper over the ns-bitset arbiter (ids are small
 * masks; the >= 0x8000 clear op ids never become returns) */
SP_STATIC sp_Id spA_nsref(void *ud, sp_Id id, sp_Id old, sp_Mask *mask) {
    return spA_apply((SpRef *)ud, spA_nsset, NULL, id, old, mask);
}

/* seed the ref table from the tree's segment tally (for trees built
 * manually, bypassing the arbiter) */
SP_STATIC void spA_seed(SpRef *r, sp_Tree *t) {
    long counts[SP_REFN];
    spA_tally(t, counts);
    memcpy(r->ref, counts, sizeof(counts));
    r->calls = 0;
}

#endif /* SP_TESTS_H */
