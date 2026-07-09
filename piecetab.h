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

typedef struct pt_Hole   pt_Hole;
typedef struct pt_State  pt_State;
typedef struct pt_Cursor pt_Cursor;

typedef const struct pt_Tree *pt_Blob;

typedef ptrdiff_t pt_Offset;
typedef size_t    pt_Size;

typedef void *pt_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

PT_API pt_State *pt_newstate(pt_Alloc *allocf, void *ud);
PT_API void      pt_close(pt_State *S);
PT_API pt_Alloc *pt_getallocf(pt_State *S, void **pud);

/* blob snapshot */

PT_API pt_Blob pt_empty(pt_State *S);
PT_API pt_Blob pt_from(pt_State *S, const char *s, size_t len);
PT_API int     pt_version(pt_Blob b);
PT_API void    pt_retain(pt_Blob b);
PT_API void    pt_release(pt_Blob b);
PT_API size_t  pt_bytes(pt_Blob b);

/* cursor */

#define pt_offset(C) ((C)->off + (C)->poff)

PT_API int pt_seek(pt_Cursor *C, pt_Blob b, pt_Offset off);
PT_API int pt_advance(pt_Cursor *C, pt_Offset delta);

/* editing */

PT_API int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len);
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
    pt_Size          poff;
    pt_Offset        off;
    short            dirty;
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
# define PT_MAX_HOLESIZE (64 - sizeof(unsigned short))
#endif

#define PT_MASK_BITS (sizeof(pt_Mask) * CHAR_BIT)

#ifndef PT_MASK_SIZE
# define PT_MASK_SIZE ((PT_FANOUT + PT_MASK_BITS - 1) / PT_MASK_BITS)
#endif

#define pt_min(a, b) ((a) < (b) ? (a) : (b))
#define pt_max(a, b) ((a) > (b) ? (a) : (b))

#define PT_STATIC_ASSERT(cond)      PT_SA_0(cond, pt_SA_, __LINE__)
#define PT_SA_0(cond, prefix, line) PT_SA_1(cond, prefix, line)
#define PT_SA_1(cond, prefix, line) typedef char prefix##line[(cond) ? 1 : -1]

PT_NS_BEGIN

typedef size_t pt_Mask;

typedef struct pt_Node {
    pt_Size         bytes[PT_FANOUT];
    pt_Mask         mask[PT_MASK_SIZE];
    struct pt_Node *children[PT_FANOUT];
    unsigned        version;
    unsigned short  child_count;
} pt_Node;

struct pt_Hole {
    unsigned short n;
    char           data[PT_MAX_HOLESIZE];
};

typedef struct pt_Tree {
    pt_State       *S;
    struct pt_Tree *from;
    pt_Size         bytes;
    pt_Node         root;
    unsigned        refc;
    unsigned        levels;
    unsigned        version;
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
    unsigned   max_version;
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
PT_STATIC void ptP_free(pt_Pool *pool, void *obj)
{ ptP_stat(pool->live_obj -= 1); *(void **)obj = pool->freed, pool->freed = obj; }

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

PT_STATIC void *ptP_ralloc(pt_Pool *pool) {
    char *obj = pool->freed;
    assert(obj), ptP_stat(pool->live_obj += 1);
    return (pool->freed = *(void **)obj), (void *)obj;
}

PT_STATIC void *ptP_alloc(pt_State *S, pt_Pool *pool) {
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

PT_STATIC int ptP_reserve(pt_State *S, pt_Pool *pool, size_t n) {
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

static void ptP_freescratch(pt_State *S, pt_Scratch *pool) {
    void *page = pool->pages;
    while (page) {
        void *next = *(void **)page;
        S->allocf(S->alloc_ud, page, PT_PAGE_SIZE, 0);
        page = next;
    }
    page = pool->reserve;
    while (page) {
        void *next = *(void **)page;
        S->allocf(S->alloc_ud, page, PT_PAGE_SIZE, 0);
        page = next;
    }
    ptP_initscratch(pool);
}

/* mask */

static void ptM_sethole(pt_Node *n, int i, int ishole) {
    pt_Mask *m = &n->mask[i / PT_MASK_BITS];
    *m ^= (-!!ishole ^ *m) & ((pt_Mask)1 << (i % PT_MASK_BITS));
}

static int ptM_ishole(const pt_Node *n, int i) {
    const pt_Mask m = n->mask[i / PT_MASK_BITS];
    return (m & ((pt_Mask)1 << (i % PT_MASK_BITS))) != 0;
}

/* API */

PT_API int  pt_version(pt_Blob b) { return b ? b->version : 0; }
PT_API void pt_retain(pt_Blob b) {
    if (b) ((pt_Tree *)b)->refc++;
}

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
    ptP_initscratch(&S->scratch);
    S->max_version = 0;
    return S;
}

PT_API void pt_close(pt_State *S) {
    if (S == NULL) return;
    ptP_destroy(S, &S->nodes);
    ptP_destroy(S, &S->holes);
    ptP_freescratch(S, &S->scratch);
    S->allocf(S->alloc_ud, S, sizeof(pt_State), 0);
}

PT_API pt_Blob pt_empty(pt_State *S) {
    pt_Tree *b = S ? (pt_Tree *)S->allocf(S->alloc_ud, NULL, 0, sizeof(pt_Tree))
                   : NULL;
    if (b == NULL) return NULL;
    memset(b, 0, sizeof(pt_Tree));
    b->S = S;
    b->version = 0;
    b->refc = 1;
    return b;
}

PT_API pt_Blob pt_from(pt_State *S, const char *s, size_t len) {
    pt_Tree *b;
    if (!S || (!s && len > 0)) return NULL;
    b = (pt_Tree *)S->allocf(S->alloc_ud, NULL, 0, sizeof(pt_Tree));
    if (!b) return NULL;
    memset(b, 0, sizeof(pt_Tree));
    b->S = S;
    b->refc = 1;
    if (len > 0) {
        b->root.children[0] = (pt_Node *)s;
        b->root.bytes[0] = len;
        b->root.child_count = 1;
        b->bytes = len;
    }
    return b;
}

/* pop a page from reserve (or allocf if empty), set as current write page.
   returns 1 on success, 0 on OOM. */
static int ptP_scratchpage(pt_State *S) {
    void *newpage;
    if (S->scratch.reserve) {
        newpage = S->scratch.reserve;
        S->scratch.reserve = *(void **)newpage;
    } else {
        newpage = S->allocf(S->alloc_ud, NULL, 0, PT_PAGE_SIZE);
        if (newpage == NULL) return 0;
    }
    *(void **)newpage = S->scratch.pages;
    S->scratch.pages = newpage;
    S->scratch.buffer = (char *)newpage + sizeof(void *);
    S->scratch.remain = PT_PAGE_SIZE - sizeof(void *);
    return 1;
}

PT_API char *pt_literal(pt_State *S, size_t *plen) {
    if (S == NULL || plen == NULL || *plen == 0) return NULL;
    if (S->scratch.remain == 0 && !ptP_scratchpage(S)) return NULL;
    if (*plen > S->scratch.remain) *plen = S->scratch.remain;
    S->scratch.remain -= *plen;
    S->scratch.buffer += *plen;
    return (char *)S->scratch.buffer - *plen;
}

PT_API char *pt_scratch(pt_State *S, size_t *plen) {
    if (S == NULL || plen == NULL) return NULL;
    if (S->scratch.remain == 0 && !ptP_scratchpage(S)) return NULL;
    *plen = S->scratch.remain;
    return S->scratch.buffer;
}

static void ptR_freechildren(pt_State *S, pt_Node *n, int rl, unsigned ver) {
    int i;
    if (rl == 0) {
        for (i = 0; i < n->child_count; ++i)
            if (ptM_ishole(n, i)) ptP_free(&S->holes, (void *)n->children[i]);
    } else {
        for (i = 0; i < n->child_count; ++i) {
            pt_Node *c = n->children[i];
            if (c->version != ver)
                continue; /* shared node owned by older ver */
            ptR_freechildren(S, c, rl - 1, ver);
            ptP_free(&S->nodes, c);
        }
    }
}

PT_API void pt_release(pt_Blob b) {
    pt_State *S;
    pt_Tree  *from;
    if (b == NULL) return;
    if (b->refc > 1) {
        --((pt_Tree *)b)->refc;
        return;
    }
    S = b->S, from = b->from;
    if (b->levels > 0)
        ptR_freechildren(S, &((pt_Tree *)b)->root, (int)b->levels, b->version);
    else if (b->root.child_count > 0)
        ptR_freechildren(S, &((pt_Tree *)b)->root, 0, b->version);
    S->allocf(S->alloc_ud, (void *)b, sizeof(pt_Tree), 0);
    if (from) pt_release(from);
}

/* commit helpers — ptC_* hole→literal freeze */

PT_STATIC void ptH_free(pt_State *S, pt_Hole *h);

static size_t ptC_leafbytes(const pt_Node *cont) {
    size_t s = 0;
    int    i;
    for (i = 0; i < (int)cont->child_count; ++i)
        if (ptM_ishole(cont, i)) s += ((pt_Hole *)cont->children[i])->n;
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
        if (ix[l] >= (int)p->child_count) {
            --l;
            continue;
        }
        k = ix[l]++;
        if (!ptM_ishole(p, k)) continue;
        if (l == (int)tree->levels - 1) {
            cont = p->children[k];
            total += ptC_leafbytes(cont);
        } else {
            stk[++l] = p->children[k];
            ix[l] = 0;
        }
    }
    return total;
}

/* reserve enough scratch pages so freeze never calls allocf (全或无). */
static int ptC_reservescratch(pt_State *S, size_t total) {
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
    for (i = 0; i < (int)cont->child_count; ++i) {
        if (ptM_ishole(cont, i)) {
            pt_Hole *h = (pt_Hole *)cont->children[i];
            char    *lit = ptC_freshscratch(S, h->data, (size_t)h->n);
            cont->children[i] = (pt_Node *)lit;
            ptH_free(S, h);
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
        if (ix[l] >= (int)p->child_count) {
            --l;
            continue;
        }
        k = ix[l]++;
        if (!ptM_ishole(p, k)) continue;
        if (l == (int)tree->levels - 1) {
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
    if (total > 0 && !ptC_reservescratch(S, total)) /* 相1 全或无闸门 */
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

PT_API pt_Alloc *pt_getallocf(pt_State *S, void **pud) {
    if (S == NULL) return NULL;
    if (pud) *pud = S->alloc_ud;
    return S->allocf;
}

PT_API size_t pt_bytes(pt_Blob b) { return b ? b->bytes : 0; }

/* locate */

#define ptK_levels(C) ((int)(C)->tree->levels)
#define ptK_bytes(C)  ((C)->tree->bytes)

#define ptK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define ptK_idx(C, p, l) ((int)((C)->paths[(l)] - (p)->children))

static void ptK_findleaf(pt_Cursor *C, int l, size_t *poff) {
    for (; l <= ptK_levels(C); ++l) {
        pt_Node *p = ptK_parent(C, l);
        int      i;
        for (i = 0; i < (int)p->child_count && *poff >= p->bytes[i]; ++i)
            *poff -= p->bytes[i], C->off += p->bytes[i];
        C->paths[l] = &p->children[i];
    }
}

static void ptK_locend(pt_Cursor *C) {
    pt_Node *n = &C->tree->root;
    int      l, last;
    for (l = 0; l < ptK_levels(C); ++l)
        n = *(C->paths[l] = &n->children[n->child_count - 1]);
    if (n->child_count == 0) {
        C->paths[0] = n->children, C->off = 0, C->poff = 0;
        return;
    }
    last = n->child_count - 1, C->paths[l] = &n->children[last];
    C->poff = n->bytes[last], C->off = ptK_bytes(C) - C->poff;
}

PT_API int pt_seek(pt_Cursor *C, pt_Blob b, pt_Offset off) {
    size_t n;
    if (C == NULL || b == NULL) return PT_ERRPARAM;
    n = (size_t)pt_min(pt_max(0, off), (pt_Offset)b->bytes);
    memset(C, 0, sizeof(pt_Cursor)), C->tree = (pt_Tree *)b;
    if (n >= b->bytes) return ptK_locend(C), PT_OK;
    return ptK_findleaf(C, 0, &n), (C->poff = (pt_Size)n), PT_OK;
}

static int ptK_forwardoff(pt_Cursor *C, size_t d) {
    pt_Node *p = ptK_parent(C, ptK_levels(C));
    int      l, i = ptK_idx(C, p, ptK_levels(C));
    size_t   in = p->bytes[i] - C->poff;
    if (d < in) return C->poff += d, 0;
    d -= in, C->off += p->bytes[i], C->poff = 0;
    for (l = ptK_levels(C); l >= 0; --l) {
        p = ptK_parent(C, l), i = ptK_idx(C, p, l) + 1;
        for (; i < (int)p->child_count && d >= p->bytes[i]; ++i)
            d -= p->bytes[i], C->off += p->bytes[i];
        if (i < (int)p->child_count) break;
    }
    assert(l >= 0 && i < (int)p->child_count);
    C->paths[l] = &p->children[i];
    return ptK_findleaf(C, l + 1, &d), C->poff = (pt_Size)d, 1;
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
    return ptK_findleaf(C, l + 1, &d), C->poff = (pt_Size)d, 1;
}

PT_API int pt_advance(pt_Cursor *C, pt_Offset delta) {
    pt_Offset n;
    if (C == NULL || C->tree == NULL) return PT_ERRPARAM;
    if (delta == 0 || ptK_bytes(C) == 0) return PT_OK;
    n = (pt_Offset)(C->off + C->poff) + delta;
    if (n <= 0) return ptK_backwardoff(C, C->off + C->poff), PT_OK;
    if ((size_t)n >= ptK_bytes(C)) return ptK_locend(C), PT_OK;
    if (delta < 0) return ptK_backwardoff(C, (size_t)(-delta)), PT_OK;
    return ptK_forwardoff(C, (size_t)delta), PT_OK;
}

/* editing helpers */

typedef ptrdiff_t pt_Diff;

/* makeroom needs at most 2 free slots; a split of a full node leaves
 * FANOUT/2 free in the cursor's half, so require FANOUT >= 4. */
PT_STATIC_ASSERT(PT_FANOUT >= 4);

PT_STATIC size_t ptN_sumbytes(const pt_Node *n, int i, int end) {
    size_t s = 0;
    for (; i < end; ++i) s += n->bytes[i];
    return s;
}

static void ptM_upbytes(pt_Cursor *C, int l, pt_Diff db) {
    if (db == 0) return;
    for (; l >= 0; --l) {
        pt_Node *p = ptK_parent(C, l);
        int      i = ptK_idx(C, p, l);
        p->bytes[i] += (size_t)db;
    }
    C->tree->bytes += (size_t)db;
}

static void ptN_makespace(pt_Node *p, int i, int n) {
    int moved = p->child_count - i, j;
    assert(p->child_count + n <= PT_FANOUT && i <= p->child_count);
    memmove(&p->children[i + n], &p->children[i], moved * sizeof(pt_Node *));
    memmove(&p->bytes[i + n], &p->bytes[i], moved * sizeof(pt_Size));
    for (j = moved - 1; j >= 0; --j) {
        if (ptM_ishole(p, i + j))
            ptM_sethole(p, i + j + n, 1), ptM_sethole(p, i + j, 0);
    }
    p->child_count += (unsigned short)n;
}

static void ptN_delslot(pt_Node *p, int k) {
    int moved = p->child_count - k - 1, j;
    memmove(&p->children[k], &p->children[k + 1], moved * sizeof(pt_Node *));
    memmove(&p->bytes[k], &p->bytes[k + 1], moved * sizeof(pt_Size));
    for (j = 0; j < moved; ++j) ptM_sethole(p, k + j, ptM_ishole(p, k + j + 1));
    p->child_count -= 1;
}

PT_STATIC void ptN_copy(pt_Node *d, int di, const pt_Node *s, int si, int n) {
    int j;
    memcpy(&d->children[di], &s->children[si], (size_t)n * sizeof(pt_Node *));
    memcpy(&d->bytes[di], &s->bytes[si], (size_t)n * sizeof(pt_Size));
    for (j = 0; j < n; ++j) ptM_sethole(d, di + j, ptM_ishole(s, si + j));
}

PT_STATIC void ptN_delrange(pt_Node *p, int s, int e) {
    int moved = p->child_count - e, j;
    assert(s <= e && e <= (int)p->child_count);
    memmove(&p->children[s], &p->children[e],
            (size_t)moved * sizeof(pt_Node *));
    memmove(&p->bytes[s], &p->bytes[e], (size_t)moved * sizeof(pt_Size));
    for (j = 0; j < moved; ++j) ptM_sethole(p, s + j, ptM_ishole(p, e + j));
    p->child_count -= (unsigned short)(e - s);
}

PT_STATIC void ptH_free(pt_State *S, pt_Hole *h) { ptP_free(&S->holes, h); }

PT_STATIC void ptH_trimhead(pt_Hole *h, size_t cut) {
    size_t remain = (size_t)h->n - cut;
    memmove(h->data, h->data + cut, remain);
    h->n = (unsigned short)remain;
}

PT_STATIC void ptH_trimtail(pt_Hole *h, size_t keep) {
    h->n = (unsigned short)keep;
}

PT_STATIC void ptH_append(pt_Hole *h, const char *s, size_t len) {
    assert((size_t)h->n + len <= PT_MAX_HOLESIZE);
    memcpy(h->data + h->n, s, len);
    h->n = (unsigned short)((size_t)h->n + len);
}

PT_STATIC void ptH_insmid(pt_Hole *h, size_t poff, const char *s, size_t len) {
    assert(poff <= (size_t)h->n && (size_t)h->n + len <= PT_MAX_HOLESIZE);
    memmove(h->data + poff + len, h->data + poff, (size_t)h->n - poff);
    memcpy(h->data + poff, s, len);
    h->n = (unsigned short)((size_t)h->n + len);
}

PT_STATIC pt_Hole *ptH_new(pt_State *S, const char *s, size_t len) {
    pt_Hole *h = (pt_Hole *)ptP_alloc(S, &S->holes);
    assert(h && len <= PT_MAX_HOLESIZE);
    h->n = (unsigned short)len;
    memcpy(h->data, s, len);
    return h;
}

PT_STATIC void ptH_delmid(pt_Hole *h, size_t a, size_t b) {
    size_t nbytes = (size_t)h->n - b;
    size_t remain = (size_t)h->n - (b - a);
    memmove(h->data + a, h->data + b, nbytes);
    h->n = (unsigned short)remain;
}

static int ptK_fork(pt_Cursor *C) {
    pt_State *S = C->tree->S;
    pt_Tree  *old = C->tree, *nb;
    int       i;
    if (C->dirty) return PT_OK;
    nb = (pt_Tree *)S->allocf(S->alloc_ud, NULL, 0, sizeof(pt_Tree));
    if (!nb) return PT_ERRMEM;
    *nb = *old; /* copy root; children shared until cowpath */
    nb->version = ++S->max_version;
    nb->root.version = nb->version;
    nb->refc = 1;
    nb->from = old, pt_retain(old); /* keep source alive: COW lifetime */
    /* deep-copy holes in root so each tree owns its copies (levels==0) */
    if (nb->levels == 0)
        for (i = 0; i < (int)nb->root.child_count; ++i)
            if (ptM_ishole(&old->root, i)) {
                pt_Hole *nh = (pt_Hole *)ptP_alloc(S, &S->holes);
                *nh = *(pt_Hole *)old->root.children[i];
                nb->root.children[i] = (pt_Node *)nh;
            }
    C->paths[0] = nb->root.children + (C->paths[0] - old->root.children);
    C->tree = nb, C->dirty = 1;
    return PT_OK; /* paths[1..levels] rebased later by cowpath */
}

static pt_Node *ptK_cownode(
        pt_Cursor *C, pt_Node **slot, pt_Node **old, int rl) {
    pt_State *S = C->tree->S;
    pt_Node  *child = *slot, *nw;
    int       i;
    *old = child;
    if (child->version == C->tree->version) return child;
    nw = (pt_Node *)ptP_ralloc(&S->nodes);
    *nw = *child, nw->version = C->tree->version;
    /* deep-copy holes in leaf data (rl==0: children are leaf data) */
    if (rl == 0)
        for (i = 0; i < (int)nw->child_count; ++i)
            if (ptM_ishole(nw, i)) {
                pt_Hole *nh = (pt_Hole *)ptP_alloc(S, &S->holes);
                *nh = *(pt_Hole *)child->children[i];
                nw->children[i] = (pt_Node *)nh;
            }
    return (*slot = nw);
}

static void ptK_cowpath(pt_Cursor *C) {
    int l;
    for (l = 0; l < ptK_levels(C); ++l) {
        int      rl = ptK_levels(C) - 1 - l;
        pt_Node *old, *nw = ptK_cownode(C, C->paths[l], &old, rl);
        if (nw != old)
            C->paths[l + 1] = nw->children + (C->paths[l + 1] - old->children);
    }
}

PT_STATIC int ptK_cowedit(pt_Cursor *C, pt_Cursor *R) {
    int l, levels = ptK_levels(C);
    int fl;
    for (l = 0; l < levels && C->paths[l] == R->paths[l]; ++l) {
        int      rl = levels - 1 - l;
        pt_Node *old, *nw = ptK_cownode(C, C->paths[l], &old, rl);
        if (nw != old) {
            C->paths[l + 1] = nw->children + (C->paths[l + 1] - old->children);
            R->paths[l + 1] = nw->children + (R->paths[l + 1] - old->children);
        }
    }
    fl = l;
    for (l = fl; l < levels; ++l) {
        int      rl = levels - 1 - l;
        pt_Node *old, *nw = ptK_cownode(C, C->paths[l], &old, rl);
        if (nw != old)
            C->paths[l + 1] = nw->children + (C->paths[l + 1] - old->children);
    }
    for (l = fl; l < levels; ++l) {
        int      rl = levels - 1 - l;
        pt_Node *old, *nw = ptK_cownode(R, R->paths[l], &old, rl);
        if (nw != old)
            R->paths[l + 1] = nw->children + (R->paths[l + 1] - old->children);
    }
    return fl;
}

static void ptM_movebits(pt_Node *dst, pt_Node *src, int from, int n) {
    int j;
    for (j = 0; j < n; ++j) ptM_sethole(dst, j, ptM_ishole(src, from + j));
}

static void ptM_setsubmask(pt_Node *p, int i, const pt_Node *child) {
    size_t w;
    for (w = 0; w < PT_MASK_SIZE; ++w)
        if (child->mask[w]) {
            ptM_sethole(p, i, 1);
            return;
        }
    ptM_sethole(p, i, 0);
}

static void ptM_upmask(pt_Cursor *C) {
    int l;
    for (l = ptK_levels(C) - 1; l >= 0; --l) {
        pt_Node *p = ptK_parent(C, l);
        pt_Node *c = ptK_parent(C, l + 1);
        ptM_setsubmask(p, ptK_idx(C, p, l), c);
    }
}

static void ptI_splitnode(pt_Cursor *C, int l) {
    pt_State *S = C->tree->S;
    int       rl = ptK_levels(C) - 1 - l;
    pt_Node  *o, *nw, *nd = ptK_cownode(C, C->paths[l], &o, rl), *p;
    int       i, cs, mid = nd->child_count / 2, nc = nd->child_count - mid;
    if (nd != o)
        C->paths[l + 1] = nd->children + (C->paths[l + 1] - o->children);
    p = ptK_parent(C, l), i = ptK_idx(C, p, l);
    nw = (pt_Node *)ptP_ralloc(&S->nodes), nw->version = C->tree->version;
    memset(nw->mask, 0, sizeof(nw->mask));
    memcpy(nw->children, &nd->children[mid], (size_t)nc * sizeof(pt_Node *));
    memcpy(nw->bytes, &nd->bytes[mid], (size_t)nc * sizeof(pt_Size));
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

static void ptI_splitroot(pt_Cursor *C) {
    pt_State *S = C->tree->S;
    pt_Node  *root = &C->tree->root, save = *root;
    pt_Node  *pp = (pt_Node *)ptP_ralloc(&S->nodes);
    pt_Node  *nw = (pt_Node *)ptP_ralloc(&S->nodes);
    int       i = ptK_idx(C, root, 0), mid = save.child_count / 2;
    int       nc = save.child_count - mid;
    *pp = save, pp->child_count = (unsigned short)mid;
    for (nc = mid; nc < (int)save.child_count; ++nc) ptM_sethole(pp, nc, 0);
    nc = save.child_count - mid;
    memset(nw->mask, 0, sizeof(nw->mask));
    memcpy(nw->children, &save.children[mid], (size_t)nc * sizeof(pt_Node *));
    memcpy(nw->bytes, &save.bytes[mid], (size_t)nc * sizeof(pt_Size));
    ptM_movebits(nw, &save, mid, nc);
    nw->child_count = (unsigned short)nc;
    pp->version = nw->version = C->tree->version;
    root->children[0] = pp, root->children[1] = nw, root->child_count = 2;
    root->bytes[0] = ptN_sumbytes(pp, 0, mid);
    root->bytes[1] = C->tree->bytes - root->bytes[0];
    ptM_setsubmask(root, 0, pp), ptM_setsubmask(root, 1, nw);
    C->tree->levels++;
    memmove(C->paths + 1, C->paths, C->tree->levels * sizeof(pt_Node **));
    C->paths[0] = &root->children[i >= mid];
    C->paths[1] = &(*C->paths[0])->children[i < mid ? i : i - mid];
}

/* Ensure the cursor's leaf node has `need` free slots, splitting full
 * ancestors down the path (or growing the root). The loop's last pass
 * (l=levels-1) splits the leaf node too (a pt_Node); pieces are atomic. */
static void ptI_makeroom(pt_Cursor *C, int need) {
    int l;
    for (l = ptK_levels(C); l >= 0; --l)
        if ((int)ptK_parent(C, l)->child_count + need <= PT_FANOUT) break;
    if (l < 0) {
        ptI_splitroot(C);
        l = 1;
    }
    for (; l < ptK_levels(C); ++l) ptI_splitnode(C, l);
}

static void ptI_onepiece(pt_Cursor *C, const char *s, size_t len) {
    pt_Node *root = &C->tree->root;
    root->children[0] = (pt_Node *)s, root->bytes[0] = (pt_Size)len;
    root->child_count = 1, ptM_sethole(root, 0, 0);
    C->tree->bytes = (pt_Size)len;
    C->paths[0] = &root->children[0], C->off = 0, C->poff = 0;
}

/* split the cursor's leaf piece i into [0,poff) at i and [poff,len) at i+1,
 * returning the index i+1 where a new literal is to be inserted between. */
static int ptI_midsplit(pt_Cursor *C, pt_Node *p, int i) {
    ptN_makespace(p, i + 1, 1);
    p->bytes[i + 1] = p->bytes[i] - C->poff, p->bytes[i] = C->poff;
    p->children[i + 1] = (pt_Node *)((char *)p->children[i] + C->poff);
    return ptM_sethole(p, i + 1, 0), i + 1;
}

/* insert first hole into an empty tree */
static void ptH_onepiece(pt_Cursor *C, const char *s, size_t len) {
    pt_State *S = C->tree->S;
    pt_Node  *root = &C->tree->root;
    pt_Hole  *h = ptH_new(S, s, len);
    root->children[0] = (pt_Node *)h;
    root->bytes[0] = (pt_Size)len;
    root->child_count = 1;
    ptM_sethole(root, 0, 1);
    C->tree->bytes = (pt_Size)len;
    C->paths[0] = &root->children[0];
    C->off = 0;
    C->poff = (pt_Size)len;
}

/* insert a fresh hole slot at cursor position, reusing ptI machine.
   pre: tree non-empty, C->dirty, reserves already held */
static void ptH_putslot(pt_Cursor *C, const char *s, size_t len) {
    pt_State *S = C->tree->S;
    pt_Node  *p;
    int       l, i, ins, mid;
    pt_Offset off0 = (pt_Offset)(C->off + C->poff);
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
        p->bytes[ins] = (pt_Size)len;
        ptM_sethole(p, ins, 1);
    }
    C->paths[l] = &p->children[ins];
    ptM_upbytes(C, l - 1, (pt_Diff)len);
    ptM_upmask(C); /* 开放点2: 插 hole 后必须传播 mask */
    C->off = off0;
    C->poff = (pt_Size)len;
}

/* split hole at cursor when n+len > CAP: left-orig + fresh + right, net +2
   slots. pre: cursor in hole middle (0<poff<n), C->dirty, reserves held */
static void ptH_splitins(pt_Cursor *C, const char *s, size_t len) {
    pt_State *S = C->tree->S;
    pt_Node  *p;
    pt_Hole  *h, *rh, *nh;
    size_t    poff, n;
    int       l, i;
    pt_Offset off0 = (pt_Offset)(C->off + C->poff);
    l = ptK_levels(C);
    p = ptK_parent(C, l);
    i = ptK_idx(C, p, l);
    h = (pt_Hole *)p->children[i];
    n = (size_t)h->n;
    poff = C->poff;
    rh = ptH_new(S, h->data + poff, n - poff); /* right half */
    h->n = (unsigned short)poff;               /* left half shrunk */
    p->bytes[i] = (pt_Size)poff;
    ptI_makeroom(C, 2);
    l = ptK_levels(C);
    p = ptK_parent(C, l);
    i = ptK_idx(C, p, l);
    ptN_makespace(p, i + 1, 2);
    nh = ptH_new(S, s, len); /* fresh middle */
    p->children[i + 1] = (pt_Node *)nh;
    p->bytes[i + 1] = (pt_Size)len;
    ptM_sethole(p, i + 1, 1);
    p->children[i + 2] = (pt_Node *)rh;
    p->bytes[i + 2] = (pt_Size)(n - poff);
    ptM_sethole(p, i + 2, 1);
    C->paths[l] = &p->children[i + 1];
    ptM_upbytes(C, l - 1, (pt_Diff)len);
    ptM_upmask(C);
    C->off = off0;
    C->poff = (pt_Size)len;
}

/* hole insert dispatcher: 4 branches A/B/C/D.
   pre: C->dirty, reserves already held, tree non-empty.
   post: cursor at insertion end (off+poff increased by len). */
static void ptH_insert(pt_Cursor *C, const char *s, size_t len) {
    pt_Node *p;
    int      l, i;
    size_t   n;
    assert(C->dirty);
    if (C->tree->root.child_count == 0) {
        ptH_onepiece(C, s, len);
        return;
    }
    l = ptK_levels(C);
    p = ptK_parent(C, l);
    i = ptK_idx(C, p, l);
    /* A: current piece is a hole, cursor at tail, fits */
    if (ptM_ishole(p, i) && C->poff == p->bytes[i]) {
        pt_Hole *h = (pt_Hole *)p->children[i];
        if ((size_t)h->n + len <= PT_MAX_HOLESIZE) {
            ptH_append(h, s, len);
            p->bytes[i] = h->n;
            C->poff += (pt_Size)len;
            ptM_upbytes(C, l - 1, (pt_Diff)len);
            ptM_upmask(C);
            return;
        }
        /* hole full -> fall through to D (fresh slot) */
    }
    /* B: cursor in hole middle */
    if (ptM_ishole(p, i) && C->poff > 0 && C->poff < p->bytes[i]) {
        pt_Hole *h = (pt_Hole *)p->children[i];
        n = (size_t)h->n;
        if (n + len <= PT_MAX_HOLESIZE) {
            ptH_insmid(h, C->poff, s, len);
            p->bytes[i] = h->n;
            C->poff += (pt_Size)len;
            ptM_upbytes(C, l - 1, (pt_Diff)len);
            ptM_upmask(C);
            return;
        }
        ptH_splitins(C, s, len);
        return;
    }
    /* C: poff==0 and previous piece is a hole that fits */
    if (C->poff == 0 && i > 0 && ptM_ishole(p, i - 1)) {
        pt_Hole *prev = (pt_Hole *)p->children[i - 1];
        if ((size_t)prev->n + len <= PT_MAX_HOLESIZE) {
            ptH_append(prev, s, len);
            p->bytes[i - 1] = prev->n;
            C->paths[l] = &p->children[i - 1];
            C->poff = prev->n;
            C->off -= p->bytes[i - 1] - (pt_Size)len;
            ptM_upbytes(C, l - 1, (pt_Diff)len);
            ptM_upmask(C);
            return;
        }
        /* prev hole full -> fall through to D */
    }
    /* D: fresh hole slot */
    ptH_putslot(C, s, len);
}

static int ptK_beginedit(pt_Cursor *C, size_t need) {
    int r;
    if (!ptP_reserve(C->tree->S, &C->tree->S->nodes, need)) return PT_ERRMEM;
    if ((r = ptK_fork(C)) != PT_OK) return r;
    return ptK_cowpath(C), PT_OK;
}

static void ptR_cutpiece(
        pt_Cursor *C, pt_Node *p, int i, size_t lo, size_t hi) {
    pt_State *S = C->tree->S;
    size_t    len = p->bytes[i];
    if (ptM_ishole(p, i)) {
        pt_Hole *h = (pt_Hole *)p->children[i];
        ptH_delmid(h, lo, hi);
        p->bytes[i] = h->n;
        if (h->n == 0) {
            ptH_free(S, h);
            ptN_delslot(p, i);
            if (ptK_idx(C, p, ptK_levels(C)) > i) C->paths[ptK_levels(C)] -= 1;
        }
        return;
    }
    if (lo == 0 && hi == len) {
        ptN_delslot(p, i);
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

static void ptR_rebalance(pt_Cursor *C, int l);

static void ptR_eraseleaf(pt_Cursor *C, pt_Cursor *R) {
    pt_State *S = C->tree->S;
    int       l = ptK_levels(C), dF, dE, j;
    pt_Node  *p = ptK_parent(C, l);
    int       iL = ptK_idx(C, p, l), iR = ptK_idx(R, p, l);
    size_t    pL = C->poff, pR = R->poff;
    size_t    de = (size_t)(pt_offset(R) - pt_offset(C));
    if (iL == iR) {
        ptR_cutpiece(C, p, iL, pL, pR);
        if (p->child_count == 0) {
            C->paths[l] = &p->children[0];
            C->off = 0;
            C->poff = 0;
        }
        ptM_upbytes(C, l - 1, -(pt_Diff)de);
        return;
    }
    if (pL > 0) {
        if (ptM_ishole(p, iL))
            ((pt_Hole *)p->children[iL])->n = (unsigned short)pL;
        p->bytes[iL] = pL;
        dF = iL + 1;
    } else {
        if (ptM_ishole(p, iL)) ptH_free(S, (pt_Hole *)p->children[iL]);
        dF = iL;
    }
    for (j = iL + 1; j < iR; ++j)
        if (ptM_ishole(p, j)) ptH_free(S, (pt_Hole *)p->children[j]);
    if (pR > 0 && pR < p->bytes[iR]) {
        if (ptM_ishole(p, iR))
            ptH_trimhead((pt_Hole *)p->children[iR], pR);
        else
            p->children[iR] = (pt_Node *)((char *)p->children[iR] + pR);
        p->bytes[iR] -= pR;
        dE = iR;
    } else if (pR == p->bytes[iR]) {
        if (ptM_ishole(p, iR)) ptH_free(S, (pt_Hole *)p->children[iR]);
        dE = iR + 1;
    } else {
        dE = iR;
    }
    ptN_delrange(p, dF, dE);
    ptM_upbytes(C, l - 1, -(pt_Diff)de);
    if (p->child_count == 0) {
        C->paths[l] = &p->children[0];
        C->off = 0;
        C->poff = 0;
    } else if (pL == 0) {
        if (dF >= (int)p->child_count)
            ptK_locend(C);
        else {
            C->paths[l] = &p->children[dF];
            C->poff = 0;
        }
    } else {
        C->poff = pL;
    }
    if (l > 0) {
        ptR_rebalance(C, l - 1);
        ptM_upmask(C);
    }
}

/* === cross-leaf container deletion helpers === */

static void ptR_erasechildren(
        pt_State *S, pt_Node *p, int rl, int s, int e, unsigned ver) {
    int j;
    if (rl == 0) {
        for (j = s; j < e; ++j)
            if (ptM_ishole(p, j)) ptP_free(&S->holes, (void *)p->children[j]);
    } else {
        for (j = s; j < e; ++j) {
            pt_Node *c = (pt_Node *)p->children[j];
            if (c->version == ver) {
                ptR_freechildren(S, c, rl - 1, ver);
                ptP_free(&S->nodes, c);
            }
        }
    }
    ptN_delrange(p, s, e);
}

static void ptR_trimright(pt_Cursor *L, int l) {
    pt_State *S = L->tree->S;
    pt_Node  *p = ptK_parent(L, l);
    int       i = ptK_idx(L, p, l);
    size_t    poff = L->poff;
    int       cc = (int)p->child_count;
    int       keep = i + (poff > 0 ? 1 : 0);
    int       j;
    size_t    db = ptN_sumbytes(p, i, cc) - poff;

    if (poff > 0) {
        if (ptM_ishole(p, i))
            ((pt_Hole *)p->children[i])->n = (unsigned short)poff;
        p->bytes[i] = poff;
    }
    /* when poff==0: loop below frees holes from index keep(=i) onward */
    for (j = keep; j < cc; ++j)
        if (ptM_ishole(p, j)) ptH_free(S, (pt_Hole *)p->children[j]);
    p->child_count = (unsigned short)keep;
    ptM_upbytes(L, l - 1, -(pt_Diff)db);
}

static void ptR_trimleft(pt_Cursor *R, int l) {
    pt_State *S = R->tree->S;
    pt_Node  *p = ptK_parent(R, l);
    int       i = ptK_idx(R, p, l);
    size_t    poff = R->poff;
    int       delStart;
    int       j;
    size_t    db;

    if (poff > 0 && poff < p->bytes[i]) {
        if (ptM_ishole(p, i))
            ptH_trimhead((pt_Hole *)p->children[i], poff);
        else
            p->children[i] = (pt_Node *)((char *)p->children[i] + poff);
        p->bytes[i] -= poff;
        delStart = i;
        db = ptN_sumbytes(p, 0, i) + poff;
    } else if (poff == p->bytes[i]) {
        if (ptM_ishole(p, i)) ptH_free(S, (pt_Hole *)p->children[i]);
        delStart = i + 1;
        db = ptN_sumbytes(p, 0, i + 1);
    } else {
        delStart = i;
        db = ptN_sumbytes(p, 0, i);
    }
    for (j = 0; j < delStart; ++j)
        if (ptM_ishole(p, j)) ptH_free(S, (pt_Hole *)p->children[j]);
    if (delStart > 0) ptN_delrange(p, 0, delStart);
    ptM_upbytes(R, l - 1, -(pt_Diff)db);
}

static void ptR_cutrange(pt_Cursor *L, pt_Cursor *R, pt_Node *rt, int fl) {
    pt_State *S = L->tree->S;
    int       lvls = ptK_levels(L);
    unsigned  ver = L->tree->version;
    int       kl, k;

    for (kl = lvls - 1; kl > fl; --kl) {
        pt_Node *p;
        int      i, cc;
        size_t   db;

        p = ptK_parent(L, kl), i = ptK_idx(L, p, kl), cc = (int)p->child_count;
        db = ptN_sumbytes(p, i + 1, cc);
        ptM_upbytes(L, kl - 1, -(pt_Diff)db);
        ptR_erasechildren(S, p, lvls - kl, i + 1, cc, ver);

        k = (lvls - 1) - kl;
        p = ptK_parent(R, kl), i = ptK_idx(R, p, kl), cc = (int)p->child_count;
        rt[k].child_count = (unsigned short)(cc - i);
        ptN_copy(&rt[k], 0, p, i, (int)rt[k].child_count);
        ptR_erasechildren(S, p, lvls - kl, 0, i, ver);
        memset(p->bytes, 0, (size_t)cc * sizeof(pt_Size));
        p->child_count = 0;
    }
    /* divergence level fl */
    {
        pt_Node *p = ptK_parent(R, fl);
        int      i = ptK_idx(R, p, fl);
        int      cc = (int)p->child_count;
        int      rl = lvls - fl;
        pt_Size  occ = (pt_Size)cc;
        size_t   db;

        k = (lvls - 1) - fl;
        rt[k].child_count = (unsigned short)(cc - i);
        ptN_copy(&rt[k], 0, p, i, (int)rt[k].child_count);
        /* correct rt bytes: children may have been emptied by loops above */
        {
            int _j;
            for (_j = 0; _j < (int)rt[k].child_count; ++_j) {
                pt_Node *_ch = (pt_Node *)rt[k].children[_j];
                rt[k].bytes[_j] = ptN_sumbytes(_ch, 0, (int)_ch->child_count);
            }
        }
        p->child_count = (unsigned short)i;
        /* L side at fl: delete middle [iL+1..i) */

        i = ptK_idx(L, p, fl);
        cc = (int)p->child_count;
        db = ptN_sumbytes(p, i + 1, (int)occ);
        ptM_upbytes(L, fl - 1, -(pt_Diff)db);
        ptR_erasechildren(S, p, rl, i + 1, cc, ver);
    }
}

static int ptR_makechain(pt_Cursor *C, int from, int to) {
    pt_Node *p, *nn;
    int      l, r = 0;
    if (from < 0) {
        nn = (pt_Node *)ptP_ralloc(&C->tree->S->nodes);
        p = &C->tree->root, *nn = *p;
        p->bytes[0] = C->tree->bytes;
        memset(p->mask, 0, sizeof(p->mask));
        p->children[0] = nn, p->child_count = 1;
        memmove(C->paths + to + 2, C->paths + to + 1,
                (size_t)(ptK_levels(C) - to) * sizeof(pt_Node **));
        C->tree->levels += 1, from = 0, to += 1, r = 1;
    }
    for (l = from; l < to; ++l) {
        nn = (pt_Node *)ptP_ralloc(&C->tree->S->nodes);
        p = ptK_parent(C, l), nn->child_count = 0;
        nn->version = C->tree->version;
        memset(nn->mask, 0, sizeof(nn->mask));
        p->bytes[p->child_count] = 0;
        p->children[p->child_count] = nn, p->child_count++;
        C->paths[l] = &p->children[p->child_count - 1];
    }
    C->paths[to] = &nn->children[0];
    return r;
}

static int ptR_findroom(pt_Cursor *C, pt_Node *rt, int l) {
    int      i, fl, c;
    pt_Node *p;
    for (fl = l - 1; fl >= 0; --fl) {
        p = ptK_parent(C, fl), i = ptK_idx(C, p, fl);
        if (i < PT_FANOUT - 1) break;
    }
    if (fl >= 0 && (c = (int)p->child_count - i - 1) > 0) {
        int    k = ptK_levels(C) - fl;
        size_t db = ptN_sumbytes(p, i + 1, p->child_count);
        ptM_upbytes(C, fl - 1, -(pt_Diff)db);
        rt[k].child_count = 0;
        ptN_copy(&rt[k], 0, p, i + 1, c);
        p->child_count = (unsigned short)(i + 1);
        rt[k].child_count = (unsigned short)c;
    }
    return ptR_makechain(C, fl, l);
}

static void ptR_backwardnode(pt_Cursor *C, int d, int l) {
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

static void ptR_mergeleaf(pt_Cursor *L, pt_Node *rt) {
    pt_State *S = L->tree->S;
    int       lvls = ptK_levels(L);
    pt_Node  *PL = ptK_parent(L, lvls);
    pt_Node  *PR;
    int       m;

    if (rt[0].child_count == 0) return;
    PR = (pt_Node *)rt[0].children[0];
    if (PR->child_count == 0) return;
    m = (int)pt_min(
            (size_t)PR->child_count, (size_t)(PT_FANOUT - PL->child_count));
    if (m > 0) {
        ptN_copy(PL, (int)PL->child_count, PR, 0, m);
        PL->child_count += (unsigned short)m;
        ptM_upbytes(L, lvls - 1, (pt_Diff)ptN_sumbytes(PR, 0, m));
    }
    if (m == (int)PR->child_count) {
        ptP_free(&S->nodes, PR);
        ptN_delrange(&rt[0], 0, 1);
    } else {
        ptN_delrange(PR, 0, m);
    }
}

static int ptR_checkstitch(pt_Cursor *C) {
    return ptP_reserve(
            C->tree->S, &C->tree->S->nodes, (size_t)ptK_levels(C) + 2);
}

static void ptR_stitchnode(pt_Cursor *L, pt_Node *rt) {
    int k, d = 0, l = ptK_levels(L);
    for (k = 0; k <= ptK_levels(L); ++k) {
        int      kl = ptK_levels(L) - k;
        int      rtcc = (int)rt[k].child_count;
        int      m;
        pt_Node *p;

        rt[k].child_count = 0;
        p = ptK_parent(L, kl);
        m = (int)pt_min((size_t)rtcc, (size_t)(PT_FANOUT - p->child_count));
        if (m > 0) {
            size_t db = ptN_sumbytes(&rt[k], 0, m);
            ptN_copy(p, (int)p->child_count, &rt[k], 0, m);
            p->child_count += (unsigned short)m;
            ptM_upbytes(L, kl - 1, (pt_Diff)db);
        }
        if (!(m < rtcc || kl == 0)) continue;
        if (k) ptR_backwardnode(L, d, l);
        if (!(m < rtcc)) continue;
        {
            int kl2 = kl;
            d = k ? PT_FANOUT - ptK_idx(L, ptK_parent(L, kl2), kl2) : m;
        }
        {
            int    r = ptR_findroom(L, rt, l = kl);
            size_t db;
            l += r;
            p = ptK_parent(L, l);
            ptN_copy(p, 0, &rt[k], m, rtcc - m);
            p->child_count = (unsigned short)(rtcc - m);
            db = ptN_sumbytes(&rt[k], m, rtcc);
            ptM_upbytes(L, l - 1, (pt_Diff)db);
        }
    }
}

static void ptR_stitch(pt_Cursor *L, pt_Node *rt) {
    int l = ptK_levels(L);
    ptR_checkstitch(L);
    ptR_mergeleaf(L, rt);
    ptR_stitchnode(L, rt);
    if (l > 0) ptR_rebalance(L, l - 1);
    ptM_upmask(L);
}

static void ptR_eraserange(pt_Cursor *L, pt_Cursor *R, int fl) {
    pt_Node rt[PT_MAX_LEVEL];
    int     l, lvls = ptK_levels(L);
    for (l = 0; l < PT_MAX_LEVEL; ++l) {
        memset(&rt[l], 0, sizeof(pt_Node));
        rt[l].version = L->tree->version;
    }
    ptR_trimright(L, lvls);
    ptR_trimleft(R, lvls);
    ptR_cutrange(L, R, rt, fl);
    ptR_stitch(L, rt);
}

static int ptR_balancenode(pt_Node **ns, int left, pt_Diff *ds) {
    int d, l = ns[0]->child_count, r = ns[1]->child_count;
    int j;
    d = l - ((l + r + (left != 0)) >> 1);
    assert(d != 0);
    if (d < 0) {
        ptN_copy(ns[0], l, ns[1], 0, -d);
        memmove(&ns[1]->children[0], &ns[1]->children[-d],
                (size_t)(r + d) * sizeof(pt_Node *));
        memmove(&ns[1]->bytes[0], &ns[1]->bytes[-d],
                (size_t)(r + d) * sizeof(pt_Size));
        for (j = 0; j < r + d; ++j)
            ptM_sethole(ns[1], j, ptM_ishole(ns[1], j - d));
        *ds = -(pt_Diff)ptN_sumbytes(ns[0], l, l - d);
    } else {
        memmove(&ns[1]->children[d], &ns[1]->children[0],
                (size_t)r * sizeof(pt_Node *));
        memmove(&ns[1]->bytes[d], &ns[1]->bytes[0],
                (size_t)r * sizeof(pt_Size));
        for (j = r - 1; j >= 0; --j)
            ptM_sethole(ns[1], j + d, ptM_ishole(ns[1], j));
        ptN_copy(ns[1], 0, ns[0], l - d, d);
        *ds = (pt_Diff)ptN_sumbytes(ns[1], 0, d);
    }
    ns[0]->child_count = (unsigned short)(l - d);
    ns[1]->child_count = (unsigned short)(r + d);
    return d;
}

static int ptR_foldnode(pt_Cursor *C, int lfirst, int l) {
    pt_Node  *p = ptK_parent(C, l);
    int       i = ptK_idx(C, p, l);
    pt_Node **ns = &p->children[i];
    pt_Node  *o = *ns;
    pt_Diff   ds;
    int       cl, cr;
    unsigned  w;
    assert(p->child_count > 1);
    if (ns[0]->child_count > PT_FANOUT / 2) return 0;
    if ((i && lfirst) || i == (int)p->child_count - 1) ns -= 1, i -= 1;
    cl = ns[0]->child_count, cr = ns[1]->child_count;
    if (cl + cr <= PT_FANOUT) {
        ptN_copy(ns[0], cl, ns[1], 0, cr);
        ns[0]->child_count += (unsigned short)cr;
        ns[1]->child_count -= (unsigned short)cr;
        p->bytes[i] += p->bytes[i + 1];
        for (w = 0; w < PT_MASK_SIZE; ++w) ns[0]->mask[w] |= ns[1]->mask[w];
        if (C->paths[l] == &p->children[i + 1]) {
            int childoff = (int)(C->paths[l + 1] - ns[1]->children);
            C->paths[l] = &p->children[i];
            C->paths[l + 1] = &ns[0]->children[cl + childoff];
        }
        ptP_free(&C->tree->S->nodes, ns[1]);
        ptN_delslot(p, i + 1);
        return 1;
    }
    {
        int dn = ptR_balancenode(ns, (ns[0] == o), &ds);
        p->bytes[i] -= ds;
        p->bytes[i + 1] += ds;
        if (ns[0] != o) C->paths[l + 1] += dn;
        return 0;
    }
}

static void ptR_rebalance(pt_Cursor *C, int l) {
    pt_State *S = C->tree->S;
    for (; l > 0; --l) {
        pt_Node *p = ptK_parent(C, l);
        pt_Node *child = p->children[ptK_idx(C, p, l)];
        if (child->child_count >= PT_FANOUT / 2) return;
        if (p->child_count > 1 && !ptR_foldnode(C, 0, l)) return;
    }
    while (ptK_levels(C) > 0 && C->tree->root.child_count == 1) {
        pt_Node *only = ptK_parent(C, 1);
        int      i = ptK_idx(C, only, 1);
        C->tree->root = *only;
        ptP_free(&S->nodes, only);
        C->tree->levels--;
        C->paths[0] += i;
        memmove(C->paths + 1, C->paths + 2,
                (size_t)ptK_levels(C) * sizeof(pt_Node **));
    }
}

static void ptK_mergelit(pt_Cursor *C) {
    int      l = ptK_levels(C);
    pt_Node *p = ptK_parent(C, l);
    int      i = ptK_idx(C, p, l);
    if (i + 1 < (int)p->child_count && !ptM_ishole(p, i)
        && !ptM_ishole(p, i + 1)
        && (char *)p->children[i] + p->bytes[i] == (char *)p->children[i + 1])
        p->bytes[i] += p->bytes[i + 1], ptN_delslot(p, i + 1);
    if (i > 0 && !ptM_ishole(p, i - 1) && !ptM_ishole(p, i)
        && (char *)p->children[i - 1] + p->bytes[i - 1]
                   == (char *)p->children[i]) {
        pt_Size g = p->bytes[i - 1];
        p->bytes[i - 1] += p->bytes[i], ptN_delslot(p, i);
        C->paths[l] = &p->children[i - 1], C->off -= g, C->poff += g;
    }
}

PT_API int pt_insert(pt_Cursor *C, const char *s, size_t len) {
    pt_Node  *p;
    pt_Offset off;
    int       l, i, ins, mid, r;
    if (C == NULL || C->tree == NULL || s == NULL) return PT_ERRPARAM;
    if (len == 0) return PT_OK;
    if ((r = ptK_beginedit(C, 2 * (size_t)ptK_levels(C) + 3)) != PT_OK)
        return r;
    off = (pt_Offset)(C->off + C->poff);
    if (C->tree->root.child_count == 0) return ptI_onepiece(C, s, len), PT_OK;
    l = ptK_levels(C), p = ptK_parent(C, l), i = ptK_idx(C, p, l);
    mid = (C->poff > 0 && C->poff < p->bytes[i]);
    ptI_makeroom(C, mid ? 2 : 1);
    l = ptK_levels(C), p = ptK_parent(C, l), i = ptK_idx(C, p, l);
    ins = mid ? ptI_midsplit(C, p, i) : i + (C->poff > 0);
    ptN_makespace(p, ins, 1);
    p->children[ins] = (pt_Node *)s, p->bytes[ins] = (pt_Size)len;
    ptM_sethole(p, ins, 0), C->paths[l] = &p->children[ins];
    ptM_upbytes(C, l - 1, (pt_Diff)len);
    C->off = off, C->poff = 0;
    return ptK_mergelit(C), PT_OK;
}

PT_API int pt_append(pt_Cursor *C, const char *s, size_t len) {
    int r = pt_insert(C, s, len);
    if (r == PT_OK && len > 0) pt_advance(C, (pt_Offset)len);
    return r;
}

PT_API int pt_splice(pt_Cursor *C, size_t del, const char *s, size_t len) {
    int r, lvls;
    if (!C || !C->tree) return PT_ERRPARAM;
    if (del == 0 && (s == NULL || len == 0)) return PT_OK;
    lvls = ptK_levels(C);
    if (!ptP_reserve(C->tree->S, &C->tree->S->nodes, 6 * (size_t)lvls + 8))
        return PT_ERRMEM;
    r = pt_remove(C, del);
    if (r != PT_OK) return r;
    if (s != NULL && len > 0) r = pt_append(C, s, len);
    return r;
}

PT_API int pt_remove(pt_Cursor *C, size_t len) {
    pt_Cursor R;
    pt_Tree  *old;
    size_t    dellen;
    int       r, lvls;

    if (!C || !C->tree) return PT_ERRPARAM;
    if (len == 0 || ptK_bytes(C) == 0) return PT_OK;
    if ((size_t)(C->off + C->poff) + len > ptK_bytes(C))
        len = (size_t)(ptK_bytes(C) - (C->off + C->poff));
    R = *C, pt_advance(&R, (pt_Offset)len);
    dellen = (size_t)(pt_offset(&R) - pt_offset(C));
    if (dellen == 0) return PT_OK;
    lvls = ptK_levels(C);
    r = ptP_reserve(C->tree->S, &C->tree->S->nodes, 4 * (size_t)lvls + 5);
    if (!r) return PT_ERRMEM;
    old = C->tree, r = ptK_fork(C);
    if (r != PT_OK) return r;
    R.tree = C->tree;
    R.paths[0] = C->tree->root.children + (R.paths[0] - old->root.children);
    if (lvls == 0 || *C->paths[lvls - 1] == *R.paths[lvls - 1]) {
        ptK_cowedit(C, &R);
        ptR_eraseleaf(C, &R);
    } else {
        int fl = ptK_cowedit(C, &R);
        ptR_eraserange(C, &R, fl);
    }
    ptK_mergelit(C);
    return PT_OK;
}

PT_API int pt_edit(pt_Cursor *C, size_t del, const char *s, size_t len) {
    int r, lvls;
    if (!C || !C->tree) return PT_ERRPARAM;
    if (len > PT_MAX_HOLESIZE) return PT_ERRPARAM;
    if (s == NULL && len > 0) return PT_ERRPARAM;
    if (del == 0 && len == 0) return PT_OK;
    lvls = ptK_levels(C);
    if (!ptP_reserve(C->tree->S, &C->tree->S->nodes, 6 * (size_t)lvls + 8))
        return PT_ERRMEM;
    if (len > 0 && !ptP_reserve(C->tree->S, &C->tree->S->holes, 2))
        return PT_ERRMEM;
    if (del > 0) {
        r = pt_remove(C, del);
        if (r != PT_OK) return r;
    }
    if (len > 0) {
        if (!C->dirty) {
            r = ptK_beginedit(C, 2 * (size_t)lvls + 3);
            if (r != PT_OK) return r;
        }
        ptH_insert(C, s, len);
    }
    return PT_OK;
}

PT_NS_END

#endif /* PT_IMPLEMENTATION */
