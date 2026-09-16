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

#include <stddef.h>

#define AT_OK       (0)         /* No error */
#define AT_ERRPARAM (-1)        /* Invalid parameter */
#define AT_ERRMEM   (-2)        /* Memory allocation failed */
#define AT_ESTALE   (-3)        /* Cursor is stale; seek or locate again */
#define AT_NONE     (~(at_Id)0) /* End-of-iteration sentinel */

#define AT_ITER_TREE 0u /* normal tree iteration */
#define AT_ITER_DONE 1u /* iteration exhausted */

AT_NS_BEGIN

typedef enum at_Gravity {
    AT_GROW,  /* [L,R] extends when insert before or after */
    AT_FIXED, /* [R,L] does not extend when insert in both side */
    AT_SHIFT, /* [R,R] shifts when insert before, extends after */
    AT_LOCK   /* [L,L] extends when insert before, not after */
} at_Gravity;

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
AT_API size_t   at_bytes(const at_Tree *T);
AT_API void     at_reset(at_Tree *T);

/* cursor */
AT_API int   at_seek(at_Cursor *C, at_Tree *T, size_t pos);
AT_API int   at_locate(at_Cursor *C, size_t pos);
AT_API int   at_advance(at_Cursor *C, at_Delta d);
AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen);

/* anchors */
AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out);
AT_API int at_delanchor(at_Tree *T, at_Id id);
AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end);
AT_API int at_gravity(at_Tree *T, at_Id id);

/* splice */
AT_API int at_splice(at_Cursor *C, size_t del, size_t ins);

/* structure */

#ifndef AT_MAX_LEVEL
# define AT_MAX_LEVEL 16 /* safe for AT_FANOUT=32; see docs/max_levels.md */
#endif

struct at_Cursor {
    at_Tree *tree;     /* tree under navigation */
    size_t   off;      /* start position of the current span */
    size_t   poff;     /* offset within the current span */
    unsigned side;     /* endpoint bucket: 0=left, 1=right, 2=done */
    unsigned next_id;  /* next slot anchor id; 0 = bucket exhausted */
    unsigned next_end; /* next slot is_end tag */
    unsigned iter;     /* iteration phase: AT_ITER_TREE/DONE */
    size_t   epoch;    /* tree generation captured by seek/locate */

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
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define AT_STATIC_ASSERT(cond)      AT_SA_0(cond, at_SA_, __LINE__)
#define AT_SA_0(cond, prefix, line) AT_SA_1(cond, prefix, line)
#define AT_SA_1(cond, prefix, line) typedef char prefix##line[(cond) ? 1 : -1]

#ifndef AT_FANOUT
# define AT_FANOUT 32
#endif /* AT_FANOUT */

AT_STATIC_ASSERT(AT_FANOUT >= 4);
AT_STATIC_ASSERT(AT_FANOUT <= 0xffffu);

#define AT_ANCHOR_MAX ((size_t)1u << 31) /* at_Point.id is 31 bits */
/* largest coordinate/delta representable without narrowing through at_Delta */
#define AT_DELTA_MAX (((size_t)1 << (sizeof(at_Delta) * CHAR_BIT - 1)) - 1)

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
    unsigned id : 31;    /* anchor id; 0 = empty slot */
    unsigned is_end : 1; /* 1 = END occurrence, 0 = START occurrence */
} at_Point;

typedef struct at_Anchor {
    struct at_Edge *start;       /* start edge; NULL when dead */
    struct at_Edge *end;         /* end edge; NULL when dead */
    at_Point        snext;       /* next START occurrence in its bucket */
    at_Point        enext;       /* next END occurrence in its bucket */
    unsigned        ns : 29;     /* namespace filter value */
    unsigned        gravity : 2; /* at_Gravity */
} at_Anchor;

typedef struct at_Edge {
    at_Node *parent; /* leaf node holding this edge */
    at_Index idx;    /* slot index in parent */
    at_Point left;   /* anchor occurrences on the LEFT side */
    at_Point right;  /* anchor occurrences on the RIGHT side */
} at_Edge;

struct at_State {
    void     *alloc_ud; /* allocator user data */
    at_Alloc *allocf;   /* memory allocator */
    at_Pool   nodes;    /* pool for at_Node objects */
    at_Pool   edges;    /* pool for at_Edge objects */
};

struct at_Tree {
    at_Node    root;   /* embedded root node */
    at_State  *S;      /* owning state */
    size_t     bytes;  /* position of the tail edge or last span end */
    int        levels; /* tree height, 0 = leaf-only root */
    at_Anchor *ids;    /* dense anchor array; id 0 is reserved */
    size_t     idcap;  /* allocated capacity of ids */
    at_Id      idfree; /* freelist head for anchor ids */
    size_t     epoch;  /* structural generation; bumped on tree mutation */
};

AT_STATIC_ASSERT(sizeof(at_Point) == 4);
AT_STATIC_ASSERT(sizeof(at_Node) < AT_PAGE_SIZE / 2);
AT_STATIC_ASSERT(sizeof(at_Edge) < AT_PAGE_SIZE / 2);

#define atN_cc(n)            ((int)(n)->child_count)
#define atN_setcc(n, v)      ((n)->child_count = (at_Index)(v))
#define atN_edge(n, i)       ((at_Edge *)(n)->children[i])
#define atN_setedge(n, i, e) ((n)->children[i] = (at_Node *)(e))
#define atN_min()            (AT_FANOUT / 2)

#define atK_levels(C)    ((C)->tree->levels)
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
    char  *page, *end;
    size_t sz = p->obj_size;
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

/* object/edge helpers */

/* clang-format off */
static int atO_empty(at_Edge *e)
{ return e->left.id == 0 && e->right.id == 0; }

static at_Id atO_none(size_t *plen)
{ return (void)(plen && (*plen = 0)), AT_NONE; }

static at_Id atO_done(at_Cursor *C, size_t *plen)
{ return C->iter = AT_ITER_DONE, atO_none(plen); }

static void atO_cursorstart(at_Cursor *C, at_Edge *e)
{ C->side = 0, C->next_id = e->left.id, C->next_end = e->left.is_end; }

static int atO_idok(at_Tree *T, at_Id id)
{ return T != NULL && id != 0 && T->ids != NULL && id < T->idcap; }
/* clang-format on */

AT_API size_t at_bytes(const at_Tree *T) { return T ? T->bytes : 0; }

static at_Edge *atO_newedge(at_Tree *T) {
    at_Edge *e = (at_Edge *)atP_ralloc(&T->S->edges);
    return memset(e, 0, sizeof(*e)), e;
}

static int atO_side(int g, int end) {
    if (end) return g == AT_GROW || g == AT_SHIFT;
    return g == AT_FIXED || g == AT_SHIFT;
}

static void atO_link(at_Edge *e, int side, at_Point slot, at_Anchor *a) {
    at_Point *head = side ? &e->right : &e->left;
    at_Point *next = slot.is_end ? &a->enext : &a->snext;
    *next = *head, *head = slot;
}

static void atO_unlink(at_Tree *T, at_Edge *e, int side, at_Point slot) {
    at_Point   prev, next, *head = side ? &e->right : &e->left, p = *head;
    at_Anchor *a, *pa;
    prev.id = 0, prev.is_end = 0;
    while (p.id != 0 && (p.id != slot.id || p.is_end != slot.is_end)) {
        a = &T->ids[p.id];
        prev = p;
        p = p.is_end ? a->enext : a->snext;
    }
    assert(p.id == slot.id && p.is_end == slot.is_end), a = &T->ids[p.id];
    next = p.is_end ? a->enext : a->snext;
    if (prev.id == 0)
        *head = next;
    else {
        pa = &T->ids[prev.id];
        *(prev.is_end ? &pa->enext : &pa->snext) = next;
    }
}

static void atO_moveside(at_Tree *T, at_Edge *dst, at_Edge *src, int side) {
    at_Point   next, *head = side ? &src->right : &src->left, p = *head;
    at_Anchor *a;
    head->id = 0, head->is_end = 0;
    for (; p.id != 0; p = next) {
        a = &T->ids[p.id];
        next = p.is_end ? a->enext : a->snext;
        *(p.is_end ? &a->end : &a->start) = dst;
        atO_link(dst, side, p, a);
    }
}

static void atO_clear(at_Tree *T) {
    if (T->ids != NULL) {
        T->S->allocf(T->S->alloc_ud, T->ids, T->idcap * sizeof(at_Anchor), 0);
        T->ids = NULL, T->idcap = 0, T->idfree = 0;
    }
}

static int atO_growids(at_Tree *T) {
    at_Anchor *anchors;
    size_t     sz, i, oldcap = T->idcap, newcap = oldcap ? oldcap * 2 : 8;
    size_t     from = oldcap ? oldcap : 1;
    if (newcap <= oldcap || newcap > AT_ANCHOR_MAX) return AT_ERRMEM;
    if (newcap > (size_t)-1 / sizeof(at_Anchor)) return AT_ERRMEM;
    sz = newcap * sizeof(at_Anchor);
    anchors = (at_Anchor *)T->S->allocf(
            T->S->alloc_ud, T->ids, oldcap * sizeof(at_Anchor), sz);
    if (anchors == NULL) return AT_ERRMEM;
    if (oldcap == 0) memset(anchors, 0, sizeof(at_Anchor));
    memset(anchors + from, 0, (newcap - from) * sizeof(at_Anchor));
    for (i = newcap; i > from; --i) {
        at_Anchor *a = &anchors[i - 1];
        a->snext.id = (unsigned)T->idfree, a->snext.is_end = 0;
        T->idfree = (at_Id)(i - 1);
    }
    return T->ids = anchors, T->idcap = newcap, AT_OK;
}

static int atO_idalloc(at_Tree *T, at_Id *out) {
    at_Anchor *a;
    int        r;
    if (T->idfree == 0 && (r = atO_growids(T)) != AT_OK) return r;
    a = &T->ids[T->idfree];
    *out = T->idfree, T->idfree = a->snext.id;
    return a->start = NULL, a->end = NULL, AT_OK;
}

static void atO_idfree(at_Tree *T, at_Id id) {
    at_Anchor *a = &T->ids[id];
    a->snext.id = T->idfree, a->snext.is_end = 0, a->end = NULL, T->idfree = id;
}

static at_Anchor *atO_anchor(at_Tree *T, at_Id id) {
    at_Anchor *a;
    assert(atO_idok(T, id));
    a = &T->ids[id];
    return a->end == NULL ? NULL : a;
}

static int atO_scan(at_Cursor *C, at_Edge *e, int ns, at_Id *out) {
    at_Tree   *T = C->tree;
    at_Point   p, next;
    at_Anchor *a;
    while (C->side < 2) {
        if (C->next_id == 0) {
            C->side += 1;
            if (C->side == 1)
                C->next_id = e->right.id, C->next_end = e->right.is_end;
        } else {
            p.id = C->next_id, p.is_end = C->next_end;
            a = &T->ids[p.id], next = p.is_end ? a->enext : a->snext;
            C->next_id = next.id, C->next_end = next.is_end;
            if (!p.is_end && (ns == 0 || a->ns == (unsigned)ns))
                return *out = p.id, 1;
        }
    }
    return 0;
}

static at_Id atO_found(at_Tree *T, at_Id id, size_t *plen) {
    size_t start, end;
    int    r;
    if (plen == NULL) return id;
    start = end = 0;
    r = at_range(T, id, &start, &end), assert(r == AT_OK), (void)r;
    return *plen = start <= end ? end - start : 0, id;
}

/* node operations */

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
    atN_move(n, i + k, i, cc - i), atN_setcc(n, cc + k);
    atN_fixlinks(n, i + k, cc - i, leaf);
}

static void atN_removeslot(at_Node *n, int i, int k, int leaf) {
    int cc = atN_cc(n);
    assert(i >= 0 && i + k <= cc);
    atN_move(n, i, i + k, cc - i - k), atN_setcc(n, cc - k);
    atN_fixlinks(n, i, cc - i - k, leaf);
}

/* clang-format off */
static size_t atN_sumbytes(const at_Node *n, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += n->bytes[i]; return s; }

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
            atP_free(&T->S->edges, edge);
        }
    } else {
        for (i = s; i < e; ++i) {
            at_Node *c = p->children[i];
            atN_purge(T, c, k - 1, 0, atN_cc(c));
            atP_free(&T->S->nodes, c);
        }
    }
}

static void atN_remove(at_Tree *T, at_Node *p, int k, int s, int e) {
    int leaf = k == 0, cc = atN_cc(p);
    assert(s <= e && e <= cc);
    atN_purge(T, p, k, s, e);
    atN_move(p, s, e, cc - e), atN_setcc(p, cc - (e - s));
    atN_fixlinks(p, s, cc - e, leaf);
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
    return memset(T, 0, sizeof(at_Tree)), T->S = S, T->epoch = 1, T;
}

AT_API void at_deltree(at_Tree *T) {
    if (T == NULL) return;
    atN_purge(T, &T->root, T->levels, 0, atN_cc(&T->root));
    atO_clear(T);
    T->S->allocf(T->S->alloc_ud, T, sizeof(at_Tree), 0);
}

AT_API void at_reset(at_Tree *T) {
    if (T == NULL) return;
    T->epoch += 1;
    atN_purge(T, &T->root, T->levels, 0, atN_cc(&T->root));
    atO_clear(T);
    memset(&T->root, 0, sizeof(at_Node));
    T->bytes = 0, T->levels = 0;
}

/* metric/path helpers */

static void atM_up(at_Cursor *C, int l, at_Delta db) {
    for (; l >= 0; --l) {
        at_Node *p;
        int      i = atK_idx(C, p = atK_parent(C, l), l);
        p->bytes[i] += db;
    }
    C->tree->bytes += db;
}

/* cursor navigation */

static int atK_locend(at_Cursor *C) {
    at_Node *n = &C->tree->root;
    int      l;
    if (atK_levels(C) == 0 && atN_cc(n) == 0)
        return (C->paths[0] = n->children, C->off = 0, C->poff = 0), 0;
    for (l = 0; l < atK_levels(C); ++l)
        n = *(C->paths[l] = &n->children[atN_cc(n) - 1]);
    assert(atN_cc(n)), C->paths[l] = &n->children[atN_cc(n) - 1];
    C->poff = n->bytes[atN_cc(n) - 1];
    return (C->off = C->tree->bytes - C->poff), 1;
}

static void atK_findleaf(at_Cursor *C, int l, size_t *poff) {
    for (; l <= atK_levels(C); ++l) {
        at_Node *p = atK_parent(C, l);
        int      i;
        for (i = 0; i < atN_cc(p) && *poff >= p->bytes[i]; ++i)
            *poff -= p->bytes[i], C->off += p->bytes[i];
        assert(i < atN_cc(p)), C->paths[l] = &p->children[i];
    }
}

static int atK_forwardoff(at_Cursor *C, size_t d) {
    at_Node *p;
    int      l, i = atK_idx(C, p = atK_parent(C, atK_levels(C)), atK_levels(C));
    size_t   in = p->bytes[i] - C->poff;
    if (d < in) return C->poff += d, 0;
    d -= in, C->off += p->bytes[i], C->poff = 0;
    for (l = atK_levels(C); l >= 0; --l) {
        i = atK_idx(C, p = atK_parent(C, l), l) + 1;
        for (; i < atN_cc(p) && d >= p->bytes[i]; ++i)
            d -= p->bytes[i], C->off += p->bytes[i];
        if (i < atN_cc(p)) break;
    }
    assert(l >= 0 && i < atN_cc(p)), C->paths[l] = &p->children[i];
    return atK_findleaf(C, l + 1, &d), C->poff = d, 1;
}

static int atK_backwardoff(at_Cursor *C, size_t d) {
    at_Node *p = NULL;
    int      l, i = 0;
    if (d <= C->poff) return C->poff -= d, 0;
    d -= C->poff, C->poff = 0;
    for (l = atK_levels(C); l >= 0; --l) {
        i = atK_idx(C, p = atK_parent(C, l), l) - 1;
        for (; i >= 0 && d > p->bytes[i]; --i)
            d -= p->bytes[i], C->off -= p->bytes[i];
        if (i >= 0) break;
    }
    assert(l >= 0 && i >= 0), d = p->bytes[i] - d, C->off -= p->bytes[i];
    C->paths[l] = &p->children[i];
    return atK_findleaf(C, l + 1, &d), C->poff = d, 1;
}

static int atK_nextedge(at_Cursor *C) {
    at_Node *p, *q;
    int      l, i, cc, L = atK_levels(C);
    size_t   b;
    p = L ? *C->paths[L - 1] : &C->tree->root;
    i = atK_idx(C, p, L), cc = atN_cc(p), b = p->bytes[i];
    if (i + 1 < cc) {
        C->paths[L] = &p->children[i + 1];
        return C->off += b, C->poff = 0, 1;
    }
    for (l = L - 1; l >= 0; --l) {
        q = l ? *C->paths[l - 1] : &C->tree->root;
        i = atK_idx(C, q, l), cc = atN_cc(q);
        if (i + 1 < cc) {
            C->paths[l] = &q->children[i + 1];
            for (; l < L; ++l) {
                p = *C->paths[l];
                C->paths[l + 1] = &p->children[0];
            }
            return C->off += b, C->poff = 0, 1;
        }
    }
    if (C->poff > 0) return 0;
    return C->poff = b, 0;
}

static void atO_startcurrent(at_Cursor *C) {
    at_Node *p;
    int      L, i;
    C->iter = AT_ITER_TREE;
    if (atN_cc(&C->tree->root) != 0) {
        L = C->tree->levels, p = L ? *C->paths[L - 1] : &C->tree->root;
        i = (int)(C->paths[L] - p->children);
        atO_cursorstart(C, atN_edge(p, i));
    }
}

AT_API int at_locate(at_Cursor *C, size_t pos) {
    at_Tree *T;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    T = C->tree, C->iter = AT_ITER_TREE, C->epoch = T->epoch;
    C->off = C->poff = 0;
    if (pos >= T->bytes) {
        atK_locend(C), C->poff += pos - T->bytes;
        return atO_startcurrent(C), AT_OK;
    }
    atK_findleaf(C, 0, &pos), C->poff = pos;
    return atO_startcurrent(C), AT_OK;
}

AT_API int at_seek(at_Cursor *C, at_Tree *T, size_t pos) {
    if (C == NULL || T == NULL) return AT_ERRPARAM;
    memset(C, 0, sizeof(at_Cursor)), C->tree = T;
    return at_locate(C, pos);
}

AT_API int at_advance(at_Cursor *C, at_Delta d) {
    at_Tree *T;
    size_t   off, step;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    if (d == 0) return AT_OK;
    T = C->tree, off = at_offset(C), C->iter = AT_ITER_TREE;
    step = d < 0 ? (size_t)0 - (size_t)d : (size_t)d;
    if (d > 0 && step > (size_t)-1 - off) step = (size_t)-1 - off;
    if (T->bytes == 0) {
        C->poff = d < 0 ? (step < off ? off - step : 0) : off + step;
        C->off = 0, C->paths[0] = T->root.children;
        return atO_startcurrent(C), AT_OK;
    }
    if (d < 0) {
        atK_backwardoff(C, step > off ? off : step);
        return atO_startcurrent(C), AT_OK;
    }
    if (off + step >= T->bytes) {
        atK_locend(C), C->poff += off + step - T->bytes;
        return atO_startcurrent(C), AT_OK;
    }
    return atK_forwardoff(C, step), atO_startcurrent(C), AT_OK;
}

static at_Edge *atK_edge(at_Cursor *C) {
    at_Node *p = atK_parent(C, atK_levels(C));
    return atN_edge(p, atK_idx(C, p, atK_levels(C)));
}

static at_Id atO_step(at_Cursor *C, int ns, size_t *plen) {
    at_Edge *e;
    at_Node *p;
    at_Tree *T = C->tree;
    at_Id    id;
    size_t   b;
    int      i, L = atK_levels(C);
    p = L ? *C->paths[L - 1] : &T->root;
    i = atK_idx(C, p, L), b = p->bytes[i];
    if (C->poff > b) return atO_done(C, plen);
    if (C->poff > 0) {
        if (!atK_nextedge(C)) return atO_done(C, plen);
        atO_startcurrent(C);
    }
    e = atK_edge(C);
    while (!atO_scan(C, e, ns, &id)) {
        if (!atK_nextedge(C)) return atO_done(C, plen);
        atO_startcurrent(C), e = atK_edge(C);
    }
    return atO_found(T, id, plen);
}

AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen) {
    if (C == NULL || C->tree == NULL) return atO_none(plen);
    if (C->epoch != C->tree->epoch) return atO_none(plen);
    if (ns < 0) return atO_none(plen);
    if (C->iter == AT_ITER_DONE) return atO_none(plen);
    if (C->tree->bytes == 0 && atN_cc(&C->tree->root) == 0)
        return atO_done(C, plen);
    return atO_step(C, ns, plen);
}

/* cursor/edge helpers */

static size_t atK_edgepos(at_Cursor *C) {
    size_t pos = 0;
    int    l;
    for (l = 0; l <= atK_levels(C); ++l) {
        at_Node *p = atK_parent(C, l);
        pos += atN_sumbytes(p, 0, atK_idx(C, p, l));
    }
    return C->off = pos;
}

static void atK_fromedge(at_Cursor *C, at_Tree *T, at_Edge *e) {
    at_Node *n = e->parent;
    int      l;
    C->tree = T, C->iter = AT_ITER_TREE;
    C->side = 2, C->next_id = 0, C->next_end = 0;
    C->paths[T->levels] = &n->children[e->idx];
    for (l = T->levels - 1; l >= 0; --l) {
        at_Node *p = n->parent;
        C->paths[l] = &p->children[n->idx], n = p;
    }
    C->poff = 0, C->epoch = T->epoch;
    atK_edgepos(C);
}

static void atK_touch(at_Cursor *C, at_Delta d) {
    at_Node *p;
    int      l = atK_levels(C);
    p = atK_parent(C, l);
    p->bytes[atK_idx(C, p, l)] += d, atM_up(C, l - 1, d);
}

static at_Edge *atK_prevedge(const at_Cursor *C) {
    at_Node *p = atK_parent(C, atK_levels(C));
    int      h = 0, i = atK_idx(C, p, atK_levels(C));
    for (; i == 0 && p->parent != NULL; ++h) i = p->idx, p = p->parent;
    if (i == 0) return NULL;
    if (h == 0) return atN_edge(p, i - 1);
    p = p->children[i - 1], --h;
    for (; h > 0; --h) p = p->children[atN_cc(p) - 1];
    return atN_edge(p, atN_cc(p) - 1);
}

static int atI_reserve(at_Cursor *C, size_t edges, size_t passes) {
    at_Tree *T = C->tree;
    size_t   nodes = passes * (4u * (size_t)(T->levels + 2) + 8u);
    int      r = atP_reserve(T->S, &T->S->nodes, nodes);
    if (r != AT_OK) return r;
    return atP_reserve(T->S, &T->S->edges, edges);
}

/* insertion / split */

static int atI_splitnode(at_State *S, at_Node *n, int level, at_Node **pr) {
    int      cc = atN_cc(n), left = cc / 2, right = cc - left;
    at_Node *r = (at_Node *)atP_ralloc(&S->nodes);
    assert(left >= AT_FANOUT / 2 && right >= AT_FANOUT / 2);
    atN_setcc(r, right), r->parent = n->parent, r->idx = 0;
    atN_copy(r, 0, n, left, right);
    atN_fixlinks(r, 0, right, level == 0);
    return atN_setcc(n, left), *pr = r, left;
}

static void atI_splitroot(at_Cursor *C) {
    at_Tree *T = C->tree;
    at_Node *pp, *nw, *r = &T->root, save = *r;
    int      i = atK_idx(C, r, 0), mid = atN_cc(&save) / 2;
    int      nc = atN_cc(&save) - mid, leaf = T->levels == 0;
    pp = (at_Node *)atP_ralloc(&T->S->nodes);
    nw = (at_Node *)atP_ralloc(&T->S->nodes);
    *pp = save, atN_setcc(pp, mid);
    atN_copy(nw, 0, &save, mid, atN_setcc(nw, nc));
    atN_fixlinks(pp, 0, mid, leaf), atN_fixlinks(nw, 0, nc, leaf);
    atN_set(r, 0, pp, atN_sumbytes(pp, 0, mid));
    atN_set(r, 1, nw, atN_sumbytes(nw, 0, nc));
    atN_setcc(r, 2), T->levels += 1, assert(T->levels < AT_MAX_LEVEL);
    memmove(C->paths + 1, C->paths, (size_t)T->levels * sizeof(at_Node **));
    C->paths[0] = &r->children[i >= mid];
    C->paths[1] = &(*C->paths[0])->children[i < mid ? i : i - mid];
}

static void atI_splitchild(at_Cursor *C, int l) {
    at_Tree *T = C->tree;
    at_Node *nw, *p = atK_parent(C, l), *nd = (at_Node *)*(C->paths[l]);
    int      cs, mid, i = atK_idx(C, p, l);
    assert(atN_cc(nd) >= AT_FANOUT);
    mid = atI_splitnode(T->S, nd, T->levels - l - 1, &nw);
    atN_makespace(p, i + 1, 1, 0);
    atN_set(p, i + 1, nw, atN_sumbytes(nw, 0, atN_cc(nw)));
    p->bytes[i] = atN_sumbytes(nd, 0, atN_cc(nd));
    if ((cs = atK_idx(C, nd, l + 1)) >= mid) {
        C->paths[l] = &p->children[i + 1];
        C->paths[l + 1] = &nw->children[cs - mid];
    }
}

static void atI_makeroom(at_Cursor *C) {
    int l;
    for (l = atK_levels(C); l >= 0; --l)
        if (atN_cc(atK_parent(C, l)) < AT_FANOUT) break;
    if (l < 0) atI_splitroot(C), l = 1;
    for (; l < atK_levels(C); ++l) atI_splitchild(C, l);
}

static void atI_splitat(at_Cursor *C, at_Edge *e) {
    at_Node *p;
    size_t   rem;
    int      l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l);
    assert(C->poff > 0 && C->poff < p->bytes[i]);
    rem = p->bytes[i] - C->poff;
    if (atN_cc(p) >= AT_FANOUT) {
        atI_makeroom(C);
        l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l);
    }
    atN_makespace(p, i + 1, 1, 1);
    p->bytes[i] = C->poff, atN_setleaf(p, i + 1, e, rem);
    C->off += C->poff, C->poff = 0;
    C->paths[l] = &p->children[i + 1];
}

static void atI_pad(at_Cursor *C, at_Edge *e) {
    at_Node *p;
    int      l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l);
    int      cc = atN_cc(p);
    if (cc >= AT_FANOUT) {
        atI_makeroom(C);
        l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l);
        cc = atN_cc(p);
    }
    assert(i == cc - 1 && C->poff == p->bytes[i]);
    atN_makespace(p, cc, 1, 1);
    atN_setleaf(p, cc, e, 0);
    C->off += p->bytes[i], C->paths[l] = &p->children[cc], C->poff = 0;
}

/* remove */

static int atD_balancenode(at_Node **ns, int left, at_Delta *ds, int leaf) {
    int d, l = atN_cc(ns[0]), r = atN_cc(ns[1]);
    d = l - ((l + r + (left != 0)) >> 1);
    if (assert(d != 0), d < 0) {
        atN_copy(ns[0], l, ns[1], 0, -d), atN_move(ns[1], 0, -d, r + d);
        atN_fixlinks(ns[0], l, -d, leaf), atN_fixlinks(ns[1], 0, r + d, leaf);
        *ds = -(at_Delta)atN_sumbytes(ns[0], l, l - d);
    } else {
        atN_move(ns[1], d, 0, r), atN_copy(ns[1], 0, ns[0], l - d, d);
        atN_fixlinks(ns[1], 0, r + d, leaf);
        *ds = (at_Delta)atN_sumbytes(ns[1], 0, d);
    }
    return atN_setcc(ns[0], l - d), atN_setcc(ns[1], r + d), d;
}

static int atD_foldnode(at_Cursor *C, int lfirst, int l) {
    at_Node  *p, ***cp = &C->paths[l];
    int       leaf = l == atK_levels(C) - 1;
    int       cL, cR, dn, r = 0, i = atK_idx(C, p = atK_parent(C, l), l);
    at_Node **ns = &p->children[i];
    at_Delta  ds;
    if (assert(atN_cc(p) > 1), atN_cc(ns[0]) > atN_min()) return 0;
    if ((i && lfirst) || i == atN_cc(p) - 1) ns -= 1, i -= 1, r = 1;
    cL = atN_cc(ns[0]), cR = atN_cc(ns[1]);
    if (cL + cR <= AT_FANOUT) {
        atN_copy(ns[0], cL, ns[1], 0, cR), atN_fixlinks(ns[0], cL, cR, leaf);
        atN_setcc(ns[0], cL + cR), atN_setcc(ns[1], 0);
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
    int      i;
    assert(l == 0 || l < atK_levels(C));
    for (; l > 0; --l) {
        p = atK_parent(C, l);
        if (atN_cc(p->children[atK_idx(C, p, l)]) >= atN_min()) return;
        if (assert(atN_cc(p) > 1), !atD_foldnode(C, 0, l)) return;
    }
    assert(atK_levels(C) > 0), i = atK_idx(C, p = &C->tree->root, 0);
    if (atN_cc(p->children[i]) < atN_min() && atN_cc(p) >= 2)
        atD_foldnode(C, 0, 0);
    while (atK_levels(C) && atN_cc(&C->tree->root) == 1) {
        int cc, nl;
        i = atK_idx(C, p = atK_parent(C, 1), 1);
        cc = atN_cc(p), nl = atK_levels(C) - 1;
        C->tree->root = *p, C->tree->root.parent = NULL;
        atP_free(&C->tree->S->nodes, p);
        C->tree->levels = nl, C->paths[0] += i;
        memmove(C->paths + 1, C->paths + 2, atK_levels(C) * sizeof(at_Node **));
        atN_fixlinks(&C->tree->root, 0, cc, nl == 0);
    }
}

static void atD_rmleaf(at_Cursor *C) {
    at_Tree  *T = C->tree;
    at_Cursor P;
    at_Node  *p;
    at_Edge  *e, *prev;
    size_t    delta;
    int       i, l = atK_levels(C);
    p = atK_parent(C, l), i = atK_idx(C, p, l);
    e = atN_edge(p, i), delta = p->bytes[i], prev = atK_prevedge(C);
    assert(atO_empty(e));
    atN_removeslot(p, i, 1, 1), atP_free(&T->S->edges, e);
    if (prev == NULL) {
        assert(T->bytes == 0);
        C->off = C->poff = 0, C->paths[0] = p->children;
    } else {
        atK_fromedge(&P, T, prev), atK_touch(&P, (at_Delta)delta);
        C->paths[l] = p->children, atM_up(C, l - 1, -(at_Delta)delta);
        if (l > 0 && atN_cc(p) < atN_min()) atD_rebalance(C, l - 1);
        atK_fromedge(C, T, prev), C->poff = prev->parent->bytes[prev->idx];
    }
}

static void atD_bounds(at_Cursor *L, at_Cursor *R, size_t *bR) {
    at_Tree *T = L->tree;
    at_Edge *e;
    assert(L->poff == 0);
    *bR = 0;
    if (at_offset(R) >= T->bytes) return;
    if (R->poff > 0) atI_splitat(R, atO_newedge(T));
    e = atK_edge(R);
    *bR = e->parent->bytes[e->idx];
}

static void atD_rmempty(at_Cursor *C) {
    at_Edge *e = atK_edge(C);
    size_t   at, pos = at_offset(C);
    if (!atO_empty(e)) return;
    at = atK_edgepos(C);
    if (at == 0 && C->tree->bytes != 0) return;
    atD_rmleaf(C);
    if (atN_cc(&C->tree->root) != 0) {
        assert(pos >= C->off), C->poff = pos - C->off;
    }
}

static void atD_remove(at_Cursor *C, size_t del) {
    at_Tree  *T = C->tree;
    at_Cursor R, S, L = *C;
    at_Edge  *le, *e;
    size_t    pos, Rpos, bR;
    int       r;
    assert(del > 0);
    pos = at_offset(C);
    if (pos >= T->bytes) return;
    if (del > T->bytes - pos) del = T->bytes - pos;
    if (L.poff > 0) atI_splitat(&L, atO_newedge(T));
    le = atK_edge(&L), Rpos = pos + del, R = L;
    r = at_advance(&R, (at_Delta)del), assert(r == AT_OK), (void)r;
    atD_bounds(&L, &R, &bR);
    atK_fromedge(&S, T, le);
    while (atK_nextedge(&S) && S.off <= Rpos) {
        e = atK_edge(&S);
        atO_moveside(T, le, e, 0), atO_moveside(T, le, e, 1);
        atD_rmleaf(&S);
    }
    atK_fromedge(&L, T, le);
    atK_touch(&L, (at_Delta)bR - (at_Delta)le->parent->bytes[le->idx]);
    if (atO_empty(le)) atD_rmempty(&L);
    *C = L, C->epoch = T->epoch;
}

/* insertion */

static void atI_emptyedge(at_Cursor *C, at_Edge **out) {
    at_Tree *T = C->tree;
    at_Edge *e = atO_newedge(T);
    size_t   pos = at_offset(C);
    if (pos == 0) {
        atN_setleaf(&T->root, 0, e, 0), atN_setcc(&T->root, 1);
        C->paths[0] = &T->root.children[0], C->off = 0, C->poff = 0;
    } else {
        at_Edge *e0 = atO_newedge(T);
        atN_setleaf(&T->root, 0, e0, pos), atN_setleaf(&T->root, 1, e, 0);
        atN_setcc(&T->root, 2), T->bytes = pos;
        C->paths[0] = &T->root.children[1], C->off = pos, C->poff = 0;
    }
    *out = e;
}

static void atI_ensure(at_Cursor *C, at_Edge **out) {
    at_Tree *T = C->tree;
    at_Node *p;
    at_Edge *e;
    size_t   b, pos = at_offset(C);
    int      l, i;
    if (atN_cc(&T->root) == 0) {
        atI_emptyedge(C, out);
    } else if (C->poff == 0) {
        *out = atK_edge(C);
    } else {
        l = atK_levels(C), p = atK_parent(C, l), i = atK_idx(C, p, l);
        b = p->bytes[i];
        if (C->poff < b)
            atI_splitat(C, e = atO_newedge(T)), *out = e;
        else {
            if (pos > T->bytes) atK_touch(C, (at_Delta)(pos - T->bytes));
            e = atO_newedge(T);
            atI_pad(C, e), *out = e;
        }
    }
}

static void atI_edges(at_Cursor *C, size_t end, at_Edge **a, at_Edge **b) {
    at_Cursor D, L = *C;
    size_t    start = at_offset(C);
    int       r;
    atI_ensure(&L, a);
    if (end == start)
        *b = *a;
    else {
        D = L, r = at_advance(&D, (at_Delta)(end - start));
        assert(r == AT_OK), (void)r;
        atI_ensure(&D, b);
    }
}

static void atI_insertempty(at_Cursor *C, size_t pos, size_t end) {
    at_Tree *T = C->tree;
    at_Edge *e = atO_newedge(T);
    atN_setleaf(&T->root, 0, e, end);
    atN_setcc(&T->root, 1), T->bytes = end;
    C->paths[0] = &T->root.children[0], C->off = 0, C->poff = pos;
}

static void atI_insertsplit(at_Cursor *C, at_Node *p, int i, size_t ins) {
    at_Tree *T = C->tree;
    at_Edge *e = atN_edge(p, i), *ne = atO_newedge(T);
    size_t   b;
    int      l;
    assert(C->poff == 0 && e->right.id != 0);
    if (atN_cc(p) >= AT_FANOUT) {
        atI_makeroom(C);
        l = atK_levels(C), p = atK_parent(C, l), i = atK_idx(C, p, l);
        e = atN_edge(p, i);
    }
    b = p->bytes[i], atK_touch(C, (at_Delta)ins);
    p->bytes[i] = ins, atN_makespace(p, i + 1, 1, 1);
    atN_setleaf(p, i + 1, ne, b);
    atO_moveside(T, ne, e, 1), atD_rmempty(C);
}

static void atI_insert(at_Cursor *C, size_t ins) {
    at_Tree *T = C->tree;
    at_Node *p;
    at_Edge *e;
    size_t   pos;
    int      l, i;
    assert(ins > 0);
    pos = at_offset(C), assert(ins <= (size_t)-1 - pos);
    if (atN_cc(&T->root) == 0) {
        atI_insertempty(C, pos, pos + ins);
    } else {
        l = atK_levels(C), p = atK_parent(C, l), i = atK_idx(C, p, l);
        e = atN_edge(p, i);
        if (C->poff == 0 && e->right.id != 0)
            atI_insertsplit(C, p, i, ins);
        else {
            size_t b = p->bytes[i];
            atK_touch(C, (at_Delta)(ins + (C->poff > b ? C->poff - b : 0)));
            atD_rmempty(C);
        }
    }
}

/* APIs */

AT_API int at_splice(at_Cursor *C, size_t del, size_t ins) {
    at_Tree *T;
    size_t   L, M;
    int      r;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    T = C->tree, L = at_offset(C);
    if (del == 0 && ins == 0) return AT_OK;
    if (ins == 0 && L >= T->bytes) return AT_OK;
    M = L > T->bytes ? L : T->bytes;
    if (M > AT_DELTA_MAX || ins > AT_DELTA_MAX - M) return AT_ERRPARAM;
    r = atI_reserve(C, 6, 3);
    if (r != AT_OK) return r;
    T->epoch += 1, C->epoch = T->epoch;
    if (del > 0) atD_remove(C, del);
    if (ins > 0) atI_insert(C, ins);
    assert(at_offset(C) == L), (void)L;
    return atO_startcurrent(C), AT_OK;
}

AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out) {
    at_Edge   *se, *ee;
    at_Tree   *T;
    at_Anchor *a;
    at_Point   slot;
    at_Cursor  L;
    at_Id      id;
    size_t     start;
    int        r;
    if (C == NULL || C->tree == NULL || out == NULL) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    if (g < AT_GROW || g > AT_LOCK) return AT_ERRPARAM;
    T = C->tree, start = at_offset(C);
    if (start > AT_DELTA_MAX || len > AT_DELTA_MAX - start) return AT_ERRPARAM;
    L = *C;
    if ((r = atO_idalloc(T, &id)) != AT_OK) return r;
    if ((r = atI_reserve(&L, 4, 2)) != AT_OK) return atO_idfree(T, id), r;
    T->epoch += 1, L.epoch = T->epoch;
    atI_edges(&L, start + len, &se, &ee);
    a = &T->ids[id], a->start = se, a->end = ee;
    a->ns = (unsigned)ns & 0x1fffffffu, a->gravity = (unsigned)g;
    slot.id = id, slot.is_end = 0, atO_link(se, atO_side(g, 0), slot, a);
    slot.is_end = 1, atO_link(ee, atO_side(g, 1), slot, a);
    return *out = id, AT_OK;
}

AT_API int at_delanchor(at_Tree *T, at_Id id) {
    at_Anchor *a;
    at_Cursor  C;
    at_Edge   *se, *ee;
    at_Point   slot;
    if (!atO_idok(T, id)) return AT_ERRPARAM;
    if ((a = atO_anchor(T, id)) == NULL) return AT_ERRPARAM;
    T->epoch += 1;
    se = a->start, ee = a->end, slot.id = id, slot.is_end = 0;
    atO_unlink(T, se, atO_side(a->gravity, 0), slot);
    slot.is_end = 1;
    atO_unlink(T, ee, atO_side(a->gravity, 1), slot);
    atO_idfree(T, id);
    atK_fromedge(&C, T, se), atD_rmempty(&C);
    if (ee != se) atK_fromedge(&C, T, ee), atD_rmempty(&C);
    return AT_OK;
}

AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end) {
    at_Anchor *a;
    at_Cursor  C;
    if (start == NULL || end == NULL) return AT_ERRPARAM;
    if (!atO_idok(T, id)) return AT_ERRPARAM;
    if ((a = atO_anchor(T, id)) == NULL) return AT_ERRPARAM;
    atK_fromedge(&C, T, a->start), *start = atK_edgepos(&C);
    return atK_fromedge(&C, T, a->end), *end = atK_edgepos(&C), AT_OK;
}

AT_API int at_gravity(at_Tree *T, at_Id id) {
    at_Anchor *a;
    if (!atO_idok(T, id)) return AT_ERRPARAM;
    if ((a = atO_anchor(T, id)) == NULL) return AT_ERRPARAM;
    return (int)a->gravity;
}

AT_NS_END

#endif /* AT_IMPLEMENTATION */
