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

typedef const struct pt_Tree *pt_Snapshot;

typedef ptrdiff_t pt_Offset;
typedef size_t    pt_Size;

typedef struct pt_LineCol {
    pt_Offset line;
    pt_Offset col;
} pt_LineCol;

typedef void *pt_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);

PT_API pt_State *pt_newstate(pt_Alloc *allocf, void *ud);
PT_API void      pt_close(pt_State *S);

PT_API pt_Alloc *pt_getallocf(pt_State *S, void **pud);

/* snapshot */

PT_API pt_Snapshot pt_empty(pt_State *S);
PT_API pt_Snapshot pt_from(pt_State *S, const char *s, size_t len);

PT_API int  pt_version(pt_Snapshot snap);
PT_API void pt_retain(pt_Snapshot snap);
PT_API void pt_release(pt_Snapshot snap);

PT_API size_t pt_bytes(pt_Snapshot snap);
PT_API size_t pt_chars(pt_Snapshot snap);
PT_API size_t pt_lines(pt_Snapshot snap);

/* cursor */

PT_API int pt_locate(pt_Cursor *out, pt_Snapshot snap, pt_Offset off);
PT_API int pt_locline(pt_Cursor *out, pt_Snapshot snap, pt_LineCol linecol);

PT_API int pt_advance(pt_Cursor *c, pt_Offset delta);
PT_API int pt_advline(pt_Cursor *c, pt_Offset delta);
PT_API int pt_setcol(pt_Cursor *c, pt_Size column);

PT_API pt_Offset  pt_bytepos(const pt_Cursor *c);
PT_API pt_LineCol pt_linepos(const pt_Cursor *c);
PT_API pt_Size    pt_remainbytes(const pt_Cursor *c);

PT_API char *pt_buffer(pt_State *S, size_t *plen);

PT_API const char *pt_peek(const pt_Cursor *c, pt_Size *len);

PT_API int pt_next(pt_Cursor *c);
PT_API int pt_prev(pt_Cursor *c);

PT_API int pt_insert(pt_Cursor *c, const char *s, size_t len);
PT_API int pt_remove(pt_Cursor *c, pt_Cursor *end);
PT_API int pt_replace(pt_Cursor *c, const char *s, size_t len);

PT_API void        pt_rollback(pt_Cursor *c);
PT_API pt_Snapshot pt_commit(pt_Cursor *c);

/* clang-format off */
PT_STATIC pt_LineCol pt_linecol(pt_Offset line, pt_Offset col)
{ pt_LineCol lc; lc.line = line; lc.col = col; return lc; }
/* clang-format on */

/* callbacks */

#define PT_PIECE_MAXLINES 58    /* maximum lines in a piece */
#define PT_PIECE_MAXSIZE  65535 /* maximum bytes in a piece */

typedef unsigned short pt_PieceSize;
typedef pt_PieceSize   pt_Ends[PT_PIECE_MAXLINES];

typedef struct pt_LinesInfo {
    pt_PieceSize chars;  /* total chars in result */
    pt_PieceSize breaks; /* count of linebreaks in result */
    pt_Ends     *ends;   /* offset to the linebreak+1 in result */
} pt_LinesInfo;

typedef struct pt_Slice {
    const char  *s;
    pt_PieceSize len;
} pt_Slice;

typedef pt_PieceSize pt_Chars(void *ud, pt_Slice s, pt_PieceSize *pcol);
typedef pt_PieceSize pt_Lines(void *ud, pt_Slice s, pt_LinesInfo *pli);

PT_API pt_Chars *pt_getcharsf(pt_State *S, void **pud);
PT_API pt_Lines *pt_getlinesf(pt_State *S, void **pud);
PT_API void      pt_setcharsf(pt_State *S, pt_Chars *charsf, void *ud);
PT_API void      pt_setlinesf(pt_State *S, pt_Lines *linesf, void *ud);

/* cursor definition */

#define PT_MAX_LEVEL 16

struct pt_Cursor {
    /*
     * Each entry points to a slot inside a parent's children[] array.
     * paths[0] == &snap->root->children[idx],
     * paths[i] == &parent->children[idx],
     * paths[snap->levels] is the slot of current piece for non-empty tree.
     * Leaf slots are pt_Piece* and must be cast after a leaf check.
     */
    struct pt_Node **paths[PT_MAX_LEVEL];
    struct pt_Tree  *snap;    /* snapshot this cursor operates on */
    struct pt_Tree  *oldsnap; /* committed snapshot before mutation, for rollback */
    short            dirty;   /* non-zero if cursor has uncommitted changes */
    unsigned short   pbr;     /* line breaks before poff in current piece */
    unsigned short   poff;    /* byte offset within current piece */
    pt_Offset        off;     /* absolute byte offset in the tree */
    pt_Offset        remain;  /* bytes remaining to next line break (cached from gap) */
    pt_LineCol       linecol; /* line and column in the tree; col may be -1 (unknown) */
};

PT_NS_END

#endif /* piecetab_h */

#if defined(PT_IMPLEMENTATION) && !defined(pt_implemented)
#define pt_implemented

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifndef PT_MAX_FANOUT
# define PT_MAX_FANOUT 62
#endif

#ifndef PT_PAGE_SIZE
# define PT_PAGE_SIZE 65536
#endif

#define pt_min(a, b) ((a) < (b) ? (a) : (b))
#define pt_max(a, b) ((a) > (b) ? (a) : (b))

#define PT_STATIC_ASSERT(cond)      PT_SA_0(cond, pt_SA_, __LINE__)
#define PT_SA_0(cond, prefix, line) PT_SA_1(cond, prefix, line)
#define PT_SA_1(cond, prefix, line) typedef char prefix##line[(cond) ? 1 : -1]

PT_NS_BEGIN

typedef struct pt_Node pt_Node;

typedef struct pt_Piece {
    pt_PieceSize ends[PT_PIECE_MAXLINES]; /* offset to linebreak+1 within data */
    unsigned     version;                 /* tree version when this piece was written */
    const char  *data;                    /* pointer into pt_buffer's memory */
} pt_Piece;

PT_STATIC_ASSERT(sizeof(pt_Piece) <= 128);

struct pt_Node {
    pt_Size        bytes[PT_MAX_FANOUT];  /* total bytes in this subtree */
    pt_Size        chars[PT_MAX_FANOUT];  /* total chars in this subtree */
    pt_Size        breaks[PT_MAX_FANOUT]; /* total linebreaks in this subtree */
    unsigned       version;               /* version of this node */
    unsigned short child_count;           /* count of children */
    pt_Node       *children[PT_MAX_FANOUT]; /* pt_Piece if it's a leaf */
};

PT_STATIC_ASSERT(sizeof(pt_Node) <= 2048);

typedef struct pt_Metrics {
    pt_Size bytes;  /* total bytes in subtree */
    pt_Size breaks; /* total line breaks */
    pt_Size chars;  /* total characters */
} pt_Metrics;

struct pt_Tree {
    pt_State *S; /* state this tree belongs to */

    pt_Metrics metrics; /* metrics of the whole tree */
    pt_Node    root;    /* root node of the tree */

    unsigned refcount; /* reference count */
    unsigned levels;   /* number of levels in the tree */
    unsigned version;  /* version of this tree */
};

PT_STATIC_ASSERT(sizeof(pt_Tree) <= 2048);

typedef struct pt_Pool {
    size_t obj_size; /* size of each object in this pool */
    void  *freed;    /* freelist head */
    void  *pages;    /* linked list of allocated pages */
} pt_Pool;

typedef struct pt_BufferPool {
    size_t remain; /* remaining bytes in current buffer */
    char  *buffer;  /* current write position */
    void  *pages;   /* linked list of allocated pages */
} pt_BufferPool;

struct pt_State {
    void         *alloc_ud;   /* user data for allocf */
    pt_Alloc     *allocf;     /* memory allocator */
    void         *chars_ud;   /* user data for charsf */
    pt_Chars     *charsf;     /* character counting callback */
    void         *lines_ud;   /* user data for linesf */
    pt_Lines     *linesf;     /* line break scanning callback */
    pt_BufferPool buffers;    /* pool for piece data buffers */
    pt_Pool       trees;      /* pool for pt_Tree objects */
    pt_Pool       nodes;      /* pool for pt_Node objects */
    pt_Pool       pieces;     /* pool for pt_Piece objects */
    unsigned      max_version; /* highest version assigned so far */
};

/* mempool */

/* clang-format off */
static void pt_poolfree(pt_Pool* pool, void* obj)
{ *(void**)obj = pool->freed, pool->freed = obj; }

static void pt_initbuffers(pt_BufferPool* pool)
{ memset(pool, 0, sizeof(pt_BufferPool)); }

static pt_Slice pt_slice(const char *s, pt_PieceSize len)
{ pt_Slice slice; slice.s = s, slice.len = len; return slice; }
/* clang-format on */

static void pt_initpool(pt_Pool *pool, size_t obj_size) {
    memset(pool, 0, sizeof(pt_Pool));
    pool->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < PT_PAGE_SIZE / 4);
}

static void pt_freepool(pt_State *S, pt_Pool *pool) {
    void *page = pool->pages;
    while (page) {
        void *next = *(void **)((char *)page + PT_PAGE_SIZE - sizeof(void *));
        S->allocf(S->alloc_ud, page, PT_PAGE_SIZE, 0);
        page = next;
    }
    pt_initpool(pool, pool->obj_size);
}

static void *pt_poolalloc(pt_State *S, pt_Pool *pool) {
    void *obj = pool->freed;
    if (obj == NULL) {
        size_t objsize = pool->obj_size, offset;
        void  *newpage = S->allocf(S->alloc_ud, NULL, 0, PT_PAGE_SIZE);
        if (newpage == NULL) return NULL;
        offset = ((PT_PAGE_SIZE - sizeof(void *)) / objsize - 1) * objsize;
        for (; offset > 0; offset -= objsize) {
            void **entry = (void **)((char *)newpage + offset);
            *entry = pool->freed, pool->freed = (void *)entry;
        }
        *(void **)((char *)newpage + PT_PAGE_SIZE
                   - sizeof(void *)) = pool->pages;
        pool->pages = newpage;
        return newpage;
    }
    pool->freed = *(void **)obj;
    return obj;
}

static void pt_freebuffers(pt_State *S, pt_BufferPool *pool) {
    void *page = pool->pages;
    while (page) {
        void *next = *(void **)page;
        S->allocf(S->alloc_ud, page, PT_PAGE_SIZE, 0);
        page = next;
    }
    pt_initbuffers(pool);
}

/* locate */

#define ptC_levels(c) ((c)->snap->levels)
#define ptC_breaks(c) ((c)->snap->metrics.breaks)
#define ptC_chars(c)  ((c)->snap->metrics.chars)
#define ptC_bytes(c)  ((c)->snap->metrics.bytes)

#define ptC_leaf(c)      (*(pt_Piece **)(c)->paths[ptC_levels(c)])
#define ptC_parent(c, l) ((l) ? *(c)->paths[(l) - 1] : &c->snap->root)
#define ptC_idx(c, p, l) ((c)->paths[(l)] - (p)->children)

#define ptP_start(p, b)  ((b) > 0 ? (p)->ends[(b) - 1] : 0)
#define ptP_end(p, b, n) ((b) < (n)->breaks[i] ? (p)->ends[b] : (n)->bytes[i])

PT_API pt_Size pt_remainbytes(const pt_Cursor *c) {
    pt_Cursor start;
    if (c == NULL || c->snap == NULL) return 0;
    if (c->remain >= 0) return c->remain;
    if (c->linecol.line == ptC_breaks(c))
        start.off = ptC_bytes(c);
    else {
        int r = pt_advline((start = *c, &start), 1);
        (void)r, assert(r == PT_OK);
    }
    return ((pt_Cursor *)c)->remain = start.off - c->off;
}

static void ptC_locend(pt_Cursor *c) {
    pt_Node *parent, *n = (pt_Node *)&c->snap->root;
    int      l, i;
    if (n->child_count == 0) return;
    c->linecol.line = ptC_breaks(c);
    for (l = 0; l <= ptC_levels(c); n = *c->paths[l++])
        c->paths[l] = &n->children[n->child_count - 1];
    parent = ptC_parent(c, ptC_levels(c)), i = parent->child_count - 1;
    c->pbr = parent->breaks[i], c->poff = parent->bytes[i];
    c->off = ptC_bytes(c), c->linecol.line = ptC_breaks(c);
    c->remain = 0, c->linecol.col = -1;
}

static void ptC_findpieces(pt_Cursor *c, int l, pt_Offset *poff) {
    int i;
    for (; l <= ptC_levels(c); ++l) {
        pt_Node *parent = ptC_parent(c, l);
        for (i = 0; i < parent->child_count; ++i) {
            if (*poff <= parent->bytes[i]) break;
            c->off += parent->bytes[i], c->linecol.line += parent->breaks[i];
            *poff -= parent->bytes[i];
        }
        c->paths[l] = &parent->children[i];
    }
}

static void ptC_locinpiece(pt_Cursor *c, pt_Offset off) {
    pt_Node  *parent = ptC_parent(c, ptC_levels(c));
    int       l, i = ptC_idx(c, parent, ptC_levels(c));
    pt_Piece *p = (pt_Piece *)parent->children[i];
    c->linecol.line -= c->pbr, c->off -= c->poff;
    for (l = 0; l < parent->breaks[i] && off >= p->ends[l]; ++l) continue;
    c->off += off;
    c->pbr = l, c->poff = off;
    c->linecol.line += l, c->remain = -1;
    c->linecol.col = c->off == 0 || (l > 0 && off == p->ends[l - 1]) ? 0 : -1;
}

PT_API int pt_locate(pt_Cursor *out, pt_Snapshot snap, pt_Offset off) {
    if (out == NULL || snap == NULL) return PT_ERRPARAM;
    off = pt_min(pt_max(0, off), snap->metrics.bytes);
    memset(out, 0, sizeof(pt_Cursor));
    out->snap = (pt_Tree *)snap;
    if (off == ptC_bytes(out)) return ptC_locend(out), PT_OK;
    return ptC_findpieces(out, 0, &off), ptC_locinpiece(out, off), PT_OK;
}

PT_API int pt_locline(pt_Cursor *out, pt_Snapshot snap, pt_LineCol linecol) {
    pt_Node *parent;
    int      l, i;
    if (out == NULL || snap == NULL) return PT_ERRPARAM;
    if (linecol.line > snap->metrics.breaks) return PT_ERRPARAM;
    if (linecol.line < 0 || linecol.col < 0) return PT_ERRPARAM;
    memset(out, 0, sizeof(pt_Cursor));
    out->snap = (pt_Tree *)snap, out->linecol.line = linecol.line;
    if (ptC_bytes(out) == 0) return PT_OK;
    assert(ptC_levels(out) < PT_MAX_LEVEL);
    for (l = 0; l <= ptC_levels(out); ++l) {
        parent = ptC_parent(out, l);
        for (i = 0; i < parent->child_count; ++i) {
            if (linecol.line <= parent->breaks[i]) break;
            out->off += parent->bytes[i], linecol.line -= parent->breaks[i];
        }
        out->paths[l] = &parent->children[assert(i < parent->child_count), i];
    }
    out->pbr = (assert(linecol.line <= parent->breaks[i]), linecol.line);
    if (linecol.line) out->poff = ptC_leaf(out)->ends[linecol.line - 1];
    out->linecol.col = -1, out->remain = -1;
    return pt_setcol(out, linecol.col);
}

/* navigation */

PT_API const char *pt_peek(const pt_Cursor *c, pt_Size *plen) {
    if (c && c->paths[ptC_levels(c)]) {
        pt_Node  *parent = ptC_parent(c, ptC_levels(c));
        int       i = ptC_idx(c, parent, ptC_levels(c)), end;
        pt_Piece *p = (pt_Piece *)parent->children[i];
        assert(p && c->poff <= parent->bytes[i] && c->pbr <= parent->breaks[i]);
        if ((end = ptP_end(p, c->pbr, parent)) > c->poff) {
            if (plen) *plen = end - c->poff;
            return ptC_leaf(c)->data + c->poff;
        }
    }
    if (plen) *plen = 0;
    return NULL;
}

static int ptC_nextpiece(pt_Cursor *c) {
    int      l = ptC_levels(c), i;
    pt_Node *parent = ptC_parent(c, l);
    i = ptC_idx(c, parent, l);
    c->off += parent->bytes[i] - c->poff;
    c->linecol.line += parent->breaks[i] - c->pbr;
    while (i + 1 >= parent->child_count && --l >= 0)
        parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
    if (l < 0) return PT_ERREMPTY; /* last piece */
    c->paths[l] = &parent->children[i + 1];
    for (; l < ptC_levels(c); ++l)
        c->paths[l + 1] = &(*c->paths[l])->children[0];
    c->poff = 0, c->pbr = 0;
    c->linecol.col = -1, c->remain = -1;
    return PT_OK;
}

PT_API int pt_next(pt_Cursor *c) {
    pt_Node  *parent;
    pt_Piece *p;
    int       l = ptC_levels(c), i, end;
    if (c == NULL || c->snap == NULL) return PT_ERRPARAM;
    if (c->paths[l] == NULL) return PT_ERREMPTY;
    parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
    p = (pt_Piece *)parent->children[i];
    if ((end = ptP_end(p, c->pbr, parent)) > c->poff) {
        c->off += end - c->poff;
        c->poff = (unsigned short)end;
        if (c->pbr < parent->breaks[i]) ++c->linecol.line, ++c->pbr;
    }
    if (c->pbr == parent->breaks[i]) return ptC_nextpiece(c);
    c->linecol.col = 0, c->remain = -1;
    return PT_OK;
}

static int ptC_prevpiece(pt_Cursor *c) {
    int      l = ptC_levels(c), i;
    pt_Node *parent = ptC_parent(c, l);
    i = ptC_idx(c, parent, l);
    while (i == 0 && --l >= 0)
        parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
    if (l < 0) return PT_ERREMPTY; /* first piece */
    c->paths[l] = &parent->children[i - 1];
    for (; l < ptC_levels(c); ++l) {
        parent = *c->paths[l];
        c->paths[l + 1] = &parent->children[i = parent->child_count - 1];
    }
    c->off -= c->poff + parent->bytes[i];
    c->linecol.line -= c->pbr + parent->breaks[i];
    c->poff = 0, c->pbr = 0;
    c->linecol.col = -1, c->remain = -1;
    return PT_OK;
}

PT_API int pt_prev(pt_Cursor *c) {
    int start, i;
    if (c == NULL || c->snap == NULL) return PT_ERRPARAM;
    if (c->paths[ptC_levels(c)] == NULL) return PT_ERREMPTY;
    start = ptP_start(ptC_leaf(c), c->pbr);
    if (c->poff > start) /* in middle line */
        c->off -= c->poff - start, c->poff = start;
    else if (c->pbr > 0) { /* has previous line in current piece */
        pt_Piece *p = ptC_leaf(c);
        size_t    prev = ptP_start(p, c->pbr - 1);
        c->off -= c->poff - prev, c->poff = prev, --c->pbr, --c->linecol.line;
    } else {
        pt_Node *parent;
        if (ptC_prevpiece(c) != PT_OK) return PT_ERREMPTY;
        parent = ptC_parent(c, ptC_levels(c));
        i = ptC_idx(c, parent, ptC_levels(c));
        if (parent->breaks[i] > 0) {
            c->pbr = parent->breaks[i];
            c->poff = ptC_leaf(c)->ends[parent->breaks[i] - 1];
            c->off += c->poff, c->linecol.line += c->pbr;
        }
        return c->linecol.col = -1, c->remain = -1, PT_OK;
    }
    return c->linecol.col = 0, c->remain = -1, PT_OK;
}

static void ptC_forwardoff(pt_Cursor *c, pt_Offset d) {
    int      l = ptC_levels(c), i, in;
    pt_Node *parent = ptC_parent(c, l);
    i = ptC_idx(c, parent, l), in = parent->bytes[i] - c->poff;
    if (d < in) return ptC_locinpiece(c, c->poff + d);
    d -= in, c->off += in, c->linecol.line += parent->breaks[i] - c->pbr;
    for (; l >= 0; --l) {
        parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
        if (l == ptC_levels(c)) ++i;
        for (; i < parent->child_count; ++i) {
            if (d < parent->bytes[i]) break;
            c->off += parent->bytes[i], c->linecol.line += parent->breaks[i];
            d -= parent->bytes[i];
        }
        if (i < parent->child_count) break;
    }
    c->paths[assert(l >= 0 && l <= ptC_levels(c)), l] = &parent->children[i];
    ptC_findpieces(c, l + 1, &d);
    c->pbr = 0, ptC_locinpiece(c, d);
}

static void ptC_backwardoff(pt_Cursor *c, pt_Offset d) {
    int      l, i;
    pt_Node *parent = ptC_parent(c, ptC_levels(c));
    i = ptC_idx(c, parent, ptC_levels(c));
    if (d <= c->poff) return ptC_locinpiece(c, c->poff - d);
    d -= c->poff, c->off -= c->poff, c->linecol.line -= c->pbr;
    for (l = ptC_levels(c); l >= 0; --l) {
        parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
        if (l == ptC_levels(c)) --i;
        for (; i >= 0; --i) {
            if (d <= parent->bytes[i]) break;
            c->off -= parent->bytes[i], c->linecol.line -= parent->breaks[i];
            d -= parent->bytes[i];
        }
        if (i >= 0) break;
    }
    c->paths[assert(l >= 0 && l <= ptC_levels(c)), l] = &parent->children[i];
    c->off -= parent->bytes[i], c->linecol.line -= parent->breaks[i];
    d = parent->bytes[i] - d, ptC_findpieces(c, l + 1, &d);
    c->pbr = 0, ptC_locinpiece(c, d);
}

PT_API int pt_advance(pt_Cursor *c, pt_Offset delta) {
    pt_Offset n;
    if (c == NULL || c->snap == NULL) return PT_ERRPARAM;
    if (delta == 0 || ptC_bytes(c) == 0) return PT_OK;
    n = c->off + delta;
    /* clang-format off */
    if (n < 0) ptC_backwardoff(c, (pt_Size)-c->off);
    else if (n >= ptC_bytes(c)) ptC_locend(c);
    else if (delta < 0) ptC_backwardoff(c, -delta);
    else ptC_forwardoff(c, delta);
    return PT_OK; /* clang-format on */
}

static void ptC_findlines(pt_Cursor *c, int l, pt_Offset *poff) {
    int i;
    for (; l <= ptC_levels(c); ++l) {
        pt_Node *parent = ptC_parent(c, l);
        for (i = 0; i < parent->child_count; ++i) {
            if (*poff <= parent->breaks[i]) break;
            c->off += parent->bytes[i], c->linecol.line += parent->breaks[i];
            *poff -= parent->breaks[i];
        }
        c->paths[l] = &parent->children[i];
    }
}

static void ptC_forwardline(pt_Cursor *c, pt_Offset d) {
    int      l = ptC_levels(c), start = c->poff, i, in;
    pt_Node *parent = ptC_parent(c, l);
    i = ptC_idx(c, parent, l), in = parent->breaks[i] - c->pbr;
    if ((assert(d > 0), d) < in) {
        pt_Piece *p = (pt_Piece *)parent->children[i];
        return ptC_locinpiece(c, p->ends[c->pbr + d - 1]);
    }
    d -= in, c->off += parent->bytes[i] - start, c->linecol.line += in;
    for (; l >= 0; --l) {
        parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
        if (l == ptC_levels(c)) i += 1;
        for (; i < parent->child_count; ++i) {
            if (d <= parent->breaks[i]) break;
            c->off += parent->bytes[i], c->linecol.line += parent->breaks[i];
            d -= parent->breaks[i];
        }
        if (i < parent->child_count) break;
    }
    c->paths[assert(l >= 0), l] = &parent->children[i];
    ptC_findlines(c, l + 1, &d);
    c->pbr = 0, ptC_locinpiece(c, ptP_start(ptC_leaf(c), d));
}

static int ptC_islinehead(const pt_Cursor *c) {
    pt_Node  *parent = ptC_parent(c, ptC_levels(c));
    int       i = ptC_idx(c, parent, ptC_levels(c));
    pt_Piece *p = (pt_Piece *)parent->children[i];
    return c->off == 0 || (c->pbr > 0 && c->poff == p->ends[c->pbr - 1]);
}

static void ptC_backwardline(pt_Cursor *c, pt_Offset d) {
    int      l = ptC_levels(c), i;
    pt_Node *parent = ptC_parent(c, l);
    i = ptC_idx(c, parent, l);
    if (d == 0 && ptC_islinehead(c)) return (void)(c->linecol.col = 0);
    if (d < c->pbr) {
        pt_Piece *p = (pt_Piece *)parent->children[i];
        return ptC_locinpiece(c, p->ends[c->pbr - d - 1]);
    }
    d -= c->pbr, c->off -= c->poff, c->linecol.line -= c->pbr;
    for (; l >= 0; --l) {
        parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
        if (l == ptC_levels(c)) i -= 1;
        for (; i >= 0; --i) {
            if (d < parent->breaks[i]) break;
            c->off -= parent->bytes[i], c->linecol.line -= parent->breaks[i];
            d -= parent->breaks[i];
        }
        if (i >= 0) break;
    }
    l = pt_max(0, l), i = pt_max(0, i); /* first line */
    c->paths[l] = &parent->children[i];
    c->off -= parent->bytes[i], c->linecol.line -= parent->breaks[i];
    d = parent->breaks[i] - d, ptC_findlines(c, l + 1, &d);
    c->pbr = 0, ptC_locinpiece(c, ptP_start(ptC_leaf(c), d));
}

PT_API int pt_advline(pt_Cursor *c, pt_Offset delta) {
    pt_Offset n, line = c ? c->linecol.line : 0;
    if (c == NULL || c->snap == NULL) return PT_ERRPARAM;
    if (ptC_bytes(c) == 0) return PT_OK;
    n = pt_min(pt_max(0, line + delta), ptC_breaks(c));
    if (n == line) return ptC_backwardline(c, 0), PT_OK;
    /* clang-format off */
    if (n == 0) ptC_backwardline(c, line);
    else if (n == ptC_breaks(c)) ptC_forwardline(c, ptC_breaks(c) - line);
    else if (delta < 0) ptC_backwardline(c, -delta);
    else ptC_forwardline(c, delta);
    return PT_OK; /* clang-format on */
}

static void ptC_findcol(pt_Cursor *c, int l, pt_Size *pcol) {
    int i;
    for (; l <= ptC_levels(c); ++l) {
        pt_Node *parent = ptC_parent(c, l);
        for (i = 0; i < parent->child_count; ++i) {
            if (*pcol < parent->chars[i] || parent->breaks[i] > 0) break;
            *pcol -= parent->chars[i], c->linecol.col += parent->chars[i];
            c->off += parent->bytes[i];
        }
        c->paths[l] = &parent->children[pt_min(i, parent->child_count - 1)];
    }
}

static int ptC_callchars(pt_Cursor *c, pt_Size *pcol) {
    pt_Node  *parent = ptC_parent(c, ptC_levels(c));
    int       i = ptC_idx(c, parent, ptC_levels(c));
    pt_Piece *p = (pt_Piece *)parent->children[i];

    pt_PieceSize start = ptP_start(p, c->pbr), end = ptP_end(p, c->pbr, parent);
    pt_Slice     s = pt_slice(p->data + start, end - start);
    pt_PieceSize col = pt_min(*pcol, s.len), old = col;
    pt_PieceSize len = c->snap->S->charsf(c->snap->S->chars_ud, s, &col);
    assert(len <= s.len && col <= s.len);
    if (len > s.len) len = s.len;
    c->poff += (unsigned short)len, c->off += len;
    col = old - col, c->linecol.col += col, *pcol -= col;
    return c->pbr < parent->breaks[i] || *pcol == 0;
}

PT_API int pt_setcol(pt_Cursor *c, pt_Size column) {
    pt_Node *parent;
    int      l, i, r;
    if (c == NULL || c->snap == NULL) return PT_ERRPARAM;
    ptC_backwardline(c, 0); /* move to line head */
    assert(c->linecol.col == 0);
    if (column == 0) return PT_OK;
    for (l = ptC_levels(c); l >= 0; --l) {
        parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
        if (l == ptC_levels(c)) {
            if (ptC_callchars(c, &column)) return PT_OK;
            i += 1;
        }
        for (; i < parent->child_count; ++i) {
            if (column < parent->chars[i] || parent->breaks[i] > 0) break;
            column -= parent->chars[i], c->linecol.col += parent->chars[i];
            c->off += parent->bytes[i];
        }
        if (i < parent->child_count) break;
    }
    c->paths[assert(l >= 0 && l <= ptC_levels(c)), l] = &parent->children[i];
    ptC_findcol(c, l + 1, &column);
    c->pbr = 0, c->poff = 0;
    return (void)ptC_callchars(c, &column), PT_OK;
}

static void ptC_charsforpiece(pt_Cursor *c, int l, pt_Offset *pd) {
    int i;
    for (; l <= ptC_levels(c); ++l) {
        pt_Node *parent = ptC_parent(c, l);
        for (i = 0; i < parent->child_count; ++i) {
            if (*pd < parent->bytes[i]) break;
            *pd -= parent->bytes[i], c->linecol.col += parent->chars[i];
        }
        c->paths[l] = &parent->children[i];
    }
}

static pt_Size ptC_measure(pt_Cursor *c, pt_PieceSize off, pt_PieceSize len) {
    pt_PieceSize col = len;
    pt_Slice     s = pt_slice(ptC_leaf(c)->data + off, len);
    c->snap->S->charsf(c->snap->S->chars_ud, s, &col);
    return len - col;
}

static void ptC_charsfromdelta(pt_Cursor *c, pt_Offset d) {
    int      l = ptC_levels(c), i;
    pt_Node *parent;
    for (; l >= 0; --l) {
        parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
        if (l == ptC_levels(c)) {
            pt_Offset end = parent->bytes[i] - c->poff;
            c->linecol.col += ptC_measure(c, c->poff, pt_min(d, end));
            if (d <= end) return;
            i += 1, d -= end;
        }
        for (; i < parent->child_count; ++i) {
            if (d < parent->bytes[i]) break;
            d -= parent->bytes[i], c->linecol.col += parent->chars[i];
        }
        if (i < parent->child_count) break;
    }
    assert(l >= 0 && l <= ptC_levels(c));
    c->paths[l] = &parent->children[i];
    ptC_charsforpiece(c, l + 1, &d);
    c->linecol.col += ptC_measure(c, 0, d);
}

PT_API pt_LineCol pt_linepos(const pt_Cursor *c) {
    pt_Cursor start;
    pt_Offset col;
    if (c == NULL || c->snap == NULL) return pt_linecol(0, 0);
    if (c->linecol.col >= 0) return c->linecol;
    start = *c, ptC_backwardline(&start, 0);
    ptC_charsfromdelta(&start, c->off - start.off);
    ((pt_Cursor *)c)->linecol.col = start.linecol.col;
    return c->linecol;
}

/* insert */

typedef struct pt_Insert {
    pt_Cursor  *c;   /* cursor for the insertion */
    const char *s;   /* string to insert */
    size_t      len; /* length of the string to insert */

    pt_Node *parents[PT_MAX_LEVEL]; /* parents for c->paths */
    pt_Node  pend[PT_MAX_LEVEL];    /* pending nodes/pieces */
    pt_Node *pend_root;             /* pending unused node for splitroot */
} pt_Insert;

/* clang-format off */
static void ptN_setmetrics(pt_Node *n, int i, pt_Metrics m)
{ n->bytes[i] = m.bytes, n->breaks[i] = m.breaks, n->chars[i] = m.chars; }

static void ptN_addmetrics(pt_Node *n, int i, pt_Metrics m)
{ n->bytes[i] += m.bytes, n->breaks[i] += m.breaks, n->chars[i] += m.chars; }

static void ptN_submetrics(pt_Node *n, int i, pt_Metrics m)
{ n->bytes[i] -= m.bytes, n->breaks[i] -= m.breaks, n->chars[i] -= m.chars; }

static void ptM_addmetrics(pt_Metrics *d, pt_Metrics m)
{ d->bytes += m.bytes, d->breaks += m.breaks, d->chars += m.chars; }
/* clang-format on */

static int ptI_fill(pt_Insert *ctx, pt_Node *n) {
    pt_State *S = ctx->c->snap->S;
    int       i;
    for (i = n->child_count; i < PT_MAX_FANOUT && ctx->len > 0; ++i) {
        pt_LinesInfo li = {0, 0, NULL};
        pt_Slice     sl = pt_slice(ctx->s, pt_min(ctx->len, PT_PIECE_MAXSIZE));
        short        cur = n->child_count;
        pt_Piece    *p;
        if (!(p = (pt_Piece *)pt_poolalloc(S, &S->pieces))) return PT_ERRMEM;
        li.ends = &p->ends, n->bytes[cur] = S->linesf(S->lines_ud, sl, &li);
        assert(n->bytes[cur] > 0 && n->bytes[cur] <= sl.len);
        assert(li.chars <= n->bytes[cur] && li.breaks <= PT_PIECE_MAXLINES);
        n->breaks[cur] = li.breaks, n->chars[cur] = li.chars;
        p->version = ctx->c->snap->root.version, p->data = sl.s;
        n->children[n->child_count++] = (pt_Node *)p;
        ctx->s += n->bytes[cur], ctx->len -= n->bytes[cur];
    }
    return PT_OK;
}

static pt_Metrics ptI_sumrange(const pt_Node *n, int start, int count) {
    pt_Metrics m = {0, 0, 0};
    int        i;
    for (i = 0; i < count; ++i) {
        m.bytes += n->bytes[start + i];
        m.breaks += n->breaks[start + i];
        m.chars += n->chars[start + i];
    }
    return m;
}

static int ptI_fillroot(pt_Insert *x) {
    pt_Tree   *snap = x->c->snap;
    pt_Metrics m;
    int        r = ptI_fill(x, &snap->root);
    if (r != PT_OK) return r;
    m = ptI_sumrange(&snap->root, 0, snap->root.child_count);
    ptC_locend(x->c);
    return PT_OK;
}

static int ptI_splitpiece(pt_Insert *x) {
    pt_State *S = x->c->snap->S;
    pt_Node  *pend = &x->pend[ptC_levels(x->c)];
    pt_Node  *parent = ptC_parent(x->c, ptC_levels(x->c));
    int       pi, i = ptC_idx(x->c, parent, ptC_levels(x->c));
    pt_Piece *r, *l = (pt_Piece *)parent->children[i];
    pt_Size   breaks = parent->breaks[i], size = parent->bytes[i];
    if (x->c->poff == 0 || x->c->poff == size) return PT_OK;
    if (!(r = (pt_Piece *)pt_poolalloc(S, &S->pieces))) return PT_ERRMEM;
    if (breaks > 0) {
        for (pi = x->c->pbr; pi < breaks; ++pi)
            r->ends[pi - x->c->pbr] = l->ends[pi] - x->c->poff;
        parent->breaks[i] = x->c->pbr, pend->breaks[0] = (breaks -= x->c->pbr);
    }
    if (breaks == 0 || x->c->pbr == 0) {
        pt_Size chars = parent->chars[i], remain = size - x->c->poff;
        parent->chars[i] = ptC_measure(x->c, 0, x->c->poff);
        pend->chars[0] = chars ? chars - parent->chars[i]
                               : ptC_measure(x->c, x->c->poff, remain);
    }
    parent->bytes[i] = x->c->poff, pend->bytes[0] = size - x->c->poff;
    r->version = x->c->snap->version, r->data = l->data + x->c->poff;
    pend->children[0] = (pt_Node *)r, pend->child_count = 1;
    return PT_OK;
}

static int ptI_init(pt_Insert *x, pt_Cursor *c, const char *s, size_t len) {
    pt_Tree  *snap = c->snap;
    pt_State *S = snap->S;
    int       l, i, tc, r;
    x->c = c, x->s = s, x->len = len, x->pend_root = NULL;
    if (!c->dirty) { /* make a new tree */
        if (!(snap = (pt_Tree *)pt_poolalloc(S, &S->trees))) return PT_ERRMEM;
        *snap = *c->snap, c->oldsnap = c->snap, c->snap = snap, c->dirty = 1;
        snap->version = ++snap->S->max_version;
        snap->root.version = snap->version;
    }
    if (ptC_bytes(c) == 0 && (r = ptI_fillroot(x)) != PT_OK) return r;
    for (l = 0; l <= ptC_levels(c); ++l) {
        pt_Node *n, *parent = ptC_parent(c, l), **cs = parent->children;
        i = ptC_idx(c, parent, l), tc = parent->child_count;
        x->parents[l] = parent, x->pend[l].child_count = 0;
        if (l < ptC_levels(c) && i < tc && cs[i]->version != snap->version) {
            if (!(n = (pt_Node *)pt_poolalloc(S, &S->nodes))) return PT_ERRMEM;
            c->paths[l + 1] = &n->children[c->paths[l + 1] - cs[i]->children];
            *n = *cs[i], cs[i] = n, n->version = snap->version;
        }
    }
    return ptI_splitpiece(x);
}

static int ptI_checkpendroot(pt_Insert *ctx) {
    pt_State *S = ctx->c->snap->S;
    if (ctx->pend_root) return PT_OK;
    ctx->pend_root = (pt_Node *)pt_poolalloc(S, &S->nodes);
    return ctx->pend_root ? PT_OK : PT_ERRMEM;
}

static void ptI_makespace(pt_Node *d, int i, int n) {
    int moved = d->child_count - i;
    assert(d->child_count + n <= PT_MAX_FANOUT && i <= d->child_count);
    memmove(&d->children[i + n], &d->children[i], moved * sizeof(pt_Node *));
    memmove(&d->bytes[i + n], &d->bytes[i], moved * sizeof(pt_Size));
    memmove(&d->breaks[i + n], &d->breaks[i], moved * sizeof(pt_Size));
    memmove(&d->chars[i + n], &d->chars[i], moved * sizeof(pt_Size));
}

static void ptI_move(pt_Node *d, int di, const pt_Node *s, int si, int c) {
    memcpy(&d->children[di], &s->children[si], c * sizeof(pt_Node *));
    memcpy(&d->bytes[di], &s->bytes[si], c * sizeof(pt_Size));
    memcpy(&d->breaks[di], &s->breaks[si], c * sizeof(pt_Size));
    memcpy(&d->chars[di], &s->chars[si], c * sizeof(pt_Size));
    d->child_count += c;
}

static void ptI_freenodes(pt_State *S, pt_Node *n, int rl) {
    int i;
    if (rl == 0) {
        for (i = 0; i < n->child_count; ++i)
            pt_poolfree(&S->pieces, n->children[i]);
    } else {
        for (i = 0; i < n->child_count; ++i) {
            ptI_freenodes(S, (pt_Node *)n->children[i], rl - 1);
            pt_poolfree(&S->nodes, n->children[i]);
        }
    }
}

static void ptI_adddelta(pt_Insert *x, int l, pt_Metrics m) {
    while (--l >= 0) {
        pt_Node *parent = x->parents[l];
        ptN_addmetrics(parent, ptC_idx(x->c, parent, l), m);
    }
}

static void ptI_subdelta(pt_Insert *x, int l, pt_Metrics m) {
    while (--l >= 0) {
        pt_Node *parent = x->parents[l];
        ptN_submetrics(parent, ptC_idx(x->c, parent, l), m);
    }
}

static void ptI_takegapright(pt_Insert *x, int l, pt_Node *suffix) {
    pt_Node *parent = x->parents[l];
    int      i = ptC_idx(x->c, parent, l);
    suffix->child_count = 0;
    if (i + 1 == parent->child_count) return; /* no gap right */
    ptI_move(suffix, 0, parent, i + 1, parent->child_count - i - 1);
    ptI_subdelta(x, l, ptI_sumrange(suffix, 0, suffix->child_count));
    parent->child_count = i + 1;
}

static pt_Node *ptI_packright(pt_Insert *x, int l, int start, pt_Node *suffix) {
    pt_State *S = x->c->snap->S;
    pt_Node  *n;
    if (!(n = (pt_Node *)pt_poolalloc(S, &S->nodes))) return NULL;
    memset(n, 0, sizeof(pt_Node));
    n->version = x->c->snap->version;
    ptI_move(n, 0, &x->pend[l], start, x->pend[l].child_count - start);
    ptI_move(n, n->child_count, suffix, 0, suffix->child_count);
    x->pend[l].child_count = 0, suffix->child_count = 0;
    return n;
}

static int ptI_splitroot(pt_Insert *x, pt_Node *r) {
    pt_Tree *snap = x->c->snap;
    pt_Node *l, ***paths = x->c->paths;
    int      i = paths[0] - x->parents[0]->children;
    if (snap->levels + 1 >= PT_MAX_LEVEL) return PT_ERRFULL;
    l = x->pend_root, x->pend_root = NULL;
    *l = snap->root;
    snap->root.children[0] = l;
    snap->root.children[1] = r;
    snap->root.child_count = 2;
    ptN_setmetrics(&snap->root, 0, ptI_sumrange(l, 0, l->child_count));
    ptN_setmetrics(&snap->root, 1, ptI_sumrange(r, 0, r->child_count));
    memmove(paths + 1, paths, (snap->levels + 1) * sizeof(pt_Node **));
    paths[0] = &snap->root.children[x->parents[0] == r];
    memmove(x->parents + 1, x->parents, (snap->levels + 1) * sizeof(pt_Node *));
    x->parents[0] = &snap->root;
    if (x->parents[1] != r) paths[1] = &l->children[i], x->parents[1] = l;
    snap->levels += 1, x->pend[snap->levels].child_count = 0;
    return PT_OK;
}

static int ptI_simple(pt_Insert *x, int l, int movegap) {
    pt_Metrics m;
    pt_Node   *parent = x->parents[l], *pend = &x->pend[l];
    int        i = ptC_idx(x->c, parent, l);
    if (pend->child_count + parent->child_count > PT_MAX_FANOUT) return 0;
    m = ptI_sumrange(pend, 0, pend->child_count);
    ptI_makespace(parent, i + 1, pend->child_count);
    ptI_move(parent, i + 1, pend, 0, pend->child_count);
    ptI_adddelta(x, l, m);
    if (l == ptC_levels(x->c)) ptM_addmetrics(&x->c->snap->metrics, m);
    if (movegap) x->c->paths[l] = &parent->children[i + pend->child_count];
    pend->child_count = 0;
    return 1;
}

static void ptI_splitinsert(pt_Insert *x, int l, pt_Node *next) {
    pt_Node suffix, *n, *parent, *pend;
    parent = x->parents[l], pend = &x->pend[l];
    int r, i = ptC_idx(x->c, parent, l), space, pmove, smove;
    assert(parent->child_count + pend->child_count > PT_MAX_FANOUT);
    ptI_takegapright(x, l, &suffix);
    space = PT_MAX_FANOUT - parent->child_count;
    if ((pmove = pt_min(space, pend->child_count)) > 0) {
        pt_Metrics m = ptI_sumrange(pend, 0, pmove);
        ptI_move(parent, i + 1, pend, 0, pmove);
        ptI_adddelta(x, l, m);
        if (l == ptC_levels(x->c)) ptM_addmetrics(&x->c->snap->metrics, m);
        space -= pmove;
    }
    if ((smove = pt_min(space, suffix.child_count)) > 0) {
        ptI_move(parent, i + 1 + pmove, &suffix, 0, smove);
        ptI_adddelta(x, l, ptI_sumrange(&suffix, 0, smove));
    }
    if (pmove < pend->child_count)
        ptI_move(next, 0, pend, pmove, pend->child_count - pmove);
    if (smove < suffix.child_count)
        ptI_move(
                next, next->child_count, &suffix, smove,
                suffix.child_count - smove);
    pend->child_count = 0;
}

static int ptI_flush(pt_Insert *x, int l, int movegap) {
    pt_State *S = x->c->snap->S;
    pt_Node   suffix, *n, *parent, *pend;
    int       r, i, space, pc, pleft, sleft;
    for (; l >= 0; --l) {
        if (ptI_simple(x, l, movegap)) return PT_OK;
        n = (pt_Node *)pt_poolalloc(S, &S->nodes);
        if (n == NULL) return PT_ERRMEM;
        ptI_splitinsert(x, l, n);
        if (l == 0) return ptI_splitroot(x, n);
        x->pend[l - 1].children[x->pend[l - 1].child_count++] = n;
    }
    return PT_OK;
}

static void ptI_disposepend(pt_Insert *x, int l) {
    pt_State *S = x->c->snap->S;
    int       i;
    for (i = 0; i < x->pend[l].child_count; ++i) {
        if (l == 0)
            pt_poolfree(&S->pieces, x->pend[l].children[i]);
        else {
            ptI_freenodes(S, (pt_Node *)x->pend[l].children[i], l - 1);
            pt_poolfree(&S->nodes, x->pend[l].children[i]);
        }
    }
}

PT_API int pt_insert(pt_Cursor *c, const char *s, size_t len) {
    pt_Insert x;
    int       r, l, i;
    if (len == 0) return PT_OK;
    if (c == NULL || c->snap == NULL || s == NULL) return PT_ERRPARAM;
    if ((r = ptI_init(&x, c, s, len)) != PT_OK) return r;
    l = ptC_levels(c), assert(l < PT_MAX_LEVEL);
    while (x.len > 0) {
        if ((r = ptI_checkpendroot(&x)) != PT_OK) break;
        if ((r = ptI_fill(&x, &x.pend[l])) != PT_OK) break;
        if ((r = ptI_flush(&x, l, 1)) != PT_OK) break;
        l = ptC_levels(c);
    }
    if (r != PT_OK)
        for (l = 0; l <= ptC_levels(c); ++l) ptI_disposepend(&x, l);
    if (x.pend_root) pt_poolfree(&c->snap->S->nodes, x.pend_root);
    return r;
}

/* remove */

PT_API int pt_remove(pt_Cursor *c, pt_Cursor *end);
PT_API int pt_replace(pt_Cursor *c, const char *s, size_t len);

/* API */

/* clang-format off */
PT_API int pt_version(pt_Snapshot snap)
{ return snap ? snap->version : 0; }

PT_API void pt_retain(pt_Snapshot snap)
{ if (snap) ((pt_Tree*)snap)->refcount++; }

PT_API pt_Offset pt_bytepos(const pt_Cursor* c)
{ return c ? c->off : 0; }

PT_API size_t pt_bytes(pt_Snapshot snap)
{ return snap ? snap->metrics.bytes : 0; }

PT_API size_t pt_chars(pt_Snapshot snap)
{ return snap ? snap->metrics.chars : 0; }

PT_API size_t pt_lines(pt_Snapshot snap)
{ return snap ? snap->metrics.breaks + 1 : 0; }
/* clang-format on */

static void *ptS_defallocf(void *ud, void *ptr, size_t osize, size_t nsize) {
    void *newptr;
    (void)ud, (void)osize;
    if (nsize == 0) return free(ptr), (void *)NULL;
    newptr = realloc(ptr, nsize);
    if (newptr == NULL) abort(); /* failure is unrecoverable by default */
    return newptr;
}

static pt_PieceSize ptS_defcharsf(void *ud, pt_Slice s, pt_PieceSize *pcol) {
    (void)ud;
    if (*pcol >= s.len) return (*pcol -= s.len), s.len;
    s.len = *pcol, *pcol = 0;
    return s.len;
}

static pt_PieceSize ptS_deflinesf(void *ud, pt_Slice s, pt_LinesInfo *p) {
    pt_PieceSize i;
    (void)ud;
    for (i = 0; i < s.len; ++i) {
        if (s.s[i] == '\n') {
            if (p->breaks == PT_PIECE_MAXLINES) break;
            (*p->ends)[p->breaks++] = i + 1;
        }
        p->chars += 1;
    }
    return i;
}

PT_API pt_State *pt_newstate(pt_Alloc *allocf, void *ud) {
    pt_State *S;
    if (allocf == NULL) allocf = &ptS_defallocf;
    S = (pt_State *)allocf(ud, NULL, 0, sizeof(pt_State));
    if (!S) return NULL;
    memset(S, 0, sizeof(pt_State));
    S->alloc_ud = ud;
    S->allocf = allocf;
    S->charsf = &ptS_defcharsf;
    S->linesf = &ptS_deflinesf;
    pt_initpool(&S->trees, sizeof(pt_Tree));
    pt_initpool(&S->nodes, sizeof(pt_Node));
    pt_initpool(&S->pieces, sizeof(pt_Piece));
    pt_initbuffers(&S->buffers);
    S->max_version = 0;
    return S;
}

PT_API void pt_close(pt_State *S) {
    if (S == NULL) return;
    pt_freepool(S, &S->trees);
    pt_freepool(S, &S->nodes);
    pt_freepool(S, &S->pieces);
    pt_freebuffers(S, &S->buffers);
    S->allocf(S->alloc_ud, S, sizeof(pt_State), 0);
}

PT_API pt_Snapshot pt_empty(pt_State *S) {
    pt_Tree *snap = S ? (pt_Tree *)pt_poolalloc(S, &S->trees) : NULL;
    if (snap == NULL) return NULL;
    memset(snap, 0, sizeof(pt_Tree));
    snap->S = S;
    snap->version = 0;
    snap->refcount = 1;
    return snap;
}

PT_API pt_Snapshot pt_from(pt_State *S, const char *s, size_t len) {
    pt_Snapshot snap = pt_empty(S), r = NULL;
    pt_Cursor   c;
    if (snap == NULL) return NULL;
    if (pt_locate(&c, snap, 0) == PT_OK && pt_insert(&c, s, len) == PT_OK)
        r = pt_commit(&c);
    return pt_release(snap), r;
}

PT_API char *pt_buffer(pt_State *S, size_t *plen) {
    if (S == NULL || plen == NULL || *plen == 0) return NULL;
    if (S->buffers.remain == 0) {
        void *newpage = S->allocf(S->alloc_ud, NULL, 0, PT_PAGE_SIZE);
        if (newpage == NULL) return NULL;
        *(void **)newpage = S->buffers.pages;
        S->buffers.pages = newpage;
        S->buffers.buffer = (char *)newpage + sizeof(void *);
        S->buffers.remain = PT_PAGE_SIZE - sizeof(void *);
    }
    if (*plen > S->buffers.remain) *plen = S->buffers.remain;
    S->buffers.remain -= *plen;
    S->buffers.buffer += *plen;
    return (char *)S->buffers.buffer - *plen;
}

static void ptC_freepieces(pt_Cursor *c, pt_Node *n) {
    int i;
    for (i = 0; i < n->child_count; ++i) {
        pt_Piece *child = (pt_Piece *)n->children[i];
        if (child->version == c->snap->version)
            pt_poolfree(&c->snap->S->pieces, child);
    }
}

static void ptC_free(pt_Cursor *c) {
    pt_Node *parent = &c->snap->root;
    int      l = 0, i = -1;
    if (ptC_levels(c) == 0) return ptC_freepieces(c, &c->snap->root);
    while (l >= 0) {
        c->paths[l] = &parent->children[i + 1];
        while (++l < ptC_levels(c))
            parent = ptC_parent(c, l), c->paths[l] = &parent->children[0];
        parent = ptC_parent(c, l);
        ptC_freepieces(c, parent);
        if (parent->version == c->snap->version)
            pt_poolfree(&c->snap->S->nodes, parent);
        while (--l >= 0) {
            parent = ptC_parent(c, l), i = ptC_idx(c, parent, l);
            if (i + 1 < parent->child_count) break;
            if (l > 0 && parent->version == c->snap->version)
                pt_poolfree(&c->snap->S->nodes, parent);
        }
    }
}

PT_API void pt_release(pt_Snapshot snap) {
    if (snap == NULL) return;
    if (snap->refcount > 1) return (void)(((pt_Tree *)snap)->refcount -= 1);
    if (snap->levels > 0 || snap->root.child_count > 0) {
        pt_Cursor c;
        memset(&c, 0, sizeof(pt_Cursor));
        c.snap = (pt_Tree *)snap;
        ptC_free(&c);
    }
    return pt_poolfree(&((pt_Tree *)snap)->S->trees, (void *)snap);
}

PT_API pt_Snapshot pt_commit(pt_Cursor *c) {
    if (c == NULL || c->snap == NULL) return NULL;
    if (!c->dirty) return pt_retain(c->snap), c->snap;
    c->oldsnap = NULL, c->dirty = 0;
    return c->snap;
}

PT_API void pt_rollback(pt_Cursor *c) {
    if (c == NULL || c->snap == NULL || !c->dirty) return;
    c->dirty = 0, pt_release(c->snap);
    c->snap = c->oldsnap, c->oldsnap = NULL;
}

PT_API pt_Alloc *pt_getallocf(pt_State *S, void **pud) {
    if (S == NULL) return NULL;
    if (pud) *pud = S->alloc_ud;
    return S->allocf;
}

PT_API pt_Chars *pt_getcharsf(pt_State *S, void **pud) {
    if (S == NULL) return NULL;
    if (pud) *pud = S->chars_ud;
    return S->charsf;
}

PT_API pt_Lines *pt_getlinesf(pt_State *S, void **pud) {
    if (S == NULL) return NULL;
    if (pud) *pud = S->lines_ud;
    return S->linesf;
}

PT_API void pt_setcharsf(pt_State *S, pt_Chars *charsf, void *ud) {
    if (S == NULL) return;
    if (charsf == NULL) charsf = &ptS_defcharsf;
    S->charsf = charsf, S->chars_ud = ud;
}

PT_API void pt_setlinesf(pt_State *S, pt_Lines *linesf, void *ud) {
    if (S == NULL) return;
    if (linesf == NULL) linesf = &ptS_deflinesf;
    S->linesf = linesf, S->lines_ud = ud;
}

PT_NS_END

#endif /* PT_IMPLEMENTATION */
