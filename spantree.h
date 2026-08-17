#ifndef spantree_h
#define spantree_h

#ifndef SP_NS_BEGIN
# ifdef __cplusplus
#   define SP_NS_BEGIN extern "C" {
#   define SP_NS_END   }
# else
#   define SP_NS_BEGIN
#   define SP_NS_END
# endif
#endif /* SP_NS_BEGIN */

#ifndef SP_STATIC
# if __GNUC__
#   define SP_STATIC static __attribute((unused))
# else
#   define SP_STATIC static
# endif
#endif /* SP_STATIC */

#ifdef SP_STATIC_API
# ifndef SP_IMPLEMENTATION
#   define SP_IMPLEMENTATION
# endif
# define SP_API SP_STATIC
#endif /* SP_STATIC_API */

#if !defined(SP_API) && defined(_WIN32)
# ifdef SP_IMPLEMENTATION
#   define SP_API __declspec(dllexport)
# else
#   define SP_API __declspec(dllimport)
# endif
#endif /* SP_API */

#ifndef SP_API
# define SP_API extern
#endif

#include <limits.h>
#include <stddef.h>

#define SP_OK       (0)  /* No error */
#define SP_ERRPARAM (-1) /* Invalid parameter */
#define SP_ERRMEM   (-2) /* Memory allocation failed */

#define SP_MASK_BITS (sizeof(sp_Mask) * CHAR_BIT)

SP_NS_BEGIN

typedef struct sp_State  sp_State;  /* allocator + pools      */
typedef struct sp_Tree   sp_Tree;   /* one span tree          */
typedef struct sp_Cursor sp_Cursor; /* traversal state        */

typedef ptrdiff_t sp_Delta;
typedef size_t    sp_Id;
typedef size_t    sp_Mask; /* ns bitset; layout hidden from users */

typedef void *sp_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
typedef sp_Id sp_Arbiterf(void *ud, sp_Id id, sp_Id old, sp_Mask *mask);

/* state */

SP_API sp_State *sp_open(sp_Alloc *allocf, void *ud);
SP_API void      sp_close(sp_State *S);

/* tree */
SP_API sp_Tree *sp_newtree(sp_State *S);
SP_API void     sp_freetree(sp_Tree *T);
SP_API size_t   sp_bytes(const sp_Tree *T);

/* blending */
SP_API void sp_setarbiter(sp_Tree *T, sp_Arbiterf *cb, void *ud);
SP_API int  sp_addns(sp_Mask *mask, int ns);
SP_API int  sp_delns(sp_Mask *mask, int ns);
SP_API int  sp_hasns(const sp_Mask *mask, int ns);

/* cursor */

#define sp_offset(C) ((C)->off + (C)->poff)

/* construction*/
SP_API int sp_seek(sp_Cursor *C, sp_Tree *T, size_t off);

/* navigation */
SP_API int sp_locate(sp_Cursor *C, size_t off);
SP_API int sp_advance(sp_Cursor *C, sp_Delta d);

/* marking */
SP_API int sp_clear(sp_Tree *T, int ns, sp_Id id);
SP_API int sp_fill(sp_Cursor *C, sp_Id id, size_t len);

/* reading */
SP_API sp_Id sp_next(sp_Cursor *C, int ns, size_t *plen);
SP_API sp_Id sp_prev(sp_Cursor *C, int ns, size_t *plen);
SP_API sp_Id sp_style(sp_Cursor *C, size_t *plen, sp_Mask *pmask);

/* editing */
SP_API int sp_splice(sp_Cursor *C, size_t del, size_t ins);
SP_API int sp_append(sp_Cursor *C, size_t ins);
SP_API int sp_insert(sp_Cursor *C, size_t ins);
SP_API int sp_remove(sp_Cursor *L, sp_Cursor *R);

/* cursor definition */

#define SP_MAX_LEVEL 16

struct sp_Cursor {
    struct sp_Node **paths[SP_MAX_LEVEL]; /* root-to-leaf child slot ptrs */
    struct sp_Tree  *tree;                /* tree under navigation */
    size_t           poff;                /* offset in current leaf span */
    size_t           off;                 /* bytes before current span */
};

SP_NS_END

#endif /* spantree_h */

#if defined(SP_IMPLEMENTATION) && !defined(sp_implemented)
#define sp_implemented

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define SP_STATIC_ASSERT(cond)      SP_SA_0(cond, sp_SA_, __LINE__)
#define SP_SA_0(cond, prefix, line) SP_SA_1(cond, prefix, line)
#define SP_SA_1(cond, prefix, line) typedef char prefix##line[(cond) ? 1 : -1]

#ifndef SP_FANOUT
# define SP_FANOUT 62
#endif /* SP_FANOUT */

/* a full-node split leaves FANOUT/2 free; makeroom needs 2, so >= 4 */
SP_STATIC_ASSERT(SP_FANOUT >= 4);

#ifndef SP_PAGE_SIZE
# define SP_PAGE_SIZE 65536
#endif

#ifndef SP_MAX_LEVEL
# define SP_MAX_LEVEL 16
#endif

SP_NS_BEGIN

typedef struct sp_Pool {
    size_t obj_size;  /* size of each object in this pool */
    void  *freed;     /* freelist head */
    void  *pages;     /* linked list of allocated pages */
    size_t freed_obj; /* number of objects in freelist */
#ifdef SP_POOL_STATS
    size_t live_obj;
#endif
} sp_Pool;

typedef struct sp_Node {
    struct sp_Node *children[SP_FANOUT]; /* interior subnodes or leaf ids */
    size_t          bytes[SP_FANOUT];    /* subtree sum or span length */
    sp_Mask         mask[SP_FANOUT];     /* leaf ns set or OR of children */
    unsigned short  child_count;         /* valid child count in node */
} sp_Node;

struct sp_State {
    void     *alloc_ud;         /* user data for allocator */
    sp_Alloc *allocf;           /* allocator function */
    sp_Pool   nodes;            /* pool for sp_Node */
    sp_Node   rt[SP_MAX_LEVEL]; /* scratch nodes for tree stitch */
};

struct sp_Tree {
    sp_Node        root;   /* embedded root node */
    sp_State      *S;      /* owning sp_State */
    size_t         bytes;  /* total bytes in this tree */
    unsigned short levels; /* tree height, 0 = leaf-only root */
    sp_Arbiterf   *arb;    /* span id merge callback */
    void          *aud;    /* user data for arbiter */
};

#define sp_min(a, b) ((a) < (b) ? (a) : (b))

/* mempool */

#ifdef SP_POOL_STATS
# define spP_stat(stmt) stmt
#else
# define spP_stat(stmt) ((void)0)
#endif

static void spP_init(sp_Pool *p, size_t obj_size) {
    memset(p, 0, sizeof(sp_Pool)), p->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < SP_PAGE_SIZE / 2);
}

static void spP_destroy(sp_State *S, sp_Pool *p) {
    void *next, *page = p->pages;
    for (; page; page = next) {
        next = *(void **)((char *)page + SP_PAGE_SIZE - sizeof(void *));
        S->allocf(S->alloc_ud, page, SP_PAGE_SIZE, 0);
    }
    spP_init(p, p->obj_size);
}

static void *spP_ralloc(sp_Pool *p) {
    void *obj = p->freed;
    assert(obj), spP_stat(p->live_obj += 1), p->freed_obj -= 1;
    return (p->freed = *(void **)obj), (void *)obj;
}

static void spP_free(sp_Pool *p, void *obj) {
    spP_stat(p->live_obj -= 1), p->freed_obj += 1;
    *(void **)obj = p->freed, p->freed = obj;
}

static void *spP_alloc(sp_State *S, sp_Pool *p) {
    size_t sz = p->obj_size;
    char  *page, *end;
    if (p->freed_obj) return spP_ralloc(p);
    page = (char *)S->allocf(S->alloc_ud, NULL, 0, SP_PAGE_SIZE);
    if (page == NULL) return NULL;
    end = &page[SP_PAGE_SIZE - sizeof(void *)], *(void **)end = p->pages;
    p->pages = (void *)page, page += sz, end -= sz;
    while ((page += sz) <= end) *(void **)(page - sz) = page;
    *(void **)(page - sz) = p->freed, spP_stat(p->live_obj += 1);
    p->freed_obj = (end - (char *)p->pages) / sz;
    return (p->freed = (void *)((char *)p->pages + sz)), p->pages;
}

static int spP_reserve(sp_State *S, sp_Pool *p, size_t n) {
    size_t avail = p->freed_obj;
    void  *obj;
    if (avail >= n) return SP_OK;
    while (p->freed_obj = 0, (obj = spP_alloc(S, p)))
        if (spP_free(p, obj), (avail += p->freed_obj) >= n) break;
    return (p->freed_obj = avail) >= n ? SP_OK : SP_ERRMEM;
}

/* utils */

#define spK_levels(C)       ((C)->tree->levels)
#define spK_bytes(C)        ((C)->tree->bytes)
#define spK_parent(C, l)    ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define spK_idx(C, p, l)    ((int)((C)->paths[(l)] - (p)->children))
#define spN_cc(n)           ((int)(n)->child_count)
#define spN_setcc(n, v)     ((n)->child_count = (unsigned short)(v))
#define spL_id(p, i)        ((sp_Id)(p)->children[i])
#define spL_setid(p, i, id) ((p)->children[i] = (struct sp_Node *)(id))

#define spM_bit(ns) ((sp_Mask)1 << ((ns) - 1))

/* clang-format off */
static sp_Id spA_born(sp_Tree *T, sp_Id id)
{ sp_Mask m = 0; return T->arb && id ? T->arb(T->aud, id, 0, &m) : id; }

static void spA_died(sp_Tree *T, sp_Id id)
{ sp_Mask m = 0; if (T->arb && id) (void)T->arb(T->aud, 0, id, &m); }

static size_t spN_sumbytes(const sp_Node *n, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += n->bytes[i]; return s; }

static sp_Mask spM_sumns(const sp_Node *n)
{ sp_Mask m = 0; int i; for (i=0; i<spN_cc(n); ++i) m |= n->mask[i]; return m; }

static void spM_remask(sp_Node *p, int i, int k)
{ if (k) p->mask[i] = spM_sumns(p->children[i]); }

static int spM_check(const sp_Mask *m, int ns)
{ return m != NULL && ns >= 1 && ns <= (int)SP_MASK_BITS; }

SP_API int sp_addns(sp_Mask *m, int ns)
{ return spM_check(m, ns) ? (*m |= spM_bit(ns), SP_OK) : SP_ERRPARAM; }

SP_API int sp_delns(sp_Mask *m, int ns)
{ return spM_check(m, ns) ? (*m &= ~spM_bit(ns), SP_OK) : SP_ERRPARAM; }

SP_API int sp_hasns(const sp_Mask *m, int ns)
{ return spM_check(m, ns) && (*m & spM_bit(ns)) != 0; }
/* clang-format on */

static void spN_copy(sp_Node *d, int di, const sp_Node *s, int si, int n) {
    assert(di + n <= SP_FANOUT && si + n <= SP_FANOUT);
    memcpy(&d->children[di], &s->children[si], n * sizeof(sp_Node *));
    memcpy(&d->bytes[di], &s->bytes[si], n * sizeof(size_t));
    memcpy(&d->mask[di], &s->mask[si], n * sizeof(sp_Mask));
}

static void spN_move(sp_Node *d, int di, int si, int n) {
    assert(di + n <= SP_FANOUT && si + n <= SP_FANOUT);
    memmove(&d->children[di], &d->children[si], n * sizeof(sp_Node *));
    memmove(&d->bytes[di], &d->bytes[si], n * sizeof(size_t));
    memmove(&d->mask[di], &d->mask[si], n * sizeof(sp_Mask));
}

static void spN_remove(sp_Node *p, int i, int n) {
    assert(i + n <= spN_cc(p));
    spN_move(p, i, i + n, spN_cc(p) - (i + n));
    spN_setcc(p, spN_cc(p) - n);
}

static void spN_makespace(sp_Node *p, int i, int n) {
    assert(spN_cc(p) + n <= SP_FANOUT && i <= spN_cc(p));
    spN_move(p, i + n, i, spN_cc(p) - i);
    spN_setcc(p, spN_cc(p) + n);
}

static void spN_purge(sp_Tree *T, sp_Node *p, int k, int s, int e) {
    int i;
    assert(s <= e && e <= spN_cc(p));
    if (k == 0) {
        for (i = s; i < e; ++i) spA_died(T, spL_id(p, i));
        return; /* leaf container: children hold ids, not pointers */
    }
    for (i = s; i < e; ++i) {
        spN_purge(T, p->children[i], k - 1, 0, spN_cc(p->children[i]));
        spP_free(&T->S->nodes, p->children[i]);
    }
}

static void spM_up(sp_Cursor *C, int l, sp_Delta db) {
    int      i;
    sp_Node *p;
    for (; l >= 0; --l) {
        i = spK_idx(C, p = spK_parent(C, l), l), p->bytes[i] += db;
        if (l < spK_levels(C)) p->mask[i] = spM_sumns(p->children[i]);
    }
    if (db) C->tree->bytes += db;
}
/* state */

static void *spS_defallocf(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    (void)ud, (void)osize;
    if (nsize == 0) return (void)free(p), NULL;
    return (np = realloc(p, nsize)) ? np : ((void)abort(), NULL);
}

SP_API sp_State *sp_open(sp_Alloc *allocf, void *ud) {
    sp_State *S;
    if (allocf == NULL) allocf = &spS_defallocf;
    S = (sp_State *)allocf(ud, NULL, 0, sizeof(sp_State));
    if (!S) return NULL;
    memset(S, 0, sizeof(sp_State)), S->alloc_ud = ud, S->allocf = allocf;
    spP_init(&S->nodes, sizeof(sp_Node));
    return S;
}

/* clang-format off */
SP_API void sp_close(sp_State *S)
{ if (S) spP_destroy(S, &S->nodes), S->allocf(S->alloc_ud, S, sizeof(sp_State), 0); }

SP_API void sp_setarbiter(sp_Tree *t, sp_Arbiterf *cb, void *ud)
{ if (t) t->arb = cb, t->aud = ud; }
/* clang-format on */

SP_API size_t sp_bytes(const sp_Tree *t) { return t ? t->bytes : 0; }

SP_API sp_Tree *sp_newtree(sp_State *S) {
    sp_Tree *t;
    if (S == NULL) return NULL;
    t = (sp_Tree *)S->allocf(S->alloc_ud, NULL, 0, sizeof(sp_Tree));
    if (!t) return NULL;
    memset(t, 0, sizeof(sp_Tree));
    t->S = S;
    return t;
}

SP_API void sp_freetree(sp_Tree *t) {
    if (t == NULL) return;
    spN_purge(t, &t->root, t->levels, 0, spN_cc(&t->root));
    t->S->allocf(t->S->alloc_ud, t, sizeof(sp_Tree), 0);
}

/* cursor lookup and traversal */

static void spK_findleaf(sp_Cursor *C, int l, size_t *poff) {
    for (; l <= spK_levels(C); ++l) {
        sp_Node *p = spK_parent(C, l);
        int      i;
        for (i = 0; i < spN_cc(p) && *poff >= p->bytes[i]; ++i)
            *poff -= p->bytes[i], C->off += p->bytes[i];
        assert(i < spN_cc(p)), C->paths[l] = &p->children[i];
    }
}

static int spK_locend(sp_Cursor *C) {
    sp_Node *n = &C->tree->root;
    int      l;
    if (spK_levels(C) == 0 && spN_cc(n) == 0)
        return (C->paths[0] = n->children, C->off = 0, C->poff = 0), 0;
    for (l = 0; l < spK_levels(C); ++l)
        n = *(C->paths[l] = &n->children[spN_cc(n) - 1]);
    assert(spN_cc(n)), C->paths[l] = &n->children[spN_cc(n) - 1];
    C->poff = n->bytes[spN_cc(n) - 1];
    return (C->off = spK_bytes(C) - C->poff), 1;
}

SP_API int sp_seek(sp_Cursor *C, sp_Tree *T, size_t off) {
    if (C == NULL || T == NULL) return SP_ERRPARAM;
    memset(C, 0, sizeof(sp_Cursor)), C->tree = T;
    if (off >= T->bytes) return spK_locend(C), C->poff += off - T->bytes, SP_OK;
    return spK_findleaf(C, 0, &off), (C->poff = off), SP_OK;
}

SP_API int sp_locate(sp_Cursor *C, size_t off) {
    if (C == NULL || C->tree == NULL) return SP_ERRPARAM;
    C->off = C->poff = 0;
    if (off >= C->tree->bytes)
        return spK_locend(C), C->poff += off - C->tree->bytes, SP_OK;
    return spK_findleaf(C, 0, &off), (C->poff = off), SP_OK;
}

static int spK_forwardoff(sp_Cursor *C, size_t d) {
    sp_Node *p;
    int      l, i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    size_t   in = p->bytes[i] - C->poff;
    if (d < in) return C->poff += d, 0;
    d -= in, C->off += p->bytes[i], C->poff = 0;
    for (l = spK_levels(C); l >= 0; --l) {
        i = spK_idx(C, p = spK_parent(C, l), l) + 1;
        for (; i < spN_cc(p) && d >= p->bytes[i]; ++i)
            d -= p->bytes[i], C->off += p->bytes[i];
        if (i < spN_cc(p)) break;
    }
    assert(l >= 0 && i < spN_cc(p)), C->paths[l] = &p->children[i];
    return (spK_findleaf(C, l + 1, &d), C->poff = d), 1;
}

static int spK_backwardoff(sp_Cursor *C, size_t d) {
    sp_Node *p = NULL;
    int      l, i = 0;
    if (d <= C->poff) return C->poff -= d, 0;
    d -= C->poff, C->poff = 0;
    for (l = spK_levels(C); l >= 0; --l) {
        i = spK_idx(C, p = spK_parent(C, l), l) - 1;
        for (; i >= 0 && d > p->bytes[i]; --i)
            d -= p->bytes[i], C->off -= p->bytes[i];
        if (i >= 0) break;
    }
    assert(l >= 0 && i >= 0), d = p->bytes[i] - d, C->off -= p->bytes[i];
    C->paths[l] = &p->children[i];
    return (spK_findleaf(C, l + 1, &d), C->poff = d), 1;
}

SP_API int sp_advance(sp_Cursor *C, sp_Delta d) {
    size_t off;
    if (C == NULL || C->tree == NULL) return SP_ERRPARAM;
    off = sp_offset(C);
    if (d < 0 && (size_t)-d > off) return spK_backwardoff(C, off), SP_OK;
    if (d < 0) return spK_backwardoff(C, (size_t)(-d)), SP_OK;
    if ((off + d) >= spK_bytes(C)) {
        size_t vir = (off + d) - spK_bytes(C);
        return spK_locend(C), C->poff += vir, SP_OK;
    }
    return spK_forwardoff(C, d), SP_OK;
}

static void spK_offtail(sp_Cursor *C) {
    sp_Node *p;
    int      i;
    if (sp_offset(C) >= spK_bytes(C)) return;
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    if (C->poff >= p->bytes[i]) spK_forwardoff(C, 0);
}

static int spK_seamleaf(sp_Cursor *C, int right) {
    int      i, l = spK_levels(C);
    sp_Node *p;
    size_t   n;
    i = spK_idx(C, p = spK_parent(C, l), l) + right;
    if (i < 1 || i >= spN_cc(p)) return 0;
    if (spL_id(p, i - 1) != spL_id(p, i)) return 0;
    n = p->bytes[i - 1], p->bytes[i - 1] += p->bytes[i];
    p->mask[i - 1] |= p->mask[i];
    spA_died(C->tree, spL_id(p, i));
    if (C->paths[l] == &p->children[i])
        C->off -= n, C->poff += n, C->paths[l] = &p->children[i - 1];
    return spN_remove(p, i, 1), 1;
}

SP_API sp_Id sp_style(sp_Cursor *C, size_t *plen, sp_Mask *pmask) {
    sp_Node *p;
    int      i;
    if (pmask) *pmask = 0;
    if (C == NULL || C->tree == NULL) return (void)(plen && (*plen = 0)), 0;
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    if (C->tree->bytes == 0 || C->poff >= p->bytes[i])
        return (void)(plen && (*plen = 0)), 0;
    if (plen) *plen = p->bytes[i] - C->poff;
    if (pmask) *pmask = p->mask[i];
    return spL_id(p, i);
}

static int spF_findslot(sp_Node *p, int i, int d, sp_Mask bit, size_t *pbc) {
    for (; i >= 0 && i < spN_cc(p) && bit && !(p->mask[i] & bit); i += d)
        *pbc += p->bytes[i];
    return i;
}

SP_API sp_Id sp_next(sp_Cursor *C, int ns, size_t *plen) {
    sp_Mask  bit;
    sp_Node *p;
    int      i, l;
    size_t   bc;
    if (C == NULL || C->tree == NULL) return (void)(plen && (*plen = 0)), 0;
    if (ns < 0 || ns > (int)SP_MASK_BITS) return (void)(plen && (*plen = 0)), 0;
    bit = ns ? spM_bit(ns) : 0;
    l = spK_levels(C), i = spK_idx(C, p = spK_parent(C, l), l);
    if (C->tree->bytes == 0 || C->poff >= p->bytes[i])
        return (void)(plen && (*plen = 0)), 0;
    bc = p->bytes[i];
    while ((i = spF_findslot(p, i + 1, 1, bit, &bc)) == spN_cc(p)) {
        if (--l < 0) return spK_locend(C), (void)(plen && (*plen = 0)), 0;
        i = spK_idx(C, p = spK_parent(C, l), l);
    }
    C->paths[l] = &p->children[i];
    for (; ++l <= spK_levels(C); C->paths[l] = &p->children[i])
        i = spF_findslot(p = spK_parent(C, l), 0, 1, bit, &bc);
    assert(!bit || (p->mask[i] & bit)), C->off += bc, C->poff = 0;
    return (void)(plen && (*plen = p->bytes[i])), spL_id(p, i);
}

SP_API sp_Id sp_prev(sp_Cursor *C, int ns, size_t *plen) {
    sp_Mask  bit;
    sp_Node *p;
    int      i, l;
    size_t   bc = 0;
    if (C == NULL || C->tree == NULL) return (void)(plen && (*plen = 0)), 0;
    if (ns < 0 || ns > (int)SP_MASK_BITS) return (void)(plen && (*plen = 0)), 0;
    bit = ns ? spM_bit(ns) : 0;
    if (C->tree->bytes == 0) return (void)(plen && (*plen = 0)), 0;
    l = spK_levels(C), i = spK_idx(C, p = spK_parent(C, l), l);
    if (C->poff > 0 && (!bit || (p->mask[i] & bit)))
        return (void)(plen && (*plen = sp_min(C->poff, p->bytes[i]))),
               C->poff = 0, spL_id(p, i);
    if (C->off == 0) return (C->poff = 0), (void)(plen && (*plen = 0)), 0;
    while ((i = spF_findslot(p, i - 1, -1, bit, &bc)) < 0) {
        if (--l < 0)
            return assert(bit), (void)(plen && (*plen = 0)), sp_locate(C, 0), 0;
        i = spK_idx(C, p = spK_parent(C, l), l);
    }
    C->paths[l] = &p->children[i];
    for (; ++l <= spK_levels(C); C->paths[l] = &p->children[i])
        p = spK_parent(C, l), i = spF_findslot(p, spN_cc(p) - 1, -1, bit, &bc);
    assert(!bit || (p->mask[i] & bit)), C->off -= bc + p->bytes[i], C->poff = 0;
    return (void)(plen && (*plen = p->bytes[i])), spL_id(p, i);
}

/* remove / balance / stitch */

static void spD_trimright(sp_Cursor *L) {
    sp_Node *p;
    int      l = spK_levels(L), i = spK_idx(L, p = spK_parent(L, l), l);
    spM_up(L, l, -(sp_Delta)(p->bytes[i] - L->poff));
    if (p->bytes[i] == 0) spA_died(L->tree, spL_id(p, i));
}

static void spD_trimleft(sp_Cursor *R) {
    sp_Node *p;
    int      l = spK_levels(R), i = spK_idx(R, p = spK_parent(R, l), l);
    p->bytes[i] -= R->poff;
    if (p->bytes[i] == 0) spA_died(R->tree, spL_id(p, i));
    spM_up(R, l - 1, -(sp_Delta)R->poff), R->poff = 0;
}

static void spD_cutrange(sp_Cursor *L, sp_Cursor *R, sp_Node *rt, int fl) {
    sp_State *S = L->tree->S;
    int       kl, k, i, ir, cc, l = spK_levels(L);
    sp_Delta  db = 0;
    sp_Node  *p;
    for (kl = l; kl > fl; --kl) {
        p = spK_parent(L, kl), i = spK_idx(L, p, kl), cc = spN_cc(p);
        p->bytes[i] -= db, db += spN_sumbytes(p, i + 1, cc);
        k = l - kl, spN_purge(L->tree, p, k, i + 1, cc);
        spN_remove(p, i + 1, cc - (i + 1));
        ir = spK_idx(R, p = spK_parent(R, kl), kl), cc = spN_cc(p);
        i = ir + (k || p->bytes[ir] == 0);
        spN_copy(&rt[k], 0, p, i, cc - i), spN_setcc(&rt[k], cc - i);
        spN_purge(L->tree, p, k, 0, ir);
        if (kl > fl + 1) spP_free(&S->nodes, p); /* emptied R-path shell */
        spN_setcc(p, 0);
    }
    p = spK_parent(R, fl), i = spK_idx(R, p, fl), cc = spN_cc(p);
    k = l - fl, ir = i, i += (k || p->bytes[i] == 0);
    spN_copy(&rt[k], 0, p, i, spN_setcc(&rt[k], cc - i));
    spN_setcc(p, i), i = spK_idx(L, p, fl);
    p->bytes[i] -= db, db += spN_sumbytes(p, i + 1, cc);
    spM_up(L, fl - 1, -db);
    if (k > 0)
        spN_purge(L->tree, p, k, i + 1, ir + 1);
    else
        for (k = i + 1; k < ir; ++k) spA_died(L->tree, spL_id(p, k));
    spN_remove(p, i + 1, spN_cc(p) - (i + 1));
    spM_up(L, l - 1, 0); /* removed subtrees change the path aggregates */
}

static void spD_makechain(sp_Cursor *C, int from, int to) {
    sp_Node *p, *nn = NULL;
    int      l;
    assert(from < to);
    for (l = from; l < to; ++l) {
        nn = (sp_Node *)spP_ralloc(&C->tree->S->nodes);
        p = spK_parent(C, l), nn->child_count = 0;
        p->bytes[spN_cc(p)] = 0, p->mask[spN_cc(p)] = 0;
        C->paths[l] = &p->children[spN_cc(p)];
        p->children[spN_cc(p)] = nn, p->child_count += 1;
    }
    C->paths[to] = &nn->children[0];
}

static void spD_findroom(sp_Cursor *C, int l) {
    int      fl, i = 0;
    sp_Node *p = NULL;
    for (fl = l - 1; fl >= 0; --fl) {
        p = spK_parent(C, fl), i = spK_idx(C, p, fl);
        if (i < SP_FANOUT - 1) break;
    }
    assert(fl >= 0 && spN_cc(p) - i - 1 == 0);
    spD_makechain(C, fl, l);
}

static void spD_backwardnode(sp_Cursor *C, int d, int l) {
    sp_Node *p = spK_parent(C, l);
    int      dl, i = spK_idx(C, p, l);
    if (d > i) {
        d -= i + 1, dl = l;
        while (--dl >= 0 && spK_idx(C, spK_parent(C, dl), dl) == 0) continue;
        assert(dl >= 0), C->paths[dl] -= 1;
        while (++dl <= l)
            p = spK_parent(C, dl), C->paths[dl] = &p->children[spN_cc(p) - 1];
    }
    C->paths[l] -= d;
}

static int spD_balancenode(sp_Node **ns, int left, sp_Delta *ds) {
    int d, l = spN_cc(ns[0]), r = spN_cc(ns[1]);
    d = l - ((l + r + (left != 0)) >> 1);
    assert(d != 0);
    if (d < 0) {
        spN_copy(ns[0], l, ns[1], 0, -d);
        spN_move(ns[1], 0, -d, r + d);
        *ds = -(sp_Delta)spN_sumbytes(ns[0], l, l - d);
    } else {
        spN_move(ns[1], d, 0, r);
        spN_copy(ns[1], 0, ns[0], l - d, d);
        *ds = (sp_Delta)spN_sumbytes(ns[1], 0, d);
    }
    return spN_setcc(ns[0], l - d), spN_setcc(ns[1], r + d), d;
}

static void spD_seambound(sp_Cursor *C, int l, sp_Node **ns, int *cL, int *cR) {
    sp_Node *p = spK_parent(C, l);
    int      i = (int)(ns - p->children);
    size_t   n;
    *cL = spN_cc(ns[0]), *cR = spN_cc(ns[1]);
    if (l != spK_levels(C) - 1 || *cL == 0 || *cR == 0) return;
    if (spL_id(ns[0], *cL - 1) != spL_id(ns[1], 0)) return;
    if (*C->paths[l] == ns[0]) {
        n = ns[1]->bytes[0], ns[0]->bytes[*cL - 1] += n;
        ns[0]->mask[*cL - 1] |= ns[1]->mask[0];
        spA_died(C->tree, spL_id(ns[1], 0));
        p->bytes[i] += n, p->bytes[i + 1] -= n;
        spN_remove(ns[1], 0, 1), *cR -= 1;
    } else {
        ns[1]->children[0] = ns[0]->children[*cL - 1];
        n = ns[0]->bytes[*cL - 1], ns[1]->bytes[0] += n;
        ns[1]->mask[0] |= ns[0]->mask[*cL - 1];
        spA_died(C->tree, spL_id(ns[1], 0));
        p->bytes[i] -= n, p->bytes[i + 1] += n;
        if (C->paths[l + 1] == &ns[1]->children[0]) C->off -= n, C->poff += n;
        *cL -= 1, spN_setcc(ns[0], *cL);
    }
}

static int spD_foldnode(sp_Cursor *C, int lfirst, int l) {
    sp_Node  *p = spK_parent(C, l), ***cp = &C->paths[l];
    int       r, cL, cR, dn, i = spK_idx(C, p, l);
    sp_Node **ns = &p->children[i], *o = *ns;
    sp_Delta  ds;
    if (assert(spN_cc(p) > 1), spN_cc(*ns) > SP_FANOUT / 2) return 0;
    if ((i && lfirst) || i == spN_cc(p) - 1) ns -= 1, i -= 1;
    r = (*ns != o);
    if (spD_seambound(C, l, ns, &cL, &cR), cL + cR <= SP_FANOUT) {
        spN_copy(ns[0], cL, ns[1], 0, cR);
        spN_setcc(ns[0], cL + cR), spN_setcc(ns[1], 0);
        p->bytes[i] += p->bytes[i + 1], p->mask[i] = spM_sumns(ns[0]);
        if (r) cp[1] = &ns[0]->children[cp[1] - ns[1]->children + cL], --cp[0];
        spP_free(&C->tree->S->nodes, ns[1]);
        return spN_remove(p, i + 1, 1), spM_up(C, l - 1, 0), 1;
    }
    dn = spD_balancenode(ns, (*ns == o), &ds), assert(dn != 0 && (dn < 0) != r);
    if (r) cp[1] += dn;
    p->bytes[i] -= ds, p->bytes[i + 1] += ds;
    p->mask[i] = spM_sumns(ns[0]), p->mask[i + 1] = spM_sumns(ns[1]);
    return spM_up(C, l - 1, 0), 0;
}

static void spD_rebalance(sp_Cursor *C, int l) {
    sp_Node *p;
    assert(l == 0 || l < spK_levels(C));
    for (; l > 0; --l) {
        p = spK_parent(C, l);
        if (spN_cc(p->children[spK_idx(C, p, l)]) >= SP_FANOUT / 2) return;
        assert(spN_cc(p) > 1);
        if (!spD_foldnode(C, 0, l)) return;
    }
    if (l == 0 && spK_levels(C) > 0) { /* fold the root children */
        int i = spK_idx(C, p = &C->tree->root, 0);
        if (spN_cc(p->children[i]) < SP_FANOUT / 2 && spN_cc(p) >= 2)
            spD_foldnode(C, 0, 0);
    }
    while (spK_levels(C) && spN_cc(&C->tree->root) == 1) {
        sp_Node *only = spK_parent(C, 1);
        int      i = spK_idx(C, only, 1);
        C->tree->root = *only;
        spP_free(&C->tree->S->nodes, only);
        C->tree->levels--, C->paths[0] += i;
        memmove(C->paths + 1, C->paths + 2, spK_levels(C) * sizeof(sp_Node **));
    }
}

static void spD_stitchnode(sp_Cursor *L, sp_Node *rt) {
    int      k, i, d = 0, l = spK_levels(L);
    sp_Delta db = 0;
    sp_Node *p, *r;
    for (k = 0; k <= spK_levels(L); ++k) {
        int m, fl, kl = spK_levels(L) - k, rtcc = spN_cc(r = &rt[k]);
        spN_setcc(r, 0), i = spK_idx(L, p = spK_parent(L, kl), kl);
        if (i < spN_cc(p)) p->bytes[i] += db, spM_remask(p, i, k);
        if ((m = sp_min(rtcc, SP_FANOUT - spN_cc(p))) > 0) {
            spN_copy(p, spN_cc(p), r, 0, m), spN_setcc(p, spN_cc(p) + m);
            db += (sp_Delta)spN_sumbytes(r, 0, m);
        }
        if (!(m < rtcc || kl == 0)) continue;
        spM_up(L, kl - 1, db), db = 0;
        if (kl == 0 && spN_cc(&L->tree->root) == 1)
            spD_rebalance(L, 0), l -= (k - spK_levels(L));
        if (l > kl)
            for (fl = kl; fl < l; ++fl) spD_foldnode(L, (fl == kl), fl);
        if (k) spD_backwardnode(L, d, l);
        if (!(m < rtcc)) continue;
        p = spK_parent(L, l = kl), d = k ? spN_cc(p) - spK_idx(L, p, l) : m;
        spD_findroom(L, l), p = spK_parent(L, l);
        spN_copy(p, 0, r, m, spN_setcc(p, rtcc - m));
        db += (sp_Delta)spN_sumbytes(r, m, rtcc);
    }
}

static void spD_mergeleaf(sp_Cursor *C, sp_Node *rt) {
    sp_Node *p = spK_parent(C, spK_levels(C));
    int      cc = spN_cc(p), l = spK_levels(C);
    size_t   bc = (assert(cc > 0), p->bytes[cc - 1]);
    if (spL_id(p, cc - 1) != spL_id(rt, 0))
        C->off += C->poff, C->poff = 0, C->paths[l] = &p->children[cc];
    else {
        rt->bytes[0] += bc, rt->children[0] = p->children[cc - 1];
        rt->mask[0] |= p->mask[cc - 1];
        spA_died(C->tree, spL_id(p, cc - 1));
        spN_setcc(p, cc - 1), spM_up(C, l - 1, -(sp_Delta)bc);
        if (spK_idx(C, p, l) == cc) C->off -= bc, C->poff = bc;
        C->paths[l] = &p->children[cc - 1];
    }
}

static void spD_stitch(sp_Cursor *L, sp_Node *rt) {
    int      cc, i, l = spK_levels(L);
    sp_Node *p = spK_parent(L, l);
    assert(L->tree->S->nodes.freed_obj >= (size_t)(spK_levels(L) + 2));
    if ((cc = spN_cc(p)) && p->bytes[cc - 1] == 0)
        spN_remove(p, cc - 1, 1), cc -= 1;
    if (cc && spN_cc(&rt[0])) spD_mergeleaf(L, rt);
    spD_stitchnode(L, rt), spD_rebalance(L, 0);
    l = spK_levels(L), i = spK_idx(L, p = spK_parent(L, l), l), cc = spN_cc(p);
    if (cc && i == cc) {
        L->paths[spK_levels(L)] -= 1;
        L->poff += p->bytes[cc - 1], L->off -= p->bytes[cc - 1];
    }
    spM_up(L, spK_levels(L), 0);
}

static void spD_rmrange(sp_Cursor *L, sp_Cursor *R, int fl) {
    sp_Node *rt = L->tree->S->rt;
    int      k;
    for (k = 0; k < SP_MAX_LEVEL; ++k) spN_setcc(&rt[k], 0);
    spD_trimright(L), spD_trimleft(R);
    spD_cutrange(L, R, rt, fl), spD_stitch(L, rt);
}

static void spD_cutpiece(sp_Cursor *C, size_t lo, size_t hi) {
    sp_Node *p;
    int      i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    if (lo == 0 && hi == p->bytes[i])
        spA_died(C->tree, spL_id(p, i)), spN_remove(p, i, 1),
                spK_seamleaf(C, 0);
    else
        p->bytes[i] -= hi - lo;
}

static void spD_rmleaf(sp_Cursor *C, size_t del) {
    sp_Node *p = spK_parent(C, spK_levels(C));
    int      l = spK_levels(C), oc = spN_cc(p);
    assert(C->poff + del <= p->bytes[spK_idx(C, p, l)]);
    spD_cutpiece(C, C->poff, C->poff + del), spM_up(C, l - 1, -(sp_Delta)del);
    if (spN_cc(p) == 0)
        C->paths[l] = &p->children[0], C->off = 0, C->poff = 0;
    else if (spK_idx(C, p, l) == spN_cc(p)) {
        C->paths[l] -= 1;
        C->poff = p->bytes[spN_cc(p) - 1], C->off -= p->bytes[spN_cc(p) - 1];
    }
    if (spN_cc(p) < oc && l > 0) spD_rebalance(C, l - 1);
}

static int spD_splitpaths(sp_Cursor *L, sp_Cursor *R) {
    int l;
    for (l = 0; l < spK_levels(L) && L->paths[l] == R->paths[l]; ++l) continue;
    return l + (l == spK_levels(L) && L->paths[l] == R->paths[l]);
}

static int spD_remove(sp_Cursor *C, size_t len) {
    sp_Cursor R;
    int       r, l;
    if (len == 0 || sp_offset(C) >= spK_bytes(C)) return SP_OK;
    if (sp_offset(C) + len > spK_bytes(C)) len = spK_bytes(C) - sp_offset(C);
    R = *C, sp_advance(&R, (sp_Delta)len);
    if (sp_offset(&R) >= spK_bytes(&R)) spK_locend(&R);
    r = spP_reserve(C->tree->S, &C->tree->S->nodes, 4 * spK_levels(C) + 5);
    if (r != SP_OK) return r;
    if ((l = spD_splitpaths(C, &R)) > spK_levels(C))
        return spD_rmleaf(C, len), SP_OK;
    return spD_rmrange(C, &R, l), SP_OK;
}

SP_API int sp_remove(sp_Cursor *L, sp_Cursor *R) {
    if (!L || !R || !L->tree || L->tree != R->tree) return SP_ERRPARAM;
    if (sp_offset(L) >= sp_offset(R)) return SP_OK;
    return spD_remove(L, sp_offset(R) - sp_offset(L));
}

/* insertion / split */

static void spI_onepiece(sp_Cursor *C, size_t len, sp_Id id) {
    sp_Node *r = &C->tree->root;
    spL_setid(r, 0, id), r->bytes[0] = len, r->mask[0] = 0;
    C->tree->bytes = len, C->paths[0] = &r->children[0], spN_setcc(r, 1);
    C->off = 0, C->poff = 0;
}

static void spI_splitroot(sp_Cursor *C) {
    sp_Node *r = &C->tree->root, save = *r;
    sp_Node *pp = (sp_Node *)spP_ralloc(&C->tree->S->nodes);
    sp_Node *nw = (sp_Node *)spP_ralloc(&C->tree->S->nodes);
    int      i = spK_idx(C, r, 0), mid = spN_cc(&save) / 2;
    int      nc = spN_cc(&save) - mid;
    *pp = save, spN_setcc(pp, mid);
    spN_copy(nw, 0, &save, mid, nc), spN_setcc(nw, nc);
    r->children[0] = pp, r->children[1] = nw, spN_setcc(r, 2);
    r->bytes[0] = spN_sumbytes(pp, 0, mid);
    r->bytes[1] = C->tree->bytes - r->bytes[0];
    r->mask[0] = spM_sumns(pp), r->mask[1] = spM_sumns(nw);
    C->tree->levels++;
    memmove(C->paths + 1, C->paths, C->tree->levels * sizeof(sp_Node **));
    C->paths[0] = &r->children[i >= mid];
    C->paths[1] = &(*C->paths[0])->children[i < mid ? i : i - mid];
}

static void spI_splitchild(sp_Cursor *C, int l) {
    sp_Node *p, *nw, *nd;
    int      cs, i, mid, nc;
    i = spK_idx(C, p = spK_parent(C, l), l);
    nd = p->children[i], mid = spN_cc(nd) / 2, nc = spN_cc(nd) - mid;
    nw = (sp_Node *)spP_ralloc(&C->tree->S->nodes);
    spN_copy(nw, 0, nd, mid, nc), spN_setcc(nw, nc);
    spN_setcc(nd, mid);
    spN_makespace(p, i + 1, 1), p->children[i + 1] = nw;
    p->bytes[i] = spN_sumbytes(nd, 0, mid);
    p->bytes[i + 1] = spN_sumbytes(nw, 0, nc);
    p->mask[i] = spM_sumns(nd), p->mask[i + 1] = spM_sumns(nw);
    if ((cs = spK_idx(C, nd, l + 1)) >= mid) {
        C->paths[l] = &p->children[i + 1];
        C->paths[l + 1] = &nw->children[cs - mid];
    }
}

static void spI_insertrt(sp_Cursor *C, sp_Node *rt, int s, int e) {
    int      l, i;
    sp_Node *p;
    for (l = spK_levels(C); l >= 0; --l)
        if (spN_cc(spK_parent(C, l)) < SP_FANOUT) break;
    if (l < 0) spI_splitroot(C), l = 1;
    for (; l < spK_levels(C); ++l) spI_splitchild(C, l);
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    spN_makespace(p, i, e - s), spN_copy(p, i, rt, s, e - s);
}

static void spI_fillrt(sp_Cursor *C, size_t len, sp_Id id, sp_Mask m) {
    sp_Node *rt = C->tree->S->rt, *p;
    int      i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    sp_Id    sid = spL_id(p, i), nid;
    size_t   po = C->poff, n = p->bytes[i], rm = n - po;
    if (po) p->bytes[i] -= rm, C->off += po, C->paths[spK_levels(C)] += 1;
    spL_setid(rt, 0, spA_born(C->tree, id)), rt->bytes[0] = len;
    rt->mask[0] = m, spN_setcc(rt, 1);
    if (po > 0 && po < n) {
        nid = spA_born(C->tree, sid);
        if (nid == spL_id(rt, 0))
            rt->bytes[0] += rm, rt->mask[0] |= p->mask[i],
                    spA_died(C->tree, nid);
        else
            spL_setid(rt, 1, nid), rt->bytes[1] = rm, rt->mask[1] = p->mask[i],
                                   spN_setcc(rt, 2);
    }
}

static void spI_splitins(sp_Cursor *C, size_t len, sp_Id id, sp_Mask m) {
    sp_Node *p, *rt = &C->tree->S->rt[0];
    int      l, cc, need, mf, i;
    size_t   n, po = C->poff;
    l = spK_levels(C), i = spK_idx(C, p = spK_parent(C, l), l);
    n = p->bytes[i], cc = spN_cc(p);
    assert(po <= n);
    spI_fillrt(C, len, id, m), i = spK_idx(C, p, l), need = spN_cc(rt);
    if ((mf = sp_min(need, SP_FANOUT - cc)) > 0)
        spN_makespace(p, i, mf), spN_copy(p, i, rt, 0, mf);
    if (mf == need)
        spM_up(C, l - 1, len);
    else {
        sp_Delta shrink = po ? (sp_Delta)(n - po) : 0;
        sp_Delta db = (sp_Delta)spN_sumbytes(rt, 0, mf);
        C->paths[l] += mf, spM_up(C, l - 1, db - shrink);
        spI_insertrt(C, rt, mf, need), l = spK_levels(C);
        spM_up(C, l - 1, (sp_Delta)spN_sumbytes(rt, 0, spN_cc(rt)) - db);
        sp_locate(C, C->off);
    }
    C->poff = len;
    spK_seamleaf(C, 0), spK_seamleaf(C, 1);
}

static sp_Id spI_inherit(sp_Cursor *C, int useleft, sp_Mask *m) {
    sp_Node *p = spK_parent(C, spK_levels(C));
    int      l = spK_levels(C) - 1, i = spK_idx(C, p, spK_levels(C));
    size_t   n = p->bytes[i];
    if (C->poff < n && (C->poff > 0 || !useleft))
        return *m = p->mask[i], spL_id(p, i);
    if (C->poff == 0 && useleft && i == 0) {
        /* leaf head: the stream neighbor is the previous leaf's last */
        while (l >= 0 && spK_idx(C, p = spK_parent(C, l), l) == 0) --l;
        if (l < 0) return *m = 0, 0;
        p = p->children[spK_idx(C, p, l) - 1];
        while (++l < spK_levels(C)) p = p->children[spN_cc(p) - 1];
        return *m = p->mask[spN_cc(p) - 1], spL_id(p, spN_cc(p) - 1);
    }
    if (C->poff == 0 && useleft) return *m = p->mask[i - 1], spL_id(p, i - 1);
    assert(sp_offset(C) >= spK_bytes(C));
    if (!useleft) return *m = 0, 0;
    return *m = p->mask[i], spL_id(p, i);
}

static void spI_pad(sp_Cursor *C) {
    size_t excess;
    if (sp_offset(C) <= C->tree->bytes) return;
    excess = sp_offset(C) - C->tree->bytes;
    if (C->tree->bytes == 0)
        spI_onepiece(C, excess, 0);
    else
        spK_locend(C), spI_splitins(C, excess, 0, 0);
    spK_locend(C);
}

static int spI_append(sp_Cursor *C, size_t ins, int growleft) {
    sp_Mask m;
    int     r;
    if (!C || !C->tree) return SP_ERRPARAM;
    if (ins == 0) return SP_OK;
    /* reserve before pad: the pad insertrt may split up to levels+1 */
    r = spP_reserve(C->tree->S, &C->tree->S->nodes, 3 * spK_levels(C) + 4);
    if (r != SP_OK) return r;
    spI_pad(C);
    if (spK_bytes(C) == 0) return spI_onepiece(C, ins, 0), C->poff = ins, SP_OK;
    if (sp_offset(C) >= spK_bytes(C)) spK_locend(C);
    /* inherit left (useleft) or right; cursor ends at the run tail */
    spI_splitins(C, ins, spI_inherit(C, growleft, &m), m);
    return spK_offtail(C), SP_OK;
}

SP_API int sp_append(sp_Cursor *C, size_t ins) { return spI_append(C, ins, 1); }

SP_API int sp_insert(sp_Cursor *C, size_t ins) {
    int r = spI_append(C, ins, 0);
    return r != SP_OK ? r : sp_advance(C, -(sp_Delta)ins);
}

SP_API int sp_splice(sp_Cursor *C, size_t del, size_t ins) {
    int r;
    if (!C || !C->tree) return SP_ERRPARAM;
    if (del == 0 && ins == 0) return SP_OK;
    if (del > 0 && (r = spD_remove(C, del)) != SP_OK) return r;
    return spI_append(C, ins, 1);
}

/* fill */

/* clang-format off */
static int spF_cancel(sp_Cursor *C, sp_Id id, int k)
{ if (k >= 1) spA_died(C->tree, id); return 0; }
/* clang-format on */

static sp_Id spF_arb(sp_Cursor *C, sp_Id in, sp_Id old, sp_Mask *m, int k) {
    sp_Mask  z = 0;
    sp_Tree *T = C->tree;
    if (T->arb && k >= 1 && old) (void)T->arb(T->aud, old, 0, &z);
    return T->arb ? T->arb(T->aud, in, old, m) : in;
}

static void spF_splitspan(sp_Cursor *C) {
    sp_Node *p, *rt = &C->tree->S->rt[0];
    int      i, cc;
    sp_Id    sid;
    size_t   rm;
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    cc = spN_cc(p), rm = p->bytes[i] - C->poff, sid = spL_id(p, i);
    C->off += C->poff, C->poff = 0, C->paths[spK_levels(C)] += 1;
    if (cc < SP_FANOUT) {
        spN_makespace(p, i + 1, 1);
        p->children[i + 1] = p->children[i];
        p->bytes[i] -= rm, p->bytes[i + 1] = rm, p->mask[i + 1] = p->mask[i];
    } else {
        spL_setid(rt, 0, sid), rt->bytes[0] = rm, rt->mask[0] = p->mask[i];
        spN_setcc(rt, 1), p->bytes[i] -= rm;
        spM_up(C, spK_levels(C) - 1, -(sp_Delta)rm);
        spI_insertrt(C, rt, 0, 1);
        spM_up(C, spK_levels(C) - 1, (sp_Delta)rm);
    }
}

static int spF_filterleaf(sp_Cursor *C, size_t len, sp_Id in) {
    sp_Node *p;
    int      k, i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    sp_Id    nid, oid = spL_id(p, i);
    sp_Mask  m = p->mask[i];
    k = (C->poff > 0) + (C->poff + len < p->bytes[i]);
    nid = spF_arb(C, in, oid, &m, k);
    sp_advance(C, (sp_Delta)len);
    if (nid == oid && m == p->mask[i]) return spF_cancel(C, oid, k);
    if (sp_offset(C) >= spK_bytes(C)) spK_locend(C);
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    if (C->poff > 0 && C->poff < p->bytes[i]) spF_splitspan(C);
    sp_advance(C, -(sp_Delta)len);
    if (C->poff > 0) spF_splitspan(C);
    if (k == 2) spA_born(C->tree, oid);
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    spL_setid(p, i, nid), p->mask[i] = nid ? m : 0, C->poff = p->bytes[i];
    spK_seamleaf(C, 0), spK_seamleaf(C, 1);
    return spM_up(C, spK_levels(C) - 1, 0), 0;
}

static int spF_appendspan(sp_Cursor *C, sp_Id id, size_t len, sp_Mask m) {
    sp_Node *p;
    int      cc;
    p = spK_parent(C, spK_levels(C));
    cc = spN_cc(p);
    assert(spK_idx(C, p, spK_levels(C)) >= cc - 1);
    if (cc > 0 && spL_id(p, cc - 1) == id) {
        spA_died(C->tree, id);
        p->bytes[cc - 1] += len, p->mask[cc - 1] |= m, C->poff += len;
        spM_up(C, spK_levels(C) - 1, (sp_Delta)len);
        return 0;
    }
    if (cc >= SP_FANOUT) return 1;
    spL_setid(p, cc, id), p->bytes[cc] = len, p->mask[cc] = id ? m : 0;
    spN_setcc(p, cc + 1);
    C->off += C->poff + len, C->poff = 0;
    C->paths[spK_levels(C)] = &p->children[cc];
    spM_up(C, spK_levels(C) - 1, (sp_Delta)len);
    return 0;
}

static void spF_findroom(sp_Cursor *C, int fl) {
    int      l, cc = 0;
    sp_Node *p = NULL;
    for (l = spK_levels(C); l >= fl; --l) {
        p = spK_parent(C, l), cc = spN_cc(p);
        if (cc < SP_FANOUT) break;
    }
    assert(l >= fl && cc < SP_FANOUT);
    spD_makechain(C, l, spK_levels(C));
}

static void spF_append(sp_Cursor *C, sp_Id id, size_t len, sp_Mask m, int fl) {
    if (spF_appendspan(C, id, len, m)) {
        spF_findroom(C, fl);
        spF_appendspan(C, id, len, m);
    }
}

static void spF_appendleaves(sp_Cursor *C, sp_Id in, int fl, int n) {
    sp_Node *rt = C->tree->S->rt;
    int      i;
    for (i = 0; i < n; ++i) {
        sp_Id nid = spF_arb(C, in, spL_id(&rt[0], i), &rt[0].mask[i], 0);
        if (!nid) rt[0].mask[i] = 0;
        spF_append(C, nid, rt[0].bytes[i], rt[0].mask[i], fl);
    }
    spN_remove(&rt[0], 0, n);
}

static void spF_lower(sp_Cursor *C, int k) {
    sp_Node *rt = C->tree->S->rt;
    spN_copy(&rt[k - 1], 0, rt[k].children[0], 0, spN_cc(rt[k].children[0]));
    spN_setcc(&rt[k - 1], spN_cc(rt[k].children[0]));
    spP_free(&C->tree->S->nodes, rt[k].children[0]);
    spN_remove(&rt[k], 0, 1);
}

static void spF_peel(sp_Cursor *C, sp_Id in, int fl) {
    sp_Node *rt = C->tree->S->rt;
    int      k;
    for (;;) {
        if (spN_cc(&rt[0])) {
            spF_appendleaves(C, in, fl, spN_cc(&rt[0]));
        } else {
            for (k = 1; k < SP_MAX_LEVEL && !spN_cc(&rt[k]); ++k) continue;
            if (k >= SP_MAX_LEVEL) break;
            spF_lower(C, k);
        }
    }
}

static void spF_peelpre(sp_Cursor *C, sp_Cursor *R, sp_Id in, int fl, int l) {
    sp_Node *rt = C->tree->S->rt;
    int      i, k;
    for (;;) {
        if (spN_cc(&rt[0])) {
            if (l == spK_levels(C)) {
                i = (int)(R->paths[l] - rt[0].children);
                if (i == 0) break;
                spF_appendleaves(C, in, fl, i);
                R->paths[l] = rt[0].children;
            } else
                spF_appendleaves(C, in, fl, spN_cc(&rt[0]));
        } else {
            for (k = 1; k <= spK_levels(C) - l; ++k) {
                if (k == spK_levels(C) - l) {
                    i = (int)(R->paths[l] - rt[k].children);
                    if (i > 0) break;
                } else if (spN_cc(&rt[k]))
                    break;
            }
            if (k > spK_levels(C) - l) break;
            spF_lower(C, k);
            if (k == spK_levels(C) - l) R->paths[l] -= 1;
        }
    }
}

static void spF_peeldown(sp_Cursor *C, sp_Cursor *R, sp_Id in, int fl, int l) {
    sp_Node *rt = C->tree->S->rt, *rn;
    int      k;
    spF_peelpre(C, R, in, fl, l);
    if (l == spK_levels(C)) return;
    k = spK_levels(C) - l, rn = rt[k].children[0];
    R->paths[l + 1] = rt[k - 1].children + (R->paths[l + 1] - rn->children);
    spF_lower(C, k);
    spF_peeldown(C, R, in, fl, l + 1);
}

static void spF_filterrange(sp_Cursor *C, sp_Cursor *R, int fl, sp_Id in) {
    sp_Node *rt = C->tree->S->rt, *p;
    int      kl, i, cc, l = spK_levels(C);
    sp_Delta db;
    for (kl = 0; kl < SP_MAX_LEVEL; ++kl) spN_setcc(&rt[kl], 0);
    if (fl < l) {
        for (kl = l; kl > fl; --kl) { /* phase 1: cut L's right */
            p = spK_parent(C, kl), i = spK_idx(C, p, kl), cc = spN_cc(p);
            db = (sp_Delta)spN_sumbytes(p, i + 1, cc);
            spN_copy(&rt[l - kl], 0, p, i + 1, cc - i - 1);
            spN_setcc(&rt[l - kl], cc - i - 1), spN_setcc(p, i + 1);
            spM_up(C, kl - 1, -db);
        }
        spF_peel(C, in, fl);
    }
    p = spK_parent(C, fl), i = spK_idx(C, p, fl), cc = spN_cc(p);
    db = (sp_Delta)spN_sumbytes(p, i + 1, cc); /* phase 2a: cut fl */
    spN_copy(&rt[l - fl], 0, p, i + 1, cc - i - 1);
    spN_setcc(&rt[l - fl], cc - i - 1), spN_setcc(p, i + 1);
    spM_up(C, fl - 1, -db);
    i = (int)(R->paths[fl] - p->children) - (i + 1);
    R->paths[fl] = rt[l - fl].children + i;
    spF_peeldown(C, R, in, fl, fl); /* phases 2b-3 */
    spD_stitch(C, rt);              /* phase 4 */
}

static void spF_fillrange(sp_Cursor *C, sp_Cursor *R, int fl, sp_Id in) {
    sp_Node *p;
    int      i;
    size_t   off0, len, pl, bl, pr;
    off0 = sp_offset(C), len = sp_offset(R) - off0;
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    pl = C->poff, bl = p->bytes[i];
    i = spK_idx(R, p = spK_parent(R, spK_levels(R)), spK_levels(R));
    pr = R->poff, assert(pr <= p->bytes[i]);
    spF_filterrange(C, R, fl, in);
    sp_locate(C, off0);
    spF_filterleaf(C, bl - pl, in);
    sp_locate(C, off0 + len - pr);
    if (pr > 0) spF_filterleaf(C, pr, in);
}

SP_API int sp_fill(sp_Cursor *C, sp_Id id, size_t len) {
    sp_Cursor R;
    int       fl, r;
    size_t    off0;
    sp_Mask   m = 0;
    if (!C || !C->tree) return SP_ERRPARAM;
    if (len == 0) return SP_OK;
    /* reserve before pad: both pads may split up to levels+1 nodes */
    r = spP_reserve(C->tree->S, &C->tree->S->nodes, 6 * spK_levels(C) + 7);
    if (r != SP_OK) return r;
    spI_pad(C), off0 = sp_offset(C); /* pad [bytes, C) from virtual */
    /* seek builds R (off0 + len may sit virtual) */
    r = sp_seek(&R, C->tree, off0 + len), assert(r == SP_OK);
    if (sp_offset(&R) > C->tree->bytes) /* pad [bytes, off0+len) & announces */
        spI_pad(&R), spF_arb(&R, 0, 0, &m, 0), sp_locate(C, off0);
    else if (sp_offset(&R) >= C->tree->bytes)
        spK_locend(&R);
    fl = spD_splitpaths(C, &R);
    if (fl > spK_levels(C))
        spF_filterleaf(C, len, id);
    else
        spF_fillrange(C, &R, fl, id);
    return spK_offtail(C), SP_OK;
}

/* prune-clear: bulk arb per container, cursor lands via sp_next */

static void spC_compactleaf(sp_Tree *T, sp_Node *p) {
    int w = 0, r;
    for (r = 1; r < spN_cc(p); ++r) {
        if (spL_id(p, w) == spL_id(p, r)) {
            spA_died(T, spL_id(p, r));
            p->bytes[w] += p->bytes[r], p->mask[w] |= p->mask[r];
        } else {
            ++w, p->children[w] = p->children[r];
            p->bytes[w] = p->bytes[r], p->mask[w] = p->mask[r];
        }
    }
    spN_setcc(p, w + 1);
}

static void spC_clearleaf(sp_Cursor *C, sp_Mask bit, sp_Id id, size_t endoff) {
    sp_Node *p = spK_parent(C, spK_levels(C));
    int      i, cc = spN_cc(p), changed = 0;
    for (i = 0; i < cc; ++i)
        if (p->mask[i] & bit) {
            sp_Id nid = spF_arb(C, id, spL_id(p, i), &p->mask[i], 0);
            if (!nid) p->mask[i] = 0;
            if (nid != spL_id(p, i)) spL_setid(p, i, nid), changed = 1;
        }
    if (!changed) {
        /* masks may move without id changes: refresh or prunes go stale */
        spM_up(C, spK_levels(C) - 1, 0);
        sp_locate(C, endoff);
        return;
    }
    spC_compactleaf(C->tree, p);
    sp_locate(C, endoff - 1);
    spM_up(C, spK_levels(C) - 1, 0);
    sp_locate(C, endoff);
}

SP_API int sp_clear(sp_Tree *T, int ns, sp_Id id) {
    sp_Cursor C;
    sp_Mask   bit;
    size_t    endoff;
    sp_Node  *p;
    int       i;
    if (T == NULL || ns < 1 || ns > (int)SP_MASK_BITS) return SP_ERRPARAM;
    bit = spM_bit(ns), sp_seek(&C, T, 0);
    for (;;) {
        i = spK_idx(&C, p = spK_parent(&C, spK_levels(&C)), spK_levels(&C));
        if (T->bytes == 0 || C.poff >= p->bytes[i]) break;
        if (p->mask[i] & bit) {
            endoff = C.off + spN_sumbytes(p, i, spN_cc(p));
            spC_clearleaf(&C, bit, id, endoff);
            continue; /* clearleaf lands at the container end */
        }
        if (!sp_next(&C, ns, NULL)) break;
    }
    return SP_OK;
}

SP_NS_END

#endif /* SP_IMPLEMENTATION */
