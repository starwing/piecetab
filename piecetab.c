#ifdef _MSC_VER
#define _CRT_SECURE_NO_DEPRECATE 1
#define _CRT_SECURE_NO_WARNINGS  1
#endif

#define LUA_LIB
#include <assert.h>
#include <lauxlib.h>
#include <lua.h>

#define PT_STATIC_API
#include "piecetab.h"
#define LC_STATIC_API
#include "linecache.h"

/* compat layer (ref: lua-protobuf pb.c) */

/* PT_STATIC while unused outside 5.1; switch to static once used */
PT_STATIC int lpt_relindex(int idx, int offset) {
    return idx < 0 && idx > LUA_REGISTRYINDEX ? idx - offset : idx;
}

#if LUA_VERSION_NUM < 502
#define LUAMOD_API                LUALIB_API
# define LUA_OK                   0
# define lua_rawlen               lua_objlen
# define lua_setuservalue(L, idx) lua_setfenv(L, idx)
# define lua_getuservalue(L, idx) lua_getfenv(L, idx)
# define luaL_setfuncs(L, l, n)   (assert(n == 0), luaL_register(L, NULL, l))
# define luaL_setmetatable(L, name) \
    (luaL_getmetatable((L), (name)), lua_setmetatable(L, -2))

static void lua_rawgetp(lua_State *L, int idx, const void *p) {
    lua_pushlightuserdata(L, (void *)p);
    lua_rawget(L, lpt_relindex(idx, 1));
}

static void lua_rawsetp(lua_State *L, int idx, const void *p) {
    lua_pushlightuserdata(L, (void *)p);
    lua_insert(L, -2);
    lua_rawset(L, lpt_relindex(idx, 1));
}

# ifndef LUA_GCISRUNNING /* not LuaJIT 2.1 */
#  define luaL_newlib(L, l) (lua_newtable(L), luaL_register(L, NULL, l))
# endif
#endif /* LUA_VERSION_NUM < 502 */

#if LUA_VERSION_NUM >= 503
#define lua53_rawgetp lua_rawgetp
#else  /* not Lua 5.3 */
static int lua53_rawgetp(lua_State *L, int idx, const void *p) {
    return lua_rawgetp(L, idx, p), lua_type(L, -1);
}
#endif /* LUA_VERSION_NUM >= 503 */

#define LPT_VERSION    "0.1.0"
#define LPT_STATE_KEY  ((void *)0x91ECE7AB)
#define LPT_STATE_TYPE "piecetab.State"

#define lpt_checkmem(L, p) ((p) || luaL_error(L, "piecetab: out of memory"))

/* global state */

typedef struct lpt_State {
    pt_State *PS;
    lc_State *LS;
} lpt_State;

static int Lstate_gc(lua_State *L) {
    lpt_State *S = (lpt_State *)lua_touserdata(L, 1);
    if (S->PS) pt_close(S->PS), S->PS = NULL;
    if (S->LS) lc_close(S->LS), S->LS = NULL;
    return 0;
}

static lpt_State *lpt_state(lua_State *L) {
    lpt_State *S;
    void      *ud;
    lua_Alloc  f;
    if (lua53_rawgetp(L, LUA_REGISTRYINDEX, LPT_STATE_KEY) != LUA_TNIL) {
        S = (lpt_State *)lua_touserdata(L, -1);
        return lua_pop(L, 1), S;
    }
    lua_pop(L, 1);
    S = (lpt_State *)lua_newuserdata(L, sizeof(lpt_State));
    S->PS = NULL, S->LS = NULL;
    f = lua_getallocf(L, &ud);
    S->PS = pt_open((pt_Alloc *)f, ud);
    S->LS = lc_open((lc_Alloc *)f, ud);
    if (S->PS == NULL || S->LS == NULL) {
        pt_close(S->PS), lc_close(S->LS);
        luaL_error(L, "piecetab: out of memory");
    }
    if (luaL_newmetatable(L, LPT_STATE_TYPE))
        lua_pushcfunction(L, Lstate_gc), lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    return lua_rawsetp(L, LUA_REGISTRYINDEX, LPT_STATE_KEY), S;
}

/* buffer + cursor binding */

#define LPT_BUFFER_TYPE "piecetab.Buffer"
#define LPT_CURSOR_TYPE "piecetab.Cursor"

static void lpt_checkerror(lua_State *L, int err) {
    switch (err) {
        case PT_OK:
            return;
        case PT_ERRPARAM:
            luaL_error(L, "piecetab: invalid parameter");
            break;
        case PT_ERRMEM:
            luaL_error(L, "piecetab: out of memory");
            break;
        default:
            luaL_error(L, "piecetab: unknown error(%d)", err);
    }
}

static pt_Buffer lpt_checkbuffer(lua_State *L, int idx) {
    pt_Buffer *b = (pt_Buffer *)luaL_checkudata(L, idx, LPT_BUFFER_TYPE);
    return luaL_argcheck(L, *b != NULL, idx, "invalid Buffer"), *b;
}

static pt_Cursor *lpt_checkcursor(lua_State *L, int idx) {
    pt_Cursor *C = (pt_Cursor *)luaL_checkudata(L, idx, LPT_CURSOR_TYPE);
    return luaL_argcheck(L, pt_valid(C), idx, "invalid Cursor"), C;
}

/* copy s into tree-owned arena, return literal pointer (never NULL) */
static const char *lpt_toliteral(lua_State *L, int i, size_t *l, pt_Cursor *C) {
    const char *lit, *s = luaL_checklstring(L, i, l);
    char       *p;
    if (*l == 0) return NULL;
    lpt_checkmem(L, p = pt_reserve(C, *l)), memcpy(p, s, *l);
    return lpt_checkmem(L, lit = pt_literal(C, *l)), lit;
}

/* read len bytes from C into a new Lua string (clamped by pt_read) */
static int lpt_readstring(lua_State *L, pt_Cursor *C, size_t len) {
    luaL_Buffer B;
    size_t      chunk, got;
    luaL_buffinit(L, &B);
    for (; len > 0; len -= got) {
        chunk = len < LUAL_BUFFERSIZE ? len : LUAL_BUFFERSIZE;
        got = pt_read(C, luaL_prepbuffer(&B), chunk);
        luaL_addsize(&B, got);
        if (got < chunk) break;
    }
    return luaL_pushresult(&B), 1;
}

/* push a new Buffer userdata with b == NULL, metatable set */
static pt_Buffer *lpt_newbuffer(lua_State *L) {
    pt_Buffer *pb = (pt_Buffer *)lua_newuserdata(L, sizeof(pt_Buffer *));
    luaL_setmetatable(L, LPT_BUFFER_TYPE);
    return *pb = NULL, pb;
}

/* buffer methods */

/* clang-format off */
static int Lbuf_len(lua_State *L)
{ return lua_pushinteger(L, (lua_Integer)pt_bytes(lpt_checkbuffer(L, 1))), 1; }

static int Lpt_empty(lua_State *L)
{ return *lpt_newbuffer(L) = pt_empty(lpt_state(L)->PS), 1; }
/* clang-format on */

static int Lbuf_gc(lua_State *L) {
    pt_Buffer *pb = (pt_Buffer *)lua_touserdata(L, 1);
    if (pb && *pb) pt_release(*pb), *pb = NULL;
    return 0;
}

static int Lbuf_read(lua_State *L) {
    pt_Buffer b = lpt_checkbuffer(L, 1);
    size_t    off = (size_t)luaL_checkinteger(L, 2);
    size_t    total = pt_bytes(b), n;
    pt_Cursor C;
    luaL_argcheck(L, (lua_Integer)off >= 0, 2, "offset must be non-negative");
    if (off >= total) return lua_pushliteral(L, ""), 1;
    n = (size_t)luaL_optinteger(L, 3, (lua_Integer)(total - off));
    luaL_argcheck(L, (lua_Integer)n >= 0, 3, "length must be non-negative");
    return pt_seek(&C, b, off), lpt_readstring(L, &C, n);
}

static int Lbuf_pieceiter(lua_State *L) {
    pt_Cursor  *C = (pt_Cursor *)lua_touserdata(L, lua_upvalueindex(1));
    size_t      len;
    const char *s;
    luaL_argcheck(L, C != NULL, 1, "piecetab: invalid Cursor");
    if ((s = pt_piece(C, &len)) == NULL) return 0;
    lua_pushinteger(L, (lua_Integer)pt_offset(C));
    lua_pushinteger(L, (lua_Integer)len);
    lua_pushlstring(L, s, len);
    pt_next(C, &len);
    return 3;
}

static int Lbuf_pieces(lua_State *L) {
    pt_Buffer  b = lpt_checkbuffer(L, 1);
    pt_Cursor *C = (pt_Cursor *)lua_newuserdata(L, sizeof(pt_Cursor));
    pt_seek(C, b, 0);
    lua_pushvalue(L, 1);
    return lua_pushcclosure(L, Lbuf_pieceiter, 2), 1;
}

static int Lbuf_compact(lua_State *L) {
    lpt_State *S = lpt_state(L);
    pt_Buffer  b = lpt_checkbuffer(L, 1), *out = lpt_newbuffer(L);
    return lpt_checkmem(L, *out = pt_compact(S->PS, b)), 1;
}

static int Lbuf_cursor(lua_State *L) {
    pt_Buffer  b = lpt_checkbuffer(L, 1);
    size_t     off = (size_t)luaL_optinteger(L, 2, 0);
    pt_Cursor *c = (pt_Cursor *)lua_newuserdata(L, sizeof(pt_Cursor));
    pt_seek(c, b, off);
    luaL_setmetatable(L, LPT_CURSOR_TYPE);
    lua_createtable(L, 1, 0);
    lua_pushvalue(L, 1), lua_rawseti(L, -2, 1);
    return lua_setuservalue(L, -2), 1;
}

static void lpt_openbuffer(lua_State *L) {
    static const luaL_Reg buf_methods[] = {
            {"__gc", Lbuf_gc}, {"__close", Lbuf_gc}, {"__len", Lbuf_len},
#define ENTRY(name) {#name, Lbuf_##name}
            ENTRY(len),        ENTRY(read),          ENTRY(pieces),
            ENTRY(compact),    ENTRY(cursor),
#undef ENTRY
            {NULL, NULL}};
    if (luaL_newmetatable(L, LPT_BUFFER_TYPE)) {
        luaL_setfuncs(L, buf_methods, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

/* cursor methods */

static int Lcur_gc(lua_State *L) {
    pt_Cursor *C = (pt_Cursor *)lua_touserdata(L, 1);
    if (pt_valid(C)) pt_release(pt_rollback(C));
    return 0;
}

static int Lcur_offset(lua_State *L) {
    pt_Cursor *C = lpt_checkcursor(L, 1);
    return lua_pushinteger(L, (lua_Integer)pt_offset(C)), 1;
}

static int Lcur_locate(lua_State *L) {
    pt_Cursor *C = lpt_checkcursor(L, 1);
    size_t     off = (size_t)luaL_checkinteger(L, 2);
    luaL_argcheck(L, (lua_Integer)off >= 0, 2, "offset must be non-negative");
    pt_locate(C, off);
    return lua_settop(L, 1), 1;
}

static int Lcur_advance(lua_State *L) {
    pt_Cursor  *C = lpt_checkcursor(L, 1);
    lua_Integer d = luaL_checkinteger(L, 2);
    pt_advance(C, (pt_Delta)d);
    return lua_settop(L, 1), 1;
}

static int Lcur_read(lua_State *L) {
    pt_Cursor *C = lpt_checkcursor(L, 1);
    size_t     len = (size_t)luaL_checkinteger(L, 2);
    luaL_argcheck(L, (lua_Integer)len >= 0, 2, "length must be non-negative");
    return lpt_readstring(L, C, len);
}

static int Lcur_append(lua_State *L) {
    pt_Cursor  *C = lpt_checkcursor(L, 1);
    size_t      len;
    const char *s = lpt_toliteral(L, 2, &len, C);
    if (len == 0) return lua_settop(L, 1), 1;
    lpt_checkerror(L, pt_append(C, s, len));
    return lua_settop(L, 1), 1;
}

static int Lcur_insert(lua_State *L) {
    pt_Cursor  *C = lpt_checkcursor(L, 1);
    size_t      len;
    const char *s = lpt_toliteral(L, 2, &len, C);
    if (len == 0) return lua_settop(L, 1), 1;
    lpt_checkerror(L, pt_insert(C, s, len));
    return lua_settop(L, 1), 1;
}

static int Lcur_edit(lua_State *L) {
    pt_Cursor  *C = lpt_checkcursor(L, 1);
    size_t      del = (size_t)luaL_checkinteger(L, 2);
    size_t      len;
    const char *s = luaL_checklstring(L, 3, &len);
    luaL_argcheck(L, len <= PT_MAX_HOLESIZE, 3, "edit too long (use splice)");
    lpt_checkerror(L, pt_edit(C, del, s, len));
    return lua_settop(L, 1), 1;
}

static int Lcur_remove(lua_State *L) {
    pt_Cursor *C = lpt_checkcursor(L, 1);
    size_t     len = (size_t)luaL_checkinteger(L, 2);
    lpt_checkerror(L, pt_remove(C, len));
    return lua_settop(L, 1), 1;
}

static int Lcur_splice(lua_State *L) {
    pt_Cursor  *C = lpt_checkcursor(L, 1);
    size_t      del = (size_t)luaL_checkinteger(L, 2);
    size_t      len;
    const char *s = lpt_toliteral(L, 3, &len, C);
    lpt_checkerror(L, pt_splice(C, del, s, len));
    return lua_settop(L, 1), 1;
}

static int Lcur_commit(lua_State *L) {
    pt_Cursor *C = lpt_checkcursor(L, 1);
    pt_Buffer *b = lpt_newbuffer(L);
    return lpt_checkmem(L, *b = pt_commit(C)), 1;
}

static int Lcur_rollback(lua_State *L) {
    pt_Cursor *C = lpt_checkcursor(L, 1);
    pt_Buffer *b = lpt_newbuffer(L);
    return lpt_checkmem(L, *b = pt_rollback(C)), 1;
}

static void lpt_opencursor(lua_State *L) {
    static const luaL_Reg libs[] = {
            {"__gc", Lcur_gc}, {"__close", Lcur_gc},
#define ENTRY(name) {#name, Lcur_##name}
            ENTRY(offset),     ENTRY(locate),        ENTRY(advance),
            ENTRY(read),       ENTRY(append),        ENTRY(insert),
            ENTRY(edit),       ENTRY(splice),        ENTRY(remove),
            ENTRY(commit),     ENTRY(rollback),
#undef ENTRY
            {NULL, NULL}};
    if (luaL_newmetatable(L, LPT_CURSOR_TYPE)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

/* library functions */

static int Lpt_from(lua_State *L) {
    lpt_State  *S = lpt_state(L);
    size_t      len;
    const char *s;
    pt_Cursor   C;
    int         r = PT_OK;
    r = pt_seek(&C, pt_empty(S->PS), 0), assert(r == PT_OK), (void)r;
    lpt_checkmem(L, s = lpt_toliteral(L, 1, &len, &C));
    if (len == 0) return *lpt_newbuffer(L) = pt_empty(S->PS), 1;
    if ((r = pt_insert(&C, s, len)) != PT_OK
        || !(*lpt_newbuffer(L) = pt_commit(&C)))
        pt_release(pt_rollback(&C)), lpt_checkerror(L, r ? r : PT_ERRMEM);
    return 1;
}

/* metatable registration */

LUAMOD_API int luaopen_piecetab(lua_State *L) {
    luaL_Reg libs[] = {
            {"version", NULL},
            {"MAX_HOLESIZE", NULL},
#define ENTRY(name) {#name, Lpt_##name}
            ENTRY(from),
            ENTRY(empty),
#undef ENTRY
            {NULL, NULL}};
    lpt_state(L), lpt_openbuffer(L), lpt_opencursor(L);
    luaL_newlib(L, libs);
    lua_pushliteral(L, LPT_VERSION), lua_setfield(L, -2, "version");
    lua_pushinteger(L, PT_MAX_HOLESIZE), lua_setfield(L, -2, "MAX_HOLESIZE");
    return 1;
}
