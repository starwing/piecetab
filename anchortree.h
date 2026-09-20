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

#define AT_OK          (0)  /* No error */
#define AT_ERRPARAM    (-1) /* Invalid parameter */
#define AT_ERRMEM      (-2) /* Memory allocation failed */
#define AT_ESTALE      (-3) /* Cursor is stale; seek again */
#define AT_ERROVERFLOW (-4) /* size_t coordinate/byte addition overflow */

#define AT_NONE   0 /* End-of-iteration; anchor id 0 is never allocated */
#define AT_MAX_NS ((1u << 29) - 1) /* largest namespace filter value */

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
typedef struct at_Point  at_Point;

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

/* anchors */
AT_API int at_delanchor(at_Tree *T, at_Id id);
AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end);
AT_API int at_gravity(at_Tree *T, at_Id id);

/* cursor */

/* constructor */
AT_API int at_seek(at_Cursor *C, at_Tree *T, size_t pos);

/* navigation */
AT_API int   at_locate(at_Cursor *C, size_t pos);
AT_API int   at_advance(at_Cursor *C, at_Delta d);
AT_API at_Id at_anchor(at_Cursor *C, int *pns, size_t *plen);
AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen);

/* anchors creation */
AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out);

/* text modification */
AT_API int at_remove(at_Cursor *C, size_t len);
AT_API int at_insert(at_Cursor *C, size_t ins);
AT_API int at_append(at_Cursor *C, size_t ins);
AT_API int at_splice(at_Cursor *C, size_t del, size_t ins);

/* structure */

#ifndef AT_MAX_LEVEL
# define AT_MAX_LEVEL 16 /* safe for AT_FANOUT=32; see docs/max_levels.md */
#endif

/* clang-format off */
struct at_Point { unsigned id : 31, is_end : 1; };
/* clang-format on */

struct at_Cursor {
    at_Tree *tree;  /* tree under navigation */
    size_t   off;   /* start position of the current span */
    size_t   poff;  /* offset within the current span */
    at_Point cur;   /* last consumed slot */
    unsigned epoch; /* tree generation captured by seek/locate */

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
AT_STATIC_ASSERT(AT_FANOUT <= 0xffffu);

#define AT_ANCHOR_MAX (((size_t)1u << 31) - 1) /* largest anchor id */

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

typedef struct at_Anchor {
    struct at_Edge *start;           /* start edge; NULL when dead */
    struct at_Edge *end;             /* end edge; NULL when dead */
    at_Point        snext;           /* next START occurrence in its bucket */
    at_Point        enext;           /* next END occurrence in its bucket */
    unsigned        ns : 29;         /* namespace filter value */
    unsigned        start_right : 1; /* 1 = start occurrence in right bucket */
    unsigned        end_right : 1;   /* 1 = end occurrence in right bucket */
} at_Anchor;

typedef struct at_Edge {
    at_Node *parent; /* leaf node holding this edge */
    at_Index idx;    /* slot index in parent */
    at_Point left;   /* anchor occurrences on the LEFT side */
    at_Point right;  /* anchor occurrences on the RIGHT side */
} at_Edge;

struct at_State {
    void     *ud;               /* allocator user data */
    at_Alloc *allocf;           /* memory allocator */
    at_Pool   nodes;            /* pool for at_Node objects */
    at_Pool   edges;            /* pool for at_Edge objects */
    at_Node   rt[AT_MAX_LEVEL]; /* scratch roots for cutrange/stitch */
};

struct at_Tree {
    at_Node    root;    /* embedded root node */
    size_t     bytes;   /* position of the tail edge or last span end */
    at_State  *S;       /* owning state */
    at_Anchor *anchors; /* dense anchor array; id 0 is reserved */
    at_Id      idmax;   /* allocated capacity of anchors */
    at_Id      idfree;  /* freelist head for anchor anchors */
    unsigned   epoch;   /* structural generation; bumped on tree mutation */
    int        levels;  /* tree height, 0 = leaf-only root */
    at_Edge   *seam;    /* survivor edge for purge link moves; NULL = free */
};

AT_STATIC_ASSERT(sizeof(at_Point) == 4);
AT_STATIC_ASSERT(sizeof(at_Node) < AT_PAGE_SIZE / 2);
AT_STATIC_ASSERT(sizeof(at_Edge) < AT_PAGE_SIZE / 2);

/* mempool */

static void atP_init(at_Pool *p, size_t obj_size) {
    memset(p, 0, sizeof(at_Pool)), p->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < AT_PAGE_SIZE / 2);
}

static void atP_destroy(at_State *S, at_Pool *p) {
    void *next, *page = p->pages;
    for (; page; page = next) {
        next = *(void **)((char *)page + AT_PAGE_SIZE - sizeof(void *));
        S->allocf(S->ud, page, AT_PAGE_SIZE, 0);
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
    page = (char *)S->allocf(S->ud, NULL, 0, AT_PAGE_SIZE);
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

#define atA_slot(T, id)   (&(T)->anchors[(id) - 1])
#define atE_head(side, e) ((side) ? &(e)->right : &(e)->left)
#define atE_next(p, a)    ((p).is_end ? (a)->enext : (a)->snext)

/* clang-format off */
static int atE_empty(at_Edge *e)
{ return e->left.id == 0 && e->right.id == 0; }

static at_Point atU_point(unsigned id, unsigned is_end)
{ at_Point p; p.id = id, p.is_end = is_end; return p; }

static at_Id atK_none(size_t *plen)
{ return (void)(plen && (*plen = 0)), AT_NONE; }
/* clang-format on */

static at_Edge *atE_new(at_Tree *T) {
    at_Edge *e = (at_Edge *)atP_ralloc(&T->S->edges);
    return assert(e), memset(e, 0, sizeof(*e)), e;
}

static void atE_unlink(at_Tree *T, at_Point *h, at_Point slot) {
    at_Point   prev, next, p = *h;
    at_Anchor *a, *pa;
    prev = atU_point(0, 0);
    while (p.id != 0 && (p.id != slot.id || p.is_end != slot.is_end))
        a = atA_slot(T, p.id), prev = p, p = atE_next(p, a);
    assert(p.id == slot.id && p.is_end == slot.is_end);
    a = atA_slot(T, p.id), next = atE_next(p, a);
    if (prev.id == 0)
        *h = next;
    else {
        pa = atA_slot(T, prev.id);
        *(prev.is_end ? &pa->enext : &pa->snext) = next;
    }
}

static void atE_mvlink(at_Tree *T, at_Edge *dst, at_Edge *src, int side) {
    at_Point   next, p, t, *sh = atE_head(side, src), *dh = atE_head(side, dst);
    at_Anchor *a;
    p = *sh, *sh = atU_point(0, 0);
    if (p.id == 0) return;
    for (next = p; next.id != 0; next = atE_next(t, a)) {
        t = next, a = atA_slot(T, t.id);
        *(t.is_end ? &a->end : &a->start) = dst;
    }
    *(t.is_end ? &a->enext : &a->snext) = *dh, *dh = p;
}

static void atA_clear(at_Tree *T) {
    if (T->anchors == NULL) return;
    T->S->allocf(T->S->ud, T->anchors, T->idmax * sizeof(at_Anchor), 0);
    T->anchors = NULL, T->idmax = 0, T->idfree = 0;
}

static at_Id atA_alloc(at_Tree *T) {
    at_Id      i, id = T->idfree;
    at_Anchor *as;
    size_t     nc;
    if (id) return (T->idfree = *(at_Id *)atA_slot(T, id)), id;
    id = T->idmax + 1;
    nc = T->idmax ? T->idmax + (T->idmax >> 1) : 8;
    if (nc <= T->idmax || nc > AT_ANCHOR_MAX) return 0;
    as = (at_Anchor *)T->S->allocf(
            T->S->ud, T->anchors, T->idmax * sizeof(at_Anchor),
            nc * sizeof(at_Anchor));
    if (as == NULL) return 0;
    for (i = id + 1; i < (at_Id)nc; ++i) *(at_Id *)&as[i - 1] = (at_Id)(i + 1);
    *(at_Id *)&as[i - 1] = T->idfree, T->idfree = (at_Id)(id + 1);
    return T->anchors = as, T->idmax = (at_Id)nc, id;
}

static void atA_free(at_Tree *T, at_Id id) {
    at_Anchor *a = atA_slot(T, id);
    *(at_Id *)a = T->idfree, a->end = NULL, T->idfree = id;
}

static at_Anchor *atA_fromid(at_Tree *T, at_Id id) {
    at_Anchor *a;
    if (T == NULL || id == 0 || id > T->idmax) return NULL;
    return (a = atA_slot(T, id))->end ? a : NULL;
}

static at_Id atE_nextstart(at_Tree *T, at_Point *p, int ns) {
    at_Anchor *a;
    for (; p->id != 0; *p = atE_next(*p, a)) {
        a = atA_slot(T, p->id);
        if (!p->is_end && (ns == 0 || a->ns == (unsigned)ns)) return p->id;
    }
    return AT_NONE;
}

static int atU_reserve(at_Cursor *C, size_t edges, size_t pass, size_t extra) {
    size_t nodes = pass * (size_t)C->tree->levels + extra;
    int    r = atP_reserve(C->tree->S, &C->tree->S->nodes, nodes);
    if (r != AT_OK) return r;
    return atP_reserve(C->tree->S, &C->tree->S->edges, edges);
}

/* node operations */

#define atN_cc(n)            ((int)(n)->child_count)
#define atN_setcc(n, v)      ((n)->child_count = (at_Index)(v))
#define atN_edge(n, i)       ((at_Edge *)(n)->children[i])
#define atN_setedge(n, i, e) ((n)->children[i] = (at_Node *)(e))

#define atK_levels(C)    ((C)->tree->levels)
#define atK_bytes(C)     ((C)->tree->bytes)
#define atK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define atK_idx(C, p, l) ((int)((C)->paths[(l)] - (p)->children))

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
    int j;
    for (j = i; j < i + k; ++j) {
        if (leaf) {
            at_Edge *e = atN_edge(n, j);
            e->parent = n, e->idx = (at_Index)j;
        } else {
            at_Node *c = n->children[j];
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

/* clang-format off */
static size_t atN_sumbytes(const at_Node *n, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += n->bytes[i]; return s; }

static void atN_setleaf(at_Node *n, int i, at_Edge *e, size_t b)
{ atN_setedge(n, i, e), n->bytes[i] = b; e->parent = n, e->idx = (at_Index)i; }

static void atN_set(at_Node *n, int i, at_Node *c, size_t b)
{ n->children[i] = c, n->bytes[i] = b; c->parent = n, c->idx = (at_Index)i; }

static void atK_rewind(at_Cursor *C)
{ C->cur = atU_point(0, atN_cc(&C->tree->root) ? 0 : 1); }
/* clang-format on */

static void atN_purge(at_Tree *T, at_Node *p, int k, int s, int e) {
    int i;
    assert(s <= e && e <= atN_cc(p));
    if (k == 0) {
        for (i = s; i < e; ++i) {
            at_Edge *edge = atN_edge(p, i);
            assert(edge != T->seam);
            if (T->seam != NULL) {
                atE_mvlink(T, T->seam, edge, 0);
                atE_mvlink(T, T->seam, edge, 1);
            }
            atP_free(&T->S->edges, edge);
        }
        return;
    }
    for (i = s; i < e; ++i) {
        at_Node *c = p->children[i];
        atN_purge(T, c, k - 1, 0, atN_cc(c)), atP_free(&T->S->nodes, c);
    }
}

static void atN_remove(at_Tree *T, at_Node *p, int k, int s, int e) {
    int cc = atN_cc(p);
    assert(s <= e && e <= cc), atN_purge(T, p, k, s, e);
    atN_move(p, s, e, cc - e), atN_setcc(p, cc - (e - s));
    atN_fixlinks(p, s, cc - e, k == 0);
}

static void atM_up(at_Cursor *C, int l, at_Delta db) {
    at_Node *p;
    for (; l >= 0; --l) p = atK_parent(C, l), p->bytes[atK_idx(C, p, l)] += db;
    C->tree->bytes += db;
}

static int atA_side(at_Tree *T, at_Point p) {
    at_Anchor *a = atA_slot(T, p.id);
    return p.is_end ? a->end_right : a->start_right;
}

/* cursor navigation */

static int atK_locend(at_Cursor *C) {
    at_Node *n = &C->tree->root;
    int      l;
    if (atK_levels(C) == 0 && atN_cc(n) == 0) {
        C->paths[0] = n->children, C->off = 0, C->poff = 0;
        return atK_rewind(C), 0;
    }
    for (l = 0; l < atK_levels(C); ++l)
        n = *(C->paths[l] = &n->children[atN_cc(n) - 1]);
    assert(atN_cc(n)), C->paths[l] = &n->children[atN_cc(n) - 1];
    C->poff = n->bytes[atN_cc(n) - 1];
    return (C->off = atK_bytes(C) - C->poff), atK_rewind(C), 1;
}

static void atK_findleaf(at_Cursor *C, int l, size_t *poff) {
    for (; l <= atK_levels(C); ++l) {
        at_Node *p = atK_parent(C, l);
        int      i;
        for (i = 0; i < atN_cc(p) && *poff >= p->bytes[i]; ++i)
            *poff -= p->bytes[i], C->off += p->bytes[i];
        assert(i < atN_cc(p)), C->paths[l] = &p->children[i];
    }
    atK_rewind(C);
}

static int atK_forwardoff(at_Cursor *C, size_t d) {
    at_Node *p;
    int      l, i = atK_idx(C, p = atK_parent(C, atK_levels(C)), atK_levels(C));
    size_t   in, off = at_offset(C);
    if (off + d >= atK_bytes(C)) {
        assert(off <= (size_t)-1 - d);
        atK_locend(C), C->poff += off + d - atK_bytes(C);
        return 1;
    }
    in = p->bytes[i] - C->poff;
    if (d < in) return C->poff += d, atK_rewind(C), 0;
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
    if (d <= C->poff) return C->poff -= d, atK_rewind(C), 0;
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
    at_Node *p;
    int      l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l);
    size_t   bc;
    if (C->poff >= p->bytes[i]) return 0;
    bc = p->bytes[i];
    for (; (i += 1) >= atN_cc(p); i = atK_idx(C, p = atK_parent(C, l), l))
        if (--l < 0) return atK_locend(C), 0;
    C->paths[l] = &p->children[i];
    for (; ++l <= atK_levels(C); C->paths[l] = &p->children[0])
        p = atK_parent(C, l);
    return (C->off += bc, C->poff = 0), 1;
}

static at_Edge *atK_edge(at_Cursor *C) {
    at_Node *p = atK_parent(C, atK_levels(C));
    return atN_edge(p, atK_idx(C, p, atK_levels(C)));
}

static size_t atK_fromedge(at_Cursor *C, at_Tree *T, at_Edge *e) {
    at_Node *p, *n = e->parent;
    size_t   pos = atN_sumbytes(n, 0, e->idx);
    int      l;
    C->tree = T, C->poff = 0, C->epoch = T->epoch, C->cur = atU_point(0, 0);
    C->paths[T->levels] = &n->children[e->idx];
    for (l = T->levels - 1; l >= 0; --l) {
        p = n->parent, C->paths[l] = &p->children[n->idx];
        pos += atN_sumbytes(p, 0, n->idx), n = p;
    }
    return C->off = pos;
}

static at_Id atK_nextstart(at_Cursor *C, int ns) {
    at_Point q = atK_edge(C)->left;
    int      side = 0;
    assert(!(C->cur.id == 0 && C->cur.is_end));
    if (C->cur.id == 0 && C->poff > 0) return AT_NONE;
    if (C->cur.id != 0) {
        at_Anchor *a = atA_slot(C->tree, C->cur.id);
        at_Edge   *e = (q = C->cur, q.is_end ? a->end : a->start);
        side = (int)(q.is_end ? a->end_right : a->start_right);
        if (atK_edge(C) != e) atK_fromedge(C, C->tree, e), C->cur = q;
        q = atE_next(q, a);
    }
    for (; side < 2; ++side) {
        if (atE_nextstart(C->tree, &q, ns) != AT_NONE) return C->cur = q, q.id;
        if (side == 0) q = atK_edge(C)->right;
    }
    return C->cur = atU_point(0, 0), AT_NONE;
}

/* remove */

static void atE_resize(at_Cursor *C, at_Delta d) {
    int      l = atK_levels(C);
    at_Node *p = atK_parent(C, l);
    p->bytes[atK_idx(C, p, l)] += d, atM_up(C, l - 1, d);
}

static void atD_cutrange(at_Cursor *L, at_Cursor *R, at_Node *rt, int fl) {
    int      i, k, kl, cc, l = atK_levels(L);
    at_Delta db = 0;
    at_Node *p;
    for (kl = l; kl > fl; --kl) {
        i = atK_idx(L, p = atK_parent(L, kl), kl), cc = atN_cc(p);
        p->bytes[i] -= db, db += (at_Delta)atN_sumbytes(p, i + 1, cc);
        k = l - kl, atN_remove(L->tree, p, k, i + 1, cc);
        i = atK_idx(R, p = atK_parent(R, kl), kl), cc = atN_cc(p);
        i += (k != 0);
        atN_copy(&rt[k], 0, p, i, atN_setcc(&rt[k], cc - i));
        atN_purge(L->tree, p, k, 0, i), atN_setcc(p, 0);
    }
    i = atK_idx(R, p = atK_parent(R, fl), fl), cc = atN_cc(p);
    k = l - fl, i += (k != 0);
    atN_copy(&rt[k], 0, p, i, atN_setcc(&rt[k], cc - i));
    atN_setcc(p, i), i = atK_idx(L, p, fl);
    p->bytes[i] -= db, db += (at_Delta)atN_sumbytes(p, i + 1, cc);
    atM_up(L, fl - 1, -db), atN_remove(L->tree, p, k, i + 1, atN_cc(p));
}

static void atD_mergeleaf(at_Cursor *C) {
    at_Node *p = atK_parent(C, atK_levels(C));
    int      cc = atN_cc(p), l = atK_levels(C);
    assert(cc > 0);
    C->off += C->poff, C->poff = 0;
    C->paths[l] = &p->children[cc];
}

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
    int       cL, cR, dn, r = 0, i = atK_idx(C, p = atK_parent(C, l), l);
    int       leaf = l == atK_levels(C) - 1;
    at_Node **ns = &p->children[i];
    at_Delta  ds;
    if (assert(atN_cc(p) > 1), atN_cc(ns[0]) > AT_FANOUT / 2) return 0;
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
    assert(l == 0 || l < atK_levels(C));
    for (; l > 0; --l) {
        p = atK_parent(C, l);
        if (atN_cc(p->children[atK_idx(C, p, l)]) >= AT_FANOUT / 2) return;
        if (assert(atN_cc(p) > 1), !atD_foldnode(C, 0, l)) return;
    }
    if (atK_levels(C) > 0) { /* fold the root children */
        int i = atK_idx(C, p = &C->tree->root, 0);
        if (atN_cc(p->children[i]) < AT_FANOUT / 2 && atN_cc(p) >= 2)
            atD_foldnode(C, 0, 0);
    }
    while (atK_levels(C) && atN_cc(&C->tree->root) == 1) {
        int cc, nl, i = atK_idx(C, p = atK_parent(C, 1), 1);
        cc = atN_cc(p), nl = atK_levels(C) - 1;
        C->tree->root = *p, C->tree->root.parent = NULL;
        atP_free(&C->tree->S->nodes, p);
        C->tree->levels = nl, C->paths[0] += i;
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

static void atD_makechain(at_Cursor *C, int from, int to) {
    at_Node *nn;
    int      l;
    assert(from >= 0 && from < to);
    assert(C->tree->S->nodes.freed_obj >= (size_t)(to - from));
    for (l = from; l < to; ++l) {
        at_Node *p = atK_parent(C, l);
        nn = (at_Node *)atP_ralloc(&C->tree->S->nodes);
        memset(nn, 0, sizeof(at_Node));
        nn->parent = p, nn->idx = (at_Index)atN_cc(p);
        p->bytes[atN_cc(p)] = 0;
        C->paths[l] = &p->children[atN_cc(p)];
        p->children[atN_cc(p)] = nn, p->child_count += 1;
    }
    C->paths[to] = &nn->children[0];
}

static void atD_findroom(at_Cursor *C, int l) {
    int      fl, i = 0;
    at_Node *p = NULL;
    for (fl = l - 1; fl >= 0; --fl)
        if ((i = atK_idx(C, p = atK_parent(C, fl), fl)) < AT_FANOUT - 1) break;
    assert(fl >= 0 && atN_cc(p) - i - 1 == 0);
    atD_makechain(C, fl, l);
}

static void atD_stitchnode(at_Cursor *L, at_Node *rt) {
    int      k, i, d = 0, l = atK_levels(L);
    at_Delta db = 0;
    for (k = 0; k <= atK_levels(L); ++k) {
        at_Node *p, *r = &rt[k];
        int      m, fl, cc, kl = atK_levels(L) - k, rtcc = atN_cc(r);
        atN_setcc(r, 0), i = atK_idx(L, p = atK_parent(L, kl), kl);
        if (i < atN_cc(p)) p->bytes[i] += db;
        m = rtcc < AT_FANOUT - atN_cc(p) ? rtcc : AT_FANOUT - atN_cc(p);
        if (m > 0) {
            cc = atN_cc(p), atN_copy(p, cc, r, 0, m), atN_setcc(p, cc + m);
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
        atD_findroom(L, l), p = atK_parent(L, l);
        atN_copy(p, 0, r, m, atN_setcc(p, rtcc - m));
        atN_fixlinks(p, 0, rtcc - m, k == 0);
        db += (at_Delta)atN_sumbytes(r, m, rtcc);
    }
}

static void atD_stitch(at_Cursor *L, at_Node *rt) {
    int      cc, l = atK_levels(L);
    at_Node *p = atK_parent(L, l);
    assert(L->tree->S->nodes.freed_obj >= (size_t)atK_levels(L));
    if ((cc = atN_cc(p)) && p->bytes[cc - 1] == 0)
        atN_purge(L->tree, p, 0, cc - 1, cc), atN_setcc(p, cc - 1), cc -= 1;
    if (cc && atN_cc(rt)) atD_mergeleaf(L);
    atD_stitchnode(L, rt), atD_rebalance(L, 0);
    l = atK_levels(L), cc = atN_cc(p = atK_parent(L, l));
    assert(cc != 0 && atK_idx(L, p, l) < cc);
}

static void atD_rmrange(at_Cursor *L, at_Cursor *R, int fl) {
    at_Node *p, *rt = L->tree->S->rt;
    at_Point q, c = atU_point(0, 0);
    int      k, l, i;
    if (L->poff == 0 && L->cur.id != 0 && (c = L->cur, atA_side(L->tree, c))) {
        c = atU_point(0, 0), q = atK_edge(L)->left;
        while (q.id != 0) c = q, q = atE_next(q, atA_slot(L->tree, q.id));
    }
    for (k = 0; k < AT_MAX_LEVEL; ++k) atN_setcc(&rt[k], 0);
    l = atK_levels(L), i = atK_idx(L, p = atK_parent(L, l), l);
    atE_resize(L, -(at_Delta)(p->bytes[i] - L->poff));
    l = atK_levels(R), i = atK_idx(R, p = atK_parent(R, l), l);
    atE_resize(R, -(at_Delta)R->poff), R->poff = 0;
    atD_cutrange(L, R, rt, fl), atD_stitch(L, rt), L->cur = c;
}

static void atD_rmleaf(at_Cursor *C, size_t del) {
    at_Node *p;
    int      l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l);
    int      cc = atN_cc(p), oc = cc;
    assert(C->poff + del <= p->bytes[i]);
    if (C->poff > 0 || C->poff + del < p->bytes[i])
        p->bytes[i] -= del;
    else if (assert(i + 1 == cc), atE_empty(atN_edge(p, i)))
        assert(i == 0), atN_remove(C->tree, p, 0, i, i + 1), cc -= 1;
    else
        p->bytes[i] = 0;
    assert(cc == 0 || i < cc), atM_up(C, l - 1, -(at_Delta)del);
    if (cc == 0)
        C->paths[l] = &p->children[0], C->off = 0, C->poff = 0,
        C->cur = atU_point(0, 1);
    if (cc < oc) atD_rebalance(C, l - 1);
}

static int atD_diverlevel(at_Cursor *L, at_Cursor *R) {
    int l;
    for (l = 0; l < atK_levels(L) && L->paths[l] == R->paths[l]; ++l) continue;
    return l + (l == atK_levels(L) && L->paths[l] == R->paths[l]);
}

static int atD_remove(at_Cursor *C, size_t del) {
    size_t    pos = at_offset(C);
    int       fl, r;
    at_Cursor R;
    if (pos >= atK_bytes(C)) return AT_OK;
    if (assert(del > 0), del > atK_bytes(C) - pos) del = atK_bytes(C) - pos;
    if ((r = atU_reserve(C, 0, 1, 0)) != AT_OK) return r;
    R = *C, atK_forwardoff(&R, del);
    fl = atD_diverlevel(C, &R);
    if (fl <= atK_levels(C)) {
        C->tree->seam = atK_edge(&R);
        r = (at_offset(&R) == atK_bytes(C));
        assert(C->tree->seam != atK_edge(C));
        assert(r || C->tree->seam->parent->bytes[C->tree->seam->idx] > 0);
        return atD_rmrange(C, &R, fl), C->tree->seam = NULL, AT_OK;
    }
    return atD_rmleaf(C, del), C->tree->seam = NULL, AT_OK;
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

static void atE_purge(at_Tree *T, at_Edge *e) {
    at_Cursor C, P;
    at_Node  *p;
    at_Edge  *prev;
    int       l, i, cc;
    size_t    b;
    assert(T->seam == NULL);
    if (!atE_empty(e)) return;
    if (atK_fromedge(&C, T, e) == 0 && T->bytes != 0) return;
    l = atK_levels(&C), i = atK_idx(&C, p = atK_parent(&C, l), l);
    b = p->bytes[i], prev = atK_prevedge(&C);
    if (prev == NULL) {
        assert(T->bytes == 0 && b == 0), atN_remove(T, p, 0, i, i + 1);
        return;
    }
    atN_remove(T, p, 0, i, i + 1), cc = atN_cc(p);
    C.paths[l] = cc ? &p->children[i < cc ? i : cc - 1] : p->children;
    atK_fromedge(&P, T, prev), atE_resize(&P, (at_Delta)b);
    atM_up(&C, l - 1, -(at_Delta)b);
    if (cc < AT_FANOUT / 2 && l > 0) atD_rebalance(&C, l - 1);
}

/* insertion */

static void atI_splitroot(at_Cursor *C) {
    at_Node *pp, *nw, *r = &C->tree->root, save = *r;
    int      i = atK_idx(C, r, 0), mid = atN_cc(&save) / 2;
    int      nc = atN_cc(&save) - mid, leaf = (atK_levels(C) == 0);
    pp = (at_Node *)atP_ralloc(&C->tree->S->nodes);
    nw = (at_Node *)atP_ralloc(&C->tree->S->nodes);
    *pp = save, atN_setcc(pp, mid);
    atN_copy(nw, 0, &save, mid, atN_setcc(nw, nc));
    atN_fixlinks(pp, 0, mid, leaf), atN_fixlinks(nw, 0, nc, leaf);
    atN_set(r, 0, pp, atN_sumbytes(pp, 0, mid));
    atN_set(r, 1, nw, atN_sumbytes(nw, 0, nc));
    atN_setcc(r, 2), ++C->tree->levels, assert(atK_levels(C) < AT_MAX_LEVEL);
    memmove(C->paths + 1, C->paths, atK_levels(C) * sizeof(at_Node **));
    C->paths[0] = &r->children[i >= mid];
    C->paths[1] = &(*C->paths[0])->children[i < mid ? i : i - mid];
}

static void atI_splitchild(at_Cursor *C, int l) {
    at_Node *nw, *p, *nd;
    int      cs, i, mid, nc;
    i = atK_idx(C, p = atK_parent(C, l), l);
    nd = (at_Node *)*(C->paths[l]);
    mid = atN_cc(nd) / 2, nc = atN_cc(nd) - mid;
    assert(mid >= AT_FANOUT / 2 && nc >= AT_FANOUT / 2);
    nw = (at_Node *)atP_ralloc(&C->tree->S->nodes);
    atN_setcc(nw, nc), nw->parent = nd->parent, nw->idx = 0;
    atN_copy(nw, 0, nd, mid, nc);
    atN_fixlinks(nw, 0, nc, atK_levels(C) - l - 1 == 0);
    atN_setcc(nd, mid), atN_makespace(p, i + 1, 1, 0);
    atN_set(p, i + 1, nw, atN_sumbytes(nw, 0, nc));
    p->bytes[i] = atN_sumbytes(nd, 0, mid);
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

static int atI_growleft(at_Cursor *C, size_t ins) {
    at_Edge *prev = atK_prevedge(C);
    at_Node *n;
    int      k = atK_levels(C);
    size_t   b;
    assert(C->poff == 0);
    if (prev == NULL) return 0;
    n = prev->parent, C->paths[k] = &n->children[prev->idx];
    b = n->bytes[prev->idx];
    for (--k; k >= 0; --k, n = n->parent)
        C->paths[k] = &n->parent->children[n->idx];
    C->off -= b, C->poff = b, atE_resize(C, (at_Delta)ins);
    return (C->cur = atU_point(0, 0)), 1;
}

static void atI_insertsplit(at_Cursor *C, size_t ins) {
    at_Node *p;
    at_Edge *ne = atE_new(C->tree);
    int      l = (atI_makeroom(C), atK_levels(C));
    int      i = atK_idx(C, p = atK_parent(C, l), l);
    size_t   b = p->bytes[i];
    at_Edge *e = atN_edge(p, i);
    assert(C->poff == 0 && e->right.id != 0), atE_resize(C, (at_Delta)ins);
    p->bytes[i] = ins, atN_makespace(p, i + 1, 1, 1);
    atN_setleaf(p, i + 1, ne, b);
    atE_mvlink(C->tree, ne, e, 1);
    if (C->cur.id != 0 && atA_side(C->tree, C->cur)) C->cur = atU_point(0, 0);
}

static int atI_insert(at_Cursor *C, size_t ins) {
    size_t b, pos = at_offset(C), lim = pos > atK_bytes(C) ? pos : atK_bytes(C);
    int    r, l, i;
    at_Node *p;
    at_Edge *e;
    if (assert(ins > 0), lim + ins < lim) return AT_ERROVERFLOW;
    if ((r = atU_reserve(C, 1, 1, 2)) != AT_OK) return r;
    if (atN_cc(&C->tree->root) == 0) {
        e = atE_new(C->tree), atN_setleaf(&C->tree->root, 0, e, pos + ins);
        atN_setcc(&C->tree->root, 1), C->tree->bytes = pos + ins;
        C->paths[0] = &C->tree->root.children[0], C->off = 0, C->poff = pos;
        return AT_OK;
    }
    l = atK_levels(C), p = atK_parent(C, l), i = atK_idx(C, p, l);
    e = atN_edge(p, i), b = p->bytes[i];
    if (C->poff == 0 && e->right.id != 0) {
        if (e->left.id == 0 && atI_growleft(C, ins)) return AT_OK;
        return atI_insertsplit(C, ins), AT_OK;
    }
    atE_resize(C, (at_Delta)(ins + (C->poff > b ? C->poff - b : 0)));
    return AT_OK;
}

/* anchor edges */

static at_Edge *atA_splitat(at_Cursor *C) {
    at_Edge *e = atE_new(C->tree);
    int      i, l = (atI_makeroom(C), atK_levels(C));
    size_t   rem;
    at_Node *p;
    i = atK_idx(C, p = atK_parent(C, l), l);
    assert(C->poff && C->poff < p->bytes[i]), rem = p->bytes[i] - C->poff;
    atN_makespace(p, i + 1, 1, 1);
    p->bytes[i] = C->poff, atN_setleaf(p, i + 1, e, rem);
    C->paths[l] = &p->children[i + 1], C->off += C->poff, C->poff = 0;
    return e;
}

static at_Edge *atA_pad(at_Cursor *C) {
    at_Edge *e = atE_new(C->tree);
    at_Node *p;
    int      l, i, cc;
    l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l), cc = atN_cc(p);
    assert(i == cc - 1 && C->poff >= p->bytes[i]);
    atE_resize(C, (at_Delta)(C->poff - p->bytes[i]));
    atI_makeroom(C);
    l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l), cc = atN_cc(p);
    assert(i == cc - 1 && C->poff == p->bytes[i]);
    atN_makespace(p, cc, 1, 1), atN_setleaf(p, cc, e, 0);
    C->off += p->bytes[i], C->paths[l] = &p->children[cc], C->poff = 0;
    return e;
}

static at_Edge *atA_newempty(at_Cursor *C) {
    at_Edge *e = atE_new(C->tree);
    at_Node *root = &C->tree->root;
    size_t   pos = at_offset(C);
    int      i = 0;
    if (pos > 0)
        atN_setleaf(root, 0, atE_new(C->tree), pos), atN_setcc(root, 1), i = 1;
    atN_setleaf(root, i, e, 0), atN_setcc(root, i + 1);
    C->tree->bytes = pos, C->paths[0] = &root->children[i];
    C->off = pos, C->poff = 0;
    return e;
}

static void atA_addpoint(at_Cursor *C, at_Point slot) {
    at_Node   *p;
    size_t     b;
    int        i, l = atK_levels(C), cc = atN_cc(&C->tree->root);
    at_Anchor *a = atA_slot(C->tree, slot.id);
    at_Edge   *e = cc ? atK_edge(C) : atA_newempty(C);
    at_Point  *h;
    i = atK_idx(C, p = atK_parent(C, l), l), b = p->bytes[i];
    if (C->poff > 0) e = C->poff < b ? atA_splitat(C) : atA_pad(C);
    h = atE_head(slot.is_end ? a->end_right : a->start_right, e);
    *(slot.is_end ? &a->enext : &a->snext) = *h, *h = slot;
    *(slot.is_end ? &a->end : &a->start) = e;
}

/* APIs */

AT_API size_t at_bytes(const at_Tree *T) { return T ? T->bytes : 0; }

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
    memset(S, 0, sizeof(at_State)), S->ud = ud, S->allocf = allocf;
    atP_init(&S->nodes, sizeof(at_Node)), atP_init(&S->edges, sizeof(at_Edge));
    return S;
}

AT_API void at_close(at_State *S) {
    if (S == NULL) return;
    atP_destroy(S, &S->nodes), atP_destroy(S, &S->edges);
    S->allocf(S->ud, S, sizeof(at_State), 0);
}

AT_API at_Tree *at_newtree(at_State *S) {
    at_Tree *T;
    if (S == NULL) return NULL;
    T = (at_Tree *)S->allocf(S->ud, NULL, 0, sizeof(at_Tree));
    if (T == NULL) return NULL;
    return memset(T, 0, sizeof(at_Tree)), T->S = S, T->epoch = 1, T;
}

AT_API void at_deltree(at_Tree *T) {
    if (T == NULL) return;
    T->seam = NULL;
    atN_purge(T, &T->root, T->levels, 0, atN_cc(&T->root)), atA_clear(T);
    T->S->allocf(T->S->ud, T, sizeof(at_Tree), 0);
}

AT_API void at_reset(at_Tree *T) {
    if (T == NULL) return;
    T->epoch += 1, T->seam = NULL;
    atN_purge(T, &T->root, T->levels, 0, atN_cc(&T->root)), atA_clear(T);
    memset(&T->root, 0, sizeof(at_Node));
    T->bytes = 0, T->levels = 0;
}

AT_API int at_locate(at_Cursor *C, size_t pos) {
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    C->epoch = C->tree->epoch, C->off = C->poff = 0;
    if (pos >= atK_bytes(C))
        return atK_locend(C), C->poff += pos - atK_bytes(C), AT_OK;
    return atK_findleaf(C, 0, &pos), C->poff = pos, AT_OK;
}

AT_API int at_seek(at_Cursor *C, at_Tree *T, size_t pos) {
    if (C == NULL || T == NULL) return AT_ERRPARAM;
    return memset(C, 0, sizeof(at_Cursor)), (C->tree = T), at_locate(C, pos);
}

AT_API int at_advance(at_Cursor *C, at_Delta d) {
    size_t off;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    if (d == 0) return AT_OK; /* d == 0 keeps bucket scan state */
    off = at_offset(C);
    if (d < 0 && -(size_t)d > off) return atK_backwardoff(C, off), AT_OK;
    if (d < 0) return atK_backwardoff(C, -(size_t)d), AT_OK;
    if (off + d < off) return AT_ERROVERFLOW; /* size_t wrap */
    return atK_forwardoff(C, d), AT_OK;
}

AT_API at_Id at_anchor(at_Cursor *C, int *pns, size_t *plen) {
    size_t     s = 0, e = 0;
    at_Anchor *a;
    if (!C || !C->tree || C->epoch != C->tree->epoch) return atK_none(plen);
    if ((a = atA_fromid(C->tree, C->cur.id)) == NULL) return atK_none(plen);
    if (pns) *pns = a->ns;
    if (plen) at_range(C->tree, C->cur.id, &s, &e), *plen = s <= e ? e - s : 0;
    return C->cur.id;
}

AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen) {
    size_t start = 0, end = 0;
    at_Id  id;
    if (C == NULL || C->tree == NULL) return atK_none(plen);
    if (C->epoch != C->tree->epoch || ns < 0 || (unsigned)ns > AT_MAX_NS)
        return atK_none(plen);
    if (atN_cc(&C->tree->root) == 0 || (C->cur.id == 0 && C->cur.is_end))
        return C->cur = atU_point(0, 1), atK_none(plen);
    while ((id = atK_nextstart(C, ns)) == AT_NONE)
        if (!atK_nextedge(C)) return C->cur = atU_point(0, 1), atK_none(plen);
    if (plen) {
        at_range(C->tree, id, &start, &end);
        *plen = start <= end ? end - start : 0;
    }
    return id;
}

AT_API int at_remove(at_Cursor *C, size_t len) {
    int r;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    if (len == 0 || at_offset(C) >= C->tree->bytes) return AT_OK;
    if ((r = atD_remove(C, len)) != AT_OK) return r;
    assert(C->tree->seam == NULL);
    return C->tree->epoch += 1, C->epoch = C->tree->epoch, AT_OK;
}

AT_API int at_insert(at_Cursor *C, size_t ins) {
    int r;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    if (ins == 0) return AT_OK;
    if ((r = atI_insert(C, ins)) != AT_OK) return r;
    return C->tree->epoch += 1, C->epoch = C->tree->epoch, AT_OK;
}

AT_API int at_append(at_Cursor *C, size_t ins) {
    int r;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    if (ins == 0) return AT_OK;
    if ((r = atI_insert(C, ins)) != AT_OK) return r;
    atK_forwardoff(C, ins);
    return C->tree->epoch += 1, C->epoch = C->tree->epoch, AT_OK;
}

AT_API int at_splice(at_Cursor *C, size_t del, size_t ins) {
    size_t pos, lim, rm, add;
    int    r;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    pos = at_offset(C);
    if (del == 0 && ins == 0) return AT_OK;
    if (ins == 0 && pos >= atK_bytes(C)) return AT_OK;
    lim = pos > atK_bytes(C) ? pos : atK_bytes(C);
    if (lim + ins < lim) return AT_ERROVERFLOW;
    rm = del > 0 && pos < atK_bytes(C), add = ins > 0;
    if ((r = atU_reserve(C, 1, rm + add, 2 * add)) != AT_OK) return r;
    if (rm) r = atD_remove(C, del), assert(!r);
    if (ins > 0) r = atI_insert(C, ins), assert(!r), atK_forwardoff(C, ins);
    assert(at_offset(C) == pos + ins);
    return C->tree->epoch += 1, C->epoch = C->tree->epoch, AT_OK;
}

AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out) {
    size_t     start;
    at_Anchor *a;
    at_Id      id;
    if (C == NULL || C->tree == NULL || out == NULL) return AT_ERRPARAM;
    if (g < AT_GROW || g > AT_LOCK) return AT_ERRPARAM;
    if (ns < 0 || (unsigned)ns > AT_MAX_NS) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    if (start = at_offset(C), start + len < start) return AT_ERROVERFLOW;
    if ((id = atA_alloc(C->tree)) == 0) return AT_ERRMEM;
    a = atA_slot(C->tree, id), memset(a, 0, sizeof(*a));
    if (atU_reserve(C, 3, 2, 5) != AT_OK)
        return atA_free(C->tree, id), AT_ERRMEM;
    a->ns = (unsigned)ns;
    a->start_right = (g == AT_FIXED || g == AT_SHIFT);
    a->end_right = (g == AT_GROW || g == AT_SHIFT);
    atA_addpoint(C, atU_point(id, 0));
    atK_forwardoff(C, len), atA_addpoint(C, atU_point(id, 1));
    return (C->tree->epoch += 1, *out = id), AT_OK;
}

AT_API int at_delanchor(at_Tree *T, at_Id id) {
    at_Edge   *se, *ee;
    at_Anchor *a;
    if ((a = atA_fromid(T, id)) == NULL) return AT_ERRPARAM;
    T->epoch += 1, se = a->start, ee = a->end;
    atE_unlink(T, atE_head(a->start_right, se), atU_point(id, 0));
    atE_unlink(T, atE_head(a->end_right, ee), atU_point(id, 1));
    atA_free(T, id), atE_purge(T, se);
    if (ee != se) atE_purge(T, ee);
    return AT_OK;
}

AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end) {
    at_Anchor *a;
    at_Cursor  C;
    if (start == NULL || end == NULL) return AT_ERRPARAM;
    if ((a = atA_fromid(T, id)) == NULL) return AT_ERRPARAM;
    *start = atK_fromedge(&C, T, a->start);
    *end = atK_fromedge(&C, T, a->end);
    return AT_OK;
}

AT_API int at_gravity(at_Tree *T, at_Id id) {
    at_Anchor *a;
    if ((a = atA_fromid(T, id)) == NULL) return AT_ERRPARAM;
    if (a->start_right) return a->end_right ? AT_SHIFT : AT_FIXED;
    return a->end_right ? AT_GROW : AT_LOCK;
}

AT_NS_END

#endif /* AT_IMPLEMENTATION */
