#ifndef compositor_h
#define compositor_h

#ifndef CP_NS_BEGIN
# ifdef __cplusplus
#   define CP_NS_BEGIN extern "C" {
#   define CP_NS_END   }
# else
#   define CP_NS_BEGIN
#   define CP_NS_END
# endif
#endif /* CP_NS_BEGIN */

#ifndef CP_STATIC
# if __GNUC__
#   define CP_STATIC static __attribute((unused))
# else
#   define CP_STATIC static
# endif
#endif /* CP_STATIC */

#ifdef CP_STATIC_API
# ifndef CP_IMPLEMENTATION
#   define CP_IMPLEMENTATION
# endif
# define CP_API CP_STATIC
#endif /* CP_STATIC_API */

#if !defined(CP_API) && defined(_WIN32)
# ifdef CP_IMPLEMENTATION
#   define CP_API __declspec(dllexport)
# else
#   define CP_API __declspec(dllimport)
# endif
#endif /* CP_API */

#ifndef CP_API
# define CP_API extern
#endif

#include <stddef.h>

#define CP_OK       (0)
#define CP_ERRPARAM (-1)
#define CP_ERRMEM   (-2)
#define CP_ESTALE   (-3)
#define CP_ELIMIT   (-4)

#define CP_NOATTR    ((cp_Attr)0)
#define CP_IDENTITY  (1 << 0)
#define CP_SENSITIVE (1 << 1)

#ifndef CP_STORE_MAX
# define CP_STORE_MAX 4
#endif

CP_NS_BEGIN

typedef struct cp_State        cp_State;
typedef struct cp_Store        cp_Store;
typedef struct cp_StoreOps     cp_StoreOps;
typedef struct cp_MarkCursor   cp_MarkCursor;
typedef struct cp_StyledCursor cp_StyledCursor;

typedef void *cp_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

typedef int    cp_NS;
typedef size_t cp_Id;
typedef size_t cp_Attr;
typedef size_t cp_Epoch;

typedef struct cp_Range {
    size_t start, end;
} cp_Range;
typedef struct cp_Mark {
    cp_Range range;
    cp_Id    id;
    cp_Attr  attr;
} cp_Mark;
typedef struct cp_Styled {
    cp_Range range;
    cp_Attr  attr;
    int      last;
} cp_Styled;

/* lifecycle */
CP_API int  cp_init(cp_State *S, cp_Alloc *alloc, void *ud);
CP_API void cp_free(cp_State *S);
CP_API void cp_reset(cp_State *S);

/* namespace */
CP_API cp_NS cp_nsnew(cp_State *S, cp_StoreOps *ops, int flags);
CP_API int   cp_nsdel(cp_State *S, cp_NS ns);
CP_API int   cp_nsclear(cp_State *S, cp_NS ns);

/* document edit boundary */
CP_API int      cp_commit(cp_State *S);
CP_API cp_Epoch cp_epoch(const cp_State *S);

/* attr priority */
CP_API int cp_setpriority(cp_State *S, cp_Attr attr, int prio);

/* marks */
CP_API int cp_mark(cp_State *S, cp_NS ns, cp_Attr attr, cp_Range r, cp_Id *id);
CP_API int cp_unmark(cp_State *S, cp_Id id);
CP_API int cp_range(cp_State *S, cp_Id id, cp_Range *out);
CP_API cp_Attr cp_attr(cp_State *S, cp_Id id);

/* mark iteration */
CP_API int cp_seek(cp_MarkCursor *MC, cp_State *S, cp_NS ns, size_t pos);
CP_API int cp_next(cp_MarkCursor *MC, cp_Mark *out);

/* styled iteration */
CP_API int cp_seekstyled(cp_StyledCursor *SC, cp_State *S, size_t pos);
CP_API int cp_nextstyled(cp_StyledCursor *SC, cp_Styled *out);

/* embedded spanvec reference backend */
CP_API cp_StoreOps *cp_spanvec(cp_State *S);

/* ===================== StoreOps provider interface ===================== */

typedef size_t cp_Leaf;

#define cp_ops(st) (*(cp_StoreOps **)(void *)(st))

typedef struct cp_SVCursor {
    size_t pos;
} cp_SVCursor;
typedef union cp_OpsCursor cp_OpsCursor;
typedef union cp_StoreCursor {
    cp_SVCursor  sv;
    cp_OpsCursor ops;
} cp_StoreCursor;

typedef struct cp_Seg {
    cp_Range range;
    cp_Leaf  id;
} cp_Seg;

struct cp_StoreOps {
    int nsmax;
    int (*write)(cp_Store *st, cp_NS ns, cp_Leaf id, cp_Range r);
    int (*clearleaf)(cp_Store *st, cp_NS ns, cp_Leaf id);
    int (*clearall)(cp_Store *st, cp_NS ns);
    int (*seek)(cp_Store *st, cp_NS ns, size_t off, cp_StoreCursor *c);
    int (*next)(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o);
    int (*prev)(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o);
};

/* store events */
CP_API cp_Leaf cp_born(cp_Store *st, cp_Leaf id);
CP_API void    cp_died(cp_Store *st, cp_Leaf id);
CP_API void    cp_touch(cp_Store *st, cp_Leaf id);
CP_API cp_Leaf cp_mergewrite(cp_Store *st, cp_NS ns, cp_Leaf in, cp_Leaf old);
CP_API cp_Leaf cp_mergeclear(cp_Store *st, cp_NS ns, cp_Leaf old);
CP_API cp_Leaf cp_mergecut(cp_Store *st, cp_NS ns, cp_Leaf in, cp_Leaf old);

struct cp_MarkCursor {
    cp_State      *S;
    cp_NS          ns;
    size_t         pos;
    cp_Epoch       epoch;
    cp_StoreCursor cur;
    cp_Range       seg;
    cp_Leaf        segid;
    size_t         segi;
    size_t         segn;
    cp_Id          last;
};

struct cp_StyledCursor {
    cp_State      *S;
    size_t         pos;
    cp_Epoch       epoch;
    cp_Range       group;
    size_t         nattr;
    size_t         iattr;
    cp_StoreCursor cur[CP_STORE_MAX];
};

CP_NS_END

#endif /* compositor_h */

/* ==================================================================== */
/*                           IMPLEMENTATION                             */
/* ==================================================================== */

#if defined(CP_IMPLEMENTATION) && !defined(compositor_implemented)
#define compositor_implemented

#include <assert.h>
#include <stdlib.h>
#include <string.h>

CP_NS_BEGIN

typedef struct cpV_Header {
    size_t len, cap;
} cpV_Header;

#define cpV_hdr(A)           ((cpV_Header *)(void *)(A) - 1)
#define cpV_len(A)           ((A) ? cpV_hdr(A)->len : 0u)
#define cpV_cap(A)           ((A) ? cpV_hdr(A)->cap : 0u)
#define cpV_init(A)          ((A) = NULL)
#define cpV_sz(c, s)         (sizeof(cpV_Header) + (c) * (s))
#define cpV_free(S, A)       cpV_resize_(S, (void **)&(A), 0, sizeof(*(A)))
#define cpV_reserve(S, A, n) cpV_reserve_(S, (void **)&(A), (n), sizeof(*(A)))
#define cpV_push(S, A, V) \
    cpV_push_(S, (void **)&(A), (const void *)&(V), sizeof(*(A)))

typedef struct cp_SpanSeg {
    cp_Range range;
    cp_Leaf  id;
} cp_SpanSeg;

typedef struct cp_IdRec {
    cp_NS   ns;
    cp_Attr attr;
    cp_Leaf leaf;
    int     kind;
    int     dead;
    size_t  start;
    size_t  end;
    size_t  sensnext;
} cp_IdRec;

typedef struct cp_RawRec {
    cp_Id   pub;
    cp_Leaf a, b;
    size_t  refcnt;
    size_t  nextfree;
    int     kind; /* 0 free, 1 direct, 2 composite */
} cp_RawRec;

#define CP_LEAF_BASE ((cp_Leaf)1 << 20)
#define CP_LEAF_MAX  64

typedef struct cp_PrioRec {
    cp_Attr attr;
    int     prio;
} cp_PrioRec;

struct cp_Store {
    cp_StoreOps *ops;
    cp_State    *S;
};

typedef struct cp_NSReg {
    cp_Store    store;
    int         flags;
    int         active;
    size_t      nextfree;
    cp_SpanSeg *segs;
    size_t      segn;
    size_t      segcap;
} cp_NSReg;

struct cp_State {
    cp_Alloc   *alloc;
    void       *ud;
    cp_NSReg   *nsv;
    size_t      nsfree;
    size_t      store_count;
    cp_IdRec   *ids;
    cp_RawRec  *raws;
    size_t      rawfree;
    size_t      rawfreecnt;
    cp_PrioRec *prios;
    cp_Epoch    epoch;
    size_t      senshead;
};

static int cpV_resize_(cp_State *S, void **pA, size_t cap, size_t sz) {
    cpV_Header *h, *old = *pA ? cpV_hdr(*pA) : NULL;
    if (old == NULL && cap == 0) return CP_OK;
    if (cap == 0) {
        S->alloc(S->ud, old, cpV_sz(old->cap, sz), 0);
        *pA = NULL;
        return CP_OK;
    }
    h = (cpV_Header *)S->alloc(
            S->ud, old, old ? cpV_sz(old->cap, sz) : 0, cpV_sz(cap, sz));
    if (h == NULL) return CP_ERRMEM;
    if (old == NULL) h->len = 0;
    h->cap = cap;
    *pA = (void *)(h + 1);
    return CP_OK;
}

static int cpV_reserve_(cp_State *S, void **pA, size_t need, size_t sz) {
    cpV_Header *h = *pA ? cpV_hdr(*pA) : NULL;
    size_t      have = h ? h->cap : 0, want = (h ? h->len : 0) + need;
    size_t      cap = have ? have : 4;
    if (have >= want) return CP_OK;
    while (cap < want) cap += cap >> 1;
    return cpV_resize_(S, pA, cap, sz);
}

static int cpV_push_(cp_State *S, void **pA, const void *pv, size_t sz) {
    cpV_Header *h;
    if (cpV_reserve_(S, pA, 1, sz) != CP_OK) return CP_ERRMEM;
    h = cpV_hdr(*pA);
    memcpy((char *)*pA + h->len * sz, pv, sz);
    h->len += 1;
    return CP_OK;
}

static void *cpA_default(void *ud, void *p, size_t osize, size_t nsize) {
    (void)ud, (void)osize;
    if (nsize == 0) return free(p), NULL;
    return realloc(p, nsize);
}

static cp_NSReg *cpN_reg(cp_State *S, cp_NS ns) {
    if (ns <= 0 || (size_t)ns > cpV_len(S->nsv)) return NULL;
    return S->nsv[ns - 1].active ? &S->nsv[ns - 1] : NULL;
}

static cp_IdRec *cpI_slot(cp_State *S, cp_Id id) {
    if (id == 0 || id > cpV_len(S->ids)) return NULL;
    return &S->ids[id - 1];
}

static int cpI_active(const cp_IdRec *r) { return r && !r->dead; }

static cp_IdRec *cpI_rec(cp_State *S, cp_Id id) {
    cp_IdRec *r = cpI_slot(S, id);
    return cpI_active(r) ? r : NULL;
}

static cp_Id cpI_findtoken(const cp_State *S, cp_NS ns, cp_Attr attr) {
    size_t i, n = cpV_len(S->ids);
    for (i = 0; i < n; ++i) {
        if (!S->ids[i].dead && S->ids[i].kind == 1 && S->ids[i].ns == ns
            && S->ids[i].attr == attr)
            return i + 1;
    }
    return 0;
}

static int cpI_add(cp_State *S, cp_NS ns, cp_Attr attr, int kind, cp_Id *out) {
    cp_IdRec rec;
    memset(&rec, 0, sizeof(rec));
    rec.ns = ns, rec.attr = attr, rec.kind = kind;
    if (cpV_push(S, S->ids, rec) != CP_OK) return CP_ERRMEM;
    *out = cpV_len(S->ids);
    return CP_OK;
}

static void cpI_drop(cp_State *S, cp_Id id) {
    cp_IdRec *r = cpI_slot(S, id);
    if (r) r->dead = 1;
}

static cp_RawRec *cpR_slot(cp_State *S, cp_Leaf id) {
    size_t k;
    if (id < CP_LEAF_BASE) return NULL;
    k = (size_t)(id - CP_LEAF_BASE);
    if (k >= cpV_len(S->raws)) return NULL;
    return &S->raws[k];
}

static cp_Leaf cpR_id(size_t k) { return (cp_Leaf)(CP_LEAF_BASE + k); }

static int cpR_add_direct(cp_State *S, cp_Id pub, cp_Leaf *out) {
    cp_RawRec rec;
    memset(&rec, 0, sizeof(rec));
    rec.pub = pub, rec.kind = 1;
    if (cpV_push(S, S->raws, rec) != CP_OK) return CP_ERRMEM;
    *out = cpR_id(cpV_len(S->raws) - 1);
    return CP_OK;
}

static int cpR_reserve(cp_State *S, size_t need) {
    size_t i, have = S->rawfreecnt;
    if (have >= need) return CP_OK;
    if (cpV_reserve(S, S->raws, need - have) != CP_OK) return CP_ERRMEM;
    for (i = 0; i < need - have; ++i) {
        cp_RawRec rec;
        cp_Leaf   id;
        memset(&rec, 0, sizeof(rec));
        rec.kind = 0, rec.nextfree = S->rawfree;
        if (cpV_push(S, S->raws, rec) != CP_OK) return CP_ERRMEM;
        id = cpR_id(cpV_len(S->raws) - 1);
        S->rawfree = id;
        S->rawfreecnt += 1;
    }
    return CP_OK;
}

static cp_Leaf cpR_popfree(cp_State *S) {
    cp_Leaf    id;
    cp_RawRec *r;
    if (S->rawfree == 0) return 0;
    id = S->rawfree;
    r = cpR_slot(S, id);
    S->rawfree = r->nextfree;
    S->rawfreecnt -= 1;
    memset(r, 0, sizeof(*r));
    r->kind = 2;
    return id;
}

static void cpR_free(cp_State *S, cp_Leaf id) {
    cp_RawRec *r = cpR_slot(S, id);
    if (r == NULL || r->kind == 0) return;
    memset(r, 0, sizeof(*r));
    r->nextfree = S->rawfree;
    S->rawfree = id;
    S->rawfreecnt += 1;
}

static void cpR_refup(cp_State *S, cp_Leaf id) {
    cp_RawRec *r = cpR_slot(S, id);
    if (r && r->kind != 0) r->refcnt += 1;
}

static void cpR_refdown(cp_State *S, cp_Leaf id) {
    cp_RawRec *r = cpR_slot(S, id);
    cp_Leaf    a, b;
    if (r == NULL || r->kind == 0 || r->refcnt == 0) return;
    r->refcnt -= 1;
    if (r->refcnt > 0) return;
    if (r->kind == 1) {
        cp_Id     pub = r->pub;
        cp_IdRec *pr = cpI_slot(S, pub);
        if (pr && pr->kind == 2 && !pr->dead) cpI_drop(S, pub);
        return;
    }
    a = r->a, b = r->b;
    cpR_free(S, id);
    cpR_refdown(S, a);
    cpR_refdown(S, b);
}

typedef struct cp_RawPart {
    cp_NS   ns;
    cp_Leaf leaf;
    cp_Id   pub;
} cp_RawPart;

static int cpR_expand_rec(cp_State *S, cp_Leaf id, cp_RawPart *out, int *pn) {
    cp_RawRec *r = cpR_slot(S, id);
    cp_IdRec  *pr;
    if (r == NULL || r->kind == 0) return 0;
    if (*pn >= CP_LEAF_MAX) return 0;
    if (r->kind == 1) {
        pr = cpI_slot(S, r->pub);
        if (pr == NULL || pr->dead) return 0;
        out[*pn].ns = pr->ns;
        out[*pn].leaf = id;
        out[*pn].pub = r->pub;
        *pn += 1;
        return 1;
    }
    if (!cpR_expand_rec(S, r->a, out, pn)) return 0;
    return cpR_expand_rec(S, r->b, out, pn);
}

static int cpR_expand(cp_State *S, cp_Leaf id, cp_RawPart *out, int cap) {
    int n = 0;
    if (cap > CP_LEAF_MAX) cap = CP_LEAF_MAX;
    (void)cap;
    if (!cpR_expand_rec(S, id, out, &n)) return 0;
    return n;
}

static int cpR_has_part(cp_State *S, cp_Leaf id, cp_Leaf target) {
    cp_RawPart parts[CP_LEAF_MAX];
    int        n = cpR_expand(S, id, parts, CP_LEAF_MAX), i;
    for (i = 0; i < n; ++i)
        if (parts[i].leaf == target) return 1;
    return 0;
}

static cp_Leaf cpR_build(cp_State *S, cp_RawPart *parts, int n) {
    cp_Leaf a;
    int     i, j;
    if (n <= 0) return 0;
    if (n == 1) return parts[0].leaf;
    for (i = 1; i < n; ++i) {
        cp_RawPart v = parts[i];
        for (j = i; j > 0 && parts[j - 1].ns > v.ns; --j)
            parts[j] = parts[j - 1];
        parts[j] = v;
    }
    a = parts[0].leaf;
    for (i = 1; i < n; ++i) {
        cp_Leaf    b = parts[i].leaf;
        cp_Leaf    id = cpR_popfree(S);
        cp_RawRec *r;
        if (id == 0) {
            cp_RawRec rec;
            memset(&rec, 0, sizeof(rec));
            rec.kind = 2, rec.a = a, rec.b = b;
            if (cpV_push(S, S->raws, rec) != CP_OK) return 0;
            id = cpR_id(cpV_len(S->raws) - 1);
            r = cpR_slot(S, id);
        } else {
            r = cpR_slot(S, id);
            r->a = a, r->b = b;
        }
        cpR_refup(S, a);
        cpR_refup(S, b);
        a = id;
    }
    return a;
}

static int cpS_storeused(const cp_State *S, const cp_StoreOps *ops) {
    size_t i, n = cpV_len(S->nsv);
    for (i = 0; i < n; ++i) {
        if (S->nsv[i].active && S->nsv[i].store.ops == ops) return 1;
    }
    return 0;
}

static void cpS_queuesens(cp_State *S, cp_Id id) {
    cp_IdRec *r = cpI_slot(S, id);
    if (r == NULL || r->dead || r->kind != 2 || r->sensnext != 0) return;
    if (S->senshead == id) return;
    r->sensnext = S->senshead;
    S->senshead = id;
}

static int cpS_isobj(const cp_NSReg *r) {
    return (r->flags & CP_IDENTITY) != 0;
}

static void cpS_drop_leaf(cp_State *S, cp_Leaf id) {
    cp_RawRec *r = cpR_slot(S, id);
    cp_RawPart parts[CP_LEAF_MAX];
    int        n, i;
    if (r == NULL || r->kind == 0) return;
    n = cpR_expand(S, id, parts, CP_LEAF_MAX);
    cpR_refdown(S, id);
    for (i = 0; i < n; ++i) {
        cp_IdRec *pr = cpI_slot(S, parts[i].pub);
        cp_NSReg *nsr;
        if (pr == NULL || pr->dead || pr->kind != 2) continue;
        nsr = cpN_reg(S, pr->ns);
        if (nsr && (nsr->flags & CP_SENSITIVE)) cpS_queuesens(S, parts[i].pub);
    }
}

static void cpB_born(cp_Store *st, cp_Leaf id) {
    if (st) cpR_refup(st->S, id);
}

static void cpB_drop(cp_Store *st, cp_Leaf id) {
    if (st) cpS_drop_leaf(st->S, id);
}

static void cpB_dropseg(cp_State *S, cp_NSReg *r, size_t i) {
    cp_Store st;
    st.ops = r->store.ops, st.S = S;
    cpB_drop(&st, r->segs[i].id);
    memmove(&r->segs[i], &r->segs[i + 1], (r->segn - i - 1) * sizeof(*r->segs));
    r->segn -= 1;
}

static void cpB_insertseg(
        cp_State *S, cp_NSReg *r, size_t i, cp_Range range, cp_Leaf id) {
    cp_Store st;
    size_t   n = r->segn;
    assert(cpV_cap(r->segs) > r->segn);
    memmove(&r->segs[i + 1], &r->segs[i], (n - i) * sizeof(*r->segs));
    r->segs[i].range = range, r->segs[i].id = id;
    r->segn += 1;
    st.ops = r->store.ops, st.S = S;
    cpB_born(&st, id);
}

static int cpB_write_obj(cp_State *S, cp_NSReg *r, cp_Leaf id, cp_Range range) {
    size_t i = 0, n = r->segn;
    if (cpV_cap(r->segs) <= r->segn) return CP_ERRMEM;
    while (i < n
           && (r->segs[i].range.start < range.start
               || (r->segs[i].range.start == range.start
                   && r->segs[i].id < id)))
        ++i;
    cpB_insertseg(S, r, i, range, id);
    return CP_OK;
}

static void cpB_merge_same(
        cp_State *S, cp_NSReg *r, cp_Leaf id, cp_Range range, size_t *ls,
        size_t *le) {
    size_t i = 0;
    while (i < r->segn) {
        cp_SpanSeg *cur = &r->segs[i];
        if (cur->id == id && cur->range.end >= range.start
            && cur->range.start <= range.end) {
            if (cur->range.start < *ls) *ls = cur->range.start;
            if (cur->range.end > *le) *le = cur->range.end;
            cpB_dropseg(S, r, i);
        } else {
            ++i;
        }
    }
}

static void cpB_split_diff(
        cp_State *S, cp_NSReg *r, cp_Leaf id, cp_Range range) {
    size_t i = 0;
    while (i < r->segn) {
        cp_SpanSeg *cur = &r->segs[i];
        size_t      oldend;
        if (cur->id == id || cur->range.start >= range.end
            || cur->range.end <= range.start) {
            ++i;
            continue;
        }
        oldend = cur->range.end;
        if (cur->range.start < range.start) {
            cur->range.end = range.start;
            if (oldend > range.end) {
                cp_Range part;
                part.start = range.end, part.end = oldend;
                cpB_insertseg(S, r, i + 1, part, cur->id);
                i += 1;
            }
        } else if (oldend > range.end) {
            cur->range.start = range.end;
        } else {
            cpB_dropseg(S, r, i);
            continue;
        }
        ++i;
    }
}

static int cpB_write_token(
        cp_State *S, cp_NSReg *r, cp_Leaf id, cp_Range range) {
    size_t   ls = range.start, le = range.end, i;
    cp_Range rr;
    cpB_merge_same(S, r, id, range, &ls, &le);
    cpB_split_diff(S, r, id, range);
    rr.start = ls, rr.end = le;
    i = 0;
    while (i < r->segn && r->segs[i].range.start < rr.start) ++i;
    cpB_insertseg(S, r, i, rr, id);
    return CP_OK;
}

static int cpB_write(cp_Store *st, cp_NS ns, cp_Leaf id, cp_Range range) {
    cp_State *S = st->S;
    cp_NSReg *r = cpN_reg(S, ns);
    if (r == NULL || range.start >= range.end) return CP_ERRPARAM;
    if (cpS_isobj(r)) return cpB_write_obj(S, r, id, range);
    return cpB_write_token(S, r, id, range);
}

static int cpB_clearleaf(cp_Store *st, cp_NS ns, cp_Leaf id) {
    cp_State *S = st->S;
    cp_NSReg *r = cpN_reg(S, ns);
    size_t    i = 0;
    if (r == NULL) return CP_ERRPARAM;
    while (i < r->segn) {
        if (r->segs[i].id == id)
            cpB_dropseg(S, r, i);
        else
            ++i;
    }
    return CP_OK;
}

static int cpB_clearall(cp_Store *st, cp_NS ns) {
    cp_State *S = st->S;
    cp_NSReg *r = cpN_reg(S, ns);
    size_t    i;
    if (r == NULL) return CP_ERRPARAM;
    for (i = r->segn; i > 0; --i) cpB_dropseg(S, r, i - 1);
    return CP_OK;
}

static int cpB_seek(cp_Store *st, cp_NS ns, size_t off, cp_StoreCursor *c) {
    cp_NSReg *r = cpN_reg(st->S, ns);
    size_t    i;
    if (r == NULL || c == NULL) return CP_ERRPARAM;
    for (i = 0; i < r->segn; ++i) {
        if (r->segs[i].range.end > off) {
            c->sv.pos = i;
            return CP_OK;
        }
    }
    c->sv.pos = r->segn;
    return CP_OK;
}

static int cpB_next(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o) {
    cp_NSReg *r = cpN_reg(st->S, ns);
    if (r == NULL || c == NULL || o == NULL) return CP_ERRPARAM;
    if (c->sv.pos >= r->segn) return 0;
    o->range = r->segs[c->sv.pos].range;
    o->id = r->segs[c->sv.pos].id;
    c->sv.pos += 1;
    return 1;
}

static int cpB_prev(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o) {
    cp_NSReg *r = cpN_reg(st->S, ns);
    if (r == NULL || c == NULL || o == NULL) return CP_ERRPARAM;
    if (c->sv.pos == 0) return 0;
    c->sv.pos -= 1;
    o->range = r->segs[c->sv.pos].range;
    o->id = r->segs[c->sv.pos].id;
    return 1;
}

static cp_StoreOps cpB_ops = {0,        cpB_write, cpB_clearleaf, cpB_clearall,
                              cpB_seek, cpB_next,  cpB_prev};

CP_API cp_StoreOps *cp_spanvec(cp_State *S) {
    (void)S;
    return &cpB_ops;
}

CP_API int cp_init(cp_State *S, cp_Alloc *alloc, void *ud) {
    if (S == NULL) return CP_ERRPARAM;
    memset(S, 0, sizeof(*S));
    S->alloc = alloc ? alloc : cpA_default;
    S->ud = ud;
    return CP_OK;
}

static void cp_freevecs(cp_State *S) {
    size_t i, n = cpV_len(S->nsv);
    for (i = 0; i < n; ++i) cpV_free(S, S->nsv[i].segs);
    cpV_free(S, S->nsv);
    cpV_free(S, S->ids);
    cpV_free(S, S->raws);
    cpV_free(S, S->prios);
}

CP_API void cp_free(cp_State *S) {
    if (S == NULL) return;
    cp_freevecs(S);
    memset(S, 0, sizeof(*S));
}

CP_API void cp_reset(cp_State *S) {
    cp_Alloc *a;
    void     *ud;
    if (S == NULL) return;
    a = S->alloc, ud = S->ud;
    cp_freevecs(S);
    memset(S, 0, sizeof(*S));
    S->alloc = a, S->ud = ud;
}

static int cpN_fill(cp_State *S, size_t upto) {
    cp_NSReg z;
    size_t   i, len = cpV_len(S->nsv);
    if (upto <= len) return CP_OK;
    if (cpV_reserve(S, S->nsv, upto - len) != CP_OK) return CP_ERRMEM;
    memset(&z, 0, sizeof(z));
    for (i = len; i < upto; ++i)
        if (cpV_push(S, S->nsv, z) != CP_OK) return CP_ERRMEM;
    return CP_OK;
}

static int cpN_pop_free(cp_State *S, size_t lo, size_t hi, cp_NS *out) {
    size_t *p = &S->nsfree;
    while (*p != 0) {
        size_t id = *p;
        if (id >= lo && id <= hi) {
            *p = S->nsv[id - 1].nextfree;
            *out = (cp_NS)id;
            return 1;
        }
        p = &S->nsv[id - 1].nextfree;
    }
    return 0;
}

static int cpN_find_inactive(cp_State *S, size_t lo, size_t hi, cp_NS *out) {
    size_t i, n = cpV_len(S->nsv);
    for (i = lo - 1; i < n && i < hi; ++i)
        if (!S->nsv[i].active) {
            *out = (cp_NS)(i + 1);
            return 1;
        }
    return 0;
}

CP_API cp_NS cp_nsnew(cp_State *S, cp_StoreOps *ops, int flags) {
    cp_NSReg reg, *r;
    cp_NS    ns = 0;
    size_t   lo, hi;
    int      bounded, rc;
    if (S == NULL || ops == NULL) return CP_ERRPARAM;
    if ((flags & CP_SENSITIVE) && !(flags & CP_IDENTITY)) return CP_ERRPARAM;
    bounded = ops->nsmax > 0;
    lo = bounded ? 1 : CP_LEAF_MAX + 1;
    hi = bounded ? (size_t)ops->nsmax : (size_t)-1;
    if (bounded && lo > hi) return CP_ELIMIT;
    if (!cpS_storeused(S, ops)) {
        if (S->store_count >= CP_STORE_MAX) return CP_ELIMIT;
        S->store_count += 1;
    }
    memset(&reg, 0, sizeof(reg));
    reg.store.ops = ops, reg.store.S = S;
    reg.flags = flags, reg.active = 1;
    if (cpN_pop_free(S, lo, hi, &ns)
        || cpN_find_inactive(S, lo, hi, &ns)) {
        r = &S->nsv[ns - 1];
        *r = reg;
        return ns;
    }
    if (bounded && cpV_len(S->nsv) >= (size_t)ops->nsmax) {
        if (!cpS_storeused(S, ops)) S->store_count -= 1;
        return CP_ELIMIT;
    }
    ns = (cp_NS)(cpV_len(S->nsv) + 1);
    if ((size_t)ns < lo) ns = (cp_NS)lo;
    if (cpN_fill(S, (size_t)ns - 1) != CP_OK) {
        if (!cpS_storeused(S, ops)) S->store_count -= 1;
        return CP_ERRMEM;
    }
    rc = cpV_push(S, S->nsv, reg);
    if (rc != CP_OK) {
        if (!cpS_storeused(S, ops)) S->store_count -= 1;
        return rc;
    }
    return ns;
}

static void cpN_unregister(cp_State *S, cp_NS ns) {
    cp_NSReg *r = cpN_reg(S, ns);
    size_t    i, n, other = 0;
    if (r == NULL) return;
    n = cpV_len(S->nsv);
    for (i = 0; i < n; ++i) {
        if (&S->nsv[i] != r && S->nsv[i].active
            && S->nsv[i].store.ops == r->store.ops)
            other = 1;
    }
    if (!other) S->store_count -= 1;
    cpV_free(S, r->segs);
    r->active = 0;
    r->nextfree = S->nsfree;
    S->nsfree = (size_t)ns;
    n = cpV_len(S->ids);
    for (i = 0; i < n; ++i) {
        if (!S->ids[i].dead && S->ids[i].ns == ns) S->ids[i].dead = 1;
    }
}

static int cpS_premutate(cp_State *S, cp_NSReg *r);

static int cpS_clear_ns(cp_State *S, cp_NS ns) {
    cp_NSReg *r = cpN_reg(S, ns);
    cp_Store  st;
    int       rc;
    if (r == NULL) return CP_ERRPARAM;
    if (r->store.ops != &cpB_ops) {
        rc = cpS_premutate(S, r);
        if (rc != CP_OK) return rc;
    }
    st.ops = r->store.ops, st.S = S;
    rc = r->store.ops->clearall(&st, ns);
    if (rc != CP_OK) return rc;
    S->epoch += 1;
    return CP_OK;
}

CP_API int cp_nsclear(cp_State *S, cp_NS ns) {
    return S ? cpS_clear_ns(S, ns) : CP_ERRPARAM;
}

CP_API int cp_nsdel(cp_State *S, cp_NS ns) {
    int rc;
    if (S == NULL) return CP_ERRPARAM;
    rc = cpS_clear_ns(S, ns);
    if (rc != CP_OK) return rc;
    cpN_unregister(S, ns);
    return CP_OK;
}

static int cpP_find(const cp_State *S, cp_Attr attr, size_t *out) {
    size_t i, n = cpV_len(S->prios);
    for (i = 0; i < n; ++i) {
        if (S->prios[i].attr == attr) return *out = i, 1;
    }
    return 0;
}

static int cpP_prio(const cp_State *S, cp_Attr attr) {
    size_t i;
    return cpP_find(S, attr, &i) ? S->prios[i].prio : 0;
}

CP_API int cp_setpriority(cp_State *S, cp_Attr attr, int prio) {
    cp_PrioRec rec;
    size_t     i;
    if (S == NULL || attr == CP_NOATTR) return CP_ERRPARAM;
    if (cpP_find(S, attr, &i)) {
        if (S->prios[i].prio == prio) return CP_OK;
        S->prios[i].prio = prio;
    } else {
        if (prio == 0) return CP_OK;
        rec.attr = attr, rec.prio = prio;
        if (cpV_push(S, S->prios, rec) != CP_OK) return CP_ERRMEM;
    }
    S->epoch += 1;
    return CP_OK;
}

static size_t cpS_raw_need(const cp_State *S) {
    return (cpV_len(S->nsv) + 1) * CP_LEAF_MAX + 8;
}

static int cpS_prewrite(cp_State *S, cp_NSReg *r, cp_Leaf leaf) {
    (void)leaf;
    if (r->store.ops == &cpB_ops) return cpV_reserve(S, r->segs, r->segn + 3);
    return cpR_reserve(S, cpS_raw_need(S));
}

static int cpS_premutate(cp_State *S, cp_NSReg *r) {
    if (r->store.ops == &cpB_ops) return CP_OK;
    return cpR_reserve(S, cpS_raw_need(S));
}

static int cpS_write(
        cp_State *S, cp_NS ns, cp_NSReg *r, cp_Leaf leaf, cp_Range range) {
    cp_Store st;
    st.ops = r->store.ops, st.S = S;
    return r->store.ops->write(&st, ns, leaf, range);
}

static int cpS_mark_fail(cp_State *S, int isnew, cp_Id id, int rc) {
    if (isnew) cpI_drop(S, id);
    return rc;
}

static void cpS_setobjrange(cp_State *S, cp_Id id, cp_Range r) {
    cp_IdRec *rec = cpI_rec(S, id);
    if (rec) rec->start = r.start, rec->end = r.end;
}

CP_API int cp_mark(
        cp_State *S, cp_NS ns, cp_Attr attr, cp_Range r, cp_Id *out) {
    cp_NSReg *reg;
    cp_Id     id = 0;
    cp_Leaf   leaf = 0;
    int       rc, kind, isnew = 0;
    if (S == NULL || out == NULL || r.start >= r.end) return CP_ERRPARAM;
    reg = cpN_reg(S, ns);
    if (reg == NULL) return CP_ERRPARAM;
    kind = cpS_isobj(reg) ? 2 : 1;
    if (kind == 1 && attr == CP_NOATTR) return CP_ERRPARAM;
    if (kind == 1) id = cpI_findtoken(S, ns, attr);
    if (id == 0) {
        rc = cpI_add(S, ns, attr, kind, &id);
        if (rc != CP_OK) return rc;
        isnew = 1;
    }
    if (isnew || cpI_rec(S, id)->leaf == 0) {
        rc = cpR_add_direct(S, id, &leaf);
        if (rc != CP_OK) return cpS_mark_fail(S, isnew, id, rc);
        cpI_rec(S, id)->leaf = leaf;
    } else {
        leaf = cpI_rec(S, id)->leaf;
    }
    rc = cpS_prewrite(S, reg, leaf);
    if (rc != CP_OK) return cpS_mark_fail(S, isnew, id, rc);
    rc = cpS_write(S, ns, reg, leaf, r);
    if (rc != CP_OK) return cpS_mark_fail(S, isnew, id, rc);
    if (kind == 2) cpS_setobjrange(S, id, r);
    S->epoch += 1;
    *out = id;
    return CP_OK;
}

static int cpS_count_leaf(cp_State *S, cp_NS ns, cp_Leaf id) {
    cp_NSReg      *r = cpN_reg(S, ns);
    cp_StoreCursor c;
    cp_Seg         seg;
    int            count = 0, rc;
    if (r == NULL || id == 0) return -1;
    rc = r->store.ops->seek(&r->store, ns, 0, &c);
    if (rc != CP_OK) return -1;
    while ((rc = r->store.ops->next(&r->store, ns, &c, &seg)) == 1)
        if (cpR_has_part(S, seg.id, id)) count += 1;
    return rc < 0 ? -1 : count;
}

static int cpS_obj_range(cp_State *S, cp_Id id, cp_Range *out) {
    cp_IdRec      *rec = cpI_rec(S, id);
    cp_NSReg      *r;
    cp_StoreCursor c;
    cp_Seg         seg;
    size_t         lo = (size_t)-1, hi = 0;
    int            found = 0, rc;
    if (rec == NULL || rec->kind != 2 || rec->leaf == 0) return CP_ERRPARAM;
    r = cpN_reg(S, rec->ns);
    if (r == NULL) return CP_ERRPARAM;
    rc = r->store.ops->seek(&r->store, rec->ns, 0, &c);
    if (rc != CP_OK) return rc;
    while ((rc = r->store.ops->next(&r->store, rec->ns, &c, &seg)) == 1) {
        if (!cpR_has_part(S, seg.id, rec->leaf)) continue;
        if (seg.range.start < lo) lo = seg.range.start;
        if (seg.range.end > hi) hi = seg.range.end;
        found = 1;
    }
    if (rc < 0) return rc;
    if (!found) return CP_ERRPARAM;
    out->start = lo, out->end = hi;
    return CP_OK;
}

CP_API int cp_unmark(cp_State *S, cp_Id id) {
    cp_IdRec *rec;
    cp_NSReg *r;
    cp_Store  st;
    cp_Leaf   leaf;
    int       rc, count = 0;
    if (S == NULL) return CP_ERRPARAM;
    rec = cpI_slot(S, id);
    if (rec == NULL) return CP_ERRPARAM;
    if (rec->dead) return rec->kind == 2 ? 0 : CP_ERRPARAM;
    r = cpN_reg(S, rec->ns);
    if (r == NULL) return rec->kind == 2 ? 0 : CP_ERRPARAM;
    leaf = rec->leaf;
    if (leaf == 0) return rec->kind == 2 ? 0 : CP_ERRPARAM;
    if (rec->kind == 2) {
        rc = cpS_premutate(S, r);
        if (rc != CP_OK) return rc;
        st.ops = r->store.ops, st.S = S;
        rc = r->store.ops->clearleaf(&st, rec->ns, leaf);
        if (rc != CP_OK) return rc;
        S->epoch += 1;
        return 1;
    }
    count = cpS_count_leaf(S, rec->ns, leaf);
    if (count < 0) return CP_ERRPARAM;
    if (count == 0) return 0;
    rc = cpS_premutate(S, r);
    if (rc != CP_OK) return rc;
    st.ops = r->store.ops, st.S = S;
    rc = r->store.ops->clearleaf(&st, rec->ns, leaf);
    if (rc != CP_OK) return rc;
    S->epoch += 1;
    return count;
}

CP_API int cp_range(cp_State *S, cp_Id id, cp_Range *out) {
    if (S == NULL || out == NULL) return CP_ERRPARAM;
    return cpS_obj_range(S, id, out);
}

CP_API cp_Attr cp_attr(cp_State *S, cp_Id id) {
    cp_IdRec *r;
    if (S == NULL) return CP_NOATTR;
    r = cpI_rec(S, id);
    return r ? r->attr : CP_NOATTR;
}

CP_API cp_Leaf cp_born(cp_Store *st, cp_Leaf id) {
    if (st == NULL) return id;
    cpB_born(st, id);
    return id;
}

CP_API void cp_died(cp_Store *st, cp_Leaf id) {
    if (st) cpB_drop(st, id);
}

static void cpS_touch_leaf(cp_State *S, cp_Leaf id) {
    cp_RawPart parts[CP_LEAF_MAX];
    int        n = cpR_expand(S, id, parts, CP_LEAF_MAX), i;
    for (i = 0; i < n; ++i) {
        cp_IdRec *pr = cpI_slot(S, parts[i].pub);
        cp_NSReg *nsr;
        if (pr == NULL || pr->dead || pr->kind != 2) continue;
        nsr = cpN_reg(S, pr->ns);
        if (nsr && (nsr->flags & CP_SENSITIVE)) cpS_queuesens(S, parts[i].pub);
    }
}

CP_API void cp_touch(cp_Store *st, cp_Leaf id) {
    if (st) cpS_touch_leaf(st->S, id);
}

static int cpR_part_kept(const cp_RawPart *parts, int n, cp_Leaf leaf) {
    int i;
    for (i = 0; i < n; ++i)
        if (parts[i].leaf == leaf) return 1;
    return 0;
}

static void cpS_drop_old_keep(
        cp_State *S, cp_Leaf old, const cp_RawPart *kept, int n) {
    cp_RawPart parts[CP_LEAF_MAX];
    int        m = cpR_expand(S, old, parts, CP_LEAF_MAX), i;
    cpR_refdown(S, old);
    for (i = 0; i < m; ++i) {
        cp_IdRec *pr;
        cp_NSReg *nsr;
        if (cpR_part_kept(kept, n, parts[i].leaf)) continue;
        pr = cpI_slot(S, parts[i].pub);
        if (pr == NULL || pr->dead || pr->kind != 2) continue;
        nsr = cpN_reg(S, pr->ns);
        if (nsr && (nsr->flags & CP_SENSITIVE)) cpS_queuesens(S, parts[i].pub);
    }
}

static cp_Leaf cpR_finish_merge(
        cp_State *S, cp_RawPart *parts, int n, cp_Leaf old) {
    cp_Leaf ret = cpR_build(S, parts, n);
    if (ret != 0) cpR_refup(S, ret);
    if (old != 0) cpS_drop_old_keep(S, old, parts, n);
    return ret;
}

CP_API cp_Leaf cp_mergewrite(cp_Store *st, cp_NS ns, cp_Leaf in, cp_Leaf old) {
    cp_RawPart parts[CP_LEAF_MAX], ins[CP_LEAF_MAX];
    int        n, ni, i, j;
    if (st == NULL) return in;
    if (in == old) return in;
    n = cpR_expand(st->S, old, parts, CP_LEAF_MAX);
    if (n <= 0) {
        if (old != 0) cpS_drop_leaf(st->S, old);
        if (in != 0) cpR_refup(st->S, in);
        return in;
    }
    for (i = j = 0; i < n; ++i)
        if (parts[i].ns != ns) parts[j++] = parts[i];
    n = j;
    ni = cpR_expand(st->S, in, ins, CP_LEAF_MAX);
    for (i = 0; i < ni; ++i) parts[n++] = ins[i];
    return cpR_finish_merge(st->S, parts, n, old);
}

CP_API cp_Leaf cp_mergeclear(cp_Store *st, cp_NS ns, cp_Leaf old) {
    cp_RawPart parts[CP_LEAF_MAX];
    int        n, i, j;
    if (st == NULL) return 0;
    n = cpR_expand(st->S, old, parts, CP_LEAF_MAX);
    for (i = j = 0; i < n; ++i)
        if (parts[i].ns != ns) parts[j++] = parts[i];
    return cpR_finish_merge(st->S, parts, j, old);
}

CP_API cp_Leaf cp_mergecut(cp_Store *st, cp_NS ns, cp_Leaf in, cp_Leaf old) {
    cp_RawPart parts[CP_LEAF_MAX];
    int        n, i, j;
    if (st == NULL) return in;
    n = cpR_expand(st->S, old, parts, CP_LEAF_MAX);
    for (i = j = 0; i < n; ++i)
        if (parts[i].ns != ns || parts[i].leaf != in) parts[j++] = parts[i];
    return cpR_finish_merge(st->S, parts, j, old);
}

static int cpS_clearspanvecs(cp_State *S) {
    size_t i, n = cpV_len(S->nsv);
    for (i = 0; i < n; ++i) {
        cp_NSReg *r = &S->nsv[i];
        cp_Store  st;
        int       rc;
        if (!r->active || r->store.ops != &cpB_ops) continue;
        st.ops = r->store.ops, st.S = S;
        rc = r->store.ops->clearall(&st, (cp_NS)(i + 1));
        if (rc != CP_OK) return rc;
    }
    return CP_OK;
}

static int cpS_dropsens(cp_State *S, cp_Id id) {
    cp_IdRec *r = cpI_slot(S, id);
    cp_NSReg *nsr;
    cp_Store  st;
    if (r == NULL || r->dead || r->kind != 2) return CP_OK;
    nsr = cpN_reg(S, r->ns);
    if (nsr == NULL) return CP_ERRPARAM;
    if (r->leaf == 0) return CP_OK;
    st.ops = nsr->store.ops, st.S = S;
    return nsr->store.ops->clearleaf(&st, r->ns, r->leaf);
}

static int cpS_preflight_sens(cp_State *S) {
    cp_Id id;
    for (id = S->senshead; id != 0;) {
        cp_IdRec *r = cpI_slot(S, id);
        cp_NSReg *nsr;
        int       rc;
        if (r == NULL) break;
        id = r->sensnext;
        if (r->dead || r->kind != 2) continue;
        nsr = cpN_reg(S, r->ns);
        if (nsr == NULL) return CP_ERRPARAM;
        if (nsr->store.ops == &cpB_ops) continue;
        rc = cpS_premutate(S, nsr);
        if (rc != CP_OK) return rc;
    }
    return CP_OK;
}

CP_API int cp_commit(cp_State *S) {
    cp_Id id;
    int   rc;
    if (S == NULL) return CP_ERRPARAM;
    rc = cpS_clearspanvecs(S);
    if (rc != CP_OK) return rc;
    rc = cpS_preflight_sens(S);
    if (rc != CP_OK) {
        S->epoch += 1;
        return rc;
    }
    id = S->senshead;
    while (id) {
        cp_Id next = cpI_slot(S, id)->sensnext;
        rc = cpS_dropsens(S, id);
        if (rc != CP_OK) {
            S->epoch += 1;
            return rc;
        }
        S->senshead = next;
        id = next;
    }
    S->senshead = 0;
    S->epoch += 1;
    return CP_OK;
}

CP_API cp_Epoch cp_epoch(const cp_State *S) {
    return S ? (cp_Epoch)S->epoch : 0;
}

/* ---- mark iteration ---- */

typedef struct cp_MarkItem {
    cp_Range range;
    cp_Id    id;
    int      kind;
} cp_MarkItem;

static int cpM_cmp(const cp_MarkItem *a, const cp_MarkItem *b) {
    if (a->range.start != b->range.start)
        return a->range.start < b->range.start ? -1 : 1;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    return 0;
}

static int cpM_collect_obj(cp_State *S, cp_NS ns, cp_MarkItem **items) {
    size_t i, n = cpV_len(S->ids);
    for (i = 0; i < n; ++i) {
        cp_IdRec   *rec = &S->ids[i];
        cp_MarkItem it;
        cp_Range    range;
        int         rc;
        if (rec->dead || rec->kind != 2 || rec->ns != ns) continue;
        rc = cpS_obj_range(S, (cp_Id)(i + 1), &range);
        if (rc == CP_ERRPARAM) continue;
        if (rc != CP_OK) return rc;
        it.range = range;
        it.id = i + 1, it.kind = 2;
        if (cpV_push(S, *items, it) != CP_OK) {
            cpV_free(S, *items);
            return CP_ERRMEM;
        }
    }
    return CP_OK;
}

static int cpM_collect_tok(cp_State *S, cp_NS ns, cp_MarkItem **items) {
    cp_NSReg      *r = cpN_reg(S, ns);
    cp_StoreCursor c;
    cp_Seg         seg;
    int            rc;
    if (r == NULL) return CP_ERRPARAM;
    rc = r->store.ops->seek(&r->store, ns, 0, &c);
    if (rc != CP_OK) return rc;
    while ((rc = r->store.ops->next(&r->store, ns, &c, &seg)) == 1) {
        cp_RawPart parts[CP_LEAF_MAX];
        int        n = cpR_expand(S, seg.id, parts, CP_LEAF_MAX), i;
        for (i = 0; i < n; ++i) {
            cp_MarkItem it;
            cp_IdRec   *rec;
            if (parts[i].ns != ns) continue;
            rec = cpI_slot(S, parts[i].pub);
            if (rec == NULL || rec->dead || rec->kind != 1) continue;
            it.range = seg.range;
            it.id = parts[i].pub;
            it.kind = 1;
            if (cpV_push(S, *items, it) != CP_OK) {
                cpV_free(S, *items);
                return CP_ERRMEM;
            }
        }
    }
    return rc < 0 ? rc : CP_OK;
}

static int cpM_collect(cp_State *S, cp_NS ns, cp_MarkItem **out) {
    cp_NSReg    *r = cpN_reg(S, ns);
    cp_MarkItem *items = NULL;
    size_t       i, n;
    int          rc;
    if (r == NULL) return CP_ERRPARAM;
    rc = cpS_isobj(r) ? cpM_collect_obj(S, ns, &items)
                      : cpM_collect_tok(S, ns, &items);
    if (rc != CP_OK) return rc;
    n = cpV_len(items);
    for (i = 1; i < n; ++i) {
        cp_MarkItem v = items[i];
        size_t      j = i;
        while (j > 0 && cpM_cmp(&items[j - 1], &v) > 0) {
            items[j] = items[j - 1];
            --j;
        }
        items[j] = v;
    }
    *out = items;
    return CP_OK;
}

static int cpM_first(cp_MarkItem *items, size_t pos, size_t *idx) {
    size_t i, n = cpV_len(items);
    for (i = 0; i < n; ++i)
        if (items[i].range.end > pos) return *idx = i, 1;
    return 0;
}

CP_API int cp_seek(cp_MarkCursor *MC, cp_State *S, cp_NS ns, size_t pos) {
    cp_MarkItem *items = NULL;
    size_t       idx = 0;
    int          rc, found;
    if (MC == NULL || S == NULL) return CP_ERRPARAM;
    memset(MC, 0, sizeof(*MC));
    rc = cpM_collect(S, ns, &items);
    if (rc != CP_OK) return rc;
    found = cpM_first(items, pos, &idx);
    MC->S = S, MC->ns = ns, MC->pos = pos;
    MC->epoch = (cp_Epoch)S->epoch;
    MC->segi = found ? idx : 0;
    MC->segn = found ? cpV_len(items) : 0;
    cpV_free(S, items);
    return CP_OK;
}

CP_API int cp_next(cp_MarkCursor *MC, cp_Mark *out) {
    cp_MarkItem *items = NULL;
    cp_MarkItem  it;
    size_t       i;
    int          rc;
    if (MC == NULL || out == NULL || MC->S == NULL) return CP_ERRPARAM;
    if ((cp_Epoch)MC->S->epoch != MC->epoch) return CP_ESTALE;
    if (MC->segi >= MC->segn) return 0;
    rc = cpM_collect(MC->S, MC->ns, &items);
    if (rc != CP_OK) return rc;
    i = MC->segi;
    it = items[i];
    cpV_free(MC->S, items);
    out->range = it.range, out->id = it.id;
    out->attr = cp_attr(MC->S, it.id);
    MC->segi += 1;
    return 1;
}

/* ---- styled iteration ---- */

typedef struct cp_Contrib {
    cp_Range range;
    cp_Attr  attr;
    int      prio;
} cp_Contrib;

static int cpC_less(const cp_Contrib *a, const cp_Contrib *b) {
    if (a->prio != b->prio) return a->prio < b->prio;
    return a->attr < b->attr;
}

static void cpC_sort(cp_Contrib *v, size_t n) {
    size_t i;
    for (i = 1; i < n; ++i) {
        cp_Contrib x = v[i];
        size_t     j = i;
        while (j > 0 && cpC_less(&x, &v[j - 1])) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = x;
    }
}

static int cpC_add_obj(cp_State *S, cp_NS ns, cp_Contrib **v) {
    size_t k, m = cpV_len(S->ids);
    for (k = 0; k < m; ++k) {
        cp_IdRec  *rec = &S->ids[k];
        cp_Contrib c;
        cp_Range   range;
        int        rc;
        if (rec->dead || rec->kind != 2 || rec->ns != ns
            || rec->attr == CP_NOATTR)
            continue;
        rc = cpS_obj_range(S, (cp_Id)(k + 1), &range);
        if (rc == CP_ERRPARAM) continue;
        if (rc != CP_OK) return rc;
        c.range = range;
        c.attr = rec->attr;
        c.prio = cpP_prio(S, rec->attr);
        if (cpV_push(S, *v, c) != CP_OK) {
            cpV_free(S, *v);
            return CP_ERRMEM;
        }
    }
    return CP_OK;
}

static int cpC_add_tok(cp_State *S, cp_NS ns, cp_Contrib **v) {
    cp_NSReg      *r = cpN_reg(S, ns);
    cp_StoreCursor c;
    cp_Seg         seg;
    int            rc;
    if (r == NULL) return CP_ERRPARAM;
    rc = r->store.ops->seek(&r->store, ns, 0, &c);
    if (rc != CP_OK) return rc;
    while ((rc = r->store.ops->next(&r->store, ns, &c, &seg)) == 1) {
        cp_RawPart parts[CP_LEAF_MAX];
        int        n = cpR_expand(S, seg.id, parts, CP_LEAF_MAX), i;
        for (i = 0; i < n; ++i) {
            cp_IdRec  *rec;
            cp_Contrib c;
            if (parts[i].ns != ns) continue;
            rec = cpI_slot(S, parts[i].pub);
            if (rec == NULL || rec->dead || rec->kind != 1
                || rec->attr == CP_NOATTR)
                continue;
            c.range = seg.range;
            c.attr = rec->attr;
            c.prio = cpP_prio(S, rec->attr);
            if (cpV_push(S, *v, c) != CP_OK) {
                cpV_free(S, *v);
                return CP_ERRMEM;
            }
        }
    }
    return rc < 0 ? rc : CP_OK;
}

static int cpC_collect(cp_State *S, cp_Contrib **out) {
    cp_Contrib *v = NULL;
    size_t      i, n = cpV_len(S->nsv);
    for (i = 0; i < n; ++i) {
        cp_NSReg *r = &S->nsv[i];
        int       rc;
        if (!r->active) continue;
        rc = cpS_isobj(r) ? cpC_add_obj(S, (cp_NS)(i + 1), &v)
                          : cpC_add_tok(S, (cp_NS)(i + 1), &v);
        if (rc != CP_OK) return rc;
    }
    *out = v;
    return CP_OK;
}

static int cpC_fill(
        cp_State *S, cp_Contrib *all, size_t x, cp_Contrib **active,
        size_t *gstart, size_t *next) {
    size_t i, n = cpV_len(all);
    *gstart = 0, *next = (size_t)-1;
    for (i = 0; i < n; ++i) {
        if (all[i].range.start <= x && all[i].range.end > x) {
            if (cpV_push(S, *active, all[i]) != CP_OK) return CP_ERRMEM;
            if (all[i].range.start > *gstart) *gstart = all[i].range.start;
        } else if (all[i].range.start < x && all[i].range.end <= x) {
            if (all[i].range.end > *gstart) *gstart = all[i].range.end;
        }
        if (all[i].range.start > x
            && (*next == (size_t)-1 || all[i].range.start < *next))
            *next = all[i].range.start;
    }
    return CP_OK;
}

static int cpC_finish(
        cp_Contrib *active, size_t gstart, size_t next, cp_Range *grange,
        cp_Contrib **pactive, size_t *pnattr) {
    size_t i, n = 0;
    grange->start = gstart;
    grange->end = next;
    for (i = 0; i < cpV_len(active); ++i) {
        if (active[i].range.end < grange->end)
            grange->end = active[i].range.end;
    }
    cpC_sort(active, cpV_len(active));
    for (i = 0; i < cpV_len(active); ++i) {
        if (n == 0 || active[n - 1].attr != active[i].attr)
            active[n++] = active[i];
    }
    cpV_hdr(active)->len = n;
    *pactive = active;
    *pnattr = n;
    return 1;
}

static int cpC_group(
        cp_State *S, size_t x, cp_Range *grange, cp_Contrib **pactive,
        size_t *pnattr) {
    cp_Contrib *all = NULL, *active = NULL;
    size_t      gstart, next;
    int         rc;
    rc = cpC_collect(S, &all);
    if (rc != CP_OK) return rc;
    rc = cpC_fill(S, all, x, &active, &gstart, &next);
    if (rc != CP_OK) {
        cpV_free(S, all), cpV_free(S, active);
        return rc;
    }
    if (cpV_len(active) == 0 && next == (size_t)-1) {
        cpV_free(S, all), cpV_free(S, active);
        return 0;
    }
    if (cpV_len(active) == 0) {
        cpV_free(S, all), cpV_free(S, active);
        return cpC_group(S, next, grange, pactive, pnattr);
    }
    cpV_free(S, all);
    return cpC_finish(active, gstart, next, grange, pactive, pnattr);
}

CP_API int cp_seekstyled(cp_StyledCursor *SC, cp_State *S, size_t pos) {
    cp_Contrib *act = NULL;
    cp_Range    gr;
    size_t      nattr = 0;
    int         rc;
    if (SC == NULL || S == NULL) return CP_ERRPARAM;
    memset(SC, 0, sizeof(*SC));
    rc = cpC_group(S, pos, &gr, &act, &nattr);
    if (rc <= 0) {
        cpV_free(S, act);
        if (rc < 0) return rc;
        SC->S = S, SC->epoch = (cp_Epoch)S->epoch;
        SC->pos = pos, SC->nattr = 0;
        return CP_OK;
    }
    SC->S = S, SC->epoch = (cp_Epoch)S->epoch;
    SC->pos = gr.start, SC->group = gr, SC->nattr = nattr, SC->iattr = 0;
    cpV_free(S, act);
    return CP_OK;
}

CP_API int cp_nextstyled(cp_StyledCursor *SC, cp_Styled *out) {
    cp_Contrib *act = NULL;
    cp_Range    gr;
    size_t      nattr = 0, idx;
    int         rc;
    if (SC == NULL || out == NULL || SC->S == NULL) return CP_ERRPARAM;
    if ((cp_Epoch)SC->S->epoch != SC->epoch) return CP_ESTALE;
    rc = cpC_group(SC->S, SC->pos, &gr, &act, &nattr);
    if (rc <= 0) {
        cpV_free(SC->S, act);
        return rc < 0 ? rc : 0;
    }
    if (gr.start != SC->group.start || gr.end != SC->group.end) {
        SC->group = gr, SC->nattr = nattr, SC->iattr = 0;
    }
    idx = SC->iattr;
    out->range = SC->group;
    out->attr = act[idx].attr;
    SC->iattr += 1;
    out->last = (SC->iattr >= SC->nattr) ? 1 : 0;
    if (out->last) SC->pos = SC->group.end;
    cpV_free(SC->S, act);
    return 1;
}

CP_NS_END

#endif /* CP_IMPLEMENTATION */
