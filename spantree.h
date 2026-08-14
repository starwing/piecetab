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

#include <stddef.h>

#define SP_OK       (0)  /* No error */
#define SP_ERRPARAM (-1) /* Invalid parameter */
#define SP_ERRMEM   (-2) /* Memory allocation failed */

SP_NS_BEGIN

typedef struct sp_State  sp_State;  /* allocator + pools      */
typedef struct sp_Tree   sp_Tree;   /* one span tree          */
typedef struct sp_Cursor sp_Cursor; /* traversal state        */

typedef ptrdiff_t sp_Delta;
typedef size_t    sp_Id;

typedef void *sp_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/* state */

/* construction */
SP_API sp_State *sp_open(sp_Alloc *allocf, void *ud);
SP_API void      sp_close(sp_State *S);

/* tree */

/* construction */
SP_API sp_Tree *sp_newtree(sp_State *S);
SP_API void     sp_freetree(sp_Tree *T);

/* metrics */
SP_API size_t sp_bytes(const sp_Tree *T);

/* cursor */

/* construction */
SP_API int sp_seek(sp_Cursor *C, sp_Tree *T, size_t off);

/* navigate */
SP_API int sp_locate(sp_Cursor *C, size_t off);
SP_API int sp_advance(sp_Cursor *C, sp_Delta d);

/* coloring */

/* Arbiter: merges the current segment id with an incoming write id.
 * spantree writes whatever the arbiter returns; a 0 return clears the
 * segment (the arbiter owns all merge semantics, incl. finalizers). */
typedef unsigned sp_Arbiterf(void *ud, sp_Id old, sp_Id in);

/* fill: replace [cursor, cursor+len) with the arbiter's merge of each
 * segment's current id and the incoming id (0 clears). */
SP_API void sp_setarbiter(sp_Tree *T, sp_Arbiterf *cb, void *ud);
SP_API int  sp_fill(sp_Cursor *C, sp_Id id, size_t len);

/* read: current segment id and length; the returned pointer is
 * invalidated by any tree edit. NULL at the ends of the tree.
 * next/prev first move the cursor onto the segment they read. */
SP_API const sp_Id *sp_style(sp_Cursor *C, size_t *plen);
SP_API const sp_Id *sp_next(sp_Cursor *C, size_t *plen);
SP_API const sp_Id *sp_prev(sp_Cursor *C, size_t *plen);

/* editing (edit sync, no content: lengths only) */

/* splice: delete del bytes then insert ins bytes at the cursor; the
 * inserted run inherits the left segment id. */
SP_API int sp_splice(sp_Cursor *C, size_t del, size_t ins);

/* append: insert at the cursor, inherit the left segment id. */
SP_API int sp_append(sp_Cursor *C, size_t ins);

/* insert: insert at the cursor, inherit the right segment id. */
SP_API int sp_insert(sp_Cursor *C, size_t ins);

/* remove: delete the bytes between the two cursors. */
SP_API int sp_remove(sp_Cursor *L, sp_Cursor *R);

/* cursor definition */

#define SP_MAX_LEVEL 16

struct sp_Cursor {
    struct sp_Node **paths[SP_MAX_LEVEL]; /* root-to-leaf child slot ptrs */
    struct sp_Tree  *tree;                /* tree under navigation */
    size_t           poff;                /* offset in current leaf seg */
    size_t           off;                 /* bytes before current seg */
};

/* query */
#define sp_offset(C) ((C)->off + (C)->poff)

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

/* makeroom needs at most 2 free slots; a split of a full node leaves
 * FANOUT/2 free in the cursor's half, so require FANOUT >= 4. */
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
    size_t          bytes[SP_FANOUT];    /* subtree sum or segment length */
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
    sp_Arbiterf   *arb;    /* segment merge callback */
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

/* clang-format off */
static size_t spN_sumbytes(const sp_Node *n, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += n->bytes[i]; return s; }
/* clang-format on */

static void spN_copy(sp_Node *d, int di, const sp_Node *s, int si, int n) {
    assert(di + n <= SP_FANOUT && si + n <= SP_FANOUT);
    memcpy(&d->children[di], &s->children[si], n * sizeof(sp_Node *));
    memcpy(&d->bytes[di], &s->bytes[si], n * sizeof(size_t));
}

static void spN_move(sp_Node *d, int di, int si, int n) {
    assert(di + n <= SP_FANOUT && si + n <= SP_FANOUT);
    memmove(&d->children[di], &d->children[si], n * sizeof(sp_Node *));
    memmove(&d->bytes[di], &d->bytes[si], n * sizeof(size_t));
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

static void spN_purge(sp_State *S, sp_Node *p, int k, int s, int e) {
    int i;
    assert(s <= e && e <= spN_cc(p));
    if (k == 0) return; /* leaf container: children hold ids, not pointers */
    for (i = s; i < e; ++i) {
        spN_purge(S, p->children[i], k - 1, 0, spN_cc(p->children[i]));
        spP_free(&S->nodes, p->children[i]);
    }
}

static void spM_up(sp_Cursor *C, int l, sp_Delta db) {
    int      i;
    sp_Node *p;
    if (db == 0) return;
    for (; l >= 0; --l) {
        i = spK_idx(C, p = spK_parent(C, l), l), p->bytes[i] += db;
    }
    C->tree->bytes += db;
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
    memset(S, 0, sizeof(sp_State));
    S->alloc_ud = ud, S->allocf = allocf;
    spP_init(&S->nodes, sizeof(sp_Node));
    return S;
}

/* clang-format off */
SP_API void sp_close(sp_State *S)
{ if (S) spP_destroy(S, &S->nodes), S->allocf(S->alloc_ud, S, sizeof(sp_State), 0); }
/* clang-format on */

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
    spN_purge(t->S, &t->root, t->levels, 0, spN_cc(&t->root));
    t->S->allocf(t->S->alloc_ud, t, sizeof(sp_Tree), 0);
}

SP_API size_t sp_bytes(const sp_Tree *t) { return t ? t->bytes : 0; }

SP_API void sp_setarbiter(sp_Tree *t, sp_Arbiterf *cb, void *ud) {
    if (t) t->arb = cb, t->aud = ud;
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

static void spK_locend(sp_Cursor *C) {
    sp_Node *n = &C->tree->root;
    int      l;
    if (spK_levels(C) == 0 && spN_cc(n) == 0) {
        C->paths[0] = n->children, C->off = 0, C->poff = 0;
        return;
    }
    for (l = 0; l < spK_levels(C); ++l)
        n = *(C->paths[l] = &n->children[spN_cc(n) - 1]);
    assert(spN_cc(n)), C->paths[l] = &n->children[spN_cc(n) - 1];
    C->poff = n->bytes[spN_cc(n) - 1], C->off = C->tree->bytes - C->poff;
}

SP_API int sp_seek(sp_Cursor *C, sp_Tree *T, size_t off) {
    if (C == NULL || T == NULL) return SP_ERRPARAM;
    memset(C, 0, sizeof(sp_Cursor)), C->tree = T;
    if (off >= T->bytes)
        spK_locend(C), C->poff = 0, C->off = off;
    else
        spK_findleaf(C, 0, &off), C->poff = off;
    return SP_OK;
}

SP_API int sp_locate(sp_Cursor *C, size_t off) {
    if (C == NULL || C->tree == NULL) return SP_ERRPARAM;
    C->off = 0;
    if (off >= C->tree->bytes)
        spK_locend(C), C->poff = 0, C->off = off;
    else
        spK_findleaf(C, 0, &off), C->poff = off;
    return SP_OK;
}

static void spK_forwardoff(sp_Cursor *C, size_t d) {
    sp_Node *p;
    int      l, i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    size_t   in = p->bytes[i] - C->poff;
    if (d < in) {
        C->poff += d;
        return;
    }
    d -= in, C->off += p->bytes[i], C->poff = 0;
    for (l = spK_levels(C); l >= 0; --l) {
        i = spK_idx(C, p = spK_parent(C, l), l) + 1;
        for (; i < spN_cc(p) && d >= p->bytes[i]; ++i)
            d -= p->bytes[i], C->off += p->bytes[i];
        if (i < spN_cc(p)) break;
    }
    assert(l >= 0 && i < spN_cc(p)), C->paths[l] = &p->children[i];
    spK_findleaf(C, l + 1, &d), C->poff = d;
}

static void spK_backwardoff(sp_Cursor *C, size_t d) {
    sp_Node *p = NULL;
    int      l, i = 0;
    if (d <= C->poff) {
        C->poff -= d;
        return;
    }
    d -= C->poff, C->poff = 0;
    for (l = spK_levels(C); l >= 0; --l) {
        i = spK_idx(C, p = spK_parent(C, l), l) - 1;
        for (; i >= 0 && d > p->bytes[i]; --i)
            d -= p->bytes[i], C->off -= p->bytes[i];
        if (i >= 0) break;
    }
    assert(l >= 0 && i >= 0), d = p->bytes[i] - d, C->off -= p->bytes[i];
    C->paths[l] = &p->children[i];
    spK_findleaf(C, l + 1, &d), C->poff = d;
}

SP_API int sp_advance(sp_Cursor *C, sp_Delta d) {
    size_t off, step;
    if (C == NULL || C->tree == NULL) return SP_ERRPARAM;
    if (d == 0 || C->tree->bytes == 0) return SP_OK;
    off = sp_offset(C);
    if (d > 0) {
        if (off + (size_t)d >= C->tree->bytes)
            return spK_locend(C), C->poff = 0, C->off = off + (size_t)d, SP_OK;
        return spK_forwardoff(C, (size_t)d), SP_OK;
    }
    step = (size_t)(-d);
    if (off > C->tree->bytes && step <= off - C->tree->bytes)
        return spK_locend(C), C->poff = 0, C->off = off - step, SP_OK;
    if (off >= C->tree->bytes) {
        off = step > off ? 0 : off - step;
        C->off = 0, spK_findleaf(C, 0, &off), C->poff = off;
        return SP_OK;
    }
    return spK_backwardoff(C, sp_min(off, step)), SP_OK;
}

SP_API const sp_Id *sp_style(sp_Cursor *C, size_t *plen) {
    sp_Node *p;
    int      i;
    if (C == NULL || C->tree == NULL) return NULL;
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    if (C->off >= C->tree->bytes || C->poff >= p->bytes[i])
        return (void)(plen && (*plen = 0)), NULL;
    if (plen) *plen = p->bytes[i] - C->poff;
    return (const sp_Id *)&p->children[i];
}

SP_API const sp_Id *sp_next(sp_Cursor *C, size_t *plen) {
    int      i, l;
    size_t   bc;
    sp_Node *p;
    if (C == NULL || C->tree == NULL) return NULL;
    if (C->off >= C->tree->bytes) return (void)(plen && (*plen = 0)), NULL;
    l = spK_levels(C), i = spK_idx(C, p = spK_parent(C, l), l);
    if (C->poff == p->bytes[i]) return (void)(plen && (*plen = 0)), NULL;
    bc = p->bytes[i] - C->poff;
    while (i + 1 >= spN_cc(p) && --l >= 0)
        i = spK_idx(C, p = spK_parent(C, l), l);
    if (l < 0) return C->poff += bc, plen && (*plen = 0), (const sp_Id *)NULL;
    C->paths[l] += 1, C->off += bc + C->poff, C->poff = 0;
    while (++l <= spK_levels(C)) C->paths[l] = &spK_parent(C, l)->children[0];
    return sp_style(C, plen);
}

SP_API const sp_Id *sp_prev(sp_Cursor *C, size_t *plen) {
    sp_Node *p;
    int      i, l;
    if (C == NULL || C->tree == NULL) return NULL;
    if (C->off >= C->tree->bytes) spK_locend(C);
    l = spK_levels(C), i = spK_idx(C, p = spK_parent(C, l), l);
    if (C->poff > 0)
        return (void)(plen && (*plen = C->poff)),
               C->poff = 0, (const sp_Id *)&p->children[i];
    if (C->off == 0) return (void)(plen && (*plen = 0)), NULL;
    while (i <= 0 && --l >= 0) i = spK_idx(C, p = spK_parent(C, l), l);
    assert(l >= 0 && i > 0), C->paths[l] -= 1, i -= 1;
    while (++l <= spK_levels(C))
        p = spK_parent(C, l), C->paths[l] = &p->children[i = spN_cc(p) - 1];
    C->off -= p->bytes[i], C->poff = 0;
    return (void)(plen && (*plen = p->bytes[i])),
           (const sp_Id *)&p->children[i];
}

/* remove / balance / stitch */

static void spD_trimright(sp_Cursor *L) {
    sp_Node *p;
    int      l = spK_levels(L), i = spK_idx(L, p = spK_parent(L, l), l);
    spM_up(L, l, -(sp_Delta)(p->bytes[i] - L->poff));
}

static void spD_trimleft(sp_Cursor *R) {
    sp_Node *p;
    int      l = spK_levels(R), i = spK_idx(R, p = spK_parent(R, l), l);
    p->bytes[i] -= R->poff;
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
        k = l - kl, spN_purge(S, p, k, i + 1, cc);
        spN_remove(p, i + 1, cc - (i + 1));
        ir = spK_idx(R, p = spK_parent(R, kl), kl), cc = spN_cc(p);
        i = ir + (k || p->bytes[ir] == 0);
        spN_copy(&rt[k], 0, p, i, cc - i), spN_setcc(&rt[k], cc - i);
        spN_purge(S, p, k, 0, ir);
        if (kl > fl + 1) spP_free(&S->nodes, p); /* emptied R-path shell */
        spN_setcc(p, 0);
    }
    p = spK_parent(R, fl), i = spK_idx(R, p, fl), cc = spN_cc(p);
    k = l - fl, ir = i, i += (k || p->bytes[i] == 0);
    spN_copy(&rt[k], 0, p, i, spN_setcc(&rt[k], cc - i));
    spN_setcc(p, i), i = spK_idx(L, p, fl);
    p->bytes[i] -= db, db += spN_sumbytes(p, i + 1, cc);
    spM_up(L, fl - 1, -db);
    if (k > 0) spN_purge(S, p, k, i + 1, ir + 1);
    spN_remove(p, i + 1, spN_cc(p) - (i + 1));
}

static void spD_makechain(sp_Cursor *C, int from, int to) {
    sp_Node *p, *nn = NULL;
    int      l;
    assert(from < to);
    for (l = from; l < to; ++l) {
        nn = (sp_Node *)spP_ralloc(&C->tree->S->nodes);
        p = spK_parent(C, l), nn->child_count = 0;
        p->bytes[spN_cc(p)] = 0, C->paths[l] = &p->children[spN_cc(p)];
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

static int spD_foldnode(sp_Cursor *C, int lfirst, int l) {
    sp_Node  *p;
    int       cL, cR, i = spK_idx(C, p = spK_parent(C, l), l);
    sp_Node **ns = &p->children[i], *o = *ns;
    sp_Delta  ds;
    int       dn;
    if (spN_cc(p) < 2 || spN_cc(ns[0]) > SP_FANOUT / 2) return 0;
    if ((i && lfirst) || i == spN_cc(p) - 1) ns -= 1, i -= 1;
    cL = spN_cc(ns[0]), cR = spN_cc(ns[1]);
    if (cL + cR <= SP_FANOUT) {
        spN_copy(ns[0], cL, ns[1], 0, cR);
        spN_setcc(ns[0], cL + cR), spN_setcc(ns[1], 0);
        p->bytes[i] += p->bytes[i + 1];
        if (*ns != o) {
            C->paths[l + 1] += ns[0]->children - ns[1]->children + cL;
            C->paths[l] -= 1;
        }
        spP_free(&C->tree->S->nodes, ns[1]);
        return spN_remove(p, i + 1, 1), 1;
    }
    dn = spD_balancenode(ns, (*ns == o), &ds);
    assert(dn != 0 && (dn < 0) != (*ns != o));
    p->bytes[i] -= ds, p->bytes[i + 1] += ds;
    if (*ns != o) C->paths[l + 1] += dn;
    return 0;
}

static void spD_rebalance(sp_Cursor *C, int l) {
    assert(l == 0 || l < spK_levels(C));
    for (; l >= 0 && l < spK_levels(C); --l) {
        sp_Node *p = spK_parent(C, l);
        if (spN_cc(p->children[spK_idx(C, p, l)]) >= SP_FANOUT / 2) break;
        if (spN_cc(p) < 2) break;
        if (!spD_foldnode(C, 0, l)) break;
    }
    while (spK_levels(C) > 0 && spN_cc(&C->tree->root) == 1) {
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
        if (i < spN_cc(p)) p->bytes[i] += db;
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
    size_t   bc;
    assert(cc > 0);
    bc = p->bytes[cc - 1];
    if (spL_id(p, cc - 1) == spL_id(rt, 0)) {
        rt->bytes[0] += bc, rt->children[0] = p->children[cc - 1];
        spM_up(C, l - 1, -(sp_Delta)bc);
        if (spK_idx(C, p, l) == cc) C->off -= bc, C->poff = bc;
        C->paths[l] = &p->children[cc - 1], spN_setcc(p, cc - 1);
    } else
        C->off += C->poff, C->poff = 0, C->paths[l] = &p->children[cc];
}

static void spD_mergeleft(sp_Cursor *L, sp_Node *rt) {
    int      dl, l = spK_levels(L), i, dl0;
    sp_Node *p;
    size_t   bc;
    for (dl = l - 1; dl >= 0 && spK_idx(L, spK_parent(L, dl), dl) == 0; --dl)
        continue;
    if (dl < 0) return;
    dl0 = dl;
    p = spK_parent(L, dl0), i = spK_idx(L, p, dl0) - 1;
    for (; dl < l - 1; ++dl) p = p->children[i], i = spN_cc(p) - 1;
    p = p->children[i], i = spN_cc(p) - 1;
    if (spL_id(p, i) != spL_id(rt, 0)) return;
    bc = rt->bytes[0], p->bytes[i] += bc;
    p = spK_parent(L, dl0), i = spK_idx(L, p, dl0) - 1;
    for (dl = dl0; dl < l; ++dl) {
        p->bytes[i] += bc; /* left-neighbor chain, every level down */
        if (dl + 1 < l) p = p->children[i], i = spN_cc(p) - 1;
    }
    spM_up(L, dl0 - 1, (sp_Delta)bc);
    spN_remove(rt, 0, 1);
}

static void spD_stitch(sp_Cursor *L, sp_Node *rt) {
    int      cc, l = spK_levels(L);
    sp_Node *p = spK_parent(L, l);
    assert(L->tree->S->nodes.freed_obj >= (size_t)(spK_levels(L) + 2));
    if ((cc = spN_cc(p)) && p->bytes[cc - 1] == 0)
        spN_remove(p, cc - 1, 1), cc -= 1;
    if (cc && spN_cc(&rt[0]))
        spD_mergeleaf(L, rt);
    else if (!cc && spN_cc(&rt[0]))
        spD_mergeleft(L, rt);
    spD_stitchnode(L, rt), spD_rebalance(L, 0);
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
        spN_remove(p, i, 1);
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
        C->poff = p->bytes[spN_cc(p) - 1];
        C->off -= p->bytes[spN_cc(p) - 1];
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
    spL_setid(r, 0, id), r->bytes[0] = len, C->tree->bytes = len;
    C->paths[0] = &r->children[0], spN_setcc(r, 1);
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

static void spI_fillrt(sp_Cursor *C, size_t len, sp_Id id) {
    sp_Node *rt = C->tree->S->rt, *p;
    int      i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    sp_Id    sid = spL_id(p, i);
    size_t   po = C->poff, n = p->bytes[i], rm = n - po;
    if (po) p->bytes[i] -= rm, C->off += po, C->paths[spK_levels(C)] += 1;
    spL_setid(rt, 0, id), rt->bytes[0] = len, spN_setcc(rt, 1);
    if (po > 0 && po < n) {
        spL_setid(rt, 1, sid), rt->bytes[1] = rm;
        spN_setcc(rt, 2);
    }
}

static void spI_splitins(sp_Cursor *C, size_t len, sp_Id id) {
    sp_Node *p, *rt = &C->tree->S->rt[0];
    int      l, cc, need, m, i;
    size_t   n, po = C->poff;
    l = spK_levels(C), i = spK_idx(C, p = spK_parent(C, l), l);
    n = p->bytes[i], cc = spN_cc(p);
    assert(po <= n), need = 1 + (po > 0 && po < n);
    spI_fillrt(C, len, id), i = spK_idx(C, p, l);
    if ((m = sp_min(need, SP_FANOUT - cc)) > 0)
        spN_makespace(p, i, m), spN_copy(p, i, rt, 0, m);
    if (m == need)
        spM_up(C, l - 1, len);
    else {
        sp_Delta shrink = po ? (sp_Delta)(n - po) : 0;
        sp_Delta db = (sp_Delta)spN_sumbytes(rt, 0, m);
        C->paths[l] += m, spM_up(C, l - 1, db - shrink);
        spI_insertrt(C, rt, m, need), l = spK_levels(C);
        spM_up(C, l - 1, (sp_Delta)spN_sumbytes(rt, 0, spN_cc(rt)) - db);
        sp_locate(C, C->off);
    }
    C->poff = len;
}

static void spI_merge(sp_Cursor *C);

static void spI_pad(sp_Cursor *C) {
    size_t excess;
    if (sp_offset(C) <= C->tree->bytes) return;
    excess = sp_offset(C) - C->tree->bytes;
    if (C->tree->bytes == 0)
        spI_onepiece(C, excess, 0);
    else {
        spK_locend(C), spI_splitins(C, excess, 0);
        spI_merge(C);
    }
    spK_locend(C);
}

static sp_Id spI_inherit(sp_Cursor *C, int useleft) {
    sp_Node *p = spK_parent(C, spK_levels(C));
    int      l = spK_levels(C) - 1, i = spK_idx(C, p, spK_levels(C));
    size_t   n = p->bytes[i];
    if (C->poff < n && (C->poff > 0 || !useleft)) return spL_id(p, i);
    if (C->poff == 0 && useleft && i == 0) {
        /* leaf head: the stream neighbor is the previous leaf's last */
        while (l >= 0 && spK_idx(C, p = spK_parent(C, l), l) == 0) --l;
        if (l < 0) return 0;
        p = p->children[spK_idx(C, p, l) - 1];
        while (++l < spK_levels(C)) p = p->children[spN_cc(p) - 1];
        return spL_id(p, spN_cc(p) - 1);
    }
    if (C->poff == 0 && useleft) return spL_id(p, i - 1);
    return useleft ? spL_id(p, i) : 0;
}

static void spI_foldchain(sp_Cursor *C, sp_Node *cur) {
    int x = spK_levels(C) - 1;
    while (spN_cc(cur) < (x == spK_levels(C) - 1 ? 2 : SP_FANOUT / 2)) {
        if (spN_cc(cur) == 0) {
            spP_free(&C->tree->S->nodes, cur);
            spN_remove(spK_parent(C, x), spK_idx(C, spK_parent(C, x), x), 1);
            if (x == 0) return;
        } else {
            if (x == 0 && spN_cc(spK_parent(C, 0)) <= 1) return;
            assert(spN_cc(spK_parent(C, x)) > 1);
            if (!spD_foldnode(C, 0, x)) return;
            if (x == 0 || spN_cc(spK_parent(C, x)) >= SP_FANOUT / 2) return;
        }
        cur = spK_parent(C, x), x -= 1;
    }
}

static int spI_neighbor(sp_Cursor *C, int *pdl, int right) {
    sp_Node *p;
    int      dl = spK_levels(C) - 1, i = spK_idx(C, p = spK_parent(C, dl), dl);
    while ((right ? i == spN_cc(p) - 1 : i == 0) && --dl >= 0)
        i = spK_idx(C, p = spK_parent(C, dl), dl);
    if (dl < 0) return 0;
    *pdl = dl, C->paths[dl] += right ? 1 : -1;
    while (++dl <= spK_levels(C))
        p = spK_parent(C, dl),
        C->paths[dl] = &p->children[right ? 0 : spN_cc(p) - 1];
    return 1;
}

static int spI_mergeleft(sp_Cursor *C) {
    sp_Node  *pr = spK_parent(C, spK_levels(C)), *pl, *pf, *rl, *cl;
    sp_Node **sav[SP_MAX_LEVEL];
    int       dl, i, l;
    size_t    bc, bl, off0 = sp_offset(C);
    for (l = 0; l <= spK_levels(C); ++l) sav[l] = C->paths[l];
    if (!spI_neighbor(C, &dl, 0)) return 0;
    pf = spK_parent(C, dl), i = spK_idx(C, pf, dl);
    pl = spK_parent(C, spK_levels(C)), bl = pl->bytes[spN_cc(pl) - 1];
    if (spL_id(pl, spN_cc(pl) - 1) != spL_id(pr, 0)) {
        for (l = 0; l <= spK_levels(C); ++l) C->paths[l] = sav[l];
        return 0;
    }
    bc = pr->bytes[0], pf->bytes[i] += bc, pf->bytes[i + 1] -= bc;
    for (l = dl + 1; l < spK_levels(C); ++l) {
        rl = spK_parent(C, l), cl = *sav[l - 1];
        rl->bytes[spN_cc(rl) - 1] += bc;
        cl->bytes[sav[l] - cl->children] -= bc;
    }
    pl->bytes[spN_cc(pl) - 1] += bc, spN_remove(pr, 0, 1);
    if (spN_cc(pr) < 2 && spK_levels(C) > 0) {
        for (l = 0; l <= spK_levels(C); ++l) C->paths[l] = sav[l];
        spI_foldchain(C, pr), sp_locate(C, C->off);
        C->poff = off0 - C->off;
        spD_rebalance(C, 0);
    } else
        C->off -= bl, C->poff += bl;
    return 1;
}

static int spI_mergeright(sp_Cursor *C) {
    sp_Node  *pr = spK_parent(C, spK_levels(C)), *nr, *pf, *rl, *cl;
    sp_Node **sav[SP_MAX_LEVEL];
    int       dl, i, l;
    size_t    bc, offn = sp_offset(C);
    for (l = 0; l <= spK_levels(C); ++l) sav[l] = C->paths[l];
    if (!spI_neighbor(C, &dl, 1)) return 0;
    pf = spK_parent(C, dl), i = spK_idx(C, pf, dl) - 1;
    nr = spK_parent(C, spK_levels(C));
    if (spL_id(nr, 0) != spL_id(pr, spN_cc(pr) - 1)) {
        for (l = 0; l <= spK_levels(C); ++l) C->paths[l] = sav[l];
        return 0;
    }
    bc = nr->bytes[0], pr->bytes[spN_cc(pr) - 1] += bc;
    pf->bytes[i] += bc, pf->bytes[i + 1] -= bc;
    for (l = dl + 1; l < spK_levels(C); ++l) {
        rl = spK_parent(C, l), cl = *sav[l - 1];
        rl->bytes[0] -= bc;
        cl->bytes[sav[l] - cl->children] += bc;
    }
    spN_remove(nr, 0, 1);
    if (spN_cc(nr) < 2 && spK_levels(C) > 0) {
        spI_foldchain(C, nr), sp_locate(C, C->off);
        C->poff = offn - C->off;
        spD_rebalance(C, 0);
    } else
        for (l = 0; l <= spK_levels(C); ++l) C->paths[l] = sav[l];
    return 1;
}

static void spI_merge(sp_Cursor *C) {
    sp_Node *p;
    int      i;
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    assert(C->poff == p->bytes[i]);
    while (i > 0 && spL_id(p, i - 1) == spL_id(p, i)) {
        C->off -= p->bytes[i - 1], C->poff += p->bytes[i - 1];
        p->bytes[i - 1] += p->bytes[i];
        spN_remove(p, i, 1), i -= 1;
        C->paths[spK_levels(C)] -= 1;
    }
    if (i == 0 && spK_levels(C) > 0) spI_mergeleft(C);
    i = spK_idx(C, p = spK_parent(C, spK_levels(C)), spK_levels(C));
    while (i + 1 < spN_cc(p) && spL_id(p, i) == spL_id(p, i + 1)) {
        p->bytes[i] += p->bytes[i + 1];
        spN_remove(p, i + 1, 1);
    }
    if (i + 1 == spN_cc(p) && spK_levels(C) > 0) spI_mergeright(C);
}

static int spI_append(sp_Cursor *C, size_t ins, int growleft) {
    int r;
    if (!C || !C->tree) return SP_ERRPARAM;
    if (ins == 0) return SP_OK;
    /* reserve before pad: the pad insertrt may split up to levels+1 */
    r = spP_reserve(C->tree->S, &C->tree->S->nodes, 3 * spK_levels(C) + 4);
    if (r != SP_OK) return r;
    spI_pad(C);
    if (spK_bytes(C) == 0) return spI_onepiece(C, ins, 0), C->poff = ins, SP_OK;
    if (sp_offset(C) >= spK_bytes(C)) spK_locend(C);
    /* inherits left when useleft, right otherwise; the cursor ends at
     * the run's tail either way (sp_insert walks it back via advance) */
    spI_splitins(C, ins, spI_inherit(C, growleft));
    spI_merge(C);
    return SP_OK;
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

static sp_Id spF_arb(sp_Cursor *C, sp_Id old, sp_Id in) {
    return C->tree->arb ? C->tree->arb(C->tree->aud, old, in) : in;
}

static void spF_splitseg(sp_Cursor *C) {
    sp_Node *p, *rt = &C->tree->S->rt[0];
    int      i, cc;
    size_t   rm;
    sp_Id    sid;
    p = spK_parent(C, spK_levels(C));
    i = spK_idx(C, p, spK_levels(C));
    cc = spN_cc(p), rm = p->bytes[i] - C->poff, sid = spL_id(p, i);
    C->off += C->poff, C->poff = 0, C->paths[spK_levels(C)] += 1;
    if (cc < SP_FANOUT) {
        spN_makespace(p, i + 1, 1);
        p->children[i + 1] = p->children[i];
        p->bytes[i] -= rm, p->bytes[i + 1] = rm;
    } else {
        spL_setid(rt, 0, sid), rt->bytes[0] = rm, spN_setcc(rt, 1);
        p->bytes[i] -= rm;
        spM_up(C, spK_levels(C) - 1, -(sp_Delta)rm);
        spI_insertrt(C, rt, 0, 1);
        spM_up(C, spK_levels(C) - 1, (sp_Delta)rm);
    }
}

static void spF_filterleaf(sp_Cursor *C, size_t len, sp_Id in) {
    sp_Node *p;
    int      i;
    sp_Id    sid, nid;
    p = spK_parent(C, spK_levels(C));
    i = spK_idx(C, p, spK_levels(C));
    sid = spL_id(p, i), nid = spF_arb(C, sid, in);
    sp_advance(C, (sp_Delta)len);
    if (nid == sid) return;
    if (sp_offset(C) >= spK_bytes(C)) spK_locend(C);
    p = spK_parent(C, spK_levels(C));
    i = spK_idx(C, p, spK_levels(C));
    if (C->poff > 0 && C->poff < p->bytes[i]) spF_splitseg(C);
    sp_advance(C, -(sp_Delta)len);
    if (C->poff > 0) spF_splitseg(C);
    p = spK_parent(C, spK_levels(C));
    i = spK_idx(C, p, spK_levels(C));
    spL_setid(p, i, nid), C->poff = p->bytes[i];
    spI_merge(C);
}

static int spF_appendseg(sp_Cursor *C, sp_Id id, size_t len) {
    sp_Node *p;
    int      cc;
    p = spK_parent(C, spK_levels(C));
    cc = spN_cc(p);
    assert(spK_idx(C, p, spK_levels(C)) >= cc - 1);
    if (cc > 0 && spL_id(p, cc - 1) == id) {
        p->bytes[cc - 1] += len, C->poff += len;
        spM_up(C, spK_levels(C) - 1, (sp_Delta)len);
        return 0;
    }
    if (cc >= SP_FANOUT) return 1;
    spL_setid(p, cc, id), p->bytes[cc] = len, spN_setcc(p, cc + 1);
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

static void spF_append(sp_Cursor *C, sp_Id id, size_t len, int fl) {
    if (spF_appendseg(C, id, len)) {
        spF_findroom(C, fl);
        spF_appendseg(C, id, len);
    }
}

static void spF_appendleaves(sp_Cursor *C, sp_Id in, int fl, int n) {
    sp_Node *rt = C->tree->S->rt;
    int      i;
    for (i = 0; i < n; ++i)
        spF_append(C, spF_arb(C, spL_id(&rt[0], i), in), rt[0].bytes[i], fl);
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
    size_t   off0, len, pl, bl, pr, br;
    off0 = sp_offset(C), len = sp_offset(R) - off0;
    p = spK_parent(C, spK_levels(C)), i = spK_idx(C, p, spK_levels(C));
    pl = C->poff, bl = p->bytes[i];
    p = spK_parent(R, spK_levels(R)), i = spK_idx(R, p, spK_levels(R));
    pr = R->poff, br = p->bytes[i];
    assert(pr <= br);
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
    if (!C || !C->tree) return SP_ERRPARAM;
    if (len == 0) return SP_OK;
    /* reserve before pad: both pads may run insertrt splits (levels+1
     * nodes each) on top of the phase budget (design detail 7) */
    r = spP_reserve(C->tree->S, &C->tree->S->nodes, 6 * spK_levels(C) + 7);
    if (r != SP_OK) return r;
    /* pad [bytes, C) as id 0 when the cursor sits virtual */
    spI_pad(C), off0 = sp_offset(C);
    /* seek builds R; advance is a no-op on an empty tree */
    sp_seek(&R, C->tree, off0 + len);
    if (sp_offset(&R) > C->tree->bytes) {
        /* pad [bytes, off0+len); arb(0,0) announces the pad */
        spI_pad(&R), spF_arb(&R, 0, 0);
        sp_locate(C, off0);
    } else if (sp_offset(&R) >= C->tree->bytes)
        spK_locend(&R);
    fl = spD_splitpaths(C, &R);
    if (fl > spK_levels(C)) return spF_filterleaf(C, len, id), SP_OK;
    spF_fillrange(C, &R, fl, id);
    return SP_OK;
}

SP_NS_END

#endif /* SP_IMPLEMENTATION */
