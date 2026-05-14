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

/* initialize */
LC_API int lc_seek(lc_Cursor *C, lc_Cache *c, size_t offset);
LC_API int lc_seekline(lc_Cursor *C, lc_Cache *c, size_t line);

/* move */
LC_API int lc_advance(lc_Cursor *C, ptrdiff_t delta);
LC_API int lc_advline(lc_Cursor *C, ptrdiff_t delta);

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
    lc_State *S;      /* owning state */
    lc_Node   root;   /* root node of the B+ tree */
    size_t    breaks; /* total line breaks in the tree */
    size_t    bytes;  /* total bytes in the tree */
    unsigned  levels; /* tree depth: 0 = root only (empty or single leaf) */
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

/* scan helpers */

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
            if (*poff <= p->bytes[i]) break;
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
        if (off <= leaf->bytes[i]) break;
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
        if (l == (int)lcK_levels(C)) --i;
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
            if (l == (int)lcK_levels(C)) i -= 1;
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
    if (pos >= c->bytes) return lcK_locend(C), LC_OK;
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

LC_API int lc_advance(lc_Cursor *C, ptrdiff_t delta) {
    ptrdiff_t n;
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    if (delta == 0 || lcK_bytes(C) == 0) return LC_OK;
    n = (ptrdiff_t)lc_offset(C) + delta;
    if (n < 0) return lcK_backwardoff(C, lc_offset(C)), LC_OK;
    if ((size_t)n >= lcK_bytes(C)) return lcK_locend(C), LC_OK;
    if (delta < 0)
        lcK_backwardoff(C, (size_t)(-delta));
    else
        lcK_forwardoff(C, (size_t)delta);
    return LC_OK;
}

LC_API int lc_advline(lc_Cursor *C, ptrdiff_t delta) {
    ptrdiff_t n, line;
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    if (lcK_bytes(C) == 0) return LC_OK;
    line = (ptrdiff_t)C->idx, n = line + delta;
    if (n < 0) n = 0;
    if ((size_t)n > lcK_breaks(C)) n = (ptrdiff_t)lcK_breaks(C);
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

/* mutation helpers */

static void lcN_makespace(lc_Node *d, unsigned i, unsigned n) {
    unsigned moved = d->child_count - i;
    assert(d->child_count + n <= LC_FANOUT && i <= d->child_count);
    memmove(&d->children[i + n], &d->children[i], moved * sizeof(lc_Node *));
    memmove(&d->bytes[i + n], &d->bytes[i], moved * sizeof(size_t));
    memmove(&d->breaks[i + n], &d->breaks[i], moved * sizeof(size_t));
    d->child_count += n;
}

static void lcK_upmetrics(lc_Cursor *C, int l, ptrdiff_t db, ptrdiff_t dl) {
    for (; l >= 0; --l) {
        lc_Node *p = lcK_parent(C, l);
        int      i = lcK_idx(C, p, l);
        p->bytes[i] += db, p->breaks[i] += dl;
    }
    C->tree->bytes += db, C->tree->breaks += dl;
}

/* insert break at cursor */
static void lcB_putbreak(lc_Cursor *C, unsigned br) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      i = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf *leaf = (lc_Leaf *)p->children[i];
    size_t   count = p->breaks[i];
    unsigned len = leaf->bytes[C->lidx], split = C->col + br;
    /* cursor must be on a valid segment, not trailing */
    assert(C->lidx < count && split < len);
    memmove(&leaf->bytes[C->lidx + 2], &leaf->bytes[C->lidx + 1],
            (count - C->lidx - 1) * sizeof(unsigned));
    leaf->bytes[C->lidx] = split;
    leaf->bytes[C->lidx + 1] = len - split;
    lcK_upmetrics(C, lcK_levels(C), 0, 1);
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
    for (l = 0; l <= (int)lcK_levels(C); ++l) {
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
        if (wr_leaf->bytes[wr_lidx] != br)
            fprintf(stderr, "DBG BAD WRITE: leaf=%p bytes[%u]=%u expected %u\n",
                    (void *)wr_leaf, wr_lidx, wr_leaf->bytes[wr_lidx], br);
        C.loff += br, C.lidx++, C.idx++,
                lcK_upmetrics(&C, lcK_levels(&C), br, 1);
    }
    C.col = 0;
    return LC_OK;
}

static void lcD_shrink(lc_Cursor *C, int l, int start, int end);
static int lcD_divergence(lc_Cursor *C1, lc_Cursor *C2);
static void lcD_prune(lc_Cursor *C1, lc_Cursor *C2, int l);
static void lcD_trimnode(lc_Cursor *C, int l, int left);
static void lcD_trimleaf(lc_Cursor *C, int left);
static unsigned lcL_append(lc_Leaf *dst, unsigned dst_segs,
                            const lc_Leaf *src, unsigned n);
static void     lcN_append(lc_Node *dst, const lc_Node *src, unsigned n);
static int  lcD_mergeleaf2(lc_Cursor *C1, lc_Cursor *C2);
static int  lcD_mergenode(lc_Cursor *C1, lc_Cursor *C2, unsigned l);
static void lcD_rebalance(lc_Cursor *C, unsigned l);
static void lcD_splicerange(lc_Cursor *C1, lc_Cursor *C2);
static int  lcD_clearleaf(lc_Cursor *C, size_t *pdel, int whole); /* TODO: remove */
static void lcD_spliceleaf(lc_Cursor *C, size_t *pdel);
static void lcD_mergeleaf(lc_Cursor *C1, int no_break_boundary);
static void lcD_merge(lc_Cursor *C1, int l);
static void lcD_freerange(
        lc_Cursor *C1, lc_Cursor *C2, size_t *pdel, int *no_break_boundary);

/* TODO: remove when freerange is deleted */
static int lcD_clearleaf(lc_Cursor *C, size_t *pdel, int whole) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      li = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf *leaf = (lc_Leaf *)p->children[li];
    unsigned count = (unsigned)p->breaks[li];
    unsigned first, seg, off_in_seg, segs_removed;
    size_t   del, total_del;
    int      no_leading_break;

    if (whole) {
        first = 0;
        seg = 0;
        off_in_seg = 0;
    } else {
        first = C->lidx;
        seg = C->lidx;
        off_in_seg = C->col;
    }

    if (!whole && off_in_seg > 0 && seg + 1 < count
        && off_in_seg == leaf->bytes[seg])
        off_in_seg = 0, seg++, first = seg;

    del = *pdel;
    total_del = del;
    segs_removed = 0;
    no_leading_break = 0;

    {
        unsigned remaining = leaf->bytes[seg] - off_in_seg;
        unsigned consumed = (unsigned)(del < remaining ? del : remaining);
        leaf->bytes[seg] -= consumed;
        del -= consumed;
        if (leaf->bytes[seg] == 0) {
            if (!whole && off_in_seg == 0 && seg > 0) first = seg - 1;
            segs_removed++;
            seg++;
        } else {
            if (whole) {
                if (consumed > 0) no_leading_break = 1;
            } else if (off_in_seg == 0 && seg > 0) {
                leaf->bytes[seg - 1] += leaf->bytes[seg];
                leaf->bytes[seg] = 0;
                first = seg - 1;
                segs_removed++;
            }
            seg++;
        }
    }

    while (del > 0 && seg < count) {
        unsigned consumed = (unsigned)(del < leaf->bytes[seg]
                                               ? del : leaf->bytes[seg]);
        leaf->bytes[seg] -= consumed;
        del -= consumed;
        if (leaf->bytes[seg] == 0) segs_removed++, seg++;
        else { seg++; break; }
    }

    if (segs_removed > 0) {
        unsigned write = first, read;
        for (read = first; read < count; read++)
            if (leaf->bytes[read] > 0) leaf->bytes[write++] = leaf->bytes[read];
    }

    lcK_upmetrics(C, lcK_levels(C), -(ptrdiff_t)(total_del - del),
                  -(ptrdiff_t)segs_removed);
    *pdel = del;
    return no_leading_break;
}

/* Delete *pdel bytes from leaf starting at C's position.
 * Same-leaf deletion only — no whole flag, no return value. */
static void lcD_spliceleaf(lc_Cursor *C, size_t *pdel) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      li = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf *leaf = (lc_Leaf *)p->children[li];
    unsigned count = (unsigned)p->breaks[li];
    unsigned first, seg, segs_removed, off_in_seg;
    size_t   del, total_del;

    first = C->lidx;
    seg = C->lidx;
    off_in_seg = C->col;

    /* normalize: cursor at segment boundary moves to next segment start */
    if (off_in_seg > 0 && seg + 1 < count
        && off_in_seg == leaf->bytes[seg])
        off_in_seg = 0, seg++, first = seg;

    del = *pdel;
    total_del = del;
    segs_removed = 0;

    /* first segment (may be partially consumed) */
    {
        unsigned remaining = leaf->bytes[seg] - off_in_seg;
        unsigned consumed = (unsigned)(del < remaining ? del : remaining);
        leaf->bytes[seg] -= consumed;
        del -= consumed;
        if (leaf->bytes[seg] == 0) {
            if (off_in_seg == 0 && seg > 0) first = seg - 1;
            segs_removed++;
            seg++;
        } else {
            if (off_in_seg == 0 && seg > 0) {
                /* consumed leading break: merge remainder into prev segment */
                leaf->bytes[seg - 1] += leaf->bytes[seg];
                leaf->bytes[seg] = 0;
                first = seg - 1;
                segs_removed++;
            }
            seg++;
        }
    }

    /* consume full subsequent segments */
    while (del > 0 && seg < count) {
        unsigned consumed = (unsigned)(del < leaf->bytes[seg]
                                               ? del
                                               : leaf->bytes[seg]);
        leaf->bytes[seg] -= consumed;
        del -= consumed;
        if (leaf->bytes[seg] == 0) segs_removed++, seg++;
        else { seg++; break; }
    }

    /* compact: drop zero-length segments */
    if (segs_removed > 0) {
        unsigned write = first, read;
        for (read = first; read < count; read++)
            if (leaf->bytes[read] > 0) leaf->bytes[write++] = leaf->bytes[read];
    }

    lcK_upmetrics(C, lcK_levels(C), -(ptrdiff_t)(total_del - del),
                  -(ptrdiff_t)segs_removed);
    *pdel = del;
}

/* Find first level where C1 and C2 diverge. Returns l <= levels.
 * If l > levels, C1 and C2 are in the same leaf. */
static int lcD_divergence(lc_Cursor *C1, lc_Cursor *C2) {
    int l;
    for (l = 0; l <= (int)lcK_levels(C1); l++)
        if (C1->paths[l] != C2->paths[l]) return l;
    return l;
}

/* Delete subtrees between C1 and C2 at divergence layer l.
 * Frees children[i1+1..i2-1], shrinks parent arrays, fixes C2 paths. */
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
    lcK_upmetrics(C1, l - 1, -(ptrdiff_t)mid_bytes, -(ptrdiff_t)mid_breaks);
}

/* Trim boundary node: left=1 keeps children[0..idx], left=0 keeps [idx..end).
 * Frees trimmed subtrees via lcD_shrink, updates parent-of-parent metrics. */
static void lcD_trimnode(lc_Cursor *C, int l, int left) {
    lc_Node *p = lcK_parent(C, l);
    int      idx = lcK_idx(C, p, l);
    int      mi;
    size_t   del_bytes, del_breaks;
    if (left) {
        if (idx + 1 >= (int)p->child_count) return;
        del_bytes = del_breaks = 0;
        for (mi = idx + 1; mi < (int)p->child_count; mi++)
            del_bytes += p->bytes[mi], del_breaks += p->breaks[mi];
        lcD_shrink(C, l, idx + 1, (int)p->child_count);
    } else {
        if (idx <= 0) return;
        del_bytes = del_breaks = 0;
        for (mi = 0; mi < idx; mi++)
            del_bytes += p->bytes[mi], del_breaks += p->breaks[mi];
        lcD_shrink(C, l, 0, idx);
        C->paths[l] = &p->children[0];
    }
    lcK_upmetrics(C, l - 1, -(ptrdiff_t)del_bytes, -(ptrdiff_t)del_breaks);
}

/* Trim boundary leaf: left=1 deletes from C to end of leaf,
 * left=0 deletes from start of leaf to C.
 * Normalizes col==bytes[lidx] to next segment start first. */
static void lcD_trimleaf(lc_Cursor *C, int left) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      li = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf *leaf = (lc_Leaf *)p->children[li];
    int      count = (int)p->breaks[li];
    size_t   db = 0;
    int      dl = 0, si;

    /* trailing: left=keep-all (no-op), right=delete-all */
    if ((int)C->lidx >= count) {
        if (left) return;
        for (si = 0; si < count; si++) db += leaf->bytes[si];
        lcK_upmetrics(C, lcK_levels(C), -(ptrdiff_t)db, -(ptrdiff_t)count);
        return;
    }

    /* normalize: cursor at segment end → next segment start */
    if (C->col > 0 && C->col == leaf->bytes[C->lidx]
        && C->lidx + 1 < (unsigned)count)
        C->lidx++, C->col = 0;

    if (left) {
        if (C->col > 0) {
            db = leaf->bytes[C->lidx] - C->col;
            leaf->bytes[C->lidx] = C->col;
            dl = count - (int)C->lidx - 1;
            for (si = (int)C->lidx + 1; si < count; si++) db += leaf->bytes[si];
        } else {
            for (si = (int)C->lidx; si < count; si++) db += leaf->bytes[si];
            dl = count - (int)C->lidx;
        }
    } else {
        if (C->col > 0) {
            for (si = 0; si < (int)C->lidx; si++) db += leaf->bytes[si];
            db += C->col;
            leaf->bytes[C->lidx] -= C->col;
            dl = (int)C->lidx;
            if (C->lidx > 0)
                memmove(leaf->bytes, &leaf->bytes[C->lidx],
                        (unsigned)(count - (int)C->lidx) * sizeof(unsigned));
        } else {
            for (si = 0; si < (int)C->lidx; si++) db += leaf->bytes[si];
            dl = (int)C->lidx;
            if (C->lidx > 0)
                memmove(leaf->bytes, &leaf->bytes[C->lidx],
                        (unsigned)(count - (int)C->lidx) * sizeof(unsigned));
        }
    }
    lcK_upmetrics(C, lcK_levels(C), -(ptrdiff_t)db, -(ptrdiff_t)dl);
}

static unsigned lcL_append(
        lc_Leaf *dst, unsigned dst_segs, const lc_Leaf *src, unsigned n) {
    memcpy(&dst->bytes[dst_segs], src->bytes, n * sizeof(unsigned));
    return dst_segs + n;
}

static void lcN_append(lc_Node *dst, const lc_Node *src, unsigned n) {
    unsigned cc = dst->child_count;
    memcpy(&dst->children[cc], src->children, n * sizeof(lc_Node *));
    memcpy(&dst->bytes[cc], src->bytes, n * sizeof(size_t));
    memcpy(&dst->breaks[cc], src->breaks, n * sizeof(size_t));
    dst->child_count = (unsigned short)(cc + n);
}

/* lcD_mergeleaf2: merge C2's leaf into C1's leaf (adjacent siblings).
 * Returns 1 if C2's leaf was freed, 0 if redistributed or no-op. */
static int lcD_mergeleaf2(lc_Cursor *C1, lc_Cursor *C2) {
    lc_Node *p = lcK_parent(C1, lcK_levels(C1));
    int      i1 = lcK_idx(C1, p, lcK_levels(C1));
    int      i2 = lcK_idx(C2, p, lcK_levels(C2));
    lc_Leaf *left, *right;
    unsigned left_segs, right_segs, total, mid, n;
    if (i2 != i1 + 1) return 0;
    left = (lc_Leaf *)p->children[i1];
    right = (lc_Leaf *)p->children[i2];
    left_segs = (unsigned)p->breaks[i1];
    right_segs = (unsigned)p->breaks[i2];
    if (left_segs >= LC_LEAF_FANOUT / 2 && right_segs >= LC_LEAF_FANOUT / 2)
        return 0;
    total = left_segs + right_segs;
    if (total > LC_LEAF_FANOUT) {
        mid = total / 2;
        if (left_segs < mid) {
            n = mid - left_segs;
            lcL_append(left, left_segs, right, n);
            right_segs -= n;
            memmove(right->bytes, &right->bytes[n],
                    right_segs * sizeof(unsigned));
            p->breaks[i1] = mid, p->breaks[i2] = right_segs;
            p->bytes[i1] = lcL_sumbytes(left, 0, (int)mid);
            p->bytes[i2] = lcL_sumbytes(right, 0, (int)right_segs);
        }
        return 0;
    }
    lcL_append(left, left_segs, right, right_segs);
    if (C1->col > 0) {
        left->bytes[left_segs - 1] += left->bytes[left_segs];
        if (right_segs > 1)
            memmove(&left->bytes[left_segs], &left->bytes[left_segs + 1],
                    (right_segs - 1) * sizeof(unsigned));
        total--;
    }
    p->breaks[i1] = total;
    p->bytes[i1] += p->bytes[i2];
    lcD_shrink(C1, lcK_levels(C1), i2, i2 + 1);
    C1->paths[lcK_levels(C1)] = &p->children[i1];
    return 1;
}

/* lcD_mergenode: merge C2's node into C1's node at level l (adjacent siblings).
 * Returns 1 if C2's node was freed, 0 if redistributed or no-op. */
static int lcD_mergenode(lc_Cursor *C1, lc_Cursor *C2, unsigned l) {
    lc_Node *p = lcK_parent(C1, (int)l);
    int      i1 = lcK_idx(C1, p, (int)l);
    int      i2 = lcK_idx(C2, p, (int)l);
    lc_Node *cur, *sib;
    unsigned cur_cc, sib_cc, total, mid, n;
    if (i2 != i1 + 1) return 0;
    cur = p->children[i1], sib = p->children[i2];
    cur_cc = cur->child_count, sib_cc = sib->child_count;
    if (cur_cc >= LC_FANOUT / 2 && sib_cc >= LC_FANOUT / 2) return 0;
    total = cur_cc + sib_cc;
    if (total > LC_FANOUT) {
        mid = total / 2;
        if (cur_cc < mid) {
            n = mid - cur_cc;
            lcN_append(cur, sib, n);
            sib_cc -= n;
            memmove(sib->children, &sib->children[n],
                    sib_cc * sizeof(lc_Node *));
            memmove(sib->bytes, &sib->bytes[n], sib_cc * sizeof(size_t));
            memmove(sib->breaks, &sib->breaks[n], sib_cc * sizeof(size_t));
            sib->child_count = (unsigned short)sib_cc;
            p->bytes[i1] = lcN_sumbytes(cur, 0, (int)cur->child_count);
            p->breaks[i1] = lcN_sumbreaks(cur, 0, (int)cur->child_count);
            p->bytes[i2] = lcN_sumbytes(sib, 0, (int)sib_cc);
            p->breaks[i2] = lcN_sumbreaks(sib, 0, (int)sib_cc);
        }
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

/* lcD_rebalance: fix under-filled nodes from level l upward, shrink root. */
static void lcD_rebalance(lc_Cursor *C, unsigned l) {
    int dl;
    for (dl = (int)lcK_levels(C) - 1; dl >= (int)l; dl--)
        lcD_merge(C, dl);
    while (lcK_levels(C) > 0 && C->tree->root.child_count == 1) {
        lc_Node *only = C->tree->root.children[0], save = *only;
        C->tree->root = save;
        lc_poolfree(&C->tree->S->nodes, only);
        C->tree->levels--;
        if (lcK_levels(C) > 0)
            memmove(C->paths + 1, C->paths + 2,
                    lcK_levels(C) * sizeof(lc_Node **));
    }
}

static void lcD_splicerange(lc_Cursor *C1, lc_Cursor *C2) {
    int l = lcD_divergence(C1, C2);
    int levels = (int)lcK_levels(C1);
    int dl, removed;
    assert(l <= levels);
    lcD_prune(C1, C2, l);
    for (dl = l + 1; dl <= levels; dl++) {
        lcD_trimnode(C1, dl, 1);
        lcD_trimnode(C2, dl, 0);
    }
    lcD_trimleaf(C1, 1);
    lcD_trimleaf(C2, 0);
    removed = lcD_mergeleaf2(C1, C2);
    for (dl = levels - 1; dl > l; dl--) {
        if (removed) C2->paths[dl - 1] = C1->paths[dl - 1];
        removed = lcD_mergenode(C1, C2, (unsigned)dl);
    }
    if (removed) C2->paths[l] = C1->paths[l];
    lcD_rebalance(C1, (unsigned)l);
}

static void lcD_shrink(lc_Cursor *C, int l, int start, int end) {
    lc_Node *p = lcK_parent(C, l);
    unsigned moved = p->child_count - end, n;
    if (start >= end) return;
    if (l == (int)lcK_levels(C)) {
        for (n = (unsigned)start; n < (unsigned)end; ++n)
            lc_poolfree(&C->tree->S->leaves, p->children[n]);
    } else {
        for (n = (unsigned)start; n < (unsigned)end; ++n) {
            lcN_freechildren(C->tree->S, p->children[n], l + 1, lcK_levels(C));
            lc_poolfree(&C->tree->S->nodes, p->children[n]);
        }
    }
    memmove(&p->children[start], &p->children[end], moved * sizeof(lc_Node *));
    memmove(&p->bytes[start], &p->bytes[end], moved * sizeof(size_t));
    memmove(&p->breaks[start], &p->breaks[end], moved * sizeof(size_t));
    p->child_count -= (end - start);
}

/* lcD_mergeleaf: merge C1's leaf with right sibling if fill < half.
 * no_break_boundary: if set, right leaf lost its leading break so the
 * last segment of left leaf and first segment of right leaf are merged. */
static void lcD_mergeleaf(lc_Cursor *C1, int no_break_boundary) {
    lc_Node *p = lcK_parent(C1, lcK_levels(C1));
    int      i = lcK_idx(C1, p, lcK_levels(C1));
    lc_Leaf *left, *right;
    unsigned left_segs, right_segs, total, mid, n, merged_byte;
    if (i + 1 >= (int)p->child_count) return;
    left = (lc_Leaf *)p->children[i];
    right = (lc_Leaf *)p->children[i + 1];
    left_segs = (unsigned)p->breaks[i];
    right_segs = (unsigned)p->breaks[i + 1];

    /* merge consumed boundary break first, then handle as normal merge */
    if (no_break_boundary) {
        merged_byte = right->bytes[0];
        left->bytes[left_segs - 1] += merged_byte;
        right_segs--;
        if (right_segs > 0) {
            memmove(right->bytes, &right->bytes[1],
                    right_segs * sizeof(unsigned));
            right->bytes[right_segs] = 0;
        } else
            right->bytes[0] = 0;
        C1->tree->breaks--;
        p->bytes[i] += merged_byte, p->bytes[i + 1] -= merged_byte;
        p->breaks[i + 1] = right_segs;
        if (right_segs == 0) {
            p->bytes[i] += p->bytes[i + 1];
            lcD_shrink(C1, lcK_levels(C1), i + 1, i + 2);
            C1->paths[lcK_levels(C1)] = &p->children[i];
            return;
        }
    }

    if (left_segs >= LC_LEAF_FANOUT / 2 && right_segs >= LC_LEAF_FANOUT / 2)
        return;
    total = left_segs + right_segs;
    if (total > LC_LEAF_FANOUT) {
        mid = total / 2;
        if (left_segs < mid) {
            n = mid - left_segs;
            memcpy(&left->bytes[left_segs], right->bytes, n * sizeof(unsigned));
            memmove(right->bytes, &right->bytes[n],
                    (right_segs - n) * sizeof(unsigned));
            p->breaks[i] = mid, p->breaks[i + 1] = right_segs - n;
            p->bytes[i] = lcL_sumbytes(left, 0, (int)mid);
            p->bytes[i + 1] = lcL_sumbytes(right, 0, (int)(right_segs - n));
        }
        return;
    }
    /* merge right into left */
    memcpy(&left->bytes[left_segs], right->bytes,
           right_segs * sizeof(unsigned));
    p->breaks[i] = left_segs + right_segs;
    p->bytes[i] += p->bytes[i + 1];
    lcD_shrink(C1, lcK_levels(C1), i + 1, i + 2);
    C1->paths[lcK_levels(C1)] = &p->children[i];
}

/* lcD_merge: merge low-fill internal node at C->paths[l] with sibling.
 * Only C1 is needed (C2 is dead after freerange). */
static void lcD_merge(lc_Cursor *C1, int l) {
    lc_Node *p, *cur, *sib;
    int      i, levels = (int)lcK_levels(C1);
    unsigned total, mid, n, cur_cc;
    if (l < 0 || l >= levels) return;
    p = lcK_parent(C1, l), i = lcK_idx(C1, p, l);
    cur = p->children[i];
    if (cur->child_count >= LC_FANOUT / 2) return;
    if (i + 1 < (int)p->child_count) {
        sib = p->children[i + 1];
        total = cur->child_count + sib->child_count;
        if (total <= LC_FANOUT) {
            cur_cc = cur->child_count, n = sib->child_count;
            memcpy(&cur->children[cur_cc], sib->children,
                   n * sizeof(lc_Node *));
            memcpy(&cur->bytes[cur_cc], sib->bytes, n * sizeof(size_t));
            memcpy(&cur->breaks[cur_cc], sib->breaks, n * sizeof(size_t));
            cur->child_count += n;
            p->bytes[i] += p->bytes[i + 1], p->breaks[i] += p->breaks[i + 1];
            /* fixup C1 paths that pointed into the shifted sibling */
            if (C1->paths[l + 1] >= sib->children
                && C1->paths[l + 1] < sib->children + n)
                C1->paths[l + 1] = cur->children + cur_cc
                                 + (unsigned)(C1->paths[l + 1] - sib->children);
            sib->child_count = 0; /* children transferred to cur */
            lcD_shrink(C1, l, i + 1, i + 2);
        } else {
            mid = total / 2;
            if (cur->child_count < mid) {
                n = mid - cur->child_count;
                memcpy(&cur->children[cur->child_count], sib->children,
                       n * sizeof(lc_Node *));
                memcpy(&cur->bytes[cur->child_count], sib->bytes,
                       n * sizeof(size_t));
                memcpy(&cur->breaks[cur->child_count], sib->breaks,
                       n * sizeof(size_t));
                if (C1->paths[l + 1] >= sib->children
                    && C1->paths[l + 1] < sib->children + n)
                    C1->paths[l + 1] = cur->children + cur->child_count
                                     + (unsigned)(C1->paths[l + 1]
                                                  - sib->children);
                cur->child_count = (unsigned short)mid;
                sib->child_count -= n;
                memmove(sib->children, &sib->children[n],
                        sib->child_count * sizeof(lc_Node *));
                memmove(sib->bytes, &sib->bytes[n],
                        sib->child_count * sizeof(size_t));
                memmove(sib->breaks, &sib->breaks[n],
                        sib->child_count * sizeof(size_t));
                p->bytes[i] = lcN_sumbytes(cur, 0, mid);
                p->breaks[i] = lcN_sumbreaks(cur, 0, mid);
                p->bytes[i + 1] = lcN_sumbytes(sib, 0, sib->child_count);
                p->breaks[i + 1] = lcN_sumbreaks(sib, 0, sib->child_count);
            }
        }
    } else if (i > 0) {
        sib = p->children[i - 1];
        total = cur->child_count + sib->child_count;
        if (total <= LC_FANOUT) {
            cur_cc = sib->child_count, n = cur->child_count;
            memcpy(&sib->children[cur_cc], cur->children,
                   n * sizeof(lc_Node *));
            memcpy(&sib->bytes[cur_cc], cur->bytes, n * sizeof(size_t));
            memcpy(&sib->breaks[cur_cc], cur->breaks, n * sizeof(size_t));
            sib->child_count += n;
            p->bytes[i - 1] += p->bytes[i], p->breaks[i - 1] += p->breaks[i];
            if (C1->paths[l + 1] >= cur->children
                && C1->paths[l + 1] < cur->children + n)
                C1->paths[l + 1] = sib->children + cur_cc
                                 + (unsigned)(C1->paths[l + 1] - cur->children);
            if (C1->paths[l] == &p->children[i])
                C1->paths[l] = &p->children[i - 1];
            cur->child_count = 0; /* children transferred to sib */
            lcD_shrink(C1, l, i, i + 1);
            i = i - 1;
        } else {
            mid = total / 2;
            if (cur->child_count < mid) {
                n = mid - cur->child_count;
                memcpy(&cur->children[cur->child_count],
                       &sib->children[sib->child_count - n],
                       n * sizeof(lc_Node *));
                memcpy(&cur->bytes[cur->child_count],
                       &sib->bytes[sib->child_count - n], n * sizeof(size_t));
                memcpy(&cur->breaks[cur->child_count],
                       &sib->breaks[sib->child_count - n], n * sizeof(size_t));
                cur->child_count = (unsigned short)mid;
                sib->child_count -= n;
                p->bytes[i] = lcN_sumbytes(cur, 0, mid);
                p->breaks[i] = lcN_sumbreaks(cur, 0, mid);
                p->bytes[i - 1] = lcN_sumbytes(sib, 0, sib->child_count);
                p->breaks[i - 1] = lcN_sumbreaks(sib, 0, sib->child_count);
            }
        }
    }
    if (p->child_count < LC_FANOUT / 2) lcD_merge(C1, l - 1);
    if (l == 0 && p == &C1->tree->root && p->child_count == 1 && levels > 0) {
        lc_Node *only = p->children[0], save = *only;
        C1->tree->root = save;
        lc_poolfree(&C1->tree->S->nodes, only);
        C1->tree->levels--;
    }
}

/* lcD_freerange: dual-cursor recursive range deletion.
 * Processes one fork level; recurses into subtrees when l < levels.
 * Sets *no_break_boundary when right leaf's first segment was partially
 * consumed (its leading break is gone). No while loop — one pass only. */
static void lcD_freerange(
        lc_Cursor *C1, lc_Cursor *C2, size_t *pdel, int *no_break_boundary) {
    unsigned levels = lcK_levels(C1);
    unsigned l;

    for (l = 0; l <= levels; l++) {
        lc_Node *p = lcK_parent(C1, l);
        unsigned i1 = (unsigned)lcK_idx(C1, p, l);
        unsigned i2 = (unsigned)lcK_idx(C2, p, l);

        if (i1 == i2) continue; /* same child: descend */

        /* ---- fork at level l ---- */
        fprintf(stderr, "DBG freerange FORK l=%u i1=%u i2=%u *pdel=%zu\n", l,
                i1, i2, *pdel);

        /* 1. RIGHT side: child[i2] subtree */
        {
            size_t right_bytes, right_orig;
            if (l == levels) {
                right_bytes = C2->loff + C2->col;
                right_orig = right_bytes;
                if (no_break_boundary)
                    *no_break_boundary = lcD_clearleaf(C2, &right_bytes, 1);
                *pdel -= (right_orig - right_bytes);
                fprintf(stderr,
                        "DBG freerange RIGHT leaf: l=%u right_orig=%zu "
                        "right_bytes=%zu *pdel=%zu\n",
                        l, right_orig, right_bytes, *pdel);
            } else {
                /* internal-level right side: build cursor at start of
                 * child[i2]'s subtree, recurse */
                lc_Cursor Cr = *C1;
                unsigned  dl;
                Cr.paths[l] = &p->children[i2];
                for (dl = l + 1; dl <= levels; dl++)
                    Cr.paths[dl] = &(*Cr.paths[dl - 1])->children[0];
                Cr.lidx = 0;
                Cr.loff = 0;
                Cr.col = 0;
                /* right_bytes = C2 offset within child[i2] subtree */
                right_bytes = 0;
                for (dl = l + 1; dl <= levels; dl++) {
                    lc_Node *np = *(C2->paths[dl - 1]);
                    unsigned idx = (unsigned)(C2->paths[dl] - np->children);
                    right_bytes += lcN_sumbytes(np, 0, (int)idx);
                }
                right_bytes += C2->loff + C2->col;
                right_orig = right_bytes;
                fprintf(stderr,
                        "DBG freerange RIGHT internal: l=%u right_orig=%zu "
                        "same_leaf=%d\n",
                        l, right_orig, Cr.paths[levels] == C2->paths[levels]);
                if (Cr.paths[levels] == C2->paths[levels]) {
                    if (no_break_boundary)
                        *no_break_boundary = lcD_clearleaf(C2, &right_bytes, 1);
                } else {
                    lcD_freerange(&Cr, C2, &right_bytes, no_break_boundary);
                }
                *pdel -= (right_orig - right_bytes);
                p->bytes[i2] = lcN_sumbytes(
                        p->children[i2], 0, (int)p->children[i2]->child_count);
                p->breaks[i2] = lcN_sumbreaks(
                        p->children[i2], 0, (int)p->children[i2]->child_count);
                fprintf(stderr,
                        "DBG freerange RIGHT internal done: right_bytes=%zu "
                        "*pdel=%zu\n",
                        right_bytes, *pdel);
            }
        }

        /* 2. MIDDLE: children [i1+1, i2-1] — free entirely */
        if (i2 > i1 + 1) {
            size_t   mid_bytes = 0, mid_breaks = 0;
            unsigned mi;
            for (mi = i1 + 1; mi < i2; mi++) {
                mid_bytes += p->bytes[mi];
                mid_breaks += p->breaks[mi];
            }
            lcD_shrink(C1, l, i1 + 1, i2);
            *pdel -= mid_bytes;
            C1->tree->bytes -= mid_bytes;
            C1->tree->breaks -= mid_breaks;
        }

        /* 3. LEFT side: child[i1] subtree */
        {
            size_t left_bytes;
            if (l == levels) {
                left_bytes = p->bytes[i1] - (C1->loff + C1->col);
                fprintf(stderr,
                        "DBG freerange LEFT leaf: l=%u left_bytes=%zu\n", l,
                        left_bytes);
                if (left_bytes > 0) {
                    size_t left_orig = left_bytes;
                    lcD_clearleaf(C1, &left_bytes, 0);
                    *pdel -= (left_orig - left_bytes);
                    fprintf(stderr,
                            "DBG freerange LEFT leaf done: left_bytes=%zu "
                            "*pdel=%zu\n",
                            left_bytes, *pdel);
                }
            } else {
                /* internal-level left side: C1 to end of child[i1] subtree */
                lc_Cursor C1e = *C1;
                unsigned  dl;
                size_t    prefix = 0;
                int       dummy_nb = 0;
                /* compute offset of C1 within child[i1] subtree */
                for (dl = l + 1; dl <= levels; dl++) {
                    lc_Node *np = *(C1->paths[dl - 1]);
                    unsigned idx = (unsigned)(C1->paths[dl] - np->children);
                    prefix += lcN_sumbytes(np, 0, (int)idx);
                }
                prefix += C1->loff + C1->col;
                left_bytes = p->bytes[i1] - prefix;
                fprintf(stderr,
                        "DBG freerange LEFT internal: l=%u left_bytes=%zu\n", l,
                        left_bytes);
                if (left_bytes > 0) {
                    size_t left_orig = left_bytes;
                    /* C1e = end of child[i1] subtree */
                    C1e.paths[l] = &p->children[i1];
                    for (dl = l + 1; dl <= levels; dl++) {
                        lc_Node *child = *(C1e.paths[dl - 1]);
                        C1e.paths[dl] = &child->children
                                                 [child->child_count - 1];
                    }
                    {
                        lc_Node *leaf_p = lcK_parent(&C1e, levels);
                        int      leaf_i = lcK_idx(&C1e, leaf_p, levels);
                        C1e.lidx = (unsigned short)leaf_p->breaks[leaf_i];
                        C1e.loff = leaf_p->bytes[leaf_i];
                        C1e.col = 0;
                    }
                    if (C1->paths[levels] == C1e.paths[levels]) {
                        lcD_clearleaf(C1, &left_bytes, 0);
                    } else {
                        lcD_freerange(C1, &C1e, &left_bytes, &dummy_nb);
                    }
                    *pdel -= (left_orig - left_bytes);
                    p->bytes[i1] = lcN_sumbytes(
                            p->children[i1], 0,
                            (int)p->children[i1]->child_count);
                    p->breaks[i1] = lcN_sumbreaks(
                            p->children[i1], 0,
                            (int)p->children[i1]->child_count);
                }
            }
        }

        return; /* one fork processed, done */
    }
}

static void lcD_checknode(
        const lc_Node *n, int l, int levels, const char *tag) {
    int i;
    if (levels < 0) return;
    if (l == levels) {
        /* leaf level: n->children[i] are lc_Leaf*; breaks[] is segment count */
        for (i = 0; i < (int)n->child_count; i++) {
            lc_Leaf *lf = (lc_Leaf *)n->children[i];
            if (n->breaks[i] > LC_LEAF_FANOUT)
                fprintf(stderr,
                        "CHK %s LEAF BAD BREAKS l=%d i=%d node=%p leaf=%p "
                        "breaks=%zu\n",
                        tag, l, i, (void *)n, (void *)lf, n->breaks[i]);
        }
    } else {
        for (i = 0; i < (int)n->child_count; i++) {
            lc_Node *ch = n->children[i];
            size_t   sb = lcN_sumbytes(ch, 0, (int)ch->child_count);
            size_t   sg = lcN_sumbreaks(ch, 0, (int)ch->child_count);
            if (n->bytes[i] != sb || n->breaks[i] != sg)
                fprintf(stderr,
                        "CHK %s MISMATCH l=%d i=%d node=%p child=%p: "
                        "bytes[%d]=%zu sum=%zu, breaks[%d]=%zu sum=%zu cc=%u\n",
                        tag, l, i, (void *)n, (void *)ch, i, n->bytes[i], sb, i,
                        n->breaks[i], sg, ch->child_count);
            if (ch->child_count > LC_FANOUT)
                fprintf(stderr,
                        "CHK %s BAD CC l=%d i=%d node=%p child=%p cc=%u\n", tag,
                        l, i, (void *)n, (void *)ch, ch->child_count);
            lcD_checknode(ch, l + 1, levels, tag);
        }
    }
}

static void lcD_checktree(const lc_Cache *c, const char *tag) {
    fprintf(stderr, "CHK %s: levels=%u root.cc=%u bytes=%zu breaks=%zu\n", tag,
            c->levels, c->root.child_count, c->bytes, c->breaks);
    if (c->root.child_count > 0 && c->levels > 0)
        lcD_checknode(&c->root, 0, (int)c->levels, tag);
}

LC_API void lc_splice(lc_Cursor *C, size_t del, size_t ins) {
    lc_Node *parent;
    lc_Leaf *leaf;
    size_t   remaining;
    unsigned leaf_i;
    if (C == NULL || C->tree == NULL || (del == 0 && ins == 0)) return;
    if (lcK_levels(C) == 0 && C->tree->root.child_count == 0) return;
    remaining = C->tree->bytes - lc_offset(C);
    if (del > remaining) del = remaining;
    if (del == 0 && ins == 0) return;
    parent = lcK_parent(C, lcK_levels(C));

    /* step 1: delete — same-leaf fast path or cross-leaf splicerange */
    if (C->loff + C->col + del
        <= parent->bytes[lcK_idx(C, parent, lcK_levels(C))]) {
        lcD_spliceleaf(C, &del);
        lcD_mergeleaf(C, 0);
        if (lcK_levels(C) > 0) lcD_merge(C, (int)lcK_levels(C) - 1);
    } else {
        lc_Cursor C2 = *C;
        lc_advance(&C2, (ptrdiff_t)del);
        lcD_splicerange(C, &C2);
    }

    /* step 2: empty tree reset */
    if (C->tree->bytes == 0 && C->tree->breaks == 0) {
        memset(&C->tree->root, 0, sizeof(lc_Node));
        C->tree->levels = 0;
        C->col = (unsigned)ins;
        return;
    }

    /* step 3: insert */
    if (ins == 0) return;
    parent = lcK_parent(C, lcK_levels(C));
    leaf_i = (unsigned)lcK_idx(C, parent, lcK_levels(C));
    leaf = (lc_Leaf *)parent->children[leaf_i];
    if (C->lidx == parent->breaks[leaf_i]) {
        C->lidx--, C->col = leaf->bytes[C->lidx];
        C->loff -= C->col;
    }
    leaf->bytes[C->lidx] = (unsigned)((ptrdiff_t)leaf->bytes[C->lidx]
                                      + (ptrdiff_t)ins);
    lcK_upmetrics(C, lcK_levels(C), (ptrdiff_t)ins, 0);
    C->col += ins;
}

LC_API int lc_clearbreaks(lc_Cursor *C, size_t len) {
    if (C == NULL || C->tree == NULL) return LC_ERRPARAM;
    lc_splice(C, len, len);
    return LC_OK;
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
        unsigned extra = (unsigned)(br - remain);
        lc_advance(C, (ptrdiff_t)remain);
        lc_splice(C, extra, 0);
        if (C->tree->root.child_count == 0) return lcB_initempty(C, br);
        if (need > C->tree->bytes) {
            ptrdiff_t ext = (ptrdiff_t)(need - C->tree->bytes);
            lcK_upmetrics(C, lcK_levels(C), ext, 0);
            C->tree->bytes = need;
        }
        r = lc_seek(C, C->tree, need);
        if (r != LC_OK) return r;
        if ((r = lcB_fitleaf(C)) < 0) return r;
        lcB_putbreak(C, 0);
        C->loff += C->col, C->idx++, C->lidx++, C->col = 0;
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

LC_NS_END

#endif /* LC_IMPLEMENTATION */
