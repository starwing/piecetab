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
typedef const struct pt_Tree *pt_Buffer;

typedef ptrdiff_t pt_Delta;

typedef void *pt_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/* lifetime*/

PT_API pt_State *pt_open(pt_Alloc *allocf, void *ud);
PT_API void      pt_reset(pt_State *S);
PT_API void      pt_close(pt_State *S);

PT_API pt_Alloc *pt_getallocf(pt_State *S, void **pud);

PT_API unsigned pt_retain(pt_Buffer b);
PT_API unsigned pt_release(pt_Buffer b);

/* buffer */

/* construction */
PT_API pt_Buffer pt_empty(pt_State *S);
PT_API pt_Buffer pt_from(pt_State *S, const char *s, size_t len);
PT_API pt_Buffer pt_compact(pt_State *S, pt_Buffer b);

/* query */
PT_API unsigned pt_version(pt_Buffer b);
PT_API size_t   pt_bytes(pt_Buffer b);

/* cursor */

/* construction */
PT_API int pt_seek(pt_Cursor *C, pt_Buffer b, size_t off);

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
#define pt_valid(C)  ((C)->tree != NULL)
#define pt_buffer(C) ((C)->tree)

/* editing */

/* hole edit */
PT_API int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len);

/* literal edit */
PT_API int pt_insert(pt_Cursor *C, const char *s, size_t len);
PT_API int pt_append(pt_Cursor *C, const char *s, size_t len);
PT_API int pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len);
PT_API int pt_remove(pt_Cursor *C, size_t len);

/* transaction */
PT_API pt_Buffer pt_rollback(pt_Cursor *C);
PT_API pt_Buffer pt_commit(pt_Cursor *C);

/* literal scratch buffer (arena) */
PT_API char *pt_reserve(pt_Cursor *C, size_t len);
PT_API char *pt_scratch(pt_Cursor *C, size_t *plen);

PT_API const char *pt_literal(pt_Cursor *C, size_t len);

/* cursor definition */

#define PT_MAX_LEVEL 16

struct pt_Cursor {
    struct pt_Node **paths[PT_MAX_LEVEL]; /* root-to-leaf child slot ptrs */
    struct pt_Tree  *tree;                /* buffer under navigation or edit */
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

#ifdef _MSC_VER
# include <intrin.h>
#endif

#define PT_STATIC_ASSERT(cond)      PT_SA_0(cond, pt_SA_, __LINE__)
#define PT_SA_0(cond, prefix, line) PT_SA_1(cond, prefix, line)
#define PT_SA_1(cond, prefix, line) typedef char prefix##line[(cond) ? 1 : -1]

#ifndef PT_FANOUT
# define PT_FANOUT 62
#endif

/* makeroom needs at most 2 free slots; a split of a full node leaves
 * FANOUT/2 free in the cursor's half, so require FANOUT >= 4. */
PT_STATIC_ASSERT(PT_FANOUT >= 4);

#ifndef PT_PAGE_SIZE
# define PT_PAGE_SIZE 65536
#endif

#ifndef PT_MAX_HOLESIZE
# define PT_MAX_HOLESIZE 64
#endif

#ifndef PT_ARENA_SIZE
# define PT_ARENA_SIZE 1024
#endif

#ifndef PT_COMPACT_RANGES
# define PT_COMPACT_RANGES 64 /* initial capacity of compact range array */
#endif

/* ranges array will increase by 1.5x, when PT_COMPACT_RANGES == 1 it will not
 * grow, so it must be greater than 1. */
PT_STATIC_ASSERT(PT_COMPACT_RANGES > 1);

#define pt_min(a, b) ((a) < (b) ? (a) : (b))
#define pt_max(a, b) ((a) > (b) ? (a) : (b))

PT_NS_BEGIN

typedef size_t   pt_Mask;
typedef unsigned pt_Ver;

#define PT_MASK_BITS (sizeof(pt_Mask) * CHAR_BIT)

PT_STATIC_ASSERT(PT_FANOUT <= PT_MASK_BITS);

/* clang-format off */
typedef struct pt_Range { size_t start, end; } pt_Range;
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
    size_t obj_size;  /* size of each object in this pool */
    void  *freed;     /* freelist head */
    void  *pages;     /* linked list of allocated pages */
    size_t freed_obj; /* number of objects in freelist */
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

static void ptP_init(pt_Pool *p, size_t obj_size) {
    memset(p, 0, sizeof(pt_Pool)), p->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < PT_PAGE_SIZE / 2);
}

static void ptP_destroy(pt_State *S, pt_Pool *p) {
    void *next, *page = p->pages;
    for (; page; page = next) {
        next = *(void **)((char *)page + PT_PAGE_SIZE - sizeof(void *));
        S->allocf(S->alloc_ud, page, PT_PAGE_SIZE, 0);
    }
    ptP_init(p, p->obj_size);
}

static void *ptP_ralloc(pt_Pool *p) {
    void *obj = p->freed;
    assert(obj), ptP_stat(p->live_obj += 1), p->freed_obj -= 1;
    return (p->freed = *(void **)obj), (void *)obj;
}

static void ptP_free(pt_Pool *p, void *obj) {
    ptP_stat(p->live_obj -= 1), p->freed_obj += 1;
    *(void **)obj = p->freed, p->freed = obj;
}

static void *ptP_alloc(pt_State *S, pt_Pool *p) {
    size_t sz = p->obj_size;
    char  *page, *end;
    if (p->freed_obj) return ptP_ralloc(p);
    page = (char *)S->allocf(S->alloc_ud, NULL, 0, PT_PAGE_SIZE);
    if (page == NULL) return NULL;
    end = &page[PT_PAGE_SIZE - sizeof(void *)], *(void **)end = p->pages;
    p->pages = (void *)page, page += sz, end -= sz;
    while ((page += sz) <= end) *(void **)(page - sz) = page;
    *(void **)(page - sz) = p->freed, ptP_stat(p->live_obj += 1);
    p->freed_obj = (end - (char *)p->pages) / sz;
    return (p->freed = (void *)((char *)p->pages + sz)), p->pages;
}

static int ptP_reserve(pt_State *S, pt_Pool *p, size_t n) {
    size_t avail = p->freed_obj;
    void  *obj;
    if (avail >= n) return PT_OK;
    while (p->freed_obj = 0, (obj = ptP_alloc(S, p)))
        if (ptP_free(p, obj), (avail += p->freed_obj) >= n) break;
    return (p->freed_obj = avail) >= n ? PT_OK : PT_ERRMEM;
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

static int ptM_ctz(pt_Mask m) {
    unsigned long i = 0;
    assert(m != 0);
#if defined(_MSC_VER) && defined(_WIN64)
    return _BitScanForward64(&i, m), (int)i;
#elif defined(_MSC_VER)
    return _BitScanForward(&i, (unsigned long)m), (int)i;
#elif defined(__GNUC__)
    return (void)i, __builtin_ctzll(m);
#else
    while (!(m & 1)) m >>= 1, ++i;
    return (int)i;
#endif
}

/* clang-format off */
static size_t ptN_sumbytes(const pt_Node *n, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += n->bytes[i]; return s; }

static int ptM_ishole(const pt_Node *n, int i)
{ return assert(i >= 0 && i < PT_FANOUT), (n->mask & ((pt_Mask)1 << i)) != 0; }

static int ptM_iterhole(pt_Mask *m, int *pi, int cc)
{ return *m ? (*pi = ptM_ctz(*m), *m &= *m - 1, *pi < cc) : 0; }
/* clang-format on */

PT_API unsigned pt_version(pt_Buffer b) { return b ? b->root.version : 0; }
PT_API size_t   pt_bytes(pt_Buffer b) { return b ? b->bytes : 0; }
PT_API unsigned pt_retain(pt_Buffer b) {
    return b ? ++((pt_Tree *)b)->refc : 0;
}
PT_API pt_Buffer pt_empty(pt_State *S) { return S ? &S->empty : NULL; }

static void ptM_sethole(pt_Node *n, int i, int h) {
    assert(i >= 0 && i < PT_FANOUT);
    n->mask ^= (-!!h ^ n->mask) & ((pt_Mask)1 << i);
}

static int ptM_remask(pt_Node *p, int i, int k) {
    int h;
    if (k == 0) return 1;
    h = (p->children[i]->mask & ptM_mask(ptN_cc(p->children[i]))) != 0;
    return ptM_ishole(p, i) == h ? 0 : (ptM_sethole(p, i, h), 1);
}

static void ptM_up(pt_Cursor *C, int l, pt_Delta db) {
    int      i;
    pt_Node *p;
    for (; l >= 0; --l) {
        i = ptK_idx(C, p = ptK_parent(C, l), l), p->bytes[i] += db;
        if (!ptM_remask(p, i, ptK_levels(C) - l) && !db) return;
    }
    if (db) C->tree->bytes += db;
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
            pt_Node *c = p->children[i];
            if (c->version != v) continue;
            ptN_purge(S, c, k - 1, 0, ptN_cc(c), v), ptP_free(&S->nodes, c);
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
    ptN_setcc(p, ptN_cc(p) - (e - s));
}

static void ptN_makespace(pt_Node *p, int i, int n) {
    assert(ptN_cc(p) + n <= PT_FANOUT && i <= ptN_cc(p));
    ptN_move(p, i + n, i, ptN_cc(p) - i);
    ptN_setcc(p, ptN_cc(p) + n);
}

/* lifetime */

/* clang-format off */
PT_API void pt_close(pt_State *S)
{ if (S) pt_reset(S), S->allocf(S->alloc_ud, S, sizeof(pt_State), 0); }
/* clang-format on */

static void *ptS_defallocf(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    (void)ud, (void)osize;
    if (nsize == 0) return (void)free(p), NULL;
    return (np = realloc(p, nsize)) ? np : ((void)abort(), NULL);
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

PT_API void pt_reset(pt_State *S) {
    if (S == NULL) return;
    ptP_destroy(S, &S->nodes);
    ptP_destroy(S, &S->holes);
    ptP_destroy(S, &S->trees);
    S->max_version = 0;
}

PT_API pt_Alloc *pt_getallocf(pt_State *S, void **pud) {
    if (S == NULL) return NULL;
    if (pud) *pud = S->alloc_ud;
    return S->allocf;
}

PT_API pt_Buffer pt_from(pt_State *S, const char *s, size_t len) {
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

PT_API unsigned pt_release(pt_Buffer b) {
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

PT_API int pt_seek(pt_Cursor *C, pt_Buffer b, size_t off) {
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
    if (C->poff >= p->bytes[i]) return (void)(plen && (*plen = 0)), NULL;
    if (plen) *plen = p->bytes[i] - C->poff;
    return ptN_lit(p, i) + C->poff;
}

PT_API const char *pt_next(pt_Cursor *C, size_t *plen) {
    int      i, l;
    size_t   bc;
    pt_Node *p;
    if (C == NULL || C->tree == NULL) return NULL;
    l = ptK_levels(C), i = ptK_idx(C, p = ptK_parent(C, l), l);
    if (C->poff == p->bytes[i]) return (void)(plen && (*plen = 0)), NULL;
    bc = p->bytes[i] - C->poff;
    while (i + 1 >= ptN_cc(p) && --l >= 0)
        i = ptK_idx(C, p = ptK_parent(C, l), l);
    if (l < 0) return C->poff += bc, plen && (*plen = 0), (const char *)NULL;
    C->paths[l] += 1, C->off += bc + C->poff, C->poff = 0;
    while (++l <= ptK_levels(C)) C->paths[l] = &ptK_parent(C, l)->children[0];
    return pt_piece(C, plen);
}

PT_API const char *pt_prev(pt_Cursor *C, size_t *plen) {
    pt_Node *p;
    int      i, l;
    if (C == NULL || C->tree == NULL) return NULL;
    l = ptK_levels(C), i = ptK_idx(C, p = ptK_parent(C, l), l);
    if (C->poff > 0)
        return (void)(plen && (*plen = C->poff)), C->poff = 0, ptN_lit(p, i);
    if (C->off == 0) return (void)(plen && (*plen = 0)), NULL;
    while (i <= 0 && --l >= 0) i = ptK_idx(C, p = ptK_parent(C, l), l);
    assert(l >= 0 && i > 0), C->paths[l] -= 1, i -= 1;
    while (++l <= ptK_levels(C))
        p = ptK_parent(C, l), C->paths[l] = &p->children[i = ptN_cc(p) - 1];
    C->off -= p->bytes[i], C->poff = 0;
    return (void)(plen && (*plen = p->bytes[i])), ptN_lit(p, i);
}

PT_API size_t pt_read(pt_Cursor *C, char *buf, size_t len) {
    size_t      m, n, total = 0;
    const char *p;
    if (C == NULL || C->tree == NULL || buf == NULL) return 0;
    for (p = pt_piece(C, &n); len > 0 && n > 0; p = pt_next(C, &n)) {
        m = pt_min(n, len);
        memcpy(buf, p, m), buf += m, len -= m, total += m;
        if (m < n) {
            C->poff += m;
            break;
        }
    }
    return total;
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
    pt_Arena *a;
    pt_Block *b, **pp;
    if (C == NULL || C->tree == NULL) return NULL;
    if (ptK_markdirty(C) != PT_OK) return NULL;
    if (len == 0) len = PT_ARENA_SIZE;
    S = C->tree->S, a = &C->tree->arena, b = C->tree->arena.current;
    if (b && b->size - b->used >= len) return (char *)(b + 1) + b->used;
    for (pp = &a->current; *pp; pp = &(*pp)->next) {
        if (b = *pp, b->size - b->used >= len) {
            *pp = b->next, b->next = a->current, a->current = b;
            return (char *)(b + 1) + b->used;
        }
    }
    if ((b = ptA_alloc(S, pt_max(len, PT_ARENA_SIZE))) == NULL) return NULL;
    return b->next = a->current, a->current = b, (char *)(b + 1);
}

PT_API char *pt_scratch(pt_Cursor *C, size_t *plen) {
    pt_Block *b;
    if (C == NULL || C->tree == NULL || plen == NULL) return NULL;
    if ((b = C->tree->arena.current) == NULL) return *plen = 0, (char *)NULL;
    *plen = (assert(b->used < b->size), b->size - b->used);
    return (char *)(b + 1) + b->used;
}

PT_API const char *pt_literal(pt_Cursor *C, size_t len) {
    pt_Arena *a;
    pt_Block *b;
    size_t    n, off;
    if (C == NULL || C->tree == NULL || len == 0) return NULL;
    if (ptK_markdirty(C) != PT_OK) return NULL;
    if ((b = (a = &C->tree->arena)->current) == NULL) return NULL;
    if ((n = (off = b->used) + len) > b->size) return NULL;
    if (n == b->size) a->current = b->next, b->next = a->full, a->full = b;
    return b->used += len, (char *)(b + 1) + off;
}

/* insert */

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
    assert(d <= end && end + len <= PT_MAX_HOLESIZE);
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
    ptM_sethole(r, 0, pp->mask != 0), ptM_sethole(r, 1, nw->mask != 0);
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
    ptM_sethole(p, i, nd->mask != 0), ptM_sethole(p, i + 1, nw->mask != 0);
    if ((cs = ptK_idx(C, nd, l + 1)) >= mid) {
        C->paths[l] = &p->children[i + 1];
        C->paths[l + 1] = &nw->children[cs - mid];
    }
}

static void ptI_insertrt(pt_Cursor *C, pt_Node *rt, int s, int e) {
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
    int      l, cc, need, m, i;
    size_t   n, po = C->poff;
    l = ptK_levels(C), i = ptK_idx(C, p = ptK_parent(C, l), l);
    n = p->bytes[i], cc = ptN_cc(p);
    assert(po <= n), need = 1 + (po > 0 && po < n);
    rt->mask = 0, rt->version = C->tree->root.version;
    ptI_fillrt(C, s, len, h), i = ptK_idx(C, p, l);
    if ((m = pt_min(need, PT_FANOUT - cc)) > 0)
        ptN_makespace(p, i, m), ptN_copy(p, i, rt, 0, m);
    if (m == need)
        C->paths[l] += need - (po > 0), ptM_up(C, l - 1, len);
    else {
        pt_Delta shrink = po ? (pt_Delta)(n - po) : 0;
        pt_Delta db = (pt_Delta)ptN_sumbytes(rt, 0, m);
        C->paths[l] += m, ptM_up(C, l - 1, db - shrink);
        ptI_insertrt(C, rt, m, need), l = ptK_levels(C);
        C->paths[l] += need - m - (po > 0);
        ptM_up(C, l - 1, (pt_Delta)ptN_sumbytes(rt, 0, ptN_cc(rt)) - db);
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
    l = ptK_levels(C), i = ptK_idx(C, p = ptK_parent(C, l), l);
    if (ptM_ishole(p, i) && ptH_fit(p, i, len)) {
        ptH_append(p, i, C->poff, s, len), C->poff += len;
        return ptM_up(C, l - 1, (pt_Delta)len), PT_OK;
    }
    if (C->poff == 0 && i && ptM_ishole(p, i - 1) && ptH_fit(p, i - 1, len)) {
        ptH_append(p, i - 1, p->bytes[i - 1], s, len), C->off += len;
        return ptM_up(C, l - 1, (pt_Delta)len), PT_OK;
    }
    return ptI_splitins(C, s, len, 1), PT_OK;
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
        return ptM_up(C, ptK_levels(C) - 1, (pt_Delta)len), PT_OK;
    }
    if (C->poff == p->bytes[i] && !ptM_ishole(p, i)
        && ptN_lit(p, i) + p->bytes[i] == s) {
        p->bytes[i] += len, C->poff += len;
        return ptM_up(C, ptK_levels(C) - 1, (pt_Delta)len), PT_OK;
    }
    if (C->poff == 0 && !ptM_ishole(p, i) && s + len == ptN_lit(p, i)) {
        p->children[i] = (pt_Node *)s, p->bytes[i] += len, C->poff = len;
        return ptM_up(C, ptK_levels(C) - 1, (pt_Delta)len), PT_OK;
    }
    return ptI_splitins(C, s, len, 0), PT_OK;
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
    assert(d <= end && d + len <= end);
    memmove(h->data + d, h->data + d + len, end - (d + len));
    n->bytes[i] = end - len;
}

static void ptD_trimright(pt_Cursor *L) {
    pt_Node *p;
    int      l = ptK_levels(L), i = ptK_idx(L, p = ptK_parent(L, l), l);
    ptM_up(L, l, -(pt_Delta)(p->bytes[i] - L->poff));
}

static void ptD_trimleft(pt_Cursor *R) {
    pt_Node *p;
    int      l = ptK_levels(R), i = ptK_idx(R, p = ptK_parent(R, l), l);
    if (ptM_ishole(p, i))
        ptH_remove(p, i, 0, R->poff);
    else {
        p->children[i] = (pt_Node *)(ptN_lit(p, i) + R->poff);
        p->bytes[i] -= R->poff;
    }
    ptM_up(R, l - 1, -(pt_Delta)R->poff), R->poff = 0;
}

static void ptD_cutrange(pt_Cursor *L, pt_Cursor *R, pt_Node *rt, int fl) {
    pt_State *S = L->tree->S;
    int       kl, k, i, cc, l = ptK_levels(L);
    pt_Delta  db = 0;
    pt_Node  *p;
    for (kl = l; kl > fl; --kl) {
        p = ptK_parent(L, kl), i = ptK_idx(L, p, kl), cc = ptN_cc(p);
        p->bytes[i] -= db, db += ptN_sumbytes(p, i + 1, cc);
        k = l - kl, ptN_remove(S, p, k, i + 1, cc);
        i = ptK_idx(R, p = ptK_parent(R, kl), kl), cc = ptN_cc(p);
        i += (k || p->bytes[i] == 0);
        ptN_copy(&rt[k], 0, p, i, cc - i), ptN_setcc(&rt[k], cc - i);
        ptN_purge(S, p, k, 0, i, rt->version), ptN_setcc(p, 0);
    }
    p = ptK_parent(R, fl), i = ptK_idx(R, p, fl), cc = ptN_cc(p);
    k = l - fl, i += (k || p->bytes[i] == 0);
    ptN_copy(&rt[k], 0, p, i, ptN_setcc(&rt[k], cc - i));
    ptN_setcc(p, i), i = ptK_idx(L, p, fl);
    p->bytes[i] -= db, db += ptN_sumbytes(p, i + 1, cc);
    ptM_up(L, fl - 1, -db), ptN_remove(S, p, k, i + 1, ptN_cc(p));
}

static int ptD_makechain(pt_Cursor *C, int from, int to, int nofail) {
    pt_Node *p, *nn, ***cp = C->paths + to;
    int      l, r = 0;
    if (!nofail && ptP_reserve(C->tree->S, &C->tree->S->nodes, to - from + 1))
        return PT_ERRMEM;
    if (assert(from < to), from < 0) {
        int h = (assert(to == ptK_levels(C)), p = &C->tree->root,
                 p->mask & ptM_mask(ptN_cc(p)))
             != 0;
        nn = (pt_Node *)ptP_ralloc(&C->tree->S->nodes), *nn = *p;
        p->bytes[0] = ptK_bytes(C), p->children[0] = nn;
        ptN_setcc(p, 1), p->mask = 0, ptM_sethole(p, 0, h);
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

static void ptD_findroom(pt_Cursor *C, int l) {
    int      i, fl;
    pt_Node *p;
    for (fl = l - 1; fl >= 0; --fl) {
        p = ptK_parent(C, fl), i = ptK_idx(C, p, fl);
        if (i < PT_FANOUT - 1) break;
    }
    assert(fl >= 0 && ptN_cc(p) - i - 1 == 0);
    ptD_makechain(C, fl, l, 1);
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
    int       cL, cR, dn, i = ptK_idx(C, p, l);
    pt_Node **ns = &p->children[i], *o = *ns;
    pt_Delta  ds;
    if (assert(ptN_cc(p) > 1), ptN_cc(ns[0]) > PT_FANOUT / 2) return 0;
    if ((i && lfirst) || i == ptN_cc(p) - 1) ns -= 1, i -= 1;
    ns[0] = ptK_cow(C, l, -(*ns != o)), ns[1] = ptK_cow(C, l, *ns == o);
    if ((cL = ptN_cc(ns[0])) + (cR = ptN_cc(ns[1])) <= PT_FANOUT) {
        ptN_copy(ns[0], cL, ns[1], 0, cR);
        ptN_setcc(*ns, ptN_cc(*ns) + cR), ptN_setcc(ns[1], ptN_cc(ns[1]) - cR);
        p->bytes[i] += p->bytes[i + 1], ns[0]->mask |= ns[1]->mask << cL;
        ptM_sethole(p, i, !!(ns[0]->mask & ptM_mask(cL + cR)));
        if (*ns != o)
            cp[1] = &ns[0]->children[cp[1] - ns[1]->children + cL], cp[0] -= 1;
        return ptN_remove(C->tree->S, p, ptK_levels(C) - l, i + 1, i + 2), 1;
    }
    dn = ptD_balancenode(ns, (*ns == o), &ds);
    assert(dn != 0 && (dn < 0) != (*ns != o));
    p->bytes[i] -= ds, p->bytes[i + 1] += ds;
    ptM_sethole(p, i, ns[0]->mask != 0);
    ptM_sethole(p, i + 1, ns[1]->mask != 0);
    if (*ns != o) cp[1] += dn;
    return 0;
}

static void ptD_rebalance(pt_Cursor *C, int l) {
    assert(l == 0 || l < ptK_levels(C));
    for (; l >= 0 && l < ptK_levels(C); --l) {
        pt_Node *p = ptK_parent(C, l);
        if (ptN_cc(p->children[ptK_idx(C, p, l)]) >= PT_FANOUT / 2) break;
        if (ptN_cc(p) < 2) break; /* lone-child root: collapse below */
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

static void ptD_stitchnode(pt_Cursor *L, pt_Node *rt) {
    int      k, i, d = 0, l = ptK_levels(L);
    pt_Delta db = 0;
    pt_Node *p, *r;
    for (k = 0; k <= ptK_levels(L); ++k) {
        int m, fl, kl = ptK_levels(L) - k, rtcc = ptN_cc(r = &rt[k]);
        ptN_setcc(r, 0), i = ptK_idx(L, p = ptK_parent(L, kl), kl);
        if (i < ptN_cc(p)) p->bytes[i] += db, ptM_remask(p, i, k);
        if ((m = pt_min(rtcc, PT_FANOUT - ptN_cc(p))) > 0) {
            ptN_copy(p, ptN_cc(p), r, 0, m), ptN_setcc(p, ptN_cc(p) + m);
            db += (pt_Delta)ptN_sumbytes(r, 0, m);
        }
        if (!(m < rtcc || kl == 0)) continue;
        ptM_up(L, kl - 1, db), db = 0;
        if (kl == 0 && ptN_cc(&L->tree->root) == 1)
            ptD_rebalance(L, 0), l -= (k - ptK_levels(L));
        for (fl = kl; fl < l; ++fl) ptD_foldnode(L, (fl == kl), fl);
        if (k) ptD_backwardnode(L, d, l);
        if (!(m < rtcc)) continue;
        p = ptK_parent(L, l = kl), d = k ? ptN_cc(p) - ptK_idx(L, p, l) : m;
        ptD_findroom(L, l), p = ptK_parent(L, l);
        ptN_copy(p, 0, r, m, ptN_setcc(p, rtcc - m));
        db += (pt_Delta)ptN_sumbytes(r, m, rtcc);
    }
}

static int ptD_mergeleaf(pt_Cursor *C, pt_Node *rt) {
    int      cc, hL, hR, merged = 0, l = ptK_levels(C);
    pt_Node *p = ptK_parent(C, l);
    size_t   d = 0, bc = p->bytes[assert(ptN_cc(p)), (cc = ptN_cc(p)) - 1];
    hL = ptM_ishole(p, cc - 1), hR = ptM_ishole(rt, 0);
    if (!hL && !hR && ptN_lit(p, cc - 1) + bc == ptN_lit(rt, 0))
        merged = 1;
    else if (hL && hR) {
        d = pt_min(rt->bytes[0], PT_MAX_HOLESIZE - bc);
        ptH_append(p, cc - 1, bc, ptN_hole(rt, 0)->data, d);
        if (d < rt->bytes[0])
            ptH_remove(rt, 0, 0, d), ptM_up(C, l - 1, (pt_Delta)d);
        else
            ptP_free(&C->tree->S->holes, ptN_hole(rt, 0)), merged = 1, d = 0;
    }
    if (!merged)
        C->off += C->poff + d, C->poff = 0, C->paths[l] = &p->children[cc];
    else {
        rt->bytes[0] += bc, rt->children[0] = p->children[cc - 1];
        ptM_up(C, l - 1, -(pt_Delta)bc);
        if (ptK_idx(C, p, l) == cc) C->off -= bc, C->poff = bc;
        C->paths[l] = &p->children[cc - 1], ptN_setcc(p, cc - 1);
    }
    return (int)d;
}

static void ptD_stitch(pt_Cursor *L, pt_Node *rt) {
    size_t   d = 0;
    int      i, cc, l = ptK_levels(L);
    pt_Node *p = ptK_parent(L, l);
    assert(L->tree->S->nodes.freed_obj >= (size_t)(ptK_levels(L) + 2));
    if ((cc = ptN_cc(p)) && p->bytes[cc - 1] == 0)
        ptN_remove(L->tree->S, p, 0, cc - 1, cc), cc -= 1;
    if (cc && ptN_cc(&rt[0])) d = (size_t)ptD_mergeleaf(L, rt);
    ptD_stitchnode(L, rt), ptD_rebalance(L, 0);
    l = ptK_levels(L), i = ptK_idx(L, p = ptK_parent(L, l), l), cc = ptN_cc(p);
    if (cc && i == cc) {
        L->paths[ptK_levels(L)] -= 1;
        L->poff += p->bytes[cc - 1], L->off -= p->bytes[cc - 1];
    }
    if (d > L->poff) {
        ptD_backwardnode(L, 1, l);
        p = ptK_parent(L, l), i = ptK_idx(L, p, l);
        d -= L->poff, L->poff = p->bytes[i], L->off -= p->bytes[i];
    }
    L->poff -= d, ptM_up(L, ptK_levels(L), 0);
}

static void ptD_rmrange(pt_Cursor *L, pt_Cursor *R, int fl) {
    pt_Node *rt = L->tree->S->rt;
    int      k;
    for (k = 0; k < PT_MAX_LEVEL; ++k)
        ptN_setcc(&rt[k], 0), rt[k].version = L->tree->root.version;
    ptD_trimright(L), ptD_trimleft(R);
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

static void ptD_rmleaf(pt_Cursor *C, size_t del) {
    pt_Node *p = ptK_parent(C, ptK_levels(C));
    int      l = ptK_levels(C), i = ptK_idx(C, p, l), oc = ptN_cc(p);
    size_t   endpoff = C->poff + del;
    assert(endpoff <= p->bytes[i]);
    if (!ptM_ishole(p, i) && C->poff > 0 && endpoff < p->bytes[i]) {
        for (l = ptK_levels(C); l >= 0; --l)
            if (ptN_cc(ptK_parent(C, l)) + 1 <= PT_FANOUT) break;
        if (l < 0) ptI_splitroot(C), l = 1;
        for (; l < ptK_levels(C); ++l) ptI_splitchild(C, l);
        i = ptK_idx(C, p = ptK_parent(C, l), l);
    }
    ptD_cutpiece(C, C->poff, endpoff), ptM_up(C, l - 1, -(pt_Delta)del);
    if (ptN_cc(p) == 0)
        C->paths[l] = &p->children[0], C->off = 0, C->poff = 0;
    else if (ptK_idx(C, p, l) == ptN_cc(p)) {
        C->paths[l] -= 1;
        C->poff = p->bytes[ptN_cc(p) - 1];
        C->off -= p->bytes[ptN_cc(p) - 1];
    }
    if (ptN_cc(p) < oc && l > 0)
        ptD_rebalance(C, l - 1), ptM_up(C, ptK_levels(C), 0);
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
        return ptD_rmleaf(C, len), PT_OK;
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

/* commit */

static size_t ptC_holebytes(pt_Node *n, int k) {
    pt_Mask m = n->mask;
    size_t  total = 0;
    int     i;
    while (ptM_iterhole(&m, &i, ptN_cc(n)))
        total += k ? ptC_holebytes(n->children[i], k - 1) : n->bytes[i];
    return total;
}

static int ptC_nexthole(pt_Cursor *C, int l) {
    for (; l >= 0; --l) {
        pt_Node *p = ptK_parent(C, l);
        pt_Mask  m;
        int      i;
        while (m = p->mask, ptM_iterhole(&m, &i, ptN_cc(p))) {
            C->paths[l] = &p->children[i];
            if (l++ == ptK_levels(C)) return 1;
            p = p->children[i];
        }
    }
    return 0;
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

static int ptC_freeze(pt_Cursor *C, char **ppos) {
    pt_State *S = C->tree->S;
    int       r, w, l = ptK_levels(C);
    pt_Node  *p = ptK_parent(C, l);
    for (;;) {
        if (ptP_reserve(S, &S->nodes, 2 * l + 2)) return PT_ERRMEM;
        if (p->mask) ptC_freezeleaf(S, p, ppos);
        for (r = 1, w = 0; r < ptN_cc(p); ++r) {
            if (ptN_lit(p, w) + p->bytes[w] == ptN_lit(p, r))
                p->bytes[w] += p->bytes[r];
            else
                ++w, p->children[w] = p->children[r], p->bytes[w] = p->bytes[r];
        }
        ptN_setcc(p, w + 1), C->paths[l] = &p->children[0], ptM_up(C, l - 1, 0);
        if (l == 0 || ptN_cc(p) >= PT_FANOUT / 2) return PT_OK;
        ptD_rebalance(C, l - 1), l = ptK_levels(C), p = ptK_parent(C, l);
    }
}

PT_API pt_Buffer pt_commit(pt_Cursor *C) {
    size_t    total;
    char     *buf;
    pt_Buffer b;
    if (C == NULL || C->tree == NULL) return NULL;
    if (!C->dirty) return b = C->tree, pt_retain(b), C->tree = NULL, b;
    if ((total = ptC_holebytes(&C->tree->root, C->tree->levels)) > 0) {
        pt_Cursor sC;
        int       r;
        if (!(buf = pt_reserve(C, total)) || pt_literal(C, total) != buf)
            return NULL;
        sC.tree = C->tree, sC.off = sC.poff = 0, sC.dirty = 0;
        sC.paths[0] = C->tree->root.children;
        if (!ptC_nexthole(&sC, 0)) return PT_OK;
        while ((r = ptC_freeze(&sC, &buf)) == PT_OK)
            if (!ptC_nexthole(&sC, ptK_levels(&sC) - 1)) break;
        if (r != PT_OK) return pt_locate(C, pt_offset(C)), (pt_Buffer)NULL;
    }
    return C->dirty = 0, b = C->tree, C->tree = NULL, b;
}

PT_API pt_Buffer pt_rollback(pt_Cursor *C) {
    pt_Buffer b;
    if (C == NULL || C->tree == NULL) return NULL;
    if (!C->dirty) return b = C->tree, pt_retain(b), C->tree = NULL, b;
    /* the fork's from-retain is consumed by pt_release's chain walk; the
     * caller's reference must be added first or a sole-owned from dies */
    b = C->tree->from, assert(b != NULL), pt_retain(b);
    return pt_release(C->tree), C->tree = NULL, C->dirty = 0, b;
}

/* compact */

typedef struct pt_Compact {
    pt_Cursor *oC;  /* source cursor over old tree */
    pt_Range  *rs;  /* arena block ranges, sorted by start */
    size_t     nr;  /* range count */
    size_t     cap; /* range array capacity */
} pt_Compact;

static int ptZ_rangecmp(const void *a, const void *b) {
    size_t sa = ((const pt_Range *)a)->start;
    size_t sb = ((const pt_Range *)b)->start;
    return sa < sb ? -1 : sa > sb;
}

static int ptZ_inrange(const pt_Compact *B, const char *s) {
    size_t q = (size_t)s, lo = 0, hi = B->nr;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        if (B->rs[mid].start <= q)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo > 0 && q < B->rs[lo - 1].end;
}

static int ptZ_addranges(pt_State *S, pt_Compact *B, const pt_Block *b) {
    for (; b; b = b->next) {
        if (B->nr == B->cap) {
            size_t nc = B->cap ? (B->cap + (B->cap >> 1)) : PT_COMPACT_RANGES;
            size_t sz = B->cap * sizeof(pt_Range), nz = nc * sizeof(pt_Range);
            pt_Range *nrs = (pt_Range *)S->allocf(S->alloc_ud, B->rs, sz, nz);
            if (nrs == NULL) return PT_ERRMEM;
            B->rs = nrs, B->cap = nc;
        }
        B->rs[B->nr].start = (size_t)(b + 1);
        B->rs[B->nr].end = B->rs[B->nr].start + b->size, B->nr += 1;
    }
    return PT_OK;
}

static int ptZ_collect(pt_State *S, const pt_Tree *t, pt_Compact *B) {
    for (; t != &S->empty; t = t->from) {
        pt_Block *c = t->arena.current, *f = t->arena.full;
        if (ptZ_addranges(S, B, c) || ptZ_addranges(S, B, f)) return PT_ERRMEM;
    }
    if (B->nr) qsort(B->rs, B->nr, sizeof(pt_Range), &ptZ_rangecmp);
    return PT_OK;
}

static int ptZ_append(pt_Cursor *C, pt_Compact *B) {
    pt_Node    *p;
    int         l = ptK_levels(C), i;
    pt_Delta    db = 0;
    size_t      n;
    const char *w, *s = pt_piece(B->oC, &n);
    i = ptN_cc(p = ptK_parent(C, l));
    for (; s && i < PT_FANOUT; s = pt_next(B->oC, &n)) {
        if (ptZ_inrange(B, s)) { /* migrate into the new arena */
            if ((w = pt_reserve(C, n)) == NULL) return PT_ERRMEM;
            memcpy((char *)w, s, n), s = pt_literal(C, n);
        }
        if (i > 0 && ptN_lit(p, i - 1) + p->bytes[i - 1] == s)
            p->bytes[i - 1] += n;
        else
            p->children[i] = (pt_Node *)s, p->bytes[i] = n, i += 1;
        db += (pt_Delta)n;
    }
    ptN_setcc(p, i), C->paths[l] = &p->children[assert(i > 0), i - 1];
    return ptM_up(C, l - 1, db), i == PT_FANOUT && s != NULL;
}

static int ptZ_build(pt_Cursor *C, pt_Compact *B) {
    int r, l;
    if ((r = ptK_markdirty(C)) != PT_OK) return r;
    while ((r = ptZ_append(C, B)) > 0) {
        for (l = ptK_levels(C); l >= 0; --l)
            if (ptN_cc(ptK_parent(C, l)) < PT_FANOUT) break;
        if ((r = ptD_makechain(C, l, ptK_levels(C), 0)) < 0) break;
    }
    if (r < 0) return r;
    for (l = 0; l < ptK_levels(C); ++l) ptD_foldnode(C, 0, l);
    return ptD_rebalance(C, 0), PT_OK;
}

PT_API pt_Buffer pt_compact(pt_State *S, pt_Buffer b) {
    pt_Cursor  nC, oC;
    pt_Compact B;
    int        r;
    if (S == NULL || b == NULL || b->S != S) return NULL;
    if (b->bytes == 0) return pt_empty(S);
    assert(b->root.mask == 0); /* committed blob has no hole */
    pt_seek(&oC, b, 0), pt_seek(&nC, pt_empty(S), 0);
    memset(&B, 0, sizeof(B)), B.oC = &oC;
    if ((r = ptZ_collect(S, b, &B)) == PT_OK) r = ptZ_build(&nC, &B);
    if (B.rs) S->allocf(S->alloc_ud, B.rs, B.cap * sizeof(pt_Range), 0);
    return (r == PT_OK) ? pt_commit(&nC) : (pt_rollback(&nC), (pt_Buffer)NULL);
}

PT_NS_END

#endif /* PT_IMPLEMENTATION */
