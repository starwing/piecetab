#define CP_STATIC_API
#define SP_FANOUT    4
#define SP_PAGE_SIZE 512
#define SP_STATIC_API
#include <stdio.h>

#include "../spantree.h"
#include "sp_tests.h"

union cp_OpsCursor {
    struct {
        sp_Cursor span;
        int       primed;
    } sp;
    int dummy;
};

#include "compositor.h"
#include "tests.h"

static cp_Leaf leaf_of(cp_State *S, cp_Id id) {
    cp_IdRec *r = cpI_rec(S, id);
    return r ? r->leaf : 0;
}

#define SP_CLEAR_TOKEN ((cp_Leaf)1 << 31)

typedef struct cp_SpStore {
    cp_StoreOps base;
    sp_State   *spS;
    sp_Tree    *tree;
    cp_State   *S;
    cp_NS       cur_ns;
    cp_Leaf     cur_clear;
    int         mode;     /* 0 write, 1 clearleaf, 2 clearall */
    int         clearing; /* inside sp_clear: in == 0 means ns removal */
} cp_SpStore;

static sp_Mask sp_mask_of(cp_State *S, cp_Leaf id) {
    cp_RawPart parts[CP_LEAF_MAX];
    sp_Mask    m = 0;
    int        n = cpR_expand(S, id, parts, CP_LEAF_MAX), i;
    for (i = 0; i < n; ++i)
        if (parts[i].ns > 0) sp_addns(&m, (int)parts[i].ns);
    return m;
}

static sp_Id sp_arb(void *ud, sp_Id in, sp_Id old, sp_Mask *mask) {
    cp_SpStore *b = (cp_SpStore *)ud;
    cp_Store    st;
    cp_Leaf     ret;
    st.ops = &b->base, st.S = b->S;
    if (in == old) {
        if (in != 0) cp_touch(&st, (cp_Leaf)in);
        return in;
    }
    if (in == 0) {
        if (b->clearing) {
            ret = cp_mergeclear(&st, b->cur_ns, (cp_Leaf)old);
            *mask = sp_mask_of(b->S, ret);
            return (sp_Id)ret;
        }
        cp_died(&st, (cp_Leaf)old);
        *mask = 0;
        return 0;
    }
    if (old == 0) {
        ret = cp_born(&st, (cp_Leaf)in);
        *mask = sp_mask_of(b->S, ret);
        return (sp_Id)ret;
    }
    if (b->mode == 1)
        ret = cp_mergecut(&st, b->cur_ns, b->cur_clear, (cp_Leaf)old);
    else
        ret = cp_mergewrite(&st, b->cur_ns, (cp_Leaf)in, (cp_Leaf)old);
    *mask = sp_mask_of(b->S, ret);
    return (sp_Id)ret;
}

static int sp_write(cp_Store *st, cp_NS ns, cp_Leaf id, cp_Range r) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    sp_Cursor   C;
    if (b == NULL || b->tree == NULL) return CP_ERRPARAM;
    b->S = st->S, b->cur_ns = ns, b->mode = 0;
    if (sp_seek(&C, b->tree, r.start) != SP_OK) return CP_ERRPARAM;
    return sp_fill(&C, (sp_Id)id, (size_t)(r.end - r.start)) == SP_OK
                 ? CP_OK
                 : CP_ERRMEM;
}

static int sp_clearleaf(cp_Store *st, cp_NS ns, cp_Leaf id) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    int         rc;
    if (b == NULL || b->tree == NULL) return CP_ERRPARAM;
    b->S = st->S, b->cur_ns = ns, b->cur_clear = id, b->mode = 1;
    b->clearing = 1;
    rc = sp_clear(b->tree, (int)ns, (sp_Id)SP_CLEAR_TOKEN) == SP_OK ? CP_OK
                                                                    : CP_ERRMEM;
    b->clearing = 0;
    return rc;
}

static int sp_clearall(cp_Store *st, cp_NS ns) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    int         rc;
    if (b == NULL || b->tree == NULL) return CP_ERRPARAM;
    b->S = st->S, b->cur_ns = ns, b->mode = 2;
    b->clearing = 1;
    rc = sp_clear(b->tree, (int)ns, 0) == SP_OK ? CP_OK : CP_ERRMEM;
    b->clearing = 0;
    return rc;
}

static int spst_seek(cp_Store *st, cp_NS ns, size_t off, cp_StoreCursor *c) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    sp_Cursor  *C;
    sp_Mask     m;
    size_t      len;
    sp_Id       id;
    if (b == NULL || b->tree == NULL || c == NULL) return CP_ERRPARAM;
    C = &c->ops.sp.span;
    if (sp_seek(C, b->tree, off) != SP_OK) return CP_ERRPARAM;
    c->ops.sp.primed = 0;
    id = sp_style(C, &len, &m);
    if (id == SP_NONE) return CP_OK;
    if (C->poff > 0) sp_advance(C, -(sp_Delta)C->poff);
    for (;;) {
        id = sp_style(C, &len, &m);
        if (id == SP_NONE || len == 0) return CP_OK;
        if (ns == 0 || sp_hasns(&m, (int)ns)) {
            c->ops.sp.primed = 1;
            return CP_OK;
        }
        sp_advance(C, (sp_Delta)len);
    }
}

static int spst_next(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    sp_Cursor  *C;
    sp_Mask     m;
    size_t      len;
    sp_Id       id;
    if (b == NULL || b->tree == NULL || c == NULL || o == NULL)
        return CP_ERRPARAM;
    C = &c->ops.sp.span;
    for (;;) {
        id = sp_style(C, &len, &m);
        if (id == SP_NONE || len == 0) return 0;
        if (c->ops.sp.primed || ns == 0 || sp_hasns(&m, (int)ns)) {
            size_t start = sp_offset(C);
            o->range.start = start, o->range.end = start + len;
            o->id = (cp_Leaf)id;
            sp_advance(C, (sp_Delta)len);
            c->ops.sp.primed = 0;
            return 1;
        }
        sp_advance(C, (sp_Delta)len);
    }
}

static int spst_prev(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    sp_Cursor  *C;
    size_t      len;
    sp_Id       id;
    if (b == NULL || b->tree == NULL || c == NULL || o == NULL)
        return CP_ERRPARAM;
    C = &c->ops.sp.span;
    if (c->ops.sp.primed) c->ops.sp.primed = 0;
    id = sp_prev(C, (int)ns, &len);
    if (id == SP_NONE) return 0;
    o->range.start = sp_offset(C);
    o->range.end = o->range.start + len;
    o->id = (cp_Leaf)id;
    return 1;
}

static void sp_store_init(cp_SpStore *b) {
    memset(b, 0, sizeof(*b));
    b->spS = sp_open(NULL, NULL);
    b->tree = sp_newtree(b->spS);
    sp_setarbiter(b->tree, sp_arb, b);
    b->base.nsmax = SP_MASK_BITS;
    b->base.write = sp_write;
    b->base.clearleaf = sp_clearleaf;
    b->base.clearall = sp_clearall;
    b->base.seek = spst_seek;
    b->base.next = spst_next;
    b->base.prev = spst_prev;
}

static void sp_store_free(cp_SpStore *b) {
    if (b->tree) sp_freetree(b->tree);
    if (b->spS) sp_close(b->spS);
    b->tree = NULL, b->spS = NULL;
}

static cp_Range R(size_t a, size_t b) {
    cp_Range r;
    r.start = a, r.end = b;
    return r;
}

static cp_NS newns(cp_State *S, int flags) {
    return cp_nsnew(S, cp_spanvec(S), flags);
}

static int dummy_write(cp_Store *st, cp_NS ns, cp_Leaf id, cp_Range r) {
    (void)st, (void)ns, (void)id, (void)r;
    return CP_ERRPARAM;
}
static int dummy_clearleaf(cp_Store *st, cp_NS ns, cp_Leaf id) {
    (void)st, (void)ns, (void)id;
    return CP_OK;
}
static int dummy_clearall(cp_Store *st, cp_NS ns) {
    (void)st, (void)ns;
    return CP_OK;
}
static int dummy_seek(cp_Store *st, cp_NS ns, size_t off, cp_StoreCursor *c) {
    (void)st, (void)ns, (void)off, (void)c;
    return CP_OK;
}
static int dummy_next(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o) {
    (void)st, (void)ns, (void)c, (void)o;
    return 0;
}
static int dummy_prev(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o) {
    (void)st, (void)ns, (void)c, (void)o;
    return 0;
}
static void dummy_init(cp_StoreOps *o) {
    o->nsmax = 0;
    o->write = dummy_write;
    o->clearleaf = dummy_clearleaf;
    o->clearall = dummy_clearall;
    o->seek = dummy_seek;
    o->next = dummy_next;
    o->prev = dummy_prev;
}

static int fail_write(cp_Store *st, cp_NS ns, cp_Leaf id, cp_Range r) {
    (void)st, (void)ns, (void)id, (void)r;
    return CP_ERRMEM;
}
static cp_StoreOps fail_ops;

static void fail_init(void) {
    dummy_init(&fail_ops);
    fail_ops.write = fail_write;
}

static int ev_write(cp_Store *st, cp_NS ns, cp_Leaf id, cp_Range r) {
    (void)ns, (void)r;
    cp_born(st, id);
    return CP_OK;
}
static int ev_clearleaf(cp_Store *st, cp_NS ns, cp_Leaf id) {
    (void)ns;
    cp_died(st, id);
    return CP_OK;
}
static cp_StoreOps ev_ops;

static void ev_init(void) {
    dummy_init(&ev_ops);
    ev_ops.write = ev_write;
    ev_ops.clearleaf = ev_clearleaf;
}

static cp_Seg  seg_one;
static int     seg_served;
static cp_Leaf seg_id;

static int seg_write(cp_Store *st, cp_NS ns, cp_Leaf id, cp_Range r) {
    (void)st, (void)ns, (void)r;
    seg_id = id;
    return CP_OK;
}
static int seg_clearleaf(cp_Store *st, cp_NS ns, cp_Leaf id) {
    (void)st, (void)ns, (void)id;
    seg_id = 0;
    return CP_OK;
}
static int seg_seek(cp_Store *st, cp_NS ns, size_t off, cp_StoreCursor *c) {
    (void)st, (void)ns, (void)off, (void)c;
    seg_served = 0;
    return CP_OK;
}
static int seg_next(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o) {
    (void)st, (void)ns, (void)c;
    if (seg_id == 0 || seg_served) return 0;
    seg_served = 1;
    *o = seg_one;
    return 1;
}
static cp_StoreOps seg_ops;

static void seg_init(void) {
    dummy_init(&seg_ops);
    seg_ops.write = seg_write;
    seg_ops.clearleaf = seg_clearleaf;
    seg_ops.seek = seg_seek;
    seg_ops.next = seg_next;
}

static int fc_failcount;

static int fc_write(cp_Store *st, cp_NS ns, cp_Leaf id, cp_Range r) {
    (void)ns;
    seg_one.range = r, seg_one.id = id;
    seg_id = id;
    cp_born(st, id);
    return CP_OK;
}
static int fc_clearleaf(cp_Store *st, cp_NS ns, cp_Leaf id) {
    (void)ns;
    if (fc_failcount == 0) {
        fc_failcount = 1;
        return CP_ERRMEM;
    }
    cp_died(st, id);
    seg_id = 0;
    return CP_OK;
}
static cp_StoreOps fc_ops;

static void fc_init(void) {
    dummy_init(&fc_ops);
    fc_ops.write = fc_write;
    fc_ops.clearleaf = fc_clearleaf;
    fc_ops.seek = seg_seek;
    fc_ops.next = seg_next;
}

static void assert_mark(
        cp_State *S, cp_NS ns, cp_Attr attr, cp_Range r, cp_Id *id) {
    asserteq(cp_mark(S, ns, attr, r, id), CP_OK);
}

TEST(lifecycle_basic) {
    cp_State S;
    Count    c;
    c.live = 0;
    asserteq(cp_init(&S, count_alloc, &c), CP_OK);
    asserteq(cp_epoch(&S), 0);
    cp_free(&S);
    asserteq(c.live, 0);
    cp_init(&S, NULL, NULL);
    cp_reset(&S);
    cp_free(&S);
    cp_free(NULL);
    cp_reset(NULL);
    asserteq(cp_init(NULL, NULL, NULL), CP_ERRPARAM);
}

TEST(namespace_params) {
    cp_State S;
    cp_NS    ns;
    cp_init(&S, NULL, NULL);
    asserteq(cp_nsnew(NULL, NULL, 0), CP_ERRPARAM);
    asserteq(cp_nsnew(&S, NULL, 0), CP_ERRPARAM);
    asserteq(cp_nsnew(&S, cp_spanvec(&S), CP_SENSITIVE), CP_ERRPARAM);
    ns = cp_nsnew(&S, cp_spanvec(&S), CP_IDENTITY | CP_SENSITIVE);
    assertok(ns > 0);
    asserteq(cp_nsdel(&S, ns), CP_OK);
    asserteq(cp_nsdel(&S, ns), CP_ERRPARAM);
    cp_free(&S);
}

TEST(namespace_nsdel_token_invalid) {
    cp_State S;
    cp_NS    ns;
    cp_Id    id;
    cp_Range out;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 7, R(1, 4), &id);
    asserteq(cp_attr(&S, id), 7);
    asserteq(cp_nsdel(&S, ns), CP_OK);
    asserteq(cp_range(&S, id, &out), CP_ERRPARAM);
    asserteq(cp_attr(&S, id), 0);
    asserteq(cp_unmark(&S, id), CP_ERRPARAM);
    cp_free(&S);
}

TEST(namespace_limit) {
    cp_State    S;
    cp_StoreOps ops[6];
    cp_NS       n1, n2, n3, n4, n5;
    int         i;
    cp_init(&S, NULL, NULL);
    for (i = 0; i < 6; ++i) dummy_init(&ops[i]);
    n1 = cp_nsnew(&S, &ops[0], 0);
    n2 = cp_nsnew(&S, &ops[1], 0);
    n3 = cp_nsnew(&S, &ops[2], 0);
    n4 = cp_nsnew(&S, &ops[3], 0);
    assertok(n1 > 0 && n2 > 0 && n3 > 0 && n4 > 0);
    n5 = cp_nsnew(&S, &ops[4], 0);
    asserteq(n5, CP_ELIMIT);
    asserteq(cp_nsdel(&S, n2), CP_OK);
    n5 = cp_nsnew(&S, &ops[4], 0);
    assertok(n5 > 0);
    cp_free(&S);
}

TEST(namespace_nsmax) {
    cp_State S;
    cp_NS    ns;
    cp_init(&S, NULL, NULL);
    cp_spanvec(&S)->nsmax = 2;
    ns = cp_nsnew(&S, cp_spanvec(&S), 0);
    assertok(ns > 0);
    ns = cp_nsnew(&S, cp_spanvec(&S), 0);
    assertok(ns > 0);
    ns = cp_nsnew(&S, cp_spanvec(&S), 0);
    asserteq(ns, CP_ELIMIT);
    cp_spanvec(&S)->nsmax = 0;
    cp_free(&S);
}

TEST(token_overwrite_merge) {
    cp_State      S;
    cp_NS         ns;
    cp_Id         a, b, c;
    cp_MarkCursor MC;
    cp_Mark       m;
    int           r;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 1, R(0, 10), &a);
    assert_mark(&S, ns, 2, R(2, 8), &b);
    assert_mark(&S, ns, 3, R(4, 6), &c);
    assertok(a != b && b != c);
    asserteq(cp_seek(&MC, &S, ns, 0), CP_OK);
    r = cp_next(&MC, &m);
    asserteq(r, 1);
    asserteq(m.range.start, 0);
    asserteq(m.range.end, 2);
    asserteq(m.attr, 1);
    r = cp_next(&MC, &m);
    asserteq(r, 1);
    asserteq(m.range.start, 2);
    asserteq(m.range.end, 4);
    asserteq(m.attr, 2);
    r = cp_next(&MC, &m);
    asserteq(r, 1);
    asserteq(m.range.start, 4);
    asserteq(m.range.end, 6);
    asserteq(m.attr, 3);
    r = cp_next(&MC, &m);
    asserteq(r, 1);
    asserteq(m.range.start, 6);
    asserteq(m.range.end, 8);
    asserteq(m.attr, 2);
    r = cp_next(&MC, &m);
    asserteq(r, 1);
    asserteq(m.range.start, 8);
    asserteq(m.range.end, 10);
    asserteq(m.attr, 1);
    asserteq(cp_next(&MC, &m), 0);
    cp_free(&S);
}

TEST(token_unmark_count) {
    cp_State S;
    cp_NS    ns;
    cp_Id    a, b;
    cp_Range out;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 1, R(0, 10), &a);
    assert_mark(&S, ns, 2, R(2, 8), &b);
    asserteq(cp_unmark(&S, b), 1);
    asserteq(cp_unmark(&S, b), 0);
    asserteq(cp_attr(&S, a), 1);
    asserteq(cp_range(&S, a, &out), CP_ERRPARAM);
    /* a is split around b; clearing b leaves a whole again after merging */
    assert_mark(&S, ns, 1, R(0, 10), &a);
    asserteq(cp_unmark(&S, a), 1);
    asserteq(cp_unmark(&S, a), 0);
    cp_free(&S);
}

TEST(token_unmark_external_store) {
    cp_State S;
    cp_NS    ns;
    cp_Id    id;
    cp_init(&S, NULL, NULL);
    seg_init();
    seg_one.range.start = 0, seg_one.range.end = 5, seg_one.id = 7;
    seg_id = 0;
    ns = cp_nsnew(&S, &seg_ops, 0);
    assert_mark(&S, ns, 3, R(0, 5), &id);
    asserteq(seg_id, leaf_of(&S, id));
    asserteq(seg_id != (cp_Leaf)id, 1);
    seg_one.id = seg_id;
    asserteq(cp_unmark(&S, id), 1);
    asserteq(seg_id, 0);
    asserteq(cp_unmark(&S, id), 0);
    asserteq(cp_attr(&S, id), 3);
    cp_free(&S);
}

TEST(object_overlap_range) {
    cp_State S;
    cp_NS    ns;
    cp_Id    o1, o2;
    cp_Range out;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, CP_IDENTITY);
    assert_mark(&S, ns, 0, R(1, 5), &o1);
    assert_mark(&S, ns, 9, R(3, 8), &o2);
    assertok(o1 != o2);
    asserteq(cp_range(&S, o1, &out), CP_OK);
    asserteq(out.start, 1);
    asserteq(out.end, 5);
    asserteq(cp_range(&S, o2, &out), CP_OK);
    asserteq(out.start, 3);
    asserteq(out.end, 8);
    asserteq(cp_attr(&S, o1), 0);
    asserteq(cp_attr(&S, o2), 9);
    cp_free(&S);
}

TEST(object_unmark_exact) {
    cp_State S;
    cp_NS    ns;
    cp_Id    o1, o2;
    cp_Range out;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, CP_IDENTITY);
    assert_mark(&S, ns, 1, R(0, 4), &o1);
    assert_mark(&S, ns, 2, R(2, 6), &o2);
    asserteq(cp_unmark(&S, o1), 1);
    asserteq(cp_range(&S, o1, &out), CP_ERRPARAM);
    asserteq(cp_attr(&S, o1), 0);
    asserteq(cp_unmark(&S, o1), 0);
    asserteq(cp_range(&S, o2, &out), CP_OK);
    asserteq(out.start, 2);
    asserteq(out.end, 6);
    cp_free(&S);
}

TEST(priority_order_dedupe) {
    cp_State        S;
    cp_NS           n1, n2, n3, n4;
    cp_Id           a, b, c, d;
    cp_StyledCursor SC;
    cp_Styled       s;
    int             r;
    cp_init(&S, NULL, NULL);
    n1 = newns(&S, 0);
    n2 = newns(&S, 0);
    n3 = newns(&S, 0);
    n4 = newns(&S, 0);
    assert_mark(&S, n1, 1, R(0, 10), &a);
    assert_mark(&S, n2, 2, R(0, 10), &b);
    assert_mark(&S, n3, 3, R(0, 10), &c);
    /* same attr in a second namespace must be deduped in styled output */
    assert_mark(&S, n4, 1, R(0, 10), &d);
    cp_setpriority(&S, 1, 10);
    cp_setpriority(&S, 2, -1);
    cp_setpriority(&S, 3, 10);
    asserteq(cp_seekstyled(&SC, &S, 0), CP_OK);
    r = cp_nextstyled(&SC, &s);
    asserteq(r, 1);
    asserteq(s.attr, 2);
    asserteq(s.last, 0);
    r = cp_nextstyled(&SC, &s);
    asserteq(r, 1);
    asserteq(s.attr, 1);
    asserteq(s.last, 0);
    r = cp_nextstyled(&SC, &s);
    asserteq(r, 1);
    asserteq(s.attr, 3);
    asserteq(s.last, 1);
    asserteq(cp_nextstyled(&SC, &s), 0);
    cp_free(&S);
}

TEST(styled_boundaries) {
    cp_State        S;
    cp_NS           n1, n2;
    cp_Id           a, b;
    cp_StyledCursor SC;
    cp_Styled       s;
    int             r;
    cp_init(&S, NULL, NULL);
    n1 = newns(&S, 0);
    n2 = newns(&S, 0);
    assert_mark(&S, n1, 1, R(0, 4), &a);
    assert_mark(&S, n2, 2, R(2, 6), &b);
    asserteq(cp_seekstyled(&SC, &S, 0), CP_OK);
    r = cp_nextstyled(&SC, &s);
    asserteq(r, 1);
    asserteq(s.range.start, 0);
    asserteq(s.range.end, 2);
    asserteq(s.attr, 1);
    asserteq(s.last, 1);
    r = cp_nextstyled(&SC, &s);
    asserteq(r, 1);
    asserteq(s.range.start, 2);
    asserteq(s.range.end, 4);
    asserteq(s.attr, 1);
    asserteq(s.last, 0);
    r = cp_nextstyled(&SC, &s);
    asserteq(r, 1);
    asserteq(s.range.start, 2);
    asserteq(s.range.end, 4);
    asserteq(s.attr, 2);
    asserteq(s.last, 1);
    r = cp_nextstyled(&SC, &s);
    asserteq(r, 1);
    asserteq(s.range.start, 4);
    asserteq(s.range.end, 6);
    asserteq(s.attr, 2);
    asserteq(s.last, 1);
    asserteq(cp_nextstyled(&SC, &s), 0);
    cp_free(&S);
}

TEST(styled_adjacent_group_grow) {
    cp_State        S;
    cp_NS           n1, n2, n3;
    cp_Id           a;
    cp_StyledCursor SC;
    cp_Styled       s;
    cp_init(&S, NULL, NULL);
    n1 = newns(&S, 0);
    n2 = newns(&S, 0);
    n3 = newns(&S, 0);
    assert_mark(&S, n1, 1, R(0, 5), &a);
    assert_mark(&S, n2, 2, R(5, 10), &a);
    assert_mark(&S, n3, 3, R(5, 10), &a);
    asserteq(cp_seekstyled(&SC, &S, 0), CP_OK);
    asserteq(cp_nextstyled(&SC, &s), 1);
    asserteq(s.range.start, 0);
    asserteq(s.range.end, 5);
    asserteq(s.attr, 1);
    asserteq(s.last, 1);
    asserteq(cp_nextstyled(&SC, &s), 1);
    asserteq(s.range.start, 5);
    asserteq(s.range.end, 10);
    asserteq(s.attr, 2);
    asserteq(s.last, 0);
    asserteq(cp_nextstyled(&SC, &s), 1);
    asserteq(s.attr, 3);
    asserteq(s.last, 1);
    asserteq(cp_nextstyled(&SC, &s), 0);
    cp_free(&S);
}

TEST(epoch_invalidate) {
    cp_State        S;
    cp_NS           ns, obj;
    cp_Id           id, oid;
    cp_MarkCursor   MC;
    cp_StyledCursor SC;
    cp_Mark         m;
    cp_Styled       st;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    obj = newns(&S, CP_IDENTITY);
    assert_mark(&S, ns, 1, R(0, 3), &id);
    assert_mark(&S, obj, 5, R(0, 3), &oid);
    asserteq(cp_seek(&MC, &S, ns, 0), CP_OK);
    asserteq(cp_seekstyled(&SC, &S, 0), CP_OK);
    assert_mark(&S, ns, 2, R(1, 2), &id);
    asserteq(cp_next(&MC, &m), CP_ESTALE);
    asserteq(cp_nextstyled(&SC, &st), CP_ESTALE);
    /* object mark does not affect token mark cursor, but epoch is shared */
    cp_free(&S);
}

TEST(nsclear_token_keeps_id) {
    cp_State      S;
    cp_NS         ns;
    cp_Id         id;
    cp_MarkCursor MC;
    cp_Mark       m;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 4, R(1, 5), &id);
    asserteq(cp_nsclear(&S, ns), CP_OK);
    asserteq(cp_attr(&S, id), 4);
    asserteq(cp_unmark(&S, id), 0);
    asserteq(cp_seek(&MC, &S, ns, 0), CP_OK);
    asserteq(cp_next(&MC, &m), 0);
    /* reuse the same token id after clear */
    assert_mark(&S, ns, 4, R(0, 2), &id);
    asserteq(cp_unmark(&S, id), 1);
    cp_free(&S);
}

TEST(nsclear_object_invalidates) {
    cp_State S;
    cp_NS    ns;
    cp_Id    o;
    cp_Range out;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, CP_IDENTITY);
    assert_mark(&S, ns, 0, R(2, 6), &o);
    asserteq(cp_nsclear(&S, ns), CP_OK);
    asserteq(cp_range(&S, o, &out), CP_ERRPARAM);
    asserteq(cp_attr(&S, o), 0);
    asserteq(cp_unmark(&S, o), 0);
    cp_free(&S);
}

TEST(spanvec_commit_clears) {
    cp_State S;
    cp_NS    tok, obj;
    cp_Id    t, o;
    cp_Range out;
    cp_init(&S, NULL, NULL);
    tok = newns(&S, 0);
    obj = newns(&S, CP_IDENTITY | CP_SENSITIVE);
    assert_mark(&S, tok, 7, R(0, 4), &t);
    assert_mark(&S, obj, 0, R(1, 3), &o);
    asserteq(cp_commit(&S), CP_OK);
    asserteq(cp_attr(&S, t), 7);
    asserteq(cp_unmark(&S, t), 0);
    asserteq(cp_range(&S, o, &out), CP_ERRPARAM);
    cp_free(&S);
}

TEST(spanvec_identity_overlap_commit) {
    cp_State S;
    cp_NS    ns;
    cp_Id    a, b;
    cp_Range out;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, CP_IDENTITY);
    assert_mark(&S, ns, 3, R(0, 5), &a);
    assert_mark(&S, ns, 4, R(2, 7), &b);
    asserteq(cp_commit(&S), CP_OK);
    asserteq(cp_range(&S, a, &out), CP_ERRPARAM);
    asserteq(cp_range(&S, b, &out), CP_ERRPARAM);
    cp_free(&S);
}

TEST(sensitive_touch_commit) {
    cp_State S;
    cp_NS    ns;
    cp_Id    o;
    cp_Leaf  l;
    cp_Store st;
    cp_Range out;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, CP_IDENTITY | CP_SENSITIVE);
    assert_mark(&S, ns, 8, R(1, 4), &o);
    l = leaf_of(&S, o);
    st.ops = cp_spanvec(&S), st.S = &S;
    cp_touch(&st, l);
    asserteq(cp_commit(&S), CP_OK);
    asserteq(cp_range(&S, o, &out), CP_ERRPARAM);
    cp_free(&S);
}

TEST(seek_window) {
    cp_State      S;
    cp_NS         tok, obj;
    cp_Id         t, o;
    cp_MarkCursor MC;
    cp_Mark       m;
    cp_init(&S, NULL, NULL);
    tok = newns(&S, 0);
    obj = newns(&S, CP_IDENTITY);
    assert_mark(&S, tok, 1, R(0, 4), &t);
    assert_mark(&S, obj, 9, R(3, 8), &o);
    /* inside a token segment: returns the segment */
    asserteq(cp_seek(&MC, &S, tok, 2), CP_OK);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.range.start, 0);
    asserteq(m.range.end, 4);
    /* object hull is returned even when pos is inside it */
    asserteq(cp_seek(&MC, &S, obj, 4), CP_OK);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.range.start, 3);
    asserteq(m.range.end, 8);
    asserteq(cp_attr(&S, m.id), 9);
    cp_free(&S);
}

TEST(styled_seek_window) {
    cp_State        S;
    cp_NS           n1, n2;
    cp_Id           a, b;
    cp_StyledCursor SC;
    cp_Styled       s;
    int             r;
    cp_init(&S, NULL, NULL);
    n1 = newns(&S, 0);
    n2 = newns(&S, 0);
    assert_mark(&S, n1, 1, R(0, 6), &a);
    assert_mark(&S, n2, 2, R(2, 4), &b);
    asserteq(cp_seekstyled(&SC, &S, 3), CP_OK);
    r = cp_nextstyled(&SC, &s);
    asserteq(r, 1);
    asserteq(s.range.start, 2);
    asserteq(s.range.end, 4);
    asserteq(s.attr, 1);
    asserteq(s.last, 0);
    r = cp_nextstyled(&SC, &s);
    asserteq(r, 1);
    asserteq(s.range.start, 2);
    asserteq(s.range.end, 4);
    asserteq(s.attr, 2);
    asserteq(s.last, 1);
    cp_free(&S);
}

TEST(reset_no_leak) {
    cp_State S;
    Count    c;
    cp_NS    ns;
    cp_Id    id;
    c.live = 0;
    cp_init(&S, count_alloc, &c);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 1, R(0, 5), &id);
    cp_reset(&S);
    asserteq(c.live, 0);
    asserteq(cp_epoch(&S), 0);
    ns = newns(&S, 0);
    assertok(ns > 0);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(params_mark) {
    cp_State S;
    cp_NS    ns, obj;
    cp_Id    id;
    cp_Range out;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    obj = newns(&S, CP_IDENTITY);
    asserteq(cp_mark(NULL, ns, 1, R(0, 1), &id), CP_ERRPARAM);
    asserteq(cp_mark(&S, ns, 1, R(0, 1), NULL), CP_ERRPARAM);
    asserteq(cp_mark(&S, ns, 1, R(1, 1), &id), CP_ERRPARAM);
    asserteq(cp_mark(&S, ns, 0, R(0, 1), &id), CP_ERRPARAM);
    asserteq(cp_mark(&S, (cp_NS)999, 1, R(0, 1), &id), CP_ERRPARAM);
    assert_mark(&S, obj, 0, R(0, 1), &id);
    asserteq(cp_range(NULL, id, &out), CP_ERRPARAM);
    asserteq(cp_range(&S, id, NULL), CP_ERRPARAM);
    cp_free(&S);
}

static void oom_setup(cp_State *S, OomCount *c);

TEST(spanvec_store_ops) {
    cp_State       S;
    cp_NS          ns;
    cp_Id          a, b;
    cp_Store       st;
    cp_StoreCursor cur;
    cp_Seg         seg;
    int            r;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 1, R(0, 10), &a);
    assert_mark(&S, ns, 2, R(2, 4), &b);
    st.ops = cp_spanvec(&S), st.S = &S;
    asserteq(st.ops->seek(&st, ns, 0, &cur), CP_OK);
    r = st.ops->next(&st, ns, &cur, &seg);
    asserteq(r, 1);
    asserteq(seg.range.start, 0);
    asserteq(seg.range.end, 2);
    r = st.ops->next(&st, ns, &cur, &seg);
    asserteq(r, 1);
    asserteq(seg.range.start, 2);
    asserteq(seg.range.end, 4);
    r = st.ops->next(&st, ns, &cur, &seg);
    asserteq(r, 1);
    asserteq(seg.range.start, 4);
    asserteq(seg.range.end, 10);
    asserteq(st.ops->next(&st, ns, &cur, &seg), 0);
    asserteq(st.ops->seek(&st, ns, 3, &cur), CP_OK);
    asserteq(st.ops->prev(&st, ns, &cur, &seg), 1);
    asserteq(seg.range.start, 0);
    asserteq(seg.range.end, 2);
    asserteq(st.ops->prev(&st, ns, &cur, &seg), 0);
    asserteq(st.ops->seek(&st, ns, 100, &cur), CP_OK);
    asserteq(st.ops->next(&st, ns, &cur, &seg), 0);
    asserteq(st.ops->seek(&st, ns, 5, &cur), CP_OK);
    asserteq(st.ops->prev(&st, ns, &cur, &seg), 1);
    asserteq(seg.range.start, 2);
    asserteq(seg.range.end, 4);
    asserteq(st.ops->prev(&st, ns, &cur, &seg), 1);
    asserteq(seg.range.start, 0);
    asserteq(st.ops->prev(&st, ns, &cur, &seg), 0);
    asserteq(st.ops->seek(&st, (cp_NS)999, 0, &cur), CP_ERRPARAM);
    asserteq(st.ops->next(&st, ns, &cur, NULL), CP_ERRPARAM);
    asserteq(st.ops->prev(&st, ns, &cur, NULL), CP_ERRPARAM);
    {
        cp_NS on = newns(&S, CP_IDENTITY);
        cp_Id o1, o2;
        assert_mark(&S, on, 1, R(4, 6), &o1);
        assert_mark(&S, on, 2, R(4, 6), &o2);
        asserteq(st.ops->seek(&st, on, 4, &cur), CP_OK);
        asserteq(st.ops->next(&st, on, &cur, &seg), 1);
        asserteq(seg.id, leaf_of(&S, o1));
        asserteq(st.ops->next(&st, on, &cur, &seg), 1);
        asserteq(seg.id, leaf_of(&S, o2));
    }
    cp_free(&S);
}

TEST(event_functions) {
    cp_State S;
    cp_NS    ns;
    cp_Id    oid;
    cp_Store st;
    cp_Range out;
    cp_Leaf  l;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, CP_IDENTITY | CP_SENSITIVE);
    assert_mark(&S, ns, 3, R(0, 2), &oid);
    l = leaf_of(&S, oid);
    st.ops = cp_spanvec(&S), st.S = &S;
    asserteq(cp_born(&st, l), l);
    asserteq(cp_born(NULL, l), l);
    cp_died(&st, l);
    cp_died(NULL, l);
    cp_touch(NULL, l);
    asserteq(cp_mergewrite(&st, ns, l, 0), l);
    asserteq(cp_mergewrite(NULL, ns, l, l), l);
    cp_died(&st, l);
    asserteq(cp_mergeclear(&st, ns, 0), 0);
    asserteq(cp_mergeclear(NULL, ns, l), 0);
    asserteq(cp_mergecut(&st, ns, l, 0), 0);
    asserteq(cp_mergecut(NULL, ns, l, l), l);
    asserteq(cp_range(&S, oid, &out), CP_OK);
    cp_free(&S);
}

TEST(sensitive_partial_queue) {
    cp_State S;
    cp_NS    ns;
    cp_Id    oid;
    cp_Store st;
    cp_Range out;
    cp_Leaf  l;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, CP_IDENTITY | CP_SENSITIVE);
    assert_mark(&S, ns, 1, R(0, 2), &oid);
    l = leaf_of(&S, oid);
    st.ops = cp_spanvec(&S), st.S = &S;
    cp_born(&st, l);
    cp_died(&st, l);
    /* second queue attempt is idempotent */
    cp_touch(&st, l);
    cp_touch(&st, l);
    asserteq(cp_commit(&S), CP_OK);
    asserteq(cp_range(&S, oid, &out), CP_ERRPARAM);
    cp_free(&S);
}

TEST(share_store_nsdel) {
    cp_State S;
    cp_NS    n1, n2;
    cp_init(&S, NULL, NULL);
    n1 = newns(&S, 0);
    n2 = newns(&S, 0);
    assertok(n1 > 0 && n2 > 0);
    asserteq(S.store_count, 1);
    asserteq(cp_nsdel(&S, n1), CP_OK);
    asserteq(S.store_count, 1);
    asserteq(cp_nsdel(&S, n2), CP_OK);
    asserteq(S.store_count, 0);
    cp_free(&S);
}

TEST(priority_existing) {
    cp_State S;
    cp_init(&S, NULL, NULL);
    asserteq(cp_setpriority(&S, 5, 3), CP_OK);
    asserteq(cp_setpriority(&S, 5, 3), CP_OK);
    asserteq(cp_setpriority(&S, 5, 4), CP_OK);
    cp_free(&S);
}

TEST(token_full_inside_overwrite) {
    cp_State      S;
    cp_NS         ns;
    cp_Id         a, b, c;
    cp_MarkCursor MC;
    cp_Mark       m;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 1, R(0, 10), &a);
    assert_mark(&S, ns, 2, R(2, 4), &b);
    assert_mark(&S, ns, 3, R(1, 9), &c);
    asserteq(cp_seek(&MC, &S, ns, 0), CP_OK);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.attr, 1);
    asserteq(m.range.start, 0);
    asserteq(m.range.end, 1);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.attr, 3);
    asserteq(m.range.start, 1);
    asserteq(m.range.end, 9);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.attr, 1);
    asserteq(m.range.start, 9);
    asserteq(m.range.end, 10);
    cp_free(&S);
}

TEST(oom_mark_prewrite) {
    cp_State S;
    OomCount c;
    cp_NS    n1, n2;
    cp_Id    id, id2 = 0;
    oom_setup(&S, &c);
    n1 = cp_nsnew(&S, cp_spanvec(&S), 0);
    n2 = cp_nsnew(&S, cp_spanvec(&S), 0);
    assertok(n1 > 0 && n2 > 0);
    assert_mark(&S, n1, 1, R(0, 1), &id);
    c.oom = 0;
    asserteq(cp_mark(&S, n2, 2, R(0, 1), &id2), CP_ERRMEM);
    asserteq(id2, 0);
    asserteq(cp_attr(&S, id), 1);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(event_backend_commit) {
    cp_State S;
    Count    c;
    cp_NS    ns;
    cp_Id    oid;
    cp_Leaf  l;
    cp_Range out;
    c.live = 0;
    cp_init(&S, count_alloc, &c);
    ev_init();
    ns = cp_nsnew(&S, &ev_ops, CP_IDENTITY | CP_SENSITIVE);
    assertok(ns > 0);
    assert_mark(&S, ns, 2, R(0, 3), &oid);
    l = leaf_of(&S, oid);
    {
        cp_Store st;
        st.ops = &ev_ops, st.S = &S;
        cp_touch(&st, l);
    }
    asserteq(cp_commit(&S), CP_OK);
    asserteq(cp_range(&S, oid, &out), CP_ERRPARAM);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(object_mark_order) {
    cp_State      S;
    cp_NS         ns;
    cp_Id         a, b;
    cp_MarkCursor MC;
    cp_Mark       m;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, CP_IDENTITY);
    assert_mark(&S, ns, 1, R(10, 12), &a);
    assert_mark(&S, ns, 2, R(0, 2), &b);
    asserteq(cp_seek(&MC, &S, ns, 0), CP_OK);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.id, b);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.id, a);
    asserteq(cp_next(&MC, &m), 0);
    /* same-start tie uses id order */
    assert_mark(&S, ns, 3, R(0, 2), &a);
    asserteq(cp_seek(&MC, &S, ns, 0), CP_OK);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.range.start, 0);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(cp_next(&MC, &m), 0);
    cp_free(&S);
}

TEST(cmp_equal) {
    cp_MarkItem a, b;
    a.range.start = 3, a.range.end = 7, a.id = 9, a.kind = 2;
    b = a;
    asserteq(cpM_cmp(&a, &b), 0);
}

TEST(styled_empty_and_gap) {
    cp_State        S;
    cp_NS           ns;
    cp_Id           id;
    cp_StyledCursor SC;
    cp_Styled       st;
    cp_init(&S, NULL, NULL);
    asserteq(cp_seekstyled(&SC, &S, 0), CP_OK);
    asserteq(cp_nextstyled(&SC, &st), 0);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 1, R(5, 8), &id);
    asserteq(cp_seekstyled(&SC, &S, 0), CP_OK);
    asserteq(cp_nextstyled(&SC, &st), 1);
    asserteq(st.range.start, 5);
    asserteq(st.range.end, 8);
    cp_free(&S);
}

TEST(oom_seek_token) {
    cp_State      S;
    OomCount      c;
    cp_NS         ns;
    cp_Id         id;
    cp_MarkCursor MC;
    oom_setup(&S, &c);
    ns = cp_nsnew(&S, cp_spanvec(&S), 0);
    assert_mark(&S, ns, 1, R(0, 2), &id);
    c.oom = 0;
    asserteq(cp_seek(&MC, &S, ns, 0), CP_ERRMEM);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(oom_seek_object) {
    cp_State      S;
    OomCount      c;
    cp_NS         ns;
    cp_Id         id;
    cp_MarkCursor MC;
    oom_setup(&S, &c);
    ns = cp_nsnew(&S, cp_spanvec(&S), CP_IDENTITY);
    assert_mark(&S, ns, 1, R(0, 2), &id);
    c.oom = 0;
    asserteq(cp_seek(&MC, &S, ns, 0), CP_ERRMEM);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(oom_styled_token) {
    cp_State        S;
    OomCount        c;
    cp_NS           ns;
    cp_Id           id;
    cp_StyledCursor SC;
    oom_setup(&S, &c);
    ns = cp_nsnew(&S, cp_spanvec(&S), 0);
    assert_mark(&S, ns, 1, R(0, 2), &id);
    c.oom = 0;
    asserteq(cp_seekstyled(&SC, &S, 0), CP_ERRMEM);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(oom_styled_object) {
    cp_State        S;
    OomCount        c;
    cp_NS           ns;
    cp_Id           id;
    cp_StyledCursor SC;
    oom_setup(&S, &c);
    ns = cp_nsnew(&S, cp_spanvec(&S), CP_IDENTITY);
    assert_mark(&S, ns, 1, R(0, 2), &id);
    c.oom = 0;
    asserteq(cp_seekstyled(&SC, &S, 0), CP_ERRMEM);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(oom_styled_group) {
    cp_State        S;
    OomCount        c;
    cp_NS           ns;
    cp_Id           id;
    cp_StyledCursor SC;
    oom_setup(&S, &c);
    ns = cp_nsnew(&S, cp_spanvec(&S), 0);
    assert_mark(&S, ns, 1, R(0, 2), &id);
    c.oom = 1;
    asserteq(cp_seekstyled(&SC, &S, 0), CP_ERRMEM);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(invalid_segment_skip) {
    cp_State        S;
    cp_NS           ns;
    cp_Id           id;
    cp_MarkCursor   MC;
    cp_Mark         m;
    cp_StyledCursor SC;
    cp_Styled       st;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 1, R(0, 2), &id);
    S.nsv[ns - 1].segs[0].id = 999;
    asserteq(cp_seek(&MC, &S, ns, 0), CP_OK);
    asserteq(cp_next(&MC, &m), 0);
    asserteq(cp_seekstyled(&SC, &S, 0), CP_OK);
    asserteq(cp_nextstyled(&SC, &st), 0);
    cp_free(&S);
}

TEST(params_bulk) {
    cp_State        S;
    cp_NS           ns;
    cp_Id           id;
    cp_Range        r, out;
    cp_MarkCursor   MC;
    cp_StyledCursor SC;
    cp_Mark         m;
    cp_Styled       st;
    cp_Store        store;
    cp_StoreCursor  cur;
    cp_Seg          seg;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 1, R(0, 1), &id);
    r.start = 0, r.end = 1;
    /* seek / next null and invalid ns */
    asserteq(cp_seek(NULL, &S, ns, 0), CP_ERRPARAM);
    asserteq(cp_seek(&MC, NULL, ns, 0), CP_ERRPARAM);
    asserteq(cp_seek(&MC, &S, (cp_NS)999, 0), CP_ERRPARAM);
    asserteq(cp_seek(&MC, &S, ns, 0), CP_OK);
    asserteq(cp_next(NULL, &m), CP_ERRPARAM);
    MC.S = NULL;
    asserteq(cp_next(&MC, &m), CP_ERRPARAM);
    MC.S = &S;
    /* styled null */
    asserteq(cp_seekstyled(NULL, &S, 0), CP_ERRPARAM);
    asserteq(cp_seekstyled(&SC, NULL, 0), CP_ERRPARAM);
    asserteq(cp_seekstyled(&SC, &S, 0), CP_OK);
    asserteq(cp_nextstyled(NULL, &st), CP_ERRPARAM);
    SC.S = NULL;
    asserteq(cp_nextstyled(&SC, &st), CP_ERRPARAM);
    SC.S = &S;
    /* invalid ids */
    asserteq(cp_unmark(NULL, id), CP_ERRPARAM);
    asserteq(cp_unmark(&S, 0), CP_ERRPARAM);
    asserteq(cp_range(NULL, id, &out), CP_ERRPARAM);
    asserteq(cp_range(&S, 0, &out), CP_ERRPARAM);
    asserteq(cp_attr(NULL, id), CP_NOATTR);
    asserteq(cp_attr(&S, 0), CP_NOATTR);
    /* namespace / priority null */
    asserteq(cp_nsclear(NULL, ns), CP_ERRPARAM);
    asserteq(cp_nsdel(NULL, ns), CP_ERRPARAM);
    asserteq(cp_setpriority(NULL, 1, 1), CP_ERRPARAM);
    asserteq(cp_setpriority(&S, CP_NOATTR, 1), CP_ERRPARAM);
    asserteq(cp_commit(NULL), CP_ERRPARAM);
    asserteq(cp_epoch(NULL), 0);
    /* spanvec ops invalid parameters */
    store.ops = cp_spanvec(&S), store.S = &S;
    asserteq(store.ops->write(&store, (cp_NS)999, (cp_Leaf)id, r), CP_ERRPARAM);
    asserteq(
            store.ops->clearleaf(&store, (cp_NS)999, (cp_Leaf)id), CP_ERRPARAM);
    asserteq(store.ops->clearall(&store, (cp_NS)999), CP_ERRPARAM);
    asserteq(store.ops->seek(&store, (cp_NS)999, 0, &cur), CP_ERRPARAM);
    asserteq(store.ops->seek(&store, ns, 0, NULL), CP_ERRPARAM);
    asserteq(store.ops->next(&store, ns, NULL, &seg), CP_ERRPARAM);
    asserteq(store.ops->prev(&store, ns, NULL, &seg), CP_ERRPARAM);
    /* mergewrite with different old id */
    {
        cp_Id other;
        assert_mark(&S, ns, 2, R(2, 3), &other);
        asserteq(cp_mergewrite(&store, ns, (cp_Leaf)id, (cp_Leaf)other), id);
    }
    cp_free(&S);
}

/* OOM helpers: run mutator with a budget that forces failure after every
 * allocation site. */
static void oom_setup(cp_State *S, OomCount *c) {
    c->live = 0, c->oom = 100000;
    cp_init(S, oomcount_alloc, c);
}

TEST(oom_nsnew) {
    cp_State S;
    OomCount c;
    oom_setup(&S, &c);
    c.oom = 0;
    asserteq(cp_nsnew(&S, cp_spanvec(&S), 0), CP_ERRMEM);
    asserteq(cpV_len(S.nsv), 0);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(oom_mark) {
    cp_State S;
    OomCount c;
    cp_NS    ns;
    cp_Id    id;
    oom_setup(&S, &c);
    ns = cp_nsnew(&S, cp_spanvec(&S), 0);
    assertok(ns > 0);
    c.oom = 0;
    asserteq(cp_mark(&S, ns, 1, R(0, 1), &id), CP_ERRMEM);
    asserteq(cp_attr(&S, id), 0);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(oom_setpriority) {
    cp_State S;
    OomCount c;
    oom_setup(&S, &c);
    c.oom = 0;
    asserteq(cp_setpriority(&S, 1, 5), CP_ERRMEM);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(mark_backend_fail) {
    cp_State S;
    Count    c;
    cp_NS    ns;
    cp_Id    id;
    c.live = 0;
    cp_init(&S, count_alloc, &c);
    fail_init();
    ns = cp_nsnew(&S, &fail_ops, 0);
    assertok(ns > 0);
    asserteq(cp_mark(&S, ns, 1, R(0, 1), &id), CP_ERRMEM);
    asserteq(cp_attr(&S, id), 0);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(spantree_two_namespaces) {
    cp_State   S;
    cp_SpStore b;
    cp_NS      n1, n2;
    cp_Id      a, b2;
    sp_Cursor  C;
    size_t     len;
    sp_Mask    m;
    sp_Id      id;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    n1 = cp_nsnew(&S, &b.base, 0);
    n2 = cp_nsnew(&S, &b.base, 0);
    assertok(n1 > 0 && n2 > 0);
    assert_mark(&S, n1, 1, R(0, 10), &a);
    assert_mark(&S, n2, 2, R(0, 10), &b2);
    assertok(sp_checktree(b.tree));
    asserteq(sp_seek(&C, b.tree, 0), SP_OK);
    assertok(sp_checkcursor(&C, 0));
    id = sp_style(&C, &len, &m);
    assertok(id != SP_NONE);
    assertok(sp_hasns(&m, (int)n1));
    assertok(sp_hasns(&m, (int)n2));
    sp_store_free(&b);
    cp_free(&S);
}

TEST(spantree_token_unmark_composite) {
    cp_State      S;
    cp_SpStore    b;
    cp_NS         n1, n2;
    cp_Id         a, b2;
    cp_MarkCursor MC;
    cp_Mark       m;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    n1 = cp_nsnew(&S, &b.base, 0);
    n2 = cp_nsnew(&S, &b.base, 0);
    assert_mark(&S, n1, 1, R(0, 10), &a);
    assert_mark(&S, n2, 2, R(2, 8), &b2);
    assertok(sp_checktree(b.tree));
    asserteq(cp_unmark(&S, b2), 1);
    assertok(sp_checktree(b.tree));
    asserteq(cp_seek(&MC, &S, n1, 0), CP_OK);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.range.start, 0);
    asserteq(m.range.end, 10);
    asserteq(cp_next(&MC, &m), 0);
    sp_store_free(&b);
    cp_free(&S);
}

TEST(spantree_object_range_edit) {
    cp_State   S;
    cp_SpStore b;
    cp_NS      obj;
    cp_Id      o;
    cp_Range   out;
    sp_Cursor  C;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    obj = cp_nsnew(&S, &b.base, CP_IDENTITY);
    assert_mark(&S, obj, 0, R(2, 10), &o);
    asserteq(cp_range(&S, o, &out), CP_OK);
    asserteq(out.start, 2);
    asserteq(out.end, 10);
    /* native spantree edit trims the object span; cp_range re-derives. */
    asserteq(sp_seek(&C, b.tree, 2), SP_OK);
    asserteq(sp_splice(&C, 4, 0), SP_OK);
    assertok(sp_checktree(b.tree));
    asserteq(cp_commit(&S), CP_OK);
    asserteq(cp_range(&S, o, &out), CP_OK);
    asserteq(out.start, 2);
    asserteq(out.end, 6);
    sp_store_free(&b);
    cp_free(&S);
}

TEST(spantree_sensitive_merge_retain) {
    cp_State   S;
    cp_SpStore b;
    cp_NS      obj, tok;
    cp_Id      o, t;
    cp_Range   out;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    obj = cp_nsnew(&S, &b.base, CP_IDENTITY | CP_SENSITIVE);
    tok = cp_nsnew(&S, &b.base, 0);
    assert_mark(&S, obj, 7, R(0, 10), &o);
    assert_mark(&S, tok, 1, R(0, 10), &t);
    assertok(sp_checktree(b.tree));
    /* another layer merging over the object is not an object edit. */
    asserteq(cp_range(&S, o, &out), CP_OK);
    asserteq(cp_nsclear(&S, tok), CP_OK);
    assertok(sp_checktree(b.tree));
    asserteq(cp_range(&S, o, &out), CP_OK);
    sp_store_free(&b);
    cp_free(&S);
}

TEST(spantree_sensitive_edit_invalidate) {
    cp_State   S;
    cp_SpStore b;
    cp_NS      obj;
    cp_Id      o;
    cp_Range   out;
    sp_Cursor  C;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    obj = cp_nsnew(&S, &b.base, CP_IDENTITY | CP_SENSITIVE);
    assert_mark(&S, obj, 5, R(0, 10), &o);
    asserteq(sp_seek(&C, b.tree, 4), SP_OK);
    asserteq(sp_splice(&C, 2, 0), SP_OK);
    assertok(sp_checktree(b.tree));
    assertok(S.senshead != 0);
    asserteq(cp_commit(&S), CP_OK);
    asserteq(cp_range(&S, o, &out), CP_ERRPARAM);
    sp_store_free(&b);
    cp_free(&S);
}

TEST(commit_retry_failure) {
    cp_State S;
    cp_NS    ns;
    cp_Id    oid;
    cp_Leaf  l;
    cp_Store st;
    cp_Range out;
    cp_Epoch e;
    cp_init(&S, NULL, NULL);
    fc_init();
    fc_failcount = 0;
    seg_one.range.start = 0, seg_one.range.end = 5, seg_one.id = 0;
    seg_id = 0;
    ns = cp_nsnew(&S, &fc_ops, CP_IDENTITY | CP_SENSITIVE);
    assert_mark(&S, ns, 3, R(0, 5), &oid);
    l = leaf_of(&S, oid);
    seg_one.id = l;
    st.ops = &fc_ops, st.S = &S;
    cp_touch(&st, l);
    e = cp_epoch(&S);
    asserteq(cp_commit(&S), CP_ERRMEM);
    asserteq(cp_epoch(&S), e + 1);
    asserteq(cp_range(&S, oid, &out), CP_OK);
    asserteq(cp_commit(&S), CP_OK);
    asserteq(cp_epoch(&S), e + 2);
    asserteq(cp_range(&S, oid, &out), CP_ERRPARAM);
    cp_free(&S);
}

TEST(spantree_write_replaces_same_ns) {
    cp_State      S;
    cp_SpStore    b;
    cp_NS         ns;
    cp_Id         a, b2;
    cp_MarkCursor MC;
    cp_Mark       m;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    ns = cp_nsnew(&S, &b.base, 0);
    assert_mark(&S, ns, 1, R(0, 10), &a);
    assert_mark(&S, ns, 2, R(0, 10), &b2);
    assertok(sp_checktree(b.tree));
    asserteq(cp_seek(&MC, &S, ns, 0), CP_OK);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.attr, 2);
    asserteq(m.range.start, 0);
    asserteq(m.range.end, 10);
    asserteq(cp_next(&MC, &m), 0);
    sp_store_free(&b);
    cp_free(&S);
}

TEST(spantree_lower_ns_after_higher) {
    cp_State   S;
    cp_SpStore b;
    cp_NS      n1, n2;
    cp_Id      a, b2;
    sp_Cursor  C;
    size_t     len;
    sp_Mask    m;
    sp_Id      id;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    n1 = cp_nsnew(&S, &b.base, 0);
    n2 = cp_nsnew(&S, &b.base, 0);
    assert_mark(&S, n2, 2, R(0, 10), &b2);
    assert_mark(&S, n1, 1, R(0, 10), &a);
    assertok(sp_checktree(b.tree));
    asserteq(sp_seek(&C, b.tree, 0), SP_OK);
    assertok(sp_checkcursor(&C, 0));
    id = sp_style(&C, &len, &m);
    assertok(id != SP_NONE);
    assertok(sp_hasns(&m, (int)n1));
    assertok(sp_hasns(&m, (int)n2));
    sp_store_free(&b);
    cp_free(&S);
}

TEST(mergecut_sensitive_queues) {
    cp_State   S;
    cp_SpStore b;
    cp_NS      obj, tok;
    cp_Id      o, t;
    cp_Leaf    l, mid;
    cp_Store   st;
    sp_Cursor  C;
    size_t     len;
    sp_Mask    m;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    obj = cp_nsnew(&S, &b.base, CP_IDENTITY | CP_SENSITIVE);
    tok = cp_nsnew(&S, &b.base, 0);
    assert_mark(&S, obj, 9, R(0, 10), &o);
    assert_mark(&S, tok, 1, R(2, 8), &t);
    assertok(sp_checktree(b.tree));
    l = leaf_of(&S, o);
    asserteq(sp_seek(&C, b.tree, 3), SP_OK);
    mid = (cp_Leaf)sp_style(&C, &len, &m);
    assertok(sp_hasns(&m, (int)obj));
    assertok(sp_hasns(&m, (int)tok));
    st.ops = &b.base, st.S = &S;
    /* Remove the object from the middle composite only; left/right keep
     * it alive, so a sensitive object must be queued. */
    (void)cp_mergecut(&st, obj, l, mid);
    assertok(S.senshead != 0);
    sp_store_free(&b);
    cp_free(&S);
}

TEST(nsclear_external_preflight_oom) {
    cp_State S;
    OomCount c;
    cp_NS    ns;
    cp_Id    id;
    cp_Epoch e;
    oom_setup(&S, &c);
    ev_init();
    ns = cp_nsnew(&S, &ev_ops, 0);
    assertok(ns > 0);
    assert_mark(&S, ns, 1, R(0, 1), &id);
    e = cp_epoch(&S);
    S.rawfree = 0, S.rawfreecnt = 0;
    c.oom = 0;
    asserteq(cp_nsclear(&S, ns), CP_ERRMEM);
    asserteq(cp_epoch(&S), e);
    asserteq(cp_attr(&S, id), 1);
    asserteq(cp_nsdel(&S, ns), CP_ERRMEM);
    asserteq(cp_epoch(&S), e);
    c.oom = 100000;
    asserteq(cp_nsdel(&S, ns), CP_OK);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(commit_preflight_oom) {
    cp_State S;
    OomCount c;
    cp_NS    ns;
    cp_Id    oid;
    cp_Leaf  l;
    cp_Store st;
    cp_Epoch e;
    oom_setup(&S, &c);
    ev_init();
    ns = cp_nsnew(&S, &ev_ops, CP_IDENTITY | CP_SENSITIVE);
    assertok(ns > 0);
    assert_mark(&S, ns, 3, R(0, 2), &oid);
    l = leaf_of(&S, oid);
    st.ops = &ev_ops, st.S = &S;
    cp_touch(&st, l);
    e = cp_epoch(&S);
    S.rawfree = 0, S.rawfreecnt = 0;
    c.oom = 0;
    asserteq(cp_commit(&S), CP_ERRMEM);
    asserteq(cp_epoch(&S), e + 1);
    assertok(S.senshead != 0);
    cp_free(&S);
    asserteq(c.live, 0);
}

TEST(styled_token_noattr_skip) {
    cp_State        S;
    cp_NS           ns;
    cp_Id           id;
    cp_StyledCursor SC;
    cp_Styled       s;
    cp_init(&S, NULL, NULL);
    ns = newns(&S, 0);
    assert_mark(&S, ns, 1, R(0, 2), &id);
    S.ids[id - 1].attr = CP_NOATTR;
    asserteq(cp_seekstyled(&SC, &S, 0), CP_OK);
    asserteq(cp_nextstyled(&SC, &s), 0);
    cp_free(&S);
}

TEST(mergewrite_direct_composite) {
    cp_State S;
    cp_NS    n1, n2;
    cp_Id    a, b;
    cp_Leaf  l1, l2, ret;
    cp_Store st;
    cp_init(&S, NULL, NULL);
    n1 = newns(&S, 0);
    n2 = newns(&S, 0);
    assert_mark(&S, n1, 1, R(0, 1), &a);
    assert_mark(&S, n2, 2, R(0, 1), &b);
    l1 = leaf_of(&S, a), l2 = leaf_of(&S, b);
    st.ops = cp_spanvec(&S), st.S = &S;
    /* No pre-reserved pool is available on a direct spanvec call; the
     * event path must still be able to build a composite. */
    ret = cp_mergewrite(&st, n2, l2, l1);
    assertok(ret != 0);
    assertok(ret != l1 && ret != l2);
    cp_free(&S);
}

TEST(spantree_native_remove_full_leaf) {
    cp_State   S;
    cp_SpStore b;
    cp_NS      obj, tok;
    cp_Id      o, t;
    sp_Cursor  C;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    obj = cp_nsnew(&S, &b.base, CP_IDENTITY);
    tok = cp_nsnew(&S, &b.base, 0);
    assert_mark(&S, obj, 7, R(0, 10), &o);
    assert_mark(&S, tok, 1, R(0, 10), &t);
    assertok(sp_checktree(b.tree));
    asserteq(sp_seek(&C, b.tree, 0), SP_OK);
    asserteq(sp_splice(&C, 10, 0), SP_OK);
    assertok(sp_checktree(b.tree));
    asserteq(cp_commit(&S), CP_OK);
    asserteq(cp_unmark(&S, o), 0);
    asserteq(cp_attr(&S, t), 1);
    asserteq(cp_unmark(&S, t), 0);
    sp_store_free(&b);
    cp_free(&S);
}

TEST(spantree_object_unmark_composite) {
    cp_State      S;
    cp_SpStore    b;
    cp_NS         obj, tok;
    cp_Id         o, t;
    cp_MarkCursor MC;
    cp_Mark       m;
    cp_Range      out;
    cp_init(&S, NULL, NULL);
    sp_store_init(&b);
    b.S = &S;
    obj = cp_nsnew(&S, &b.base, CP_IDENTITY);
    tok = cp_nsnew(&S, &b.base, 0);
    assert_mark(&S, obj, 7, R(0, 10), &o);
    assert_mark(&S, tok, 1, R(2, 8), &t);
    assertok(sp_checktree(b.tree));
    asserteq(cp_unmark(&S, o), 1);
    assertok(sp_checktree(b.tree));
    asserteq(cp_range(&S, o, &out), CP_ERRPARAM);
    asserteq(cp_seek(&MC, &S, tok, 0), CP_OK);
    asserteq(cp_next(&MC, &m), 1);
    asserteq(m.range.start, 2);
    asserteq(m.range.end, 8);
    asserteq(cp_next(&MC, &m), 0);
    sp_store_free(&b);
    cp_free(&S);
}

/* More tests will be appended after the initial set compiles. */
#include "compositor_test.gen.inc"
