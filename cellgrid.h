#ifndef cellgrid_h
#define cellgrid_h

#ifndef CG_NS_BEGIN
# ifdef __cplusplus
#   define CG_NS_BEGIN extern "C" {
#   define CG_NS_END   }
# else
#   define CG_NS_BEGIN
#   define CG_NS_END
# endif
#endif

#ifndef CG_STATIC
# if __GNUC__
#   define CG_STATIC static __attribute((unused))
# else
#   define CG_STATIC static
# endif
#endif

#ifdef CG_STATIC_API
# ifndef CG_IMPLEMENTATION
#   define CG_IMPLEMENTATION
# endif
# define CG_API CG_STATIC
#endif

#if !defined(CG_API) && defined(_WIN32)
# ifdef CG_IMPLEMENTATION
#   define CG_API __declspec(dllexport)
# else
#   define CG_API __declspec(dllimport)
# endif
#endif

#ifndef CG_API
# define CG_API extern
#endif

#include <stddef.h>

CG_NS_BEGIN

#define CG_OK       (0)
#define CG_ERRPARAM (-1)
#define CG_ERRMEM   (-2)

typedef void *cg_Allocf(void *ud, void *p, size_t osize, size_t nsize);
typedef int   cg_WcWidthf(void *ud, int cp);

typedef struct cg_Grid cg_Grid;
typedef struct cg_Diff cg_Diff;

/* clang-format off */
typedef struct cg_Slice { const char *s, *e; } cg_Slice;
/* clang-format on */

/* lifecycle */
CG_API int  cg_init(cg_Grid *G, cg_Allocf *f, void *ud);
CG_API void cg_free(cg_Grid *G);
CG_API void cg_setwcwidth(cg_Grid *G, cg_WcWidthf *f, void *ud);
CG_API void cg_settabstop(cg_Grid *G, int ts);

#define cg_valid(G) ((G) && (G)->rows)

/* column calculation */
CG_API cg_Slice cg_slice(const char *s, size_t len);
CG_API int      cg_next(const cg_Grid *G, int c, cg_Slice *s);
CG_API int      cg_cols(const cg_Grid *G, int c, cg_Slice s);
CG_API size_t   cg_byte(const cg_Grid *G, int c, cg_Slice s, int col);

/* frame */
CG_API int  cg_begin(cg_Grid *G, int top, int rows, int cols);
CG_API void cg_clear(cg_Grid *G);

/* cell write */
CG_API void cg_put(cg_Grid *G, int r, int c, int cp, unsigned st);
CG_API void cg_clearrow(cg_Grid *G, int r, int cs, int ce);
CG_API void cg_fill(cg_Grid *G, int r, int cs, int ce, int cp);
CG_API void cg_span(cg_Grid *G, int r, int cs, int ce, unsigned st);
CG_API int  cg_putslice(cg_Grid *G, int r, int c, cg_Slice s, unsigned st);

/* diff / freeze  */
CG_API int  cg_diff(const cg_Grid *G, cg_Diff *diff);
CG_API void cg_freeze(cg_Grid *G);

/* getters */

#define cg_rows(G)    ((G) ? (G)->rows : 0)
#define cg_ncols(G)   ((G) ? (G)->cols : 0)
#define cg_top(G)     ((G) ? (G)->top : 0)
#define cg_tabstop(G) ((G) ? (G)->tabstop : 0)

CG_API int cg_cell(const cg_Grid *G, int r, int c, unsigned *st);
CG_API int cg_back(const cg_Grid *G, int r, int c, unsigned *st);
CG_API int cg_isdirty(const cg_Grid *G, int r, int c);

/* structure */

struct cg_Diff {
    int fill_min;
    int (*scroll)(cg_Diff *D, int top, int bot, int n);
    int (*move)(cg_Diff *D, int r, int c);
    int (*style)(cg_Diff *D, unsigned st);
    int (*fill)(cg_Diff *D, int n, int cp);
    int (*put)(cg_Diff *D, int cp);
    int (*finish)(cg_Diff *D);
};

struct cg_Grid {
    int          top, rows, cols;
    int          all_dirty, scroll;
    int          off;
    int          tabstop;
    cg_Allocf   *allocf;
    void        *ud;
    cg_WcWidthf *wcwidthf;
    void        *wud;
    int         *cur_cp;
    unsigned    *cur_st;
    int         *back_cp;
    unsigned    *back_st;
};

CG_NS_END

#endif /* cellgrid_h */

/* ======================================================================== */
/*                           IMPLEMENTATION                                 */
/* ======================================================================== */

#if defined(CG_IMPLEMENTATION) && !defined(cg_implemented)
#define cg_implemented

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifndef CG_DEFAULT_MINFILL
# define CG_DEFAULT_MINFILL 4
#endif

CG_NS_BEGIN

#define cg_min(a, b)  ((a) < (b) ? (a) : (b))
#define cg_max(a, b)  ((a) > (b) ? (a) : (b))
#define cgR_idx(G, r) (((G)->off + (unsigned)(r)) % (unsigned)(G)->rows)
#define cgF_cursz(G)  ((G)->rows * (G)->cols * (sizeof(int) + sizeof(unsigned)))
#define cgF_gridsz(G) (cgF_cursz(G) * 2)

#define cgP_checkrc(G, r, c)                                     \
    ((G) && (G)->rows && (r) >= 0 && (r) < (G)->rows && (c) >= 0 \
     && (c) < (G)->cols)

/* misc helpers */

/* clang-format off */
static int cgC_wc(const cg_Grid *G, int cp)
{ return G->wcwidthf && G->wcwidthf(G->wud, cp) > 1 ? 2 : 1; }

static int cgC_tab(const cg_Grid *G, int col)
{ return G->tabstop > 1 ? G->tabstop - col % G->tabstop : 1; }

CG_API cg_Slice cg_slice(const char *s, size_t len)
{ cg_Slice sl; return sl.s = s, sl.e = s + len, sl; }

CG_API int cg_cols(const cg_Grid *G, int c, cg_Slice s)
{ while (s.s < s.e) c += cg_next(G, c, &s); return c; }

CG_API void cg_setwcwidth(cg_Grid *G, cg_WcWidthf *f, void *ud)
{ if (G) G->wcwidthf = f, G->wud = ud; }

CG_API void cg_settabstop(cg_Grid *G, int ts)
{ if (G) G->tabstop = ts; }
/* clang-format on */

static int cgK_utflen(const char *s) {
    int b = (unsigned char)*s;
    if (b < 0x80) return 1;
    if (b < 0xc0) return 0;
    if (b < 0xe0) return 2;
    if (b < 0xf0) return 3;
    return 4;
}

static int cgK_tocp(const char *s, int len) {
    int b = (unsigned char)*s;
    if (b < 0x80) return b;
    if (len >= 2 && b < 0xe0)
        return ((b & 0x1f) << 6) | ((unsigned char)s[1] & 0x3f);
    if (len >= 3 && b < 0xf0)
        return ((b & 0x0f) << 12) | (((unsigned char)s[1] & 0x3f) << 6)
             | ((unsigned char)s[2] & 0x3f);
    return ((b & 0x07) << 18) | (((unsigned char)s[1] & 0x3f) << 12)
         | (((unsigned char)s[2] & 0x3f) << 6) | ((unsigned char)s[3] & 0x3f);
}

CG_API int cg_next(const cg_Grid *G, int c, cg_Slice *s) {
    int b;
    if (s->s == s->e) return 0;
    b = *s->s & 0xFF;
    if (b == '\t') return s->s++, cgC_tab(G, c);
    if (b >= 0xc0) {
        int w, n = cgK_utflen(s->s);
        if (s->s + n <= s->e)
            return w = cgC_wc(G, cgK_tocp(s->s, n)), s->s += n, w;
        return s->s++, 1; /* truncated tail: single byte, width 1 */
    }
    if (b >= 0x80) return s->s++, 0; /* stray continuation: skipped */
    return s->s++, 1;
}

CG_API size_t cg_byte(const cg_Grid *G, int c, cg_Slice s, int col) {
    size_t off = 0;
    int    start = c;
    if (col < 0) return 0;
    while (s.s < s.e) {
        cg_Slice t = s;
        int      w = cg_next(G, c, &t);
        if (c + w > start + col) break; /* clamp to char start */
        off += (size_t)(t.s - s.s), c += w, s = t;
    }
    return off;
}

static int cgF_initgrid(cg_Grid *G, int rows, int cols) {
    int    t = rows * cols;
    size_t tsz = t * (sizeof(int) + sizeof(unsigned)) * 2;
    if (!(G->cur_cp = (int *)G->allocf(G->ud, NULL, 0, tsz))) return CG_ERRMEM;
    G->cur_st = (unsigned *)(G->cur_cp + t);
    G->back_cp = (int *)(G->cur_st + t);
    G->back_st = (unsigned *)(G->back_cp + t);
    memset(G->cur_cp, 0, tsz);
    G->rows = rows, G->cols = cols, G->off = 0, G->all_dirty = 1;
    return CG_OK;
}

static int cgF_resize(cg_Grid *G, int rows, int cols) {
    int       mr = cg_min(G->rows, rows), mc = cg_min(G->cols, cols);
    size_t    otsz = cgF_gridsz(G);
    size_t    t = rows * cols, csz = t * (sizeof(int) + sizeof(unsigned)) * 2;
    int      *nc, *nb, r;
    unsigned *ncs, *nbs;
    if (!(nc = (int *)G->allocf(G->ud, NULL, 0, csz))) return CG_ERRMEM;
    /* zeroed block: untouched tail cells of copied rows stay blank */
    memset(nc, 0, csz), ncs = (unsigned *)(nc + t);
    nb = (int *)(ncs + t), nbs = (unsigned *)(nb + t);
    for (r = 0; r < mr; ++r) {
        int oro = cgR_idx(G, r) * G->cols, nro = r * cols;
        memcpy(nc + nro, G->cur_cp + oro, mc * sizeof(int));
        memcpy(ncs + nro, G->cur_st + oro, mc * sizeof(unsigned));
        memcpy(nb + nro, G->back_cp + oro, mc * sizeof(int));
        memcpy(nbs + nro, G->back_st + oro, mc * sizeof(unsigned));
    }
    G->allocf(G->ud, G->cur_cp, otsz, 0);
    G->cur_cp = nc, G->cur_st = ncs, G->back_cp = nb, G->back_st = nbs;
    return G->off = 0, G->rows = rows, G->cols = cols, CG_OK;
}

static void cgF_blankrow(cg_Grid *G, int row) {
    int ro = cgR_idx(G, row) * G->cols;
    memset(G->cur_cp + ro, 0, G->cols * sizeof(int));
    memset(G->cur_st + ro, 0, G->cols * sizeof(unsigned));
    memset(G->back_cp + ro, 0, G->cols * sizeof(int));
    memset(G->back_st + ro, 0, G->cols * sizeof(unsigned));
}

/* public API */

static void *cgS_defallocf(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    if ((void)ud, (void)osize, nsize == 0) return (void)free(p), (void *)NULL;
    return (np = realloc(p, nsize)) ? np : ((void)abort(), NULL);
}

CG_API int cg_init(cg_Grid *G, cg_Allocf *f, void *ud) {
    cg_Allocf *allocf = f ? f : cgS_defallocf;
    if (!G) return CG_ERRPARAM;
    memset(G, 0, sizeof(cg_Grid)), G->allocf = allocf, G->ud = ud;
    return CG_OK;
}

CG_API void cg_free(cg_Grid *G) {
    if (G == NULL) return;
    if (G->cur_cp) G->allocf(G->ud, G->cur_cp, cgF_gridsz(G), 0);
    cg_init(G, G->allocf, G->ud);
}

CG_API int cg_begin(cg_Grid *G, int top, int rows, int cols) {
    int delta, row, r = CG_OK;
    if (G == NULL || rows == 0 || cols == 0) return CG_ERRPARAM;
    if (!G->rows)
        r = cgF_initgrid(G, rows, cols);
    else if (rows != G->rows || cols != G->cols)
        r = cgF_resize(G, rows, cols);
    if (r != CG_OK) return r;
    if (G->all_dirty) G->off = 0;
    /* delta==0 keeps the ring offset rotated by the last scroll frame:
     * resetting it misaligns cur vs back -> spurious full redraw */
    if (G->all_dirty || (delta = G->top - top) == 0)
        return G->top = top, G->scroll = 0, CG_OK;
    G->top = top, G->scroll = delta;
    if (delta < 0 ? -delta >= G->rows : delta >= G->rows)
        return G->off = 0, G->all_dirty = 1, G->scroll = 0, CG_OK;
    G->off = (G->off - delta + G->rows) % G->rows;
    /* rows the scroll leaves physically blank: blank cur+back so diff
     * redraws them fully — skip never matches an empty back row */
    if (delta > 0)
        for (row = 0; row < delta; ++row) cgF_blankrow(G, row);
    else
        for (row = G->rows + delta; row < G->rows; ++row) cgF_blankrow(G, row);
    return CG_OK;
}

CG_API void cg_clear(cg_Grid *G) {
    if (!cg_valid(G)) return;
    memset(G->cur_cp, 0, cgF_cursz(G)), G->off = 0, G->all_dirty = 1;
}

static void cgF_putcp(cg_Grid *G, int r, int c, int cp, int w, unsigned st) {
    int       ro = cgR_idx(G, r) * G->cols, *pc = G->cur_cp + ro + c;
    unsigned *ps = G->cur_st + ro + c;
    if (c >= 1 && pc[-1] != -1 && pc[0] == -1) pc[-1] = 0;
    if (c + 1 < G->cols && pc[1] == -1) pc[1] = 0;
    if (w == 2) {
        if (c + 1 >= G->cols)
            cp = '>';
        else
            pc[1] = -1, ps[1] = st;
    }
    *pc = cp, *ps = st;
}

CG_API void cg_put(cg_Grid *G, int r, int c, int cp, unsigned st) {
    int w;
    if (!cgP_checkrc(G, r, c)) return;
    if ((w = G->wcwidthf ? G->wcwidthf(G->wud, cp) : 1) > 1) w = 2;
    cgF_putcp(G, r, c, cp, w, st);
}

CG_API void cg_clearrow(cg_Grid *G, int r, int cs, int ce) {
    int ro;
    if (!cg_valid(G) || r < 0 || r >= G->rows) return;
    ce = cg_max(0, cg_min(ce, G->cols)), cs = cg_max(0, cg_min(cs, ce));
    ro = cgR_idx(G, r) * G->cols;
    memset(G->cur_cp + ro + cs, 0, (ce - cs) * sizeof(int));
    memset(G->cur_st + ro + cs, 0, (ce - cs) * sizeof(unsigned));
}

CG_API void cg_fill(cg_Grid *G, int r, int cs, int ce, int cp) {
    int i, ro;
    if (!cg_valid(G) || r < 0 || r >= G->rows) return;
    ce = cg_max(0, cg_min(ce, G->cols)), cs = cg_max(0, cg_min(cs, ce));
    ro = cgR_idx(G, r) * G->cols;
    for (i = cs; i < ce; i++) G->cur_cp[ro + i] = cp;
}

CG_API void cg_span(cg_Grid *G, int r, int cs, int ce, unsigned st) {
    int i, ro;
    if (!cg_valid(G) || r < 0 || r >= G->rows) return;
    if (ce > G->cols) ce = G->cols;
    ce = cg_max(0, cg_min(ce, G->cols)), cs = cg_max(0, cg_min(cs, ce));
    ro = cgR_idx(G, r) * G->cols;
    for (i = cs; i < ce; i++) G->cur_st[ro + i] = st;
}

CG_API int cg_putslice(cg_Grid *G, int r, int c, cg_Slice s, unsigned st) {
    if (!cgP_checkrc(G, r, c)) return c;
    while (s.s < s.e && c < G->cols) {
        int b = *s.s & 0xFF, cp, w = 1, n = 1;
        if (b == '\t' && G->tabstop > 1) { /* tab expands */
            int k = cgC_tab(G, c);
            while (k-- && c < G->cols) cgF_putcp(G, r, c++, ' ', 1, st);
            s.s++;
        } else if (b >= 0x80 && b < 0xc0)
            s.s++; /* stray continuation: skipped */
        else {
            if (b < 0xc0)
                cp = b;
            else if (s.s + (n = cgK_utflen(s.s)) <= s.e)
                cp = cgK_tocp(s.s, n), w = cgC_wc(G, cp);
            else
                cp = b, n = 1; /* truncated tail: single byte, width 1 */
            cgF_putcp(G, r, c, cp, w, st), c += w, s.s += n;
        }
    }
    return c;
}

CG_API void cg_freeze(cg_Grid *G) {
    if (!cg_valid(G)) return;
    memcpy(G->back_cp, G->cur_cp, cgF_cursz(G)), G->all_dirty = 0;
}

static int cgD_skip(const cg_Grid *G, int ro, int c) {
    while (!G->all_dirty && G->cur_cp[ro + c] == G->back_cp[ro + c]
           && G->cur_st[ro + c] == G->back_st[ro + c])
        if (++c >= G->cols) break;
    return c;
}

static int cgD_rep(const cg_Grid *G, int ro, int c) {
    int       r = 1, *pc = G->cur_cp + ro + c, cp = *pc;
    unsigned *ps = G->cur_st + ro + c, st = *ps;
    for (; c + r < G->cols; ++r)
        if (pc[r] != cp || ps[r] != st) break;
    return r;
}

#define cgD_call(f, args) ((f) && (r = f args) ? r : CG_OK)

static int cgD_row(const cg_Grid *G, cg_Diff *D, int row) {
    int      i, r = CG_OK, cp, f, col = 0, ro = cgR_idx(G, row) * G->cols;
    unsigned st = 0;
    for (; col < G->cols && (col = cgD_skip(G, ro, col)) < G->cols; col += f) {
        f = cgD_rep(G, ro, col), cp = G->cur_cp[ro + col];
        if ((cp = cp ? cp : ' ') <= 0) continue;
        if (cgD_call(D->move, (D, row, col))) return r;
        if (G->cur_st[ro + col] != st) {
            if (cgD_call(D->style, (D, G->cur_st[ro + col]))) return r;
            st = G->cur_st[ro + col];
        }
        if (f < D->fill_min) {
            for (i = 0; i < f; ++i)
                if (cgD_call(D->put, (D, cp))) return r;
        } else if (cgD_call(D->fill, (D, f, cp)))
            return r;
    }
    return st ? cgD_call(D->style, (D, 0)) : CG_OK;
}

CG_API int cg_diff(const cg_Grid *G, cg_Diff *D) {
    int row, r = CG_OK;
    if (!cg_valid(G) || !D) return CG_ERRPARAM;
    if (D->fill_min <= 1) D->fill_min = CG_DEFAULT_MINFILL;
    if (G->scroll && cgD_call(D->scroll, (D, 1, G->rows, G->scroll))) return r;
    for (row = 0; row < G->rows; ++row)
        if ((r = cgD_row(G, D, row))) return r;
    return cgD_call(D->finish, (D));
}

/* getters */

CG_API int cg_cell(const cg_Grid *G, int r, int c, unsigned *st) {
    int ro, cp;
    if (!cgP_checkrc(G, r, c)) return ((void)(st && (*st = 0)), 0);
    ro = cgR_idx(G, r) * G->cols;
    if (st) *st = G->cur_st[ro + c];
    return cp = G->cur_cp[ro + c], cp == 0 ? ' ' : cp;
}

CG_API int cg_back(const cg_Grid *G, int r, int c, unsigned *st) {
    int ro, cp;
    if (!cgP_checkrc(G, r, c)) return ((void)(st && (*st = 0)), 0);
    ro = cgR_idx(G, r) * G->cols;
    if (st) *st = G->back_st[ro + c];
    return cp = G->back_cp[ro + c], cp == 0 ? ' ' : cp;
}

CG_API int cg_isdirty(const cg_Grid *G, int r, int c) {
    int ro;
    if (!cgP_checkrc(G, r, c)) return 0;
    ro = cgR_idx(G, r) * G->cols;
    return G->cur_cp[ro + c] != G->back_cp[ro + c]
        || G->cur_st[ro + c] != G->back_st[ro + c];
}

CG_NS_END

#endif /* CG_IMPLEMENTATION && !cg_implemented */
