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

typedef void *lc_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

/* state lifecycle */
LC_API lc_State *lc_open(lc_Alloc *allocf, void *ud);
LC_API void      lc_close(lc_State *S);
LC_API void      lc_reset(lc_State *S);

/* tree lifecycle */
LC_API lc_Cache *lc_newtree(lc_State *S);
LC_API void      lc_deltree(lc_State *S, lc_Cache *c);

typedef unsigned lc_Scanner(void *ud, size_t pos);
LC_API int       lc_scan(lc_Cache *c, lc_Scanner *scanner, void *ud);

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
LC_API int lc_markbreaks(lc_Cursor *C, const unsigned *brs, size_t count);
LC_API int lc_clearbreaks(lc_Cursor *C, size_t len);

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
#define lc_retvoid(expr) do { expr; return; } while (0)
#define lc_min(a,b)      ((a) < (b) ? (a) : (b))
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
} lc_Pool;

struct lc_State {
    void     *alloc_ud; /* user data for allocf */
    lc_Alloc *allocf;   /* memory allocator */
    lc_Pool   nodes;    /* pool for lc_Node objects */
    lc_Pool   leaves;   /* pool for lc_Leaf objects */
};

/* pool allocator */

static void lc_initpool(lc_Pool *pool, size_t obj_size) {
    memset(pool, 0, sizeof(lc_Pool));
    pool->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < LC_PAGE_SIZE / 4);
}

/* clang-format off */
static void lc_poolfree(lc_Pool *pool, void *obj)
{ *(void **)obj = pool->freed, pool->freed = obj; }
/* clang-format on */

static void lc_freepool(lc_State *S, lc_Pool *pool) {
    void *page = pool->pages;
    while (page) {
        void *next = *(void **)((char *)page + LC_PAGE_SIZE - sizeof(void *));
        S->allocf(S->alloc_ud, page, LC_PAGE_SIZE, 0);
        page = next;
    }
    lc_initpool(pool, pool->obj_size);
}

LC_STATIC void *lc_poolalloc(lc_State *S, lc_Pool *pool) {
    void *obj = pool->freed;
    if (obj == NULL) {
        size_t objsz = pool->obj_size, off;
        void  *page = S->allocf(S->alloc_ud, NULL, 0, LC_PAGE_SIZE);
        if (page == NULL) return NULL;
        off = ((LC_PAGE_SIZE - sizeof(void *)) / objsz - 1) * objsz;
        for (; off > 0; off -= objsz) {
            void **entry = (void **)((char *)page + off);
            *entry = pool->freed, pool->freed = (void *)entry;
        }
        *(void **)((char *)page + LC_PAGE_SIZE - sizeof(void *)) = pool->pages;
        pool->pages = page;
        obj = (void *)page;
        return obj;
    }
    pool->freed = *(void **)obj;
    return obj;
}

/* state lifecycle */

LC_API lc_State *lc_open(lc_Alloc *allocf, void *ud) {
    lc_State *S = (lc_State *)allocf(ud, NULL, 0, sizeof(lc_State));
    if (S == NULL) return NULL;
    S->alloc_ud = ud;
    S->allocf = allocf;
    lc_initpool(&S->nodes, sizeof(lc_Node));
    lc_initpool(&S->leaves, sizeof(lc_Leaf));
    return S;
}

LC_API void lc_close(lc_State *S) {
    lc_freepool(S, &S->nodes);
    lc_freepool(S, &S->leaves);
    S->allocf(S->alloc_ud, S, sizeof(lc_State), 0);
}

LC_API void lc_reset(lc_State *S) {
    lc_freepool(S, &S->nodes);
    lc_freepool(S, &S->leaves);
}

/* tree lifecycle */

LC_API lc_Cache *lc_newtree(lc_State *S) {
    lc_Cache *c = (lc_Cache *)S->allocf(S->alloc_ud, NULL, 0, sizeof(lc_Cache));
    if (c == NULL) return NULL;
    memset(c, 0, sizeof(lc_Cache));
    c->S = S;
    return c;
}

static void lcN_freechildren(lc_State *S, lc_Node *n, int l, int levels) {
    int i;
    if (l == levels) {
        for (i = 0; i < n->child_count; i++)
            lc_poolfree(&S->leaves, n->children[i]);
        return;
    }
    for (i = 0; i < n->child_count; i++) {
        lcN_freechildren(S, n->children[i], l + 1, levels);
        lc_poolfree(&S->nodes, n->children[i]);
    }
}

LC_API void lc_deltree(lc_State *S, lc_Cache *c) {
    if (c->levels > 0) lcN_freechildren(S, &c->root, 0, c->levels);
    S->allocf(S->alloc_ud, c, sizeof(lc_Cache), 0);
}

/* cursor macros */

#define lcK_levels(C) ((C)->tree->levels)
#define lcK_breaks(C) ((C)->tree->breaks)
#define lcK_bytes(C)  ((C)->tree->bytes)

#define lcK_parent(C, l) ((l) ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define lcK_idx(C, p, l) ((C)->paths[(l)] - (p)->children)
#define lcK_leaf(C)      (*(lc_Leaf **)(C)->paths[lcK_levels(C)])

/* utils */

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

static void lcN_append(lc_Node *dst, const lc_Node *src, unsigned n) {
    unsigned cc = dst->child_count;
    memcpy(&dst->children[cc], src->children, n * sizeof(lc_Node *));
    memcpy(&dst->bytes[cc], src->bytes, n * sizeof(size_t));
    memcpy(&dst->breaks[cc], src->breaks, n * sizeof(size_t));
    dst->child_count = (unsigned short)(cc + n);
}

/* simple queries */

LC_API size_t lc_breaks(const lc_Cache *c) { return c->breaks; }
LC_API size_t lc_bytes(const lc_Cache *c) { return c->bytes; }

/* cursor helpers */

static void lcK_findleaf(lc_Cursor *C, unsigned l, size_t *poff) {
    assert(C->off + *poff <= lcK_bytes(C));
    for (; l <= lcK_levels(C); ++l) {
        lc_Node *p = lcK_parent(C, l);
        unsigned i;
        for (i = 0; i < p->child_count; ++i) {
            if (*poff < p->bytes[i]) break;
            C->off += p->bytes[i], C->idx += p->breaks[i];
            *poff -= p->bytes[i];
        }
        C->paths[l] = &p->children[i];
    }
}

static void lcK_findline(lc_Cursor *C, unsigned l, size_t *pline) {
    assert(C->idx + *pline <= lcK_breaks(C));
    for (; l <= lcK_levels(C); ++l) {
        lc_Node *p = lcK_parent(C, l);
        unsigned i;
        for (i = 0; i < p->child_count; ++i) {
            if (*pline <= p->breaks[i]) break;
            C->off += p->bytes[i], C->idx += p->breaks[i];
            *pline -= p->breaks[i];
        }
        C->paths[l] = &p->children[i];
    }
}

static void lcK_findinleaf(lc_Cursor *C, size_t off) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    unsigned leaf_i = (unsigned)lcK_idx(C, p, lcK_levels(C));
    size_t   i = C->lidx, count = p->breaks[leaf_i];
    lc_Leaf *leaf = (lc_Leaf *)p->children[leaf_i];
    for (off += C->col; i < count; ++i) {
        if (off < leaf->bytes[i]) break;
        off -= leaf->bytes[i], C->loff += leaf->bytes[i], C->idx += 1;
    }
    assert(i < count), C->lidx = (unsigned short)i, C->col = off;
}

static void lcK_locend(lc_Cursor *C) {
    lc_Node *n = &C->tree->root;
    unsigned l;
    if (n->child_count == 0) return;
    for (l = 0; l < lcK_levels(C); ++l)
        n = *(C->paths[l] = &n->children[n->child_count - 1]);
    C->paths[l] = &n->children[n->child_count - 1];
    C->lidx = n->breaks[n->child_count - 1], C->idx = lcK_breaks(C);
    C->loff = n->bytes[n->child_count - 1], C->off = lcK_bytes(C) - C->loff;
    C->col = 0;
}

static void lcK_forwardoff(lc_Cursor *C, size_t d) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      l, i = lcK_idx(C, p, lcK_levels(C));
    size_t   lc = lcK_locoff(C), in = p->bytes[i] - lc;
    if (d < in) lc_retvoid(lcK_findinleaf(C, d));
    d -= in, C->off += p->bytes[i], C->idx += p->breaks[i] - C->lidx;
    for (l = lcK_levels(C); l >= 0; --l) {
        p = lcK_parent(C, l), i = lcK_idx(C, p, l) + 1;
        for (; i < (int)p->child_count; ++i) {
            if (d <= p->bytes[i]) break;
            C->off += p->bytes[i], C->idx += p->breaks[i];
            d -= p->bytes[i];
        }
        if (i < (int)p->child_count) break;
    }
    assert(l >= 0), C->paths[l] = &p->children[i],
                    C->loff = C->lidx = C->col = 0;
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
                if (d <= p->breaks[i]) break;
                C->off += p->bytes[i], C->idx += p->breaks[i];
                d -= p->breaks[i];
            }
            if (i < p->child_count) break;
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

LC_API int lc_seek(lc_Cursor *C, lc_Cache *c, size_t pos) {
    if (C == NULL || c == NULL) return LC_ERRPARAM;
    memset(C, 0, sizeof(lc_Cursor));
    C->tree = c;
    if (pos >= c->bytes)
        return lcK_locend(C), C->col = (unsigned)(pos - c->bytes), LC_OK;
    if (c->root.child_count == 0) return LC_OK;
    return lcK_findleaf(C, 0, &pos), lcK_findinleaf(C, pos), LC_OK;
}

LC_API int lc_seekline(lc_Cursor *C, lc_Cache *c, size_t n) {
    if (C == NULL || c == NULL) return LC_ERRPARAM;
    if (n > c->breaks) return LC_ERRPARAM;
    memset(C, 0, sizeof(lc_Cursor));
    C->tree = c;
    if (c->root.child_count == 0) return LC_OK;
    lcK_findline(C, 0, &n);
    C->loff += lcL_sumbytes(lcK_leaf(C), C->lidx, C->lidx + n) - C->col;
    C->lidx += n, C->idx += n, C->col = 0;
    return LC_OK;
}

LC_API int lc_advance(lc_Cursor *C, lc_Diff delta) {
    lc_Diff n;
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    if (delta == 0 || lcK_bytes(C) == 0) return LC_OK;
    n = (lc_Diff)lc_offset(C) + delta;
    if (n < 0) return lcK_backwardoff(C, lc_offset(C)), LC_OK;
    if ((size_t)n >= lcK_bytes(C)) return lcK_locend(C), LC_OK;
    if (delta < 0)
        lcK_backwardoff(C, (size_t)(-delta));
    else
        lcK_forwardoff(C, (size_t)delta);
    return LC_OK;
}

LC_API int lc_advline(lc_Cursor *C, lc_Diff delta) {
    lc_Diff n, line;
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    if (lcK_bytes(C) == 0) return LC_OK;
    line = (lc_Diff)C->idx, n = line + delta;
    if (n < 0) n = 0;
    if ((size_t)n > lcK_breaks(C)) n = (lc_Diff)lcK_breaks(C);
    if (n == line) return LC_OK;
    if (n == 0)
        lcK_backwardline(C, (size_t)line);
    else if ((size_t)n == lcK_breaks(C))
        lcK_forwardline(C, lcK_breaks(C) - (size_t)line);
    else if (delta < 0)
        lcK_backwardline(C, (size_t)(-delta));
    else
        lcK_forwardline(C, (size_t)delta);
    return LC_OK;
}

LC_API size_t lc_offset(const lc_Cursor *C) {
    return C ? C->off + C->loff + C->col : 0;
}
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

static void lcK_upmetrics(lc_Cursor *C, int l, lc_Diff db, lc_Diff dl) {
    for (; l >= 0; --l) {
        lc_Node *p = lcK_parent(C, l);
        int      i = lcK_idx(C, p, l);
        p->bytes[i] += db, p->breaks[i] += dl;
    }
    C->tree->bytes += db, C->tree->breaks += dl;
}

static int lcD_divergence(lc_Cursor *C1, lc_Cursor *C2) {
    int l;
    for (l = 0; l <= lcK_levels(C1); l++)
        if (C1->paths[l] != C2->paths[l]) return l;
    return l;
}

static void lcD_shrink(lc_Cursor *C, int l, int start, int end) {
    lc_Node *p = lcK_parent(C, l);
    int      moved = p->child_count - end, n;
    if (start >= end) return;
    if (l == lcK_levels(C)) {
        for (n = start; n < end; ++n)
            lc_poolfree(&C->tree->S->leaves, p->children[n]);
    } else {
        for (n = start; n < end; ++n) {
            lcN_freechildren(C->tree->S, p->children[n], l + 1, lcK_levels(C));
            lc_poolfree(&C->tree->S->nodes, p->children[n]);
        }
    }
    memmove(&p->children[start], &p->children[end], moved * sizeof(lc_Node *));
    memmove(&p->bytes[start], &p->bytes[end], moved * sizeof(size_t));
    memmove(&p->breaks[start], &p->breaks[end], moved * sizeof(size_t));
    p->child_count -= (end - start);
}

static void lcD_prune(lc_Cursor *C1, lc_Cursor *C2, int l) {
    lc_Node *p = lcK_parent(C1, l);
    int      i1 = lcK_idx(C1, p, l);
    int      i2 = lcK_idx(C2, p, l);
    int      mi;
    size_t   mid_bytes, mid_breaks;
    if (i2 <= i1 + 1) return;
    mid_bytes = mid_breaks = 0;
    for (mi = i1 + 1; mi < i2; mi++)
        mid_bytes += p->bytes[mi], mid_breaks += p->breaks[mi];
    lcD_shrink(C1, l, i1 + 1, i2);
    C2->paths[l] = &p->children[i1 + 1];
    lcK_upmetrics(C1, l - 1, -(lc_Diff)mid_bytes, -(lc_Diff)mid_breaks);
}

static void lcD_trimnode(lc_Cursor *C, int l, int left) {
    lc_Node *p = lcK_parent(C, l);
    int      mi, i = lcK_idx(C, p, l);
    lc_Diff  db = 0, dl = 0;
    if (left) {
        if (i + 1 >= p->child_count) return;
        for (mi = i + 1; mi < p->child_count; mi++)
            db += p->bytes[mi], dl += p->breaks[mi];
        lcD_shrink(C, l, i + 1, p->child_count);
    } else {
        if (i <= 0) return;
        for (mi = 0; mi < i; mi++) db += p->bytes[mi], dl += p->breaks[mi];
        lcD_shrink(C, l, 0, i);
        C->paths[l] = &p->children[0];
    }
    lcK_upmetrics(C, l - 1, -db, -dl);
}

static void lcD_trimleaf(lc_Cursor *C, int left) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      li = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf *leaf = (lc_Leaf *)p->children[li];
    int      count = (int)p->breaks[li];
    lc_Diff  db = 0, dl = 0;
    if ((int)C->lidx >= count) {
        if (left) return;
        db = p->bytes[li], dl = count;
    } else if (left)
        db = lcL_sumbytes(leaf, C->lidx, count), dl = count - C->lidx;
    else {
        db += lcL_sumbytes(leaf, 0, C->lidx), dl = C->lidx;
        memmove(leaf->bytes, &leaf->bytes[C->lidx],
                (unsigned)(count - (int)C->lidx) * sizeof(unsigned));
        if (C->lidx < count) db += C->col, leaf->bytes[0] -= C->col;
        C->lidx = 0; /* TODO: temporary move leaf to head to pass tests */
    }
    lcK_upmetrics(C, lcK_levels(C), -(lc_Diff)db, -(lc_Diff)dl);
}

static int lcD_mergeleaf(lc_Cursor *C1, lc_Cursor *C2) {
    lc_Leaf *l = lcK_leaf(C1), *r = lcK_leaf(C2);
    int      ll = lcK_levels(C1), lr = lcK_levels(C2);
    lc_Node *pl = lcK_parent(C1, ll), *pr = lcK_parent(C2, lr);
    int      il = lcK_idx(C1, pl, ll), ir = lcK_idx(C2, pr, lr);
    int      sr = C2->lidx, cl = pl->breaks[il], cr = pr->breaks[ir] - sr;
    int      mid, n, total = cl + cr, tb = pl->bytes[il] + pr->bytes[ir];
    if (cl >= LC_LEAF_FANOUT / 2 && cr >= LC_LEAF_FANOUT / 2) {
        if (sr != 0) memmove(r->bytes, &r->bytes[sr], cr * sizeof(unsigned));
        return 0;
    }
    if (total <= LC_LEAF_FANOUT) {
        memcpy(&l->bytes[cl], &r->bytes[sr], cr * sizeof(unsigned));
        pl->breaks[il] += cr, pl->bytes[il] += pr->bytes[ir];
        lcD_shrink(C1, ll, ir, ir + 1);
        if (pl != pr) {
            lcK_upmetrics(C1, ll - 1, pr->bytes[ir], cr);
            lcK_upmetrics(C2, lr - 1, -(lc_Diff)pr->bytes[ir], -(lc_Diff)cr);
        }
        return 1;
    }
    if (cl == (mid = total / 2)) return 0;
    if ((n = cl - mid) < 0) {
        memcpy(&l->bytes[cl], &r->bytes[sr], -n * sizeof(unsigned));
        cr += n, memmove(r->bytes, &r->bytes[sr - n], cr * sizeof(unsigned));
    } else {
        memmove(&r->bytes[n], &r->bytes[sr], cr * sizeof(unsigned));
        memcpy(r->bytes, &l->bytes[mid], n * sizeof(unsigned)), cr += n;
    }
    pl->breaks[il] = mid, pr->breaks[ir] = cr;
    pl->bytes[il] = lcL_sumbytes(l, 0, mid);
    pr->bytes[ir] = tb - pl->bytes[il];
    if (pl != pr) {
        lcK_upmetrics(C1, ll - 1, 0, -n);
        lcK_upmetrics(C2, lr - 1, 0, n);
    }
    return 0;
}

static int lcD_mergenode(lc_Cursor *C1, lc_Cursor *C2, unsigned l) {
    lc_Node *p = lcK_parent(C1, (int)l);
    int      i1 = lcK_idx(C1, p, (int)l);
    int      i2 = lcK_idx(C2, p, (int)l);
    lc_Node *cur, *sib;
    unsigned cur_cc, sib_cc, total, mid, n;
    assert(l < lcK_levels(C1) && i1 + 1 == i2);
    cur = p->children[i1], sib = p->children[i2];
    cur_cc = cur->child_count, sib_cc = sib->child_count;
    if (cur_cc >= LC_FANOUT / 2 && sib_cc >= LC_FANOUT / 2) return 0;
    total = cur_cc + sib_cc;
    if (total > LC_FANOUT) {
        mid = total / 2;
        if (cur_cc == mid) return 0;
        if (cur_cc < mid) {
            n = mid - cur_cc;
            lcN_append(cur, sib, n);
            sib_cc -= n;
            memmove(sib->children, &sib->children[n],
                    sib_cc * sizeof(lc_Node *));
            memmove(sib->bytes, &sib->bytes[n], sib_cc * sizeof(size_t));
            memmove(sib->breaks, &sib->breaks[n], sib_cc * sizeof(size_t));
            sib->child_count = (unsigned short)sib_cc;
        } else {
            n = cur_cc - mid;
            lcN_makespace(sib, 0, n);
            memcpy(sib->children, &cur->children[mid], n * sizeof(lc_Node *));
            memcpy(sib->bytes, &cur->bytes[mid], n * sizeof(size_t));
            memcpy(sib->breaks, &cur->breaks[mid], n * sizeof(size_t));
            lcN_append(sib, cur, n);
            cur->child_count = (unsigned short)mid;
        }
        p->bytes[i1] = lcN_sumbytes(cur, 0, (int)cur->child_count);
        p->breaks[i1] = lcN_sumbreaks(cur, 0, (int)cur->child_count);
        p->bytes[i2] = lcN_sumbytes(sib, 0, (int)sib_cc);
        p->breaks[i2] = lcN_sumbreaks(sib, 0, (int)sib_cc);
        return 0;
    }
    lcN_append(cur, sib, sib_cc);
    p->bytes[i1] += p->bytes[i2];
    p->breaks[i1] += p->breaks[i2];
    if (C1->paths[l + 1] >= sib->children
        && C1->paths[l + 1] < sib->children + sib_cc)
        C1->paths[l + 1] = cur->children + cur_cc
                         + (unsigned)(C1->paths[l + 1] - sib->children);
    sib->child_count = 0;
    lcD_shrink(C1, (int)l, i2, i2 + 1);
    return 1;
}

static void lcD_rebalance(lc_Cursor *C, int l) {
    lc_Cursor C2;
    /* leaf level */
    if (l == lcK_levels(C)) {
        lc_Node *p = lcK_parent(C, lcK_levels(C));
        int      li = lcK_idx(C, p, lcK_levels(C));
        int      merged = 0, need_merge = p->breaks[li] < LC_LEAF_FANOUT / 2;
        if (need_merge) {
            if (li + 1 < (int)p->child_count) {
                C2 = *C;
                C2.paths[lcK_levels(C)] = &p->children[li + 1];
                merged = lcD_mergeleaf(C, &C2);
            }
            if (!merged && li > 0) {
                C2 = *C;
                C2.paths[lcK_levels(C)] = &p->children[li - 1];
                lcD_mergeleaf(&C2, C);
                C->paths[lcK_levels(C)] = C2.paths[lcK_levels(C)];
            }
        }
        l -= 1;
    }
    /* internal levels upward */
    for (; l >= 0; --l) {
        lc_Node *p = lcK_parent(C, l);
        int      merged = 0, i = lcK_idx(C, p, l);
        if (p->children[i]->child_count >= LC_FANOUT / 2) continue;
        if (i + 1 < (int)p->child_count) {
            C2 = *C;
            C2.paths[l] = &p->children[i + 1];
            merged = lcD_mergenode(C, &C2, (unsigned)l);
        }
        if (!merged && i > 0) {
            C2 = *C;
            C2.paths[l] = &p->children[i - 1];
            merged = lcD_mergenode(&C2, C, (unsigned)l);
            if (merged) C->paths[l] = C2.paths[l];
        }
    }
    /* shrink single-child root, then re-check the new structure */
    while (lcK_levels(C) && C->tree->root.child_count == 1) {
        lc_Node *only, *p = lcK_parent(C, 0);
        int      i = lcK_idx(C, p, 0);
        C->tree->root = *(only = p->children[i]);
        lc_poolfree(&C->tree->S->nodes, only);
        C->tree->levels--;
        memmove(C->paths + 1, C->paths + 2, lcK_levels(C) * sizeof(lc_Node **));
    }
}

static void lcD_spliceleaf(lc_Cursor *C, size_t del) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      li = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf *leaf = (lc_Leaf *)p->children[li];
    int      end = C->lidx, count = (int)p->breaks[li];
    lc_Diff  removed;
    if (end == count) return;
    if (del < leaf->bytes[end] - C->col) {
        leaf->bytes[end] -= del, removed = del;
        lcK_upmetrics(C, lcK_levels(C), -removed, 0);
        return;
    }
    del += C->col, removed = C->col;
    for (; end < count; ++end) {
        unsigned bytes = leaf->bytes[end];
        if (del < bytes) break;
        del -= bytes, removed -= bytes;
    }
    memmove(&leaf->bytes[C->lidx], &leaf->bytes[end],
            (count - end) * sizeof(unsigned));
    if (end < count) leaf->bytes[C->lidx] += C->col - (lc_Diff)del;
    removed -= (lc_Diff)del + (end == count ? C->col : 0);
    lcK_upmetrics(C, lcK_levels(C), removed, -(lc_Diff)(end - C->lidx));
    lcD_rebalance(C, lcK_levels(C));
}

static void lcD_splicerange(lc_Cursor *C1, lc_Cursor *C2) {
    int dl, removed, l = lcD_divergence(C1, C2);
    lcD_prune(C1, C2, l);
    assert(C1->paths[l] != C2->paths[l]);
    for (dl = l + 1; dl <= lcK_levels(C1); dl++)
        lcD_trimnode(C1, dl, 1), lcD_trimnode(C2, dl, 0);
    lcD_trimleaf(C1, 1), lcD_trimleaf(C2, 0);
    removed = (l == lcK_levels(C1)) ? lcD_mergeleaf(C1, C2) : 0;
    if (!removed) removed = lcD_mergenode(C1, C2, (unsigned)l);
    lcD_rebalance(C1, l);
}

LC_API void lc_splice(lc_Cursor *C, size_t del, size_t ins) {
    lc_Node *p;
    lc_Leaf *leaf;
    size_t   remaining;
    int      li;
    if (C == NULL || C->tree == NULL || (del == 0 && ins == 0)) return;
    if (lcK_levels(C) == 0 && C->tree->root.child_count == 0) return;
    remaining = C->tree->bytes - lc_offset(C);
    if (del > remaining) del = remaining;
    p = lcK_parent(C, lcK_levels(C)), li = (int)lcK_idx(C, p, lcK_levels(C));

    /* step 1: delete — same-leaf fast path or cross-leaf splicerange */
    if (del > 0) {
        lc_Cursor C2 = *C;
        lc_advance(&C2, (lc_Diff)del);
        if (C->paths[lcK_levels(C)] != C2.paths[lcK_levels(C)])
            lcD_splicerange(C, &C2);
        else
            lcD_spliceleaf(C, del);
    }

    /* step 2: empty tree reset */
    if (C->tree->bytes == 0 && C->tree->breaks == 0) {
        memset(&C->tree->root, 0, sizeof(lc_Node));
        C->tree->levels = 0;
        C->col += (unsigned)ins;
        return;
    }

    /* step 3: insert */
    if (ins == 0) return;
    p = lcK_parent(C, lcK_levels(C)), li = (int)lcK_idx(C, p, lcK_levels(C));
    leaf = (lc_Leaf *)p->children[li];
    if (C->lidx < p->breaks[li]) {
        leaf->bytes[C->lidx] += ins;
        lcK_upmetrics(C, lcK_levels(C), (lc_Diff)ins, 0);
    }
    C->col += ins;
}

LC_API int lc_clearbreaks(lc_Cursor *C, size_t len) {
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    lc_splice(C, len, len);
    return LC_OK;
}

/* insertion */

/* initialize empty tree with first break at `br-1` bytes from BOF */
static int lcB_initempty(lc_Cursor *C, unsigned br) {
    lc_Cache *c = C->tree;
    lc_Leaf  *leaf = (lc_Leaf *)lc_poolalloc(c->S, &c->S->leaves);
    if (!leaf) return LC_ERRMEM;
    leaf->bytes[0] = br;
    c->root.children[0] = (lc_Node *)leaf;
    c->root.bytes[0] = br, c->root.breaks[0] = 1;
    c->root.child_count = 1, c->breaks = 1, c->bytes = br;
    memset(C->paths, 0, sizeof(C->paths));
    C->paths[0] = &c->root.children[0];
    C->off = 0, C->loff = br, C->idx = 1, C->lidx = 1, C->col = 0, C->tree = c;
    return LC_OK;
}

/* save old root into pooled node, rewire root children to [saved, new] */
static int lcB_rootpromote(lc_Cursor *C, lc_Node *new, unsigned mid) {
    lc_Cache *c = C->tree;
    lc_Node  *old = &c->root, save = *old;
    old->children[0] = (lc_Node *)lc_poolalloc(c->S, &c->S->nodes);
    if (!old->children[0]) return LC_ERRMEM;
    *(lc_Node *)old->children[0] = save;
    ((lc_Node *)old->children[0])->child_count = mid;
    old->bytes[0] = lcN_sumbytes((lc_Node *)old->children[0], 0, mid);
    old->breaks[0] = lcN_sumbreaks((lc_Node *)old->children[0], 0, mid);
    old->bytes[1] = c->bytes - old->bytes[0];
    old->breaks[1] = c->breaks - old->breaks[0];
    old->children[1] = new, old->child_count = 2;
    return LC_OK;
}

static int lcB_splitroot(lc_Cursor *C) {
    lc_Cache *c = C->tree;
    lc_State *S = c->S;
    lc_Node *new;
    unsigned oi = (unsigned)(C->paths[0] - c->root.children);
    unsigned mid = c->root.child_count / 2;
    unsigned n = c->root.child_count - mid;
    unsigned nx = oi < mid ? oi : oi - mid;
    int      r;
    new = (lc_Node *)lc_poolalloc(S, &S->nodes);
    if (!new) return LC_ERRMEM;
    memset(new, 0, sizeof(lc_Node));
    memcpy(new->children, c->root.children + mid, n * sizeof(lc_Node *));
    memcpy(new->bytes, c->root.bytes + mid, n * sizeof(size_t));
    memcpy(new->breaks, c->root.breaks + mid, n * sizeof(size_t));
    new->child_count = n;
    r = lcB_rootpromote(C, new, mid);
    if (r < 0) return r;
    c->levels++;
    memmove(C->paths + 1, C->paths, (c->levels) * sizeof(lc_Node **));
    C->paths[0] = &c->root.children[oi >= mid];
    C->paths[1] = &(*C->paths[0])->children[nx];
    return LC_OK;
}

/* update parent metrics and insert new child after splitting old at mid */
static void lcB_parentlink(lc_Node *p, unsigned i, lc_Node *old, lc_Node *new) {
    unsigned mid = (unsigned)old->child_count;
    size_t   ob = p->bytes[i], ob_mid = lcN_sumbytes(old, 0, mid);
    size_t   ol = p->breaks[i], ol_mid = lcN_sumbreaks(old, 0, mid);
    p->bytes[i] = ob_mid, p->breaks[i] = ol_mid;
    lcN_makespace(p, i + 1, 1);
    p->children[i + 1] = new;
    p->bytes[i + 1] = ob - ob_mid;
    p->breaks[i + 1] = ol - ol_mid;
}

static int lcB_splitchild(lc_Cursor *C, int l) {
    lc_State *S = C->tree->S;
    lc_Node  *p = lcK_parent(C, l);
    unsigned  i = lcK_idx(C, p, l);
    lc_Node  *old = p->children[i], *new;
    unsigned  mid = old->child_count / 2;
    unsigned  n = old->child_count - mid;
    new = (lc_Node *)lc_poolalloc(S, &S->nodes);
    if (!new) return LC_ERRMEM;
    memcpy(new->children, old->children + mid, n * sizeof(lc_Node *));
    memcpy(new->bytes, old->bytes + mid, n * sizeof(size_t));
    memcpy(new->breaks, old->breaks + mid, n * sizeof(size_t));
    new->child_count = n, old->child_count = mid;
    lcB_parentlink(p, i, old, new);
    if (C->paths[l + 1] >= old->children + mid) {
        unsigned nx = C->paths[l + 1] - old->children - mid;
        C->paths[l + 1] = &new->children[nx];
        C->paths[l] = &p->children[i + 1];
    }
    return LC_OK;
}

/* split all full ancestors top-down before leaf split */
static int lcB_ensurefit(lc_Cursor *C) {
    int l, r;
    for (l = 0; l <= lcK_levels(C); ++l) {
        lc_Node *p = lcK_parent(C, l);
        if (p->child_count < LC_FANOUT) continue;
        r = l ? lcB_splitchild(C, l - 1) : lcB_splitroot(C);
        if (r < 0) return r;
    }
    return LC_OK;
}

static int lcB_splitleaf(lc_Cursor *C) {
    lc_Cache *c = C->tree;
    lc_State *S = c->S;
    lc_Node  *p = lcK_parent(C, lcK_levels(C));
    int       i = lcK_idx(C, p, lcK_levels(C));
    size_t    obytes, oldb, newb, n, mid = p->breaks[i] / 2;
    lc_Leaf  *old, *new = (lc_Leaf *)lc_poolalloc(S, &S->leaves);
    if (!new) return LC_ERRMEM;
    old = (lc_Leaf *)p->children[i];
    n = p->breaks[i] - mid;
    memcpy(new->bytes, old->bytes + mid, n * sizeof(unsigned));
    oldb = lcL_sumbytes(old, 0, mid), newb = lcL_sumbytes(new, 0, n);
    obytes = p->bytes[i], p->bytes[i] = oldb;
    lcN_makespace(p, i + 1, 1);
    p->children[i + 1] = (lc_Node *)new;
    p->bytes[i + 1] = newb + (obytes - oldb - newb);
    p->breaks[i] = mid, p->breaks[i + 1] = n;
    if (C->lidx >= mid) {
        C->lidx -= mid;
        C->paths[lcK_levels(C)] = &p->children[i + 1];
        return 1; /* cursor moved to new leaf */
    }
    return 0; /* cursor still in old leaf */
}

/* ensure leaf has room for another break; split if full */
static int lcB_fitleaf(lc_Cursor *C) {
    lc_Node *parent = lcK_parent(C, lcK_levels(C));
    int      r, i = lcK_idx(C, parent, lcK_levels(C));
    if (parent->breaks[i] < LC_LEAF_FANOUT) return LC_OK;
    if ((r = lcB_ensurefit(C)) < 0) return r;
    assert(lcK_parent(C, lcK_levels(C))->child_count < LC_FANOUT);
    return lcB_splitleaf(C);
}

/* insert break at cursor */
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
    lcK_upmetrics(C, lcK_levels(C), 0, 1);
}

LC_API int lc_markbreak(lc_Cursor *C, unsigned br) {
    int    r;
    size_t need, remain;
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    if (lcK_levels(C) == 0 && C->tree->root.child_count == 0)
        return lcB_initempty(C, br);
    if (br == (remain = lc_linelen(C) - C->col)) return LC_OK;
    need = lc_offset(C) + br;
    if (br > remain) {
        size_t   leaf_off = C->loff + C->col;
        lc_Node *p;
        int      li;
        lc_splice(C, br, br);
        if (C->tree->root.child_count == 0) return lcB_initempty(C, br);
        if (need > C->tree->bytes) {
            lc_Diff ext = (lc_Diff)(need - C->tree->bytes);
            p = lcK_parent(C, lcK_levels(C));
            li = lcK_idx(C, p, lcK_levels(C));
            lcK_leaf(C)->bytes[(unsigned)(p->breaks[li] - 1)] += (unsigned)ext;
            lcK_upmetrics(C, lcK_levels(C), ext, 0);
        }
        C->col = (unsigned)leaf_off;
        lcB_putbreak(C, 0);
        C->loff += leaf_off;
        C->lidx++;
        C->loff += lcK_leaf(C)->bytes[C->lidx];
        C->idx++, C->lidx++, C->col = 0;
        return LC_OK;
    }
    if ((r = lcB_fitleaf(C)) < 0) return r;
    lcB_putbreak(C, br);
    C->loff += C->col + br, C->idx++, C->lidx++, C->col = 0;
    return LC_OK;
}

LC_API int lc_markbreaks(lc_Cursor *C, const unsigned *brs, size_t count) {
    size_t i;
    int    r;
    for (i = 0; i < count; ++i)
        if ((r = lc_markbreak(C, brs[i])) < 0) return r;
    return LC_OK;
}

LC_API int lc_scan(lc_Cache *c, lc_Scanner *scanner, void *ud) {
    lc_Cursor C;
    unsigned  br;
    int       r;
    if (c == NULL || scanner == NULL) return LC_ERRPARAM;
    C.tree = c;
    if (c->root.child_count == 0) {
        if ((br = scanner(ud, c->bytes)) == 0) return LC_OK;
        if ((r = lcB_initempty(&C, br)) < 0) return r;
    }
    lcK_locend(&C);
    while ((br = scanner(ud, c->bytes)) > 0) {
        lc_Leaf *wr_leaf;
        unsigned wr_lidx;
        if ((r = lcB_fitleaf(&C)) < 0) return r;
        wr_leaf = lcK_leaf(&C), wr_lidx = C.lidx;
        wr_leaf->bytes[wr_lidx] = br;
        C.loff += br, C.lidx++, C.idx++,
                lcK_upmetrics(&C, lcK_levels(&C), br, 1);
    }
    C.col = 0;
    return LC_OK;
}

LC_NS_END

#endif /* LC_IMPLEMENTATION */
