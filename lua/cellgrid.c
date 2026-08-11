#ifdef _MSC_VER
# define _CRT_SECURE_NO_DEPRECATE 1
# define _CRT_SECURE_NO_WARNINGS  1
#endif

#define LUA_LIB
#include <assert.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef _WIN32
# include <sys/ioctl.h>
#endif

#define CG_STATIC_API
#include "cellgrid.h"
#include "unidata.h"

#define LCG_GRID_TYPE "cellgrid.Grid"

/* ---- compat (PUC 5.5 + LuaJIT 2.1/5.1) --- */

#if LUA_VERSION_NUM < 502
# define LUA_OK                 0
# define lua_rawlen             lua_objlen
# define luaL_setfuncs(L, l, n) (assert(n == 0), luaL_register(L, NULL, l))
# define luaL_setmetatable(L, name) \
    (luaL_getmetatable((L), (name)), lua_setmetatable(L, -2))
# ifndef LUA_GCISRUNNING
#  define luaL_newlib(L, l) (lua_newtable(L), luaL_register(L, NULL, l))
# endif
#endif

#ifndef lua_isinteger
# define lua_isinteger(L, idx) (lua_type((L), (idx)) == LUA_TNUMBER)
#endif

#ifndef lua_isnoneornil
# define lua_isnoneornil(L, idx) (lua_type((L), (idx)) <= 0)
#endif

/* lua_geti is 5.3+; 5.1/5.2 (and LuaJIT) emulate via gettable so
 * metatable __index fires — style tables may be dynamic proxies */
#if LUA_VERSION_NUM >= 503
# define lua53_geti lua_geti
#else
static int lua53_geti(lua_State *L, int idx, lua_Integer i) {
    lua_pushinteger(L, i);
    lua_gettable(L, idx);
    return lua_type(L, -1);
}
#endif

/* lua_rawseti takes int (5.1/5.2) or lua_Integer (5.3+) */
#if LUA_VERSION_NUM < 503
# define lcg_rawseti(L, i, n) lua_rawseti((L), (i), (int)(n))
#else
# define lcg_rawseti(L, i, n) lua_rawseti((L), (i), (n))
#endif

/* ---- grid userdata ---- */

static cg_Grid *lcg_check(lua_State *L, int idx) {
    return (cg_Grid *)luaL_checkudata(L, idx, LCG_GRID_TYPE);
}

static int lcg_cptoutf8(int cp, char *buf) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xc0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xe0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        buf[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    buf[0] = (char)(0xf0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    buf[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* ---- wcwidth (from unidata.h) ---- */

#define lcgW_tablesize(t) (sizeof(t) / sizeof((t)[0]))

static int lcg_find(const range_table *t, size_t size, utfint ch) {
    size_t begin = 0, end = size;
    while (begin < end) {
        size_t mid = (begin + end) / 2;
        if (t[mid].last < ch)
            begin = mid + 1;
        else if (t[mid].first > ch)
            end = mid;
        else
            return (int)((ch - t[mid].first) % (utfint)t[mid].step == 0);
    }
    return 0;
}

static int lcgW_width(void *ud, int cp) {
    utfint ch = (utfint)cp;
    (void)ud;
    if (lcg_find(zerowidth_table, lcgW_tablesize(zerowidth_table), ch))
        return 0;
    if (lcg_find(doublewidth_table, lcgW_tablesize(doublewidth_table), ch))
        return 2;
    if (lcg_find(ambiwidth_table, lcgW_tablesize(ambiwidth_table), ch))
        return 2;
    return 1;
}

/* ===== Grid lifecycle ===== */

static int Lgrid_delete(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    if (cg_valid(g)) cg_free(g);
    return 0;
}

static int Lgrid_new(lua_State *L) {
    cg_Grid *g = (cg_Grid *)lua_newuserdata(L, sizeof(cg_Grid));
    cg_init(g, NULL, NULL);
    cg_setwcwidth(g, lcgW_width, NULL);
    cg_settabstop(g, 4);
    luaL_setmetatable(L, LCG_GRID_TYPE);
    return 1;
}

/* ===== Frame methods ===== */

static int lcg_checkerror(lua_State *L, int r) {
    switch (r) {
    case CG_OK: return r;
    case CG_ERRMEM: return luaL_error(L, "cellgrid: out of memory");
    case CG_ERRPARAM: return luaL_error(L, "cellgrid: invalid parameter");
    default: return luaL_error(L, "cellgrid: unknown error(%d)", r);
    }
}

static int Lgrid_begin(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    int      top = (int)luaL_checkinteger(L, 2);
    int      rows = (int)luaL_checkinteger(L, 3);
    int      cols = (int)luaL_checkinteger(L, 4);
    return lcg_checkerror(L, cg_begin(g, top, rows, cols)), lua_settop(L, 1), 1;
}

/* clang-format off */
static int Lgrid_clear(lua_State *L)
{ return cg_clear(lcg_check(L, 1)), lua_settop(L, 1), 1; }

static int Lgrid_freeze(lua_State *L)
{ return cg_freeze(lcg_check(L, 1)), lua_settop(L, 1), 1; }
/* clang-format on */

/* ===== Cell writing methods ===== */

static int Lgrid_put(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    int      r = (int)luaL_checkinteger(L, 2);
    int      c = (int)luaL_checkinteger(L, 3);
    int      cp = (int)luaL_checkinteger(L, 4);
    unsigned st = (unsigned)luaL_optinteger(L, 5, 0);
    return cg_put(g, r, c, cp, st), lua_settop(L, 1), 1;
}

static int Lgrid_clearrow(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    int      r = (int)luaL_checkinteger(L, 2);
    int      cs = (int)luaL_checkinteger(L, 3);
    int      ce = (int)luaL_checkinteger(L, 4);
    return cg_clearrow(g, r, cs, ce), lua_settop(L, 1), 1;
}

static int Lgrid_fill(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    int      r = (int)luaL_checkinteger(L, 2);
    int      cs = (int)luaL_checkinteger(L, 3);
    int      ce = (int)luaL_checkinteger(L, 4);
    int      cp = (int)luaL_checkinteger(L, 5);
    cg_fill(g, r, cs, ce, cp);
    if (!lua_isnoneornil(L, 6)) /* optional st: combine with a span */
        cg_span(g, r, cs, ce, (unsigned)luaL_checkinteger(L, 6));
    return lua_settop(L, 1), 1;
}

static int Lgrid_span(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    int      r = (int)luaL_checkinteger(L, 2);
    int      cs = (int)luaL_checkinteger(L, 3);
    int      ce = (int)luaL_checkinteger(L, 4);
    unsigned st = (unsigned)luaL_checkinteger(L, 5);
    return cg_span(g, r, cs, ce, st), lua_settop(L, 1), 1;
}

static int Lgrid_putslice(lua_State *L) {
    cg_Grid    *g = lcg_check(L, 1);
    int         r = (int)luaL_checkinteger(L, 2);
    int         c = (int)luaL_checkinteger(L, 3);
    unsigned    st = (unsigned)luaL_checkinteger(L, 4);
    size_t      len;
    const char *s = luaL_checklstring(L, 5, &len);
    lua_Integer i = luaL_optinteger(L, 6, 1);
    lua_Integer j = luaL_optinteger(L, 7, (lua_Integer)len);
    if (i < 1) i = 1;
    if (j > (lua_Integer)len) j = (lua_Integer)len;
    if (j < i) return lua_pushinteger(L, c), 1;
    /* 1-based inclusive (string.sub semantics) */
    return lua_pushinteger(L,
            cg_putslice(g, r, c, cg_slice(s + (i - 1), (size_t)(j - i + 1)),
                    st)), 1;
}

/* ===== Getter methods ===== */

static int Lgrid_cell(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    int      r = (int)luaL_checkinteger(L, 2);
    int      c = (int)luaL_checkinteger(L, 3);
    unsigned st;
    int      cp = cg_cell(g, r, c, &st);
    lua_pushinteger(L, cp);
    lua_pushinteger(L, (int)st);
    return 2;
}

static int Lgrid_back(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    int      r = (int)luaL_checkinteger(L, 2);
    int      c = (int)luaL_checkinteger(L, 3);
    unsigned st;
    int      cp = cg_back(g, r, c, &st);
    lua_pushinteger(L, cp);
    lua_pushinteger(L, (int)st);
    return 2;
}

static int Lgrid_isdirty(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    int      r = (int)luaL_checkinteger(L, 2);
    int      c = (int)luaL_checkinteger(L, 3);
    return lua_pushboolean(L, cg_isdirty(g, r, c)), 1;
}

/* clang-format off */
static int Lgrid_rows(lua_State *L)
{ return lua_pushinteger(L, cg_rows(lcg_check(L, 1))), 1; }

static int Lgrid_ncols(lua_State *L)
{ return lua_pushinteger(L, cg_ncols(lcg_check(L, 1))), 1; }

static int Lgrid_top(lua_State *L)
{ return lua_pushinteger(L, cg_top(lcg_check(L, 1))), 1; }
/* clang-format on */

/* ===== Diff rendering (shared: fd>=0→write, fd=-1→luaL_Buffer) ===== */

#define LCG_KEY_CUP "cursor_address"
#define LCG_KEY_CSR "change_scroll_region"
#define LCG_KEY_IND "parm_index"
#define LCG_KEY_RIN "parm_rindex"
#define LCG_KEY_REP "repeat_char"

#define LCG_DEF_CUP "\x1b[%d;%dH"
#define LCG_DEF_CSR "\x1b[%d;%dr"
#define LCG_DEF_IND "\x1b[%dS"
#define LCG_DEF_RIN "\x1b[%dT"
#define LCG_DEF_REP "\x1b[%db"
#define LCG_BUF     128

typedef struct lcg_Diff {
    cg_Diff     base;
    lua_State  *L;
    int         optidx;
    int         fd;
    luaL_Buffer b;
    int         rows;
    const char *cup_fmt;
    const char *csr_fmt;
    const char *indn_fmt;
    const char *rin_fmt;
    const char *rep_fmt;
} lcg_Diff;

static const char *lcg_opt(lua_State *L, int i, const char *k, const char *d) {
    const char *s;
    lua_getfield(L, i, k);
    if (!lua_isstring(L, -1)) return lua_pop(L, 1), d;
    s = lua_tostring(L, -1);
    return lua_pop(L, 1), s;
}

static const char *lcg_styleof(lua_State *L, int idx, unsigned st) {
    const char *s;
    lua53_geti(L, idx, (lua_Integer)st);
    s = lua_tostring(L, -1);
    lua_pop(L, 1);
    return s;
}

static int lcg_output(lcg_Diff *d, const char *buf, int len) {
    if (len <= 0) return 0;
    if (d->fd >= 0)
        return write(d->fd, buf, (size_t)len) == (ssize_t)len ? 0 : -1;
    return luaL_addlstring(&d->b, buf, (size_t)len), 0;
}

#define lcg_writef(D, fmt, ...)                               \
    do {                                                      \
        char _b[LCG_BUF];                                     \
        int  _w = snprintf(_b, sizeof(_b), fmt, __VA_ARGS__); \
        if (lcg_output((D), _b, _w)) return CG_ERRPARAM;      \
    } while (0)

static int lcg_scroll(cg_Diff *D, int top, int bot, int n) {
    /* n = viewport delta (cg_begin): n>0 = viewport up → content down (rin),
     * n<0 = viewport down → content up (indn) */
    lcg_Diff *d = (lcg_Diff *)D;
    lcg_writef(d, d->csr_fmt, top, bot);
    if (n > 0)
        lcg_writef(d, d->rin_fmt, n);
    else if (n < 0)
        lcg_writef(d, d->indn_fmt, -n);
    lcg_writef(d, d->csr_fmt, 1, d->rows);
    return 0;
}

static int lcg_move(cg_Diff *D, int r, int c) {
    lcg_Diff *d = (lcg_Diff *)D;
    lcg_writef(d, d->cup_fmt, r + 1, c + 1);
    return 0;
}

static int lcg_style(cg_Diff *D, unsigned st) {
    lcg_Diff   *d = (lcg_Diff *)D;
    const char *s = lcg_styleof(d->L, d->optidx, st);
    if (s) return lcg_output(d, s, (int)strlen(s));
    return 0;
}

static int lcg_fill(cg_Diff *D, int n, int cp) {
    lcg_Diff *d = (lcg_Diff *)D;
    char      buf[8];
    int       len = lcg_cptoutf8(cp, buf);
    if (lcg_output(d, buf, len)) return CG_ERRPARAM;
    if (n > 1) lcg_writef(d, d->rep_fmt, n - 1);
    return 0;
}

static int lcg_put(cg_Diff *D, int cp) {
    lcg_Diff *d = (lcg_Diff *)D;
    char      buf[8];
    int       len = lcg_cptoutf8(cp, buf);
    return lcg_output(d, buf, len);
}

static int lcg_finish(cg_Diff *D) {
    lcg_Diff   *d = (lcg_Diff *)D;
    const char *s = lcg_styleof(d->L, d->optidx, 0U);
    if (s) return lcg_output(d, s, (int)strlen(s));
    return 0;
}

static void lcg_initdiff(lcg_Diff *d, lua_State *L, int idx, int fd, int rows) {
    memset(d, 0, sizeof(*d));
    d->L = L, d->fd = fd, d->rows = rows;
    d->optidx = idx;
    d->cup_fmt = lcg_opt(L, idx, LCG_KEY_CUP, LCG_DEF_CUP);
    d->csr_fmt = lcg_opt(L, idx, LCG_KEY_CSR, LCG_DEF_CSR);
    d->indn_fmt = lcg_opt(L, idx, LCG_KEY_IND, LCG_DEF_IND);
    d->rin_fmt = lcg_opt(L, idx, LCG_KEY_RIN, LCG_DEF_RIN);
    d->rep_fmt = lcg_opt(L, idx, LCG_KEY_REP, LCG_DEF_REP);
    lua_getfield(L, idx, "fill_min");
    d->base.fill_min = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    d->base.scroll = lcg_scroll;
    d->base.move = lcg_move;
    d->base.style = lcg_style;
    d->base.fill = lcg_fill;
    d->base.put = lcg_put;
    d->base.finish = lcg_finish;
}

static int Lgrid_diff(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    lcg_Diff d;
    if (lua_isnoneornil(L, 2))
        lua_settop(L, 1), lua_newtable(L);
    else
        luaL_checktype(L, 2, LUA_TTABLE);
    lcg_initdiff(&d, L, 2, -1, cg_rows(g));
    luaL_buffinit(L, &d.b);
    lcg_checkerror(L, cg_diff(g, &d.base));
    return luaL_pushresult(&d.b), 1;
}

static int Lgrid_render(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    lcg_Diff d;
    int      fd = (int)luaL_checkinteger(L, 2);
    if (lua_isnoneornil(L, 3))
        lua_settop(L, 2), lua_newtable(L);
    else
        luaL_checktype(L, 3, LUA_TTABLE);
    lcg_initdiff(&d, L, 3, fd, cg_rows(g));
    if (fd < 0) luaL_buffinit(L, &d.b); /* buffer mode: return the CSI */
    lcg_checkerror(L, cg_diff(g, &d.base));
    return fd < 0 ? (luaL_pushresult(&d.b), 1) : (lua_settop(L, 1), 1);
}

/* ===== winsize ===== */

static int Lgrid_winsize(lua_State *L) {
    int fd = (int)luaL_optinteger(L, 1, 0);
#ifndef _WIN32
    struct winsize ws = {0};
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0) {
        lua_pushinteger(L, ws.ws_row);
        lua_pushinteger(L, ws.ws_col);
        return 2;
    }
#endif
    return 0;
}

/* ===== column conversion (Grid:cols / Grid:byte / Grid:next) ===== */

/* Grid:cols(text, off?, c?) — display column at byte off (default #text)
 * of text rendered from column c (default 0). */
static int Lgrid_cols(lua_State *L) {
    cg_Grid    *g = lcg_check(L, 1);
    size_t      len, tlen;
    const char *s = luaL_checklstring(L, 2, &tlen);
    int         c = (int)luaL_optinteger(L, 4, 0);
    len = (size_t)luaL_optinteger(L, 3, (lua_Integer)tlen);
    if (len > tlen) len = tlen;
    return lua_pushinteger(L, cg_cols(g, c, cg_slice(s, len))), 1;
}

/* Grid:byte(text, col, c?) — byte offset of column c+col (col relative
 * to c, default 0). */
static int Lgrid_byte(lua_State *L) {
    cg_Grid    *g = lcg_check(L, 1);
    size_t      len;
    const char *s = luaL_checklstring(L, 2, &len);
    int         col = (int)luaL_checkinteger(L, 3);
    int         c = (int)luaL_optinteger(L, 4, 0);
    return lua_pushinteger(L,
            (lua_Integer)cg_byte(g, c, cg_slice(s, len), col)), 1;
}

/* iterator body: state table {grid, text, off, len, col} passed as the
 * generic-for state. Yields (byte 1-based, start col); advances via
 * cg_next. */
static int Lgrid_next_iter(lua_State *L) {
    cg_Grid    *g;
    const char *s;
    size_t      tlen;
    lua_Integer off, len, col;
    cg_Slice    sl;
    int         w;
    lua_getfield(L, 1, "grid");
    g = (cg_Grid *)lua_touserdata(L, -1);
    lua_getfield(L, 1, "text");
    s = lua_tolstring(L, -1, &tlen);
    lua_getfield(L, 1, "off");
    off = lua_tointeger(L, -1);
    lua_getfield(L, 1, "len");
    len = lua_tointeger(L, -1);
    lua_getfield(L, 1, "col");
    col = lua_tointeger(L, -1);
    lua_pop(L, 5);
    if (off >= len) return 0;
    sl.s = s + off, sl.e = s + len;
    w = cg_next((const cg_Grid *)g, (int)col, &sl);
    lua_pushinteger(L, off + 1); /* byte (1-based) */
    lua_pushinteger(L, col);     /* start column */
    lua_pushinteger(L, off + (lua_Integer)(sl.s - (s + off)));
    lua_setfield(L, 1, "off");
    lua_pushinteger(L, col + w);
    lua_setfield(L, 1, "col");
    return 2;
}

/* Grid:next(text, c?) — cluster iterator: for byte, col in g:next(text) */
static int Lgrid_next(lua_State *L) {
    lua_Integer c = luaL_optinteger(L, 3, 0); /* read args first */
    size_t      len;
    luaL_checklstring(L, 2, &len);
    lua_newtable(L); /* state */
    lua_pushvalue(L, 1); /* grid */
    lua_setfield(L, -2, "grid");
    lua_pushvalue(L, 2); /* text */
    lua_setfield(L, -2, "text");
    lua_pushinteger(L, 0); /* off */
    lua_setfield(L, -2, "off");
    lua_pushinteger(L, (lua_Integer)len); /* len */
    lua_setfield(L, -2, "len");
    lua_pushinteger(L, c); /* col */
    lua_setfield(L, -2, "col");
    lua_pushcfunction(L, Lgrid_next_iter);
    lua_pushvalue(L, -2); /* state */
    return 2;             /* iter, state */
}

static int Lgrid_settabstop(lua_State *L) {
    cg_Grid *g = lcg_check(L, 1);
    int      ts = (int)luaL_checkinteger(L, 2);
    return cg_settabstop(g, ts), lua_settop(L, 1), 1;
}

static int Lgrid_tabstop(lua_State *L) {
    return lua_pushinteger(L, cg_tabstop(lcg_check(L, 1))), 1;
}

/* ===== Module registration ===== */

LUALIB_API int luaopen_cellgrid(lua_State *L) {
    luaL_Reg libs[] = {
            {"__gc", Lgrid_delete},
#define ENTRY(name) {#name, Lgrid_##name}
            ENTRY(new),
            ENTRY(delete),
            ENTRY(begin),
            ENTRY(clear),
            ENTRY(freeze),
            ENTRY(put),
            ENTRY(clearrow),
            ENTRY(fill),
            ENTRY(span),
            ENTRY(putslice),
            ENTRY(diff),
            ENTRY(render),
            ENTRY(cell),
            ENTRY(back),
            ENTRY(isdirty),
            ENTRY(rows),
            ENTRY(ncols),
            ENTRY(top),
            ENTRY(winsize),
            ENTRY(cols),
            ENTRY(byte),
            ENTRY(next),
            ENTRY(settabstop),
            ENTRY(tabstop),
#undef ENTRY
            {NULL, NULL}};
    if (luaL_newmetatable(L, LCG_GRID_TYPE)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
    return 1;
}
