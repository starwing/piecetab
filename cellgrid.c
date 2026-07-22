#ifdef _MSC_VER
# define _CRT_SECURE_NO_DEPRECATE 1
# define _CRT_SECURE_NO_WARNINGS  1
#endif

#define LUA_LIB
#include <assert.h>
#include <lauxlib.h>
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

/* ---- grid userdata ---- */

typedef struct {
    cg_Grid g;
} lcg_Grid;

static lcg_Grid *lcg_check(lua_State *L, int idx) {
    return (lcg_Grid *)luaL_checkudata(L, idx, LCG_GRID_TYPE);
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

static int lcgW_find(const range_table *t, size_t size, utfint ch) {
    size_t begin = 0, end = size;
    while (begin < end) {
        size_t mid = (begin + end) / 2;
        if (t[mid].last < ch) begin = mid + 1;
        else if (t[mid].first > ch) end = mid;
        else return (int)((ch - t[mid].first) % (utfint)t[mid].step == 0);
    }
    return 0;
}

static int lcgW_width(void *ud, int cp) {
    utfint ch = (utfint)cp;
    (void)ud;
    if (lcgW_find(zerowidth_table, lcgW_tablesize(zerowidth_table), ch))
        return 0;
    if (lcgW_find(doublewidth_table, lcgW_tablesize(doublewidth_table), ch))
        return 2;
    if (lcgW_find(ambiwidth_table, lcgW_tablesize(ambiwidth_table), ch))
        return 2;
    return 1;
}

/* ===== Grid lifecycle ===== */

static int Lgrid_delete(lua_State *L) {
    cg_free(&lcg_check(L, 1)->g);
    return 0;
}

static int Lgrid_new(lua_State *L) {
    lcg_Grid *lg = (lcg_Grid *)lua_newuserdata(L, sizeof(lcg_Grid));
    cg_init(&lg->g, NULL, NULL);
    cg_setwcwidth(&lg->g, lcgW_width, NULL);
    luaL_setmetatable(L, LCG_GRID_TYPE);
    return 1;
}

/* ===== Frame methods ===== */

static int Lgrid_begin(lua_State *L) {
    lcg_Grid *lg = lcg_check(L, 1);
    int       top = (int)luaL_checkinteger(L, 2);
    int       rows = (int)luaL_checkinteger(L, 3);
    int       cols = (int)luaL_checkinteger(L, 4);
    int       r = cg_begin(&lg->g, top, rows, cols);
    if (r != CG_OK)
        luaL_error(
                L, r == CG_ERRMEM ? "cellgrid: out of memory"
                                  : "cellgrid: invalid parameter");
    return 0;
}

static int Lgrid_clear(lua_State *L) {
    cg_clear(&lcg_check(L, 1)->g);
    return 0;
}

static int Lgrid_freeze(lua_State *L) {
    cg_freeze(&lcg_check(L, 1)->g);
    return 0;
}

/* ===== Cell writing methods ===== */

static int Lgrid_put(lua_State *L) {
    lcg_Grid *lg = lcg_check(L, 1);
    int       r = (int)luaL_checkinteger(L, 2);
    int       c = (int)luaL_checkinteger(L, 3);
    int       cp = (int)luaL_checkinteger(L, 4);
    unsigned  st = (unsigned)luaL_optinteger(L, 5, 0);
    cg_put(&lg->g, r, c, cp, st);
    return 0;
}

static int Lgrid_clearrow(lua_State *L) {
    lcg_Grid *lg = lcg_check(L, 1);
    int       r = (int)luaL_checkinteger(L, 2);
    int       cs = (int)luaL_checkinteger(L, 3);
    int       ce = (int)luaL_checkinteger(L, 4);
    cg_clearrow(&lg->g, r, cs, ce);
    return 0;
}

static int Lgrid_fill(lua_State *L) {
    lcg_Grid *lg = lcg_check(L, 1);
    int       r = (int)luaL_checkinteger(L, 2);
    int       cs = (int)luaL_checkinteger(L, 3);
    int       ce = (int)luaL_checkinteger(L, 4);
    int       cp = (int)luaL_checkinteger(L, 5);
    cg_fill(&lg->g, r, cs, ce, cp);
    return 0;
}

static int Lgrid_span(lua_State *L) {
    lcg_Grid *lg = lcg_check(L, 1);
    int       r = (int)luaL_checkinteger(L, 2);
    int       cs = (int)luaL_checkinteger(L, 3);
    int       ce = (int)luaL_checkinteger(L, 4);
    unsigned  st = (unsigned)luaL_checkinteger(L, 5);
    cg_span(&lg->g, r, cs, ce, st);
    return 0;
}

static int Lgrid_putline(lua_State *L) {
    lcg_Grid   *lg = lcg_check(L, 1);
    int         r = (int)luaL_checkinteger(L, 2);
    int         c = (int)luaL_checkinteger(L, 3);
    size_t      len;
    const char *s = luaL_checklstring(L, 4, &len);
    unsigned    st = (unsigned)luaL_optinteger(L, 5, 0);
    lua_pushinteger(L, cg_putline(&lg->g, r, c, s, st));
    return 1;
}

/* ===== Getter methods ===== */

static int Lgrid_cell(lua_State *L) {
    lcg_Grid *lg = lcg_check(L, 1);
    int       r = (int)luaL_checkinteger(L, 2);
    int       c = (int)luaL_checkinteger(L, 3);
    unsigned  st;
    int       cp = cg_cell(&lg->g, r, c, &st);
    lua_pushinteger(L, cp);
    lua_pushinteger(L, (int)st);
    return 2;
}

static int Lgrid_back(lua_State *L) {
    lcg_Grid *lg = lcg_check(L, 1);
    int       r = (int)luaL_checkinteger(L, 2);
    int       c = (int)luaL_checkinteger(L, 3);
    unsigned  st;
    int       cp = cg_back(&lg->g, r, c, &st);
    lua_pushinteger(L, cp);
    lua_pushinteger(L, (int)st);
    return 2;
}

static int Lgrid_isdirty(lua_State *L) {
    lcg_Grid *lg = lcg_check(L, 1);
    int       r = (int)luaL_checkinteger(L, 2);
    int       c = (int)luaL_checkinteger(L, 3);
    lua_pushboolean(L, cg_isdirty(&lg->g, r, c));
    return 1;
}

static int Lgrid_rows(lua_State *L) {
    lua_pushinteger(L, lcg_check(L, 1)->g.rows);
    return 1;
}

static int Lgrid_cols(lua_State *L) {
    lua_pushinteger(L, lcg_check(L, 1)->g.cols);
    return 1;
}

static int Lgrid_top(lua_State *L) {
    lua_pushinteger(L, lcg_check(L, 1)->g.top);
    return 1;
}

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

typedef struct {
    cg_Diff     base;
    lua_State  *L;
    int         tblref;
    int         fd;
    luaL_Buffer lbuf;
    int         rows;
    const char *cup_fmt;
    const char *csr_fmt;
    const char *indn_fmt;
    const char *rin_fmt;
    const char *rep_fmt;
} lcg_Diff;

static const char *lcg_optstr(
        lua_State *L, int tblidx, const char *key, const char *def) {
    const char *s;
    lua_getfield(L, tblidx, key);
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        return def;
    }
    s = lua_tostring(L, -1);
    return lua_pop(L, 1), s;
}

static const char *lcg_styleof(lua_State *L, int tblref, unsigned st) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, tblref);
    lua_rawgeti(L, -1, (int)st);
    if (lua_isstring(L, -1)) {
        const char *s = lua_tostring(L, -1);
        lua_pop(L, 2);
        return s;
    }
    lua_pop(L, 2);
    return NULL;
}

static int lcg_output(lcg_Diff *d, const char *buf, int len) {
    if (len <= 0) return 0;
    if (d->fd >= 0)
        return write(d->fd, buf, (size_t)len) == (ssize_t)len ? 0 : -1;
    luaL_addlstring(&d->lbuf, buf, (size_t)len);
    return 0;
}

#define lcg_writef(D, fmt, ...)                               \
    do {                                                      \
        char _b[LCG_BUF];                                     \
        int  _w = snprintf(_b, sizeof(_b), fmt, __VA_ARGS__); \
        if (lcg_output((D), _b, _w)) return CG_ERRPARAM;      \
    } while (0)

static int Ldiff_scroll(cg_Diff *D, int top, int bot, int n) {
    lcg_Diff *d = (lcg_Diff *)D;
    lcg_writef(d, d->csr_fmt, top, bot);
    if (n > 0)
        lcg_writef(d, d->indn_fmt, n);
    else if (n < 0)
        lcg_writef(d, d->rin_fmt, -n);
    lcg_writef(d, d->csr_fmt, 1, d->rows);
    return 0;
}

static int Ldiff_move(cg_Diff *D, int r, int c) {
    lcg_Diff *d = (lcg_Diff *)D;
    lcg_writef(d, d->cup_fmt, r + 1, c + 1);
    return 0;
}

static int Ldiff_style(cg_Diff *D, unsigned st) {
    lcg_Diff *d = (lcg_Diff *)D;
    const char *s = lcg_styleof(d->L, d->tblref, st);
    if (s) return lcg_output(d, s, (int)strlen(s));
    return 0;
}

static int Ldiff_fill(cg_Diff *D, int n, int cp) {
    lcg_Diff *d = (lcg_Diff *)D;
    char        buf[8];
    int         len = lcg_cptoutf8(cp, buf);
    if (lcg_output(d, buf, len)) return CG_ERRPARAM;
    if (n > 1) lcg_writef(d, d->rep_fmt, n - 1);
    return 0;
}

static int Ldiff_put(cg_Diff *D, int cp) {
    lcg_Diff *d = (lcg_Diff *)D;
    char        buf[8];
    int         len = lcg_cptoutf8(cp, buf);
    return lcg_output(d, buf, len);
}

static int Ldiff_finish(cg_Diff *D) {
    lcg_Diff *d = (lcg_Diff *)D;
    const char *s = lcg_styleof(d->L, d->tblref, 0U);
    if (s) return lcg_output(d, s, (int)strlen(s));
    return 0;
}

static void lcg_initdiff(
        lcg_Diff *d, lua_State *L, int tblidx, int fd, int rows) {
    memset(d, 0, sizeof(*d));
    d->L = L, d->fd = fd, d->rows = rows;
    lua_pushvalue(L, tblidx);
    d->tblref = luaL_ref(L, LUA_REGISTRYINDEX);
    d->cup_fmt = lcg_optstr(L, tblidx, LCG_KEY_CUP, LCG_DEF_CUP);
    d->csr_fmt = lcg_optstr(L, tblidx, LCG_KEY_CSR, LCG_DEF_CSR);
    d->indn_fmt = lcg_optstr(L, tblidx, LCG_KEY_IND, LCG_DEF_IND);
    d->rin_fmt = lcg_optstr(L, tblidx, LCG_KEY_RIN, LCG_DEF_RIN);
    d->rep_fmt = lcg_optstr(L, tblidx, LCG_KEY_REP, LCG_DEF_REP);
    lua_getfield(L, tblidx, "fill_min");
    d->base.fill_min = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    d->base.scroll = Ldiff_scroll;
    d->base.move = Ldiff_move;
    d->base.style = Ldiff_style;
    d->base.fill = Ldiff_fill;
    d->base.put = Ldiff_put;
    d->base.finish = Ldiff_finish;
}

static int Lgrid_diff(lua_State *L) {
    lcg_Grid  *lg = lcg_check(L, 1);
    lcg_Diff d;
    if (lua_gettop(L) < 2 || lua_isnoneornil(L, 2))
        lua_newtable(L);
    else
        luaL_checktype(L, 2, LUA_TTABLE);
    lcg_initdiff(&d, L, 2, -1, lg->g.rows);
    luaL_buffinit(L, &d.lbuf);
    if (cg_diff(&lg->g, &d.base) != CG_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, d.tblref);
        luaL_error(L, "cellgrid: diff failed");
    }
    luaL_unref(L, LUA_REGISTRYINDEX, d.tblref);
    luaL_pushresult(&d.lbuf);
    return 1;
}

static int Lgrid_render(lua_State *L) {
    lcg_Grid  *lg = lcg_check(L, 1);
    lcg_Diff d;
    int        fd = (int)luaL_checkinteger(L, 2);
    if (lua_gettop(L) < 3 || lua_isnoneornil(L, 3))
        lua_newtable(L);
    else
        luaL_checktype(L, 3, LUA_TTABLE);
    lcg_initdiff(&d, L, 3, fd, lg->g.rows);
    if (cg_diff(&lg->g, &d.base) != CG_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, d.tblref);
        luaL_error(L, "cellgrid: render failed");
    }
    luaL_unref(L, LUA_REGISTRYINDEX, d.tblref);
    return 0;
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
            ENTRY(putline),
            ENTRY(diff),
            ENTRY(render),
            ENTRY(cell),
            ENTRY(back),
            ENTRY(isdirty),
            ENTRY(rows),
            ENTRY(cols),
            ENTRY(top),
            ENTRY(winsize),
#undef ENTRY
            {NULL, NULL}};
    if (luaL_newmetatable(L, LCG_GRID_TYPE)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
    return 1;
}
