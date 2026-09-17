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
#define AT_NONE        (~(at_Id)0)      /* End-of-iteration sentinel */
#define AT_MAX_NS      ((1u << 29) - 1) /* largest namespace filter value */

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
AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen);

/* anchors creation */
AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out);

/* text modification */
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
    unsigned epoch;    /* tree generation captured by seek/locate */

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

#define atU_addoverflow(a, b) ((b) > (size_t)-1 - (a))

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
    void     *ud;     /* allocator user data */
    at_Alloc *allocf; /* memory allocator */
    at_Pool   nodes;  /* pool for at_Node objects */
    at_Pool   edges;  /* pool for at_Edge objects */
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
};

AT_STATIC_ASSERT(sizeof(at_Point) == 4);
AT_STATIC_ASSERT(sizeof(at_Node) < AT_PAGE_SIZE / 2);
AT_STATIC_ASSERT(sizeof(at_Edge) < AT_PAGE_SIZE / 2);

#define atN_cc(n)            ((int)(n)->child_count)
#define atN_setcc(n, v)      ((n)->child_count = (at_Index)(v))
#define atN_edge(n, i)       ((at_Edge *)(n)->children[i])
#define atN_setedge(n, i, e) ((n)->children[i] = (at_Node *)(e))

#define atK_levels(C)    ((C)->tree->levels)
#define atK_bytes(C)     ((C)->tree->bytes)
#define atK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define atK_idx(C, p, l) ((int)((C)->paths[(l)] - (p)->children))

#define atA_slot(T, id) (&(T)->anchors[(id) - 1])
#define atE_next(p, a)  ((p).is_end ? (a)->enext : (a)->snext)

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
    char  *page, *end;
    size_t sz = p->obj_size;
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

/* clang-format off */
static int atE_empty(at_Edge *e)
{ return e->left.id == 0 && e->right.id == 0; }

static at_Id atK_none(size_t *plen)
{ return (void)(plen && (*plen = 0)), AT_NONE; }
/* clang-format on */

AT_API size_t at_bytes(const at_Tree *T) { return T ? T->bytes : 0; }

static at_Edge *atE_new(at_Tree *T) {
    at_Edge *e = (at_Edge *)atP_ralloc(&T->S->edges);
    return memset(e, 0, sizeof(*e)), e;
}

static void atE_link(at_Edge *e, unsigned side, at_Point slot, at_Anchor *a) {
    at_Point *head = side ? &e->right : &e->left;
    *(slot.is_end ? &a->enext : &a->snext) = *head, *head = slot;
}

static int atE_unlink(at_Tree *T, at_Edge *e, unsigned side, at_Point slot) {
    at_Point   prev, next, *head = side ? &e->right : &e->left, p = *head;
    at_Anchor *a, *pa;
    prev.id = 0, prev.is_end = 0;
    while (p.id != 0 && (p.id != slot.id || p.is_end != slot.is_end))
        a = atA_slot(T, p.id), prev = p, p = atE_next(p, a);
    assert(p.id == slot.id && p.is_end == slot.is_end), a = atA_slot(T, p.id);
    next = atE_next(p, a);
    if (prev.id == 0) return (*head = next), 0;
    pa = atA_slot(T, prev.id);
    return *(prev.is_end ? &pa->enext : &pa->snext) = next, 1;
}

static void atE_movelink(at_Tree *T, at_Edge *dst, at_Edge *src, int side) {
    at_Point   next, *head = side ? &src->right : &src->left, p = *head;
    at_Anchor *a;
    head->id = 0, head->is_end = 0;
    for (; p.id != 0; p = next) {
        a = atA_slot(T, p.id), next = atE_next(p, a);
        *(p.is_end ? &a->end : &a->start) = dst, atE_link(dst, side, p, a);
    }
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

static at_Id atE_nextstart(at_Cursor *C, at_Edge *e, int ns) {
    at_Point   p, next;
    at_Anchor *a;
    while (C->side < 2) {
        if (C->next_id == 0) {
            if (++C->side == 1)
                C->next_id = e->right.id, C->next_end = e->right.is_end;
        } else {
            p.id = C->next_id, p.is_end = C->next_end;
            a = atA_slot(C->tree, p.id), next = atE_next(p, a);
            C->next_id = next.id, C->next_end = next.is_end;
            if (!p.is_end && (ns == 0 || a->ns == (unsigned)ns)) return p.id;
        }
    }
    return AT_NONE;
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
        for (i = s; i < e; ++i) atP_free(&T->S->edges, atN_edge(p, i));
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
    atN_purge(T, &T->root, T->levels, 0, atN_cc(&T->root)), atA_clear(T);
    T->S->allocf(T->S->ud, T, sizeof(at_Tree), 0);
}

AT_API void at_reset(at_Tree *T) {
    if (T == NULL) return;
    T->epoch += 1;
    atN_purge(T, &T->root, T->levels, 0, atN_cc(&T->root)), atA_clear(T);
    memset(&T->root, 0, sizeof(at_Node));
    T->bytes = 0, T->levels = 0;
}

/* metric/path helpers */

static void atM_up(at_Cursor *C, int l, at_Delta db) {
    for (; l >= 0; --l) {
        at_Node *p = atK_parent(C, l);
        p->bytes[atK_idx(C, p, l)] += db;
    }
    C->tree->bytes += db;
}

/* cursor navigation */

static void atK_rewind(at_Cursor *C) {
    at_Node *p;
    at_Edge *e;
    int      l;
    C->side = 2, C->next_id = 0, C->next_end = 0;
    if (atN_cc(&C->tree->root) == 0) return;
    l = atK_levels(C), p = l ? *C->paths[l - 1] : &C->tree->root;
    e = atN_edge(p, (int)(C->paths[l] - p->children));
    C->side = 0, C->next_id = e->left.id, C->next_end = e->left.is_end;
}

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
    return (C->off = C->tree->bytes - C->poff), atK_rewind(C), 1;
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
    size_t   in, off = at_offset(C);
    int      l, i = atK_idx(C, p = atK_parent(C, atK_levels(C)), atK_levels(C));
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
    at_Node *p, *q;
    int      l, i, cc, L = atK_levels(C);
    size_t   b;
    p = L ? *C->paths[L - 1] : &C->tree->root;
    i = atK_idx(C, p, L), cc = atN_cc(p), b = p->bytes[i];
    if (i + 1 < cc) {
        C->paths[L] = &p->children[i + 1];
        return (C->off += b, C->poff = 0), 1;
    }
    for (l = L - 1; l >= 0; --l) {
        q = l ? *C->paths[l - 1] : &C->tree->root;
        i = atK_idx(C, q, l), cc = atN_cc(q);
        if (i + 1 < cc) {
            C->paths[l] = &q->children[i + 1];
            for (; l < L; ++l)
                p = *C->paths[l], C->paths[l + 1] = &p->children[0];
            return C->off += b, C->poff = 0, 1;
        }
    }
    if (C->poff == 0) C->poff = b;
    return 0;
}

AT_API int at_locate(at_Cursor *C, size_t pos) {
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    C->epoch = C->tree->epoch, C->off = C->poff = 0;
    if (pos >= atK_bytes(C))
        atK_locend(C), C->poff += pos - atK_bytes(C);
    else
        atK_findleaf(C, 0, &pos), C->poff = pos;
    return AT_OK;
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

static at_Edge *atK_edge(at_Cursor *C) {
    at_Node *p = atK_parent(C, atK_levels(C));
    return atN_edge(p, atK_idx(C, p, atK_levels(C)));
}

static at_Id atK_step(at_Cursor *C, int ns, size_t *plen) {
    at_Id    id;
    int      l = atK_levels(C);
    at_Node *p = l ? *C->paths[l - 1] : &C->tree->root;
    if (C->poff > p->bytes[atK_idx(C, p, l)]) return atK_none(plen);
    if (C->poff > 0) {
        if (!atK_nextedge(C)) return atK_none(plen);
        atK_rewind(C);
    }
    for (; (id = atE_nextstart(C, atK_edge(C), ns)) == AT_NONE; atK_rewind(C))
        if (!atK_nextedge(C)) return atK_none(plen);
    if (plen) {
        size_t start = 0, end = 0;
        int    r = at_range(C->tree, id, &start, &end);
        assert(r == AT_OK), (void)r;
        *plen = start <= end ? end - start : 0;
    }
    return id;
}

AT_API at_Id at_next(at_Cursor *C, int ns, size_t *plen) {
    if (C == NULL || C->tree == NULL) return atK_none(plen);
    if (C->epoch != C->tree->epoch) return atK_none(plen);
    if (ns < 0 || (unsigned)ns > AT_MAX_NS) return atK_none(plen);
    if (atK_bytes(C) == 0 && atN_cc(&C->tree->root) == 0) return atK_none(plen);
    return atK_step(C, ns, plen);
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
    at_Node *p, *n = e->parent;
    int      l;
    C->tree = T;
    C->side = 2, C->next_id = 0, C->next_end = 0;
    C->paths[T->levels] = &n->children[e->idx];
    for (l = T->levels - 1; l >= 0; --l)
        p = n->parent, C->paths[l] = &p->children[n->idx], n = p;
    C->poff = 0, C->epoch = T->epoch, atK_edgepos(C);
}

static void atE_resize(at_Cursor *C, at_Delta d) {
    at_Node *p;
    int      l = atK_levels(C);
    p = atK_parent(C, l), p->bytes[atK_idx(C, p, l)] += d, atM_up(C, l - 1, d);
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

static at_Node *atI_splitnode(at_State *S, at_Node *n, int level) {
    int      cc = atN_cc(n), left = cc / 2, right = cc - left;
    at_Node *r = (at_Node *)atP_ralloc(&S->nodes);
    assert(left >= AT_FANOUT / 2 && right >= AT_FANOUT / 2);
    atN_setcc(r, right), r->parent = n->parent, r->idx = 0;
    atN_copy(r, 0, n, left, right), atN_fixlinks(r, 0, right, level == 0);
    return atN_setcc(n, left), r;
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
    int      cs, i = atK_idx(C, p, l);
    assert(atN_cc(nd) >= AT_FANOUT);
    nw = atI_splitnode(T->S, nd, T->levels - l - 1);
    atN_makespace(p, i + 1, 1, 0);
    atN_set(p, i + 1, nw, atN_sumbytes(nw, 0, atN_cc(nw)));
    p->bytes[i] = atN_sumbytes(nd, 0, atN_cc(nd));
    if ((cs = atK_idx(C, nd, l + 1)) >= atN_cc(nd)) {
        C->paths[l] = &p->children[i + 1];
        C->paths[l + 1] = &nw->children[cs - atN_cc(nd)];
    }
}

static void atI_makeroom(at_Cursor *C) {
    int l;
    if (atN_cc(atK_parent(C, atK_levels(C))) < AT_FANOUT) return;
    for (l = atK_levels(C); l >= 0; --l)
        if (atN_cc(atK_parent(C, l)) < AT_FANOUT) break;
    if (l < 0) atI_splitroot(C), l = 1;
    for (; l < atK_levels(C); ++l) atI_splitchild(C, l);
}

static at_Edge *atI_splitat(at_Cursor *C) {
    at_Edge *e = atE_new(C->tree);
    at_Node *p;
    size_t   rem;
    int      l, i;
    assert(C->poff > 0);
    atI_makeroom(C);
    l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l);
    assert(C->poff < p->bytes[i]);
    rem = p->bytes[i] - C->poff;
    atN_makespace(p, i + 1, 1, 1);
    p->bytes[i] = C->poff, atN_setleaf(p, i + 1, e, rem);
    C->paths[l] = &p->children[i + 1], C->off += C->poff, C->poff = 0;
    return e;
}

static at_Edge *atI_pad(at_Cursor *C) {
    at_Edge *e = atE_new(C->tree);
    at_Node *p;
    int      l, i, cc;
    atI_makeroom(C);
    l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l), cc = atN_cc(p);
    assert(i == cc - 1 && C->poff == p->bytes[i]);
    atN_makespace(p, cc, 1, 1), atN_setleaf(p, cc, e, 0);
    C->off += p->bytes[i], C->paths[l] = &p->children[cc], C->poff = 0;
    return e;
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
    int      i;
    assert(l == 0 || l < atK_levels(C));
    for (; l > 0; --l) {
        p = atK_parent(C, l);
        if (atN_cc(p->children[atK_idx(C, p, l)]) >= AT_FANOUT / 2) return;
        if (assert(atN_cc(p) > 1), !atD_foldnode(C, 0, l)) return;
    }
    assert(atK_levels(C) > 0), i = atK_idx(C, p = &C->tree->root, 0);
    if (atN_cc(p->children[i]) < AT_FANOUT / 2 && atN_cc(p) >= 2)
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
    at_Node  *p;
    int       l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l);
    at_Tree  *T = C->tree;
    at_Edge  *e, *prev;
    size_t    delta;
    at_Cursor P;
    e = atN_edge(p, i), delta = p->bytes[i], prev = atK_prevedge(C);
    assert(atE_empty(e));
    atN_removeslot(p, i, 1, 1), atP_free(&T->S->edges, e);
    if (prev == NULL)
        assert(T->bytes == 0), C->off = C->poff = 0, C->paths[0] = p->children;
    else {
        atK_fromedge(&P, T, prev), atE_resize(&P, (at_Delta)delta);
        C->paths[l] = p->children;
        atM_up(C, l - 1, (at_Delta)((size_t)0 - delta));
        if (l > 0 && atN_cc(p) < AT_FANOUT / 2) atD_rebalance(C, l - 1);
        atK_fromedge(C, T, prev), C->poff = prev->parent->bytes[prev->idx];
    }
}

static size_t atD_rmbound(at_Cursor *L, at_Cursor *R) {
    at_Edge *e;
    assert(L->poff == 0);
    if (at_offset(R) >= L->tree->bytes) return 0;
    if (R->poff > 0) atI_splitat(R);
    e = atK_edge(R);
    return e->parent->bytes[e->idx];
}

static void atD_rmempty(at_Cursor *C) {
    size_t pos = at_offset(C);
    if (!atE_empty(atK_edge(C))) return;
    if (atK_edgepos(C) == 0 && C->tree->bytes != 0) return;
    atD_rmleaf(C);
    if (atN_cc(&C->tree->root) != 0)
        assert(pos >= C->off), C->poff = pos - C->off;
}

static void atD_remove(at_Cursor *C, size_t del) {
    at_Tree  *T = C->tree;
    at_Cursor R;
    at_Edge  *le, *e;
    size_t    pos, Rpos, bR;
    assert(del > 0), pos = at_offset(C);
    if (pos >= T->bytes) return;
    if (del > T->bytes - pos) del = T->bytes - pos;
    if (C->poff > 0) atI_splitat(C);
    le = atK_edge(C), Rpos = pos + del, R = *C;
    atK_forwardoff(&R, del);
    bR = atD_rmbound(C, &R);
    atK_fromedge(&R, T, le);
    while (atK_nextedge(&R) && R.off <= Rpos) {
        e = atK_edge(&R);
        atE_movelink(T, le, e, 0), atE_movelink(T, le, e, 1), atD_rmleaf(&R);
    }
    atK_fromedge(C, T, le);
    atE_resize(C, (at_Delta)(bR - le->parent->bytes[le->idx]));
    if (atE_empty(le)) atD_rmempty(C);
    C->epoch = T->epoch;
}

/* insertion */

static at_Edge *atI_emptyedge(at_Cursor *C) {
    at_Tree *T = C->tree;
    at_Edge *e = atE_new(T);
    size_t   pos = at_offset(C);
    if (pos == 0) {
        atN_setleaf(&T->root, 0, e, 0), atN_setcc(&T->root, 1);
        C->paths[0] = &T->root.children[0], C->off = 0, C->poff = 0;
    } else {
        atN_setleaf(&T->root, 0, atE_new(T), pos);
        atN_setleaf(&T->root, 1, e, 0), atN_setcc(&T->root, 2), T->bytes = pos;
        C->paths[0] = &T->root.children[1], C->off = pos, C->poff = 0;
    }
    return e;
}

static at_Edge *atI_ensure(at_Cursor *C) {
    at_Tree *T = C->tree;
    at_Node *p;
    size_t   b, pos = at_offset(C);
    int      i, l = atK_levels(C);
    if (atN_cc(&T->root) == 0) return atI_emptyedge(C);
    if (C->poff == 0) return atK_edge(C);
    p = atK_parent(C, l), i = atK_idx(C, p, l), b = p->bytes[i];
    if (C->poff < b) return atI_splitat(C);
    if (pos > T->bytes) atE_resize(C, (at_Delta)(pos - T->bytes));
    return atI_pad(C);
}

static void atI_insertsplit(at_Cursor *C, size_t ins) {
    at_Tree *T = C->tree;
    at_Edge *e, *ne = atE_new(T);
    at_Node *p;
    size_t   b;
    int      l, i;
    atI_makeroom(C);
    l = atK_levels(C), i = atK_idx(C, p = atK_parent(C, l), l);
    e = atN_edge(p, i);
    assert(C->poff == 0 && e->right.id != 0);
    b = p->bytes[i], atE_resize(C, (at_Delta)ins);
    p->bytes[i] = ins, atN_makespace(p, i + 1, 1, 1);
    atN_setleaf(p, i + 1, ne, b);
    atE_movelink(T, ne, e, 1), atD_rmempty(C);
}

static int atI_insert(at_Cursor *C, size_t ins) {
    at_Node *p;
    at_Edge *e;
    size_t   pos, b;
    int      l, i;
    assert(ins > 0);
    pos = at_offset(C), assert(ins <= (size_t)-1 - pos);
    if (atN_cc(&C->tree->root) == 0) {
        e = atE_new(C->tree), atN_setleaf(&C->tree->root, 0, e, pos + ins);
        atN_setcc(&C->tree->root, 1), C->tree->bytes = pos + ins;
        C->paths[0] = &C->tree->root.children[0], C->off = 0, C->poff = pos;
        return 0;
    }
    l = atK_levels(C), p = atK_parent(C, l), i = atK_idx(C, p, l);
    e = atN_edge(p, i);
    if (C->poff == 0 && e->right.id) return atI_insertsplit(C, ins), 1;
    b = p->bytes[i];
    atE_resize(C, (at_Delta)(ins + (C->poff > b ? C->poff - b : 0)));
    return atD_rmempty(C), 1;
}

/* APIs */

AT_API int at_splice(at_Cursor *C, size_t del, size_t ins) {
    at_Tree *T;
    size_t   pos, limit;
    int      r;
    if (C == NULL || C->tree == NULL) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    T = C->tree, pos = at_offset(C);
    if (del == 0 && ins == 0) return AT_OK;
    if (ins == 0 && pos >= T->bytes) return AT_OK;
    limit = pos > T->bytes ? pos : T->bytes;
    if (atU_addoverflow(limit, ins)) return AT_ERROVERFLOW;
    r = atI_reserve(C, 6, 3);
    if (r != AT_OK) return r;
    T->epoch += 1, C->epoch = T->epoch;
    if (del > 0) atD_remove(C, del);
    if (ins > 0) {
        atI_insert(C, ins);
        atK_forwardoff(C, ins);
    } else
        atK_rewind(C);
    return assert(at_offset(C) == pos + ins), AT_OK;
}

AT_API int at_newanchor(at_Cursor *C, size_t len, int ns, int g, at_Id *out) {
    at_Anchor *a;
    at_Point   slot;
    at_Id      id;
    size_t     start;
    if (C == NULL || C->tree == NULL || out == NULL) return AT_ERRPARAM;
    if (g < AT_GROW || g > AT_LOCK) return AT_ERRPARAM;
    if (ns < 0 || (unsigned)ns > AT_MAX_NS) return AT_ERRPARAM;
    if (C->epoch != C->tree->epoch) return AT_ESTALE;
    start = at_offset(C);
    if (atU_addoverflow(start, len)) return AT_ERROVERFLOW;
    if ((id = atA_alloc(C->tree)) == 0) return AT_ERRMEM;
    a = atA_slot(C->tree, id), memset(a, 0, sizeof(*a));
    if (atI_reserve(C, 4, 2) != AT_OK) return atA_free(C->tree, id), AT_ERRMEM;
    a->start = atI_ensure(C), a->ns = (unsigned)ns;
    a->start_right = (unsigned)(g == AT_FIXED || g == AT_SHIFT);
    a->end_right = (unsigned)(g == AT_GROW || g == AT_SHIFT);
    if (len == 0)
        a->end = a->start;
    else if (len < atK_bytes(C) - start)
        atK_forwardoff(C, len), a->end = atI_ensure(C);
    else {
        atK_locend(C), C->poff += start + len - atK_bytes(C);
        a->end = atI_ensure(C);
    }
    slot.id = id, slot.is_end = 0, atE_link(a->start, a->start_right, slot, a);
    slot.is_end = 1, atE_link(a->end, a->end_right, slot, a);
    return C->tree->epoch += 1, *out = id, AT_OK;
}

AT_API int at_delanchor(at_Tree *T, at_Id id) {
    at_Anchor *a;
    at_Cursor  C;
    at_Edge   *se, *ee;
    at_Point   slot;
    if ((a = atA_fromid(T, id)) == NULL) return AT_ERRPARAM;
    T->epoch += 1;
    se = a->start, ee = a->end, slot.id = id, slot.is_end = 0;
    atE_unlink(T, se, a->start_right, slot);
    slot.is_end = 1;
    atE_unlink(T, ee, a->end_right, slot);
    atA_free(T, id), atK_fromedge(&C, T, se), atD_rmempty(&C);
    if (ee != se) atK_fromedge(&C, T, ee), atD_rmempty(&C);
    return AT_OK;
}

AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end) {
    at_Anchor *a;
    at_Cursor  C;
    if (start == NULL || end == NULL) return AT_ERRPARAM;
    if ((a = atA_fromid(T, id)) == NULL) return AT_ERRPARAM;
    atK_fromedge(&C, T, a->start), *start = atK_edgepos(&C);
    return atK_fromedge(&C, T, a->end), *end = atK_edgepos(&C), AT_OK;
}

AT_API int at_gravity(at_Tree *T, at_Id id) {
    at_Anchor *a;
    if ((a = atA_fromid(T, id)) == NULL) return AT_ERRPARAM;
    if (a->start_right) return a->end_right ? AT_SHIFT : AT_FIXED;
    return a->end_right ? AT_GROW : AT_LOCK;
}

AT_NS_END

#endif /* AT_IMPLEMENTATION */
