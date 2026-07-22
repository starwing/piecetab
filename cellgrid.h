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

/* lifecycle */
CG_API int  cg_init(cg_Grid *G, cg_Allocf *f, void *ud);
CG_API void cg_free(cg_Grid *G);
CG_API void cg_setwcwidth(cg_Grid *G, cg_WcWidthf *f, void *ud);

#define cg_valid(G) ((G) && (G)->rows)

/* frame */
CG_API int  cg_begin(cg_Grid *G, int top, int rows, int cols);
CG_API void cg_clear(cg_Grid *G);

/* cell write */
CG_API void cg_put(cg_Grid *G, int r, int c, int cp, unsigned st);
CG_API void cg_clearrow(cg_Grid *G, int r, int cs, int ce);
CG_API void cg_fill(cg_Grid *G, int r, int cs, int ce, int cp);
CG_API void cg_span(cg_Grid *G, int r, int cs, int ce, unsigned st);
CG_API int  cg_putline(cg_Grid *G, int r, int c, const char *s, unsigned st);

/* diff / freeze  */
CG_API int  cg_diff(const cg_Grid *G, cg_Diff *diff);
CG_API void cg_freeze(cg_Grid *G);

/* getters */

#define cg_rows(G) ((G) ? (G)->rows : 0)
#define cg_cols(G) ((G) ? (G)->cols : 0)
#define cg_top(G)  ((G) ? (G)->top : 0)

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

#define cgP_checkrc(G, r, c)                                     \
    ((G) && (G)->rows && (r) >= 0 && (r) < (G)->rows && (c) >= 0 \
     && (c) < (G)->cols)

/* internal helpers */

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
    size_t    otsz = G->rows * G->cols * (sizeof(int) + sizeof(unsigned)) * 2;
    size_t    t = rows * cols, csz = t * (sizeof(int) + sizeof(unsigned)) * 2;
    int      *nc, *nb, r;
    unsigned *ncs, *nbs;
    if (!(nc = (int *)G->allocf(G->ud, NULL, 0, csz))) return CG_ERRMEM;
    memset(nc, 0, csz), ncs = (unsigned *)(nc + t);
    nb = (int *)(ncs + t), nbs = (unsigned *)(nb + t);
    for (r = 0; r < rows; r++) {
        int oro = cgR_idx(G, r) * G->cols, nro = r * cols, e = 0;
        if (r < mr) {
            e = mc, memcpy(nc + nro, G->cur_cp + oro, mc * sizeof(int));
            memcpy(ncs + nro, G->cur_st + oro, mc * sizeof(unsigned));
            memcpy(nb + nro, G->back_cp + oro, mc * sizeof(int));
            memcpy(nbs + nro, G->back_st + oro, mc * sizeof(unsigned));
        }
        memset(nc + nro + e, 0, (cols - e) * sizeof(int));
        memset(ncs + nro + e, 0, (cols - e) * sizeof(unsigned));
        memset(nb + nro + e, 0, (cols - e) * sizeof(int));
        memset(nbs + nro + e, 0, (cols - e) * sizeof(unsigned));
    }
    G->allocf(G->ud, G->cur_cp, otsz, 0);
    G->cur_cp = nc, G->cur_st = ncs, G->back_cp = nb, G->back_st = nbs;
    return G->off = 0, G->rows = rows, G->cols = cols, CG_OK;
}

/* public API */

/* clang-format off */
CG_API void cg_setwcwidth(cg_Grid *G, cg_WcWidthf *f, void *ud)
{ if (G) G->wcwidthf = f, G->wud = ud; }
/* clang-format on */

static void *cgS_defallocf(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    if ((void)ud, (void)osize, nsize == 0) return (void)free(p), NULL;
    return (np = realloc(p, nsize)) ? np : ((void)abort(), NULL);
}

CG_API int cg_init(cg_Grid *G, cg_Allocf *f, void *ud) {
    cg_Allocf *allocf = f ? f : cgS_defallocf;
    if (!G) return CG_ERRPARAM;
    memset(G, 0, sizeof(cg_Grid)), G->allocf = allocf, G->ud = ud;
    return CG_OK;
}

CG_API void cg_free(cg_Grid *G) {
    size_t tsz;
    if (G == NULL) return;
    tsz = G->rows * G->cols * (sizeof(int) + sizeof(unsigned)) * 2;
    if (G->cur_cp) G->allocf(G->ud, G->cur_cp, tsz, 0);
    cg_init(G, G->allocf, G->ud);
}

CG_API int cg_begin(cg_Grid *G, int top, int rows, int cols) {
    int delta, row, ro, r = CG_OK;
    if (G == NULL || rows == 0 || cols == 0) return CG_ERRPARAM;
    if (!G->rows)
        r = cgF_initgrid(G, rows, cols);
    else if (rows != G->rows || cols != G->cols)
        r = cgF_resize(G, rows, cols);
    if (r != CG_OK) return r;
    if (G->all_dirty || (delta = G->top - top) == 0)
        return G->top = top, G->scroll = G->off = 0, CG_OK;
    G->top = top, G->scroll = delta;
    if (delta < 0 ? -delta >= G->rows : delta >= G->rows)
        return G->off = 0, G->all_dirty = 1, G->scroll = 0, CG_OK;
    G->off = (G->off - delta + G->rows) % G->rows;
    if (delta > 0)
        for (row = 0; row < delta; ++row) {
            ro = cgR_idx(G, row) * G->cols;
            memset(G->cur_cp + ro, 0, G->cols * sizeof(int));
            memset(G->cur_st + ro, 0, G->cols * sizeof(unsigned));
        }
    else {
        int nd = -delta;
        for (row = G->rows - nd; row < G->rows; ++row) {
            ro = cgR_idx(G, row) * G->cols;
            memset(G->cur_cp + ro, 0, G->cols * sizeof(int));
            memset(G->cur_st + ro, 0, G->cols * sizeof(unsigned));
        }
    }
    return CG_OK;
}

CG_API void cg_clear(cg_Grid *G) {
    size_t csz;
    if (!cg_valid(G)) return;
    csz = G->rows * G->cols * (sizeof(int) + sizeof(unsigned));
    memset(G->cur_cp, 0, csz), G->off = 0, G->all_dirty = 1;
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

CG_API int cg_putline(cg_Grid *G, int r, int c, const char *s, unsigned st) {
    if (!cgP_checkrc(G, r, c) || !s) return c;
    while (*s && c < G->cols) {
        int len, cp, w;
        while (*s && (len = cgK_utflen(s)) == 0) s++;
        if (!*s) break;
        cp = cgK_tocp(s, len);
        if ((w = G->wcwidthf ? G->wcwidthf(G->wud, cp) : 1) > 1) w = 2;
        cgF_putcp(G, r, c, cp, w, st);
        c += w, s += len;
    }
    return c;
}

CG_API void cg_freeze(cg_Grid *G) {
    size_t t;
    if (!cg_valid(G)) return;
    t = G->rows * G->cols * (sizeof(int) + sizeof(unsigned));
    memcpy(G->back_cp, G->cur_cp, t), G->all_dirty = 0;
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

CG_API int cg_diff(const cg_Grid *G, cg_Diff *D) {
    int row, r = CG_OK;
    if (!cg_valid(G) || !D) return CG_ERRPARAM;
    if (D->fill_min <= 1) D->fill_min = CG_DEFAULT_MINFILL;
    if (G->scroll && cgD_call(D->scroll, (D, 1, G->rows, G->scroll))) return r;
    for (row = 0; row < G->rows; ++row) {
        int      i, cp, f, col = 0, ro = cgR_idx(G, row) * G->cols;
        unsigned st = 0;
        for (; col < G->cols; col += f) {
            if ((col = cgD_skip(G, ro, col)) >= G->cols) break;
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
        if (st && cgD_call(D->style, (D, 0))) return r;
    }
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
