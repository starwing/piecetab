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
#define LC_AGAIN    (-4)

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
LC_API int lc_clearbreaks(lc_Cursor *C, size_t len);

/* insert texts */
LC_API int lc_insert(lc_Cursor *C, int e, lc_Scanner *scanner, void *ud);

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

static void lcN_freechildren(lc_State *S, lc_Node *p, int rl) {
    int n;
    if (rl == 0) {
        for (n = 0; n < p->child_count; ++n)
            lc_poolfree(&S->leaves, p->children[n]);
    } else {
        for (n = 0; n < p->child_count; ++n) {
            lc_Node *child = p->children[n];
            lcN_freechildren(S, child, rl - 1);
            lc_poolfree(&S->nodes, child);
        }
    }
}

static void lcN_freerange(lc_State *S, lc_Node *p, int rl, int s, int e) {
    int i;
    if (rl == 0) {
        for (i = s; i < e; ++i) lc_poolfree(&S->leaves, p->children[i]);
    } else {
        for (i = s; i < e; ++i) {
            lc_Node *child = p->children[i];
            lcN_freechildren(S, child, rl - 1);
            lc_poolfree(&S->nodes, child);
        }
    }
}

LC_API void lc_deltree(lc_State *S, lc_Cache *c) {
    lcN_freechildren(S, &c->root, c->levels);
    S->allocf(S->alloc_ud, c, sizeof(lc_Cache), 0);
}

/* cursor macros */

#define lcK_levels(C) ((int)(C)->tree->levels)
#define lcK_breaks(C) ((C)->tree->breaks)
#define lcK_bytes(C)  ((C)->tree->bytes)

#define lcK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define lcK_idx(C, p, l) ((int)((C)->paths[(l)] - (p)->children))
#define lcK_leaf(C)      (*(lc_Leaf **)(C)->paths[lcK_levels(C)])

#define lcN_tx(p, d, s, x) (p) = &(d)->children[((p) - (s)->children) + (x)]

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

/* simple queries */

LC_API size_t lc_breaks(const lc_Cache *c) { return c->breaks; }
LC_API size_t lc_bytes(const lc_Cache *c) { return c->bytes; }

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
    int      l;
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

static void lcM_up(lc_Cursor *C, int l, lc_Diff db, lc_Diff dl) {
    for (; l >= 0; --l) {
        lc_Node *p = lcK_parent(C, l);
        int      i = lcK_idx(C, p, l);
        p->bytes[i] += db, p->breaks[i] += dl;
    }
    C->tree->bytes += db, C->tree->breaks += dl;
}

static void lcM_tx(lc_Cursor *L, lc_Cursor *R, int l, lc_Diff db, lc_Diff dl) {
    if (lcK_parent(L, l) != lcK_parent(R, l))
        lcM_up(L, l, db, dl), lcM_up(R, l, -db, -dl);
    else {
        lc_Node *pl = lcK_parent(L, l), *pr = lcK_parent(R, l);
        int      il = lcK_idx(L, pl, l), ir = lcK_idx(R, pr, l);
        pl->bytes[il] += db, pl->breaks[il] += dl;
        pr->bytes[ir] -= db, pr->breaks[ir] -= dl;
    }
}

static void lcD_prune(lc_Cursor *L, int sr, int l) {
    lc_Node *p = lcK_parent(L, l);
    int      i = lcK_idx(L, p, l);
    lc_Diff  db, dl;
    if (i + 1 >= sr) return;
    db = lcN_sumbytes(p, i + 1, sr), dl = lcN_sumbreaks(p, i + 1, sr);
    lcN_freerange(L->tree->S, p, lcK_levels(L) - l, i + 1, sr);
    lcN_move(p, i + 1, sr, p->child_count - sr);
    p->child_count -= (sr - i) - 1;
    if (db && dl) lcM_up(L, l - 1, -db, -dl);
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
    lcN_freerange(C->tree->S, p, lcK_levels(C) - l, s, e);
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
        lcM_tx(L, R, l, pr->bytes[ir], cr);
        return (R->paths[l] += 1), 1;
    }
    db = lcD_balanceleaf((lc_Leaf **)&pl->children[il], cl, cr, R->lidx);
    return R->lidx = 0, lcM_tx(L, R, l, db, ((cl + cr + 1) >> 1) - cl), 0;
}

static int lcD_balancenode(lc_Node **ns, int s, int left, lc_Diff ds[2]) {
    int d, l = ns[0]->child_count, r = ns[1]->child_count;
    if ((d = l - ((l + r + (left != 0)) >> 1)) == 0) return 0;
    if (d < 0) {
        lcN_copy(ns[0], l, ns[1], s, -d), lcN_move(ns[1], 0, s - d, r + d);
        ds[0] = (lc_Diff)lcN_sumbytes(ns[0], l, l - d);
        ds[1] = (lc_Diff)lcN_sumbreaks(ns[0], l, l - d);
    } else {
        lcN_move(ns[1], d, s, r), lcN_copy(ns[1], 0, ns[0], l - d, d);
        ds[0] = -(lc_Diff)lcN_sumbytes(ns[1], 0, d);
        ds[1] = -(lc_Diff)lcN_sumbreaks(ns[1], 0, d);
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
        lcM_tx(L, R, l, p->bytes[i], p->breaks[i]);
        ns[0]->child_count += ns[1]->child_count, ns[1]->child_count = 0;
        return (R->paths[l] += 1), 1;
    }
    if (lcD_balancenode(ns, ir, 1, ds)) lcM_tx(L, R, l, ds[0], ds[1]);
    return 0;
}

static int lcD_foldleaf(lc_Cursor *C) {
    lc_Node  *p = lcK_parent(C, lcK_levels(C));
    int       cl, cr, i = lcK_idx(C, p, lcK_levels(C));
    lc_Leaf **ls = (lc_Leaf **)&p->children[i], *o = *ls;
    lc_Diff   db, dl, di = 0;
    if (p->child_count <= 1) return 0;
    if (i == p->child_count - 1) ls -= 1, i -= 1;
    cl = (int)p->breaks[i], cr = (int)p->breaks[i + 1];
    if (cl + cr <= LC_LEAF_FANOUT) {
        memcpy(ls[0]->bytes + cl, ls[1]->bytes, cr * sizeof(unsigned));
        p->breaks[i] += cr, p->breaks[i + 1] = 0;
        p->bytes[i] += p->bytes[i + 1], p->bytes[i + 1] = 0;
        if (*ls != o) C->paths[lcK_levels(C)] = &p->children[i], C->lidx += cl;
        return lcD_prune(C, i + 2, lcK_levels(C)), 1;
    }
    dl = cl - ((cl + cr + 1) / 2);
    db = lcD_balanceleaf((lc_Leaf **)&p->children[i], cl, cr, 0);
    p->breaks[i] -= dl, p->breaks[i + 1] += dl;
    p->bytes[i] -= db, p->bytes[i + 1] += db;
    if (dl < 0 && *ls != o && (int)C->lidx < -dl)
        di = cl, C->paths[lcK_levels(C)] = &p->children[i];
    else if (dl > 0 && *ls == o && (int)C->lidx >= cl - dl)
        di = dl - cl, C->paths[lcK_levels(C)] = &p->children[i + 1];
    else if (*ls != o)
        di = dl;
    return (C->lidx += di), 0;
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
        if (*ns != o) C->paths[l] = ns, C->paths[l + 1] += cl + (ns[0] - ns[1]);
        return lcD_prune(C, i + 2, l), 1;
    }
    if (!lcD_balancenode(ns, 0, (*ns == o), ds)) return 0;
    p->bytes[i] += ds[0], p->bytes[i + 1] -= ds[0];
    p->breaks[i] += ds[1], p->breaks[i + 1] -= ds[1];
    dn = cl - ((cl + cr + 1) / 2);
    if (dn < 0 && *ns != o && C->paths[l + 1] - ns[1]->children < -dn)
        lcN_tx(C->paths[l + 1], ns[0], ns[1], cl), C->paths[l] = ns;
    else if (dn > 0 && *ns == o && C->paths[l + 1] - ns[0]->children >= cl - dn)
        lcN_tx(C->paths[l + 1], ns[1], ns[0], dn - cl), C->paths[l] = ns + 1;
    else if (*ns != o)
        C->paths[l + 1] += dn;
    return 0;
}

static void lcD_rebalance(lc_Cursor *C, int l) {
    assert(l < lcK_levels(C));
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
        lc_poolfree(&C->tree->S->nodes, only);
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
    if (l == 0 && lcK_levels(L) > 0 && p->child_count == 1) lcD_rebalance(L, 0);
    if (l > 0 && lcD_foldnode(L, l - 1)) lcD_rebalance(L, l - 1);
    for (dl = l; dl < lcK_levels(L); ++dl) lcD_foldnode(L, dl);
    lcD_foldleaf(L);
}

static void lcD_emptytree(lc_Cursor *C, size_t ins) {
    memset(&C->tree->root, 0, sizeof(lc_Node));
    C->tree->levels = C->tree->bytes = C->tree->breaks = 0;
    C->col += (unsigned)ins;
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
    lc_splice(C, len, len);
    return LC_OK;
}

/* insertion */

static int lcB_initempty(lc_Cursor *C, unsigned br) {
    lc_Cache *c = C->tree;
    lc_Leaf  *leaf = (lc_Leaf *)lc_poolalloc(c->S, &c->S->leaves);
    if (!leaf) return LC_ERRMEM;
    leaf->bytes[0] = br;
    c->root.children[0] = (lc_Node *)leaf;
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
        unsigned nx = C->paths[l + 1] - old->children - mid;
        C->paths[l + 1] = &n->children[nx];
        C->paths[l] = &p->children[i + 1];
    }
}

static void lcB_splitleaf(lc_Cursor *C, lc_Leaf *prealloc) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    int      i = lcK_idx(C, p, lcK_levels(C));
    size_t   obytes, oldb, newb, n, mid = p->breaks[i] / 2;
    lc_Leaf *old = (lc_Leaf *)p->children[i];
    lc_Leaf *new = prealloc;
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
    }
}

static int lcB_makeroom(lc_Cursor *C) {
    lc_State *S = C->tree->S;
    lc_Pool  *np = &S->nodes, *lp = &S->leaves;
    int       count = 0, used = 0, l;
    lc_Node  *nodes[LC_MAX_LEVEL + 2];
    lc_Leaf  *leaf;
    for (l = lcK_levels(C);
         l >= 0 && lcK_parent(C, l)->child_count >= LC_FANOUT; --l)
        count += (l == 0) + 1;
    for (l = 0; l < count; ++l)
        if (!(nodes[l] = (lc_Node *)lc_poolalloc(S, np))) return LC_ERRMEM;
    if (!(leaf = (lc_Leaf *)lc_poolalloc(S, lp))) return LC_ERRMEM;
    for (l = lcK_levels(C); l >= 0; --l)
        if (lcK_parent(C, l)->child_count < LC_FANOUT) break;
    if (l < 0) {
        assert(used + 2 <= count);
        lcB_splitroot(C, nodes[used], nodes[used + 1]), used += 2, l = 1;
    }
    for (; l < lcK_levels(C); ++l)
        assert(used < count), lcB_splitchild(C, l, nodes[used++]);
    lcB_splitleaf(C, leaf);
    while (used < count) lc_poolfree(&S->nodes, nodes[used++]);
    return LC_OK;
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

/* bulk loading */

typedef struct lcB_Ctx {
    lc_Cursor      c;
    lc_Node        pend[LC_MAX_LEVEL];
    lc_Node       *pend_root;
    unsigned short at[LC_MAX_LEVEL];
} lcB_Ctx;

static int lcB_checkpendroot(lcB_Ctx *x) {
    if (x->pend_root) return LC_OK;
    x->pend_root = (lc_Node *)lc_poolalloc(x->c.tree->S, &x->c.tree->S->nodes);
    return x->pend_root ? LC_OK : LC_ERRMEM;
}

static int lcB_initctx(lcB_Ctx *x, lc_Cursor *C, int trailing) {
    int l, lv;
    memset(x, 0, sizeof(lcB_Ctx));
    x->c = *C;
    lv = lcK_levels(&x->c);
    if (trailing) {
        for (l = 0; l <= lv; ++l)
            x->at[l] = (unsigned short)lcK_parent(&x->c, l)->child_count;
    } else {
        for (l = 0; l <= lv; ++l)
            x->at[l] = (unsigned short)(lcK_idx(&x->c, lcK_parent(&x->c, l), l)
                                        + 1);
    }
    return LC_OK;
}

static void lcB_merge(lcB_Ctx *x, int l, lc_Node *next) {
    lc_Node *parent = lcK_parent(&x->c, l);
    lc_Node *pend = &x->pend[l];
    int      at = (int)x->at[l];
    int      space = LC_FANOUT - (int)parent->child_count;
    int      n = lc_min(space, (int)pend->child_count);
    int      pcc = (int)parent->child_count;
    lc_Diff  db, dl;
    if (at < pcc) lcN_makespace(parent, (unsigned)at, (unsigned)n);
    lcN_copy(
            parent, at < pcc ? (unsigned)at : (unsigned)pcc, pend, 0,
            (unsigned)n);
    parent->child_count = (unsigned short)(pcc + n);
    fprintf(stderr, "lcB_merge l=%d at=%d pcc=%d n=%d cc=%d -> %d\n", l, at,
            pcc, n, pcc, pcc + n);
    x->c.paths[l] = &parent->children[(at < pcc ? at : pcc) + n - 1];
    db = (lc_Diff)lcN_sumbytes(pend, 0, n);
    dl = (lc_Diff)lcN_sumbreaks(pend, 0, n);
    if (l > 0)
        lcM_up(&x->c, l - 1, db, dl);
    else
        x->c.tree->bytes += (size_t)db, x->c.tree->breaks += (size_t)dl;
    if (next && n < (int)pend->child_count) {
        int rest = (int)pend->child_count - n;
        lcN_copy(next, 0, pend, n, (unsigned)rest);
        next->child_count = (unsigned short)rest;
    }
    pend->child_count = 0;
    x->at[l] += (unsigned short)n;
}

static int lcB_rootpush(lcB_Ctx *x, lc_Node *nr) {
    lc_Cache *c = x->c.tree;
    int       l = (int)c->levels, i;
    lc_Node  *nl = (assert(x->pend_root != NULL), x->pend_root);
    if (l + 1 >= LC_MAX_LEVEL) return LC_ERRPARAM;
    i = (int)(x->c.paths[0] - c->root.children);
    *nl = c->root;
    c->root.children[0] = nl, c->root.children[1] = nr;
    c->root.child_count = 2;
    c->root.bytes[0] = c->bytes, c->root.breaks[0] = c->breaks;
    c->root.bytes[1] = lcN_sumbytes(nr, 0, (int)nr->child_count);
    c->root.breaks[1] = lcN_sumbreaks(nr, 0, (int)nr->child_count);
    c->bytes += c->root.bytes[1], c->breaks += c->root.breaks[1];
    memmove(x->c.paths + 1, x->c.paths, (size_t)(l + 1) * sizeof(lc_Node **));
    x->c.paths[0] = &c->root.children[i >= (int)nl->child_count];
    x->c.paths[1] = &nl->children[i];
    c->levels++, x->pend_root = NULL;
    memmove(x->at + 1, x->at, (size_t)(l + 1) * sizeof(unsigned short));
    return (x->pend[c->levels].child_count = 0), LC_OK;
}

/* fill a single leaf from scanner. start = initial break index in lf.
 * *pbytes = running scanner position (in/out). returns 0=done, 1=full. */
static int lcB_fill(
        lcB_Ctx *x, lc_Leaf *lf, int start, lc_Scanner *sc, void *ud,
        size_t *pbytes) {
    size_t bytes = *pbytes;
    int    i;
    (void)x;
    for (i = start; i < (int)LC_LEAF_FANOUT; ++i) {
        unsigned br = sc(ud, bytes);
        if (!br) break;
        lf->bytes[i] = br, bytes += br;
    }
    *pbytes = bytes;
    return (i >= (int)LC_LEAF_FANOUT) ? 1 : 0;
}

/* flush single layer: if parent has room, merge pend[l] directly.
 * if parent full and l==0, rootpush. otherwise return LC_AGAIN for packup. */
static int lcB_flushone(lcB_Ctx *x, int l) {
    lc_Node *parent = lcK_parent(&x->c, l);
    lc_Node *pend = &x->pend[l];
    lc_Node *nr;
    if (pend->child_count == 0) return LC_OK;
    fprintf(stderr, "flushone l=%d pend_cc=%d parent_cc=%d\n", l,
            (int)pend->child_count, (int)parent->child_count);
    if (parent->child_count + pend->child_count <= LC_FANOUT) {
        lcB_merge(x, l, NULL);
        return LC_OK;
    }
    if (l == 0) {
        nr = (lc_Node *)lc_poolalloc(x->c.tree->S, &x->c.tree->S->nodes);
        if (!nr) return LC_ERRMEM;
        memset(nr, 0, sizeof(lc_Node));
        lcN_copy(nr, 0, pend, 0, pend->child_count);
        nr->child_count = pend->child_count;
        pend->child_count = 0;
        return lcB_rootpush(x, nr);
    }
    return LC_AGAIN;
}

/* pack pend[l] children into pend[l-1]. pend[l] must be non-empty. */
static int lcB_packup(lcB_Ctx *x, int l) {
    lc_Node *src = &x->pend[l], *dst = &x->pend[l - 1];
    int      j;
    for (j = 0; j < (int)src->child_count; ++j) {
        lc_Node *last;
        int      lidx;
        if (dst->child_count > 0)
            last = dst->children[dst->child_count - 1];
        else
            last = NULL;
        if (!last || last->child_count >= LC_FANOUT) {
            lc_Node *nn;
            if (!(nn = (lc_Node *)lc_poolalloc(
                          x->c.tree->S, &x->c.tree->S->nodes)))
                return LC_ERRMEM;
            memset(nn, 0, sizeof(lc_Node));
            dst->children[dst->child_count] = nn;
            dst->bytes[dst->child_count] = 0;
            dst->breaks[dst->child_count] = 0;
            dst->child_count++, last = nn;
        }
        lidx = (int)last->child_count;
        last->children[lidx] = src->children[j];
        last->bytes[lidx] = src->bytes[j];
        last->breaks[lidx] = src->breaks[j];
        last->child_count++;
        lidx = (int)dst->child_count - 1;
        dst->bytes[lidx] = lcN_sumbytes(last, 0, (int)last->child_count);
        dst->breaks[lidx] = lcN_sumbreaks(last, 0, (int)last->child_count);
    }
    return src->child_count = 0, LC_OK;
}

static int lcB_fillflush(lcB_Ctx *x, lc_Scanner *sc, void *ud, int lv) {
    lc_Cache *c = x->c.tree;
    lc_Node  *pend;
    lc_Leaf  *lf;
    int       r, l;
    size_t    bytes = c->bytes;
    (void)lv;
    pend = &x->pend[(int)c->levels];
    for (;;) {
        if (pend->child_count >= LC_FANOUT) {
            l = (int)c->levels;
            for (;;) {
                if (x->pend[l].child_count == 0) {
                    --l;
                    if (l < 0)
                        break;
                    else
                        continue;
                }
                if ((r = lcB_checkpendroot(x)) != LC_OK) return r;
                r = lcB_flushone(x, l);
                if (r == LC_AGAIN) r = lcB_packup(x, l);
                if (r != LC_OK) return r;
            }
        }
        pend = &x->pend[(int)c->levels];
        {
            unsigned bi, sum = 0;
            lf = (lc_Leaf *)lc_poolalloc(c->S, &c->S->leaves);
            if (!lf) return LC_ERRMEM;
            memset(lf, 0, sizeof(lc_Leaf));
            r = lcB_fill(x, lf, 0, sc, ud, &bytes);
            for (bi = 0; bi < LC_LEAF_FANOUT && lf->bytes[bi]; ++bi)
                sum += lf->bytes[bi];
            if (bi == 0) {
                lc_poolfree(&c->S->leaves, lf);
                break;
            }
            pend->children[pend->child_count] = (lc_Node *)lf;
            pend->bytes[pend->child_count] = sum;
            pend->breaks[pend->child_count] = bi;
            pend->child_count++;
        }
        if (!r) break;
    }
    /* final flush: drain all pend layers */
    for (l = (int)c->levels; l >= 0; --l) {
        pend = &x->pend[l];
        if (pend->child_count == 0) continue;
        fprintf(stderr, "final_flush: l=%d pend_cc=%d root_cc=%d\n", l,
                (int)pend->child_count, (int)c->root.child_count);
        if ((r = lcB_checkpendroot(x)) != LC_OK) break;
        r = lcB_flushone(x, l);
        if (r == LC_AGAIN) r = lcB_packup(x, l);
        if (r != LC_OK || pend->child_count > 0) break;
    }
    return r;
}

LC_API int lc_scan(lc_Cache *c, lc_Scanner *scanner, void *ud) {
    lcB_Ctx   x;
    lc_Cursor cur;
    int       r, i;
    if (c == NULL || scanner == NULL) return LC_ERRPARAM;
    memset(&cur, 0, sizeof(cur));
    cur.tree = c;
    if (c->root.child_count > 0)
        lcK_locend(&cur);
    else
        cur.paths[0] = &c->root.children[0];
    if ((r = lcB_initctx(&x, &cur, 1)) != LC_OK) return r;
    r = lcB_fillflush(&x, scanner, ud, (int)c->levels);
    fprintf(stderr, "lc_scan: r=%d bytes=%zu breaks=%zu lv=%d cc=%d\n", r,
            c->bytes, c->breaks, (int)c->levels, (int)c->root.child_count);
    if (x.pend_root) lc_poolfree(&c->S->nodes, x.pend_root);
    if (r != LC_OK)
        for (i = 0; i <= (int)c->levels; ++i)
            lcN_freechildren(c->S, &x.pend[i], (int)c->levels - i);
    return r;
}

/* ================================================================
 *  lc_insert — symmetric split: left → pend, right → tree
 * ================================================================ */

/* split leaf at cursor: left half → pend[lv], right half → tree.
 * returns n (>0) = right-half break count, or negative = error. */
static int lcB_splitleafpend(lcB_Ctx *x, size_t *old_b_p, size_t *old_l_p) {
    lc_Cursor *C = &x->c;
    int        lv = lcK_levels(C);
    lc_Node   *p = lcK_parent(C, lv);
    int        li = lcK_idx(C, p, lv), count = (int)p->breaks[li];
    int        n = count - C->lidx, i, dl;
    lc_Leaf   *lf = (lc_Leaf *)p->children[li], *rt;
    lc_Diff    db;
    lc_Node   *pend;
    if (n <= 0) return 0;
    *old_b_p = p->bytes[li], *old_l_p = p->breaks[li];
    rt = (lc_Leaf *)lc_poolalloc(C->tree->S, &C->tree->S->leaves);
    if (!rt) return LC_ERRMEM;
    memset(rt, 0, sizeof(lc_Leaf));
    if (C->col) {
        unsigned orig = lf->bytes[C->lidx];
        rt->bytes[0] = orig - C->col;
        for (i = 1; i < n; ++i) rt->bytes[i] = lf->bytes[C->lidx + i];
        lf->bytes[C->lidx] = C->col;
        db = (lc_Diff)(orig - C->col + lcL_sumbytes(lf, C->lidx + 1, count));
        dl = n - 1;
    } else {
        for (i = 0; i < n; ++i) rt->bytes[i] = lf->bytes[C->lidx + i];
        db = (lc_Diff)lcL_sumbytes(lf, C->lidx, count), dl = n;
    }
    pend = &x->pend[lv];
    pend->children[0] = (lc_Node *)lf;
    pend->bytes[0] = *old_b_p - (size_t)db;
    pend->breaks[0] = *old_l_p - (size_t)dl;
    pend->child_count = 1;
    p->children[li] = (lc_Node *)rt;
    p->bytes[li] = (size_t)db, p->breaks[li] = (size_t)dl;
    lcM_up(C, lv - 1, -(lc_Diff)(pend->bytes[0]), -(lc_Diff)(pend->breaks[0]));
    return (x->at[lv] = (unsigned short)li), n;
}

/* reverse a leaf split: free pend leaves, restore original leaf to tree. */
static void lcB_unsplitleafat(lcB_Ctx *x, size_t ob, size_t ol) {
    lc_Cursor *C = &x->c;
    int        lv = lcK_levels(C);
    lc_Node   *p = lcK_parent(C, lv);
    int        li = (int)x->at[lv];
    lc_Leaf   *rt = (lc_Leaf *)p->children[li];
    size_t     rt_b = p->bytes[li], rt_l = p->breaks[li];
    lcN_freechildren(C->tree->S, &x->pend[lv], 0);
    x->pend[lv].child_count = 0;
    p->children[li] = (lc_Node *)x->pend[lv].children[0];
    x->pend[lv].children[0] = NULL;
    p->bytes[li] = ob, p->breaks[li] = ol;
    lcM_up(C, lv - 1, (lc_Diff)(ob - rt_b), (lc_Diff)(ol - rt_l));
    lc_poolfree(&C->tree->S->leaves, rt);
}

LC_API int lc_insert(lc_Cursor *C, int e, lc_Scanner *scanner, void *ud) {
    lcB_Ctx   x;
    lc_Cache *c;
    size_t    old_off, old_bytes, bytes;
    int       r, lv, trailing, i;
    if (C == NULL || (c = C->tree) == NULL || scanner == NULL)
        return LC_ERRPARAM;
    old_off = lc_offset(C), old_bytes = c->bytes;
    trailing = (old_off >= old_bytes || c->root.child_count == 0);
    if ((r = lcB_initctx(&x, C, trailing)) != LC_OK) return r;
    lv = lcK_levels(&x.c);

    if (!trailing) {
        size_t   old_b = 0, old_l = 0;
        lc_Node *p;
        r = lcB_splitleafpend(&x, &old_b, &old_l);
        if (r < 0) {
            r = LC_ERRMEM;
            goto cleanup;
        }
        p = lcK_parent(&x.c, lv);
        if (r == 0) {
            /* cursor at end of leaf — nothing to split, just add e */
            if (e > 0 && C->lidx < (int)p->breaks[lcK_idx(&x.c, p, lv)]) {
                lcK_leaf(&x.c)->bytes[x.c.lidx] += (unsigned)e;
                x.c.col += (unsigned)e;
                *C = x.c;
            }
            return LC_OK;
        }
        /* add e to right-half leaf's last line */
        if (e > 0) {
            int      li2 = (int)x.at[lv];
            lc_Leaf *rt = (lc_Leaf *)p->children[li2];
            unsigned last = (unsigned)p->breaks[li2];
            if (last > 0) {
                rt->bytes[last - 1] += (unsigned)e;
                p->bytes[li2] += (size_t)e;
                lcM_up(&x.c, lv - 1, (lc_Diff)e, 0);
            }
        }
        bytes = x.pend[lv].bytes[0];
    } else {
        lc_Node *pend = &x.pend[lv];
        lc_Leaf *first;
        first = (lc_Leaf *)lc_poolalloc(c->S, &c->S->leaves);
        if (!first) return LC_ERRMEM;
        memset(first, 0, sizeof(lc_Leaf));
        pend->children[0] = (lc_Node *)first;
        pend->bytes[0] = 0, pend->breaks[0] = 0, pend->child_count = 1;
        bytes = c->bytes;
    }

    /* fill loop */
    {
        lc_Node *pend = &x.pend[lv];
        lc_Leaf *cur;
        int      start;
        cur = (lc_Leaf *)pend->children[pend->child_count - 1];
        start = (int)pend->breaks[pend->child_count - 1];
        for (;;) {
            unsigned bi, sum;
            r = lcB_fill(&x, cur, start, scanner, ud, &bytes);
            for (sum = 0, bi = 0; bi < LC_LEAF_FANOUT && cur->bytes[bi]; ++bi)
                sum += cur->bytes[bi];
            pend->bytes[pend->child_count - 1] = sum;
            pend->breaks[pend->child_count - 1] = bi;
            if (!r) break;
            cur = (lc_Leaf *)lc_poolalloc(c->S, &c->S->leaves);
            if (!cur) {
                r = LC_ERRMEM;
                goto fail_rollback;
            }
            memset(cur, 0, sizeof(lc_Leaf));
            pend->children[pend->child_count] = (lc_Node *)cur;
            pend->bytes[pend->child_count] = 0;
            pend->breaks[pend->child_count] = 0;
            pend->child_count++;
            start = 0;
        }
    }

    /* flush: drain pend[lv] into tree, then drive-down remaining levels */
    {
        int lvl = lv, more;
        for (more = 1; more;) {
            if (x.pend[lvl].child_count == 0) {
                lvl--;
                if (lvl < 0)
                    break;
                else
                    continue;
            }
            if ((r = lcB_checkpendroot(&x)) != LC_OK) break;
            r = lcB_flushone(&x, lvl);
            if (r == LC_AGAIN) {
                if ((r = lcB_packup(&x, lvl)) != LC_OK) break;
                continue;
            }
            if (r != LC_OK) break;
            lvl = (int)x.c.tree->levels;
            more = 0;
            for (i = 0; i <= lvl; ++i)
                if (x.pend[i].child_count > 0) {
                    more = 1;
                    break;
                }
        }
    }

cleanup:
    if (x.pend_root) lc_poolfree(&c->S->nodes, x.pend_root);
    if (r != LC_OK) {
        for (i = 0; i <= (int)c->levels; ++i)
            lcN_freechildren(c->S, &x.pend[i], (int)c->levels - i);
        return r;
    }
    lc_seek(C, c, old_off + (c->bytes - old_bytes));
    if (trailing) C->col += (unsigned)e;
    return LC_OK;

fail_rollback:
    /* OOM during fill: free new pend leaves, unsplit to restore tree */
    {
        lc_Node *pend = &x.pend[lv];
        while (pend->child_count > 1)
            lc_poolfree(&c->S->leaves, pend->children[--pend->child_count]);
    }
    if (!trailing) {
        size_t old_b = 0, old_l = 0;
        (void)old_b;
        (void)old_l;
        lcB_unsplitleafat(&x, 0, 0);
    }
    goto cleanup;
}

LC_NS_END

#endif /* LC_IMPLEMENTATION */
