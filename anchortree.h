#ifndef anchortree_h
#define anchortree_h

#ifndef AT_NS_BEGIN
# ifdef __cplusplus
#   define AT_NS_BEGIN extern "C" {
#   define AT_NS_END   }
# else
#   define AT_NS_BEGIN
#   define AT_NS_END
# endif
#endif /* AT_NS_BEGIN */

#ifndef AT_STATIC
# if __GNUC__
#   define AT_STATIC static __attribute((unused))
# else
#   define AT_STATIC static
# endif
#endif /* AT_STATIC */

#ifdef AT_STATIC_API
# ifndef AT_IMPLEMENTATION
#   define AT_IMPLEMENTATION
# endif
# define AT_API AT_STATIC
#endif /* AT_STATIC_API */

#if !defined(AT_API) && defined(_WIN32)
# ifdef AT_IMPLEMENTATION
#   define AT_API __declspec(dllexport)
# else
#   define AT_API __declspec(dllimport)
# endif
#endif /* AT_API */

#ifndef AT_API
# define AT_API extern
#endif

#include <limits.h>
#include <stddef.h>

#define AT_OK       (0)         /* No error */
#define AT_ERRPARAM (-1)        /* Invalid parameter */
#define AT_ERRMEM   (-2)        /* Memory allocation failed */
#define AT_NONE     (~(at_Id)0) /* End-of-iteration sentinel */

#define AT_ITER_ORIGIN 0u /* origin not scanned in this iteration round */
#define AT_ITER_TREE   1u /* origin done, or iteration started after 0 */
#define AT_ITER_DONE   2u /* iteration exhausted */

typedef enum at_Gravity {
    AT_GROW,  /* [L,R] extends when insert before or after */
    AT_FIXED, /* [R,L] does not extend when insert in both side */
    AT_SHIFT, /* [R,R] shifts when insert before, extends after */
    AT_LOCK   /* [L,L] extends when insert before, not after */
} at_Gravity;

AT_NS_BEGIN

typedef struct at_State  at_State;
typedef struct at_Tree   at_Tree;
typedef struct at_Cursor at_Cursor;

typedef unsigned  at_Id;
typedef ptrdiff_t at_Delta;

typedef void *at_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

#define at_offset(C) ((C)->off + (C)->poff)

/* lifecycle */
AT_API at_State *at_open(at_Alloc *allocf, void *ud);
AT_API void      at_close(at_State *S);

AT_API at_Tree *at_newtree(at_State *S);
AT_API void     at_deltree(at_Tree *T);
AT_API void     at_reset(at_Tree *T);

/* cursor */
AT_API int   at_seek(at_Cursor *C, at_Tree *T, size_t pos);
AT_API int   at_advance(at_Cursor *C, at_Delta d);
AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen);

/* anchors */
AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out);
AT_API int at_del(at_Tree *T, at_Id id);
AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end);
AT_API int at_gravity(at_Tree *T, at_Id id);

/* splice */
AT_API int at_splice(at_Cursor *C, size_t del, size_t ins);

/* structure */

#ifndef AT_MAX_LEVEL
# define AT_MAX_LEVEL 16
#endif /* AT_MAX_LEVEL */

struct at_Cursor {
    at_Tree *tree;      /* tree under navigation */
    size_t   off;       /* bytes before the current edge gap */
    size_t   poff;      /* offset within the current gap */
    size_t   left_idx;  /* consumed count in the left side */
    size_t   right_idx; /* consumed count in the right side */
    unsigned iter;      /* iteration phase: AT_ITER_ORIGIN/TREE/DONE */

    struct at_Node **paths[AT_MAX_LEVEL]; /* root-to-leaf child slot pointers */
};

AT_NS_END

#endif /* anchortree_h */

/* ======================================================================== */
/*                           IMPLEMENTATION                                 */
/* ======================================================================== */

#if defined(AT_IMPLEMENTATION) && !defined(at_implemented)
#define at_implemented

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define AT_STATIC_ASSERT(cond)      AT_SA_0(cond, at_SA_, __LINE__)
#define AT_SA_0(cond, prefix, line) AT_SA_1(cond, prefix, line)
#define AT_SA_1(cond, prefix, line) typedef char prefix##line[(cond) ? 1 : -1]

#ifndef AT_FANOUT
# define AT_FANOUT 34
#endif /* AT_FANOUT */

AT_STATIC_ASSERT(AT_FANOUT >= 4);

#ifndef AT_PAGE_SIZE
# define AT_PAGE_SIZE 65536
#endif

AT_NS_BEGIN

#ifdef AT_POOL_STATS
# define atP_stat(stmt) stmt
#else
# define atP_stat(stmt) ((void)0)
#endif

typedef struct at_Pool {
    size_t obj_size;  /* size of each pooled object */
    void  *freed;     /* freelist head */
    void  *pages;     /* linked list of allocated pages */
    size_t freed_obj; /* number of objects in the freelist */
#ifdef AT_POOL_STATS
    size_t live_obj; /* live object count when pool stats are on */
#endif
} at_Pool;

typedef unsigned short at_Index;

typedef struct at_Node {
    struct at_Node *children[AT_FANOUT]; /* child nodes or leaf edges */
    size_t   bytes[AT_FANOUT]; /* subtree sums, or edge spans at leaves */
    at_Index child_count;      /* number of valid child slots */
    struct at_Node *parent;    /* parent node; NULL for the root */
    at_Index        idx;       /* child index in the parent */
} at_Node;

typedef struct at_Point {
    at_Id    id;         /* owning anchor id */
    unsigned ns : 31;    /* namespace filter value */
    unsigned is_end : 1; /* 1 = END endpoint, 0 = START endpoint */
} at_Point;

/* clang-format off */
typedef struct at_PointList { at_Point first, *tail; } at_PointList;
typedef struct at_Anchor { struct at_Edge *start, *end; } at_Anchor;
/* clang-format on */

typedef struct at_Edge {
    at_Node     *parent; /* leaf node holding this edge; NULL for origin */
    at_Index     idx;    /* slot index in parent */
    at_PointList left;   /* endpoints on the LEFT side */
    at_PointList right;  /* endpoints on the RIGHT side */
} at_Edge;

struct at_State {
    void     *alloc_ud;         /* allocator user data */
    at_Alloc *allocf;           /* memory allocator */
    at_Pool   nodes;            /* pool for at_Node objects */
    at_Pool   edges;            /* pool for at_Edge objects */
    at_Node   rt[AT_MAX_LEVEL]; /* scratch nodes for range stitch */
};

struct at_Tree {
    at_Node    root;   /* embedded root node */
    at_State  *S;      /* owning state */
    size_t     bytes;  /* position of the last tree edge; 0 if none */
    int        levels; /* tree height, 0 = leaf-only root */
    at_Edge    origin; /* position-0 edge, never stored in the tree */
    at_Anchor *ids;    /* dense anchor pool; id 0 is reserved */
    size_t     idcap;  /* allocated capacity of ids */
    at_Id      idfree; /* freelist head for anchor ids */
};

#define atN_cc(n)            ((int)(n)->child_count)
#define atN_setcc(n, v)      ((n)->child_count = (at_Index)(v))
#define atN_edge(n, i)       ((at_Edge *)(n)->children[i])
#define atN_setedge(n, i, e) ((n)->children[i] = (at_Node *)(e))
#define atN_min()            (AT_FANOUT / 2)
#define atN_full(n)          (atN_cc(n) >= AT_FANOUT)

#define atK_levels(C)    ((C)->tree->levels)
#define atK_bytes(C)     ((C)->tree->bytes)
#define atK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define atK_idx(C, p, l) ((int)((C)->paths[(l)] - (p)->children))

/* mempool */

static void atP_init(at_Pool *p, size_t obj_size) {
    memset(p, 0, sizeof(at_Pool)), p->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < AT_PAGE_SIZE / 2);
}

static void atP_destroy(at_State *S, at_Pool *p) {
    void *next, *page = p->pages;
    for (; page; page = next) {
        next = *(void **)((char *)page + AT_PAGE_SIZE - sizeof(void *));
        S->allocf(S->alloc_ud, page, AT_PAGE_SIZE, 0);
    }
    atP_init(p, p->obj_size);
}

static void *atP_ralloc(at_Pool *p) {
    void *obj = p->freed;
    assert(obj), atP_stat(p->live_obj += 1), p->freed_obj -= 1;
    return (p->freed = *(void **)obj), (void *)obj;
}

static void atP_free(at_Pool *p, void *obj) {
    atP_stat(p->live_obj -= 1), p->freed_obj += 1;
    *(void **)obj = p->freed, p->freed = obj;
}

static void *atP_alloc(at_State *S, at_Pool *p) {
    size_t sz = p->obj_size;
    char  *page, *end;
    if (p->freed_obj) return atP_ralloc(p);
    page = (char *)S->allocf(S->alloc_ud, NULL, 0, AT_PAGE_SIZE);
    if (page == NULL) return NULL;
    end = &page[AT_PAGE_SIZE - sizeof(void *)], *(void **)end = p->pages;
    p->pages = (void *)page, page += sz, end -= sz;
    while ((page += sz) <= end) *(void **)(page - sz) = page;
    *(void **)(page - sz) = p->freed, atP_stat(p->live_obj += 1);
    p->freed_obj = (end - (char *)p->pages) / sz;
    return (p->freed = (void *)((char *)p->pages + sz)), p->pages;
}

static int atP_reserve(at_State *S, at_Pool *p, size_t n) {
    size_t avail = p->freed_obj;
    void  *obj;
    if (avail >= n) return AT_OK;
    while (p->freed_obj = 0, (obj = atP_alloc(S, p)))
        if (atP_free(p, obj), (avail += p->freed_obj) >= n) break;
    return (p->freed_obj = avail) >= n ? AT_OK : AT_ERRMEM;
}

/* vector/tail storage */

/* clang-format off */
typedef struct atV_Header { unsigned len, cap; } atV_Header;
/* clang-format on */

#define atV_hdr(A) ((atV_Header *)(A) - 1)
#define atV_len(A) ((A) ? atV_hdr(A)->len : 0u)
#define atV_cap(A) ((A) ? atV_hdr(A)->cap : 0u)

#define atV_free(S, A) atV_resize_(S, (void **)&(A), 0u, sizeof(*(A)))

#define atV_push(S, A, V)                      \
    (atV_reserve(S, A, 1) != AT_OK ? AT_ERRMEM \
                                   : ((A)[atV_hdr(A)->len++] = (V), AT_OK))
#define atV_reserve(S, A, N) \
    atV_grow_(S, (void **)&(A), (unsigned)(N), sizeof(*(A)))

#define atV_sz(cap, sz) (sizeof(atV_Header) + (size_t)(cap) * (sz))

static int atV_resize_(at_State *S, void **pA, unsigned cap, size_t objsz) {
    atV_Header *hdr, *old = (*pA ? atV_hdr(*pA) : NULL);
    size_t      osz, nsz;
    if (old == NULL && cap == 0) return AT_OK;
    if (cap == 0) {
        S->allocf(S->alloc_ud, old, atV_sz(old->cap, objsz), 0);
        return *pA = NULL, AT_OK;
    }
    osz = old ? atV_sz(old->cap, objsz) : 0, nsz = atV_sz(cap, objsz);
    hdr = (atV_Header *)S->allocf(S->alloc_ud, old, osz, nsz);
    if (hdr == NULL) return AT_ERRMEM;
    if (old == NULL) hdr->len = 0;
    return hdr->cap = cap, *pA = (void *)(hdr + 1), AT_OK;
}

static int atV_grow_(at_State *S, void **pA, unsigned need, size_t objsz) {
    atV_Header *hdr;
    unsigned    c = 0, e = need, nc = 4;
    hdr = (*pA ? atV_hdr(*pA) : NULL);
    if (hdr) c = hdr->cap, e += hdr->len;
    if (c < e) {
        while (nc < e) nc += nc >> 1;
        return atV_resize_(S, pA, nc, objsz);
    }
    return AT_OK;
}

/* object/edge helpers */

/* clang-format off */
static int atO_listcount(at_PointList *ls)
{ return (ls->first.id != 0) + (ls->tail ? (int)atV_len(ls->tail) : 0); }

static int atO_empty(at_Edge *e)
{ return atO_listcount(&e->left) == 0 && atO_listcount(&e->right) == 0; }
/* clang-format on */

static int atO_listfind(at_PointList *ls, at_Id id, int end) {
    unsigned i;
    if (ls->first.id == id && ls->first.is_end == end) return 1;
    if (ls->tail != NULL)
        for (i = 0; i < atV_len(ls->tail); ++i)
            if (ls->tail[i].id == id && ls->tail[i].is_end == end) return 1;
    return 0;
}

static int atO_sideof(at_Edge *e, at_Id id, int end) {
    if (atO_listfind(&e->left, id, end)) return 0;
    if (atO_listfind(&e->right, id, end)) return 1;
    return -1;
}

static int atO_push(at_Tree *T, at_PointList *ls, at_Id id, int ns, int end) {
    at_Point p;
    p.id = id, p.ns = ns, p.is_end = end;
    if (ls->first.id == 0) return ls->first = p, AT_OK;
    return atV_push(T->S, ls->tail, p);
}

static int atO_reserve(at_Tree *T, at_PointList *ls, unsigned add) {
    if (add == 0) return AT_OK;
    if (ls->first.id != 0) return atV_reserve(T->S, ls->tail, add);
    if (add > 1u) return atV_reserve(T->S, ls->tail, add - 1u);
    return AT_OK;
}

/* clang-format off */
static void atO_discardemptytail(at_Tree *T, at_PointList *ls)
{ if (ls->tail != NULL && atV_len(ls->tail) == 0) atV_free(T->S, ls->tail); }
/* clang-format on */

static void atO_remove(at_Tree *T, at_PointList *ls, at_Id id, int end) {
    unsigned i, n;
    if (ls->first.id == id && ls->first.is_end == end) {
        n = atV_len(ls->tail);
        if (n == 0) {
            ls->first.id = 0, ls->first.is_end = 0;
            return;
        }
        ls->first = ls->tail[n - 1];
        atV_hdr(ls->tail)->len -= 1;
        if (n - 1 == 0) atV_free(T->S, ls->tail);
        return;
    }
    if (ls->tail == NULL) return;
    n = atV_len(ls->tail);
    for (i = 0; i < n; ++i)
        if (ls->tail[i].id == id && ls->tail[i].is_end == end) break;
    if (i == n) return;
    if (i != n - 1) ls->tail[i] = ls->tail[n - 1];
    atV_hdr(ls->tail)->len -= 1;
    if (n - 1 == 0) atV_free(T->S, ls->tail);
}

static void atO_destroyedge(at_Tree *T, at_Edge *e) {
    atV_free(T->S, e->left.tail), atV_free(T->S, e->right.tail);
    e->left.first.id = e->right.first.id = 0;
}

static int atO_findedge(at_Tree *T, size_t pos, at_Edge **out) {
    at_Node *n = &T->root;
    size_t   rem = pos;
    int      i, cc, level = T->levels;
    if (T->bytes == 0 || pos > T->bytes || pos == 0) return 0;
    for (;;) {
        cc = atN_cc(n);
        for (i = 0; i < cc && rem > n->bytes[i]; ++i) rem -= n->bytes[i];
        if (i >= cc || (level == 0 && rem != n->bytes[i])) return 0;
        if (level == 0) return *out = atN_edge(n, i), 1;
        n = n->children[i], level -= 1;
    }
}

static void atO_clear(at_Tree *T) {
    if (T->ids == NULL) return;
    T->S->allocf(T->S->alloc_ud, T->ids, T->idcap * sizeof(at_Anchor), 0);
    T->ids = NULL, T->idcap = 0, T->idfree = 0;
}

static int atO_growids(at_Tree *T) {
    at_Anchor *ids;
    size_t     oldcap = T->idcap, newcap = oldcap ? oldcap * 2 : 8;
    size_t     sz, i, from = oldcap ? oldcap : 1;
    if (newcap <= oldcap) return AT_ERRMEM;
    sz = newcap * sizeof(at_Anchor);
    ids = (at_Anchor *)T->S->allocf(T->S->alloc_ud, NULL, 0, sz);
    if (ids == NULL) return AT_ERRMEM;
    memset(ids, 0, newcap * sizeof(at_Anchor));
    if (oldcap != 0) memcpy(ids, T->ids, oldcap * sizeof(at_Anchor));
    for (i = newcap; i > from; --i) {
        at_Anchor *a = &ids[i - 1];
        a->start = (at_Edge *)(size_t)T->idfree;
        T->idfree = (at_Id)(i - 1);
    }
    if (oldcap != 0)
        T->S->allocf(T->S->alloc_ud, T->ids, oldcap * sizeof(at_Anchor), 0);
    return T->ids = ids, T->idcap = newcap, AT_OK;
}

static int atO_idalloc(at_Tree *T, at_Id *out) {
    at_Anchor *a;
    int        r;
    if (T->idfree == 0 && (r = atO_growids(T)) != AT_OK) return r;
    a = &T->ids[T->idfree];
    *out = T->idfree;
    T->idfree = (at_Id)(size_t)a->start;
    return a->start = NULL, a->end = NULL, AT_OK;
}

static void atO_idfree(at_Tree *T, at_Id id) {
    at_Anchor *a = &T->ids[id];
    a->start = (at_Edge *)(size_t)T->idfree;
    a->end = NULL;
    T->idfree = id;
}

static at_Anchor *atO_anchor(at_Tree *T, at_Id id) {
    at_Anchor *a;
    if (T == NULL || id == 0 || T->ids == NULL || id >= T->idcap) return NULL;
    return a = &T->ids[id], a->end == NULL ? NULL : a;
}

static size_t atO_edgepos(const at_Tree *T, const at_Edge *e) {
    const at_Node *n;
    size_t         pos = 0;
    int            i;
    if (e == &T->origin) return 0;
    n = e->parent;
    for (i = 0; i <= (int)e->idx; ++i) pos += n->bytes[i];
    while (n->parent != NULL) {
        const at_Node *p = n->parent;
        for (i = 0; i < (int)n->idx; ++i) pos += p->bytes[i];
        n = p;
    }
    return pos;
}

static int atO_gravsides(int g, int *sl, int *el) {
    if (g == AT_GROW) return *sl = 0, *el = 1, AT_OK;
    if (g == AT_FIXED) return *sl = 1, *el = 0, AT_OK;
    if (g == AT_SHIFT) return *sl = 1, *el = 1, AT_OK;
    if (g == AT_LOCK) return *sl = 0, *el = 0, AT_OK;
    return AT_ERRPARAM;
}

static int atO_reservenewanchor(at_Tree *T, size_t a, size_t b, int x, int y) {
    at_Edge *e = &T->origin;
    unsigned add = a == b && x == y ? 2u : 1u;
    int      r = AT_OK;
    if (a == 0 || atO_findedge(T, a, &e))
        r = atO_reserve(T, x ? &e->right : &e->left, add);
    if (r == AT_OK && (a != b || x != y)) {
        e = &T->origin;
        if (b == 0 || atO_findedge(T, b, &e))
            r = atO_reserve(T, y ? &e->right : &e->left, 1);
    }
    return r;
}

static int atO_nextstart(at_Edge *e, int ns, size_t *lp, size_t *rp, at_Id *o) {
    at_PointList *ls;
    at_Point      p;
    size_t       *idx, n;
    int           side;
    for (side = 0; side < 2; ++side) {
        ls = side ? &e->right : &e->left, idx = side ? rp : lp;
        n = (size_t)atO_listcount(ls);
        for (; *idx < n; ++*idx) {
            p = *idx == 0 ? ls->first : ls->tail[*idx - 1];
            if (p.is_end == 0 && (ns == 0 || p.ns == (unsigned)ns))
                return *o = p.id, ++*idx, 1;
        }
    }
    return 0;
}

static at_Id atO_none(size_t *plen) {
    if (plen != NULL) *plen = 0;
    return AT_NONE;
}

static at_Id atO_done(at_Cursor *C, size_t *plen) {
    C->iter = AT_ITER_DONE;
    return atO_none(plen);
}

static size_t atO_rangelen(at_Tree *T, at_Id id) {
    size_t start, end;
    int    r;
    r = at_range(T, id, &start, &end), assert(r == AT_OK), (void)r;
    return start <= end ? end - start : 0;
}

static at_Id atO_found(at_Tree *T, at_Id id, size_t *plen) {
    if (plen != NULL) *plen = atO_rangelen(T, id);
    return id;
}

static int atO_nextorigin(at_Cursor *C, int ns, at_Id *id) {
    if (C->iter != AT_ITER_ORIGIN) return 0;
    assert(at_offset(C) == 0);
    if (atO_nextstart(&C->tree->origin, ns, &C->left_idx, &C->right_idx, id))
        return 1;
    C->iter = AT_ITER_TREE;
    C->left_idx = C->right_idx = 0;
    return 0;
}

/* node operations */

static size_t atN_sumbytes(const at_Node *n, int i, int end) {
    size_t s = 0;
    for (; i < end; ++i) s += n->bytes[i];
    return s;
}

static void atN_copy(at_Node *d, int di, const at_Node *s, int si, int n) {
    assert(di + n <= AT_FANOUT && si + n <= AT_FANOUT);
    memcpy(&d->children[di], &s->children[si], n * sizeof(at_Node *));
    memcpy(&d->bytes[di], &s->bytes[si], n * sizeof(size_t));
}

static void atN_move(at_Node *n, int to, int from, int count) {
    assert(to + count <= AT_FANOUT && from + count <= AT_FANOUT);
    memmove(&n->children[to], &n->children[from], count * sizeof(at_Node *));
    memmove(&n->bytes[to], &n->bytes[from], count * sizeof(size_t));
}

static void atN_fixlinks(at_Node *n, int i, int k, int leaf) {
    at_Node *c;
    at_Edge *edge;
    int      j;
    for (j = i; j < i + k; ++j) {
        if (leaf) {
            edge = atN_edge(n, j);
            edge->parent = n, edge->idx = (at_Index)j;
        } else {
            c = n->children[j];
            c->parent = n, c->idx = (at_Index)j;
        }
    }
}

static void atN_makespace(at_Node *n, int i, int k, int leaf) {
    int cc = atN_cc(n);
    assert(cc + k <= AT_FANOUT && i <= cc);
    atN_move(n, i + k, i, cc - i);
    atN_setcc(n, cc + k);
    atN_fixlinks(n, i + k, atN_cc(n) - (i + k), leaf);
}

AT_STATIC void atN_removeslot(at_Node *n, int i, int k, int leaf) {
    int cc = atN_cc(n);
    assert(i >= 0 && i + k <= cc);
    atN_move(n, i, i + k, cc - i - k);
    atN_setcc(n, cc - k);
    atN_fixlinks(n, i, atN_cc(n) - i, leaf);
}

/* clang-format off */
static void atN_setleaf(at_Node *n, int i, at_Edge *e, size_t b)
{ atN_setedge(n, i, e), n->bytes[i] = b; e->parent = n, e->idx = (at_Index)i; }

static void atN_set(at_Node *n, int i, at_Node *c, size_t b)
{ n->children[i] = c, n->bytes[i] = b; c->parent = n, c->idx = (at_Index)i; }
/* clang-format on */

static void atN_purge(at_Tree *T, at_Node *p, int k, int s, int e) {
    int i;
    assert(s <= e && e <= atN_cc(p));
    if (k == 0) {
        for (i = s; i < e; ++i) {
            at_Edge *edge = atN_edge(p, i);
            atO_destroyedge(T, edge);
            atP_free(&T->S->edges, edge);
        }
        return;
    }
    for (i = s; i < e; ++i) {
        at_Node *c = p->children[i];
        atN_purge(T, c, k - 1, 0, atN_cc(c));
        atP_free(&T->S->nodes, c);
    }
}

static void atN_remove(at_Tree *T, at_Node *p, int k, int s, int e) {
    int leaf = k == 0;
    assert(s <= e && e <= atN_cc(p));
    atN_purge(T, p, k, s, e);
    atN_move(p, s, e, atN_cc(p) - e);
    atN_setcc(p, atN_cc(p) - (e - s));
    atN_fixlinks(p, s, atN_cc(p) - s, leaf);
}

static size_t atN_locate(at_Tree *T, size_t pos, at_Node **anc, int *idx) {
    size_t rem = pos;
    int    d, i, cc;
    anc[0] = &T->root;
    for (d = 0; d < T->levels; ++d) {
        cc = atN_cc(anc[d]);
        for (i = 0; i < cc && rem > anc[d]->bytes[i]; ++i)
            rem -= anc[d]->bytes[i];
        assert(i < cc), idx[d] = i;
        anc[d + 1] = anc[d]->children[i];
    }
    cc = atN_cc(anc[T->levels]);
    for (i = 0; i < cc && rem > anc[T->levels]->bytes[i]; ++i)
        rem -= anc[T->levels]->bytes[i];
    return assert(i < cc), idx[T->levels] = i, rem;
}

/* lifecycle */

static void *atS_defallocf(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    (void)ud, (void)osize;
    if (nsize == 0) return free(p), (void *)0;
    return (np = realloc(p, nsize)) ? np : (abort(), (void *)0);
}

AT_API at_State *at_open(at_Alloc *allocf, void *ud) {
    at_State *S;
    if (allocf == NULL) allocf = &atS_defallocf;
    if (!(S = (at_State *)allocf(ud, NULL, 0, sizeof(at_State)))) return NULL;
    memset(S, 0, sizeof(at_State)), S->alloc_ud = ud, S->allocf = allocf;
    atP_init(&S->nodes, sizeof(at_Node)), atP_init(&S->edges, sizeof(at_Edge));
    return S;
}

AT_API void at_close(at_State *S) {
    if (S == NULL) return;
    atP_destroy(S, &S->nodes), atP_destroy(S, &S->edges);
    S->allocf(S->alloc_ud, S, sizeof(at_State), 0);
}

AT_API at_Tree *at_newtree(at_State *S) {
    at_Tree *T;
    if (S == NULL) return NULL;
    T = (at_Tree *)S->allocf(S->alloc_ud, NULL, 0, sizeof(at_Tree));
    if (T == NULL) return NULL;
    return memset(T, 0, sizeof(at_Tree)), T->S = S, T;
}

AT_API void at_deltree(at_Tree *T) {
    if (T == NULL) return;
    atN_purge(T, &T->root, T->levels, 0, atN_cc(&T->root));
    atO_destroyedge(T, &T->origin);
    atO_clear(T);
    T->S->allocf(T->S->alloc_ud, T, sizeof(at_Tree), 0);
}

AT_API void at_reset(at_Tree *T) {
    if (T == NULL) return;
    atN_purge(T, &T->root, T->levels, 0, atN_cc(&T->root));
    atO_destroyedge(T, &T->origin);
    atO_clear(T);
    memset(&T->root, 0, sizeof(at_Node));
    memset(&T->origin, 0, sizeof(at_Edge));
    T->bytes = 0, T->levels = 0;
}

/* metric/path helpers */

static void atM_addpath(at_Node **a, int *ix, int dep, int stop, at_Delta d) {
    int k;
    for (k = dep; k > stop && k >= 1; --k) {
        size_t *b = &a[k - 1]->bytes[ix[k - 1]];
        *b += d;
    }
}

static void atM_up(at_Cursor *C, int l, at_Delta db) {
    int i;
    for (; l >= 0; --l) {
        at_Node *p = atK_parent(C, l);
        i = atK_idx(C, p, l);
        p->bytes[i] += db;
    }
    C->tree->bytes += db;
}

/* cursor navigation */

static void atK_locend(at_Cursor *C) {
    at_Node *n = &C->tree->root;
    int      d, i;
    C->off = C->poff = 0;
    for (d = 0; d < C->tree->levels; ++d) {
        i = atN_cc(n) - 1;
        C->off += atN_sumbytes(n, 0, atN_cc(n)) - n->bytes[i];
        C->paths[d] = &n->children[i];
        n = n->children[i];
    }
    i = atN_cc(n) - 1;
    C->off += atN_sumbytes(n, 0, atN_cc(n)) - n->bytes[i];
    C->paths[C->tree->levels] = &n->children[i];
    C->poff = n->bytes[i];
}

static void atK_find(at_Cursor *C, size_t pos) {
    at_Node *n = &C->tree->root;
    size_t   rem = pos;
    int      d, i, cc;
    C->off = C->poff = 0;
    for (d = 0; d < C->tree->levels; ++d) {
        cc = atN_cc(n);
        for (i = 0; i < cc && rem > n->bytes[i]; ++i)
            rem -= n->bytes[i], C->off += n->bytes[i];
        assert(i < cc), C->paths[d] = &n->children[i];
        n = n->children[i];
    }
    cc = atN_cc(n);
    for (i = 0; i < cc && rem > n->bytes[i]; ++i)
        rem -= n->bytes[i], C->off += n->bytes[i];
    assert(i < cc), C->paths[C->tree->levels] = &n->children[i];
    C->poff = rem;
}

static int atK_nextedge(at_Cursor *C) {
    at_Node *p, *q;
    int      l, i, cc, L = C->tree->levels;
    p = L ? *C->paths[L - 1] : &C->tree->root;
    i = (int)(C->paths[L] - p->children);
    cc = atN_cc(p);
    C->off += p->bytes[i];
    if (i + 1 < cc)
        return C->paths[L] = &p->children[i + 1], C->poff = p->bytes[i + 1], 1;
    for (l = L - 1; l >= 0; --l) {
        q = l ? *C->paths[l - 1] : &C->tree->root;
        i = (int)(C->paths[l] - q->children);
        if (i + 1 < atN_cc(q)) {
            C->paths[l] = &q->children[i + 1];
            while (l < L) {
                p = *C->paths[l];
                ++l;
                C->paths[l] = &p->children[0];
            }
            return C->poff = p->bytes[0], 1;
        }
    }
    return 0;
}

AT_API int at_seek(at_Cursor *C, at_Tree *T, size_t pos) {
    if (C == NULL || T == NULL) return AT_ERRPARAM;
    memset(C, 0, sizeof(at_Cursor)), C->tree = T;
    C->iter = pos == 0 ? AT_ITER_ORIGIN : AT_ITER_TREE;
    if (T->bytes == 0)
        return C->paths[0] = T->root.children, C->poff = pos, AT_OK;
    if (pos > T->bytes) {
        size_t vir = pos - T->bytes;
        return atK_locend(C), C->poff += vir, AT_OK;
    }
    return atK_find(C, pos), AT_OK;
}

AT_API int at_advance(at_Cursor *C, at_Delta d) {
    size_t pos;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    pos = C->off + C->poff;
    if (d >= 0)
        pos += (size_t)d;
    else {
        size_t back = (size_t)(-d);
        if (back > pos) back = pos;
        pos -= back;
    }
    if (d != 0) {
        C->left_idx = C->right_idx = 0;
        C->iter = pos == 0 ? AT_ITER_ORIGIN : AT_ITER_TREE;
    }
    if (C->tree->bytes == 0) {
        C->off = 0, C->poff = pos, C->paths[0] = C->tree->root.children;
        return AT_OK;
    }
    if (pos > C->tree->bytes) {
        size_t vir = pos - C->tree->bytes;
        return atK_locend(C), C->poff += vir, AT_OK;
    }
    return atK_find(C, pos), AT_OK;
}

AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen) {
    at_Tree *T;
    at_Edge *e;
    at_Node *p;
    at_Id    id;
    size_t   b;
    int      i, L;
    if (C == NULL || C->tree == NULL) return atO_none(plen);
    if (ns < 0) return atO_none(plen);
    T = C->tree;
    if (C->iter == AT_ITER_DONE) return atO_none(plen);
    if (atO_nextorigin(C, ns, &id)) return atO_found(T, id, plen);
    if (T->bytes == 0) return atO_done(C, plen);
    for (;;) {
        L = T->levels;
        p = L ? *C->paths[L - 1] : &T->root;
        i = (int)(C->paths[L] - p->children);
        b = p->bytes[i];
        if (C->poff > b) return atO_done(C, plen);
        e = atN_edge(p, i);
        if (atO_nextstart(e, ns, &C->left_idx, &C->right_idx, &id))
            return atO_found(T, id, plen);
        if (!atK_nextedge(C)) return atO_done(C, plen);
        C->left_idx = C->right_idx = 0;
    }
}

/* remove/stitch */

static void atD_trimright(at_Cursor *L) {
    at_Node *p;
    int      l = atK_levels(L), i = atK_idx(L, p = atK_parent(L, l), l);
    size_t   cut = p->bytes[i] - L->poff;
    p->bytes[i] = L->poff;
    atM_up(L, l - 1, -(at_Delta)cut);
}

static void atD_trimleft(at_Cursor *R) {
    at_Node *p;
    int      l = atK_levels(R), i = atK_idx(R, p = atK_parent(R, l), l);
    size_t   cut = R->poff;
    p->bytes[i] -= cut;
    R->poff = 0;
    atM_up(R, l - 1, -(at_Delta)cut);
}

static void atD_cutrange(at_Cursor *L, at_Cursor *R, at_Node *rt, int fl) {
    int      kl, k, i, cc, l = atK_levels(L);
    at_Delta db = 0;
    at_Node *p;
    for (kl = l; kl > fl; --kl) {
        p = atK_parent(L, kl), i = atK_idx(L, p, kl), cc = atN_cc(p);
        p->bytes[i] -= db, db += (at_Delta)atN_sumbytes(p, i + 1, cc);
        k = l - kl, atN_remove(L->tree, p, k, i + 1, cc);
        i = atK_idx(R, p = atK_parent(R, kl), kl), cc = atN_cc(p);
        i += (k || p->bytes[i] == 0);
        atN_copy(&rt[k], 0, p, i, atN_setcc(&rt[k], cc - i));
        atN_purge(L->tree, p, k, 0, i), atN_setcc(p, 0);
    }
    p = atK_parent(R, fl), i = atK_idx(R, p, fl), cc = atN_cc(p);
    k = l - fl, i += (k || p->bytes[i] == 0);
    atN_copy(&rt[k], 0, p, i, atN_setcc(&rt[k], cc - i));
    atN_setcc(p, i), i = atK_idx(L, p, fl);
    p->bytes[i] -= db, db += (at_Delta)atN_sumbytes(p, i + 1, cc);
    atM_up(L, fl - 1, -db), atN_remove(L->tree, p, k, i + 1, atN_cc(p));
}

static void atD_mergeleaf(at_Cursor *C, at_Node *rt) {
    at_Node *p = atK_parent(C, atK_levels(C));
    at_Edge *edge;
    int      cc = atN_cc(p), l = atK_levels(C);
    size_t   bc = (assert(cc > 0), p->bytes[cc - 1]);
    edge = atN_edge(p, cc - 1);
    if (!atO_empty(edge)) {
        C->off += C->poff, C->poff = 0;
        C->paths[l] = &p->children[cc];
        return;
    }
    rt->bytes[0] += bc;
    atO_destroyedge(C->tree, edge);
    atP_free(&C->tree->S->edges, edge);
    atN_setcc(p, cc - 1);
    atM_up(C, l - 1, -(at_Delta)bc);
    if (atK_idx(C, p, l) == cc) C->off -= bc, C->poff = bc;
    C->paths[l] = &p->children[cc - 1];
}

static int atD_balancenode(at_Node **ns, int left, at_Delta *ds, int leaf) {
    int d, l = atN_cc(ns[0]), r = atN_cc(ns[1]);
    d = l - ((l + r + (left != 0)) >> 1);
    if (assert(d != 0), d < 0) {
        atN_copy(ns[0], l, ns[1], 0, -d);
        atN_move(ns[1], 0, -d, r + d);
        atN_fixlinks(ns[0], l, -d, leaf);
        atN_fixlinks(ns[1], 0, r + d, leaf);
        *ds = -(at_Delta)atN_sumbytes(ns[0], l, l - d);
    } else {
        atN_move(ns[1], d, 0, r);
        atN_copy(ns[1], 0, ns[0], l - d, d);
        atN_fixlinks(ns[1], 0, r + d, leaf);
        *ds = (at_Delta)atN_sumbytes(ns[1], 0, d);
    }
    return atN_setcc(ns[0], l - d), atN_setcc(ns[1], r + d), d;
}

static int atD_foldnode(at_Cursor *C, int lfirst, int l) {
    at_Node  *p = atK_parent(C, l), ***cp = &C->paths[l];
    int       leaf = l == atK_levels(C) - 1;
    int       cL, cR, dn, r = 0, i = atK_idx(C, p, l);
    at_Node **ns = &p->children[i];
    at_Delta  ds;
    if (assert(atN_cc(p) > 1), atN_cc(ns[0]) > atN_min()) return 0;
    if ((i && lfirst) || i == atN_cc(p) - 1) ns -= 1, i -= 1, r = 1;
    cL = atN_cc(ns[0]), cR = atN_cc(ns[1]);
    if (cL + cR <= AT_FANOUT) {
        atN_copy(ns[0], cL, ns[1], 0, cR);
        atN_fixlinks(ns[0], cL, cR, leaf);
        atN_setcc(ns[0], cL + cR);
        atN_setcc(ns[1], 0);
        p->bytes[i] += p->bytes[i + 1];
        if (r) cp[1] = &ns[0]->children[cp[1] - ns[1]->children + cL], --cp[0];
        return atN_remove(C->tree, p, atK_levels(C) - l, i + 1, i + 2), 1;
    }
    dn = atD_balancenode(ns, !r, &ds, leaf);
    assert(dn != 0 && (dn < 0) != r);
    if (r) cp[1] += dn;
    return p->bytes[i] -= ds, p->bytes[i + 1] += ds, 0;
}

static void atD_rebalance(at_Cursor *C, int l) {
    at_Node *p;
    assert(l == 0 || l < atK_levels(C));
    for (; l > 0; --l) {
        p = atK_parent(C, l);
        if (atN_cc(p->children[atK_idx(C, p, l)]) >= atN_min()) return;
        if (assert(atN_cc(p) > 1), !atD_foldnode(C, 0, l)) return;
    }
    if (atK_levels(C) > 0) { /* fold the root children */
        int i = atK_idx(C, p = &C->tree->root, 0);
        if (atN_cc(p->children[i]) < atN_min() && atN_cc(p) >= 2)
            atD_foldnode(C, 0, 0);
    }
    while (atK_levels(C) && atN_cc(&C->tree->root) == 1) {
        int i = atK_idx(C, p = atK_parent(C, 1), 1);
        int cc = atN_cc(p), nl = atK_levels(C) - 1;
        C->tree->root = *p;
        C->tree->root.parent = NULL;
        atP_free(&C->tree->S->nodes, p);
        C->tree->levels = nl;
        C->paths[0] += i;
        memmove(C->paths + 1, C->paths + 2, atK_levels(C) * sizeof(at_Node **));
        atN_fixlinks(&C->tree->root, 0, cc, nl == 0);
    }
}

static void atD_backwardnode(at_Cursor *C, int d, int l) {
    at_Node *p;
    int      dl, i = atK_idx(C, p = atK_parent(C, l), l);
    if (d > i) {
        d -= i + 1, dl = l;
        while (--dl >= 0 && atK_idx(C, atK_parent(C, dl), dl) == 0) continue;
        assert(dl >= 0), C->paths[dl] -= 1;
        while (++dl <= l)
            p = atK_parent(C, dl), C->paths[dl] = &p->children[atN_cc(p) - 1];
    }
    C->paths[l] -= d;
}

static int atD_makechain(at_Cursor *C, int from, int to) {
    at_Node *p, *nn = NULL, ***cp = C->paths + to;
    int      l, r = 0;
    if (assert(from < to), from < 0) {
        nn = (at_Node *)atP_ralloc(&C->tree->S->nodes);
        p = &C->tree->root, *nn = *p;
        p->bytes[0] = atK_bytes(C);
        p->children[0] = nn, atN_setcc(p, 1);
        nn->parent = p, nn->idx = 0;
        atN_fixlinks(nn, 0, atN_cc(nn), C->tree->levels == 0);
        memmove(cp + 2, cp + 1, (atK_levels(C) - to) * sizeof(at_Node **));
        C->tree->levels += 1, assert(C->tree->levels < AT_MAX_LEVEL);
        from = 0, to += 1, cp += 1, r = 1;
    }
    for (l = from; l < to; ++l) {
        nn = (at_Node *)atP_ralloc(&C->tree->S->nodes);
        memset(nn, 0, sizeof(at_Node));
        p = atK_parent(C, l), nn->parent = p, nn->idx = (at_Index)atN_cc(p);
        p->bytes[atN_cc(p)] = 0;
        C->paths[l] = &p->children[atN_cc(p)];
        p->children[atN_cc(p)] = nn, p->child_count += 1;
    }
    return (*cp = &nn->children[0]), r;
}

static int atD_findroom(at_Cursor *C, at_Node *rt, int l) {
    int      fl, c, i = 0;
    at_Node *p = NULL;
    for (fl = l - 1; fl >= 0; --fl) {
        i = atK_idx(C, p = atK_parent(C, fl), fl);
        if (i < AT_FANOUT - 1) break;
    }
    if (fl >= 0 && (c = atN_cc(p) - i - 1) > 0) {
        int      k = atK_levels(C) - fl;
        at_Delta db = (at_Delta)atN_sumbytes(p, i + 1, atN_cc(p));
        assert(atN_cc(&rt[k]) == 0);
        atM_up(C, fl - 1, -db);
        atN_copy(&rt[k], 0, p, i + 1, c);
        atN_setcc(&rt[k], c), atN_setcc(p, i + 1);
    }
    return atD_makechain(C, fl, l);
}

static void atD_stitchnode(at_Cursor *L, at_Node *rt) {
    int      k, i, d = 0, l = atK_levels(L);
    at_Delta db = 0;
    at_Node *p, *r;
    for (k = 0; k <= atK_levels(L); ++k) {
        int m, fl, cc, kl = atK_levels(L) - k, rtcc = atN_cc(r = &rt[k]);
        atN_setcc(r, 0), i = atK_idx(L, p = atK_parent(L, kl), kl);
        if (i < atN_cc(p)) p->bytes[i] += db;
        m = rtcc < AT_FANOUT - atN_cc(p) ? rtcc : AT_FANOUT - atN_cc(p);
        if (m > 0) {
            cc = atN_cc(p);
            atN_copy(p, cc, r, 0, m), atN_setcc(p, cc + m);
            atN_fixlinks(p, cc, m, k == 0);
            db += (at_Delta)atN_sumbytes(r, 0, m);
        }
        if (!(m < rtcc || kl == 0)) continue;
        atM_up(L, kl - 1, db), db = 0;
        if (kl == 0 && atN_cc(&L->tree->root) == 1)
            atD_rebalance(L, 0), l -= (k - atK_levels(L));
        for (fl = kl; fl < l; ++fl) atD_foldnode(L, (fl == kl), fl);
        if (k) atD_backwardnode(L, d, l);
        if (!(m < rtcc)) continue;
        p = atK_parent(L, l = kl), d = k ? atN_cc(p) - atK_idx(L, p, l) : m;
        l += atD_findroom(L, rt, l), p = atK_parent(L, l);
        atN_copy(p, 0, r, m, atN_setcc(p, rtcc - m));
        atN_fixlinks(p, 0, rtcc - m, k == 0);
        db += (at_Delta)atN_sumbytes(r, m, rtcc);
    }
}

static void atD_stitchtail(at_Cursor *L) {
    int      cc, i, l = atK_levels(L);
    at_Node *p = atK_parent(L, l);
    i = atK_idx(L, p, l), cc = atN_cc(p);
    if (cc && i == cc)
        --L->paths[l], L->poff += p->bytes[cc - 1], L->off -= p->bytes[cc - 1];
}

static void atD_stitch(at_Cursor *L, at_Node *rt) {
    int      cc;
    at_Node *p = atK_parent(L, atK_levels(L));
    assert(L->tree->S->nodes.freed_obj >= (size_t)(atK_levels(L) + 2));
    if ((cc = atN_cc(p)) && p->bytes[cc - 1] == 0)
        atN_remove(L->tree, p, 0, cc - 1, cc), cc -= 1;
    if (cc && atN_cc(&rt[0])) atD_mergeleaf(L, rt);
    atD_stitchnode(L, rt), atD_rebalance(L, 0), atD_stitchtail(L);
}

static void atD_rmrange(at_Cursor *L, at_Cursor *R, int fl) {
    at_Node *rt = L->tree->S->rt;
    int      k;
    for (k = 0; k < AT_MAX_LEVEL; ++k) atN_setcc(&rt[k], 0);
    atD_trimright(L), atD_trimleft(R);
    atD_cutrange(L, R, rt, fl), atD_stitch(L, rt);
}

static void atD_rmleaf(at_Cursor *C) {
    at_Node *p = atK_parent(C, atK_levels(C));
    int      cc, i = atK_idx(C, p, atK_levels(C));
    size_t   bytes = p->bytes[i];
    atN_remove(C->tree, p, 0, i, i + 1);
    atM_up(C, atK_levels(C) - 1, -(at_Delta)bytes);
    cc = atN_cc(p);
    if (atK_levels(C) > 0 && cc < atN_min())
        atD_rebalance(C, atK_levels(C) - 1);
}

static int atD_diverlevel(at_Cursor *L, at_Cursor *R) {
    int l;
    for (l = 0; l < atK_levels(L) && L->paths[l] == R->paths[l]; ++l) continue;
    return l + (l == atK_levels(L) && L->paths[l] == R->paths[l]);
}

/* anchor-specific leaf/edge helpers */

static void atD_mergesuccessor(at_Node **a, int *ix, int L, int b, size_t d) {
    at_Node *n = a[b]->children[ix[b] + 1];
    int      k;
    a[b]->bytes[ix[b] + 1] += d;
    for (k = b + 1; k < L; ++k, n = n->children[0]) n->bytes[0] += d;
    n->bytes[0] += d;
    atM_addpath(a, ix, L, b, -(at_Delta)d);
}

static void atD_rebalancepath(at_Tree *T, at_Node **anc, int *idx, int L) {
    at_Cursor C;
    int       d;
    memset(&C, 0, sizeof(at_Cursor)), C.tree = T;
    for (d = 0; d <= L; ++d) {
        if (d == L && atN_cc(anc[L]) > 0 && idx[L] >= atN_cc(anc[L]))
            C.paths[d] = &anc[L]->children[atN_cc(anc[L]) - 1];
        else
            C.paths[d] = &anc[d]->children[idx[d]];
    }
    atD_rebalance(&C, L - 1);
}

AT_STATIC int atD_deleteedge(at_Tree *T, size_t pos) {
    at_Node *anc[AT_MAX_LEVEL + 1], *leaf;
    int      idx[AT_MAX_LEVEL + 1], i, L, d, a = -1;
    size_t   delta, rem = pos;
    at_Edge *e;
    if (T == NULL || pos == 0 || pos > T->bytes) return AT_ERRPARAM;
    rem = atN_locate(T, pos, anc, idx);
    L = T->levels, leaf = anc[L], i = idx[L];
    if (rem != leaf->bytes[i]) return AT_ERRPARAM;
    e = atN_edge(leaf, i), delta = leaf->bytes[i];
    if (i + 1 < atN_cc(leaf)) {
        leaf->bytes[i + 1] += delta;
    } else {
        for (a = -1, d = L - 1; d >= 0 && a < 0; --d)
            if (idx[d] < atN_cc(anc[d]) - 1) a = d;
        if (a >= 0)
            atD_mergesuccessor(anc, idx, L, a, delta);
        else {
            atM_addpath(anc, idx, L, -1, -(at_Delta)delta);
            T->bytes -= delta;
        }
    }
    atN_removeslot(leaf, i, 1, 1);
    atO_destroyedge(T, e);
    atP_free(&T->S->edges, e);
    assert(L == 0 || atN_cc(leaf) > 0);
    if (L > 0 && atN_cc(leaf) < atN_min()) atD_rebalancepath(T, anc, idx, L);
    return AT_OK;
}

static void atO_clean(at_Tree *T, at_Edge *e, size_t pos, at_Id id, int end) {
    int side = atO_sideof(e, id, end);
    int r;
    if (side >= 0) atO_remove(T, side ? &e->right : &e->left, id, end);
    if (e != &T->origin && atO_empty(e))
        r = atD_deleteedge(T, pos), assert(r == AT_OK), (void)r;
}

static int atO_reservenewedge(at_Tree *T, at_Edge *se, int side, at_Id id) {
    int r = AT_OK;
    if (side >= 0) r = atO_reserve(T, side ? &se->right : &se->left, 2);
    if (r != AT_OK) {
        if (se != &T->origin && atO_empty(se))
            atD_deleteedge(T, atO_edgepos(T, se));
        atO_idfree(T, id);
    }
    return r;
}

typedef struct at_Collect {
    at_State *S;     /* state supplying temporary vector allocations */
    size_t    lo;    /* lower bound of the collected edge range */
    size_t    hi;    /* upper bound of the collected edge range */
    at_Point *left;  /* collected LEFT endpoints */
    at_Point *right; /* collected RIGHT endpoints */
} at_Collect;

static int atD_collectside(at_State *S, at_Point **pbuf, at_PointList *ls) {
    unsigned i;
    int      res = AT_OK;
    if (ls->first.id != 0) res = atV_push(S, *pbuf, ls->first);
    for (i = 0; res == AT_OK && i < atV_len(ls->tail); ++i)
        res = atV_push(S, *pbuf, ls->tail[i]);
    return res;
}

static int atD_collectnode(at_Node *n, int lvl, size_t b, at_Collect *c) {
    int i, res;
    if (lvl == 0) {
        for (i = 0; i < atN_cc(n); ++i) {
            at_Edge *e;
            b += n->bytes[i];
            if (b <= c->lo || b > c->hi) continue;
            e = atN_edge(n, i);
            if ((res = atD_collectside(c->S, &c->left, &e->left)) != AT_OK)
                return res;
            if ((res = atD_collectside(c->S, &c->right, &e->right)) != AT_OK)
                return res;
        }
        return AT_OK;
    }
    for (i = 0; i < atN_cc(n); ++i) {
        if (b < c->hi && b + n->bytes[i] > c->lo) {
            res = atD_collectnode(n->children[i], lvl - 1, b, c);
            if (res != AT_OK) return res;
        }
        b += n->bytes[i];
    }
    return AT_OK;
}

static void atD_commitside(at_Tree *T, at_Edge *dst, int side, at_Point *buf) {
    at_PointList *ls = side ? &dst->right : &dst->left;
    unsigned      i, n = atV_len(buf);
    for (i = 0; i < n; ++i) {
        at_Point p = buf[i];
        int      r = atO_push(T, ls, p.id, p.ns, p.is_end);
        assert(r == AT_OK), (void)r;
        if (p.is_end)
            T->ids[p.id].end = dst;
        else
            T->ids[p.id].start = dst;
    }
}

static int atD_collectendpoints(at_Tree *T, size_t L, size_t R, at_Edge *le) {
    at_Collect c;
    int        r;
    c.S = T->S, c.lo = L, c.hi = R, c.left = NULL, c.right = NULL;
    if ((r = atD_collectnode(&T->root, T->levels, 0, &c)) != AT_OK)
        return atV_free(T->S, c.left), atV_free(T->S, c.right), r;
    if ((r = atO_reserve(T, &le->left, atV_len(c.left))) == AT_OK)
        r = atO_reserve(T, &le->right, atV_len(c.right));
    if (r != AT_OK) {
        atO_discardemptytail(T, &le->left);
        atO_discardemptytail(T, &le->right);
        return atV_free(T->S, c.left), atV_free(T->S, c.right), r;
    }
    atD_commitside(T, le, 0, c.left);
    atD_commitside(T, le, 1, c.right);
    return atV_free(T->S, c.left), atV_free(T->S, c.right), AT_OK;
}

/* insertion/splice */

static int atI_splitnode(at_State *S, at_Node *n, int level, at_Node **pr) {
    int      cc = atN_cc(n), left = (cc + 1) / 2, right = cc - left;
    at_Node *r = (at_Node *)atP_alloc(S, &S->nodes);
    if (r == NULL) return AT_ERRMEM;
    atN_setcc(r, right), r->parent = n->parent, r->idx = 0;
    atN_copy(r, 0, n, left, right);
    atN_fixlinks(r, 0, right, level == 0);
    atN_setcc(n, left);
    return *pr = r, AT_OK;
}

static int atI_splitroot(at_Tree *T) {
    at_Node *left, *right;
    int      cc = atN_cc(&T->root), level = T->levels;
    if (!(left = (at_Node *)atP_alloc(T->S, &T->S->nodes))) return AT_ERRMEM;
    atN_setcc(left, cc), left->parent = &T->root, left->idx = 0;
    atN_copy(left, 0, &T->root, 0, cc);
    atN_fixlinks(left, 0, cc, level == 0);
    if (atI_splitnode(T->S, left, level, &right) != AT_OK) {
        atN_fixlinks(&T->root, 0, cc, level == 0);
        return atP_free(&T->S->nodes, left), AT_ERRMEM;
    }
    atN_set(&T->root, 0, left, atN_sumbytes(left, 0, atN_cc(left)));
    atN_set(&T->root, 1, right, atN_sumbytes(right, 0, atN_cc(right)));
    return atN_setcc(&T->root, 2), T->levels = T->levels + 1, AT_OK;
}

static int atI_insertat(at_Tree *T, at_Node *n, int l, size_t rem, at_Edge *e) {
    at_Node *child, *right;
    int      i, r, cc = atN_cc(n);
    if (l == 0) {
        for (i = 0; i < cc && rem > n->bytes[i]; ++i) rem -= n->bytes[i];
        assert(i < cc && rem < n->bytes[i]), n->bytes[i] -= rem;
        return atN_makespace(n, i, 1, 1), atN_setleaf(n, i, e, rem), AT_OK;
    }
    for (i = 0; i < cc && rem > n->bytes[i]; ++i) rem -= n->bytes[i];
    assert(i < cc), child = n->children[i];
    if (atN_cc(child) == AT_FANOUT) {
        if ((r = atI_splitnode(T->S, child, l - 1, &right)) != AT_OK) return r;
        atN_makespace(n, i + 1, 1, 0);
        atN_set(n, i + 1, right, atN_sumbytes(right, 0, atN_cc(right)));
        n->bytes[i] = atN_sumbytes(child, 0, atN_cc(child));
        if (rem > n->bytes[i]) rem -= n->bytes[i], i += 1;
    }
    return atI_insertat(T, n->children[i], l - 1, rem, e);
}

static int atI_appendat(at_Tree *T, at_Node *n, int l, size_t d, at_Edge *e) {
    at_Node *child, *right;
    int      i, r, cc = atN_cc(n);
    if (l == 0)
        return atN_makespace(n, cc, 1, 1), atN_setleaf(n, cc, e, d), AT_OK;
    i = cc - 1, child = n->children[i];
    if (atN_cc(child) == AT_FANOUT) {
        if ((r = atI_splitnode(T->S, child, l - 1, &right)) != AT_OK) return r;
        atN_makespace(n, i + 1, 1, 0);
        atN_set(n, i + 1, right, atN_sumbytes(right, 0, atN_cc(right)));
        n->bytes[i] = atN_sumbytes(child, 0, atN_cc(child));
        child = right, i += 1;
    }
    if ((r = atI_appendat(T, child, l - 1, d, e)) == AT_OK) n->bytes[i] += d;
    return r;
}

AT_STATIC int atI_insertedge(at_Tree *T, size_t pos, at_Edge **out) {
    at_Edge *e, *found;
    int      r;
    if (T == NULL || out == NULL) return AT_ERRPARAM;
    if (pos == 0) return *out = &T->origin, AT_OK;
    if (atO_findedge(T, pos, &found)) return *out = found, AT_OK;
    if (atN_full(&T->root) && (r = atI_splitroot(T)) != AT_OK) return r;
    if (!(e = (at_Edge *)atP_alloc(T->S, &T->S->edges))) return AT_ERRMEM;
    memset(e, 0, sizeof(at_Edge));
    if (pos <= T->bytes)
        r = atI_insertat(T, &T->root, T->levels, pos, e);
    else
        r = atI_appendat(T, &T->root, T->levels, pos - T->bytes, e);
    if (r != AT_OK) return atP_free(&T->S->edges, e), r;
    if (pos > T->bytes) T->bytes = pos;
    return *out = e, AT_OK;
}

static int atO_edges(at_Tree *T, size_t s, size_t t, at_Edge **a, at_Edge **b) {
    int r;
    if ((r = atI_insertedge(T, s, a)) != AT_OK) return r;
    if (t == s) return *b = *a, AT_OK;
    if ((r = atI_insertedge(T, t, b)) == AT_OK) return AT_OK;
    if (*a != &T->origin && atO_empty(*a)) atD_deleteedge(T, s);
    return r;
}

static void atI_shift(at_Tree *T, size_t pos, at_Delta d) {
    at_Node *anc[AT_MAX_LEVEL + 1], *leaf;
    int      idx[AT_MAX_LEVEL + 1], L, i;
    size_t   rem = pos + 1;
    assert(pos < T->bytes && d != 0);
    rem = atN_locate(T, rem, anc, idx);
    L = T->levels, leaf = anc[L], i = idx[L];
    leaf->bytes[i] += d, T->bytes += d;
    atM_addpath(anc, idx, L, 0, d);
}

static void atI_moverights(at_Tree *T, at_Edge *src, at_Edge *dst) {
    int r;
    while (src->right.first.id != 0) {
        at_Point p = src->right.first;
        r = atO_push(T, &dst->right, p.id, p.ns, p.is_end);
        assert(r == AT_OK), (void)r;
        atO_remove(T, &src->right, p.id, p.is_end);
        if (p.is_end)
            T->ids[p.id].end = dst;
        else
            T->ids[p.id].start = dst;
    }
}

static int atI_insertempty(at_Tree *T, size_t pos, size_t ins) {
    at_Edge *ne;
    int      count, r;
    if (pos != 0 || T->origin.right.first.id == 0) return AT_OK;
    if ((r = atI_insertedge(T, ins, &ne)) != AT_OK) return r;
    count = atO_listcount(&T->origin.right);
    if ((r = atO_reserve(T, &ne->right, count)) != AT_OK) {
        r = atD_deleteedge(T, ins), assert(r == AT_OK), (void)r;
        return AT_ERRMEM;
    }
    return atI_moverights(T, &T->origin, ne), AT_OK;
}

static int atI_insertseam(at_Tree *T, at_Edge *e, size_t pos, size_t ins) {
    at_Edge *ne;
    int      count, r;
    /* Edge model: grow the seam gap first, then migrate RIGHT endpoints. */
    if (pos < T->bytes) atI_shift(T, pos, (at_Delta)ins);
    if (e->right.first.id == 0) return AT_OK;
    if ((r = atI_insertedge(T, pos + ins, &ne)) != AT_OK) {
        if (pos < T->bytes) atI_shift(T, pos, -(at_Delta)ins);
        return r;
    }
    count = atO_listcount(&e->right);
    if ((r = atO_reserve(T, &ne->right, count)) != AT_OK) {
        r = atD_deleteedge(T, pos + ins), assert(r == AT_OK), (void)r;
        if (pos < T->bytes) atI_shift(T, pos, -(at_Delta)ins);
        return AT_ERRMEM;
    }
    atI_moverights(T, e, ne);
    if (e != &T->origin && atO_empty(e))
        return r = atD_deleteedge(T, pos), assert(r == AT_OK), (void)r, AT_OK;
    return AT_OK;
}

static int atI_insert(at_Tree *T, size_t pos, size_t ins) {
    at_Edge *e;
    if (ins == 0) return AT_OK;
    if (T->bytes == 0) return atI_insertempty(T, pos, ins);
    if (pos > T->bytes) return AT_OK;
    if (pos != 0 && !atO_findedge(T, pos, &e)) {
        if (pos < T->bytes) atI_shift(T, pos, (at_Delta)ins);
        return AT_OK;
    }
    if (pos == 0) e = &T->origin;
    return atI_insertseam(T, e, pos, ins);
}

/* remove */

static int atD_prepwindow(at_Tree *T, size_t L, size_t R) {
    at_Edge *le = &T->origin;
    int      r, lnew = 0, rnew = 0;
    if (L != 0) {
        if ((r = atI_insertedge(T, L, &le)) != AT_OK) return r;
        lnew = atO_empty(le);
    }
    if (R < T->bytes) {
        at_Edge *re;
        int      found = atO_findedge(T, R, &re);
        if (!found && (r = atI_insertedge(T, R, &re)) != AT_OK) {
            if (lnew && atO_empty(le)) atD_deleteedge(T, L);
            return r;
        }
        rnew = !found;
    }
    if ((r = atD_collectendpoints(T, L, R, le)) != AT_OK) {
        if (rnew) atD_deleteedge(T, R);
        if (lnew && atO_empty(le)) atD_deleteedge(T, L);
        return r;
    }
    return AT_OK;
}

static int atD_remove(at_Cursor *C, size_t del) {
    at_Tree  *T = C->tree;
    at_Cursor LC, RC;
    size_t    R, L = at_offset(C);
    int       r, fl;
    if (L >= T->bytes || del == 0) return AT_OK;
    R = del > (size_t)-1 - L ? (size_t)-1 : L + del;
    if (R > T->bytes) R = T->bytes;
    if ((r = atD_prepwindow(T, L, R)) != AT_OK) return r;
    if ((r = at_seek(&LC, T, L)) != AT_OK) return r;
    if ((r = at_seek(&RC, T, R)) != AT_OK) return r;
    fl = atD_diverlevel(&LC, &RC);
    if (fl > atK_levels(&LC)) return atD_rmleaf(&LC), AT_OK;
    return atD_rmrange(&LC, &RC, fl), AT_OK;
}

/* APIs */

AT_API int at_splice(at_Cursor *C, size_t del, size_t ins) {
    at_Tree *T;
    size_t   L;
    int      r;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    T = C->tree, L = at_offset(C);
    if (del == 0 && ins == 0) return AT_OK;
    r = atP_reserve(T->S, &T->S->nodes, 4 * (T->levels + 2u) + 8u);
    if (r != AT_OK) return r;
    if ((r = atP_reserve(T->S, &T->S->edges, 3)) != AT_OK) return r;
    if (del > 0 && (r = atD_remove(C, del)) != AT_OK) return r;
    if (ins > 0 && (r = at_seek(C, T, L)) != AT_OK) return r;
    if (ins > 0 && (r = atI_insert(T, L, ins)) != AT_OK) return r;
    return at_seek(C, T, L);
}

AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out) {
    at_Edge   *se, *ee;
    at_Tree   *T;
    at_Anchor *a;
    at_Id      id;
    size_t     start, end;
    int        sl, el, side, r;
    if (C == NULL || C->tree == NULL || out == NULL) return AT_ERRPARAM;
    T = C->tree, start = at_offset(C);
    if (len > (size_t)-1 - start) return AT_ERRPARAM;
    end = start + len;
    if ((r = atO_gravsides(g, &sl, &el)) != AT_OK) return r;
    if ((r = atO_reservenewanchor(T, start, end, sl, el)) != AT_OK) return r;
    if ((r = atO_idalloc(T, &id)) != AT_OK) return r;
    if ((r = atO_edges(T, start, end, &se, &ee)) != AT_OK)
        return atO_idfree(T, id), r;
    side = se == ee && sl == el ? sl : -1;
    if ((r = atO_reservenewedge(T, se, side, id)) != AT_OK) return r;
    r = atO_push(T, sl ? &se->right : &se->left, id, ns, 0);
    assert(r == AT_OK), (void)r;
    r = atO_push(T, el ? &ee->right : &ee->left, id, ns, 1);
    assert(r == AT_OK), (void)r;
    return a = &T->ids[id], a->start = se, a->end = ee, *out = id, AT_OK;
}

AT_API int at_del(at_Tree *T, at_Id id) {
    at_Anchor *a;
    size_t     start, end;
    if ((a = atO_anchor(T, id)) == NULL) return AT_ERRPARAM;
    start = atO_edgepos(T, a->start), end = atO_edgepos(T, a->end);
    atO_clean(T, a->start, start, id, 0), atO_clean(T, a->end, end, id, 1);
    return atO_idfree(T, id), AT_OK;
}

AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end) {
    at_Anchor *a;
    if (start == NULL || end == NULL) return AT_ERRPARAM;
    if ((a = atO_anchor(T, id)) == NULL) return AT_ERRPARAM;
    *start = atO_edgepos(T, a->start), *end = atO_edgepos(T, a->end);
    return AT_OK;
}

AT_API int at_gravity(at_Tree *T, at_Id id) {
    at_Anchor *a;
    int        sl, el;
    if ((a = atO_anchor(T, id)) == NULL) return AT_ERRPARAM;
    sl = atO_sideof(a->start, id, 0), el = atO_sideof(a->end, id, 1);
    if (sl < 0 || el < 0) return AT_ERRPARAM;
    if (sl == 0 && el == 1) return AT_GROW;
    if (sl == 1 && el == 0) return AT_FIXED;
    if (sl == 1 && el == 1) return AT_SHIFT;
    return AT_LOCK;
}

AT_NS_END

#endif /* AT_IMPLEMENTATION */
