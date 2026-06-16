#ifndef linecache_h
#define linecache_h

#ifndef LC_NS_BEGIN
# ifdef __cplusplus
#   define LC_NS_BEGIN extern "C" {
#   define LC_NS_END   }
# else
#   define LC_NS_BEGIN
#   define LC_NS_END
# endif
#endif /* LC_NS_BEGIN */

#ifndef LC_STATIC
# if __GNUC__
#   define LC_STATIC static __attribute((unused))
# else
#   define LC_STATIC static
# endif
#endif /* LC_STATIC */

#ifdef LC_STATIC_API
# ifndef LC_IMPLEMENTATION
#   define LC_IMPLEMENTATION
# endif
# define LC_API LC_STATIC
#endif /* LC_STATIC_API */

#if !defined(LC_API) && defined(_WIN32)
# ifdef LC_IMPLEMENTATION
#   define LC_API __declspec(dllexport)
# else
#   define LC_API __declspec(dllimport)
# endif
#endif /* LC_API */

#ifndef LC_API
# define LC_API extern
#endif

#include <stddef.h>

#define LC_OK       (0)
#define LC_ERRPARAM (-1)
#define LC_ERRMEM   (-2)
#define LC_ERREMPTY (-3)

LC_NS_BEGIN

typedef struct lc_State lc_State;
typedef struct lc_Cache lc_Cache;

typedef void    *lc_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
typedef unsigned lc_Scanner(void *ud, size_t pos);

/* state lifecycle */
LC_API lc_State *lc_open(lc_Alloc *allocf, void *ud);
LC_API void      lc_close(lc_State *S);
LC_API void      lc_reset(lc_State *S);

/* tree lifecycle */
LC_API lc_Cache *lc_newtree(lc_State *S);
LC_API void      lc_deltree(lc_State *S, lc_Cache *c);

LC_API int lc_scan(lc_Cache *c, lc_Scanner *sc, void *ud);

/* simple queries */
LC_API size_t lc_breaks(const lc_Cache *c);
LC_API size_t lc_bytes(const lc_Cache *c);

/* cursor */

#ifndef LC_MAX_LEVEL
# define LC_MAX_LEVEL 16
#endif

struct lc_Cursor {
    /*
     * Each entry points to a slot inside a parent's children[] array.
     * paths[0] == &c->root.children[...], paths[levels] is the leaf slot.
     * Leaf slots store lc_Leaf* and must be cast when dereferenced.
     */
    struct lc_Node **paths[LC_MAX_LEVEL];
    lc_Cache        *tree; /* tree this cursor operates on */
    size_t           off;  /* byte offset of current leaf */
    size_t           idx;  /* line index of current leaf */
    size_t           loff; /* byte offset in current leaf */
    unsigned         col;  /* column in current line */
    unsigned short   lidx; /* line index in current leaf */
};

typedef struct lc_Cursor lc_Cursor;
typedef ptrdiff_t        lc_Diff;

/* initialize */
LC_API int lc_seek(lc_Cursor *C, lc_Cache *c, size_t offset);
LC_API int lc_seekline(lc_Cursor *C, lc_Cache *c, size_t line);

/* move */
LC_API int lc_advance(lc_Cursor *C, lc_Diff delta);
LC_API int lc_advline(lc_Cursor *C, lc_Diff delta);

/* queries */
LC_API size_t   lc_offset(const lc_Cursor *C);
LC_API size_t   lc_line(const lc_Cursor *C);
LC_API unsigned lc_linelen(const lc_Cursor *C);
LC_API unsigned lc_col(const lc_Cursor *C);

/* mutation breaks */
LC_API int lc_markbreak(lc_Cursor *C, unsigned br);
LC_API int lc_clearbreaks(lc_Cursor *C, size_t len);

/* insert texts */
LC_API int lc_insert(lc_Cursor *C, unsigned e, lc_Scanner *sc, void *ud);

/* mutation texts */
LC_API void lc_splice(lc_Cursor *C, size_t del, size_t ins);

LC_NS_END

#endif /* linecache_h */

/* ======================================================================== */
/*                           IMPLEMENTATION                                 */
/* ======================================================================== */

#if defined(LC_IMPLEMENTATION) && !defined(lc_implemented)
#define lc_implemented

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LC_FANOUT
# define LC_FANOUT 62
#endif

#ifndef LC_LEAF_FANOUT
# define LC_LEAF_FANOUT 62
#endif

#ifndef LC_PAGE_SIZE
# define LC_PAGE_SIZE 65536
#endif

LC_NS_BEGIN

/* clang-format off */
#define lc_return(expr) do { expr; return; } while (0)
#define lc_min(a,b)     ((a) < (b) ? (a) : (b))
/* clang-format on */

/* internal types */

typedef struct lc_Node lc_Node;

typedef struct lc_Leaf {
    unsigned bytes[LC_LEAF_FANOUT]; /* line length of current line */
} lc_Leaf;

struct lc_Node {
    size_t   bytes[LC_FANOUT];    /* cumulative bytes per child subtree */
    size_t   breaks[LC_FANOUT];   /* cumulative breaks per child subtree */
    lc_Node *children[LC_FANOUT]; /* Leaf* at leaf level, Node* otherwise */
    unsigned short child_count;   /* number of valid children (≤ LC_FANOUT) */
};

struct lc_Cache {
    lc_State      *S;      /* owning state */
    lc_Node        root;   /* root node of the B+ tree */
    size_t         breaks; /* total line breaks in the tree */
    size_t         bytes;  /* total bytes in the tree */
    unsigned short levels; /* tree depth: 0 = root->children is leaf */
};

typedef struct lc_Pool {
    size_t obj_size; /* size of each object in this pool */
    void  *freed;    /* freelist head */
    void  *pages;    /* linked list of allocated pages */
#ifdef LC_POOL_STATS
    size_t live_obj;
#endif
} lc_Pool;

struct lc_State {
    void     *alloc_ud; /* user data for allocf */
    lc_Alloc *allocf;   /* memory allocator */
    lc_Pool   nodes;    /* pool for lc_Node objects */
    lc_Pool   leaves;   /* pool for lc_Leaf objects */
};

/* pool allocator */

#ifdef LC_POOL_STATS
# define lcP_stat(stmt) stmt
#else
# define lcP_stat(stmt) ((void)0)
#endif

static void lcP_init(lc_Pool *pool, size_t obj_size) {
    memset(pool, 0, sizeof(lc_Pool)), pool->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < LC_PAGE_SIZE / 4);
}

static void lcP_destroy(lc_State *S, lc_Pool *pool) {
    void *next, *page = pool->pages;
    for (; page; page = next) {
        next = *(void **)((char *)page + LC_PAGE_SIZE - sizeof(void *));
        S->allocf(S->alloc_ud, page, LC_PAGE_SIZE, 0);
    }
    lcP_stat(pool->live_obj = 0), lcP_init(pool, pool->obj_size);
}

static void lcP_free(lc_Pool *pool, void *obj) {
    *(void **)obj = pool->freed, pool->freed = obj;
    lcP_stat(pool->live_obj -= 1);
}

static void *lcP_alloc(lc_State *S, lc_Pool *pool) {
    size_t sz = pool->obj_size;
    char  *p, *end, *obj = pool->freed;
    lcP_stat(pool->live_obj += 1);
    if (obj) return (pool->freed = *(void **)obj), (void *)obj;
    if (S == NULL) return NULL;
    p = (char *)S->allocf(S->alloc_ud, NULL, 0, LC_PAGE_SIZE);
    if (p == NULL) return lcP_stat(pool->live_obj -= 1), NULL;
    end = &p[LC_PAGE_SIZE - sizeof(void *)], *(void **)end = pool->pages;
    pool->pages = (void *)(obj = p), p += sz, end -= sz;
    while ((p += sz) < end) *(void **)(p - sz) = p;
    *(void **)(p - sz) = NULL;
    return (pool->freed = (void *)(obj + sz)), (void *)obj;
}

LC_STATIC int lcP_reserve(lc_State *S, lc_Pool *pool, size_t n) {
    void  *freed = pool->freed, **t = &freed;
    size_t c;
    for (c = 0; c < n && *t; ++c) t = (void **)*t;
    if (c >= n) return 1;
    for (pool->freed = NULL; c < n; ++c) {
        void *obj = lcP_alloc(S, pool);
        if (obj == NULL) return 0;
        lcP_stat(pool->live_obj -= 1);
        *t = obj, t = (void **)obj;
    }
    return *t = NULL, (pool->freed = freed), 1;
}

/* lifecycle */

/* clang-format off */
LC_API void lc_reset(lc_State *S)
{ if (S) lcP_destroy(S, &S->nodes), lcP_destroy(S, &S->leaves); }

LC_API void lc_close(lc_State *S)
{ if (S) lc_reset(S), S->allocf(S->alloc_ud, S, sizeof(lc_State), 0); }
/* clang-format on */

LC_API size_t lc_breaks(const lc_Cache *c) { return c->breaks; }
LC_API size_t lc_bytes(const lc_Cache *c) { return c->bytes; }

LC_API lc_State *lc_open(lc_Alloc *allocf, void *ud) {
    lc_State *S = (lc_State *)allocf(ud, NULL, 0, sizeof(lc_State));
    if (S == NULL) return NULL;
    S->alloc_ud = ud, S->allocf = allocf;
    lcP_init(&S->nodes, sizeof(lc_Node));
    lcP_init(&S->leaves, sizeof(lc_Leaf));
    return S;
}

LC_API lc_Cache *lc_newtree(lc_State *S) {
    lc_Cache *c = (lc_Cache *)S->allocf(S->alloc_ud, NULL, 0, sizeof(lc_Cache));
    if (c == NULL) return NULL;
    memset(c, 0, sizeof(lc_Cache));
    return (c->S = S), c;
}

static void lcN_freechildren(lc_State *S, lc_Node *p, int rl, int s, int e) {
    int i;
    if (rl == 0) {
        for (i = s; i < e; ++i) lcP_free(&S->leaves, p->children[i]);
        return;
    }
    for (i = s; i < e; ++i) {
        lc_Node *child = p->children[i];
        lcN_freechildren(S, child, rl - 1, 0, child->child_count);
        lcP_free(&S->nodes, child);
    }
}

LC_API void lc_deltree(lc_State *S, lc_Cache *c) {
    lcN_freechildren(S, &c->root, c->levels, 0, c->root.child_count);
    S->allocf(S->alloc_ud, c, sizeof(lc_Cache), 0);
}

/* utils */

#define lcK_levels(C) ((int)(C)->tree->levels)
#define lcK_breaks(C) ((C)->tree->breaks)
#define lcK_bytes(C)  ((C)->tree->bytes)

#define lcK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define lcK_idx(C, p, l) ((int)((C)->paths[(l)] - (p)->children))
#define lcK_leaf(C)      (*(lc_Leaf **)(C)->paths[lcK_levels(C)])

#define lcN_tx(p, d, s, x) (p) = &(d)->children[((p) - (s)->children) + (x)]

#define lcL_new(S) ((lc_Leaf *)lcP_alloc((S), &(S)->leaves))
#define lcN_new(S) ((lc_Node *)lcP_alloc((S), &(S)->nodes))

/* clang-format off */
static size_t lcL_sumbytes(const lc_Leaf *leaf, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += leaf->bytes[i]; return s; }

static size_t lcN_sumbytes(const lc_Node *n, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += n->bytes[i]; return s; }

static size_t lcN_sumbreaks(const lc_Node *n, int i, int end)
{ size_t s = 0; for (; i < end; ++i) s += n->breaks[i]; return s; }

static size_t lcK_locoff(const lc_Cursor *C)
{ return lcL_sumbytes(lcK_leaf(C), 0, C->lidx) + C->col; }
/* clang-format on */

static void lcN_makespace(lc_Node *d, unsigned i, unsigned n) {
    unsigned moved = d->child_count - i;
    assert(d->child_count + n <= LC_FANOUT && i <= d->child_count);
    memmove(&d->children[i + n], &d->children[i], moved * sizeof(lc_Node *));
    memmove(&d->bytes[i + n], &d->bytes[i], moved * sizeof(size_t));
    memmove(&d->breaks[i + n], &d->breaks[i], moved * sizeof(size_t));
    d->child_count += n;
}

static void lcN_copy(lc_Node *d, int di, const lc_Node *s, int si, unsigned n) {
    assert(di + n <= LC_FANOUT && si + n <= LC_FANOUT);
    memcpy(&d->children[di], &s->children[si], n * sizeof(lc_Node *));
    memcpy(&d->bytes[di], &s->bytes[si], n * sizeof(size_t));
    memcpy(&d->breaks[di], &s->breaks[si], n * sizeof(size_t));
}

static void lcN_move(lc_Node *d, int di, int si, unsigned n) {
    assert(di + n <= LC_FANOUT && si + n <= LC_FANOUT);
    memmove(&d->children[di], &d->children[si], n * sizeof(lc_Node *));
    memmove(&d->bytes[di], &d->bytes[si], n * sizeof(size_t));
    memmove(&d->breaks[di], &d->breaks[si], n * sizeof(size_t));
}

/* cursor helpers */

static void lcK_findleaf(lc_Cursor *C, int l, size_t *poff) {
    assert(C->off + *poff <= lcK_bytes(C));
    for (; l <= lcK_levels(C); ++l) {
        lc_Node *p = lcK_parent(C, l);
        unsigned i;
        for (i = 0; i < p->child_count; ++i) {
            if (*poff < p->bytes[i]) break;
            C->off += p->bytes[i], C->idx += p->breaks[i];
            *poff -= p->bytes[i];
        }
        C->paths[l] = &p->children[assert(i < p->child_count), i];
    }
}

static void lcK_findline(lc_Cursor *C, int l, size_t *pline) {
    assert(C->idx + *pline <= lcK_breaks(C));
    for (; l <= lcK_levels(C); ++l) {
        lc_Node *p = lcK_parent(C, l);
        unsigned i;
        for (i = 0; i < p->child_count; ++i) {
            if (*pline < p->breaks[i] + (i == p->child_count - 1)) break;
            C->off += p->bytes[i], C->idx += p->breaks[i];
            *pline -= p->breaks[i];
        }
        C->paths[l] = &p->children[assert(i < p->child_count), i];
    }
}

static void lcK_findinleaf(lc_Cursor *C, size_t off) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      li = lcK_idx(C, p, lcK_levels(C));
    size_t   i = C->lidx, count = p->breaks[li];
    lc_Leaf *leaf = (lc_Leaf *)p->children[li];
    for (off += C->col; i < count; ++i) {
        if (off < leaf->bytes[i]) break;
        off -= leaf->bytes[i], C->loff += leaf->bytes[i], C->idx += 1;
    }
    assert(i < count), C->lidx = (unsigned short)i, C->col = off;
}

static void lcK_locend(lc_Cursor *C) {
    lc_Node *n = &C->tree->root;
    int      l, cc;
    for (l = 0; l < lcK_levels(C); ++l)
        n = *(C->paths[l] = &n->children[n->child_count - 1]);
    if (n->child_count == 0)
        C->paths[0] = n->children, C->lidx = C->col = 0;
    else {
        cc = n->child_count, C->paths[l] = &n->children[cc - 1];
        C->lidx = n->breaks[cc - 1], C->loff = n->bytes[cc - 1];
    }
    C->idx = lcK_breaks(C), C->off = lcK_bytes(C) - C->loff;
}

static void lcK_forwardoff(lc_Cursor *C, size_t d) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      l, i = lcK_idx(C, p, lcK_levels(C));
    size_t   lc = lcK_locoff(C), in = p->bytes[i] - lc;
    if (d < in) lc_return(lcK_findinleaf(C, d));
    d -= in, C->off += p->bytes[i], C->idx += p->breaks[i] - C->lidx;
    for (l = lcK_levels(C); l >= 0; --l) {
        p = lcK_parent(C, l), i = lcK_idx(C, p, l) + 1;
        for (; i < (int)p->child_count; ++i) {
            if (d < p->bytes[i]) break;
            C->off += p->bytes[i], C->idx += p->breaks[i];
            d -= p->bytes[i];
        }
        if (i < (int)p->child_count) break;
    }
    assert(l >= 0 && i < (int)p->child_count);
    C->paths[l] = &p->children[i], C->loff = C->lidx = C->col = 0;
    lcK_findleaf(C, l + 1, &d), lcK_findinleaf(C, d);
}

static void lcK_backwardoff(lc_Cursor *C, size_t d) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      l, i = lcK_idx(C, p, lcK_levels(C));
    size_t   lc = lcK_locoff(C);
    if (d <= lc) {
        C->loff = C->lidx = C->col = 0, lcK_findinleaf(C, lc - d);
        return;
    }
    d -= lc, C->idx -= C->lidx, C->loff = 0, C->col = 0;
    for (l = lcK_levels(C); l >= 0; --l) {
        p = lcK_parent(C, l), i = lcK_idx(C, p, l);
        if (l == lcK_levels(C)) --i;
        for (; i >= 0; --i) {
            if (d <= p->bytes[i]) break;
            C->off -= p->bytes[i], C->idx -= p->breaks[i];
            d -= p->bytes[i];
        }
        if (i >= 0) break;
    }
    assert(l >= 0 && i >= 0);
    C->paths[l] = &p->children[i], C->loff = C->lidx = C->col = 0;
    C->off -= p->bytes[i], C->idx -= p->breaks[i], d = p->bytes[i] - d;
    lcK_findleaf(C, l + 1, &d), lcK_findinleaf(C, d);
}

static void lcK_forwardline(lc_Cursor *C, size_t d) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      l, i = lcK_idx(C, p, lcK_levels(C));
    size_t   in = p->breaks[i] - C->lidx;
    if (d >= in) {
        C->loff += lcL_sumbytes(lcK_leaf(C), C->lidx, C->lidx + in) - C->col;
        d -= in, C->off += p->bytes[i], C->idx += in, C->col = 0;
        for (l = lcK_levels(C); l >= 0; --l) {
            p = lcK_parent(C, l), i = lcK_idx(C, p, l) + 1;
            for (; i < (int)p->child_count; ++i) {
                if (d < p->breaks[i] + (i == p->child_count - 1)) break;
                C->off += p->bytes[i], C->idx += p->breaks[i];
                d -= p->breaks[i];
            }
            if (i < (int)p->child_count) break;
        }
        assert(l >= 0), C->paths[l] = &p->children[i], C->loff = C->lidx = 0;
        lcK_findline(C, l + 1, &d);
    }
    C->loff += lcL_sumbytes(lcK_leaf(C), C->lidx, C->lidx + d) - C->col;
    C->lidx += d, C->idx += d, C->col = 0;
}

static void lcK_backwardline(lc_Cursor *C, size_t d) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      l, i = lcK_idx(C, p, lcK_levels(C));
    int      in = (C->lidx >= d);
    d = in ? C->lidx - d : d - C->lidx;
    C->loff = 0, C->col = 0, C->idx -= C->lidx;
    if (!in) {
        for (l = lcK_levels(C); l >= 0; --l) {
            p = lcK_parent(C, l), i = lcK_idx(C, p, l);
            if (l == lcK_levels(C)) i -= 1;
            for (; i >= 0; --i) {
                if (d <= p->breaks[i]) break;
                C->off -= p->bytes[i], C->idx -= p->breaks[i];
                d -= p->breaks[i];
            }
            if (i >= 0) break;
        }
        assert(l >= 0 && i >= 0), C->paths[l] = &p->children[i], C->lidx = 0;
        C->off -= p->bytes[i], C->idx -= p->breaks[i];
        d = p->breaks[i] - d, lcK_findline(C, l + 1, &d);
    }
    C->loff += lcL_sumbytes(lcK_leaf(C), 0, d);
    C->idx += d, C->col = 0, C->lidx = (unsigned short)d;
}

/* cursor API */

LC_API int lc_seek(lc_Cursor *C, lc_Cache *c, size_t n) {
    if (C == NULL || c == NULL) return LC_ERRPARAM;
    memset(C, 0, sizeof(lc_Cursor)), C->tree = c;
    if (n >= c->bytes)
        return lcK_locend(C), (C->col = (unsigned)(n - c->bytes)), LC_OK;
    return lcK_findleaf(C, 0, &n), lcK_findinleaf(C, n), LC_OK;
}

LC_API int lc_seekline(lc_Cursor *C, lc_Cache *c, size_t n) {
    if (C == NULL || c == NULL) return LC_ERRPARAM;
    if (n > c->breaks) return LC_ERRPARAM;
    memset(C, 0, sizeof(lc_Cursor)), C->tree = c;
    if (c->root.child_count == 0) return C->paths[0] = c->root.children, LC_OK;
    lcK_findline(C, 0, &n);
    C->loff += lcL_sumbytes(lcK_leaf(C), C->lidx, C->lidx + n) - C->col;
    C->lidx += n, C->idx += n, C->col = 0;
    return LC_OK;
}

LC_API int lc_advance(lc_Cursor *C, lc_Diff delta) {
    lc_Diff n;
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    if (delta == 0) return LC_OK;
    n = (lc_Diff)lc_offset(C) + delta;
    if (n < 0) return lcK_backwardoff(C, lc_offset(C)), LC_OK;
    if (delta < 0) return lcK_backwardoff(C, (size_t)(-delta)), LC_OK;
    if ((size_t)n >= lcK_bytes(C))
        return lcK_locend(C), C->col = (unsigned)(n - lcK_bytes(C)), LC_OK;
    return lcK_forwardoff(C, (size_t)delta), LC_OK;
}

LC_API int lc_advline(lc_Cursor *C, lc_Diff delta) {
    lc_Diff n, line;
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    if (lcK_bytes(C) == 0) return LC_OK;
    line = (lc_Diff)C->idx, n = line + delta;
    if (n == line) return LC_OK;
    if (n <= 0) return lcK_backwardline(C, (size_t)line), LC_OK;
    if ((size_t)n >= lcK_breaks(C))
        return lcK_forwardline(C, lcK_breaks(C) - (size_t)line), LC_OK;
    if (delta < 0) return lcK_backwardline(C, (size_t)(-delta)), LC_OK;
    return lcK_forwardline(C, delta), LC_OK;
}

/* clang-format off */
LC_API size_t lc_offset(const lc_Cursor *C)
{ return C ? C->off + C->loff + C->col : 0; }
/* clang-format on */

LC_API size_t   lc_line(const lc_Cursor *C) { return C ? C->idx : 0; }
LC_API unsigned lc_col(const lc_Cursor *C) { return C ? C->col : 0; }

LC_API unsigned lc_linelen(const lc_Cursor *C) {
    lc_Node *p;
    unsigned leaf_i;
    if (!C) return 0;
    p = lcK_parent(C, lcK_levels(C));
    leaf_i = (unsigned)lcK_idx(C, p, lcK_levels(C));
    if (C->lidx == p->breaks[leaf_i]) return C->col;
    return lcK_leaf(C)->bytes[C->lidx];
}

/* bulk deletion */

static void lcM_up(lc_Cursor *C, int l, lc_Diff db, lc_Diff dl) {
    if (db == 0 && dl == 0) return;
    for (; l >= 0; --l) {
        lc_Node *p = lcK_parent(C, l);
        int      i = lcK_idx(C, p, l);
        p->bytes[i] += db, p->breaks[i] += dl;
    }
    C->tree->bytes += db, C->tree->breaks += dl;
}

static void lcM_tx(lc_Cursor *L, lc_Cursor *R, int l, lc_Diff db, lc_Diff dl) {
    if (lcK_parent(L, l) != lcK_parent(R, l))
        lcM_up(L, l, -db, -dl), lcM_up(R, l, db, dl);
    else {
        lc_Node *pl = lcK_parent(L, l), *pr = lcK_parent(R, l);
        int      il = lcK_idx(L, pl, l), ir = lcK_idx(R, pr, l);
        pl->bytes[il] -= db, pl->breaks[il] -= dl;
        pr->bytes[ir] += db, pr->breaks[ir] += dl;
    }
}

static void lcD_prune(lc_Cursor *L, int sr, int l) {
    lc_Node *p = lcK_parent(L, l);
    int      i = lcK_idx(L, p, l);
    lc_Diff  db, dl;
    if (i + 1 >= sr) return;
    db = lcN_sumbytes(p, i + 1, sr), dl = lcN_sumbreaks(p, i + 1, sr);
    lcN_freechildren(L->tree->S, p, lcK_levels(L) - l, i + 1, sr);
    lcN_move(p, i + 1, sr, p->child_count - sr);
    p->child_count -= (sr - i) - 1;
    lcM_up(L, l - 1, -db, -dl);
}

static void lcD_trimleaf(lc_Cursor *C, int left) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      li = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf *leaf = (lc_Leaf *)p->children[li];
    int      count = (int)p->breaks[li];
    lc_Diff  db = 0, dl = 0;
    if ((int)C->lidx >= count) {
        if (!left) return;
        db = p->bytes[li], dl = count;
    } else if (!left) /* trimright */
        db = lcL_sumbytes(leaf, C->lidx, count), dl = count - C->lidx;
    else {
        db += lcL_sumbytes(leaf, 0, C->lidx), dl = C->lidx;
        if (C->lidx < count) db += C->col, leaf->bytes[(int)C->lidx] -= C->col;
    }
    lcM_up(C, lcK_levels(C), -(lc_Diff)db, -(lc_Diff)dl);
}

static void lcD_trimnode(lc_Cursor *C, int l, int left) {
    lc_Node *p = lcK_parent(C, l);
    int      i = lcK_idx(C, p, l), s = 0, e = i;
    lc_Diff  db, dl;
    if (left ? i <= 0 : i >= p->child_count - 1) return;
    if (!left) s = i + 1, e = p->child_count;
    p->child_count = (unsigned short)(left ? (p->child_count - i) : (i + 1));
    db = lcN_sumbytes(p, s, e), dl = lcN_sumbreaks(p, s, e);
    lcN_freechildren(C->tree->S, p, lcK_levels(C) - l, s, e);
    lcM_up(C, l - 1, -db, -dl);
}

static lc_Diff lcD_balanceleaf(lc_Leaf **ls, int l, int r, int s) {
    unsigned *bl = ls[0]->bytes, *br = ls[1]->bytes;
    int       d = l - ((l + r + 1) >> 1);
    if (d == 0) return 0;
    if (d < 0) {
        memcpy(&bl[l], &br[s], -d * sizeof(unsigned));
        memmove(br, &br[s - d], (r + d) * sizeof(unsigned));
        return -(lc_Diff)lcL_sumbytes(ls[0], l, l - d);
    } else {
        memmove(&br[d], &br[s], r * sizeof(unsigned));
        memcpy(br, &bl[l - d], d * sizeof(unsigned));
        return (lc_Diff)lcL_sumbytes(ls[1], 0, d);
    }
}

static int lcD_shiftleaf(lc_Cursor *L, lc_Cursor *R) {
    int      l = lcK_levels(L);
    lc_Leaf *ll = lcK_leaf(L), *lr = lcK_leaf(R);
    lc_Node *pl = lcK_parent(L, l), *pr = lcK_parent(R, l);
    int      il = lcK_idx(L, pl, l), ir = lcK_idx(R, pr, l);
    int      cl = (int)pl->breaks[il], cr = (int)pr->breaks[ir];
    lc_Diff  db;
    if (cl + cr <= LC_LEAF_FANOUT) {
        memcpy(&ll->bytes[cl], &lr->bytes[R->lidx], cr * sizeof(unsigned));
        lcM_tx(L, R, l, -(lc_Diff)pr->bytes[ir], -cr);
        return (R->paths[l] += 1), 1;
    }
    if (!(db = lcD_balanceleaf((lc_Leaf **)&pl->children[il], cl, cr, R->lidx)))
        memmove(&lr->bytes, &lr->bytes[R->lidx], cr * sizeof(unsigned));
    return R->lidx = 0, lcM_tx(L, R, l, db, cl - ((cl + cr + 1) >> 1)), 0;
}

static int lcD_balancenode(lc_Node **ns, int s, int left, lc_Diff ds[2]) {
    int d, l = ns[0]->child_count, r = ns[1]->child_count;
    if ((d = l - ((l + r + (left != 0)) >> 1)) == 0) return ds[0] = ds[1] = 0;
    if (d < 0) {
        lcN_copy(ns[0], l, ns[1], s, -d), lcN_move(ns[1], 0, s - d, r + d);
        ds[0] = -(lc_Diff)lcN_sumbytes(ns[0], l, l - d);
        ds[1] = -(lc_Diff)lcN_sumbreaks(ns[0], l, l - d);
    } else {
        lcN_move(ns[1], d, s, r), lcN_copy(ns[1], 0, ns[0], l - d, d);
        ds[0] = (lc_Diff)lcN_sumbytes(ns[1], 0, d);
        ds[1] = (lc_Diff)lcN_sumbreaks(ns[1], 0, d);
    }
    return (ns[0]->child_count -= d, ns[1]->child_count += d), 1;
}

static int lcD_shiftnode(lc_Cursor *L, lc_Cursor *R, int l) {
    int      i, il, ir, cl, cr;
    lc_Node *p, *ns[2];
    lc_Diff  ds[2];
    ns[0] = lcK_parent(L, l + 1), ns[1] = lcK_parent(R, l + 1);
    il = lcK_idx(L, ns[0], l + 1), ir = lcK_idx(R, ns[1], l + 1);
    cl = ns[0]->child_count, cr = ns[1]->child_count;
    if (cl + cr <= LC_FANOUT) {
        p = lcK_parent(R, l), i = lcK_idx(R, p, l);
        lcN_copy(ns[0], il + 1, ns[1], ir, ns[1]->child_count);
        lcM_tx(L, R, l, -(lc_Diff)p->bytes[i], -(lc_Diff)p->breaks[i]);
        ns[0]->child_count += ns[1]->child_count, ns[1]->child_count = 0;
        return (R->paths[l] += 1), 1;
    }
    if (!lcD_balancenode(ns, ir, 1, ds))
        lcN_move(ns[1], 0, ir, ns[1]->child_count);
    return lcM_tx(L, R, l, ds[0], ds[1]), 0;
}

static int lcD_foldleaf(lc_Cursor *C) {
    lc_Node  *p = lcK_parent(C, lcK_levels(C));
    int       cl, cr, i = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf **ls = (lc_Leaf **)&p->children[i], *o = *ls;
    lc_Diff   db, dl;
    if (p->child_count <= 1) return 0;
    if (i == p->child_count - 1) ls -= 1, i -= 1;
    cl = (int)p->breaks[i], cr = (int)p->breaks[i + 1];
    if (cl + cr <= LC_LEAF_FANOUT) {
        memcpy(ls[0]->bytes + cl, ls[1]->bytes, cr * sizeof(unsigned));
        p->breaks[i] += cr, p->breaks[i + 1] = 0;
        if (*ls != o) C->off -= p->bytes[i], C->loff += p->bytes[i];
        p->bytes[i] += p->bytes[i + 1], p->bytes[i + 1] = 0;
        if (*ls != o) C->paths[lcK_levels(C)] = &p->children[i], C->lidx += cl;
        return lcD_prune(C, i + 2, lcK_levels(C)), 1;
    }
    dl = cl - ((cl + cr + 1) / 2);
    db = lcD_balanceleaf((lc_Leaf **)&p->children[i], cl, cr, 0);
    p->bytes[i] -= db, p->bytes[i + 1] += db;
    p->breaks[i] -= dl, p->breaks[i + 1] += dl;
    if (dl < 0 && *ls != o && (int)C->lidx < -dl)
        C->off -= db + p->bytes[i], C->lidx += cl,
                C->paths[lcK_levels(C)] = &p->children[i];
    else if (dl > 0 && *ls == o && (int)C->lidx >= cl - dl)
        C->off += p->bytes[i], C->lidx += dl - cl,
                C->paths[lcK_levels(C)] = &p->children[i + 1];
    else if (*ls != o)
        C->off -= db, C->lidx += dl;
    return 0;
}

static int lcD_foldnode(lc_Cursor *C, int l) {
    lc_Node  *p = lcK_parent(C, l);
    int       cl, cr, i = lcK_idx(C, p, l);
    lc_Node **ns = (lc_Node **)&p->children[i], *o = *ns;
    lc_Diff   ds[2], dn;
    assert(p->child_count > 1);
    if (i == p->child_count - 1) ns -= 1, i -= 1;
    cl = ns[0]->child_count, cr = ns[1]->child_count;
    if (cl + cr <= LC_FANOUT) {
        lcN_copy(ns[0], cl, ns[1], 0, cr);
        ns[0]->child_count += cr, ns[1]->child_count -= cr;
        p->bytes[i] += p->bytes[i + 1], p->bytes[i + 1] = 0;
        p->breaks[i] += p->breaks[i + 1], p->breaks[i + 1] = 0;
        if (*ns != o)
            lcN_tx(C->paths[l + 1], ns[0], ns[1], cl), C->paths[l] = ns;
        return lcD_prune(C, i + 2, l), 1;
    }
    if (!lcD_balancenode(ns, 0, (*ns == o), ds)) return 0;
    p->bytes[i] -= ds[0], p->bytes[i + 1] += ds[0];
    p->breaks[i] -= ds[1], p->breaks[i + 1] += ds[1];
    dn = cl - ((cl + cr + (*ns == o)) / 2);
    if (dn < 0 && *ns != o && C->paths[l + 1] - ns[1]->children < -dn)
        lcN_tx(C->paths[l + 1], ns[0], ns[1], cl), C->paths[l] = ns;
    else if (dn > 0 && *ns == o && C->paths[l + 1] - ns[0]->children >= cl - dn)
        lcN_tx(C->paths[l + 1], ns[1], ns[0], dn - cl), C->paths[l] = ns + 1;
    else if (*ns != o)
        C->paths[l + 1] += dn;
    return 0;
}

static void lcD_rebalance(lc_Cursor *C, int l) {
    assert(l == 0 || l < lcK_levels(C));
    for (; l > 0; --l) {
        lc_Node *p = lcK_parent(C, l);
        if (p->children[lcK_idx(C, p, l)]->child_count >= LC_FANOUT / 2) return;
        assert(p->child_count > 1);
        if (!lcD_foldnode(C, l)) return;
    }
    while (lcK_levels(C) && C->tree->root.child_count == 1) {
        lc_Node *only = lcK_parent(C, 1);
        int      i = lcK_idx(C, only, 1);
        C->tree->root = *only;
        lcP_free(&C->tree->S->nodes, only);
        C->tree->levels--;
        C->paths[0] += i;
        memmove(C->paths + 1, C->paths + 2, lcK_levels(C) * sizeof(lc_Node **));
    }
}

static void lcD_spliceleaf(lc_Cursor *C, size_t del) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      li = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf *l = (lc_Leaf *)p->children[li];
    int      end = C->lidx, count = (int)p->breaks[li];
    lc_Diff  removed;
    if (end == count) return;
    if (del < l->bytes[end] - C->col) {
        l->bytes[end] -= del, lcM_up(C, lcK_levels(C), -(lc_Diff)del, 0);
        return;
    }
    del += C->col, removed = C->col;
    for (; end < count; ++end) {
        unsigned bytes = l->bytes[end];
        if (del < bytes) break;
        del -= bytes, removed -= bytes;
    }
    memmove(&l->bytes[C->lidx], &l->bytes[end],
            (count - end) * sizeof(unsigned));
    if (end < count) l->bytes[C->lidx] += C->col - (lc_Diff)del;
    removed -= (lc_Diff)del + (end == count ? C->col : 0);
    lcM_up(C, lcK_levels(C), removed, -(lc_Diff)(end - C->lidx));
    if (p->breaks[li] < LC_LEAF_FANOUT / 2 && lcD_foldleaf(C))
        lcD_rebalance(C, lcK_levels(C) - 1);
}

static void lcD_splicerange(lc_Cursor *L, lc_Cursor *R) {
    int      l, dl, i;
    lc_Node *p;
    for (l = 0; l <= lcK_levels(L); ++l)
        if (L->paths[l] != R->paths[l]) break;
    lcD_trimleaf(L, 0), lcD_trimleaf(R, 1);
    p = lcK_parent(R, lcK_levels(R)), i = lcK_idx(R, p, lcK_levels(R));
    if (L->col && p->breaks[i]) {
        lcK_leaf(R)->bytes[(int)R->lidx] += L->col;
        lcM_up(R, lcK_levels(R), (lc_Diff)L->col, 0);
    }
    lcD_shiftleaf(L, R);
    for (dl = lcK_levels(L); dl > l; --dl)
        lcD_trimnode(L, dl, 0), lcD_trimnode(R, dl, 1),
                lcD_shiftnode(L, R, dl - 1);
    p = lcK_parent(R, l), lcD_prune(L, lcK_idx(R, p, l), l);
    if (l == 0) lcD_rebalance(L, 0);
    if (l > 0 && lcD_foldnode(L, l - 1)) lcD_rebalance(L, l - 1);
    for (dl = l; dl < lcK_levels(L); ++dl) lcD_foldnode(L, dl);
    lcD_foldleaf(L);
}

static void lcD_emptytree(lc_Cursor *C, size_t ins) {
    lcN_freechildren(
            C->tree->S, &C->tree->root, C->tree->levels, 0,
            C->tree->root.child_count);
    memset(&C->tree->root, 0, sizeof(lc_Node));
    C->tree->levels = C->tree->bytes = C->tree->breaks = 0, C->col += ins;
}

LC_API void lc_splice(lc_Cursor *C, size_t del, size_t ins) {
    lc_Node *p;
    int      li;
    if (C == NULL || C->tree == NULL || (del == 0 && ins == 0)) return;
    if (lcK_levels(C) == 0 && C->tree->root.child_count == 0) return;
    if (lc_offset(C) > C->tree->bytes) lc_return(C->col += (ins - del));
    del = lc_min(del, C->tree->bytes - lc_offset(C));
    if (lc_offset(C) == 0 && del >= C->tree->bytes)
        lc_return(lcD_emptytree(C, ins));
    if (del > 0) {
        lc_Cursor R = *C;
        lc_advance(&R, (lc_Diff)del);
        if (C->paths[lcK_levels(C)] != R.paths[lcK_levels(C)])
            lcD_splicerange(C, &R);
        else
            lcD_spliceleaf(C, del);
    }
    C->loff = lcL_sumbytes(lcK_leaf(C), 0, C->lidx);
    if (C->tree->bytes == 0 && C->tree->breaks == 0)
        lc_return(lcD_emptytree(C, ins));
    if (ins == 0) return;
    p = lcK_parent(C, lcK_levels(C)), li = (int)lcK_idx(C, p, lcK_levels(C));
    if (C->lidx < p->breaks[li]) {
        lcK_leaf(C)->bytes[C->lidx] += ins;
        lcM_up(C, lcK_levels(C), (lc_Diff)ins, 0);
    }
    C->col += (unsigned)ins;
}

LC_API int lc_clearbreaks(lc_Cursor *C, size_t len) {
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    return lc_splice(C, len, len), LC_OK;
}

/* insertion */

static int lcB_initempty(lc_Cursor *C, unsigned br) {
    lc_Cache *c = C->tree;
    lc_Leaf  *lf = lcL_new(c->S);
    if (!lf) return LC_ERRMEM;
    lf->bytes[0] = br;
    c->root.children[0] = (lc_Node *)lf;
    c->root.bytes[0] = br, c->root.breaks[0] = 1;
    c->root.child_count = 1, c->breaks = 1, c->bytes = br;
    C->off = 0, C->loff = br, C->idx = 1, C->lidx = 1, C->col = 0, C->tree = c;
    return (C->paths[0] = &c->root.children[0]), LC_OK;
}

static void lcB_splitroot(lc_Cursor *C, lc_Node *n, lc_Node *pp) {
    lc_Cache *c = C->tree;
    lc_Node   save = c->root;
    int       mid = c->root.child_count / 2;
    int       i = lcK_idx(C, &c->root, 0);
    int       nc = c->root.child_count - mid;
    lcN_copy(n, 0, &c->root, mid, nc), n->child_count = nc;
    *(c->root.children[0] = pp) = save;
    c->root.children[0]->child_count = mid;
    c->root.bytes[0] = lcN_sumbytes((lc_Node *)c->root.children[0], 0, mid);
    c->root.breaks[0] = lcN_sumbreaks((lc_Node *)c->root.children[0], 0, mid);
    c->root.bytes[1] = c->bytes - c->root.bytes[0];
    c->root.breaks[1] = c->breaks - c->root.breaks[0];
    c->root.children[1] = n, c->root.child_count = 2, c->levels++;
    memmove(C->paths + 1, C->paths, (c->levels) * sizeof(lc_Node **));
    C->paths[0] = &c->root.children[i >= mid];
    C->paths[1] = &(*C->paths[0])->children[i < mid ? i : i - mid];
}

static void lcB_splitchild(lc_Cursor *C, int l, lc_Node *n) {
    lc_Node *p = lcK_parent(C, l);
    int      i = lcK_idx(C, p, l);
    lc_Node *old = p->children[i];
    int      mid = old->child_count / 2;
    int      nc = old->child_count - mid;
    size_t   ob = p->bytes[i], ol = p->breaks[i];
    lcN_copy(n, 0, old, mid, nc), n->child_count = nc, old->child_count = mid;
    p->bytes[i] = lcN_sumbytes(old, 0, mid);
    p->breaks[i] = lcN_sumbreaks(old, 0, mid);
    lcN_makespace(p, i + 1, 1), p->children[i + 1] = n;
    p->bytes[i + 1] = ob - p->bytes[i], p->breaks[i + 1] = ol - p->breaks[i];
    if (C->paths[l + 1] >= old->children + mid) {
        unsigned d = C->paths[l + 1] - old->children - mid;
        C->paths[l + 1] = &n->children[d], C->paths[l] = &p->children[i + 1];
    }
}

static void lcB_splitleaf(lc_Cursor *C, lc_Leaf *nl) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      i = lcK_idx(C, p, lcK_levels(C));
    size_t   obytes, oldb, newb, n, mid = p->breaks[i] / 2;
    lc_Leaf *ol = (lc_Leaf *)p->children[i];
    n = p->breaks[i] - mid;
    memcpy(nl->bytes, ol->bytes + mid, n * sizeof(unsigned));
    oldb = lcL_sumbytes(ol, 0, mid), newb = lcL_sumbytes(nl, 0, n);
    obytes = p->bytes[i], p->bytes[i] = oldb;
    lcN_makespace(p, i + 1, 1);
    p->children[i + 1] = (lc_Node *)nl;
    p->bytes[i + 1] = newb + (obytes - oldb - newb);
    p->breaks[i] = mid, p->breaks[i + 1] = n;
    if (C->lidx >= mid) {
        C->lidx -= mid;
        C->paths[lcK_levels(C)] = &p->children[i + 1];
    }
}

static int lcB_makeroom(lc_Cursor *C) {
    lc_State *S = C->tree->S;
    int       l = lcK_levels(C), c = 0;
    lc_Leaf  *lf;
    for (; l >= 0 && lcK_parent(C, l)->child_count >= LC_FANOUT; --l)
        c += (l == 0) + 1;
    lcP_reserve(S, &S->nodes, c);
    if (!(lf = lcL_new(S))) return LC_ERRMEM;
    for (l = lcK_levels(C); l >= 0; --l)
        if (lcK_parent(C, l)->child_count < LC_FANOUT) break;
    if (l < 0) {
        lc_Node *n = (lc_Node *)lcP_alloc(NULL, &S->nodes);
        lc_Node *pp = (lc_Node *)lcP_alloc(NULL, &S->nodes);
        assert(n && pp), lcB_splitroot(C, n, pp), l = 1;
    }
    for (; l < lcK_levels(C); ++l) {
        lc_Node *n = (lc_Node *)lcP_alloc(NULL, &S->nodes);
        assert(n), lcB_splitchild(C, l, n);
    }
    return lcB_splitleaf(C, lf), LC_OK;
}

static void lcB_putbreak(lc_Cursor *C, unsigned br) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      i = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf *leaf = (lc_Leaf *)p->children[i];
    size_t   count = p->breaks[i];
    unsigned len = leaf->bytes[C->lidx], split = C->col + br;
    assert(C->lidx < count && split < len);
    memmove(&leaf->bytes[C->lidx + 2], &leaf->bytes[C->lidx + 1],
            (count - C->lidx - 1) * sizeof(unsigned));
    leaf->bytes[C->lidx] = split;
    leaf->bytes[C->lidx + 1] = len - split;
    lcM_up(C, lcK_levels(C), 0, 1);
}

LC_API int lc_markbreak(lc_Cursor *C, unsigned br) {
    lc_Node *p;
    int      r, i;
    size_t   remain, need = lc_offset(C) + br;
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    if (lcK_levels(C) == 0 && C->tree->root.child_count == 0)
        return lcB_initempty(C, br);
    if (br == (remain = lc_linelen(C) - C->col)) return LC_OK;
    if (br > remain) {
        lc_splice(C, br, br);
        if (C->tree->root.child_count == 0) return lcB_initempty(C, br);
        if (need > C->tree->bytes) {
            lc_Diff ext = (lc_Diff)(need - C->tree->bytes);
            lc_Diff oldlen = lcK_leaf(C)->bytes[C->lidx - 1];
            C->lidx -= 1, C->loff -= oldlen, C->col = oldlen;
            lcK_leaf(C)->bytes[C->lidx] += (unsigned)ext;
            lcM_up(C, lcK_levels(C), ext, 0);
        }
        lcB_putbreak(C, 0), C->loff += C->col + lcK_leaf(C)->bytes[C->lidx + 1];
        return (C->idx += 1, C->lidx += 2, C->col = 0), LC_OK;
    }
    p = lcK_parent(C, lcK_levels(C)), i = lcK_idx(C, p, lcK_levels(C));
    if (p->breaks[i] >= LC_LEAF_FANOUT && (r = lcB_makeroom(C)) < 0) return r;
    lcB_putbreak(C, br), C->loff += C->col + br;
    return (C->idx += 1, C->lidx += 1, C->col = 0), LC_OK;
}

/*  bulk insert & scan  */

static int lcB_append(lc_Cursor *C, lc_Scanner sc, void *ud) {
    size_t   pos = lc_offset(C);
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      i = lcK_idx(C, p, lcK_levels(C)), li = LC_LEAF_FANOUT;
    lc_Diff  cb, cl, db = 0, dl = 0;
    assert(i >= p->child_count - 1);
    for (; i < LC_FANOUT && li == LC_LEAF_FANOUT; ++i) {
        lc_Leaf *lf = (lc_Leaf *)p->children[i];
        unsigned br;
        if (li = (int)p->breaks[i], i >= p->child_count) {
            if (!(lf = lcL_new(C->tree->S))) return LC_ERRMEM;
            li = p->bytes[i] = p->breaks[i] = 0, p->children[i] = (lc_Node *)lf;
        }
        for (cb = cl = 0; li < LC_LEAF_FANOUT && (br = sc(ud, pos)) != 0; ++li)
            lf->bytes[li] = br, pos += br, cb += br, cl += 1;
        db += cb, dl += cl, p->bytes[i] += cb, p->breaks[i] += cl;
        if (i >= p->child_count && li == 0)
            i -= 1, lcP_free(&C->tree->S->leaves, lf);
        C->idx += cl, C->off += C->loff + cb, C->loff = 0, C->lidx = 0;
    }
    lcM_up(C, lcK_levels(C) - 1, (lc_Diff)db, (lc_Diff)dl);
    C->paths[lcK_levels(C)] = &p->children[i];
    return (p->child_count = i) == LC_FANOUT && li == LC_LEAF_FANOUT;
}

static int lcB_makechain(lc_Cursor *C, int from, int to) {
    lc_Node *p, *nn;
    int      l, r = 0;
    if (!lcP_reserve(C->tree->S, &C->tree->S->nodes, to - from + 1))
        return LC_ERRMEM;
    if (from < 0) {
        nn = (lc_Node *)lcP_alloc(NULL, &C->tree->S->nodes), assert(nn);
        p = &C->tree->root, *nn = *p;
        p->bytes[0] = C->tree->bytes, p->breaks[0] = C->tree->breaks;
        p->children[0] = nn, p->child_count = 1;
        C->tree->levels += 1, from = 0, to += 1, r = 1;
    }
    for (l = from; l < to; ++l) {
        nn = (lc_Node *)lcP_alloc(NULL, &C->tree->S->nodes), assert(nn);
        p = lcK_parent(C, l), nn->child_count = 0;
        p->bytes[p->child_count] = 0, p->breaks[p->child_count] = 0;
        p->children[p->child_count] = nn, p->child_count++;
        C->paths[l] = &p->children[p->child_count - 1];
    }
    if (from < to) C->paths[l] = &nn->children[0];
    return r;
}

LC_API int lc_scan(lc_Cache *c, lc_Scanner *sc, void *ud) {
    lc_Cursor C;
    int       l, r;
    if (c == NULL || sc == NULL) return LC_ERRPARAM;
    lc_seek(&C, c, c->bytes);
    while ((r = lcB_append(&C, sc, ud)) > 0) {
        for (l = lcK_levels(&C); l >= 0; --l)
            if (lcK_parent(&C, l)->child_count < LC_FANOUT) break;
        if ((r = lcB_makechain(&C, l, lcK_levels(&C))) < 0) break;
    }
    if (r < 0) return r;
    if (C.off) C.paths[lcK_levels(&C)] -= 1;
    for (l = 0; l < lcK_levels(&C); ++l) lcD_foldnode(&C, l);
    return lcD_foldleaf(&C), lcD_rebalance(&C, 0), LC_OK;
}

static int lcB_cutleaf(lc_Cursor *C, lc_Node *rt) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      cc, cr, i = lcK_idx(C, p, lcK_levels(C));
    size_t   db, dl;
    lc_Leaf *lr;
    if (!(p->child_count && (cr = (int)p->breaks[i] - C->lidx))) return LC_OK;
    if (!(lr = lcL_new(C->tree->S))) return LC_ERRMEM;
    memcpy(lr->bytes, lcK_leaf(C)->bytes + C->lidx, cr * sizeof(unsigned));
    rt[0].bytes[0] = lcL_sumbytes(lr, 0, cr), rt[0].breaks[0] = cr;
    rt[0].children[0] = (lc_Node *)lr, cc = (int)p->child_count - i;
    lcN_copy(&rt[0], 1, p, i + 1, cc - 1), rt[0].child_count = cc;
    p->breaks[i] = C->lidx, p->bytes[i] -= rt[0].bytes[0];
    db = lcN_sumbytes(&rt[0], 0, cc), dl = lcN_sumbreaks(&rt[0], 0, cc);
    lcM_up(C, lcK_levels(C) - 1, -(lc_Diff)db, -(lc_Diff)dl);
    rt[0].bytes[0] -= C->col, lr->bytes[0] -= C->col;
    C->lidx = p->breaks[i], C->loff = p->bytes[i];
    C->paths[lcK_levels(C)] = &p->children[i];
    return (p->child_count = i + 1), LC_OK;
}

static int lcB_fillrt(lc_Cursor *C, lc_Node *rt, int l) {
    int      i, fl, c;
    lc_Node *p;
    for (fl = l - 1; fl >= 0; --fl)
        if (p = lcK_parent(C, fl), (i = lcK_idx(C, p, fl)) < LC_FANOUT - 1)
            break;
    if (fl >= 0 && (c = (int)p->child_count - i - 1) > 0) {
        int     k = lcK_levels(C) - fl;
        lc_Diff db = lcN_sumbytes(p, i + 1, p->child_count);
        lc_Diff dl = lcN_sumbreaks(p, i + 1, p->child_count);
        assert(rt[k].child_count == 0), lcN_copy(&rt[k], 0, p, i + 1, c);
        p->child_count = i + 1, rt[k].child_count = c;
        lcM_up(C, fl - 1, -db, -dl);
    }
    return lcB_makechain(C, fl, l);
}

static void lcB_balancerange(lc_Cursor *C, int s, int e, int d) {
    lc_Node *p;
    int      i, l, cc;
    for (l = s; l < e; ++l) lcD_foldnode(C, l);
    p = lcK_parent(C, e), i = lcK_idx(C, p, e);
    if (d <= i) {
        C->paths[e] -= d;
    } else {
        assert(e > 0), C->paths[e - 1] -= 1, p = lcK_parent(C, e);
        C->paths[e] = &p->children[p->child_count - (d - i)];
    }
    if (e == lcK_levels(C)) {
        if ((cc = p->child_count) && i >= cc) {
            C->paths[e] = &p->children[cc - 1];
            C->lidx = p->breaks[cc - 1], C->off -= p->bytes[cc - 1];
        }
        lcD_foldleaf(C), C->loff = lcL_sumbytes(lcK_leaf(C), 0, C->lidx);
    }
}

static int lcB_stitch(lc_Cursor *C, lc_Node *rt) {
    int      k, kl, cc, rtcc, r, m, l = lcK_levels(C), d = 0;
    lc_Node *p;
    if (!lcP_reserve(C->tree->S, &C->tree->S->nodes, l + 1)) return LC_ERRMEM;
    for (k = 0; k <= lcK_levels(C); ++k) {
        lc_Diff db, dl;
        rtcc = rt[k].child_count, rt[k].child_count = 0;
        if (rtcc == 0 && k < lcK_levels(C)) continue;
        kl = lcK_levels(C) - k, p = lcK_parent(C, kl), cc = p->child_count;
        if (k == 0 && cc && p->bytes[cc - 1] == 0) { /* last leaf empty? */
            memcpy(p->children[cc - 1], rt[0].children[0], sizeof(lc_Leaf));
            lcP_free(&C->tree->S->leaves, (lc_Leaf *)rt[0].children[0]);
            rt[0].children[0] = p->children[cc -= 1], C->paths[kl] -= 1;
        }
        if ((m = lc_min(rtcc, LC_FANOUT - cc)) < rtcc || kl == 0) {
            if (k > 0) lcB_balancerange(C, kl, l, d);
            l = kl, d = LC_FANOUT - lcK_idx(C, p, kl);
        }
        lcN_copy(p, cc, &rt[k], 0, m), p->child_count = cc + m;
        db = lcN_sumbytes(&rt[k], 0, m), dl = lcN_sumbreaks(&rt[k], 0, m);
        lcM_up(C, kl - 1, db, dl);
        if (m == rtcc) continue;
        r = lcB_fillrt(C, rt, kl), assert(r >= 0), (void)r;
        kl += r, l += r, p = lcK_parent(C, kl);
        lcN_copy(p, 0, &rt[k], m, rtcc - m), p->child_count = rtcc - m;
        db = lcN_sumbytes(&rt[k], m, rtcc), dl = lcN_sumbreaks(&rt[k], m, rtcc);
        lcM_up(C, kl - 1, db, dl);
    }
    return lcB_balancerange(C, 0, 0, 0), LC_OK;
}

static int lcB_rollback(lc_Cursor *C, lc_Node *rt, lc_Cursor *sC, int sl) {
    lc_Node *p, *r = &C->tree->root;
    int      i, l, k, cc, rtcc;
    lc_Diff  db, dl;
    for (l = lcK_levels(C); l > sl; --l) {
        lcN_freechildren(C->tree->S, r, l, 1, r->child_count);
        C->tree->bytes = r->bytes[0], C->tree->breaks = r->breaks[0];
        p = r->children[0], *r = *p, lcP_free(&C->tree->S->nodes, p);
    }
    *C = *sC, C->tree->levels = sl;
    if (rt[0].child_count > 0) {
        lc_Leaf *lf = lcK_leaf(C);
        memcpy(&lf->bytes[C->lidx], rt[0].children[0],
               rt[0].breaks[0] * sizeof(unsigned));
        lcM_up(C, sl, rt[0].bytes[0], rt[0].breaks[0]);
        *C->paths[sl] = rt[0].children[0], rt[0].children[0] = (lc_Node *)lf;
    }
    for (l = 0; l <= sl; ++l) {
        if ((rtcc = (int)rt[k = sl - l].child_count) == 0) continue;
        p = lcK_parent(C, l), i = lcK_idx(C, p, l) + (k > 0);
        cc = (int)p->child_count;
        db = lcN_sumbytes(&rt[k], 0, rtcc), db -= lcN_sumbytes(p, i, cc);
        dl = lcN_sumbreaks(&rt[k], 0, rtcc), dl -= lcN_sumbreaks(p, i, cc);
        lcN_freechildren(C->tree->S, p, k, i, cc),
                lcN_copy(p, i, &rt[k], 0, rtcc);
        lcM_up(C, l - 1, db, dl), p->child_count = i + rt[k].child_count;
    }
    return LC_ERRMEM;
}

static int lcB_fixremain(lc_Cursor *sC, int l, unsigned rm) {
    lc_Node *p, ***sp;
    if (p = lcK_parent(sC, l), lcK_idx(sC, p, l) < (int)p->child_count) {
        int k = lcK_levels(sC) - l, i0 = lcK_idx(sC, &sC->tree->root, 0);
        sp = sC->paths, memmove(sp + k + 1, sp + 1, l * sizeof(lc_Node **));
        for (l = 0; l < k; ++l) sp[l] = &lcK_parent(sC, l)->children[0];
        sp[k] = &lcK_parent(sC, k)->children[i0];
        lcK_leaf(sC)->bytes[sC->lidx] += rm, lcM_up(sC, k, rm, 0);
        return 1;
    }
    return 0;
}

LC_API int lc_insert(lc_Cursor *C, unsigned e, lc_Scanner *sc, void *ud) {
    lc_Node  *p, rt[LC_MAX_LEVEL];
    int       i, r, sl;
    lc_Cursor sC;
    if (!C || !C->tree || !sc) return LC_ERRPARAM;
    for (i = 0; i < LC_MAX_LEVEL; i++) rt[i].child_count = 0;
    if ((r = lcB_cutleaf(C, rt)) != LC_OK) return r;
    sC = *C, sl = (int)lcK_levels(C);
    while ((r = lcB_append(C, sc, ud)) > 0) {
        C->off += C->loff, C->loff = 0, C->lidx = 0;
        if ((r = lcB_fillrt(C, rt, (int)lcK_levels(C))) < 0) break;
    }
    if (r < 0 || (r = lcB_stitch(C, rt)) < 0)
        return lcB_rollback(C, rt, &sC, sl);
    if (lcB_fixremain(&sC, sl, C->col)) {
        size_t *poff = &C->off;
        if ((lc_Leaf *)*sC.paths[sl] == lcK_leaf(C)) poff = &C->loff;
        *poff += C->col, C->col = 0;
    }
    p = lcK_parent(C, lcK_levels(C)), i = lcK_idx(C, p, lcK_levels(C));
    if (C->lidx < p->breaks[i])
        lcK_leaf(C)->bytes[C->lidx] += e, lcM_up(C, lcK_levels(C), e, 0);
    return (C->col += e), LC_OK;
}

LC_NS_END

#endif /* LC_IMPLEMENTATION */
