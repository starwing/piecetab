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

#define AT_ITER_TREE 0u /* normal tree iteration */
#define AT_ITER_DONE 1u /* iteration exhausted */

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
    unsigned side;     /* endpoint bucket: 0=left, 1=right, 2=restart */
    unsigned next_id;  /* next slot anchor id; 0 = bucket exhausted */
    unsigned next_end; /* next slot is_end tag */
    unsigned iter;     /* iteration phase: AT_ITER_TREE/DONE */

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
# define AT_FANOUT 32
#endif /* AT_FANOUT */

AT_STATIC_ASSERT(AT_FANOUT >= 4);

#define AT_ANCHOR_MAX ((size_t)1u << 31) /* at_Point.id is 31 bits */

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

/* clang-format off */
typedef struct at_Point {
    unsigned id : 31;    /* anchor id; 0 = empty slot */
    unsigned is_end : 1; /* 1 = END occurrence, 0 = START occurrence */
} at_Point;

typedef struct at_Anchor {
    struct at_Edge *start; /* start edge; NULL when dead */
    struct at_Edge *end;   /* end edge; NULL when dead */
    at_Point snext;        /* next START occurrence in its bucket */
    at_Point enext;        /* next END occurrence in its bucket */
    unsigned ns : 29;      /* namespace filter value */
    unsigned gravity : 2;  /* at_Gravity */
} at_Anchor;
/* clang-format on */

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
};

AT_STATIC_ASSERT(sizeof(at_Point) == 4);

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

static at_Edge *atO_newedge(at_Tree *T)
{
    at_Edge *e = (at_Edge *)atP_alloc(T->S, &T->S->edges);
    if (e == NULL) return NULL;
    return memset(e, 0, sizeof(*e)), e;
}
/* clang-format on */

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
    at_Point  *head = side ? &e->right : &e->left, p = *head, prev, next;
    at_Anchor *a, *pa;
    prev.id = 0, prev.is_end = 0;
    while (p.id != 0 && (p.id != slot.id || p.is_end != slot.is_end)) {
        a = &T->ids[p.id];
        prev = p;
        p = p.is_end ? a->enext : a->snext;
    }
    assert(p.id == slot.id && p.is_end == slot.is_end), a = &T->ids[p.id];
    next = p.is_end ? a->enext : a->snext;
    if (prev.id == 0) {
        *head = next;
        return;
    }
    pa = &T->ids[prev.id];
    *(prev.is_end ? &pa->enext : &pa->snext) = next;
}

static void atO_moveside(at_Tree *T, at_Edge *dst, at_Edge *src, int side) {
    at_Point  *head = side ? &src->right : &src->left, p = *head, next;
    at_Anchor *a;
    head->id = 0, head->is_end = 0;
    while (p.id != 0) {
        a = &T->ids[p.id];
        next = p.is_end ? a->enext : a->snext;
        *(p.is_end ? &a->end : &a->start) = dst;
        atO_link(dst, side, p, a);
        p = next;
    }
}

static void atO_clear(at_Tree *T) {
    if (T->ids == NULL) return;
    T->S->allocf(T->S->alloc_ud, T->ids, T->idcap * sizeof(at_Anchor), 0);
    T->ids = NULL, T->idcap = 0, T->idfree = 0;
}

static int atO_growids(at_Tree *T) {
    at_Anchor *anchors;
    size_t     oldcap = T->idcap, newcap = oldcap ? oldcap * 2 : 8;
    size_t     sz, i, from = oldcap ? oldcap : 1;
    if (newcap <= oldcap || newcap > AT_ANCHOR_MAX) return AT_ERRMEM;
    if (newcap > (size_t)-1 / sizeof(at_Anchor)) return AT_ERRMEM;
    sz = newcap * sizeof(at_Anchor);
    anchors = (at_Anchor *)T->S->allocf(T->S->alloc_ud, NULL, 0, sz);
    if (anchors == NULL) return AT_ERRMEM;
    memset(anchors, 0, sz);
    if (oldcap != 0) memcpy(anchors, T->ids, oldcap * sizeof(at_Anchor));
    for (i = newcap; i > from; --i) {
        at_Anchor *a = &anchors[i - 1];
        a->snext.id = (unsigned)T->idfree;
        a->snext.is_end = 0;
        T->idfree = (at_Id)(i - 1);
    }
    if (oldcap != 0)
        T->S->allocf(T->S->alloc_ud, T->ids, oldcap * sizeof(at_Anchor), 0);
    return T->ids = anchors, T->idcap = newcap, AT_OK;
}

static int atO_idalloc(at_Tree *T, at_Id *out) {
    at_Anchor *a;
    int        r;
    if (T->idfree == 0 && (r = atO_growids(T)) != AT_OK) return r;
    a = &T->ids[T->idfree];
    *out = T->idfree;
    T->idfree = a->snext.id;
    return a->start = NULL, a->end = NULL, AT_OK;
}

static void atO_idfree(at_Tree *T, at_Id id) {
    at_Anchor *a = &T->ids[id];
    a->snext.id = T->idfree, a->snext.is_end = 0;
    a->end = NULL;
    T->idfree = id;
}

static at_Anchor *atO_anchor(at_Tree *T, at_Id id) {
    at_Anchor *a;
    if (T == NULL || id == 0 || T->ids == NULL || id >= T->idcap) return NULL;
    return a = &T->ids[id], a->end == NULL ? NULL : a;
}

static size_t atO_edgepos(const at_Edge *e) {
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

static int atO_scan(at_Cursor *C, at_Edge *e, int ns, at_Id *out) {
    at_Tree   *T = C->tree;
    at_Point   p, next;
    at_Anchor *a;
    while (C->side < 2) {
        if (C->next_id == 0) {
            C->side += 1;
            if (C->side == 1) {
                C->next_id = e->right.id;
                C->next_end = e->right.is_end;
            }
            continue;
        }
        p.id = C->next_id, p.is_end = C->next_end;
        a = &T->ids[p.id];
        next = p.is_end ? a->enext : a->snext;
        C->next_id = next.id, C->next_end = next.is_end;
        if (!p.is_end && (ns == 0 || a->ns == (unsigned)ns))
            return *out = p.id, 1;
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

static at_Id atO_found(at_Tree *T, at_Id id, size_t *plen) {
    size_t start, end;
    int    r;
    if (plen == NULL) return id;
    start = end = 0;
    r = at_range(T, id, &start, &end), assert(r == AT_OK), (void)r;
    return *plen = start <= end ? end - start : 0, id;
}

static void atO_cursorstart(at_Cursor *C, at_Edge *e) {
    C->side = 0, C->next_id = e->left.id, C->next_end = e->left.is_end;
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

static void atN_addpath(at_Node *n, at_Delta db) {
    for (; n->parent != NULL; n = n->parent) n->parent->bytes[n->idx] += db;
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
    atO_clear(T);
    T->S->allocf(T->S->alloc_ud, T, sizeof(at_Tree), 0);
}

AT_API void at_reset(at_Tree *T) {
    if (T == NULL) return;
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

static int atO_findedge(at_Tree *T, size_t pos, at_Edge **out) {
    at_Cursor C;
    at_Node  *p;
    int       i, r;
    if (pos > T->bytes || (T->bytes == 0 && atN_cc(&T->root) == 0)) return 0;
    C.tree = T, r = at_locate(&C, pos), assert(r == AT_OK), (void)r;
    if (C.poff != 0) return 0;
    i = atK_idx(&C, p = atK_parent(&C, atK_levels(&C)), atK_levels(&C));
    return *out = atN_edge(p, i), 1;
}

static int atK_nextedge(at_Cursor *C) {
    at_Node *p, *q;
    int      l, i, cc, L = atK_levels(C);
    p = L ? *C->paths[L - 1] : &C->tree->root;
    i = atK_idx(C, p, L);
    cc = atN_cc(p);
    C->off += p->bytes[i];
    if (i + 1 < cc) return C->paths[L] = &p->children[i + 1], C->poff = 0, 1;
    for (l = L - 1; l >= 0; --l) {
        q = l ? *C->paths[l - 1] : &C->tree->root;
        i = atK_idx(C, q, l);
        cc = atN_cc(q);
        if (i + 1 < cc) {
            C->paths[l] = &q->children[i + 1];
            while (l < L) {
                p = *C->paths[l];
                ++l;
                C->paths[l] = &p->children[0];
            }
            return C->poff = 0, 1;
        }
    }
    return 0;
}

static void atO_startcurrent(at_Cursor *C) {
    at_Node *p;
    int      L, i;
    assert(atN_cc(&C->tree->root) != 0);
    L = C->tree->levels, p = L ? *C->paths[L - 1] : &C->tree->root;
    i = (int)(C->paths[L] - p->children);
    atO_cursorstart(C, atN_edge(p, i));
}

AT_API int at_locate(at_Cursor *C, size_t pos) {
    at_Tree *T;
    size_t   off = pos;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    T = C->tree, C->iter = AT_ITER_TREE;
    if (T->bytes == 0) {
        C->off = 0, C->poff = pos, C->paths[0] = T->root.children;
        if (atN_cc(&T->root) != 0) atO_startcurrent(C);
        return AT_OK;
    }
    if (pos >= T->bytes) {
        size_t vir = pos - T->bytes;
        atK_locend(C), C->poff += vir, atO_startcurrent(C);
        return AT_OK;
    }
    C->off = 0, atK_findleaf(C, 0, &off), C->poff = off;
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
    if (d == 0) return AT_OK;
    T = C->tree, off = at_offset(C), C->iter = AT_ITER_TREE;
    step = d < 0 ? (size_t)0 - (size_t)d : (size_t)d;
    if (T->bytes == 0) {
        C->off = 0;
        C->poff = d < 0 ? (step < off ? off - step : 0) : off + step;
        C->paths[0] = T->root.children;
    } else if (d < 0 && step > off) {
        atK_backwardoff(C, off);
    } else if (d < 0) {
        atK_backwardoff(C, step);
    } else if (off + step >= T->bytes) {
        atK_locend(C), C->poff += off + step - T->bytes;
    } else {
        atK_forwardoff(C, step);
    }
    if (atN_cc(&T->root) != 0) atO_startcurrent(C);
    return AT_OK;
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
    if (T->bytes == 0 && atN_cc(&T->root) == 0) return atO_done(C, plen);
    if (C->side >= 2) atO_startcurrent(C);
    for (;;) {
        L = atK_levels(C);
        p = L ? *C->paths[L - 1] : &T->root;
        i = atK_idx(C, p, L);
        b = p->bytes[i];
        if (C->poff > b) return atO_done(C, plen);
        if (C->poff > 0) {
            if (!atK_nextedge(C)) return atO_done(C, plen);
            atO_startcurrent(C);
            continue;
        }
        e = atN_edge(p, i);
        if (atO_scan(C, e, ns, &id)) return atO_found(T, id, plen);
        if (!atK_nextedge(C)) return atO_done(C, plen);
        atO_startcurrent(C);
    }
}

/* remove/stitch */

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
    at_Node  *p, ***cp = &C->paths[l];
    int       leaf = l == atK_levels(C) - 1;
    int       cL, cR, dn, r = 0, i = atK_idx(C, p = atK_parent(C, l), l);
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

/* clang-format off */
static at_Edge *atK_edge(at_Cursor *C)
{
    at_Node *p = atK_parent(C, atK_levels(C));
    return atN_edge(p, atK_idx(C, p, atK_levels(C)));
}
/* clang-format on */

static void atD_delsuccessor(at_Tree *T, at_Edge *prev, at_Cursor *S) {
    at_Node *leaf, *lp;
    at_Edge *e;
    int      i, l = atK_levels(S);
    size_t   delta;
    i = atK_idx(S, leaf = atK_parent(S, l), l);
    e = atN_edge(leaf, i), delta = leaf->bytes[i];
    assert(atO_empty(e));
    lp = prev->parent, lp->bytes[prev->idx] += delta;
    atN_addpath(lp, (at_Delta)delta);
    atN_addpath(leaf, -(at_Delta)delta);
    atN_removeslot(leaf, i, 1, 1), atP_free(&T->S->edges, e);
    assert(l == 0 || atN_cc(leaf) > 0);
    if (l && i >= atN_cc(leaf)) S->paths[l] = &leaf->children[atN_cc(leaf) - 1];
    if (l > 0 && atN_cc(leaf) < atN_min()) atD_rebalance(S, l - 1);
}

AT_STATIC int atD_deleteedge(at_Tree *T, size_t pos) {
    at_Cursor C, P, S;
    at_Edge  *e, *prev;
    at_Node  *leaf;
    int       i, l, r;
    if (T == NULL || atN_cc(&T->root) == 0 || pos > T->bytes)
        return AT_ERRPARAM;
    C.tree = T, r = at_locate(&C, pos), assert(r == AT_OK), (void)r;
    if (C.poff != 0) return AT_ERRPARAM;
    l = atK_levels(&C), leaf = atK_parent(&C, l);
    i = atK_idx(&C, leaf, l), e = atN_edge(leaf, i);
    assert(atO_empty(e));
    if (pos == 0) {
        assert(T->bytes == 0 && l == 0);
        return atN_removeslot(leaf, i, 1, 1), atP_free(&T->S->edges, e), AT_OK;
    }
    atK_backwardoff(&C, 1), prev = atK_edge(&C), pos = C.off;
    P.tree = T, r = at_locate(&P, pos), assert(r == AT_OK), (void)r;
    assert(P.poff == 0), S = P;
    r = atK_nextedge(&S), assert(r && atK_edge(&S) == e), (void)r;
    return atD_delsuccessor(T, prev, &S), AT_OK;
}

static void atD_dropempty(at_Tree *T, at_Edge *e) {
    size_t pos;
    int    r;
    if (!atO_empty(e)) return;
    pos = atO_edgepos(e);
    if (pos == 0 && T->bytes != 0) return;
    r = atD_deleteedge(T, pos), assert(r == AT_OK), (void)r;
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

static int atI_splitroot(at_Cursor *C) {
    at_Tree *T = C->tree;
    at_Node *r = &T->root, save = *r, *pp, *nw;
    int      i = atK_idx(C, r, 0), mid = atN_cc(&save) / 2;
    int      nc = atN_cc(&save) - mid, leaf = T->levels == 0;
    if (!(pp = (at_Node *)atP_alloc(T->S, &T->S->nodes))) return AT_ERRMEM;
    if (!(nw = (at_Node *)atP_alloc(T->S, &T->S->nodes)))
        return atP_free(&T->S->nodes, pp), AT_ERRMEM;
    *pp = save, atN_setcc(pp, mid);
    atN_copy(nw, 0, &save, mid, atN_setcc(nw, nc));
    atN_fixlinks(pp, 0, mid, leaf), atN_fixlinks(nw, 0, nc, leaf);
    atN_set(r, 0, pp, atN_sumbytes(pp, 0, mid));
    atN_set(r, 1, nw, atN_sumbytes(nw, 0, nc));
    atN_setcc(r, 2), T->levels += 1, assert(T->levels < AT_MAX_LEVEL);
    memmove(C->paths + 1, C->paths, (size_t)T->levels * sizeof(at_Node **));
    C->paths[0] = &r->children[i >= mid];
    C->paths[1] = &(*C->paths[0])->children[i < mid ? i : i - mid];
    return AT_OK;
}

static int atI_splitchild(at_Cursor *C, int l) {
    at_Tree *T = C->tree;
    at_Node *p = atK_parent(C, l), *nd = (at_Node *)*(C->paths[l]), *nw;
    int      i = atK_idx(C, p, l), cs, mid, r;
    mid = atN_cc(nd) / 2;
    if ((r = atI_splitnode(T->S, nd, T->levels - l - 1, &nw)) != AT_OK)
        return r;
    atN_makespace(p, i + 1, 1, 0);
    atN_set(p, i + 1, nw, atN_sumbytes(nw, 0, atN_cc(nw)));
    p->bytes[i] = atN_sumbytes(nd, 0, atN_cc(nd));
    if ((cs = atK_idx(C, nd, l + 1)) >= mid) {
        C->paths[l] = &p->children[i + 1];
        C->paths[l + 1] = &nw->children[cs - mid];
    }
    return AT_OK;
}

static int atI_makeroom(at_Cursor *C) {
    int l, r;
    for (l = atK_levels(C); l >= 0; --l)
        if (atN_cc(atK_parent(C, l)) < AT_FANOUT) break;
    if (l < 0) {
        if ((r = atI_splitroot(C)) != AT_OK) return r;
        l = 1;
    }
    for (; l < atK_levels(C); ++l) {
        if (atN_cc((at_Node *)*(C->paths[l])) < AT_FANOUT) continue;
        if ((r = atI_splitchild(C, l)) != AT_OK) return r;
    }
    return AT_OK;
}

static at_Edge *atI_insertat(at_Node *n, int l, size_t rem, at_Edge *e) {
    size_t rb;
    int    i, cc = atN_cc(n);
    if (l == 0) {
        for (i = 0; i < cc && rem >= n->bytes[i]; ++i) rem -= n->bytes[i];
        assert(i < cc && rem > 0 && rem < n->bytes[i]);
        rb = n->bytes[i] - rem, n->bytes[i] = rem;
        return atN_makespace(n, i + 1, 1, 1), atN_setleaf(n, i + 1, e, rb), e;
    }
    for (i = 0; i < cc && rem >= n->bytes[i]; ++i) rem -= n->bytes[i];
    assert(i < cc);
    return atI_insertat((at_Node *)n->children[i], l - 1, rem, e);
}

static void atI_appendslot(at_Node *n, int l, at_Edge *e) {
    int cc;
    for (; l > 0; --l) n = (at_Node *)n->children[atN_cc(n) - 1];
    cc = atN_cc(n);
    atN_makespace(n, cc, 1, 1), atN_setleaf(n, cc, e, 0);
}

static void atI_shift(at_Tree *T, size_t pos, at_Delta d) {
    at_Cursor C;
    at_Node  *leaf;
    int       i, l, r;
    assert(pos <= T->bytes && (T->bytes > 0 || atN_cc(&T->root) != 0));
    C.tree = T, r = at_locate(&C, pos), assert(r == AT_OK), (void)r;
    l = atK_levels(&C), i = atK_idx(&C, leaf = atK_parent(&C, l), l);
    leaf->bytes[i] += d, atM_up(&C, l - 1, d);
}

static int atI_emptyedge(at_Tree *T, size_t pos, at_Edge **out) {
    at_Edge *e = atO_newedge(T), *e0;
    if (e == NULL) return AT_ERRMEM;
    if (pos == 0)
        return atN_setleaf(&T->root, 0, e, 0), atN_setcc(&T->root, 1), *out = e,
                                                                       AT_OK;
    if ((e0 = e, e = atO_newedge(T)) == NULL)
        return atP_free(&T->S->edges, e0), AT_ERRMEM;
    atN_setleaf(&T->root, 0, e0, pos), atN_setleaf(&T->root, 1, e, 0);
    return atN_setcc(&T->root, 2), T->bytes = pos, *out = e, AT_OK;
}

AT_STATIC int atI_insertedge(at_Tree *T, size_t pos, at_Edge **out) {
    at_Cursor C;
    at_Edge  *e, *prev;
    int       r;
    if (T == NULL || out == NULL) return AT_ERRPARAM;
    if (atO_findedge(T, pos, &e)) return *out = e, AT_OK;
    if (atN_cc(&T->root) == 0) return atI_emptyedge(T, pos, out);
    if ((e = atO_newedge(T)) == NULL) return AT_ERRMEM;
    if (pos >= T->bytes) {
        C.tree = T, atK_locend(&C);
        if ((r = atI_makeroom(&C)) != AT_OK)
            return atP_free(&T->S->edges, e), r;
        prev = atK_edge(&C), atI_appendslot(&T->root, T->levels, e);
        if (pos > T->bytes) {
            size_t d = pos - T->bytes;
            prev->parent->bytes[prev->idx] += d;
            atN_addpath(prev->parent, (at_Delta)d), T->bytes += d;
        }
        return *out = e, AT_OK;
    }
    C.tree = T, r = at_locate(&C, pos), assert(r == AT_OK), (void)r;
    assert(C.poff > 0);
    if ((r = atI_makeroom(&C)) != AT_OK) return atP_free(&T->S->edges, e), r;
    return *out = atI_insertat(&T->root, T->levels, pos, e), AT_OK;
}
static int atO_edges(at_Tree *T, size_t s, size_t t, at_Edge **a, at_Edge **b) {
    int r;
    r = atP_reserve(T->S, &T->S->nodes, 4 * (T->levels + 2u) + 8u);
    if (r != AT_OK) return r;
    r = atP_reserve(T->S, &T->S->edges, t == s ? 2u : 4u);
    if (r != AT_OK) return r;
    if ((r = atI_insertedge(T, s, a)) != AT_OK) return r;
    if (t == s) return *b = *a, AT_OK;
    if ((r = atI_insertedge(T, t, b)) != AT_OK) return atD_dropempty(T, *a), r;
    return AT_OK;
}

static int atI_insertempty(at_Tree *T, size_t pos, size_t ins) {
    at_Edge *e = atO_newedge(T);
    if (e == NULL) return AT_ERRMEM;
    atN_setleaf(&T->root, 0, e, pos + ins), atN_setcc(&T->root, 1);
    return T->bytes = pos + ins, AT_OK;
}

static int atI_insert(at_Tree *T, size_t pos, size_t ins) {
    at_Edge *e, *ne;
    int      r;
    if (ins == 0) return AT_OK;
    if (atN_cc(&T->root) == 0) return atI_insertempty(T, pos, ins);
    if (pos > T->bytes) {
        atI_shift(T, T->bytes, (at_Delta)(pos - T->bytes + ins));
        return AT_OK;
    }
    if (!atO_findedge(T, pos, &e))
        return atI_shift(T, pos, (at_Delta)ins), AT_OK;
    atI_shift(T, pos, (at_Delta)ins);
    if (e->right.id == 0) {
        if (pos != 0 && atO_empty(e)) {
            r = atD_deleteedge(T, pos), assert(r == AT_OK), (void)r;
        }
        return AT_OK;
    }
    if ((r = atI_insertedge(T, pos + ins, &ne)) != AT_OK)
        return atI_shift(T, pos, -(at_Delta)ins), r;
    atO_moveside(T, ne, e, 1), atD_dropempty(T, e);
    return AT_OK;
}

/* remove */

static void atD_collapse(at_Tree *T, size_t L, size_t bR) {
    at_Cursor C;
    at_Node  *leaf;
    size_t    old;
    int       i, l, r;
    C.tree = T, r = at_locate(&C, L), assert(r == AT_OK), (void)r;
    l = atK_levels(&C), leaf = atK_parent(&C, l), i = atK_idx(&C, leaf, l);
    old = leaf->bytes[i], leaf->bytes[i] = bR;
    atM_up(&C, l - 1, (at_Delta)bR - (at_Delta)old);
}

static int atD_prep(at_Tree *T, size_t L, size_t R, at_Edge **le, size_t *bR) {
    at_Edge *re;
    int      r, d;
    *bR = 0;
    if ((r = atI_insertedge(T, L, le)) != AT_OK) return r;
    if (R == T->bytes) return AT_OK;
    if ((r = atI_insertedge(T, R, &re)) != AT_OK) {
        if (L != 0 && atO_empty(*le)) {
            d = atD_deleteedge(T, L), assert(d == AT_OK), (void)d;
        }
        return r;
    }
    return *bR = re->parent->bytes[re->idx], AT_OK;
}

AT_STATIC int atD_prepwindow(at_Tree *T, size_t L, size_t R, at_Edge **le) {
    size_t bR;
    return atD_prep(T, L, R, le, &bR);
}

static int atD_remove(at_Cursor *C, size_t del) {
    at_Tree  *T = C->tree;
    at_Cursor S;
    at_Edge  *le;
    size_t    L = at_offset(C), R, bR;
    int       r;
    if (del == 0 || L >= T->bytes) return AT_OK;
    R = del > (size_t)-1 - L ? (size_t)-1 : L + del;
    if (R > T->bytes) R = T->bytes;
    del = R - L;
    if ((r = atD_prep(T, L, R, &le, &bR)) != AT_OK) return r;
    S.tree = T, r = at_locate(&S, L), assert(r == AT_OK), (void)r;
    while (atK_nextedge(&S) && S.off <= R) {
        at_Edge *e = atK_edge(&S);
        atO_moveside(T, le, e, 0), atO_moveside(T, le, e, 1);
    }
    for (;;) {
        S.tree = T, r = at_locate(&S, L), assert(r == AT_OK), (void)r;
        if (!atK_nextedge(&S) || S.off > R) break;
        atD_delsuccessor(T, le, &S);
    }
    return atD_collapse(T, L, bR), atD_dropempty(T, le), AT_OK;
}

/* APIs */

AT_API int at_splice(at_Cursor *C, size_t del, size_t ins) {
    at_Tree *T;
    size_t   L;
    int      r;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    T = C->tree, L = at_offset(C);
    if (del == 0 && ins == 0) return AT_OK;
    if (ins > (size_t)-1 - (L > T->bytes ? L : T->bytes)) return AT_ERRPARAM;
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
    at_Point   slot;
    at_Id      id;
    size_t     start, end;
    int        sl, el, r;
    if (C == NULL || C->tree == NULL || out == NULL) return AT_ERRPARAM;
    if (g < AT_GROW || g > AT_LOCK) return AT_ERRPARAM;
    T = C->tree, start = at_offset(C);
    if (len > (size_t)-1 - start) return AT_ERRPARAM;
    end = start + len;
    sl = atO_side(g, 0), el = atO_side(g, 1);
    if ((r = atO_idalloc(T, &id)) != AT_OK) return r;
    if ((r = atO_edges(T, start, end, &se, &ee)) != AT_OK)
        return atO_idfree(T, id), r;
    a = &T->ids[id], a->start = se, a->end = ee;
    a->ns = (unsigned)ns & 0x1fffffffu, a->gravity = (unsigned)g;
    slot.id = id, slot.is_end = 0;
    atO_link(se, sl, slot, a);
    slot.is_end = 1;
    atO_link(ee, el, slot, a);
    return *out = id, AT_OK;
}

AT_API int at_delanchor(at_Tree *T, at_Id id) {
    at_Anchor *a;
    at_Edge   *se, *ee;
    at_Point   slot;
    if ((a = atO_anchor(T, id)) == NULL) return AT_ERRPARAM;
    se = a->start, ee = a->end;
    slot.id = id, slot.is_end = 0;
    atO_unlink(T, se, atO_side(a->gravity, 0), slot);
    slot.is_end = 1;
    atO_unlink(T, ee, atO_side(a->gravity, 1), slot);
    atD_dropempty(T, se);
    if (ee != se) atD_dropempty(T, ee);
    return atO_idfree(T, id), AT_OK;
}

AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end) {
    at_Anchor *a;
    if (start == NULL || end == NULL) return AT_ERRPARAM;
    if ((a = atO_anchor(T, id)) == NULL) return AT_ERRPARAM;
    *start = atO_edgepos(a->start), *end = atO_edgepos(a->end);
    return AT_OK;
}

AT_API int at_gravity(at_Tree *T, at_Id id) {
    at_Anchor *a;
    if ((a = atO_anchor(T, id)) == NULL) return AT_ERRPARAM;
    return (int)a->gravity;
}

AT_NS_END

#endif /* AT_IMPLEMENTATION */
