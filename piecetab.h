#ifndef piecetab_h
#define piecetab_h

#ifndef PT_NS_BEGIN
# ifdef __cplusplus
#   define PT_NS_BEGIN extern "C" {
#   define PT_NS_END   }
# else
#   define PT_NS_BEGIN
#   define PT_NS_END
# endif
#endif /* PT_NS_BEGIN */

#ifndef PT_STATIC
# if __GNUC__
#   define PT_STATIC static __attribute((unused))
# else
#   define PT_STATIC static
# endif
#endif /* PT_STATIC */

#ifdef PT_STATIC_API
# ifndef PT_IMPLEMENTATION
#   define PT_IMPLEMENTATION
# endif
# define PT_API PT_STATIC
#endif /* PT_STATIC_API */

#if !defined(PT_API) && defined(_WIN32)
# ifdef PT_IMPLEMENTATION
#   define PT_API __declspec(dllexport)
# else
#   define PT_API __declspec(dllimport)
# endif
#endif /* PT_API */

#ifndef PT_API
# define PT_API extern
#endif

#include <limits.h>
#include <stddef.h>

#define PT_OK       (0)  /* No error */
#define PT_ERRPARAM (-1) /* Invalid parameter */
#define PT_ERRMEM   (-2) /* Memory allocation failed */

PT_NS_BEGIN

typedef struct pt_State       pt_State;
typedef struct pt_Cursor      pt_Cursor;
typedef const struct pt_Tree *pt_Blob;

typedef ptrdiff_t pt_Delta;

typedef void *pt_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/* lifetime*/

PT_API pt_State *pt_open(pt_Alloc *allocf, void *ud);
PT_API void      pt_close(pt_State *S);
PT_API pt_Alloc *pt_getallocf(pt_State *S, void **pud);

PT_API unsigned pt_retain(pt_Blob b);
PT_API unsigned pt_release(pt_Blob b);

/* blob */

/* construction */
PT_API pt_Blob pt_empty(pt_State *S);
PT_API pt_Blob pt_from(pt_State *S, const char *s, size_t len);

/* query */
PT_API unsigned pt_version(pt_Blob b);
PT_API size_t   pt_bytes(pt_Blob b);

/* cursor */

/* construction */
PT_API int pt_seek(pt_Cursor *C, pt_Blob b, size_t off);

/* navigate */
PT_API int pt_locate(pt_Cursor *C, size_t off);
PT_API int pt_advance(pt_Cursor *C, pt_Delta d);

/* read */
PT_API size_t pt_read(pt_Cursor *C, char *buf, size_t len);

PT_API const char *pt_piece(pt_Cursor *C, size_t *plen);
PT_API const char *pt_next(pt_Cursor *C, size_t *plen);
PT_API const char *pt_prev(pt_Cursor *C, size_t *plen);

/* query */
#define pt_offset(C) ((C)->off + (C)->poff)
#define pt_blob(C)   ((C)->tree)

/* editing */

/* hole edit */
PT_API int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len);

/* literal edit */
PT_API int pt_insert(pt_Cursor *C, const char *s, size_t len);
PT_API int pt_append(pt_Cursor *C, const char *s, size_t len);
PT_API int pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len);
PT_API int pt_remove(pt_Cursor *C, size_t len);

/* transaction */
PT_API void    pt_rollback(pt_Cursor *C);
PT_API pt_Blob pt_commit(pt_Cursor *C);

/* literal scratch buffer (arena) */
PT_API char *pt_reserve(pt_Cursor *C, size_t len);
PT_API char *pt_scratch(pt_Cursor *C, size_t *plen);

PT_API const char *pt_literal(pt_Cursor *C, size_t len);

/* cursor definition */

#define PT_MAX_LEVEL 16

struct pt_Cursor {
    struct pt_Node **paths[PT_MAX_LEVEL]; /* root-to-leaf child slot ptrs */
    struct pt_Tree  *tree;                /* blob under navigation or edit */
    size_t           poff;                /* offset in current leaf piece */
    size_t           off;                 /* bytes before current piece */
    int              dirty;               /* non-zero during transient edit */
};

PT_NS_END

#endif /* piecetab_h */

#if defined(PT_IMPLEMENTATION) && !defined(pt_implemented)
#define pt_implemented

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifndef PT_FANOUT
# define PT_FANOUT 62
#endif

#ifndef PT_PAGE_SIZE
# define PT_PAGE_SIZE 65536
#endif

#ifndef PT_MAX_HOLESIZE
# define PT_MAX_HOLESIZE 64
#endif

#ifndef PT_ARENA_SIZE
# define PT_ARENA_SIZE 1024
#endif

#define PT_MASK_BITS (sizeof(pt_Mask) * CHAR_BIT)

#define pt_min(a, b) ((a) < (b) ? (a) : (b))
#define pt_max(a, b) ((a) > (b) ? (a) : (b))

#define PT_STATIC_ASSERT(cond)      PT_SA_0(cond, pt_SA_, __LINE__)
#define PT_SA_0(cond, prefix, line) PT_SA_1(cond, prefix, line)
#define PT_SA_1(cond, prefix, line) typedef char prefix##line[(cond) ? 1 : -1]

PT_NS_BEGIN

typedef size_t   pt_Mask;
typedef unsigned pt_Ver;

/* clang-format off */
typedef struct pt_Hole { char data[PT_MAX_HOLESIZE]; } pt_Hole;
PT_STATIC_ASSERT(sizeof(pt_Hole) % sizeof(void *) == 0);
/* clang-format on */

typedef struct pt_Node {
    struct pt_Node *children[PT_FANOUT]; /* interior subnodes or leaf data */
    size_t          bytes[PT_FANOUT];    /* subtree sum or piece length */
    pt_Mask         mask;                /* hole (leaf) or has-hole (inner) */
    pt_Ver          version;             /* COW version vs tree root */
    unsigned short  child_count;         /* valid child count in node */
} pt_Node;

PT_STATIC_ASSERT(PT_FANOUT <= PT_MASK_BITS);

typedef struct pt_Block {
    struct pt_Block *next; /* next block in chain */
    size_t           size; /* data capacity (bytes) */
    size_t           used; /* bytes written so far */
} pt_Block;

typedef struct pt_Arena {
    pt_Block *current; /* blocks with free space, head = active writable */
    pt_Block *full;    /* blocks that are completely full */
} pt_Arena;

typedef struct pt_Tree {
    pt_Node         root;   /* embedded root node */
    pt_State       *S;      /* owning pt_State */
    struct pt_Tree *from;   /* source tree for COW lifetime */
    pt_Arena        arena;  /* local arena for literal data (lazy) */
    size_t          bytes;  /* total bytes in this tree */
    unsigned        refc;   /* reference count */
    unsigned short  levels; /* tree height, 0 = leaf-only root */
} pt_Tree;

typedef struct pt_Pool {
    size_t obj_size; /* size of each object in this pool */
    void  *freed;    /* freelist head */
    void  *pages;    /* linked list of allocated pages */
#ifdef PT_POOL_STATS
    size_t live_obj;
#endif
} pt_Pool;

struct pt_State {
    void     *alloc_ud;         /* user data for allocator */
    pt_Alloc *allocf;           /* allocator function */
    pt_Pool   nodes;            /* pool for pt_Node */
    pt_Pool   holes;            /* pool for pt_Hole */
    pt_Pool   trees;            /* pool for pt_Tree */
    pt_Node   rt[PT_MAX_LEVEL]; /* scratch nodes for tree stitch */
    pt_Tree   empty;            /* sentinel empty tree zero-alloc */
    pt_Ver    max_version;      /* global COW version counter */
};

/* mempool */

#ifdef PT_POOL_STATS
# define ptP_stat(stmt) stmt
#else
# define ptP_stat(stmt) ((void)0)
#endif

static void ptP_init(pt_Pool *pool, size_t obj_size) {
    memset(pool, 0, sizeof(pt_Pool)), pool->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < PT_PAGE_SIZE / 2);
}

/* clang-format off */
static void ptP_free(pt_Pool *pool, void *obj)
{ ptP_stat(pool->live_obj-=1); *(void **)obj = pool->freed, pool->freed = obj; }
/* clang-format on */

static void ptP_destroy(pt_State *S, pt_Pool *pool) {
    void *next, *page = pool->pages;
    for (; page; page = next) {
        next = *(void **)((char *)page + PT_PAGE_SIZE - sizeof(void *));
        S->allocf(S->alloc_ud, page, PT_PAGE_SIZE, 0);
    }
    ptP_stat(pool->live_obj = 0), ptP_init(pool, pool->obj_size);
}

static void *ptP_ralloc(pt_Pool *pool) {
    void *obj = pool->freed;
    assert(obj), ptP_stat(pool->live_obj += 1);
    return (pool->freed = *(void **)obj), (void *)obj;
}

static void *ptP_alloc(pt_State *S, pt_Pool *p) {
    size_t sz = p->obj_size;
    char  *page, *end, *obj = (char *)p->freed;
    if (obj) return ptP_ralloc(p);
    page = (char *)S->allocf(S->alloc_ud, NULL, 0, PT_PAGE_SIZE);
    if (page == NULL) return NULL;
    end = &page[PT_PAGE_SIZE - sizeof(void *)], *(void **)end = p->pages;
    p->pages = (void *)(obj = page), page += sz, end -= sz;
    while ((page += sz) < end) *(void **)(page - sz) = page;
    *(void **)(page - sz) = NULL, ptP_stat(p->live_obj += 1);
    return (p->freed = (void *)(obj + sz)), (void *)obj;
}

static int ptP_reserve(pt_State *S, pt_Pool *p, size_t n) {
    void  *freed = p->freed, **t = &freed;
    size_t c;
    for (c = 0; c < n && *t; ++c) t = (void **)*t;
    if (c >= n) return PT_OK;
    for (p->freed = NULL; c < n; ++c) {
        void *obj = ptP_alloc(S, p);
        if (obj == NULL) return PT_ERRMEM;
        ptP_stat(p->live_obj -= 1), *t = obj, t = (void **)obj;
    }
    return *t = NULL, (p->freed = freed), PT_OK;
}

static pt_Block *ptA_alloc(pt_State *S, size_t sz) {
    pt_Block *b = (pt_Block *)S->allocf(
            S->alloc_ud, NULL, 0, sizeof(pt_Block) + sz);
    if (b == NULL) return NULL;
    return b->next = NULL, b->size = sz, b->used = 0, b;
}

static void ptA_destroy(pt_State *S, pt_Arena *a) {
    pt_Block *b, *next;
    for (b = a->current; b; b = next)
        next = b->next, S->allocf(S->alloc_ud, b, 0, 0);
    for (b = a->full; b; b = next)
        next = b->next, S->allocf(S->alloc_ud, b, 0, 0);
    memset(a, 0, sizeof(pt_Arena));
}

/* utils */

#define ptK_levels(C) ((C)->tree->levels)
#define ptK_bytes(C)  ((C)->tree->bytes)

#define ptK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define ptK_idx(C, p, l) ((int)((C)->paths[(l)] - (p)->children))

#define ptN_cc(n)       ((int)(n)->child_count)
#define ptN_setcc(n, v) ((n)->child_count = (unsigned short)(v))
#define ptN_hole(p, i)  ((pt_Hole *)((p)->children[i]))
#define ptN_lit(p, i)   ((const char *)((p)->children[i]))

#define ptM_mask(n) (((pt_Mask)1 << (n)) - 1)

/* clang-format off */
static size_t ptN_sumbytes(const pt_Node *n, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += n->bytes[i]; return s; }

static int ptM_ishole(const pt_Node *n, int i)
{ return assert(i >= 0 && i < PT_FANOUT), (n->mask & ((pt_Mask)1 << i)) != 0; }
/* clang-format on */

PT_API unsigned pt_version(pt_Blob b) { return b ? b->root.version : 0; }
PT_API size_t   pt_bytes(pt_Blob b) { return b ? b->bytes : 0; }
PT_API unsigned pt_retain(pt_Blob b) { return b ? ++((pt_Tree *)b)->refc : 0; }
PT_API pt_Blob  pt_empty(pt_State *S) { return S ? &S->empty : NULL; }

static void ptM_sethole(pt_Node *n, int i, int h) {
    assert(i >= 0 && i < PT_FANOUT);
    n->mask ^= (-!!h ^ n->mask) & ((pt_Mask)1 << i);
}

static int ptM_iterhole(pt_Mask *m, int *pi, int cc) {
    if (*m == 0) return 0;
    *pi = __builtin_ctzll(*m);
    return *m &= *m - 1, *pi < cc;
}

static void ptM_upmask(pt_Cursor *C) {
    int      l, i;
    pt_Node *p, *c;
    for (l = ptK_levels(C) - 1; l >= 0; --l) {
        i = ptK_idx(C, p = ptK_parent(C, l), l), c = p->children[i];
        ptM_sethole(p, i, (int)(c->mask & ptM_mask(ptN_cc(c))));
    }
}

static void ptM_upbytes(pt_Cursor *C, int l, pt_Delta db) {
    if (db == 0) return;
    for (; l >= 0; --l) {
        pt_Node *p;
        int      i = ptK_idx(C, p = ptK_parent(C, l), l);
        p->bytes[i] += db;
    }
    C->tree->bytes += db;
}

static void ptN_purge(pt_State *S, pt_Node *p, int k, int s, int e, pt_Ver v) {
    int i;
    assert(s <= e && e <= ptN_cc(p));
    if (k == 0) {
        pt_Mask m = p->mask & (ptM_mask(e - s) << s);
        while (ptM_iterhole(&m, &i, ptN_cc(p)))
            ptP_free(&S->holes, p->children[i]);
    } else {
        for (i = s; i < e; ++i) {
            pt_Node *c = (pt_Node *)p->children[i];
            if (c->version == v) {
                ptN_purge(S, c, k - 1, 0, ptN_cc(c), v);
                ptP_free(&S->nodes, c);
            }
        }
    }
}

static void ptN_copy(pt_Node *d, int di, const pt_Node *s, int si, int n) {
    assert(di + n <= PT_FANOUT && si + n <= PT_FANOUT);
    memcpy(&d->children[di], &s->children[si], n * sizeof(pt_Node *));
    memcpy(&d->bytes[di], &s->bytes[si], n * sizeof(size_t));
    d->mask = (d->mask & ~(ptM_mask(n) << di))
            | ((s->mask >> si) & ptM_mask(n)) << di;
}

static void ptN_move(pt_Node *d, int di, int si, int n) {
    assert(di + n <= PT_FANOUT && si + n <= PT_FANOUT);
    memmove(&d->children[di], &d->children[si], n * sizeof(pt_Node *));
    memmove(&d->bytes[di], &d->bytes[si], n * sizeof(size_t));
    d->mask = (d->mask & ~(ptM_mask(n) << di) & ~(ptM_mask(n) << si))
            | ((d->mask >> si) & ptM_mask(n)) << di;
}

static void ptN_remove(pt_State *S, pt_Node *p, int k, int s, int e) {
    assert(s <= e && e <= ptN_cc(p));
    ptN_purge(S, p, k, s, e, p->version);
    ptN_move(p, s, e, ptN_cc(p) - e);
    p->child_count -= e - s;
}

static void ptN_makespace(pt_Node *p, int i, int n) {
    assert(ptN_cc(p) + n <= PT_FANOUT && i <= ptN_cc(p));
    ptN_move(p, i + n, i, ptN_cc(p) - i);
    p->child_count += n;
}

/* lifetime */

static void *ptS_defallocf(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    (void)ud, (void)osize;
    if (nsize == 0) return free(p), (void *)NULL;
    np = realloc(p, nsize);
    if (np == NULL) abort(); /* failure is unrecoverable by default */
    return np;
}

PT_API pt_State *pt_open(pt_Alloc *allocf, void *ud) {
    pt_State *S;
    if (allocf == NULL) allocf = &ptS_defallocf;
    S = (pt_State *)allocf(ud, NULL, 0, sizeof(pt_State));
    if (!S) return NULL;
    memset(S, 0, sizeof(pt_State));
    S->alloc_ud = ud, S->allocf = allocf;
    ptP_init(&S->nodes, sizeof(pt_Node));
    ptP_init(&S->holes, sizeof(pt_Hole));
    ptP_init(&S->trees, sizeof(pt_Tree));
    S->empty.S = S, S->empty.refc = 1;
    return S;
}

PT_API void pt_close(pt_State *S) {
    if (S == NULL) return;
    ptP_destroy(S, &S->nodes);
    ptP_destroy(S, &S->holes);
    ptP_destroy(S, &S->trees);
    S->allocf(S->alloc_ud, S, sizeof(pt_State), 0);
}

PT_API pt_Alloc *pt_getallocf(pt_State *S, void **pud) {
    if (S == NULL) return NULL;
    if (pud) *pud = S->alloc_ud;
    return S->allocf;
}

PT_API pt_Blob pt_from(pt_State *S, const char *s, size_t len) {
    pt_Tree *t;
    if (!S || (!s && len > 0)) return NULL;
    if (!(t = (pt_Tree *)ptP_alloc(S, &S->trees))) return NULL;
    memset(t, 0, sizeof(pt_Tree));
    t->S = S, t->refc = 1, t->from = &S->empty;
    if (len == 0) return t;
    t->root.children[0] = (pt_Node *)s;
    t->root.bytes[0] = len, t->bytes = len;
    ptN_setcc(&t->root, 1);
    t->root.version = ++S->max_version;
    return t;
}

PT_API unsigned pt_release(pt_Blob b) {
    pt_Tree *t = (pt_Tree *)b;
    if (t == NULL || t == &t->S->empty) return 0;
    if (t->refc > 1) return --t->refc;
    for (;;) {
        pt_Tree *nt = t->from;
        pt_Node *r = (assert(nt), &t->root);
        ptN_purge(t->S, r, t->levels, 0, ptN_cc(r), r->version);
        ptA_destroy(t->S, &t->arena);
        ptP_free(&t->S->trees, t);
        if (nt == &nt->S->empty) return 0;
        if (nt->refc > 1) return --nt->refc, 0;
        t = nt;
    }
}

/* navigate */

static void ptK_findleaf(pt_Cursor *C, int l, size_t *poff) {
    for (; l <= ptK_levels(C); ++l) {
        pt_Node *p = ptK_parent(C, l);
        int      i;
        for (i = 0; i < ptN_cc(p) && *poff >= p->bytes[i]; ++i)
            *poff -= p->bytes[i], C->off += p->bytes[i];
        C->paths[l] = &p->children[assert(i < ptN_cc(p)), i];
    }
}

static int ptK_locend(pt_Cursor *C) {
    pt_Node *n = &C->tree->root;
    int      l;
    if (ptK_levels(C) == 0 && ptN_cc(n) == 0)
        return C->paths[0] = n->children, C->off = 0, C->poff = 0, 0;
    for (l = 0; l < ptK_levels(C); ++l)
        n = *(C->paths[l] = &n->children[ptN_cc(n) - 1]);
    C->paths[l] = &n->children[assert(ptN_cc(n)), ptN_cc(n) - 1];
    C->poff = n->bytes[ptN_cc(n) - 1], C->off = ptK_bytes(C) - C->poff;
    return 1;
}

PT_API int pt_seek(pt_Cursor *C, pt_Blob b, size_t off) {
    if (C == NULL || b == NULL) return PT_ERRPARAM;
    memset(C, 0, sizeof(pt_Cursor)), C->tree = (pt_Tree *)b;
    if (off >= b->bytes) return ptK_locend(C), PT_OK;
    return ptK_findleaf(C, 0, &off), (C->poff = off), PT_OK;
}

PT_API int pt_locate(pt_Cursor *C, size_t off) {
    if (C == NULL || C->tree == NULL) return PT_ERRPARAM;
    C->off = C->poff = 0;
    if (off >= ptK_bytes(C)) return ptK_locend(C), PT_OK;
    return ptK_findleaf(C, 0, &off), (C->poff = off), PT_OK;
}

static int ptK_forwardoff(pt_Cursor *C, size_t d) {
    pt_Node *p;
    int      l, i = ptK_idx(C, p = ptK_parent(C, ptK_levels(C)), ptK_levels(C));
    size_t   in = p->bytes[i] - C->poff;
    if (d < in) return C->poff += d, 0;
    d -= in, C->off += p->bytes[i], C->poff = 0;
    for (l = ptK_levels(C); l >= 0; --l) {
        i = ptK_idx(C, p = ptK_parent(C, l), l) + 1;
        for (; i < ptN_cc(p) && d >= p->bytes[i]; ++i)
            d -= p->bytes[i], C->off += p->bytes[i];
        if (i < ptN_cc(p)) break;
    }
    assert(l >= 0 && i < ptN_cc(p)), C->paths[l] = &p->children[i];
    return ptK_findleaf(C, l + 1, &d), C->poff = d, 1;
}

static int ptK_backwardoff(pt_Cursor *C, size_t d) {
    pt_Node *p;
    int      l, i;
    if (d <= C->poff) return C->poff -= d, 0;
    d -= C->poff, C->poff = 0;
    for (l = ptK_levels(C); l >= 0; --l) {
        i = ptK_idx(C, p = ptK_parent(C, l), l) - 1;
        for (; i >= 0 && d > p->bytes[i]; --i)
            d -= p->bytes[i], C->off -= p->bytes[i];
        if (i >= 0) break;
    }
    assert(l >= 0 && i >= 0), d = p->bytes[i] - d, C->off -= p->bytes[i];
    C->paths[l] = &p->children[i];
    return ptK_findleaf(C, l + 1, &d), C->poff = d, 1;
}

PT_API int pt_advance(pt_Cursor *C, pt_Delta d) {
    size_t off;
    if (C == NULL || C->tree == NULL) return PT_ERRPARAM;
    off = pt_offset(C);
    if (d == 0 || ptK_bytes(C) == 0) return PT_OK;
    if (d < 0 && (size_t)-d > off) return ptK_backwardoff(C, off), PT_OK;
    if ((off + d) >= ptK_bytes(C)) return ptK_locend(C), PT_OK;
    if (d < 0) return ptK_backwardoff(C, (size_t)(-d)), PT_OK;
    return ptK_forwardoff(C, d), PT_OK;
}

/* read */

PT_API const char *pt_piece(pt_Cursor *C, size_t *plen) {
    pt_Node *p;
    int      i;
    if (C == NULL || C->tree == NULL) return NULL;
    i = ptK_idx(C, p = ptK_parent(C, ptK_levels(C)), ptK_levels(C));
    if (C->poff >= p->bytes[i]) return plen && (*plen = 0), (const char *)NULL;
    if (plen) *plen = p->bytes[i] - C->poff;
    return ptN_lit(p, i) + C->poff;
}

PT_API const char *pt_next(pt_Cursor *C, size_t *plen) {
    int         i, l;
    size_t      bc;
    const char *s;
    pt_Node    *p;
    if (C == NULL || C->tree == NULL) return NULL;
    l = ptK_levels(C), i = ptK_idx(C, p = ptK_parent(C, l), l);
    if (C->poff == p->bytes[i]) return plen && (*plen = 0), (const char *)NULL;
    s = ptN_lit(p, i) + C->poff, bc = p->bytes[i] - C->poff;
    while (i + 1 >= ptN_cc(p) && --l >= 0)
        i = ptK_idx(C, p = ptK_parent(C, l), l);
    if (l < 0) return C->poff += bc, plen && (*plen = bc), s;
    C->paths[l] += 1, C->off += bc + C->poff, C->poff = 0;
    while (++l <= ptK_levels(C)) C->paths[l] = &ptK_parent(C, l)->children[0];
    return plen && (*plen = bc), s;
}

PT_API const char *pt_prev(pt_Cursor *C, size_t *plen) {
    pt_Node *p;
    int      i, l;
    if (C == NULL || C->tree == NULL) return NULL;
    l = ptK_levels(C), i = ptK_idx(C, p = ptK_parent(C, l), l);
    if (C->poff > 0)
        return plen && (*plen = C->poff), C->poff = 0, ptN_lit(p, i);
    if (C->off == 0) return plen && (*plen = 0), (const char *)NULL;
    while (i <= 0 && --l >= 0) i = ptK_idx(C, p = ptK_parent(C, l), l);
    assert(l >= 0 && i > 0), C->paths[l] -= 1, i -= 1;
    while (++l <= ptK_levels(C))
        p = ptK_parent(C, l), C->paths[l] = &p->children[i = ptN_cc(p) - 1];
    C->off -= p->bytes[i], C->poff = 0;
    return plen && (*plen = p->bytes[i]), ptN_lit(p, i);
}

PT_API size_t pt_read(pt_Cursor *C, char *buf, size_t len) {
    size_t n, total = 0;
    if (C == NULL || C->tree == NULL || buf == NULL) return 0;
    for (; len > 0; pt_advance(C, (pt_Delta)n)) {
        const char *p = pt_piece(C, &n);
        if (n == 0) break;
        n = pt_min(n, len), memcpy(buf, p, n);
        buf += n, len -= n, total += n;
    }
    return total;
}

/* commit */

static size_t ptC_holebytes(pt_Node *n, int k) {
    pt_Mask m = n->mask;
    size_t  total = 0;
    int     i;
    while (ptM_iterhole(&m, &i, ptN_cc(n)))
        total += k ? ptC_holebytes(n->children[i], k - 1) : n->bytes[i];
    return total;
}

static void ptC_freezeleaf(pt_State *S, pt_Node *n, char **ppos) {
    pt_Mask m = n->mask;
    int     i;
    while (ptM_iterhole(&m, &i, ptN_cc(n))) {
        pt_Hole *h = (pt_Hole *)n->children[i];
        memcpy(*ppos, h->data, n->bytes[i]);
        n->children[i] = (pt_Node *)*ppos, *ppos += n->bytes[i];
        ptP_free(&S->holes, h);
    }
    n->mask = 0;
}

static void ptC_freeze(pt_State *S, pt_Tree *tree, char *pos) {
    int       i, l = 0;
    pt_Node  *p;
    pt_Cursor C;
    C.tree = tree, C.paths[0] = tree->root.children;
    for (;;) {
        p = ptK_parent(&C, l);
        while (l > 0 && p->mask == 0) --l, p = ptK_parent(&C, l);
        if (p->mask == 0) break;
        if (l >= tree->levels)
            ptC_freezeleaf(S, p, &pos);
        else {
            ptM_iterhole(&p->mask, &i, ptN_cc(p)), C.paths[l] = &p->children[i];
            C.paths[++l] = p->children[i]->children;
        }
    }
}

PT_API pt_Blob pt_commit(pt_Cursor *C) {
    size_t total;
    char  *buf;
    if (C == NULL || C->tree == NULL) return NULL;
    if (!C->dirty) return pt_retain(C->tree), C->tree;
    if ((total = ptC_holebytes(&C->tree->root, C->tree->levels)) > 0) {
        buf = pt_reserve(C, total);
        if (buf == NULL) return NULL;
        ptC_freeze(C->tree->S, C->tree, buf);
        C->tree->arena.current->used += total;
    }
    return C->dirty = 0, C->tree;
}

PT_API void pt_rollback(pt_Cursor *c) {
    pt_Tree *from;
    if (c == NULL || c->tree == NULL || !c->dirty) return;
    if ((from = c->tree->from) != NULL && from->refc == 1) from = NULL;
    pt_release(c->tree), c->tree = from, c->dirty = 0;
}

/* literal */

static int ptK_markdirty(pt_Cursor *C) {
    pt_State *S = C->tree->S;
    pt_Tree  *old = C->tree, *nt;
    if (C->dirty) return PT_OK;
    if (!(nt = (pt_Tree *)ptP_alloc(S, &S->trees))) return PT_ERRMEM;
    *nt = *old, nt->arena.current = NULL, nt->arena.full = NULL;
    nt->root.version = ++S->max_version, nt->refc = 1;
    nt->from = old, pt_retain(old); /* keep source alive: COW lifetime */
    C->paths[0] = nt->root.children + (C->paths[0] - old->root.children);
    return C->tree = nt, C->dirty = 1, PT_OK;
}

PT_API char *pt_reserve(pt_Cursor *C, size_t len) {
    pt_State *S;
    pt_Block *b, **pp;
    if (C == NULL || C->tree == NULL) return NULL;
    if (ptK_markdirty(C) != PT_OK) return NULL;
    S = C->tree->S;
    if (len == 0) len = PT_ARENA_SIZE;
    b = C->tree->arena.current;
    if (b && b->size - b->used >= len) return (char *)(b + 1) + b->used;
    for (pp = &C->tree->arena.current; *pp; pp = &(*pp)->next) {
        if (b = *pp, b->size - b->used >= len) {
            *pp = b->next;
            b->next = C->tree->arena.current;
            C->tree->arena.current = b;
            return (char *)(b + 1) + b->used;
        }
    }
    b = ptA_alloc(S, pt_max(len, PT_ARENA_SIZE));
    if (b == NULL) return NULL;
    b->next = C->tree->arena.current;
    return C->tree->arena.current = b, (char *)(b + 1);
}

PT_API char *pt_scratch(pt_Cursor *C, size_t *plen) {
    pt_Block *b;
    if (C == NULL || C->tree == NULL || plen == NULL) return NULL;
    if ((b = C->tree->arena.current) == NULL) return *plen = 0, (char *)NULL;
    *plen = (assert(b->used < b->size), b->size - b->used);
    return (char *)(b + 1) + b->used;
}

PT_API const char *pt_literal(pt_Cursor *C, size_t len) {
    const char *s;
    pt_Block   *b;
    size_t      n;
    if (C == NULL || C->tree == NULL || len == 0) return NULL;
    if (ptK_markdirty(C) != PT_OK) return NULL;
    if ((b = C->tree->arena.current) == NULL || (n = b->size - b->used) < len)
        return NULL;
    s = (char *)(b + 1) + b->used, b->used += len;
    if (b->used == b->size) {
        C->tree->arena.current = b->next;
        b->next = C->tree->arena.full;
        C->tree->arena.full = b;
    }
    return s;
}

/* insert */

/* makeroom needs at most 2 free slots; a split of a full node leaves
 * FANOUT/2 free in the cursor's half, so require FANOUT >= 4. */
PT_STATIC_ASSERT(PT_FANOUT >= 4);

#define ptH_fit(p, i, len)  ((p)->bytes[i] + (len) <= PT_MAX_HOLESIZE)
#define ptH_reserve(S, len) ptP_reserve(S, &S->holes, (len))

static pt_Node *ptH_new(pt_State *S, const char *s, size_t len) {
    pt_Hole *h = (pt_Hole *)ptP_alloc(S, &S->holes);
    assert(h && len <= PT_MAX_HOLESIZE);
    return memcpy(h->data, s, len), (pt_Node *)h;
}

static void ptH_append(pt_Node *n, int i, size_t d, const char *s, size_t len) {
    pt_Hole *h = ptN_hole(n, i);
    size_t   end = n->bytes[i];
    assert((size_t)d <= end && end + len <= PT_MAX_HOLESIZE);
    memmove(h->data + d + len, h->data + d, end - d);
    memcpy(h->data + d, s, len);
    n->bytes[i] = end + len;
}

static pt_Node *ptK_cow(pt_Cursor *C, int l, int d) {
    pt_Node *p;
    int      i = ptK_idx(C, p = ptK_parent(C, l), l) + d;
    pt_Node *n = p->children[i], *nn;
    if (l == ptK_levels(C) || n->version == C->tree->root.version) return n;
    nn = (pt_Node *)ptP_ralloc(&C->tree->S->nodes);
    *nn = *n, nn->version = C->tree->root.version;
    if (d == 0) C->paths[l + 1] = &nn->children[C->paths[l + 1] - n->children];
    return p->children[i] = nn;
}

static int ptK_beginedit(pt_Cursor *C, size_t need) {
    int r, l;
    if ((r = ptP_reserve(C->tree->S, &C->tree->S->nodes, need))) return r;
    if ((r = ptK_markdirty(C))) return r;
    for (l = 0; l < ptK_levels(C); ++l) ptK_cow(C, l, 0);
    return PT_OK;
}

static void ptI_onepiece(pt_Cursor *C, const char *s, size_t len, int h) {
    pt_Node *r = &C->tree->root;
    if (h)
        r->children[0] = ptH_new(C->tree->S, s, len), C->poff = len;
    else
        r->children[0] = (pt_Node *)s, C->poff = 0;
    r->mask = 0, ptM_sethole(r, 0, h);
    r->bytes[0] = len, C->tree->bytes = len;
    C->paths[0] = &r->children[0], ptN_setcc(r, 1), C->off = 0;
}

static void ptI_splitroot(pt_Cursor *C) {
    pt_Node *r = &C->tree->root, save = *r;
    pt_Node *pp = (pt_Node *)ptP_ralloc(&C->tree->S->nodes);
    pt_Node *nw = (pt_Node *)ptP_ralloc(&C->tree->S->nodes);
    int      i = ptK_idx(C, r, 0), mid = ptN_cc(&save) / 2;
    int      nc = ptN_cc(&save) - mid;
    *pp = save, pp->mask &= ptM_mask(mid), ptN_setcc(pp, mid);
    nw->mask = 0, ptN_copy(nw, 0, &save, mid, nc), ptN_setcc(nw, nc);
    pp->version = nw->version = C->tree->root.version;
    r->children[0] = pp, r->children[1] = nw, ptN_setcc(r, 2);
    r->bytes[0] = ptN_sumbytes(pp, 0, mid);
    r->bytes[1] = C->tree->bytes - r->bytes[0];
    ptM_sethole(r, 0, (int)pp->mask), ptM_sethole(r, 1, (int)nw->mask);
    C->tree->levels++;
    memmove(C->paths + 1, C->paths, C->tree->levels * sizeof(pt_Node **));
    C->paths[0] = &r->children[i >= mid];
    C->paths[1] = &(*C->paths[0])->children[i < mid ? i : i - mid];
}

static void ptI_splitchild(pt_Cursor *C, int l) {
    pt_State *S = C->tree->S;
    pt_Node  *p, *nw, *nd = ptK_cow(C, l, 0);
    int       cs, mid = ptN_cc(nd) / 2, nc = ptN_cc(nd) - mid;
    int       i = ptK_idx(C, p = ptK_parent(C, l), l);
    nw = (pt_Node *)ptP_ralloc(&S->nodes), nw->version = C->tree->root.version;
    nw->mask = 0, ptN_copy(nw, 0, nd, mid, nc), ptN_setcc(nw, nc);
    nd->mask &= ptM_mask(mid), ptN_setcc(nd, mid);
    ptN_makespace(p, i + 1, 1), p->children[i + 1] = nw;
    p->bytes[i] = ptN_sumbytes(nd, 0, mid);
    p->bytes[i + 1] = ptN_sumbytes(nw, 0, nc);
    ptM_sethole(p, i, (int)nd->mask), ptM_sethole(p, i + 1, (int)nw->mask);
    if ((cs = ptK_idx(C, nd, l + 1)) >= mid) {
        C->paths[l] = &p->children[i + 1];
        C->paths[l + 1] = &nw->children[cs - mid];
    }
}

static void ptI_stitchrt(pt_Cursor *C, pt_Node *rt, int s, int e) {
    int      l, i;
    pt_Node *p;
    for (l = ptK_levels(C); l >= 0; --l)
        if (ptN_cc(ptK_parent(C, l)) < PT_FANOUT) break;
    if (l < 0) ptI_splitroot(C), l = 1;
    for (; l < ptK_levels(C); ++l) ptI_splitchild(C, l);
    i = ptK_idx(C, p = ptK_parent(C, ptK_levels(C)), ptK_levels(C));
    ptN_makespace(p, i, e - s), ptN_copy(p, i, rt, s, e - s);
}

static void ptI_fillrt(pt_Cursor *C, const char *s, size_t len, int h) {
    pt_Node *rt = C->tree->S->rt, *p;
    int      i = ptK_idx(C, p = ptK_parent(C, ptK_levels(C)), ptK_levels(C));
    int      ph = ptM_ishole(p, i);
    size_t   po = C->poff, n = p->bytes[i], rm = n - po;
    if (po) p->bytes[i] -= rm, C->off += po, C->paths[ptK_levels(C)] += 1;
    rt->children[0] = h ? ptH_new(C->tree->S, s, len) : (pt_Node *)s;
    rt->bytes[0] = len, ptM_sethole(rt, 0, h), ptN_setcc(rt, 1);
    if (po > 0 && po < n) {
        pt_Node *nn = (pt_Node *)(ptN_lit(p, i) + po);
        if (ph) nn = ptH_new(C->tree->S, ptN_hole(p, i)->data + po, rm);
        rt->children[1] = nn, rt->bytes[1] = rm, ptM_sethole(rt, 1, ph);
        ptN_setcc(rt, 2);
    }
}

static void ptI_splitins(pt_Cursor *C, const char *s, size_t len, int h) {
    pt_Node *p, *rt = &C->tree->S->rt[0];
    int      l, ph, cc, need, m, i;
    size_t   n, po = C->poff;
    l = ptK_levels(C), i = ptK_idx(C, p = ptK_parent(C, l), l);
    n = p->bytes[i], ph = ptM_ishole(p, i), cc = ptN_cc(p);
    assert(po <= n), need = 1 + (po > 0 && po < n);
    rt->mask = 0, rt->version = C->tree->root.version;
    ptI_fillrt(C, s, len, h), i = ptK_idx(C, p, l);
    if ((m = pt_min(need, PT_FANOUT - cc)) > 0)
        ptN_makespace(p, i, m), ptN_copy(p, i, rt, 0, m);
    if (m == need)
        C->paths[l] += need - (po > 0), ptM_upbytes(C, l - 1, len);
    else {
        pt_Delta shrink = po ? (pt_Delta)(n - po) : 0;
        pt_Delta db = (pt_Delta)ptN_sumbytes(rt, 0, m);
        C->paths[l] += m, ptM_upbytes(C, l - 1, db - shrink);
        ptI_stitchrt(C, rt, m, need), l = ptK_levels(C);
        C->paths[l] += need - m - (po > 0);
        ptM_upbytes(C, l - 1, (pt_Delta)ptN_sumbytes(rt, 0, ptN_cc(rt)) - db);
    }
    po == n ? (C->poff = len) : (C->off += len, C->poff = 0);
}

PT_API int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len) {
    int      i, r, l;
    pt_Node *p;
    if (!C || !C->tree || len > PT_MAX_HOLESIZE) return PT_ERRPARAM;
    if (!s && len > 0) return PT_ERRPARAM;
    if (del == 0 && len == 0) return PT_OK;
    if ((r = ptK_beginedit(C, 6 * (l = ptK_levels(C)) + 8)) != PT_OK) return r;
    if (len > 0 && (r = ptH_reserve(C->tree->S, 2))) return r;
    if (del > 0 && (r = pt_remove(C, del)) != PT_OK) return r;
    if (len == 0) return PT_OK;
    if (ptK_bytes(C) == 0) return ptI_onepiece(C, s, len, 1), PT_OK;
    i = ptK_idx(C, p = ptK_parent(C, ptK_levels(C)), ptK_levels(C));
    if (C->poff && ptM_ishole(p, i) && ptH_fit(p, i, len)) {
        ptH_append(p, i, C->poff, s, len), C->poff += len;
        return ptM_upbytes(C, l - 1, (pt_Delta)len), ptM_upmask(C), PT_OK;
    }
    if (C->poff == 0 && i && ptM_ishole(p, i - 1) && ptH_fit(p, i - 1, len)) {
        ptH_append(p, i - 1, p->bytes[i - 1], s, len), C->off += len;
        return ptM_upbytes(C, l - 1, (pt_Delta)len), ptM_upmask(C), PT_OK;
    }
    return ptI_splitins(C, s, len, 1), ptM_upmask(C), PT_OK;
}

PT_API int pt_append(pt_Cursor *C, const char *s, size_t len) {
    pt_Node *p;
    int      i, r;
    if (!C || !C->tree || !s) return PT_ERRPARAM;
    if (len == 0) return PT_OK;
    if ((r = ptK_beginedit(C, 2 * ptK_levels(C) + 3)) != PT_OK) return r;
    if (ptK_bytes(C) == 0)
        return ptI_onepiece(C, s, len, 0), C->poff = len, PT_OK;
    i = ptK_idx(C, p = ptK_parent(C, ptK_levels(C)), ptK_levels(C));
    if (C->poff == 0 && i > 0 && !ptM_ishole(p, i - 1)
        && ptN_lit(p, i - 1) + p->bytes[i - 1] == s) {
        p->bytes[i - 1] += len, C->off += len;
        return ptM_upbytes(C, ptK_levels(C) - 1, (pt_Delta)len), PT_OK;
    }
    if (C->poff == p->bytes[i] && !ptM_ishole(p, i)
        && ptN_lit(p, i) + p->bytes[i] == s) {
        p->bytes[i] += len, C->poff += len;
        return ptM_upbytes(C, ptK_levels(C) - 1, (pt_Delta)len), PT_OK;
    }
    return ptI_splitins(C, s, len, 0), ptM_upmask(C), PT_OK;
}

PT_API int pt_insert(pt_Cursor *C, const char *s, size_t len) {
    int r = pt_append(C, s, len);
    if (r == PT_OK && len > 0) pt_advance(C, -(pt_Delta)len);
    return r;
}

/* remove */

static void ptH_remove(pt_Node *n, int i, size_t d, size_t len) {
    pt_Hole *h = ptN_hole(n, i);
    size_t   end = n->bytes[i];
    assert((size_t)d <= end && d + len <= end);
    memmove(h->data + d, h->data + d + len, end - (d + len));
    n->bytes[i] = end - len;
}

static void ptD_trimright(pt_Cursor *L, int l) {
    pt_Node *p = ptK_parent(L, l);
    int      i = ptK_idx(L, p, l);
    size_t   bc = p->bytes[i];
    if (L->poff > 0 && L->poff < bc) {
        p->bytes[i] = L->poff;
        ptM_upbytes(L, l - 1, -(pt_Delta)(bc - L->poff));
    }
}

static void ptD_trimleft(pt_Cursor *R, int l) {
    pt_Node *p = ptK_parent(R, l);
    int      i = ptK_idx(R, p, l);
    size_t   old = p->bytes[i];
    size_t   poffR = R->poff;
    if (poffR > 0 && poffR < old) {
        if (ptM_ishole(p, i))
            ptH_remove(p, i, 0, poffR);
        else {
            p->children[i] = (pt_Node *)(ptN_lit(p, i) + poffR);
            p->bytes[i] -= poffR;
        }
        ptM_upbytes(R, l - 1, -(pt_Delta)poffR), R->poff = 0;
    }
}

static void ptD_cutrange(pt_Cursor *L, pt_Cursor *R, pt_Node *rt, int fl) {
    pt_State *S = L->tree->S;
    int       l = ptK_levels(L), kl, k, i, cc;
    pt_Node  *p;
    for (kl = l; kl > fl; --kl) {
        p = ptK_parent(L, kl), i = ptK_idx(L, p, kl), cc = ptN_cc(p);
        i += !(kl == l && L->poff == 0);
        ptM_upbytes(L, kl - 1, -(pt_Delta)ptN_sumbytes(p, i, cc));
        k = l - kl, ptN_remove(S, p, k, i, cc);
        p = ptK_parent(R, kl), i = ptK_idx(R, p, kl), cc = ptN_cc(p);
        i += !(kl == l && R->poff < p->bytes[i]);
        ptN_setcc(&rt[k], cc - i);
        ptN_copy(&rt[k], 0, p, i, cc - i);
        ptN_remove(S, p, k, 0, i), ptN_setcc(p, 0);
    }
    k = l - fl, p = ptK_parent(R, fl);
    i = ptK_idx(R, p, fl), cc = ptN_cc(p);
    i += !(fl == l && R->poff < p->bytes[i]);
    ptN_setcc(&rt[k], cc - i);
    ptN_copy(&rt[k], 0, p, i, cc - i), ptN_setcc(p, i);
    i = ptK_idx(L, p, fl), i += !(fl == l && L->poff == 0);
    ptM_upbytes(L, fl - 1, -(pt_Delta)ptN_sumbytes(p, i, cc));
    ptN_remove(S, p, k, i, ptN_cc(p));
}

static int ptD_makechain(pt_Cursor *C, int from, int to) {
    pt_Node *p, *nn, ***cp = C->paths + to;
    int      l, r = 0;
    if (assert(from < to), from < 0) {
        nn = (pt_Node *)ptP_ralloc(&C->tree->S->nodes);
        p = &C->tree->root, *nn = *p;
        p->bytes[0] = C->tree->bytes, ptM_sethole(p, 0, (int)nn->mask);
        p->children[0] = nn, p->child_count = 1;
        memmove(cp + 2, cp + 1, (ptK_levels(C) - to) * sizeof(pt_Node **));
        C->tree->levels += 1, from = 0, to += 1, cp += 1, r = 1;
    }
    for (l = from; l < to; ++l) {
        nn = (pt_Node *)ptP_ralloc(&C->tree->S->nodes);
        p = ptK_parent(C, l), nn->child_count = 0;
        nn->version = C->tree->root.version, nn->mask = 0;
        p->bytes[ptN_cc(p)] = 0, C->paths[l] = &p->children[ptN_cc(p)];
        p->children[ptN_cc(p)] = nn, p->child_count += 1;
        ptM_sethole(p, ptN_cc(p) - 1, 0);
    }
    return *cp = &nn->children[0], r;
}

static int ptD_findroom(pt_Cursor *C, pt_Node *rt, int l) {
    int      i, fl, c;
    pt_Node *p;
    for (fl = l - 1; fl >= 0; --fl) {
        p = ptK_parent(C, fl), i = ptK_idx(C, p, fl);
        if (i < PT_FANOUT - 1) break;
    }
    if (fl >= 0 && (c = ptN_cc(p) - i - 1) > 0) {
        int k = ptK_levels(C) - fl;
        ptM_upbytes(C, fl - 1, -(pt_Delta)ptN_sumbytes(p, i + 1, ptN_cc(p)));
        rt[k].child_count = 0;
        ptN_copy(&rt[k], 0, p, i + 1, c);
        ptN_setcc(p, i + 1), ptN_setcc(&rt[k], c);
    }
    return ptD_makechain(C, fl, l);
}

static void ptD_backwardnode(pt_Cursor *C, int d, int l) {
    pt_Node *p = ptK_parent(C, l);
    int      dl, i = ptK_idx(C, p, l);
    if (d > i) {
        d -= i + 1, dl = l;
        while (--dl >= 0 && ptK_idx(C, ptK_parent(C, dl), dl) == 0) continue;
        C->paths[assert(dl >= 0), dl] -= 1;
        while (++dl <= l)
            p = ptK_parent(C, dl), C->paths[dl] = &p->children[ptN_cc(p) - 1];
    }
    C->paths[l] -= d;
}

static int ptD_balancenode(pt_Node **ns, int left, pt_Delta *ds) {
    int d, l = ptN_cc(ns[0]), r = ptN_cc(ns[1]);
    d = l - ((l + r + (left != 0)) >> 1);
    assert(d != 0);
    if (d < 0) {
        ptN_copy(ns[0], l, ns[1], 0, -d);
        ptN_move(ns[1], 0, -d, r + d);
        *ds = -(pt_Delta)ptN_sumbytes(ns[0], l, l - d);
    } else {
        ptN_move(ns[1], d, 0, r);
        ptN_copy(ns[1], 0, ns[0], l - d, d);
        *ds = (pt_Delta)ptN_sumbytes(ns[1], 0, d);
    }
    return ptN_setcc(ns[0], l - d), ptN_setcc(ns[1], r + d), d;
}

static int ptD_foldnode(pt_Cursor *C, int lfirst, int l) {
    pt_Node  *p = ptK_parent(C, l), ***cp = &C->paths[l];
    int       cl, cr, dn, i = ptK_idx(C, p, l);
    pt_Node **ns = &p->children[i], *o = *ns;
    pt_Delta  ds;
    if (assert(ptN_cc(p) > 1), ptN_cc(ns[0]) > PT_FANOUT / 2) return 0;
    if ((i && lfirst) || i == ptN_cc(p) - 1) ns -= 1, i -= 1;
    ns[0] = ptK_cow(C, l, -(*ns != o)), ns[1] = ptK_cow(C, l, *ns == o);
    if ((cl = ptN_cc(ns[0])) + (cr = ptN_cc(ns[1])) <= PT_FANOUT) {
        ptN_copy(ns[0], cl, ns[1], 0, cr);
        ns[0]->child_count += cr, ns[1]->child_count -= cr;
        p->bytes[i] += p->bytes[i + 1];
        ns[0]->mask |= ns[1]->mask << cl;
        if (*ns != o)
            cp[1] = &ns[0]->children[cp[1] - ns[1]->children + cl], cp[0] -= 1;
        return ptN_remove(C->tree->S, p, ptK_levels(C) - l, i + 1, i + 2), 1;
    }
    dn = ptD_balancenode(ns, *ns != o, &ds);
    assert(dn != 0 && (dn < 0) != (*ns != o));
    p->bytes[i] -= ds, p->bytes[i + 1] += ds;
    ptM_sethole(p, i, (int)ns[0]->mask);
    ptM_sethole(p, i + 1, (int)ns[1]->mask);
    if (*ns != o) cp[1] += dn;
    return 0;
}

static void ptD_rebalance(pt_Cursor *C, int l) {
    assert(l == 0 || l < ptK_levels(C));
    for (; l > 0; --l) {
        pt_Node *p = ptK_parent(C, l);
        if (ptN_cc(p->children[ptK_idx(C, p, l)]) >= PT_FANOUT / 2) break;
        assert(ptN_cc(p) > 1);
        if (!ptD_foldnode(C, 0, l)) break;
    }
    while (ptK_levels(C) > 0 && ptN_cc(&C->tree->root) == 1) {
        pt_Node *only = ptK_parent(C, 1);
        int      i = ptK_idx(C, only, 1);
        C->tree->root = *only;
        ptP_free(&C->tree->S->nodes, only);
        C->tree->levels--, C->paths[0] += i;
        memmove(C->paths + 1, C->paths + 2, ptK_levels(C) * sizeof(pt_Node **));
    }
}

/* clang-format off */
PT_STATIC int ptD_checkstitch(pt_Cursor *C)
{ return ptP_reserve(C->tree->S, &C->tree->S->nodes, ptK_levels(C) + 2); }
/* clang-format on */

static void ptD_stitchnode(pt_Cursor *L, pt_Node *rt) {
    int k, d = 0, l = ptK_levels(L);
    for (k = 0; k <= ptK_levels(L); ++k) {
        int      m, fl, r, kl = ptK_levels(L) - k, rtcc = ptN_cc(&rt[k]);
        pt_Node *p = ptK_parent(L, kl);
        ptN_setcc(&rt[k], 0);
        if ((m = pt_min(rtcc, PT_FANOUT - ptN_cc(p))) > 0) {
            ptN_copy(p, ptN_cc(p), &rt[k], 0, m), ptN_setcc(p, ptN_cc(p) + m);
            ptM_upbytes(L, kl - 1, (pt_Delta)ptN_sumbytes(&rt[k], 0, m));
            ptM_upmask(L);
        }
        if (!(m < rtcc || kl == 0)) continue;
        if (kl == 0 && ptN_cc(&L->tree->root) == 1)
            ptD_rebalance(L, 0), l -= (k - ptK_levels(L));
        for (fl = kl; fl < l; ++fl) ptD_foldnode(L, (fl == kl), fl);
        if (k) ptD_backwardnode(L, d, l);
        if (!(m < rtcc)) continue;
        l = kl, d = k ? PT_FANOUT - ptK_idx(L, ptK_parent(L, l), l) : m;
        r = ptD_findroom(L, rt, l), l += r, p = ptK_parent(L, l);
        ptN_copy(p, 0, &rt[k], m, ptN_setcc(p, rtcc - m));
        ptM_upbytes(L, l - 1, (pt_Delta)ptN_sumbytes(&rt[k], m, rtcc));
        ptM_upmask(L);
    }
}

static int ptD_mergeleaf(pt_Cursor *C, pt_Node *rt) {
    int      cc, hL, hR, merged = 0, l = ptK_levels(C);
    pt_Node *p = ptK_parent(C, l);
    size_t   d = 0, bc = p->bytes[(cc = ptN_cc(p)) - 1];
    hL = ptM_ishole(p, cc - 1), hR = ptM_ishole(rt, 0);
    if (!hL && !hR && ptN_lit(p, cc - 1) + bc == ptN_lit(rt, 0))
        merged = 1;
    else if (hL && hR) {
        d = pt_min(rt->bytes[0], PT_MAX_HOLESIZE - bc);
        ptH_append(p, cc - 1, bc, ptN_hole(rt, 0)->data, d);
        if (d < rt->bytes[0])
            ptH_remove(rt, 0, 0, d);
        else
            ptP_free(&C->tree->S->holes, ptN_hole(rt, 0)), merged = 1, d = 0;
    }
    if (!merged)
        C->off += C->poff, C->poff = 0, C->paths[l] = &p->children[cc];
    else {
        rt->bytes[0] += bc, rt->children[0] = p->children[cc - 1];
        ptM_upbytes(C, l - 1, -(pt_Delta)bc);
        if (ptK_idx(C, p, l) == cc) C->off -= bc, C->poff = bc;
        C->paths[l] = &p->children[cc - 1], ptN_setcc(p, cc - 1);
    }
    return (int)d;
}

static void ptD_stitch(pt_Cursor *L, pt_Node *rt) {
    size_t   d = 0;
    int      i, l = ptK_levels(L);
    pt_Node *p = ptK_parent(L, l);
    assert(ptD_checkstitch(L) == PT_OK);
    if (ptN_cc(p) && ptN_cc(&rt[0])) d = (size_t)ptD_mergeleaf(L, rt);
    ptD_stitchnode(L, rt);
    ptD_rebalance(L, 0);
    l = ptK_levels(L), p = ptK_parent(L, l), i = ptK_idx(L, p, l);
    if (ptN_cc(p) && i == ptN_cc(p)) {
        L->paths[ptK_levels(L)] -= 1;
        L->poff += p->bytes[ptN_cc(p) - 1], L->off -= p->bytes[ptN_cc(p) - 1];
    }
    if (d > L->poff) {
        ptD_backwardnode(L, 1, l);
        p = ptK_parent(L, l), i = ptK_idx(L, p, l);
        d -= L->poff, L->poff = p->bytes[i], L->off -= p->bytes[i];
    }
    L->poff -= d, ptM_upmask(L);
}

static void ptD_rmrange(pt_Cursor *L, pt_Cursor *R, int fl) {
    pt_Node *rt = L->tree->S->rt;
    int      k, l = ptK_levels(L);
    for (k = 0; k < PT_MAX_LEVEL; ++k)
        ptN_setcc(&rt[k], 0), rt[k].version = L->tree->root.version;
    ptD_trimright(L, l), ptD_trimleft(R, l);
    ptD_cutrange(L, R, rt, fl), ptD_stitch(L, rt);
}

static void ptD_cutpiece(pt_Cursor *C, size_t lo, size_t hi) {
    pt_Node *p;
    int      l = ptK_levels(C), i = ptK_idx(C, p = ptK_parent(C, l), l);
    if (ptM_ishole(p, i)) {
        ptH_remove(p, i, lo, hi - lo);
        if (p->bytes[i] == 0) ptN_remove(C->tree->S, p, 0, i, i + 1);
        return;
    }
    if (lo == 0 && hi == p->bytes[i])
        ptN_remove(C->tree->S, p, 0, i, i + 1);
    else if (lo == 0) {
        p->children[i] = (pt_Node *)(ptN_lit(p, i) + hi);
        p->bytes[i] = p->bytes[i] - hi;
    } else if (hi == p->bytes[i])
        p->bytes[i] = lo;
    else {
        ptN_makespace(p, i + 1, 1);
        p->children[i + 1] = (pt_Node *)(ptN_lit(p, i) + hi);
        p->bytes[i + 1] = p->bytes[i] - hi, p->bytes[i] = lo;
        ptM_sethole(p, i + 1, 0);
    }
}

static void ptD_rmleaf(pt_Cursor *L, pt_Cursor *R) {
    pt_Node *p = ptK_parent(L, ptK_levels(L));
    int      l = ptK_levels(L), i = ptK_idx(L, p, l), oc = ptN_cc(p);
    pt_Delta d = pt_offset(R) - pt_offset(L);
    assert(i == ptK_idx(R, p, l));
    if (!ptM_ishole(p, i) && L->poff > 0 && R->poff < p->bytes[i]) {
        for (l = ptK_levels(L); l >= 0; --l)
            if (ptN_cc(ptK_parent(L, l)) + 1 <= PT_FANOUT) break;
        if (l < 0) ptI_splitroot(L), l = 1;
        for (; l < ptK_levels(L); ++l) ptI_splitchild(L, l);
        i = ptK_idx(L, p = ptK_parent(L, l), l);
    }
    ptD_cutpiece(L, L->poff, R->poff), ptM_upbytes(L, l - 1, -d);
    if (ptN_cc(p) == 0) L->paths[l] = &p->children[0], L->off = 0, L->poff = 0;
    if (ptN_cc(p) < oc && l > 0) ptD_rebalance(L, l - 1), ptM_upmask(L);
}

static int ptD_cowpaths(pt_Cursor *L, pt_Cursor *R) {
    int fl, l, i;
    for (l = 0; l < ptK_levels(L) && L->paths[l] == R->paths[l]; ++l) {
        i = ptK_idx(R, ptK_parent(R, l + 1), l + 1);
        R->paths[l + 1] = &ptK_cow(L, l, 0)->children[i];
    }
    for (fl = l; l < ptK_levels(L); ++l) ptK_cow(L, l, 0);
    for (l = fl; l < ptK_levels(R); ++l) ptK_cow(R, l, 0);
    return fl + (fl == ptK_levels(L) && L->paths[fl] == R->paths[fl]);
}

PT_API int pt_remove(pt_Cursor *C, size_t len) {
    int       r, i, l;
    pt_Cursor R;
    if (!C || !C->tree) return PT_ERRPARAM;
    if (len == 0 || pt_offset(C) >= ptK_bytes(C)) return PT_OK;
    if (pt_offset(C) + len > ptK_bytes(C)) len = ptK_bytes(C) - pt_offset(C);
    R = *C, pt_advance(&R, (pt_Delta)len), i = ptK_idx(&R, &R.tree->root, 0);
    r = ptP_reserve(C->tree->S, &C->tree->S->nodes, 4 * ptK_levels(C) + 5);
    if (r != PT_OK || (r = ptK_markdirty(C)) != PT_OK) return r;
    R.tree = C->tree, R.paths[0] = C->tree->root.children + i;
    if ((l = ptD_cowpaths(C, &R)) > ptK_levels(C))
        return ptD_rmleaf(C, &R), PT_OK;
    return ptD_rmrange(C, &R, l), PT_OK;
}

PT_API int pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len) {
    int r;
    if (!C || !C->tree) return PT_ERRPARAM;
    if (del == 0 && (s == NULL || len == 0)) return PT_OK;
    r = ptP_reserve(C->tree->S, &C->tree->S->nodes, 6 * ptK_levels(C) + 8);
    if (r != PT_OK || (r = pt_remove(C, del)) != PT_OK) return r;
    if (s != NULL && len > 0) return pt_append(C, s, len);
    return PT_OK;
}

PT_NS_END

#endif /* PT_IMPLEMENTATION */
