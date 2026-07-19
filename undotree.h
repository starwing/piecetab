#ifndef undotree_h
#define undotree_h

#ifndef UT_NS_BEGIN
# ifdef __cplusplus
#   define UT_NS_BEGIN extern "C" {
#   define UT_NS_END   }
# else
#   define UT_NS_BEGIN
#   define UT_NS_END
# endif
#endif

#ifndef UT_STATIC
# if __GNUC__
#   define UT_STATIC static __attribute((unused))
# else
#   define UT_STATIC static
# endif
#endif

#ifdef UT_STATIC_API
# ifndef UT_IMPLEMENTATION
#   define UT_IMPLEMENTATION
# endif
# define UT_API UT_STATIC
#endif

#if !defined(UT_API) && defined(_WIN32)
# ifdef UT_IMPLEMENTATION
#   define UT_API __declspec(dllexport)
# else
#   define UT_API __declspec(dllimport)
# endif
#endif

#ifndef UT_API
# define UT_API extern
#endif

#include <stddef.h>

#define UT_OK       (0)
#define UT_ERRPARAM (-1)
#define UT_ERRMEM   (-2)

UT_NS_BEGIN

typedef struct ut_State   ut_State;
typedef struct ut_Tree    ut_Tree;
typedef struct ut_Payload ut_Payload;
typedef struct ut_Node    ut_Node;
typedef struct ut_Hunk    ut_Hunk;

typedef void *ut_Alloc(void *ud, void *p, size_t osize, size_t nsize);
typedef void  ut_Cleaner(void *ud, ut_Payload *p);

typedef const ut_Node *ut_Vid;

/*lifecycle */

UT_API ut_State *ut_open(ut_Alloc *allocf, void *ud);
UT_API void      ut_reset(ut_State *S);
UT_API void      ut_close(ut_State *S);

UT_API void ut_setcleaner(ut_State *S, ut_Cleaner *f, void *ud);

UT_API ut_Tree *ut_newtree(ut_State *S, ut_Payload *pl);
UT_API void     ut_deltree(ut_State *S, ut_Tree *T);

/* navigate */

#define ut_payload(v)    ((v) ? (v)->payload : NULL)
#define ut_parent(v)     ((v) ? (v)->parent : NULL)
#define ut_childcount(v) ((v) ? (int)(v)->child_count : 0)
#define ut_firstchild(v) (ut_lastchild(v) ? (v)->last_child->next_sib : NULL)
#define ut_lastchild(v)  ((v) ? (v)->last_child : NULL)
#define ut_nextsib(c)    ((c) ? (c)->next_sib : NULL)
#define ut_root(T)       ((T) ? &(T)->root : NULL)
#define ut_current(T)    ((T) ? (T)->current : NULL)

UT_API ut_Vid ut_younger(ut_Vid v);
UT_API ut_Vid ut_older(ut_Vid v);
UT_API ut_Vid ut_ancestor(ut_Vid a, ut_Vid b);

/* journal */

#define ut_freshcount(T) ((T) ? (int)utV_len((T)->journal) : 0)

UT_API int  ut_record(ut_Tree *T, size_t off, size_t del, size_t ins);
UT_API void ut_unrecord(ut_Tree *T, unsigned n);

UT_API ut_Vid ut_commit(ut_Tree *T, ut_Payload *p);
UT_API int    ut_discard(ut_Tree *T);

/* diff */

#define ut_freshvid(S) ((ut_Vid)(S))

UT_API int ut_switch(ut_Tree *T, ut_Vid v);
UT_API int ut_diff(ut_Tree *T, ut_Vid from, ut_Vid to);
UT_API int ut_freshdiff(ut_Tree *T, int i, int j);

UT_API const ut_Hunk *ut_hunks(ut_Tree *T, size_t *pn);

/* private struction definition */

#define utV_len(A) ((A) ? utV_hdr(A)->len : 0u)
#define utV_hdr(A) ((utV_Header *)(A) - 1)

/* clang-format off */
typedef struct { size_t off, del, ins; } ut_Entry;  /* journal entry */
typedef struct { unsigned len, cap; } utV_Header;
/* clang-format on */

struct ut_Hunk {
    size_t pa;   /* parent: delete start offset            */
    size_t ca;   /* child:  insert start offset            */
    size_t pdel; /* parent: bytes deleted                  */
    size_t cins; /* child:  bytes inserted                 */
};

struct ut_Node {
    ut_Node    *parent;      /* parent node (NULL at root)              */
    ut_Node    *last_child;  /* youngest (last-committed) child         */
    ut_Node    *next_sib;    /* → older sibling (oldest wraps to young) */
    ut_Payload *payload;     /* caller snapshot (e.g. pt_Buffer)        */
    ut_Hunk    *h;           /* vec: parent→this changeset              */
    int         depth;       /* distance from root                      */
    int         child_count; /* number of children                       */
};

struct ut_Tree {
    ut_Node   root;    /* root node (embedded)             */
    ut_State *S;       /* owning state                     */
    ut_Node  *current; /* current head node                */
    ut_Entry *journal; /* vec of uncommitted edits         */
    int       diffhn;  /* pending diff hunk count, -1=none */
};

UT_NS_END

#endif /* undotree_h */

/* ======================================================================== */
/*                           IMPLEMENTATION                                 */
/* ======================================================================== */

#if defined(UT_IMPLEMENTATION) && !defined(ut_implemented)
#define ut_implemented

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef UT_PAGE_SIZE
# define UT_PAGE_SIZE 65536
#endif /* UT_PAGE_SIZE */

#define UT_MAX_VECLEN INT_MAX

#define ut_min(a, b) ((a) < (b) ? (a) : (b))
#define ut_max(a, b) ((a) > (b) ? (a) : (b))

UT_NS_BEGIN

/* clang-format off */
typedef struct ut_Pool {
    size_t obj_size;  /* size of each object in this pool */
    void  *freed;     /* freelist head */
    void  *pages;     /* linked list of allocated pages */
#ifdef UT_POOL_STATS
    size_t live_obj;
#endif
} ut_Pool;
/*clang-format on */

struct ut_State {
    ut_Node     base;      /* must be first; sentinel for ut_freshvid */
    ut_Alloc   *allocf;    /* lua_Alloc-style realloc                  */
    void       *ud;        /* userdata for allocf/cleaner              */
    ut_Pool     node_pool; /* pool for ut_Node objects                 */
    ut_Cleaner *cleaner;   /* payload release callback                  */
    void       *cud;       /* userdata for cleaner                      */
    ut_Hunk    *scratch;   /* vec: diff/compose temp buffer             */
};

/*  memory helpers */

#define utOK(call, cleanup)                                   \
    do {                                                      \
        int r;                                                \
        if ((r = (call)) != UT_OK) return (void)(cleanup), r; \
    } while (0)

#define utV_sz(cap, sz) (sizeof(utV_Header) + cap * sz)
#define utV_init(A)     ((A) = NULL)
#define utV_end(A)      ((A) + utV_len(A))
#define utV_pop(A)      (utV_len(A) ? --utV_hdr(A)->len : 0)
#define utV_free(S, A)  utV_resize_(S, (void **)&(A), 0, sizeof(*(A)))

#define utV_push(S, A, V)          \
    (utV_reserve(S, A, 1) != UT_OK \
             ? UT_ERRMEM           \
             : (*utV_end(A) = (V), ++utV_hdr(A)->len, UT_OK))

#define utV_reserve(S, A, N) \
    utV_grow_(S, (void **)&(A), (unsigned)(N), sizeof(*(A)))

static int utV_resize_(ut_State *S, void **pA, unsigned cap, size_t objsz) {
    utV_Header *hdr, *old = (*pA ? utV_hdr(*pA) : NULL);
    if (old == NULL && cap == 0) return UT_OK;
    hdr = (utV_Header *)S->allocf(
            S->ud, old, old ? utV_sz(old->cap, objsz) : 0, utV_sz(cap, objsz));
    if (hdr == NULL) return (cap == 0) ? (*pA = NULL, UT_OK) : UT_ERRMEM;
    if (!old) hdr->len = hdr->cap = 0;
    return hdr->cap = cap, *pA = (void *)(hdr + 1), UT_OK;
}

static int utV_grow_(ut_State *S, void **pA, unsigned need, size_t objsz) {
    utV_Header *hdr;
    unsigned    c = 0, e = need, nc = 4;
    hdr = (*pA ? (utV_Header *)(*pA) - 1 : NULL);
    if (hdr) c = hdr->cap, e += hdr->len;
    if (c < e) {
        while (nc < e && nc < (UT_MAX_VECLEN / objsz)) nc += nc >> 1;
        if (nc < e) return UT_ERRMEM;
        return utV_resize_(S, pA, nc, objsz);
    }
    return UT_OK;
}

/* pool allocator */

#ifdef UT_POOL_STATS
# define utP_stat(stmt) stmt
#else
# define utP_stat(stmt) ((void)0)
#endif

/* clang-format off */
static void utP_free(ut_Pool *p, void *obj)
{ utP_stat(p->live_obj -= 1), *(void **)obj = p->freed, p->freed = obj; }
/* clang-format on */

static void utP_init(ut_Pool *p, size_t obj_size) {
    memset(p, 0, sizeof(ut_Pool)), p->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < UT_PAGE_SIZE / 4);
}

static void utP_destroy(ut_State *S, ut_Pool *p) {
    void *next, *page = p->pages;
    for (; page; page = next) {
        next = *(void **)((char *)page + UT_PAGE_SIZE - sizeof(void *));
        S->allocf(S->ud, page, UT_PAGE_SIZE, 0);
    }
    utP_init(p, p->obj_size);
}

static void *utP_alloc(ut_State *S, ut_Pool *p) {
    size_t sz = p->obj_size;
    char  *end, *page = p->freed;
    utP_stat(p->live_obj += 1);
    if (page) return (p->freed = *(void **)page), page;
    page = (char *)S->allocf(S->ud, NULL, 0, UT_PAGE_SIZE);
    if (page == NULL) return utP_stat(p->live_obj -= 1), NULL;
    end = &page[UT_PAGE_SIZE - sizeof(void *)], *(void **)end = p->pages;
    p->pages = (void *)page, page += sz, end -= sz;
    while ((page += sz) <= end) *(void **)(page - sz) = page;
    *(void **)(page - sz) = p->freed;
    return (p->freed = (void *)((char *)p->pages + sz)), p->pages;
}

/*  lifecycle */

#define utN_alloc(S) ((ut_Node *)utP_alloc((S), &(S)->node_pool))

/* clang-format off */
UT_API void ut_reset(ut_State *S)
{ if (S) utP_destroy(S, &S->node_pool), utV_free(S, S->scratch); }

UT_API void ut_close(ut_State *S)
{ if (S) ut_reset(S), S->allocf(S->ud, S, sizeof(ut_State), 0); }

UT_API void ut_setcleaner(ut_State *S, ut_Cleaner *f, void *ud)
{ if (S) S->cleaner = f, S->cud = ud; }
/* clang-format on */

static void *utS_defallocf(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    (void)ud, (void)osize;
    if (nsize == 0) return free(p), NULL;
    return (np = realloc(p, nsize)) ? np : ((void)abort(), NULL);
}

UT_API ut_State *ut_open(ut_Alloc *allocf, void *ud) {
    ut_State *S;
    if (allocf == NULL) allocf = &utS_defallocf;
    S = (ut_State *)allocf(ud, NULL, 0, sizeof(ut_State));
    if (S == NULL) return NULL;
    memset(S, 0, sizeof(ut_State));
    utP_init(&S->node_pool, sizeof(ut_Node));
    return S->allocf = allocf, S->ud = ud, S;
}

UT_API ut_Tree *ut_newtree(ut_State *S, ut_Payload *pl) {
    ut_Tree *T;
    if (S == NULL) return NULL;
    T = (ut_Tree *)S->allocf(S->ud, NULL, 0, sizeof(ut_Tree));
    if (T == NULL) return NULL;
    memset(T, 0, sizeof(ut_Tree));
    T->root.payload = pl, T->S = S, T->current = &T->root;
    return utV_init(T->journal), T->diffhn = -1, T;
}

static void utN_freechildren(ut_State *S, ut_Node *root) {
    ut_Node *n = root, *p;
    /* descend to deepest leaf via oldest children */
    while (n->last_child) n = n->last_child->next_sib;
    while (n != root) {
        /* always remove oldest child: last_child->next_sib points to oldest */
        if ((p = n->parent)->child_count == 1)
            p->last_child = NULL;
        else
            p->last_child->next_sib = n->next_sib;
        p->child_count--;
        if (S->cleaner) S->cleaner(S->cud, n->payload);
        utV_free(S, n->h), utP_free(&S->node_pool, n), n = p;
        /* ascend to parent, then re-descend to deepest leaf */
        while (n->last_child) n = n->last_child->next_sib;
    }
}

UT_API void ut_deltree(ut_State *S, ut_Tree *T) {
    if (T == NULL) return;
    utN_freechildren(S, &T->root), utV_free(S, T->journal);
    if (S->cleaner) S->cleaner(S->cud, T->root.payload);
    utV_free(S, T->root.h), S->allocf(S->ud, T, sizeof(ut_Tree), 0);
}

UT_API const ut_Hunk *ut_hunks(ut_Tree *T, size_t *pn) {
    if (T == NULL || pn == NULL) return NULL;
    if (T->diffhn < 0) {
        if (T->current == NULL) return *pn = 0, NULL;
        return *pn = utV_len(T->current->h), T->current->h;
    }
    return *pn = T->diffhn, T->S->scratch;
}

UT_API int ut_record(ut_Tree *T, size_t off, size_t del, size_t ins) {
    ut_Entry e;
    if (T == NULL) return UT_ERRPARAM;
    if (del == 0 && ins == 0) return UT_OK;
    e.off = off, e.del = del, e.ins = ins;
    utOK(utV_push(T->S, T->journal, e), 0);
    return UT_OK;
}

UT_API void ut_unrecord(ut_Tree *T, unsigned n) {
    unsigned len;
    if (T == NULL || (len = utV_len(T->journal)) == 0) return;
    utV_hdr(T->journal)->len = len >= n ? len - n : 0;
}

/* hunk algebra helpers */

typedef struct ut_Merge {
    ut_State      *S;    /* alloc state                          */
    ut_Hunk      **out;  /* output hunk vec (X→Z)                */
    const ut_Hunk *x2y;  /* A: X→Y changeset                     */
    const ut_Hunk *y2z;  /* B: Y→Z changeset                     */
    ptrdiff_t      xoff; /* Σ(cins-pdel) over processed A hunks  */
    ptrdiff_t      zoff; /* Σ(cins-pdel) over processed B hunks  */
} ut_Merge;

typedef const ut_Hunk *ut_HunkCV; /* read-only hunk vec */

/*
 * Compose: X → Y → Z, output X → Z.
 *
 *          ,- pa <- A (x2y)
 *     X  --[pdel]--        ,- pa <- B (y2z)
 *          [cins]-->  Y  --[pdel]--           ΔA = cins−pdel
 *          `- ca           [cins]-->  Z       ΔB = cins−pdel
 *                          `- ca
 *
 * xoff = ΣΔA(processed)  tracks how far X has drifted from Y
 * zoff = ΣΔB(processed)  tracks how far Y has drifted from Z
 *
 * emitX2Y: A_i unblocked  |  emitY2Z: B_j unblocked
 *   X: pa  [pdel]         |    X: pa−xoff  [pdel]  ← shift back by prior A
 *   Y: ca  [cins]         |    Y: pa       [pdel]  ← B_j in Y
 *         ↓ +zoff         |    Z: ca       [cins]  ← unchanged
 *   Z: ca+zoff [cins]     |
 *
 * emitcross: A_i ∩ B_j overlap in Y
 *   surv = A.cins − B.pdel  (>0: A insert survives, <0: B del overflows)
 *   del  = A.pdel + max(0,−surv)     ins = max(0,surv) + B.cins
 *   pa   = min(A.pa, B.pa−xoff)      ca  = min(A.ca+zoff, B.ca)
 */

static int utH_emitX2Y(ut_Merge *M, const ut_Hunk *h) {
    ut_Hunk hk = *h;
    assert(hk.pdel || hk.cins);
    hk.ca += M->zoff, M->xoff += (ptrdiff_t)h->cins - (ptrdiff_t)h->pdel;
    return utV_push(M->S, *M->out, hk);
}

static int utH_emitY2Z(ut_Merge *M, const ut_Hunk *h) {
    ut_Hunk hk = *h;
    assert(hk.pdel || hk.cins);
    hk.pa -= M->xoff, M->zoff += (ptrdiff_t)h->cins - (ptrdiff_t)h->pdel;
    return utV_push(M->S, *M->out, hk);
}

static int utH_emitcross(ut_Merge *M, int i, int j) {
    const ut_Hunk *a = &M->x2y[i], *b = &M->y2z[j];
    ptrdiff_t      surv = (ptrdiff_t)a->cins - (ptrdiff_t)b->pdel;
    ut_Hunk        m;
    m.pa = ut_min(a->pa, b->pa - M->xoff);     /* X start: from A or B */
    m.pdel = a->pdel + (surv < 0 ? -surv : 0); /* A del + B overflow */
    m.ca = ut_min(a->ca + M->zoff, b->ca);     /* Z start: from A or B */
    m.cins = (surv > 0 ? surv : 0) + b->cins;  /* A surplus + B ins*/
    M->xoff += (ptrdiff_t)a->cins - (ptrdiff_t)a->pdel;
    M->zoff += (ptrdiff_t)b->cins - (ptrdiff_t)b->pdel;
    return (m.pdel || m.cins) ? utV_push(M->S, *M->out, m) : UT_OK;
}

static int utH_mergewalk(ut_Merge *M) {
    int i = 0, j = 0, an = (int)utV_len(M->x2y), bn = (int)utV_len(M->y2z);
    while (i < an && j < bn) {
        size_t ae = M->x2y[i].ca + M->x2y[i].cins;
        size_t be = M->y2z[j].pa + M->y2z[j].pdel;
        int    r = UT_OK;
        if (ae < M->y2z[j].pa) /* x2y frist */
            r = utH_emitX2Y(M, &M->x2y[i++]);
        else if (be < M->x2y[i].ca) /* y2z first */
            r = utH_emitY2Z(M, &M->y2z[j++]);
        else /* overlap: emit cross */
            r = utH_emitcross(M, i, j), i++, j++;
        if (r != UT_OK) return r;
    }
    for (; i < an; i++) utOK(utH_emitX2Y(M, &M->x2y[i]), 0);
    for (; j < bn; j++) utOK(utH_emitY2Z(M, &M->y2z[j]), 0);
    return UT_OK;
}

static int utH_compose(ut_State *S, ut_HunkCV a, ut_HunkCV b, ut_Hunk **out) {
    int i, r = UT_OK, an = utV_len(a), bn = utV_len(b);
    assert(S != NULL && out != NULL);
    utV_init(*out);
    if (bn == 0) {
        for (i = 0; i < an && r == UT_OK; i++) r = utV_push(S, *out, a[i]);
    } else if (an == 0) {
        for (i = 0; i < bn && r == UT_OK; i++) r = utV_push(S, *out, b[i]);
    } else {
        ut_Merge M;
        M.S = S, M.out = out, M.x2y = a, M.y2z = b, M.xoff = 0, M.zoff = 0;
        r = utH_mergewalk(&M);
    }
    return (void)(r != UT_OK && utV_free(S, *out)), r;
}

static int utH_invert(ut_State *S, const ut_Hunk *h, ut_Hunk **out) {
    ut_Hunk  inv;
    unsigned i, len;
    assert(S != NULL && out != NULL);
    utV_init(*out);
    for (i = 0, len = utV_len(h); i < len; i++) {
        inv.pa = h[i].ca, inv.ca = h[i].pa;
        inv.pdel = h[i].cins, inv.cins = h[i].pdel;
        utOK(utV_push(S, *out, inv), 0);
    }
    return UT_OK;
}

static int utH_normalize(ut_Tree *T, ut_Hunk **out, int s, int e) {
    /* clang-format off */
    struct { utV_Header hdr; ut_Hunk vec[1]; } sv;
    /* clang-format on */
    ut_Hunk *next, *cur = NULL, *single = sv.vec;
    int      r, i;
    assert(T != NULL && out != NULL), sv.hdr.len = 1, sv.hdr.cap = 1;
    for (i = s; i < e; i++) {
        single->pa = T->journal[i].off, single->ca = T->journal[i].off;
        single->pdel = T->journal[i].del, single->cins = T->journal[i].ins;
        r = utH_compose(T->S, cur, single, &next);
        if (utV_free(T->S, cur), cur = next, r != UT_OK)
            return utV_free(T->S, cur), *out = NULL, r;
    }
    return *out = cur, UT_OK;
}

UT_API ut_Vid ut_commit(ut_Tree *T, ut_Payload *pl) {
    ut_Node *n, *p;
    ut_Hunk *h = NULL;
    if (T == NULL || T->current == NULL) return NULL;
    if (utH_normalize(T, &h, 0, utV_len(T->journal)) != UT_OK) return NULL;
    if ((n = utN_alloc(T->S)) == NULL) return utV_free(T->S, h), NULL;
    memset(n, 0, sizeof(ut_Node));
    n->payload = pl, n->h = h, p = T->current;
    n->parent = p, n->depth = p->depth + 1;
    /* ring tail-insert: youngest (last_child) → new → oldest */
    if (!p->last_child)
        n->next_sib = n; /* single-node ring */
    else
        n->next_sib = p->last_child->next_sib, p->last_child->next_sib = n;
    p->last_child = n, p->child_count++;
    if (T->journal) utV_hdr(T->journal)->len = 0;
    return T->current = n;
}

UT_API int ut_discard(ut_Tree *T) {
    if (T == NULL) return UT_ERRPARAM;
    if (T->journal) T->diffhn = -1, utV_hdr(T->journal)->len = 0;
    return UT_OK;
}

UT_API int ut_switch(ut_Tree *T, ut_Vid v) {
    if (T == NULL || v == NULL || v == ut_freshvid(T->S)) return UT_ERRPARAM;
    if (utV_len(T->journal) > 0) return UT_ERRPARAM;
    return T->current = (ut_Node *)v, UT_OK;
}

UT_API ut_Vid ut_ancestor(ut_Vid a, ut_Vid b) {
    if (a == NULL || b == NULL) return NULL;
    while (a->depth > b->depth) a = a->parent;
    while (b->depth > a->depth) b = b->parent;
    while (a != b) a = a->parent, b = b->parent;
    return a;
}

UT_API ut_Vid ut_younger(ut_Vid v) {
    ut_Node *p;
    if (v == NULL) return NULL;
    if (v->last_child) return v->last_child->next_sib; /* oldest child */
    /* follow next_sib (chron next for non-youngest), ascend for youngest */
    for (; (p = v->parent) != NULL; v = p)
        if (v != p->last_child) return v->next_sib;
    return NULL;
}

UT_API ut_Vid ut_older(ut_Vid v) {
    ut_Node *p, *bro;
    if (v == NULL || v->parent == NULL) return NULL;
    /* v is oldest sibling? → parent */
    if (p = v->parent, v == p->last_child->next_sib) return p;
    /* find chronologically older sibling, then drill down */
    for (bro = p->last_child; bro->next_sib != v; bro = bro->next_sib) continue;
    while (bro->last_child) bro = bro->last_child;
    return bro;
}

typedef struct ut_DX {
    ut_Tree *T;
    ut_Hunk *cur, *fresh;
    ut_Vid  *nodes, anc;
    int      hasfrom, hasto;
} ut_DX;

static int utD_calc(ut_DX *X, ut_Vid fn, ut_Vid tn) {
    ut_State *S = X->T->S;
    ut_Vid    v;
    ut_Hunk  *next, *inv;
    int       r, i;
    if (X->hasfrom || X->hasto)
        utOK(utH_normalize(X->T, &X->fresh, 0, utV_len(X->T->journal)), 0);
    /* four-phase compose: [inv(fresh)] + fn→anc⁻¹ + anc→tn + [fresh] */
    if (X->hasfrom && (r = utH_invert(S, X->fresh, &X->cur)) != UT_OK)
        return utV_free(S, X->fresh), r;
    for (v = fn; v != X->anc; v = v->parent) { /* phase 2: fn→anc, inverted */
        utOK(utH_invert(S, v->h, &inv), 0);
        r = utH_compose(S, X->cur, inv, &next), utV_free(S, inv);
        if (utV_free(S, X->cur), X->cur = next, r != UT_OK) return r;
    }
    for (v = tn; v != X->anc; v = v->parent) /* phase 3: anc→tn, forward path */
        utOK(utV_push(S, X->nodes, v), 0);
    for (i = utV_len(X->nodes) - 1; i >= 0; i--) {
        v = X->nodes[i], r = utH_compose(S, X->cur, v->h, &next);
        if (utV_free(S, X->cur), X->cur = next, r != UT_OK) return r;
    }
    if (utV_free(S, X->nodes), X->hasto) { /* phase 4: to-fresh, forward */
        r = utH_compose(S, X->cur, X->fresh, &next);
        if (utV_free(S, X->cur), X->cur = next, r != UT_OK) return r;
    }
    return UT_OK;
}

UT_API int ut_diff(ut_Tree *T, ut_Vid from, ut_Vid to) {
    ut_DX x;
    int   r;
    if (T == NULL) return UT_ERRPARAM;
    memset(&x, 0, sizeof(x)), x.T = T;
    if ((x.hasfrom = (from == ut_freshvid(T->S)))) from = T->current;
    if ((x.hasto = (to == ut_freshvid(T->S)))) to = T->current;
    if ((x.anc = ut_ancestor(from, to)) == NULL) return UT_ERRPARAM;
    if ((r = utD_calc(&x, from, to)) != UT_OK) {
        utV_free(T->S, x.cur), utV_free(T->S, x.fresh), utV_free(T->S, x.nodes);
        return r;
    }
    utV_free(T->S, x.fresh), utV_free(T->S, T->S->scratch);
    if (utV_len(x.cur) == 0) return utV_free(T->S, x.cur), T->diffhn = 0;
    return T->S->scratch = x.cur, T->diffhn = (int)utV_len(x.cur);
}

UT_API int ut_freshdiff(ut_Tree *T, int i, int j) {
    ut_Hunk *h = NULL;
    int      r, len;
    if (T == NULL) return UT_ERRPARAM;
    len = (int)utV_len(T->journal);
    i = ut_max(0, ut_min(i, len)), j = ut_max(0, ut_min(j, len));
    if (i == j) return T->diffhn = 0, UT_OK;
    utV_free(T->S, T->S->scratch), T->diffhn = -1;
    if (i < j)
        r = utH_normalize(T, &T->S->scratch, i, j);
    else {
        r = utH_normalize(T, &h, j, i);
        if (r == UT_OK) r = utH_invert(T->S, h, &T->S->scratch);
        utV_free(T->S, h);
    }
    if (r != UT_OK) return r;
    if (utV_len(T->S->scratch) == 0)
        return utV_free(T->S, T->S->scratch), T->diffhn = 0, UT_OK;
    return T->diffhn = (int)utV_len(T->S->scratch), UT_OK;
}

UT_NS_END

#endif /* UT_IMPLEMENTATION */
