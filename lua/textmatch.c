#define LUA_LIB
#include <lauxlib.h>
#include <lua.h>
#include <stddef.h>
#include <string.h>

#if defined(__GNUC__)
# define UD_STATIC static __attribute((unused))
#endif
#include "unidata.h"

#define PT_STATIC_API
#include "piecetab.h"

#define LTM_TABSIZE(t) (sizeof(t) / sizeof((t)[0]))

static int ltm_findrange(range_table *t, size_t size, utfint ch) {
    size_t begin, end;
    begin = 0;
    end = size;
    while (begin < end) {
        size_t mid = (begin + end) / 2;
        if (t[mid].last < ch)
            begin = mid + 1;
        else if (t[mid].first > ch)
            end = mid;
        else
            return (ch - t[mid].first) % t[mid].step == 0;
    }
    return 0;
}

/* clang-format off */
static int ltm_isalpha(utfint ch)
{ return ltm_findrange(alpha_table, LTM_TABSIZE(alpha_table), ch); }
static int ltm_iscntrl(utfint ch)
{ return ltm_findrange(cntrl_table, LTM_TABSIZE(cntrl_table), ch); }
static int ltm_isdigit(utfint ch)
{ return ltm_findrange(digit_table, LTM_TABSIZE(digit_table), ch); }
static int ltm_islower(utfint ch)
{ return ltm_findrange(lower_table, LTM_TABSIZE(lower_table), ch); }
static int ltm_ispunct(utfint ch)
{ return ltm_findrange(punct_table, LTM_TABSIZE(punct_table), ch); }
static int ltm_isspace(utfint ch)
{ return ltm_findrange(space_table, LTM_TABSIZE(space_table), ch); }
static int ltm_iscompose(utfint ch)
{ return ltm_findrange(compose_table, LTM_TABSIZE(compose_table), ch); }
static int ltm_isupper(utfint ch)
{ return ltm_findrange(upper_table, LTM_TABSIZE(upper_table), ch); }
static int ltm_isxdigit(utfint ch)
{ return ltm_findrange(xdigit_table, LTM_TABSIZE(xdigit_table), ch); }
/* clang-format on */

static int ltm_isgraph(utfint ch) {
    if (ltm_findrange(space_table, LTM_TABSIZE(space_table), ch)) return 0;
    if (ltm_findrange(graph_table, LTM_TABSIZE(graph_table), ch)) return 1;
    if (ltm_findrange(compose_table, LTM_TABSIZE(compose_table), ch)) return 1;
    return 0;
}

static int ltm_isalnum(utfint ch) {
    if (ltm_findrange(alpha_table, LTM_TABSIZE(alpha_table), ch)) return 1;
    if (ltm_findrange(alnum_extend_table, LTM_TABSIZE(alnum_extend_table), ch))
        return 1;
    return 0;
}

#define TM_IS(cat, c) ltm_is##cat(c)

#define TM_STATIC_API
#include "textmatch.h"

/* compat layer (ref: lua/piecetab.c) */

#if LUA_VERSION_NUM < 502
# define LUAMOD_API LUALIB_API
# ifndef LUA_GCISRUNNING /* not LuaJIT 2.1 */
#  define luaL_newlib(L, l) (lua_newtable(L), luaL_register(L, NULL, l))
# endif
# define luaL_setfuncs(L, l, n) (assert(n == 0), luaL_register(L, NULL, l))
#endif /* LUA_VERSION_NUM < 502 */

#define LTM_BUFFER_TYPE "piecetab.Buffer"
#define LTM_STATE_TYPE  "textmatch.State"

typedef struct ltm_State {
    tm_State    S;
    tm_Slice    pat;
    size_t      len;
    size_t      endoff;
    int         srcref;
    int         patref;
    const char *s;
    pt_Buffer   b;
    pt_Cursor   C;
} ltm_State;

/* --- source ----------------------------------------------------------- */

static tm_Slice ltm_readstring(void *ud, size_t *poff) {
    ltm_State *it = (ltm_State *)ud;
    if (*poff >= it->len) return tm_slice(NULL, 0);
    return (*poff = 0), tm_slice(it->s, it->len);
}

static int ltm_getpiece(ltm_State *S, size_t t, size_t *po, tm_Slice *out) {
    size_t      len, pre, start;
    const char *s = pt_piece(&S->C, &len);
    if (s == NULL) return 0;
    pre = pt_prefix(&S->C), start = pt_offset(&S->C) - pre;
    if (t < start || t >= start + pre + len) return 0;
    return (*po = start, *out = tm_slice(s - pre, len + pre)), 1;
}

static tm_Slice ltm_readpiece(void *ud, size_t *poff) {
    ltm_State *src = (ltm_State *)ud;
    size_t     target = *poff, poff2;
    tm_Slice   out;
    int        r;
    if (target >= src->len) return tm_slice(NULL, 0);
    if (ltm_getpiece(src, target, &poff2, &out)) return (*poff = poff2), out;
    r = pt_advance(&src->C, (pt_Delta)target - (pt_Delta)pt_offset(&src->C));
    assert(r == PT_OK), (void)r;
    if (ltm_getpiece(src, target, &poff2, &out)) return (*poff = poff2), out;
    return tm_slice(NULL, 0);
}

static tm_Reader *ltm_src(lua_State *L, int idx, ltm_State *it, int retain) {
    pt_Buffer *bp;
    if (lua_type(L, idx) == LUA_TSTRING) {
        it->b = NULL, it->s = lua_tolstring(L, idx, &it->len);
        return ltm_readstring;
    }
    bp = (pt_Buffer *)luaL_checkudata(L, idx, LTM_BUFFER_TYPE);
    if (*bp == NULL) luaL_argerror(L, idx, "invalid Buffer");
    it->s = NULL, it->b = *bp, it->len = pt_bytes(*bp);
    if (retain) pt_retain(*bp);
    return pt_seek(&it->C, (assert(*bp), *bp), 0), ltm_readpiece;
}

static void ltm_attach(lua_State *L, int idx, ltm_State *it) {
    tm_Reader *reader;
    int        oldref = it->srcref;
    pt_Buffer  oldbuf = it->b;
    reader = ltm_src(L, idx, it, 1);
    pt_release(oldbuf);
    lua_pushvalue(L, idx);
    if (oldref != LUA_NOREF)
        lua_rawseti(L, LUA_REGISTRYINDEX, oldref);
    else
        it->srcref = luaL_ref(L, LUA_REGISTRYINDEX);
    tm_reset(&it->S, reader, it), it->endoff = TM_NOLIMIT;
}

static void ltm_attachpattern(lua_State *L, int idx, ltm_State *S) {
    size_t      len;
    const char *pat = luaL_checklstring(L, idx, &len);
    lua_pushvalue(L, idx);
    if (S->patref != LUA_NOREF)
        lua_rawseti(L, LUA_REGISTRYINDEX, S->patref);
    else
        S->patref = luaL_ref(L, LUA_REGISTRYINDEX);
    S->pat = tm_slice(pat, len);
}

/* --- helpers ---------------------------------------------------------- */

static const char *ltm_errmsg(int r) {
    switch (r) {
    case TM_ERRPATTERN: return "malformed pattern";
    case TM_ERRCOMPLEX: return "pattern too complex";
    default: return "textmatch: error";
    }
}

static size_t ltm_initpos(lua_State *L, const ltm_State *it, int argidx) {
    lua_Integer init = luaL_optinteger(L, argidx, 1);
    lua_Integer len = (lua_Integer)it->len;
    if (init > 0) return (size_t)(init - 1);
    if (init == 0) return 0;
    return init + len > 0 ? (size_t)(init + len) : 0;
}

static void ltm_pushstring(lua_State *L, tm_State *S, size_t off, size_t len) {
    luaL_Buffer B;
    size_t      have = 0;
    luaL_buffinit(L, &B);
    while (have < len) {
        size_t got, chunk = LUAL_BUFFERSIZE;
        if (len - have < LUAL_BUFFERSIZE) chunk = len - have;
        got = tm_copy(S, off, luaL_prepbuffer(&B), chunk);
        if (got != chunk) luaL_error(L, "textmatch: substring read failed");
        luaL_addsize(&B, got), have += got, off += got;
    }
    luaL_pushresult(&B);
}

static int ltm_pushcaptures(lua_State *L, tm_State *S, int n) {
    int i;
    for (i = 0; i < n; ++i) {
        tm_Capture c;
        if (tm_capture(S, i, &c) != TM_OK)
            return luaL_error(L, "textmatch: invalid capture");
        if (c.len == TM_CAP_POSITION)
            lua_pushinteger(L, (lua_Integer)c.start + 1);
        else
            ltm_pushstring(L, S, c.start, c.len);
    }
    return n;
}

/* --- state methods ---------------------------------------------------- */

static void ltm_range(lua_State *L, int idx, size_t *from, size_t *end) {
    lua_Integer e, f = luaL_checkinteger(L, idx);
    if (f < 0) luaL_argerror(L, idx, "offset must be non-negative");
    *from = (size_t)f, *end = TM_NOLIMIT;
    if (!lua_isnoneornil(L, idx + 1)) {
        e = luaL_checkinteger(L, idx + 1);
        *end = (e < 0) ? TM_NOLIMIT : (size_t)e;
    }
}

static int ltm_pushpos(lua_State *L, tm_State *S) {
    lua_pushinteger(L, (lua_Integer)tm_offset(S));
    lua_pushinteger(L, (lua_Integer)tm_matchend(S));
    lua_pushinteger(L, tm_captures(S));
    return 3;
}

/* clang-format off */
static int ltm_checkerror(lua_State *L, int r)
{ return r < 0 ? luaL_error(L, "%s", ltm_errmsg(r)) : 0; }

static ltm_State *ltm_checkstate(lua_State *L, int idx)
{ return (ltm_State *)luaL_checkudata(L, idx, LTM_STATE_TYPE); }

static int ltm_valid(ltm_State *it)
{ return it->srcref != LUA_NOREF; }

static int ltm_pushfail(lua_State *L, int r)
{ lua_pushnil(L); return r < 0 ? lua_pushstring(L, ltm_errmsg(r)), 2 : 1; }

static int ltm_pushresult(lua_State *L, int r, tm_State *S)
{ return r <= 0 ? ltm_pushfail(L, r) : ltm_pushpos(L, S); }
/* clang-format on */

static int Lstate_find(lua_State *L) {
    ltm_State  *st = ltm_checkstate(L, 1);
    size_t      len, from, endoff;
    const char *pat = luaL_checklstring(L, 2, &len);
    int         r;
    luaL_argcheck(L, ltm_valid(st), 1, "invalid State");
    ltm_range(L, 3, &from, &endoff), tm_seek(&st->S, from);
    r = tm_find(&st->S, tm_slice(pat, len), endoff);
    return ltm_pushresult(L, r, &st->S);
}

static int Lstate_match(lua_State *L) {
    ltm_State  *st = ltm_checkstate(L, 1);
    size_t      len;
    const char *pat = luaL_checklstring(L, 2, &len);
    lua_Integer off = luaL_checkinteger(L, 3);
    luaL_argcheck(L, ltm_valid(st), 1, "invalid State");
    luaL_argcheck(L, off >= 0, 3, "offset must be non-negative");
    tm_seek(&st->S, (size_t)off);
    return ltm_pushresult(L, tm_match(&st->S, tm_slice(pat, len)), &st->S);
}

static int Lstate_capture(lua_State *L) {
    ltm_State  *st = ltm_checkstate(L, 1);
    int         n = tm_captures(&st->S);
    lua_Integer i = luaL_optinteger(L, 2, -1);
    tm_Capture  c;
    size_t      end;
    luaL_argcheck(L, ltm_valid(st), 1, "invalid State");
    if (lua_isnoneornil(L, 2)) return lua_pushinteger(L, n), 1;
    if (i < 0 || i >= n || tm_capture(&st->S, (int)i, &c) != TM_OK)
        return lua_pushnil(L), 1;
    end = (c.len == TM_CAP_POSITION) ? c.start : c.start + c.len;
    lua_pushinteger(L, (lua_Integer)c.start);
    lua_pushinteger(L, (lua_Integer)end);
    return 2;
}

static void ltm_reset(ltm_State *it) {
    memset(it, 0, sizeof(*it));
    it->srcref = it->patref = LUA_NOREF, it->endoff = TM_NOLIMIT;
}

/* clang-format off */
static int Lstate_reset(lua_State *L)
{ ltm_attach(L, 2, ltm_checkstate(L, 1)); return lua_settop(L, 1), 1; }
/* clang-format on */

static int Lstate_delete(lua_State *L) {
    ltm_State *it = lua_touserdata(L, 1);
    if (!it || it->srcref == LUA_NOREF) return 0;
    if (it->b) pt_release(it->b), it->b = NULL;
    if (it->srcref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, it->srcref);
    if (it->patref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, it->patref);
    return ltm_reset(it), 0;
}

static int Lstate_option(lua_State *L) {
    static const char *const opts[] = {"plain", "lineanchor", NULL};

    ltm_State *st = ltm_checkstate(L, 1);
    int        idx = luaL_checkoption(L, 2, NULL, opts);
    int        flag = idx == 0 ? TM_LITERAL : TM_LINEANCHOR;
    luaL_argcheck(L, ltm_valid(st), 1, "invalid State");
    if (lua_isnoneornil(L, 3))
        return lua_pushboolean(L, (tm_flags(&st->S) & flag) != 0), 1;
    if (lua_toboolean(L, 3))
        tm_setflags(&st->S, tm_flags(&st->S) | flag);
    else
        tm_setflags(&st->S, tm_flags(&st->S) & ~flag);
    return lua_settop(L, 1), 1;
}

static int Lstate_gfind(lua_State *L);

static ltm_State *ltm_new(lua_State *L) {
    ltm_State *it = (ltm_State *)lua_newuserdata(L, sizeof(ltm_State));
    ltm_reset(it);
    if (luaL_newmetatable(L, LTM_STATE_TYPE)) {
        static const luaL_Reg methods[] = {
                {"__gc", NULL}, {"__close", NULL}, {"__index", NULL},
#define ENTRY(name) {#name, Lstate_##name}
                ENTRY(find),    ENTRY(match),      ENTRY(gfind),
                ENTRY(capture), ENTRY(reset),      ENTRY(option),
                ENTRY(delete),
#undef ENTRY
                {NULL, NULL}};
        luaL_setfuncs(L, methods, 0);
        lua_pushcfunction(L, Lstate_delete), lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, Lstate_delete), lua_setfield(L, -2, "__close");
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
    return lua_setmetatable(L, -2), it;
}

static int Lstate_gfinditer(lua_State *L) {
    ltm_State *it = (ltm_State *)lua_touserdata(L, lua_upvalueindex(1));
    size_t     start, end;
    int        r;
    assert(it->srcref != LUA_NOREF && it->patref != LUA_NOREF);
    ltm_checkerror(L, r = tm_find(&it->S, it->pat, it->endoff));
    if (r == TM_OK) return 0;
    lua_pushinteger(L, (lua_Integer)(start = tm_offset(&it->S)));
    lua_pushinteger(L, (lua_Integer)(end = tm_matchend(&it->S)));
    lua_pushinteger(L, tm_captures(&it->S));
    tm_seek(&it->S, end + (end == start));
    return 3;
}

static int Lstate_gfind(lua_State *L) {
    ltm_State *src = ltm_checkstate(L, 1);
    ltm_State *it;
    size_t     from, endoff;
    luaL_argcheck(L, ltm_valid(src), 1, "invalid State");
    ltm_range(L, 3, &from, &endoff);
    it = ltm_new(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, src->srcref);
    ltm_attach(L, -1, it);
    lua_pop(L, 1);
    ltm_attachpattern(L, 2, it);
    tm_setflags(&it->S, tm_flags(&src->S));
    tm_seek(&it->S, from);
    it->endoff = endoff;
    lua_pushvalue(L, -1), lua_pushcclosure(L, Lstate_gfinditer, 1);
    lua_pushnil(L), lua_pushnil(L), lua_pushvalue(L, -4);
    return 4;
}

/* --- public Lua-compatible API ---------------------------------------- */

static int Ltm_new(lua_State *L) { return ltm_attach(L, 1, ltm_new(L)), 1; }

static int ltm_pushmatch(lua_State *L, tm_State *S) {
    int n = tm_captures(S);
    if (n > 0) return ltm_pushcaptures(L, S, n);
    return ltm_pushstring(L, S, tm_offset(S), tm_matchend(S) - tm_offset(S)), 1;
}

static int ltm_genfind(lua_State *L, int find) {
    ltm_State   st;
    size_t      len;
    int         r, plain = find ? lua_toboolean(L, 4) : 0;
    tm_Reader  *reader = (ltm_reset(&st), ltm_src(L, 1, &st, 0));
    const char *pat = luaL_checklstring(L, 2, &len);
    tm_reset(&st.S, reader, &st);
    if (plain) tm_setflags(&st.S, TM_LITERAL);
    tm_seek(&st.S, ltm_initpos(L, &st, 3));
    r = tm_find(&st.S, tm_slice(pat, len), TM_NOLIMIT);
    if (r <= 0) return ltm_pushfail(L, r);
    if (!find) return ltm_pushmatch(L, &st.S);
    lua_pushinteger(L, (lua_Integer)tm_offset(&st.S) + 1);
    lua_pushinteger(L, (lua_Integer)tm_matchend(&st.S));
    return 2 + ltm_pushcaptures(L, &st.S, tm_captures(&st.S));
}

static int Ltm_find(lua_State *L) { return ltm_genfind(L, 1); }
static int Ltm_match(lua_State *L) { return ltm_genfind(L, 0); }

static int Ltm_gmatchiter(lua_State *L) {
    ltm_State *it = (ltm_State *)lua_touserdata(L, lua_upvalueindex(1));
    size_t     start, end;
    int        n, r;
    ltm_checkerror(L, r = tm_find(&it->S, it->pat, it->endoff));
    if (r == TM_OK) return 0;
    n = ltm_pushmatch(L, &it->S);
    start = tm_offset(&it->S), end = tm_matchend(&it->S);
    return tm_seek(&it->S, end + (end == start)), n;
}

static int Ltm_gmatch(lua_State *L) {
    ltm_State  *it;
    size_t      len;
    const char *pat = luaL_checklstring(L, 2, &len);
    if (len > 0 && pat[0] == '^') {
        lua_pushliteral(L, "%"), lua_pushvalue(L, 2), lua_concat(L, 2);
        lua_replace(L, 2), pat = lua_tolstring(L, 2, &len);
    }
    it = ltm_new(L), ltm_attach(L, 1, it), ltm_attachpattern(L, 2, it);
    lua_pushvalue(L, -1), lua_pushcclosure(L, Ltm_gmatchiter, 1);
    lua_pushnil(L), lua_pushnil(L), lua_pushvalue(L, -4);
    return 4;
}

LUAMOD_API int luaopen_textmatch(lua_State *L) {
    const luaL_Reg ltm_funcs[] = {
#define ENTRY(name) {#name, Ltm_##name}
            ENTRY(new),
            ENTRY(find),
            ENTRY(match),
            ENTRY(gmatch),
#undef ENTRY
            {NULL, NULL}};
    return luaL_newlib(L, ltm_funcs), 1;
}
