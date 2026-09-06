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

#define AT_GROW  (0)
#define AT_FIXED (1)
#define AT_SHIFT (2)
#define AT_LOCK  (3)

#ifndef AT_MAX_LEVEL
# define AT_MAX_LEVEL 16
#endif

AT_NS_BEGIN

typedef struct at_State  at_State;
typedef struct at_Tree   at_Tree;
typedef struct at_Node   at_Node;
typedef struct at_Edge   at_Edge;
typedef struct at_Anchor at_Anchor;
typedef struct at_Point  at_Point;
typedef struct at_Cursor at_Cursor;

typedef unsigned       at_Id;
typedef unsigned short at_Index;

typedef void *at_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

struct at_Cursor {
    at_Tree  *T;
    size_t    off;
    size_t    poff;
    size_t    left_idx;
    size_t    right_idx;
    at_Id     cur; /* temporary at_next state; may change later */
    at_Node **paths[AT_MAX_LEVEL];
};

#define at_offset(C) ((C)->off + (C)->poff)

/* lifecycle */
AT_API at_State *at_open(at_Alloc *allocf, void *ud);
AT_API void      at_close(at_State *S);

AT_API at_Tree *at_newtree(at_State *S);
AT_API void     at_deltree(at_Tree *T);
AT_API void     at_reset(at_Tree *T);

/* cursor */
AT_API int   at_seek(at_Cursor *C, at_Tree *T, size_t pos);
AT_API int   at_advance(at_Cursor *C, ptrdiff_t d);
AT_API at_Id at_next(at_Cursor *C);

/* anchors */
AT_API int at_newanchor(at_Cursor *C, size_t len, int gravity, at_Id *out);
AT_API int at_del(at_Tree *T, at_Id id);
AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end);
AT_API int at_gravity(at_Tree *T, at_Id id);

/* splice */
AT_API int at_splice(at_Cursor *C, size_t del, size_t ins);

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
# define AT_FANOUT 34
#endif /* AT_FANOUT */

AT_STATIC_ASSERT(AT_FANOUT >= 4);

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
    size_t obj_size;
    void  *freed;
    void  *pages;
    size_t freed_obj;
#ifdef AT_POOL_STATS
    size_t live_obj;
#endif
} at_Pool;

struct at_Node {
    at_Node *children[AT_FANOUT];
    size_t   bytes[AT_FANOUT];
    at_Index child_count;
    at_Node *parent;
    at_Index idx;
};

struct at_Point {
    at_Id    id;
    unsigned ns : 31;
    unsigned is_end : 1;
};

struct at_Edge {
    at_Node  *parent;
    at_Index  idx;
    at_Point  first_left;
    at_Point  first_right;
    at_Point *left_tail;
    at_Point *right_tail;
};

struct at_Anchor {
    at_Edge *start;
    at_Edge *end;
};

struct at_State {
    void     *alloc_ud;
    at_Alloc *allocf;
    at_Pool   nodes;
    at_Pool   edges;
    at_Node   rt[AT_MAX_LEVEL];
};

struct at_Tree {
    at_Node    root;
    at_State  *S;
    size_t     bytes;
    int        levels;
    at_Edge    origin;
    at_Anchor *ids;
    size_t     idcap;
    at_Id      idfree;
};

#define atN_cc(n)            ((int)(n)->child_count)
#define atN_setcc(n, v)      ((n)->child_count = (at_Index)(v))
#define atN_edge(n, i)       ((at_Edge *)(n)->children[i])
#define atN_setedge(n, i, e) ((n)->children[i] = (at_Node *)(e))
#define atN_min()            (AT_FANOUT / 2)
#define atN_full(n)          (atN_cc(n) >= AT_FANOUT)

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
    size_t sz = p->obj_size;
    char  *page, *end;
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

/* node/leaf array helpers */

static size_t atN_sumbytes(const at_Node *n) {
    size_t s = 0;
    int    i;
    for (i = 0; i < atN_cc(n); ++i) s += n->bytes[i];
    return s;
}

static void atN_makespace(at_Node *n, int i, int k) {
    int cc = atN_cc(n), j;
    assert(cc + k <= AT_FANOUT && i <= cc);
    memmove(&n->children[i + k], &n->children[i], (cc - i) * sizeof(at_Node *));
    memmove(&n->bytes[i + k], &n->bytes[i], (cc - i) * sizeof(size_t));
    atN_setcc(n, cc + k);
    for (j = i + k; j < cc + k; ++j)
        ((at_Node *)n->children[j])->idx = (at_Index)j;
}

AT_STATIC void atN_remove(at_Node *n, int i, int k) {
    int cc = atN_cc(n), j;
    assert(i >= 0 && i + k <= cc);
    memmove(&n->children[i], &n->children[i + k],
            (cc - i - k) * sizeof(at_Node *));
    memmove(&n->bytes[i], &n->bytes[i + k], (cc - i - k) * sizeof(size_t));
    atN_setcc(n, cc - k);
    for (j = i; j < cc - k; ++j) ((at_Node *)n->children[j])->idx = (at_Index)j;
}

static void atL_makespace(at_Node *n, int i, int k) {
    int cc = atN_cc(n), j;
    assert(cc + k <= AT_FANOUT && i <= cc);
    memmove(&n->children[i + k], &n->children[i], (cc - i) * sizeof(at_Node *));
    memmove(&n->bytes[i + k], &n->bytes[i], (cc - i) * sizeof(size_t));
    atN_setcc(n, cc + k);
    for (j = i + k; j < cc + k; ++j)
        ((at_Edge *)n->children[j])->idx = (at_Index)j;
}

static void atL_remove(at_Node *n, int i, int k) {
    int cc = atN_cc(n), j;
    assert(i >= 0 && i + k <= cc);
    memmove(&n->children[i], &n->children[i + k],
            (cc - i - k) * sizeof(at_Node *));
    memmove(&n->bytes[i], &n->bytes[i + k], (cc - i - k) * sizeof(size_t));
    atN_setcc(n, cc - k);
    for (j = i; j < cc - k; ++j) ((at_Edge *)n->children[j])->idx = (at_Index)j;
}

static void atL_set(at_Node *n, int i, at_Edge *e, size_t b) {
    atN_setedge(n, i, e), n->bytes[i] = b;
    e->parent = n, e->idx = (at_Index)i;
}

static void atN_set(at_Node *n, int i, at_Node *c, size_t b) {
    n->children[i] = c, n->bytes[i] = b;
    c->parent = n, c->idx = (at_Index)i;
}

typedef struct atV_Header {
    unsigned len, cap;
} atV_Header;

#define atV_hdr(A) ((atV_Header *)(A) - 1)
#define atV_len(A) ((A) ? atV_hdr(A)->len : 0u)
#define atV_cap(A) ((A) ? atV_hdr(A)->cap : 0u)
#define atV_sz(c)  (sizeof(atV_Header) + (size_t)(c) * sizeof(at_Point))

static int atV_resize(at_Tree *T, at_Point **tailp, unsigned cap) {
    atV_Header *h, *old = *tailp ? atV_hdr(*tailp) : NULL;
    if (old == NULL && cap == 0) return AT_OK;
    if (cap == 0) {
        T->S->allocf(T->S->alloc_ud, old, atV_sz(old->cap), 0);
        *tailp = NULL;
        return AT_OK;
    }
    h = (atV_Header *)T->S->allocf(
            T->S->alloc_ud, old, old ? atV_sz(old->cap) : 0, atV_sz(cap));
    if (h == NULL) return AT_ERRMEM;
    if (old == NULL) h->len = 0;
    h->cap = cap;
    *tailp = (at_Point *)(h + 1);
    return AT_OK;
}

static int atV_reserve(at_Tree *T, at_Point **tailp, unsigned need) {
    atV_Header *h = *tailp ? atV_hdr(*tailp) : NULL;
    unsigned    have = h ? h->len : 0, cap = h ? h->cap : 0, nc = 4;
    unsigned    want;
    if (need == 0) return AT_OK;
    if (need > UINT_MAX - have) return AT_ERRMEM;
    want = have + need;
    if (cap >= want) return AT_OK;
    while (nc < want) {
        unsigned n = nc + (nc >> 1);
        if (n <= nc) return AT_ERRMEM;
        nc = n;
    }
    return atV_resize(T, tailp, nc);
}

static int atV_push(at_Tree *T, at_Point **tailp, at_Point p) {
    if (atV_reserve(T, tailp, 1) != AT_OK) return AT_ERRMEM;
    (*tailp)[atV_hdr(*tailp)->len++] = p;
    return AT_OK;
}

static void atV_free(at_Tree *T, at_Point **ptail) {
    if (*ptail == NULL) return;
    {
        int r = atV_resize(T, ptail, 0);
        assert(r == AT_OK);
        (void)r;
    }
}

static void atV_erase(at_Tree *T, at_Point **ptail, unsigned i) {
    at_Point   *tail = *ptail;
    atV_Header *h = atV_hdr(tail);
    assert(i < h->len);
    for (; i + 1 < h->len; ++i) tail[i] = tail[i + 1];
    h->len -= 1;
    if (h->len == 0) atV_free(T, ptail);
}

static int atO_side_count(at_Edge *e, int side) {
    at_Point *first = side ? &e->first_right : &e->first_left;
    at_Point *tail = side ? e->right_tail : e->left_tail;
    if (first->id == 0 && tail == NULL) return 0;
    return (first->id != 0) + (tail ? (int)atV_len(tail) : 0);
}

static int atO_empty(at_Edge *e) {
    return atO_side_count(e, 0) == 0 && atO_side_count(e, 1) == 0;
}

static int atO_side_of(at_Edge *e, at_Id id, int is_end) {
    at_Point *tail;
    unsigned  i;
    if (e->first_left.id == id && e->first_left.is_end == is_end) return 0;
    if (e->first_right.id == id && e->first_right.is_end == is_end) return 1;
    if (e->left_tail != NULL) {
        tail = e->left_tail;
        for (i = 0; i < atV_len(tail); ++i)
            if (tail[i].id == id && tail[i].is_end == is_end) return 0;
    }
    if (e->right_tail != NULL) {
        tail = e->right_tail;
        for (i = 0; i < atV_len(tail); ++i)
            if (tail[i].id == id && tail[i].is_end == is_end) return 1;
    }
    return -1;
}

static int atO_push(at_Tree *T, at_Edge *e, int side, at_Id id, int is_end) {
    at_Point *first = side ? &e->first_right : &e->first_left;
    at_Point  p;
    if (first->id == 0) {
        first->id = id, first->ns = 0, first->is_end = is_end;
        return AT_OK;
    }
    p.id = id, p.ns = 0, p.is_end = is_end;
    return atV_push(T, side ? &e->right_tail : &e->left_tail, p);
}

static int atO_reserve(at_Tree *T, at_Edge *e, int side, unsigned add) {
    at_Point  *first = side ? &e->first_right : &e->first_left;
    at_Point **ptail = side ? &e->right_tail : &e->left_tail;
    if (add == 0) return AT_OK;
    if (first->id != 0) return atV_reserve(T, ptail, add);
    if (add > 1u) return atV_reserve(T, ptail, add - 1u);
    return AT_OK;
}

static void atO_discard_empty_tail(at_Tree *T, at_Edge *e, int side) {
    at_Point **ptail = side ? &e->right_tail : &e->left_tail;
    if (*ptail != NULL && atV_len(*ptail) == 0) atV_free(T, ptail);
}

static void atO_remove(at_Tree *T, at_Edge *e, int side, at_Id id, int is_end) {
    at_Point  *first = side ? &e->first_right : &e->first_left;
    at_Point **ptail = side ? &e->right_tail : &e->left_tail;
    unsigned   i;
    if (first->id == id && first->is_end == is_end) {
        if (*ptail != NULL) {
            *first = (*ptail)[0];
            atV_erase(T, ptail, 0);
        } else {
            first->id = 0, first->is_end = 0;
        }
        return;
    }
    for (i = 0; i < atV_len(*ptail); ++i)
        if ((*ptail)[i].id == id && (*ptail)[i].is_end == is_end) break;
    atV_erase(T, ptail, i);
}

static void atE_destroy(at_Tree *T, at_Edge *e) {
    atV_free(T, &e->left_tail), atV_free(T, &e->right_tail);
    e->first_left.id = e->first_right.id = 0;
}

static int atN_split_node(
        at_State *S, at_Node *n, int level, at_Node **pright) {
    int      cc = atN_cc(n), left = (cc + 1) / 2, i, right = cc - left;
    at_Node *r = (at_Node *)atP_alloc(S, &S->nodes);
    if (r == NULL) return AT_ERRMEM;
    atN_setcc(r, right), r->parent = n->parent, r->idx = 0;
    if (level == 0) {
        for (i = 0; i < right; ++i)
            atL_set(r, i, atN_edge(n, left + i), n->bytes[left + i]);
        atN_setcc(n, left);
    } else {
        for (i = 0; i < right; ++i)
            atN_set(r, i, (at_Node *)n->children[left + i], n->bytes[left + i]);
        atN_setcc(n, left);
    }
    *pright = r;
    return AT_OK;
}

static int atN_split_root(at_Tree *T) {
    at_Node *left, *right;
    int      cc = atN_cc(&T->root), i, level = T->levels;
    left = (at_Node *)atP_alloc(T->S, &T->S->nodes);
    if (left == NULL) return AT_ERRMEM;
    atN_setcc(left, cc), left->parent = &T->root, left->idx = 0;
    if (level == 0) {
        for (i = 0; i < cc; ++i)
            atL_set(left, i, atN_edge(&T->root, i), T->root.bytes[i]);
    } else {
        for (i = 0; i < cc; ++i)
            atN_set(left, i, (at_Node *)T->root.children[i], T->root.bytes[i]);
    }
    if (atN_split_node(T->S, left, level, &right) != AT_OK) {
        if (level == 0) {
            for (i = 0; i < cc; ++i) {
                at_Edge *edge = atN_edge(&T->root, i);
                edge->parent = &T->root, edge->idx = (at_Index)i;
            }
        } else {
            for (i = 0; i < cc; ++i) {
                at_Node *child = (at_Node *)T->root.children[i];
                child->parent = &T->root, child->idx = (at_Index)i;
            }
        }
        atP_free(&T->S->nodes, left);
        return AT_ERRMEM;
    }
    atN_set(&T->root, 0, left, atN_sumbytes(left));
    atN_set(&T->root, 1, right, atN_sumbytes(right));
    atN_setcc(&T->root, 2), T->levels = T->levels + 1;
    return AT_OK;
}

static void atF_locate(
        at_Tree *T, size_t pos, at_Node **anc, int *idx, size_t *poff) {
    int d, i, cc;
    anc[0] = &T->root, *poff = pos;
    for (d = 0; d < T->levels; ++d) {
        cc = atN_cc(anc[d]);
        for (i = 0; i < cc && *poff > anc[d]->bytes[i]; ++i)
            *poff -= anc[d]->bytes[i];
        assert(i < cc), idx[d] = i;
        anc[d + 1] = (at_Node *)anc[d]->children[i];
    }
    cc = atN_cc(anc[T->levels]);
    for (i = 0; i < cc && *poff > anc[T->levels]->bytes[i]; ++i)
        *poff -= anc[T->levels]->bytes[i];
    assert(i < cc), idx[T->levels] = i;
}

static int atE_find(at_Tree *T, size_t pos, at_Edge **out) {
    at_Node *n = &T->root;
    size_t   rem = pos;
    int      level = T->levels, i, cc;
    if (T->bytes == 0 || pos > T->bytes || pos == 0) return 0;
    for (;;) {
        cc = atN_cc(n);
        for (i = 0; i < cc && rem > n->bytes[i]; ++i) rem -= n->bytes[i];
        if (i >= cc || (level == 0 && rem != n->bytes[i])) return 0;
        if (level == 0) return *out = atN_edge(n, i), 1;
        n = (at_Node *)n->children[i], level -= 1;
    }
}

static int atN_leaf_insert(
        at_Node *n, size_t rem, int append, size_t delta, at_Edge *e,
        at_Edge **out) {
    int i = 0, cc = atN_cc(n);
    if (append) {
        atL_makespace(n, cc, 1);
        return atL_set(n, cc, e, delta), *out = e, AT_OK;
    }
    while (i < cc && rem > n->bytes[i]) rem -= n->bytes[i], ++i;
    if (i < cc && rem == n->bytes[i]) return *out = atN_edge(n, i), AT_OK;
    assert(i < cc && rem < n->bytes[i]);
    n->bytes[i] -= rem;
    atL_makespace(n, i, 1);
    atL_set(n, i, e, rem);
    return *out = e, AT_OK;
}

static int atN_insert_nonfull(
        at_Tree *T, at_Node *n, int level, size_t rem, int append, size_t delta,
        at_Edge *e, at_Edge **out) {
    at_Node *child, *right;
    int      i, cc, r;
    if (level == 0) return atN_leaf_insert(n, rem, append, delta, e, out);
    cc = atN_cc(n);
    if (append) {
        i = cc - 1;
    } else {
        for (i = 0; i < cc && rem > n->bytes[i]; ++i) rem -= n->bytes[i];
        assert(i < cc);
    }
    child = (at_Node *)n->children[i];
    if (atN_cc(child) == AT_FANOUT) {
        r = atN_split_node(T->S, child, level - 1, &right);
        if (r != AT_OK) return r;
        atN_makespace(n, i + 1, 1);
        atN_set(n, i + 1, right, atN_sumbytes(right));
        n->bytes[i] = atN_sumbytes(child);
        if (append) {
            i += 1;
        } else if (rem > n->bytes[i]) {
            rem -= n->bytes[i], i += 1;
        }
    }
    r = atN_insert_nonfull(
            T, (at_Node *)n->children[i], level - 1, rem, append, delta, e,
            out);
    if (append && r == AT_OK) n->bytes[i] += delta;
    return r;
}

AT_STATIC int atE_insert(at_Tree *T, size_t pos, at_Edge **out) {
    at_Edge *e, *found;
    size_t   delta;
    int      r;
    if (T == NULL || out == NULL) return AT_ERRPARAM;
    if (pos == 0) return *out = &T->origin, AT_OK;
    if (atE_find(T, pos, &found)) return *out = found, AT_OK;
    if (atN_full(&T->root)) {
        r = atN_split_root(T);
        if (r != AT_OK) return r;
    }
    e = (at_Edge *)atP_alloc(T->S, &T->S->edges);
    if (e == NULL) return AT_ERRMEM;
    memset(e, 0, sizeof(at_Edge));
    if (pos > T->bytes) {
        delta = pos - T->bytes;
        r = atN_insert_nonfull(T, &T->root, T->levels, 0, 1, delta, e, out);
    } else {
        r = atN_insert_nonfull(T, &T->root, T->levels, pos, 0, 0, e, out);
    }
    if (r == AT_OK) {
        if (pos > T->bytes) T->bytes = pos;
        return AT_OK;
    }
    atP_free(&T->S->edges, e);
    return r;
}

static void atF_add_path(
        at_Node **anc, int *idx, int depth, int stop, ptrdiff_t delta) {
    int d;
    for (d = depth; d > stop && d >= 1; --d) {
        size_t *b = &anc[d - 1]->bytes[idx[d - 1]];
        if (delta < 0) {
            size_t v = *b - (size_t)(-delta);
            *b = v;
        } else {
            *b += (size_t)delta;
        }
    }
}

static void atT_collapse_root(at_Tree *T) {
    while (T->levels > 0 && atN_cc(&T->root) == 1) {
        at_Node *c = (at_Node *)T->root.children[0];
        int      cc = atN_cc(c), i;
        if (T->levels == 1) {
            for (i = 0; i < cc; ++i)
                atL_set(&T->root, i, atN_edge(c, i), c->bytes[i]);
        } else {
            for (i = 0; i < cc; ++i)
                atN_set(&T->root, i, (at_Node *)c->children[i], c->bytes[i]);
        }
        atN_setcc(&T->root, cc);
        atP_free(&T->S->nodes, c), T->levels = T->levels - 1;
    }
}

static void atT_remove_empty_leaf(at_Tree *T, at_Node **anc, int *idx, int L) {
    int d;
    atN_remove(anc[L - 1], idx[L - 1], 1);
    atP_free(&T->S->nodes, anc[L]);
    for (d = L - 1; d >= 0; --d) {
        if (atN_cc(anc[d]) != 0) break;
        if (d == 0) {
            T->levels = 0;
            break;
        }
        atN_remove(anc[d - 1], idx[d - 1], 1);
        atP_free(&T->S->nodes, anc[d]);
    }
    atT_collapse_root(T);
}

static void atE_merge_successor(
        at_Node **anc, int *idx, int L, int a, size_t delta) {
    at_Node *anc2[AT_MAX_LEVEL + 1], *n2;
    int      idx2[AT_MAX_LEVEL + 1];
    int      d;
    for (d = 0; d <= a; ++d) anc2[d] = anc[d], idx2[d] = idx[d];
    idx2[a] = idx[a] + 1;
    n2 = (at_Node *)anc[a]->children[idx2[a]];
    for (d = a + 1; d <= L; ++d) {
        anc2[d] = n2;
        if (d < L) {
            n2 = (at_Node *)n2->children[0];
            idx2[d] = 0;
        }
    }
    idx2[L] = 0;
    anc2[L]->bytes[0] += delta;
    atF_add_path(anc, idx, L, a, -(ptrdiff_t)delta);
    atF_add_path(anc2, idx2, L, a, (ptrdiff_t)delta);
}

AT_STATIC int atE_delete(at_Tree *T, size_t pos) {
    at_Node *anc[AT_MAX_LEVEL + 1], *leaf;
    int      idx[AT_MAX_LEVEL + 1], i, L, a = -1, d;
    size_t   rem = pos, delta;
    at_Edge *e;
    if (T == NULL || pos == 0 || pos > T->bytes) return AT_ERRPARAM;
    atF_locate(T, pos, anc, idx, &rem);
    L = T->levels, leaf = anc[L], i = idx[L];
    if (rem != leaf->bytes[i]) return AT_ERRPARAM;
    e = atN_edge(leaf, i), delta = leaf->bytes[i];
    if (i + 1 < atN_cc(leaf)) {
        leaf->bytes[i + 1] += delta;
        atL_remove(leaf, i, 1);
        atE_destroy(T, e);
        atP_free(&T->S->edges, e);
        return AT_OK;
    }
    atL_remove(leaf, i, 1);
    for (d = L - 1; d >= 0; --d)
        if (idx[d] < atN_cc(anc[d]) - 1) {
            a = d;
            break;
        }
    if (a < 0) {
        atF_add_path(anc, idx, L, -1, -(ptrdiff_t)delta);
        T->bytes -= delta;
    } else {
        atE_merge_successor(anc, idx, L, a, delta);
    }
    atE_destroy(T, e);
    atP_free(&T->S->edges, e);
    if (L > 0 && atN_cc(leaf) == 0) atT_remove_empty_leaf(T, anc, idx, L);
    return AT_OK;
}

static void atE_shift(at_Tree *T, size_t pos, size_t ins) {
    at_Node *anc[AT_MAX_LEVEL + 1], *leaf;
    int      idx[AT_MAX_LEVEL + 1], L, i;
    size_t   rem = pos + 1;
    assert(pos < T->bytes && ins != 0);
    atF_locate(T, rem, anc, idx, &rem);
    L = T->levels, leaf = anc[L], i = idx[L];
    leaf->bytes[i] += ins;
    atF_add_path(anc, idx, L, 0, (ptrdiff_t)ins);
    T->bytes += ins;
}

static void atE_unshift(at_Tree *T, size_t pos, size_t ins) {
    at_Node *anc[AT_MAX_LEVEL + 1], *leaf;
    int      idx[AT_MAX_LEVEL + 1], L, i;
    size_t   rem = pos + 1;
    assert(pos < T->bytes && ins != 0);
    atF_locate(T, rem, anc, idx, &rem);
    L = T->levels, leaf = anc[L], i = idx[L];
    leaf->bytes[i] -= ins;
    atF_add_path(anc, idx, L, 0, -(ptrdiff_t)ins);
    T->bytes -= ins;
}

AT_STATIC void atE_remove_slot(at_Tree *T, size_t pos) {
    at_Node *anc[AT_MAX_LEVEL + 1], *leaf;
    int      idx[AT_MAX_LEVEL + 1], i, L;
    size_t   rem = pos, delta;
    at_Edge *e;
    assert(pos > 0 && pos <= T->bytes);
    atF_locate(T, pos, anc, idx, &rem);
    L = T->levels, leaf = anc[L], i = idx[L];
    assert(rem == leaf->bytes[i]);
    e = atN_edge(leaf, i), delta = leaf->bytes[i];
    atL_remove(leaf, i, 1);
    atF_add_path(anc, idx, L, -1, -(ptrdiff_t)delta);
    T->bytes -= delta;
    atE_destroy(T, e);
    atP_free(&T->S->edges, e);
    if (L > 0 && atN_cc(leaf) == 0) atT_remove_empty_leaf(T, anc, idx, L);
}

static void atN_purge(at_Tree *T, at_Node *n, int level) {
    int i;
    if (level == 0) {
        for (i = 0; i < atN_cc(n); ++i) {
            atE_destroy(T, atN_edge(n, i));
            atP_free(&T->S->edges, atN_edge(n, i));
        }
        return;
    }
    for (i = 0; i < atN_cc(n); ++i) {
        atN_purge(T, (at_Node *)n->children[i], level - 1);
        atP_free(&T->S->nodes, (at_Node *)n->children[i]);
    }
}

static void atO_clear(at_Tree *T) {
    if (T->ids == NULL) return;
    T->S->allocf(T->S->alloc_ud, T->ids, T->idcap * sizeof(at_Anchor), 0);
    T->ids = NULL, T->idcap = 0, T->idfree = 0;
}

/* state/lifecycle */

static void *atS_defallocf(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    (void)ud, (void)osize;
    if (nsize == 0) return (void)free(p), NULL;
    return (np = realloc(p, nsize)) ? np : ((void)abort(), NULL);
}

AT_API at_State *at_open(at_Alloc *allocf, void *ud) {
    at_State *S;
    if (allocf == NULL) allocf = &atS_defallocf;
    S = (at_State *)allocf(ud, NULL, 0, sizeof(at_State));
    if (S == NULL) return NULL;
    memset(S, 0, sizeof(at_State)), S->alloc_ud = ud, S->allocf = allocf;
    atP_init(&S->nodes, sizeof(at_Node));
    atP_init(&S->edges, sizeof(at_Edge));
    return S;
}

AT_API void at_close(at_State *S) {
    if (S == NULL) return;
    atP_destroy(S, &S->nodes);
    atP_destroy(S, &S->edges);
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
    atN_purge(T, &T->root, T->levels);
    atE_destroy(T, &T->origin);
    atO_clear(T);
    T->S->allocf(T->S->alloc_ud, T, sizeof(at_Tree), 0);
}

AT_API void at_reset(at_Tree *T) {
    if (T == NULL) return;
    atN_purge(T, &T->root, T->levels);
    atE_destroy(T, &T->origin);
    atO_clear(T);
    memset(&T->root, 0, sizeof(at_Node));
    memset(&T->origin, 0, sizeof(at_Edge));
    T->bytes = 0, T->levels = 0;
}

/* cursor navigation */

static void atK_locend(at_Cursor *C) {
    at_Node *n = &C->T->root;
    int      d;
    C->off = C->poff = 0;
    for (d = 0; d < C->T->levels; ++d) {
        int i = atN_cc(n) - 1;
        C->off += atN_sumbytes(n) - n->bytes[i];
        C->paths[d] = &n->children[i];
        n = (at_Node *)n->children[i];
    }
    {
        int i = atN_cc(n) - 1;
        C->off += atN_sumbytes(n) - n->bytes[i];
        C->paths[C->T->levels] = &n->children[i];
        C->poff = n->bytes[i];
    }
}

static void atK_find(at_Cursor *C, size_t pos) {
    at_Node *n = &C->T->root;
    size_t   rem = pos;
    int      d, i, cc;
    C->off = C->poff = 0;
    for (d = 0; d < C->T->levels; ++d) {
        cc = atN_cc(n);
        for (i = 0; i < cc && rem > n->bytes[i]; ++i)
            rem -= n->bytes[i], C->off += n->bytes[i];
        assert(i < cc), C->paths[d] = &n->children[i];
        n = (at_Node *)n->children[i];
    }
    cc = atN_cc(n);
    for (i = 0; i < cc && rem > n->bytes[i]; ++i)
        rem -= n->bytes[i], C->off += n->bytes[i];
    assert(i < cc), C->paths[C->T->levels] = &n->children[i];
    C->poff = rem;
}

AT_API int at_seek(at_Cursor *C, at_Tree *T, size_t pos) {
    if (C == NULL || T == NULL) return AT_ERRPARAM;
    memset(C, 0, sizeof(at_Cursor)), C->T = T;
    if (T->bytes == 0) {
        C->paths[0] = T->root.children, C->poff = pos;
        return AT_OK;
    }
    if (pos > T->bytes) {
        size_t vir = pos - T->bytes;
        atK_locend(C), C->poff += vir;
        return AT_OK;
    }
    return atK_find(C, pos), AT_OK;
}

AT_API int at_advance(at_Cursor *C, ptrdiff_t d) {
    size_t pos;
    if (C == NULL || C->T == NULL) return AT_ERRPARAM;
    if (d != 0) C->cur = 0, C->left_idx = 0, C->right_idx = 0;
    pos = C->off + C->poff;
    if (d < 0) {
        size_t back = (size_t)(-d);
        if (back > pos) back = pos;
        pos -= back;
    } else {
        pos += (size_t)d;
    }
    if (C->T->bytes == 0) {
        C->off = 0, C->poff = pos;
        C->paths[0] = C->T->root.children;
        return AT_OK;
    }
    if (pos > C->T->bytes) {
        size_t vir = pos - C->T->bytes;
        atK_locend(C), C->poff += vir;
        return AT_OK;
    }
    return atK_find(C, pos), AT_OK;
}

/* object helpers and APIs */

static int atO_growids(at_Tree *T) {
    at_Anchor *ids;
    size_t     oldcap = T->idcap, newcap = oldcap ? oldcap * 2 : 8;
    size_t     from = oldcap ? oldcap : 1, i;
    if (newcap <= oldcap) return AT_ERRMEM;
    ids = (at_Anchor *)T->S->allocf(
            T->S->alloc_ud, NULL, 0, newcap * sizeof(at_Anchor));
    if (ids == NULL) return AT_ERRMEM;
    memset(ids, 0, newcap * sizeof(at_Anchor));
    if (oldcap != 0) memcpy(ids, T->ids, oldcap * sizeof(at_Anchor));
    for (i = newcap; i > from; --i) {
        at_Anchor *a = &ids[i - 1];
        a->start = (at_Edge *)(size_t)T->idfree;
        T->idfree = (at_Id)(i - 1);
    }
    if (oldcap != 0)
        T->S->allocf(T->S->alloc_ud, T->ids, oldcap * sizeof(at_Anchor), 0);
    T->ids = ids, T->idcap = newcap;
    return AT_OK;
}

static int atO_idalloc(at_Tree *T, at_Id *out) {
    at_Anchor *a;
    int        r;
    if (T->idfree == 0) {
        r = atO_growids(T);
        if (r != AT_OK) return r;
    }
    a = &T->ids[T->idfree];
    *out = T->idfree;
    T->idfree = (at_Id)(size_t)a->start;
    a->start = NULL, a->end = NULL;
    return AT_OK;
}

static void atO_idfree(at_Tree *T, at_Id id) {
    at_Anchor *a = &T->ids[id];
    a->start = (at_Edge *)(size_t)T->idfree;
    a->end = NULL;
    T->idfree = id;
}

static at_Anchor *atO_anchor(at_Tree *T, at_Id id) {
    at_Anchor *a;
    if (T == NULL || id == 0 || T->ids == NULL || id >= T->idcap) return NULL;
    a = &T->ids[id];
    return a->end == NULL ? NULL : a;
}

static size_t atO_edgepos(const at_Tree *T, const at_Edge *e) {
    const at_Node *n;
    size_t         pos = 0;
    int            i;
    if (e == &T->origin) return 0;
    n = e->parent;
    for (i = 0; i <= (int)e->idx; ++i) pos += n->bytes[i];
    while (n->parent != NULL) {
        const at_Node *p = n->parent;
        for (i = 0; i < (int)n->idx; ++i) pos += p->bytes[i];
        n = p;
    }
    return pos;
}

static void atO_clean(
        at_Tree *T, at_Edge *e, size_t pos, at_Id id, int is_end) {
    int side = atO_side_of(e, id, is_end);
    int r;
    if (side >= 0) atO_remove(T, e, side, id, is_end);
    if (e != &T->origin && atO_empty(e)) {
        r = atE_delete(T, pos);
        assert(r == AT_OK);
        (void)r;
    }
}

static int atO_gravsides(int gravity, int *sl, int *el) {
    if (gravity == AT_GROW) return *sl = 0, *el = 1, AT_OK;
    if (gravity == AT_FIXED) return *sl = 1, *el = 0, AT_OK;
    if (gravity == AT_SHIFT) return *sl = 1, *el = 1, AT_OK;
    if (gravity == AT_LOCK) return *sl = 0, *el = 0, AT_OK;
    return AT_ERRPARAM;
}

static int atO_edges(
        at_Tree *T, size_t start, size_t end, at_Edge **se, at_Edge **ee) {
    int r;
    r = atE_insert(T, start, se);
    if (r != AT_OK) return r;
    if (end == start) return *ee = *se, AT_OK;
    r = atE_insert(T, end, ee);
    if (r != AT_OK) {
        if (*se != &T->origin && atO_empty(*se)) atE_delete(T, start);
        return r;
    }
    return AT_OK;
}

static int atO_reserve_point(at_Tree *T, size_t pos, int side, unsigned add) {
    at_Edge *e = &T->origin;
    if (pos != 0 && !atE_find(T, pos, &e)) return AT_OK;
    return atO_reserve(T, e, side, add);
}

static int atO_reserve_newanchor(
        at_Tree *T, size_t start, size_t end, int sl, int el) {
    int r = atO_reserve_point(T, start, sl, start == end && sl == el ? 2u : 1u);
    if (r == AT_OK && (start != end || sl != el))
        r = atO_reserve_point(T, end, el, 1);
    return r;
}

static int atO_reserve_newedge(
        at_Tree *T, at_Edge *se, at_Edge *ee, size_t pos, int sl, int el,
        at_Id id) {
    int r = AT_OK;
    if (se == ee && sl == el) r = atO_reserve(T, se, sl, 2);
    if (r != AT_OK) {
        if (se != &T->origin && atO_empty(se)) atE_delete(T, pos);
        atO_idfree(T, id);
    }
    return r;
}

static void atO_movepoint(
        at_Tree *T, at_Edge *src, at_Edge *dst, int side, at_Id id,
        int is_end) {
    int r = atO_push(T, dst, side, id, is_end);
    assert(r == AT_OK);
    (void)r;
    atO_remove(T, src, side, id, is_end);
    if (is_end)
        T->ids[id].end = dst;
    else
        T->ids[id].start = dst;
}

static void atD_moveedge(at_Tree *T, at_Edge *src, at_Edge *dst) {
    at_Point p;
    while (src->first_left.id != 0) {
        p = src->first_left;
        atO_movepoint(T, src, dst, 0, p.id, p.is_end);
    }
    while (src->first_right.id != 0) {
        p = src->first_right;
        atO_movepoint(T, src, dst, 1, p.id, p.is_end);
    }
}

static void atD_relocate_node(
        at_Tree *T, at_Node *n, int level, size_t base, size_t lo, size_t hi,
        at_Edge *dst) {
    size_t b = base;
    int    i;
    if (level == 0) {
        for (i = 0; i < atN_cc(n); ++i) {
            b += n->bytes[i];
            if (b > lo && b <= hi) atD_moveedge(T, atN_edge(n, i), dst);
        }
        return;
    }
    for (i = 0; i < atN_cc(n); ++i) {
        if (b < hi && b + n->bytes[i] > lo) {
            atD_relocate_node(
                    T, (at_Node *)n->children[i], level - 1, b, lo, hi, dst);
        }
        b += n->bytes[i];
    }
}

static int atD_count_node(
        at_Node *n, int level, size_t base, size_t lo, size_t hi) {
    size_t b = base;
    int    c = 0, i;
    if (level == 0) {
        for (i = 0; i < atN_cc(n); ++i) {
            b += n->bytes[i];
            if (b > lo && b <= hi) c += 1;
        }
        return c;
    }
    for (i = 0; i < atN_cc(n); ++i) {
        if (b < hi && b + n->bytes[i] > lo)
            c += atD_count_node(
                    (at_Node *)n->children[i], level - 1, b, lo, hi);
        b += n->bytes[i];
    }
    return c;
}

static void atD_count_points_node(
        at_Node *n, int level, size_t base, size_t lo, size_t hi, unsigned *lc,
        unsigned *rc) {
    size_t b = base;
    int    i;
    if (level == 0) {
        for (i = 0; i < atN_cc(n); ++i) {
            b += n->bytes[i];
            if (b > lo && b <= hi) {
                *lc += (unsigned)atO_side_count(atN_edge(n, i), 0);
                *rc += (unsigned)atO_side_count(atN_edge(n, i), 1);
            }
        }
        return;
    }
    for (i = 0; i < atN_cc(n); ++i) {
        if (b < hi && b + n->bytes[i] > lo)
            atD_count_points_node(
                    (at_Node *)n->children[i], level - 1, b, lo, hi, lc, rc);
        b += n->bytes[i];
    }
}

static void atD_fill_node(
        at_Node *n, int level, size_t base, size_t lo, size_t hi, size_t *pos,
        int *idx) {
    size_t b = base;
    int    i;
    if (level == 0) {
        for (i = 0; i < atN_cc(n); ++i) {
            b += n->bytes[i];
            if (b > lo && b <= hi) pos[(*idx)++] = b;
        }
        return;
    }
    for (i = 0; i < atN_cc(n); ++i) {
        if (b < hi && b + n->bytes[i] > lo) {
            atD_fill_node(
                    (at_Node *)n->children[i], level - 1, b, lo, hi, pos, idx);
        }
        b += n->bytes[i];
    }
}

static int atD_remove_range(at_Tree *T, size_t L, size_t R, at_Edge *le) {
    size_t  *pos;
    size_t   n, i;
    unsigned lc = 0, rc = 0;
    int      r, idx = 0;
    n = (size_t)atD_count_node(&T->root, T->levels, 0, L, R);
    if (n == 0) return AT_OK;
    pos = (size_t *)T->S->allocf(T->S->alloc_ud, NULL, 0, n * sizeof(size_t));
    if (pos == NULL) return AT_ERRMEM;
    atD_fill_node(&T->root, T->levels, 0, L, R, pos, &idx);
    atD_count_points_node(&T->root, T->levels, 0, L, R, &lc, &rc);
    r = atO_reserve(T, le, 0, lc);
    if (r == AT_OK) r = atO_reserve(T, le, 1, rc);
    if (r != AT_OK) {
        atO_discard_empty_tail(T, le, 0);
        atO_discard_empty_tail(T, le, 1);
        T->S->allocf(T->S->alloc_ud, pos, n * sizeof(size_t), 0);
        return r;
    }
    atD_relocate_node(T, &T->root, T->levels, 0, L, R, le);
    for (i = n; i > 0; --i) atE_remove_slot(T, pos[i - 1]);
    T->S->allocf(T->S->alloc_ud, pos, n * sizeof(size_t), 0);
    return AT_OK;
}

static int atD_delete(at_Tree *T, size_t L, size_t del) {
    at_Edge *le, *re;
    size_t   R;
    int      lnew = 0, rnew = 0, r;
    if (L >= T->bytes || del == 0) return AT_OK;
    R = del > (size_t)-1 - L ? (size_t)-1 : L + del;
    if (R > T->bytes) R = T->bytes;
    if (R <= L) return AT_OK;
    le = &T->origin;
    if (L != 0) {
        r = atE_insert(T, L, &le);
        if (r != AT_OK) return r;
        lnew = atO_empty(le);
    }
    if (R < T->bytes) {
        if (!atE_find(T, R, &re)) {
            r = atE_insert(T, R, &re);
            if (r != AT_OK) {
                if (lnew && atO_empty(le)) atE_delete(T, L);
                return r;
            }
            rnew = 1;
        }
    }
    r = atD_remove_range(T, L, R, le);
    if (r != AT_OK) {
        if (rnew) atE_delete(T, R);
        if (lnew && atO_empty(le)) atE_delete(T, L);
        return r;
    }
    if (lnew && atO_empty(le)) {
        r = atE_delete(T, L);
        assert(r == AT_OK);
        (void)r;
    }
    return AT_OK;
}

static void atI_move_rights(at_Tree *T, at_Edge *src, at_Edge *dst) {
    at_Point p;
    while (src->first_right.id != 0) {
        p = src->first_right;
        atO_movepoint(T, src, dst, 1, p.id, p.is_end);
    }
}

static int atI_insert(at_Tree *T, size_t pos, size_t ins) {
    at_Edge *e, *ne;
    unsigned count;
    int      r;
    if (ins == 0) return AT_OK;
    if (T->bytes == 0) {
        if (pos != 0 || T->origin.first_right.id == 0) return AT_OK;
        r = atE_insert(T, ins, &ne);
        if (r != AT_OK) return r;
        count = (unsigned)atO_side_count(&T->origin, 1);
        r = atO_reserve(T, ne, 1, count);
        if (r != AT_OK) {
            r = atE_delete(T, ins);
            assert(r == AT_OK);
            (void)r;
            return AT_ERRMEM;
        }
        atI_move_rights(T, &T->origin, ne);
        return AT_OK;
    }
    if (pos > T->bytes) return AT_OK;
    if (pos == 0) {
        e = &T->origin;
    } else if (!atE_find(T, pos, &e)) {
        if (pos < T->bytes) atE_shift(T, pos, ins);
        return AT_OK;
    }
    if (pos < T->bytes) atE_shift(T, pos, ins);
    if (e->first_right.id == 0) return AT_OK;
    r = atE_insert(T, pos + ins, &ne);
    if (r != AT_OK) {
        if (pos < T->bytes) atE_unshift(T, pos, ins);
        return r;
    }
    count = (unsigned)atO_side_count(e, 1);
    r = atO_reserve(T, ne, 1, count);
    if (r != AT_OK) {
        r = atE_delete(T, pos + ins);
        assert(r == AT_OK);
        (void)r;
        if (pos < T->bytes) atE_unshift(T, pos, ins);
        return AT_ERRMEM;
    }
    atI_move_rights(T, e, ne);
    if (e != &T->origin && atO_empty(e)) {
        r = atE_delete(T, pos);
        assert(r == AT_OK);
        (void)r;
    }
    return AT_OK;
}

AT_API int at_splice(at_Cursor *C, size_t del, size_t ins) {
    at_Tree *T;
    size_t   L;
    int      r;
    if (C == NULL || C->T == NULL) return AT_ERRPARAM;
    T = C->T, L = at_offset(C);
    if (del == 0 && ins == 0) return AT_OK;
    r = atP_reserve(T->S, &T->S->nodes, 4 * (T->levels + 2u) + 8u);
    if (r != AT_OK) return r;
    r = atP_reserve(T->S, &T->S->edges, 3);
    if (r != AT_OK) return r;
    if (del > 0) {
        r = atD_delete(T, L, del);
        if (r != AT_OK) return r;
    }
    if (ins > 0) {
        r = at_seek(C, T, L);
        if (r != AT_OK) return r;
        r = atI_insert(T, L, ins);
        if (r != AT_OK) return r;
    }
    return at_seek(C, T, L);
}

AT_API int at_newanchor(at_Cursor *C, size_t len, int gravity, at_Id *out) {
    at_Edge   *se, *ee;
    at_Tree   *T;
    at_Anchor *a;
    at_Id      id;
    size_t     start, end;
    int        sl, el, r;
    if (C == NULL || C->T == NULL || out == NULL) return AT_ERRPARAM;
    T = C->T, start = at_offset(C);
    if (len > (size_t)-1 - start) return AT_ERRPARAM;
    end = start + len;
    r = atO_gravsides(gravity, &sl, &el);
    if (r != AT_OK) return r;
    r = atO_reserve_newanchor(T, start, end, sl, el);
    if (r != AT_OK) return r;
    r = atO_idalloc(T, &id);
    if (r != AT_OK) return r;
    r = atO_edges(T, start, end, &se, &ee);
    if (r != AT_OK) return atO_idfree(T, id), r;
    r = atO_reserve_newedge(T, se, ee, start, sl, el, id);
    if (r != AT_OK) return r;
    r = atO_push(T, se, sl, id, 0), assert(r == AT_OK), (void)r;
    r = atO_push(T, ee, el, id, 1), assert(r == AT_OK), (void)r;
    a = &T->ids[id], a->start = se, a->end = ee;
    *out = id;
    return AT_OK;
}

AT_API int at_del(at_Tree *T, at_Id id) {
    at_Anchor *a;
    size_t     start, end;
    if (atO_anchor(T, id) == NULL) return AT_ERRPARAM;
    a = &T->ids[id], start = atO_edgepos(T, a->start);
    end = atO_edgepos(T, a->end);
    atO_clean(T, a->start, start, id, 0);
    atO_clean(T, a->end, end, id, 1);
    atO_idfree(T, id);
    return AT_OK;
}

AT_API int at_range(at_Tree *T, at_Id id, size_t *start, size_t *end) {
    at_Anchor *a;
    if (start == NULL || end == NULL) return AT_ERRPARAM;
    a = atO_anchor(T, id);
    if (a == NULL) return AT_ERRPARAM;
    *start = atO_edgepos(T, a->start), *end = atO_edgepos(T, a->end);
    return AT_OK;
}

AT_API int at_gravity(at_Tree *T, at_Id id) {
    at_Anchor *a;
    int        sl, el;
    a = atO_anchor(T, id);
    if (a == NULL) return AT_ERRPARAM;
    sl = atO_side_of(a->start, id, 0);
    el = atO_side_of(a->end, id, 1);
    if (sl < 0 || el < 0) return AT_ERRPARAM;
    if (sl == 0 && el == 1) return AT_GROW;
    if (sl == 1 && el == 0) return AT_FIXED;
    if (sl == 1 && el == 1) return AT_SHIFT;
    return AT_LOCK;
}

/* object iteration */

static int atO_nextstart(at_Edge *e, size_t *lp, size_t *rp, at_Id *out) {
    at_Point p;
    size_t   n;
    int      side;
    for (side = 0; side < 2; ++side) {
        at_Point *first = side ? &e->first_right : &e->first_left;
        at_Point *tail = side ? e->right_tail : e->left_tail;
        size_t   *idx = side ? rp : lp;
        n = (size_t)atO_side_count(e, side);
        for (; *idx < n; ++*idx) {
            p = *idx == 0 ? *first : tail[*idx - 1];
            if (p.is_end == 0) {
                *out = p.id, *idx += 1;
                return 1;
            }
        }
    }
    return 0;
}

static int atK_nextedge(at_Cursor *C) {
    at_Node *p, *q;
    int      L = C->T->levels, l, i, cc;
    p = L ? (at_Node *)*C->paths[L - 1] : &C->T->root;
    i = (int)(C->paths[L] - p->children);
    cc = atN_cc(p);
    C->off += p->bytes[i];
    if (i + 1 < cc) {
        C->paths[L] = &p->children[i + 1];
        C->poff = p->bytes[i + 1];
        return 1;
    }
    for (l = L - 1; l >= 0; --l) {
        q = l ? (at_Node *)*C->paths[l - 1] : &C->T->root;
        i = (int)(C->paths[l] - q->children);
        if (i + 1 < atN_cc(q)) {
            C->paths[l] = &q->children[i + 1];
            while (l < L) {
                p = (at_Node *)*C->paths[l];
                ++l;
                C->paths[l] = &p->children[0];
            }
            C->poff = p->bytes[0];
            return 1;
        }
    }
    return 0;
}

AT_API at_Id at_next(at_Cursor *C) {
    at_Tree *T;
    at_Edge *e;
    at_Node *p;
    at_Id    id;
    size_t   b;
    int      i, L;
    if (C == NULL || C->T == NULL) return AT_NONE;
    T = C->T;
    if (C->cur == AT_NONE) return AT_NONE;
    if (C->cur == 0 && at_offset(C) == 0) C->cur = 1;
    if (C->cur == 1) {
        if (atO_nextstart(&T->origin, &C->left_idx, &C->right_idx, &id))
            return id;
        C->cur = 2, C->left_idx = 0, C->right_idx = 0;
    }
    if (T->bytes == 0) return C->cur = AT_NONE, AT_NONE;
    for (;;) {
        L = T->levels;
        p = L ? (at_Node *)*C->paths[L - 1] : &T->root;
        i = (int)(C->paths[L] - p->children);
        b = p->bytes[i];
        if (C->poff > b) return C->cur = AT_NONE, AT_NONE;
        C->cur = 2, e = atN_edge(p, i);
        if (atO_nextstart(e, &C->left_idx, &C->right_idx, &id)) return id;
        if (!atK_nextedge(C)) return C->cur = AT_NONE, AT_NONE;
        C->left_idx = 0, C->right_idx = 0;
    }
}

AT_NS_END

#endif /* AT_IMPLEMENTATION */
