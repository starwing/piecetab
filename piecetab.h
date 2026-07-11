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
#define PT_ERREMPTY (-3) /* Empty tree or no next value */
#define PT_ERRFULL  (-4) /* Buffer full */

PT_NS_BEGIN

typedef struct pt_State  pt_State;
typedef struct pt_Cursor pt_Cursor;

typedef const struct pt_Tree *pt_Blob;

typedef ptrdiff_t pt_Delta;

typedef void *pt_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/* lifetime*/

PT_API pt_State *pt_newstate(pt_Alloc *allocf, void *ud);
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

/* literal scratch buffer */
PT_API char *pt_literal(pt_State *S, size_t *plen);
PT_API char *pt_scratch(pt_State *S, size_t *plen);

/* cursor definition */

#define PT_MAX_LEVEL 16

struct pt_Cursor {
    struct pt_Node **paths[PT_MAX_LEVEL];
    struct pt_Tree  *tree;
    size_t           poff;
    size_t           off;
    int              dirty;
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

#define PT_MASK_BITS (sizeof(pt_Mask) * CHAR_BIT)

#ifndef PT_MASK_SIZE
# define PT_MASK_SIZE ((int)((PT_FANOUT + PT_MASK_BITS - 1) / PT_MASK_BITS))
#endif

#define pt_min(a, b) ((a) < (b) ? (a) : (b))
#define pt_max(a, b) ((a) > (b) ? (a) : (b))

#define PT_STATIC_ASSERT(cond)      PT_SA_0(cond, pt_SA_, __LINE__)
#define PT_SA_0(cond, prefix, line) PT_SA_1(cond, prefix, line)
#define PT_SA_1(cond, prefix, line) typedef char prefix##line[(cond) ? 1 : -1]

PT_NS_BEGIN

typedef size_t    pt_Mask;
typedef ptrdiff_t pt_Diff;
typedef unsigned  pt_Ver;

typedef struct pt_Hole {
    char data[PT_MAX_HOLESIZE];
} pt_Hole;

PT_STATIC_ASSERT(sizeof(pt_Hole) % sizeof(void *) == 0);

typedef struct pt_Node {
    size_t          bytes[PT_FANOUT];
    pt_Mask         mask[PT_MASK_SIZE];
    struct pt_Node *children[PT_FANOUT];
    pt_Ver          version;
    unsigned short  child_count;
} pt_Node;

typedef struct pt_Tree {
    pt_State       *S;
    struct pt_Tree *from;
    size_t          bytes;
    pt_Node         root;
    unsigned        refc;
    unsigned short  levels;
} pt_Tree;

typedef struct pt_Pool {
    size_t obj_size; /* size of each object in this pool */
    void  *freed;    /* freelist head */
    void  *pages;    /* linked list of allocated pages */
#ifdef PT_POOL_STATS
    size_t live_obj;
#endif
} pt_Pool;

typedef struct pt_Scratch {
    size_t remain;  /* remaining bytes in current buffer */
    char  *buffer;  /* current write position */
    void  *pages;   /* linked list of allocated pages */
    void  *reserve; /* linked list of pre-allocated pages for commit */
} pt_Scratch;

struct pt_State {
    void      *alloc_ud;
    pt_Alloc  *allocf;
    pt_Scratch scratch;
    pt_Pool    nodes;
    pt_Pool    holes;
    pt_Pool    trees;
    pt_Tree    empty;
    pt_Ver     max_version;
};

/* mempool */

#ifdef PT_POOL_STATS
# define ptP_stat(stmt) stmt
#else
# define ptP_stat(stmt) ((void)0)
#endif

static void ptP_init(pt_Pool *pool, size_t obj_size) {
    memset(pool, 0, sizeof(pt_Pool)), pool->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < PT_PAGE_SIZE / 4);
}

/* clang-format off */
static void ptP_free(pt_Pool *pool, void *obj)
{ ptP_stat(pool->live_obj-=1); *(void **)obj = pool->freed, pool->freed = obj; }

static void ptP_initscratch(pt_Scratch* pool)
{ memset(pool, 0, sizeof(pt_Scratch)); }
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
    char *obj = pool->freed;
    assert(obj), ptP_stat(pool->live_obj += 1);
    return (pool->freed = *(void **)obj), (void *)obj;
}

static void *ptP_alloc(pt_State *S, pt_Pool *pool) {
    size_t sz = pool->obj_size;
    char  *p, *end, *obj = pool->freed;
    if (obj) return ptP_ralloc(pool);
    p = (char *)S->allocf(S->alloc_ud, NULL, 0, PT_PAGE_SIZE);
    if (p == NULL) return NULL;
    end = &p[PT_PAGE_SIZE - sizeof(void *)], *(void **)end = pool->pages;
    pool->pages = (void *)(obj = p), p += sz, end -= sz;
    while ((p += sz) < end) *(void **)(p - sz) = p;
    *(void **)(p - sz) = NULL, ptP_stat(pool->live_obj += 1);
    return (pool->freed = (void *)(obj + sz)), (void *)obj;
}

static int ptP_reserve(pt_State *S, pt_Pool *pool, size_t n) {
    void  *freed = pool->freed, **t = &freed;
    size_t c;
    for (c = 0; c < n && *t; ++c) t = (void **)*t;
    if (c >= n) return 1;
    for (pool->freed = NULL; c < n; ++c) {
        void *obj = ptP_alloc(S, pool);
        if (obj == NULL) return 0;
        ptP_stat(pool->live_obj -= 1);
        *t = obj, t = (void **)obj;
    }
    return *t = NULL, (pool->freed = freed), 1;
}

static char *ptP_scratch(pt_State *S) {
    void *p = S->scratch.reserve;
    if (S->scratch.remain != 0) return S->scratch.buffer;
    if (!p && !(p = S->allocf(S->alloc_ud, NULL, 0, PT_PAGE_SIZE))) return 0;
    if (p == S->scratch.reserve) S->scratch.reserve = *(void **)p;
    *(void **)p = S->scratch.pages, S->scratch.pages = p;
    S->scratch.pages = p;
    S->scratch.remain = PT_PAGE_SIZE - sizeof(void *);
    return S->scratch.buffer = (char *)p + sizeof(void *);
}

static void ptP_freescratch(pt_State *S, pt_Scratch *pool) {
    void *next, *page = pool->pages;
    for (; page; page = next)
        next = *(void **)page, S->allocf(S->alloc_ud, page, PT_PAGE_SIZE, 0);
    for (page = pool->reserve; page; page = next)
        next = *(void **)page, S->allocf(S->alloc_ud, page, PT_PAGE_SIZE, 0);
    ptP_initscratch(pool);
}

/* reserve enough scratch pages so freeze never calls allocf (全或无). */
static int ptP_reservescratch(pt_State *S, size_t total) {
    size_t usable = PT_PAGE_SIZE - sizeof(void *);
    size_t need, pages;
    void  *page, *head, *tail, *tmp;
    if (total == 0) return 1;
    need = (total > S->scratch.remain) ? total - S->scratch.remain : 0;
    if (need == 0) return 1;
    pages = need / (usable - PT_MAX_HOLESIZE) + 1;
    head = NULL;
    tail = NULL;
    while (pages--) {
        page = S->allocf(S->alloc_ud, NULL, 0, PT_PAGE_SIZE);
        if (page == NULL) {
            while (head) {
                tmp = *(void **)head;
                S->allocf(S->alloc_ud, head, PT_PAGE_SIZE, 0);
                head = tmp;
            }
            return 0;
        }
        *(void **)page = NULL;
        if (tail) {
            *(void **)tail = page;
            tail = page;
        } else {
            head = page;
            tail = page;
        }
    }
    if (head) {
        *(void **)tail = S->scratch.reserve;
        S->scratch.reserve = head;
    }
    return 1;
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

/* clang-format off */
static size_t ptN_sumbytes(const pt_Node *n, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += n->bytes[i]; return s; }
/* clang-format on */

PT_API unsigned pt_version(pt_Blob b) { return b ? b->root.version : 0; }
PT_API size_t   pt_bytes(pt_Blob b) { return b ? b->bytes : 0; }
PT_API unsigned pt_retain(pt_Blob b) { return b ? ++((pt_Tree *)b)->refc : 0; }
PT_API pt_Blob  pt_empty(pt_State *S) { return S ? &S->empty : NULL; }

static void ptM_sethole(pt_Node *n, int i, int ishole) {
    pt_Mask *m = &n->mask[i / PT_MASK_BITS];
    *m ^= (-!!ishole ^ *m) & ((pt_Mask)1 << (i % PT_MASK_BITS));
}

static int ptM_ishole(const pt_Node *n, int i) {
    const pt_Mask m = n->mask[i / PT_MASK_BITS];
    return (m & ((pt_Mask)1 << (i % PT_MASK_BITS))) != 0;
}

static void ptM_movebits(pt_Node *dst, pt_Node *src, int from, int n) {
    int j;
    for (j = 0; j < n; ++j) ptM_sethole(dst, j, ptM_ishole(src, from + j));
}

static void ptM_setsubmask(pt_Node *p, int i, const pt_Node *child) {
    size_t w;
    for (w = 0; w < PT_MASK_SIZE && !child->mask[w]; ++w) continue;
    ptM_sethole(p, i, w < PT_MASK_SIZE);
}

static void ptM_upmask(pt_Cursor *C) {
    int l;
    for (l = ptK_levels(C) - 1; l >= 0; --l) {
        pt_Node *p = ptK_parent(C, l);
        ptM_setsubmask(p, ptK_idx(C, p, l), ptK_parent(C, l + 1));
    }
}

static void ptM_upbytes(pt_Cursor *C, int l, pt_Diff db) {
    if (db == 0) return;
    for (; l >= 0; --l) {
        pt_Node *p = ptK_parent(C, l);
        int      i = ptK_idx(C, p, l);
        p->bytes[i] += db;
    }
    C->tree->bytes += db;
}

static void ptN_purge(pt_State *S, pt_Node *p, int k, int s, int e, pt_Ver v) {
    int i;
    if (k == 0) {
        for (i = s; i < e; ++i)
            if (ptM_ishole(p, i)) ptP_free(&S->holes, p->children[i]);
    } else {
        for (i = s; i < e; ++i) {
            pt_Node *c = (pt_Node *)p->children[i];
            if (c->version == v) {
                ptN_purge(S, c, k - 1, 0, c->child_count, v);
                ptP_free(&S->nodes, c);
            }
        }
    }
}

static void ptN_move(pt_Node *d, int di, int si, int n) {
    int i;
    memmove(&d->children[di], &d->children[si], n * sizeof(pt_Node *));
    memmove(&d->bytes[di], &d->bytes[si], n * sizeof(size_t));
    if (di < si)
        for (i = 0; i < n; ++i) ptM_sethole(d, di + i, ptM_ishole(d, si + i));
    else
        for (i = n - 1; i >= 0; --i)
            ptM_sethole(d, di + i, ptM_ishole(d, si + i));
}

static void ptN_remove(pt_State *S, pt_Node *p, int k, int s, int e) {
    int m = ptN_cc(p) - e;
    ptN_purge(S, p, k, s, e, p->version);
    assert(s <= e && e <= ptN_cc(p));
    ptN_move(p, s, e, m);
    p->child_count -= e - s;
}

static void ptN_makespace(pt_Node *p, int i, int n) {
    int w, m = p->child_count - i;
    assert(p->child_count + n <= PT_FANOUT && i <= p->child_count);
    memmove(&p->children[i + n], &p->children[i], m * sizeof(pt_Node *));
    memmove(&p->bytes[i + n], &p->bytes[i], m * sizeof(size_t));
    for (w = m - 1; w >= 0; --w) {
        ptM_sethole(p, w + i + n, ptM_ishole(p, w + i));
        ptM_sethole(p, w + i, 0);
    }
    p->child_count += n;
}

static void ptN_copy(pt_Node *d, int di, const pt_Node *s, int si, int n) {
    int i;
    memcpy(&d->children[di], &s->children[si], n * sizeof(pt_Node *));
    memcpy(&d->bytes[di], &s->bytes[si], n * sizeof(size_t));
    for (i = 0; i < n; ++i) ptM_sethole(d, di + i, ptM_ishole(s, si + i));
}

/* lifetime */

static void *ptS_defallocf(void *ud, void *ptr, size_t osize, size_t nsize) {
    void *newptr;
    (void)ud, (void)osize;
    if (nsize == 0) return free(ptr), (void *)NULL;
    newptr = realloc(ptr, nsize);
    if (newptr == NULL) abort(); /* failure is unrecoverable by default */
    return newptr;
}

PT_API pt_State *pt_newstate(pt_Alloc *allocf, void *ud) {
    pt_State *S;
    if (allocf == NULL) allocf = &ptS_defallocf;
    S = (pt_State *)allocf(ud, NULL, 0, sizeof(pt_State));
    if (!S) return NULL;
    memset(S, 0, sizeof(pt_State));
    S->alloc_ud = ud;
    S->allocf = allocf;
    ptP_init(&S->nodes, sizeof(pt_Node));
    ptP_init(&S->holes, sizeof(pt_Hole));
    ptP_init(&S->trees, sizeof(pt_Tree));
    ptP_initscratch(&S->scratch);
    S->empty.S = S, S->empty.refc = 1;
    S->max_version = 0;
    return S;
}

PT_API void pt_close(pt_State *S) {
    if (S == NULL) return;
    ptP_destroy(S, &S->nodes);
    ptP_destroy(S, &S->holes);
    ptP_destroy(S, &S->trees);
    ptP_freescratch(S, &S->scratch);
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
    t->root.bytes[0] = len;
    t->root.child_count = 1;
    t->root.version = ++S->max_version;
    return t->bytes = len, t;
}

PT_API char *pt_literal(pt_State *S, size_t *plen) {
    char *p;
    if (S == NULL || plen == NULL || *plen == 0) return NULL;
    if ((p = ptP_scratch(S)) == NULL) return NULL;
    if (*plen > S->scratch.remain) *plen = S->scratch.remain;
    return S->scratch.remain -= *plen, S->scratch.buffer += *plen, p;
}

PT_API char *pt_scratch(pt_State *S, size_t *plen) {
    char *p;
    if (S == NULL || plen == NULL) return NULL;
    if ((p = ptP_scratch(S)) == NULL) return NULL;
    return *plen = S->scratch.remain, p;
}

PT_API unsigned pt_release(pt_Blob b) {
    pt_Tree *t = (pt_Tree *)b;
    if (t == NULL || t == &t->S->empty) return 0;
    if (t->refc > 1) return --t->refc;
    for (;;) {
        pt_Tree *nt = t->from;
        pt_Node *r = (assert(nt), &t->root);
        ptN_purge(t->S, r, t->levels, 0, r->child_count, r->version);
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
        n = *(C->paths[l] = &n->children[n->child_count - 1]);
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
    pt_Node *p = ptK_parent(C, ptK_levels(C));
    int      l, i = ptK_idx(C, p, ptK_levels(C));
    size_t   in = p->bytes[i] - C->poff;
    if (d < in) return C->poff += d, 0;
    d -= in, C->off += p->bytes[i], C->poff = 0;
    for (l = ptK_levels(C); l >= 0; --l) {
        p = ptK_parent(C, l), i = ptK_idx(C, p, l) + 1;
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
        p = ptK_parent(C, l), i = ptK_idx(C, p, l) - 1;
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

/* insert */

static int ptK_markdirty(pt_Cursor *C) {
    pt_State *S = C->tree->S;
    pt_Tree  *old = C->tree, *nt;
    if (C->dirty) return PT_OK;
    if (!(nt = (pt_Tree *)ptP_alloc(S, &S->trees))) return PT_ERRMEM;
    *nt = *old;
    nt->root.version = ++S->max_version, nt->refc = 1;
    nt->from = old, pt_retain(old); /* keep source alive: COW lifetime */
    C->paths[0] = nt->root.children + (C->paths[0] - old->root.children);
    return C->tree = nt, C->dirty = 1, PT_OK;
}

static pt_Node *ptK_cow(pt_Cursor *C, int l, int d) {
    pt_State *S = C->tree->S;
    pt_Node  *p = ptK_parent(C, l);
    int       i = ptK_idx(C, p, l) + d;
    pt_Node  *n = p->children[i], *nn;
    if (l == ptK_levels(C) || n->version == C->tree->root.version) return n;
    nn = (pt_Node *)ptP_ralloc(&S->nodes);
    *nn = *n, nn->version = C->tree->root.version;
    if (d == 0) C->paths[l + 1] = &nn->children[C->paths[l + 1] - n->children];
    return p->children[i] = nn;
}

static int ptK_beginedit(pt_Cursor *C, size_t need) {
    int r, l;
    if (!ptP_reserve(C->tree->S, &C->tree->S->nodes, need)) return PT_ERRMEM;
    if ((r = ptK_markdirty(C)) != PT_OK) return r;
    for (l = 0; l < ptK_levels(C); ++l) ptK_cow(C, l, 0);
    return PT_OK;
}

static void ptI_onepiece(pt_Cursor *C, const char *s, size_t len) {
    pt_Node *root = &C->tree->root;
    root->children[0] = (pt_Node *)s, root->bytes[0] = len;
    root->child_count = 1, ptM_sethole(root, 0, 0);
    C->tree->bytes = len;
    C->paths[0] = &root->children[0], C->off = 0, C->poff = 0;
}

static void ptI_splitroot(pt_Cursor *C) {
    pt_State *S = C->tree->S;
    pt_Node  *root = &C->tree->root, save = *root;
    pt_Node  *pp = (pt_Node *)ptP_ralloc(&S->nodes);
    pt_Node  *nw = (pt_Node *)ptP_ralloc(&S->nodes);
    int       i = ptK_idx(C, root, 0), mid = save.child_count / 2;
    int       nc = save.child_count - mid;
    *pp = save, pp->child_count = (unsigned short)mid;
    for (nc = mid; nc < save.child_count; ++nc) ptM_sethole(pp, nc, 0);
    nc = save.child_count - mid;
    memset(nw->mask, 0, sizeof(nw->mask));
    memcpy(nw->children, &save.children[mid], nc * sizeof(pt_Node *));
    memcpy(nw->bytes, &save.bytes[mid], nc * sizeof(size_t));
    ptM_movebits(nw, &save, mid, nc);
    nw->child_count = (unsigned short)nc;
    pp->version = nw->version = C->tree->root.version;
    root->children[0] = pp, root->children[1] = nw, root->child_count = 2;
    root->bytes[0] = ptN_sumbytes(pp, 0, mid);
    root->bytes[1] = C->tree->bytes - root->bytes[0];
    ptM_setsubmask(root, 0, pp), ptM_setsubmask(root, 1, nw);
    C->tree->levels++;
    memmove(C->paths + 1, C->paths, C->tree->levels * sizeof(pt_Node **));
    C->paths[0] = &root->children[i >= mid];
    C->paths[1] = &(*C->paths[0])->children[i < mid ? i : i - mid];
}

static void ptI_splitnode(pt_Cursor *C, int l) {
    pt_State *S = C->tree->S;
    pt_Node  *nd = ptK_cow(C, l, 0), *nw, *p;
    int       i, cs, mid = nd->child_count / 2, nc = nd->child_count - mid;
    p = ptK_parent(C, l), i = ptK_idx(C, p, l);
    nw = (pt_Node *)ptP_ralloc(&S->nodes), nw->version = C->tree->root.version;
    memset(nw->mask, 0, sizeof(nw->mask));
    memcpy(nw->children, &nd->children[mid], nc * sizeof(pt_Node *));
    memcpy(nw->bytes, &nd->bytes[mid], nc * sizeof(size_t));
    ptM_movebits(nw, nd, mid, nc);
    nw->child_count = (unsigned short)nc, nd->child_count = (unsigned short)mid;
    ptN_makespace(p, i + 1, 1), p->children[i + 1] = nw;
    p->bytes[i] = ptN_sumbytes(nd, 0, mid);
    p->bytes[i + 1] = ptN_sumbytes(nw, 0, nc);
    ptM_setsubmask(p, i, nd), ptM_setsubmask(p, i + 1, nw);
    cs = (int)(C->paths[l + 1] - nd->children);
    if (cs >= mid)
        C->paths[l] = &p->children[i + 1],
        C->paths[l + 1] = &nw->children[cs - mid];
}

/* Ensure the cursor's leaf node has `need` free slots, splitting full
 * ancestors down the path (or growing the root). The loop's last pass
 * (l=levels-1) splits the leaf node too (a pt_Node); pieces are atomic. */
static void ptI_makeroom(pt_Cursor *C, int need) {
    int l;
    for (l = ptK_levels(C); l >= 0; --l)
        if (ptK_parent(C, l)->child_count + need <= PT_FANOUT) break;
    if (l < 0) ptI_splitroot(C), l = 1;
    for (; l < ptK_levels(C); ++l) ptI_splitnode(C, l);
}

/* split the cursor's leaf piece i into [0,poff) at i and [poff,len) at i+1,
 * returning the index i+1 where a new literal is to be inserted between. */
static int ptI_midsplit(pt_Cursor *C, pt_Node *p, int i) {
    ptN_makespace(p, i + 1, 1);
    p->bytes[i + 1] = p->bytes[i] - C->poff, p->bytes[i] = C->poff;
    p->children[i + 1] = (pt_Node *)((char *)p->children[i] + C->poff);
    return ptM_sethole(p, i + 1, 0), i + 1;
}

static void ptK_mergelit(pt_Cursor *C) {
    int      l = ptK_levels(C);
    pt_Node *p = ptK_parent(C, l);
    int      i = ptK_idx(C, p, l);
    assert(p->child_count == 0 || p->bytes[i] > 0);
    if (i + 1 < p->child_count && !ptM_ishole(p, i) && !ptM_ishole(p, i + 1)
        && (char *)p->children[i] + p->bytes[i] == (char *)p->children[i + 1])
        p->bytes[i] += p->bytes[i + 1],
                ptN_remove(C->tree->S, p, 0, i + 1, i + 2);
    if (i > 0 && !ptM_ishole(p, i - 1) && !ptM_ishole(p, i)
        && (char *)p->children[i - 1] + p->bytes[i - 1]
                   == (char *)p->children[i]) {
        size_t g = p->bytes[i - 1];
        p->bytes[i - 1] += p->bytes[i], ptN_remove(C->tree->S, p, 0, i, i + 1);
        C->paths[l] = &p->children[i - 1], C->off -= g, C->poff += g;
    }
}

PT_API int pt_insert(pt_Cursor *C, const char *s, size_t len) {
    size_t   off;
    pt_Node *p;
    int      l, i, ins, mid, r;
    if (C == NULL || C->tree == NULL || s == NULL) return PT_ERRPARAM;
    off = pt_offset(C);
    if (len == 0) return PT_OK;
    if ((r = ptK_beginedit(C, 2 * ptK_levels(C) + 3)) != PT_OK) return r;
    if (C->tree->root.child_count == 0) return ptI_onepiece(C, s, len), PT_OK;
    l = ptK_levels(C), p = ptK_parent(C, l), i = ptK_idx(C, p, l);
    mid = (C->poff > 0 && C->poff < p->bytes[i]);
    ptI_makeroom(C, mid ? 2 : 1);
    l = ptK_levels(C), p = ptK_parent(C, l), i = ptK_idx(C, p, l);
    ins = mid ? ptI_midsplit(C, p, i) : i + (C->poff > 0);
    ptN_makespace(p, ins, 1);
    p->children[ins] = (pt_Node *)s, p->bytes[ins] = len;
    ptM_sethole(p, ins, 0), C->paths[l] = &p->children[ins];
    ptM_upbytes(C, l - 1, (pt_Diff)len);
    C->off = off, C->poff = 0;
    return ptK_mergelit(C), PT_OK;
}

PT_API int pt_append(pt_Cursor *C, const char *s, size_t len) {
    int r = pt_insert(C, s, len);
    if (r == PT_OK && len > 0) pt_advance(C, (pt_Delta)len);
    return r;
}

/* edit */

/* makeroom needs at most 2 free slots; a split of a full node leaves
 * FANOUT/2 free in the cursor's half, so require FANOUT >= 4. */
PT_STATIC_ASSERT(PT_FANOUT >= 4);

static void ptH_append(pt_Node *n, int i, size_t d, const char *s, size_t len) {
    pt_Hole *h = ptN_hole(n, i);
    size_t   end = n->bytes[i];
    assert((size_t)d <= end && end + len <= PT_MAX_HOLESIZE);
    memmove(h->data + d + len, h->data + d, end - d);
    memcpy(h->data + d, s, len);
    n->bytes[i] = end + len;
}

static void ptH_remove(pt_Node *n, int i, size_t d, size_t len) {
    pt_Hole *h = ptN_hole(n, i);
    size_t   end = n->bytes[i];
    assert((size_t)d <= end && d + len <= end);
    memmove(h->data + d, h->data + d + len, end - (d + len));
    n->bytes[i] = end - len;
}

static pt_Hole *ptH_new(pt_State *S, const char *s, size_t len) {
    pt_Hole *h = (pt_Hole *)ptP_alloc(S, &S->holes);
    assert(h && len <= PT_MAX_HOLESIZE);
    memcpy(h->data, s, len);
    return h;
}

/* insert first hole into an empty tree */
static void ptH_onepiece(pt_Cursor *C, const char *s, size_t len) {
    pt_State *S = C->tree->S;
    pt_Node  *root = &C->tree->root;
    pt_Hole  *h = ptH_new(S, s, len);
    root->children[0] = (pt_Node *)h;
    root->bytes[0] = len;
    root->child_count = 1;
    ptM_sethole(root, 0, 1);
    C->tree->bytes = len;
    C->paths[0] = &root->children[0];
    C->off = 0;
    C->poff = len;
}

/* insert a fresh hole slot at cursor position, reusing ptI machine.
   pre: tree non-empty, C->dirty, reserves already held */
static void ptH_putslot(pt_Cursor *C, const char *s, size_t len) {
    pt_State *S = C->tree->S;
    pt_Node  *p;
    int       l, i, ins, mid;
    size_t    off = C->off + C->poff;
    l = ptK_levels(C);
    p = ptK_parent(C, l);
    i = ptK_idx(C, p, l);
    mid = (C->poff > 0 && C->poff < p->bytes[i]);
    assert(!mid || !ptM_ishole(p, i)); /* 开放点1: midsplit 对 hole 不成立 */
    ptI_makeroom(C, mid ? 2 : 1);
    l = ptK_levels(C);
    p = ptK_parent(C, l);
    i = ptK_idx(C, p, l);
    ins = mid ? ptI_midsplit(C, p, i) : i + (C->poff > 0);
    ptN_makespace(p, ins, 1);
    {
        pt_Hole *h = ptH_new(S, s, len);
        p->children[ins] = (pt_Node *)h;
        p->bytes[ins] = len;
        ptM_sethole(p, ins, 1);
    }
    C->paths[l] = &p->children[ins];
    ptM_upbytes(C, l - 1, (pt_Diff)len);
    ptM_upmask(C); /* 开放点2: 插 hole 后必须传播 mask */
    C->off = off, C->poff = len;
}

/* split hole at cursor when n+len > CAP: left-orig + fresh + right, net +2
   slots. pre: cursor in hole middle (0<poff<n), C->dirty, reserves held */
static void ptH_splitins(pt_Cursor *C, const char *s, size_t len) {
    pt_State *S = C->tree->S;
    size_t    off = pt_offset(C);
    int       i, l = ptK_levels(C);
    pt_Node  *p;
    pt_Hole  *rh, *nh, *h;
    size_t    poff, n;
    p = ptK_parent(C, l), i = ptK_idx(C, p, l);
    h = (pt_Hole *)p->children[i];
    poff = C->poff, n = p->bytes[i];
    rh = ptH_new(S, h->data + poff, n - poff); /* right half */
    p->bytes[i] = poff;                        /* left half shrunk */
    ptI_makeroom(C, 2);
    l = ptK_levels(C), p = ptK_parent(C, l), i = ptK_idx(C, p, l);
    ptN_makespace(p, i + 1, 2);
    nh = ptH_new(S, s, len); /* fresh middle */
    p->children[i + 1] = (pt_Node *)nh;
    p->bytes[i + 1] = len;
    ptM_sethole(p, i + 1, 1);
    p->children[i + 2] = (pt_Node *)rh;
    p->bytes[i + 2] = n - poff;
    ptM_sethole(p, i + 2, 1);
    C->paths[l] = &p->children[i + 1];
    ptM_upbytes(C, l - 1, (pt_Diff)len);
    ptM_upmask(C);
    C->off = off, C->poff = len;
}

/* hole insert dispatcher: 4 branches A/B/C/D.
   pre: C->dirty, reserves already held, tree non-empty.
   post: cursor at insertion end (off+poff increased by len). */
static void ptK_inserthole(pt_Cursor *C, const char *s, size_t len) {
    pt_Node *p;
    int      l, i;
    assert(C->dirty);
    if (C->tree->root.child_count == 0) {
        ptH_onepiece(C, s, len);
        return;
    }
    l = ptK_levels(C);
    p = ptK_parent(C, l);
    i = ptK_idx(C, p, l);
    /* A: current piece is a hole, cursor at tail, fits */
    if (ptM_ishole(p, i) && C->poff == p->bytes[i]
        && p->bytes[i] + len <= PT_MAX_HOLESIZE) {
        ptH_append(p, i, p->bytes[i], s, len);
        C->poff += len;
        ptM_upbytes(C, l - 1, (pt_Diff)len);
        ptM_upmask(C);
        return;
    }
    /* B: cursor in hole middle */
    if (ptM_ishole(p, i) && C->poff > 0 && C->poff < p->bytes[i]) {
        if (p->bytes[i] + len <= PT_MAX_HOLESIZE) {
            ptH_append(p, i, C->poff, s, len), C->poff += len;
            ptM_upbytes(C, l - 1, (pt_Diff)len);
            ptM_upmask(C);
            return;
        }
        ptH_splitins(C, s, len);
        return;
    }
    /* C: poff==0 and previous piece is a hole that fits */
    if (C->poff == 0 && i > 0 && ptM_ishole(p, i - 1)) {
        if (p->bytes[i - 1] + len <= PT_MAX_HOLESIZE) {
            ptH_append(p, i - 1, p->bytes[i - 1], s, len);
            C->paths[l] = &p->children[i - 1];
            C->poff = p->bytes[i - 1];
            C->off -= p->bytes[i - 1] - len;
            ptM_upbytes(C, l - 1, (pt_Diff)len);
            ptM_upmask(C);
            return;
        }
        /* prev hole full -> fall through to D */
    }
    /* D: fresh hole slot */
    ptH_putslot(C, s, len);
}

PT_API int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len) {
    int r, lvls;
    if (!C || !C->tree) return PT_ERRPARAM;
    if (len > PT_MAX_HOLESIZE) return PT_ERRPARAM;
    if (s == NULL && len > 0) return PT_ERRPARAM;
    if (del == 0 && len == 0) return PT_OK;
    lvls = ptK_levels(C);
    if (!ptP_reserve(C->tree->S, &C->tree->S->nodes, 6 * lvls + 8))
        return PT_ERRMEM;
    if (len > 0 && !ptP_reserve(C->tree->S, &C->tree->S->holes, 2))
        return PT_ERRMEM;
    if (del > 0) {
        r = pt_remove(C, del);
        if (r != PT_OK) return r;
    }
    if (len > 0) {
        if (!C->dirty) {
            r = ptK_beginedit(C, 2 * lvls + 3);
            if (r != PT_OK) return r;
        }
        ptK_inserthole(C, s, len);
    }
    return PT_OK;
}

/* remove */

static void ptD_trimright(pt_Cursor *L, int l) {
    pt_Node *p = ptK_parent(L, l);
    int      i = ptK_idx(L, p, l);
    size_t   old = p->bytes[i];
    if (L->poff > 0 && L->poff < old) {
        p->bytes[i] = L->poff;
        ptM_upbytes(L, l - 1, -(pt_Diff)(old - L->poff));
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
        else
            p->children[i] = (pt_Node *)(ptN_lit(p, i) + poffR),
            p->bytes[i] -= poffR;
        ptM_upbytes(R, l - 1, -(pt_Diff)poffR);
        R->poff = 0;
    }
}

static void ptD_cutdiv(pt_Cursor *L, pt_Cursor *R, pt_Node *rt, int fl) {
    pt_State *S = L->tree->S;
    int       levels = ptK_levels(L), k = levels - fl, i, cc;
    size_t    db;
    pt_Node  *p = ptK_parent(R, fl);
    i = ptK_idx(R, p, fl), cc = ptN_cc(p);
    i += !(fl == levels && R->poff < p->bytes[i]);
    rt[k].child_count = (unsigned short)(cc - i);
    ptN_copy(&rt[k], 0, p, i, cc - i), p->child_count = (unsigned short)i;
    i = ptK_idx(L, p, fl), i += !(fl == levels && L->poff == 0);
    db = ptN_sumbytes(p, i, cc), ptM_upbytes(L, fl - 1, -(pt_Diff)db);
    ptN_remove(S, p, k, i, ptN_cc(p));
}

static void ptD_cutrange(pt_Cursor *L, pt_Cursor *R, pt_Node *rt, int fl) {
    pt_State *S = L->tree->S;
    int       levels = ptK_levels(L), kl, k, i, cc;
    size_t    db;
    pt_Node  *p;
    for (kl = levels; kl > fl; --kl) {
        k = levels - kl;
        p = ptK_parent(L, kl), i = ptK_idx(L, p, kl), cc = ptN_cc(p);
        i += !(kl == levels && L->poff == 0);
        db = ptN_sumbytes(p, i, cc), ptM_upbytes(L, kl - 1, -(pt_Diff)db);
        ptN_remove(S, p, k, i, cc);
        p = ptK_parent(R, kl), i = ptK_idx(R, p, kl), cc = ptN_cc(p);
        i += !(kl == levels && R->poff < p->bytes[i]);
        rt[k].child_count = (unsigned short)(cc - i);
        ptN_copy(&rt[k], 0, p, i, cc - i);
        ptN_remove(S, p, k, 0, i), p->child_count = 0;
    }
    ptD_cutdiv(L, R, rt, fl);
}

static int ptD_makechain(pt_Cursor *C, int from, int to) {
    pt_Node *p, *nn, ***cp = C->paths + to;
    int      l, r = 0;
    if (from < 0) {
        nn = (pt_Node *)ptP_ralloc(&C->tree->S->nodes);
        p = &C->tree->root, *nn = *p;
        p->bytes[0] = C->tree->bytes;
        memset(p->mask, 0, sizeof(p->mask));
        p->children[0] = nn, p->child_count = 1;
        memmove(cp + 2, cp + 1, (ptK_levels(C) - to) * sizeof(pt_Node **));
        C->tree->levels += 1, from = 0, to += 1, cp += 1, r = 1;
    }
    for (l = from; l < to; ++l) {
        nn = (pt_Node *)ptP_ralloc(&C->tree->S->nodes);
        p = ptK_parent(C, l), nn->child_count = 0;
        nn->version = C->tree->root.version;
        memset(nn->mask, 0, sizeof(nn->mask));
        p->bytes[p->child_count] = 0;
        p->children[p->child_count] = nn, p->child_count++;
        C->paths[l] = &p->children[p->child_count - 1];
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
    if (fl >= 0 && (c = p->child_count - i - 1) > 0) {
        int    k = ptK_levels(C) - fl;
        size_t db = ptN_sumbytes(p, i + 1, p->child_count);
        ptM_upbytes(C, fl - 1, -(pt_Diff)db);
        rt[k].child_count = 0;
        ptN_copy(&rt[k], 0, p, i + 1, c);
        p->child_count = (unsigned short)(i + 1);
        rt[k].child_count = (unsigned short)c;
    }
    return ptD_makechain(C, fl, l);
}

static void ptD_backwardnode(pt_Cursor *C, int d, int l) {
    pt_Node *p = ptK_parent(C, l);
    int      dl, i = ptK_idx(C, p, l);
    if (d > i) {
        d -= i + 1, dl = l;
        while (--dl >= 0 && ptK_idx(C, ptK_parent(C, dl), dl) == 0) continue;
        assert(dl >= 0);
        C->paths[dl] -= 1;
        while (++dl <= l) {
            p = ptK_parent(C, dl);
            C->paths[dl] = &p->children[p->child_count - 1];
        }
    }
    C->paths[l] -= d;
}

static int ptD_balancenode(pt_Node **ns, int left, pt_Diff *ds) {
    int d, l = ns[0]->child_count, r = ns[1]->child_count;
    d = l - ((l + r + (left != 0)) >> 1);
    assert(d != 0);
    if (d < 0) {
        ptN_copy(ns[0], l, ns[1], 0, -d);
        ptN_move(ns[1], 0, -d, r + d);
        *ds = -(pt_Diff)ptN_sumbytes(ns[0], l, l - d);
    } else {
        ptN_move(ns[1], d, 0, r);
        ptN_copy(ns[1], 0, ns[0], l - d, d);
        *ds = (pt_Diff)ptN_sumbytes(ns[1], 0, d);
    }
    ns[0]->child_count = (unsigned short)(l - d);
    ns[1]->child_count = (unsigned short)(r + d);
    return d;
}

static int ptD_foldnode(pt_Cursor *C, int lfirst, int l) {
    pt_Node  *p = ptK_parent(C, l), ***cp = &C->paths[l];
    int       w, cl, cr, dn, i = ptK_idx(C, p, l), doff;
    pt_Node **ns = &p->children[i];
    pt_Diff   ds;
    if (assert(ptN_cc(p) > 1), ptN_cc(ns[0]) > PT_FANOUT / 2) return 0;
    if ((i && lfirst) || i == p->child_count - 1) ns -= 1, i -= 1;
    doff = (int)(ns - C->paths[l]);
    ns[0] = ptK_cow(C, l, doff);
    ns[1] = ptK_cow(C, l, doff + 1);
    if ((cl = ptN_cc(ns[0])) + (cr = ptN_cc(ns[1])) <= PT_FANOUT) {
        ptN_copy(ns[0], cl, ns[1], 0, cr);
        ns[0]->child_count += cr, ns[1]->child_count -= cr;
        p->bytes[i] += p->bytes[i + 1];
        for (w = 0; w < PT_MASK_SIZE; ++w) ns[0]->mask[w] |= ns[1]->mask[w];
        if (doff)
            cp[1] = &ns[0]->children[cp[1] - ns[1]->children + cl], cp[0] -= 1;
        return ptN_remove(C->tree->S, p, ptK_levels(C) - l, i + 1, i + 2), 1;
    }
    dn = ptD_balancenode(ns, !doff, &ds);
    assert(dn != 0 && (dn < 0) != (doff != 0));
    p->bytes[i] -= ds, p->bytes[i + 1] += ds;
    if (doff) cp[1] += dn;
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

static int ptD_checkstitch(pt_Cursor *C) {
    return ptP_reserve(C->tree->S, &C->tree->S->nodes, ptK_levels(C) + 2);
}

static void ptD_stitchnode(pt_Cursor *L, pt_Node *rt) {
    int k, d = 0, l = ptK_levels(L);
    for (k = 0; k <= ptK_levels(L); ++k) {
        int      m, fl, r, kl = ptK_levels(L) - k, rtcc = ptN_cc(&rt[k]);
        pt_Node *p = ptK_parent(L, kl);
        size_t   db;
        ptN_setcc(&rt[k], 0);
        if ((m = pt_min(rtcc, PT_FANOUT - ptN_cc(p))) > 0) {
            ptN_copy(p, ptN_cc(p), &rt[k], 0, m), ptN_setcc(p, ptN_cc(p) + m);
            db = ptN_sumbytes(&rt[k], 0, m),
            ptM_upbytes(L, kl - 1, (pt_Diff)db);
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
        db = ptN_sumbytes(&rt[k], m, rtcc), ptM_upbytes(L, l - 1, (pt_Diff)db);
    }
}

/* leaf 层缝合落点：L 末 piece 与 rt 首 piece 若 literal 物理连续则合并（缝的
   左右残片），否则 cursor 从保留末 piece 末尾移到 rt 首 piece 起点。*/
static int ptD_mergeleaf(pt_Cursor *L, pt_Node *rt) {
    int      l = ptK_levels(L);
    pt_Node *p = ptK_parent(L, l);
    int      cc = ptN_cc(p), atcc = ptK_idx(L, p, l) == cc;
    if (!ptM_ishole(p, cc - 1) && !ptM_ishole(rt, 0)
        && (char *)p->children[cc - 1] + p->bytes[cc - 1]
                   == (char *)rt->children[0]) {
        rt->bytes[0] += p->bytes[cc - 1];
        rt->children[0] = p->children[cc - 1];
        ptM_upbytes(L, l - 1, -(pt_Diff)p->bytes[cc - 1]);
        if (atcc) L->off -= p->bytes[cc - 1], L->poff = p->bytes[cc - 1];
        L->paths[l] = &p->children[cc - 1], ptN_setcc(p, cc - 1);
        return 0;
    }
    if (ptM_ishole(p, cc - 1) && ptM_ishole(rt, 0)) {
        pt_Hole *rh = ptN_hole(rt, 0);
        size_t   nl = p->bytes[cc - 1], nr = rt->bytes[0], can;
        if (nl + nr <= PT_MAX_HOLESIZE) {
            ptH_append(p, cc - 1, p->bytes[cc - 1], rh->data, nr);
            ptP_free(&L->tree->S->holes, rh);
            rt->bytes[0] += nl;
            rt->children[0] = p->children[cc - 1];
            ptM_upbytes(L, l - 1, -(pt_Diff)nl);
            if (atcc) L->off -= nl, L->poff = nl;
            L->paths[l] = &p->children[cc - 1], ptN_setcc(p, cc - 1);
            return 0;
        }
        can = nl < PT_MAX_HOLESIZE ? PT_MAX_HOLESIZE - nl : 0;
        if (can == 0) {
            L->off += L->poff, L->poff = 0;
            L->paths[l] = &p->children[cc];
            return 0;
        }
        ptH_append(p, cc - 1, p->bytes[cc - 1], rh->data, can);
        ptH_remove(rt, 0, 0, can);
        L->off += L->poff, L->poff = 0;
        L->paths[l] = &p->children[cc];
        return (int)can;
    }
    L->off += L->poff, L->poff = 0;
    L->paths[l] = &p->children[cc];
    return 0;
}

static void ptD_stitch(pt_Cursor *L, pt_Node *rt) {
    size_t   d = 0;
    int      i, l = ptK_levels(L);
    pt_Node *p = ptK_parent(L, l);
    ptD_checkstitch(L);
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
    L->poff -= d;
    ptM_upmask(L);
}

static void ptD_rmrange(pt_Cursor *L, pt_Cursor *R, int fl) {
    pt_Node rt[PT_MAX_LEVEL];
    int     k, l = ptK_levels(L);
    for (k = 0; k < PT_MAX_LEVEL; ++k)
        ptN_setcc(&rt[k], 0), rt[k].version = L->tree->root.version;
    ptD_trimright(L, l), ptD_trimleft(R, l);
    ptD_cutrange(L, R, rt, fl);
    ptD_stitch(L, rt);
}

static void ptD_cutpiece(
        pt_Cursor *C, pt_Node *p, int i, size_t lo, size_t hi) {
    pt_State *S = C->tree->S;
    size_t    len = p->bytes[i];
    if (ptM_ishole(p, i)) {
        ptH_remove(p, i, lo, hi - lo);
        if (p->bytes[i] == 0) {
            ptN_remove(S, p, 0, i, i + 1);
            if (ptK_idx(C, p, ptK_levels(C)) > i) C->paths[ptK_levels(C)] -= 1;
        }
        return;
    }
    if (lo == 0 && hi == len) {
        ptN_remove(S, p, 0, i, i + 1);
        if (ptK_idx(C, p, ptK_levels(C)) > i) C->paths[ptK_levels(C)] -= 1;
    } else if (lo == 0) {
        p->children[i] = (pt_Node *)((char *)p->children[i] + hi);
        p->bytes[i] = len - hi;
    } else if (hi == len) {
        p->bytes[i] = lo;
    } else {
        ptN_makespace(p, i + 1, 1);
        p->bytes[i] = lo;
        p->children[i + 1] = (pt_Node *)((char *)p->children[i] + hi);
        p->bytes[i + 1] = len - hi;
        ptM_sethole(p, i + 1, 0);
    }
}

static int ptD_rmleaf(pt_Cursor *L, pt_Cursor *R) {
    pt_Node *p = ptK_parent(L, ptK_levels(L));
    int      i = ptK_idx(L, p, ptK_levels(L)), oc = p->child_count;
    size_t   de = pt_offset(R) - pt_offset(L);
    assert(i == ptK_idx(R, p, ptK_levels(L)));
    if (!ptM_ishole(p, i) && L->poff > 0 && R->poff < p->bytes[i]) {
        ptI_makeroom(L, 1);
        p = ptK_parent(L, ptK_levels(L)), i = ptK_idx(L, p, ptK_levels(L));
    }
    ptD_cutpiece(L, p, i, L->poff, R->poff);
    ptM_upbytes(L, ptK_levels(L) - 1, -(pt_Diff)de);
    if (p->child_count == 0)
        L->paths[ptK_levels(L)] = &p->children[0], L->off = 0, L->poff = 0;
    if (p->child_count < oc && ptK_levels(L) > 0)
        ptD_rebalance(L, ptK_levels(L) - 1), ptM_upmask(L);
    return 0;
}

static int ptK_cowedit(pt_Cursor *C, pt_Cursor *R) {
    int fl, l, levels = ptK_levels(C);
    for (l = 0; l < levels && C->paths[l] == R->paths[l]; ++l) {
        ptK_cow(C, l, 0);
        R->paths[l + 1] = C->paths[l + 1];
    }
    fl = l;
    for (l = fl; l < levels; ++l) ptK_cow(C, l, 0);
    for (l = fl; l < levels; ++l) ptK_cow(R, l, 0);
    return fl;
}

PT_API int pt_remove(pt_Cursor *C, size_t len) {
    pt_Cursor R;
    pt_Tree  *old;
    size_t    dellen;
    int       r, l;
    if (!C || !C->tree) return PT_ERRPARAM;
    if (len == 0 || ptK_bytes(C) == 0) return PT_OK;
    if (C->off + C->poff + len > ptK_bytes(C))
        len = ptK_bytes(C) - (C->off + C->poff);
    R = *C, pt_advance(&R, (pt_Delta)len);
    dellen = pt_offset(&R) - pt_offset(C);
    if (dellen == 0) return PT_OK;
    l = ptK_levels(C);
    r = ptP_reserve(C->tree->S, &C->tree->S->nodes, 4 * l + 5);
    if (!r) return PT_ERRMEM;
    if (old = C->tree, r = ptK_markdirty(C), r != PT_OK) return r;
    R.tree = C->tree;
    R.paths[0] = C->tree->root.children + (R.paths[0] - old->root.children);
    if (C->paths[l] == R.paths[l])
        ptK_cowedit(C, &R), ptD_rmleaf(C, &R);
    else
        ptD_rmrange(C, &R, ptK_cowedit(C, &R));
    return PT_OK;
}

/* commit helpers — ptC_* hole→literal freeze */

static size_t ptC_leafbytes(const pt_Node *cont) {
    size_t s = 0;
    int    i;
    for (i = 0; i < cont->child_count; ++i)
        if (ptM_ishole(cont, i)) s += cont->bytes[i];
    return s;
}

static size_t ptC_holebytes(const pt_Tree *tree) {
    pt_Node *stk[PT_MAX_LEVEL], *p, *cont;
    int      ix[PT_MAX_LEVEL];
    int      k, l;
    size_t   total;
    if (tree->levels == 0) return ptC_leafbytes(&tree->root);
    stk[0] = (pt_Node *)&tree->root;
    ix[0] = 0;
    l = 0;
    total = 0;
    while (l >= 0) {
        p = stk[l];
        if (ix[l] >= p->child_count) {
            --l;
            continue;
        }
        k = ix[l]++;
        if (!ptM_ishole(p, k)) continue;
        if (l == tree->levels - 1) {
            cont = p->children[k];
            total += ptC_leafbytes(cont);
        } else {
            stk[++l] = p->children[k];
            ix[l] = 0;
        }
    }
    return total;
}

/* E7: copy hole data into scratch, switching pages when remain < n */
static char *ptC_freshscratch(pt_State *S, const char *s, size_t n) {
    char *p;
    if (S->scratch.remain < n) {
        void *page = S->scratch.reserve;
        assert(page != NULL);
        S->scratch.reserve = *(void **)page;
        *(void **)page = S->scratch.pages;
        S->scratch.pages = page;
        S->scratch.buffer = (char *)page + sizeof(void *);
        S->scratch.remain = PT_PAGE_SIZE - sizeof(void *);
    }
    p = S->scratch.buffer;
    S->scratch.buffer += n;
    S->scratch.remain -= n;
    memcpy(p, s, n);
    return p;
}

/* E11: 1:1 pointer swap, no literal merge, no child_count/bytes change */
static void ptC_freezeleaf(pt_State *S, pt_Node *cont) {
    int i;
    for (i = 0; i < cont->child_count; ++i) {
        if (ptM_ishole(cont, i)) {
            pt_Hole *h = (pt_Hole *)cont->children[i];
            char    *lit = ptC_freshscratch(S, h->data, cont->bytes[i]);
            cont->children[i] = (pt_Node *)lit;
            ptP_free(&S->holes, h);
        }
    }
    memset(cont->mask, 0, sizeof(cont->mask));
}

/* non-recursive mask-guided walk (仿 advance 上升下降). */
static void ptC_freeze(pt_State *S, pt_Tree *tree) {
    pt_Node *stk[PT_MAX_LEVEL], *p, *cont;
    int      ix[PT_MAX_LEVEL];
    int      k, l;
    if (tree->levels == 0) {
        ptC_freezeleaf(S, &tree->root);
        return;
    }
    stk[0] = &tree->root;
    ix[0] = 0;
    l = 0;
    while (l >= 0) {
        p = stk[l];
        if (ix[l] >= p->child_count) {
            --l;
            continue;
        }
        k = ix[l]++;
        if (!ptM_ishole(p, k)) continue;
        if (l == tree->levels - 1) {
            cont = p->children[k];
            ptC_freezeleaf(S, cont);
            ptM_sethole(p, k, 0);
        } else {
            ptM_sethole(p, k, 0);
            stk[++l] = p->children[k];
            ix[l] = 0;
        }
    }
}

PT_API pt_Blob pt_commit(pt_Cursor *c) {
    pt_State *S;
    size_t    total;
    if (c == NULL || c->tree == NULL) return NULL;
    if (!c->dirty) {
        pt_retain(c->tree);
        return c->tree;
    }
    S = c->tree->S;
    total = ptC_holebytes(c->tree);                 /* 相0 测量 */
    if (total > 0 && !ptP_reservescratch(S, total)) /* 相1 全或无闸门 */
        return NULL;
    ptC_freeze(S, c->tree); /* 相2 冻结（不可失败） */
    c->dirty = 0;
    return c->tree;
}

PT_API void pt_rollback(pt_Cursor *c) {
    pt_Tree *from;
    if (c == NULL || c->tree == NULL || !c->dirty) return;
    if ((from = c->tree->from) != NULL && from->refc == 1)
        from = NULL; /* source dies with transient -> invalidate cursor */
    pt_release(c->tree);
    c->tree = from, c->dirty = 0; /* return to source, or NULL if it died */
}

PT_API int pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len) {
    int r, lvls;
    if (!C || !C->tree) return PT_ERRPARAM;
    if (del == 0 && (s == NULL || len == 0)) return PT_OK;
    lvls = ptK_levels(C);
    if (!ptP_reserve(C->tree->S, &C->tree->S->nodes, 6 * lvls + 8))
        return PT_ERRMEM;
    r = pt_remove(C, del);
    if (r != PT_OK) return r;
    if (s != NULL && len > 0) r = pt_append(C, s, len);
    return r;
}

PT_NS_END

#endif /* PT_IMPLEMENTATION */
