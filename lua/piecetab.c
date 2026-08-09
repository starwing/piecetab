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
#define UT_STATIC_API
#include "undotree.h"

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

/* forward: pt_Buffer payload cleaner for undotree */
static void lpt_ut_cleaner(void *ud, ut_Payload *p);

/* global state */

typedef struct lpt_State {
    pt_State *PS;
    lc_State *LS;
    ut_State *US;
} lpt_State;

static int Lstate_gc(lua_State *L) {
    lpt_State *S = (lpt_State *)lua_touserdata(L, 1);
    if (S->PS) pt_close(S->PS), S->PS = NULL;
    if (S->LS) lc_close(S->LS), S->LS = NULL;
    if (S->US) ut_close(S->US), S->US = NULL;
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
    S->PS = NULL, S->LS = NULL, S->US = NULL;
    f = lua_getallocf(L, &ud);
    S->PS = pt_open((pt_Alloc *)f, ud);
    S->LS = lc_open((lc_Alloc *)f, ud);
    S->US = ut_open((ut_Alloc *)f, ud);
    if (luaL_newmetatable(L, LPT_STATE_TYPE))
        lua_pushcfunction(L, Lstate_gc), lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    if (!S->PS || !S->LS || !S->US) luaL_error(L, "piecetab: out of memory");
    ut_setcleaner(S->US, lpt_ut_cleaner, NULL);
    return lua_rawsetp(L, LUA_REGISTRYINDEX, LPT_STATE_KEY), S;
}

/* buffer + cursor binding */

#define LPT_BUFFER_TYPE "piecetab.Buffer"
#define LPT_CURSOR_TYPE "piecetab.Cursor"
#define LPT_DOC_TYPE    "piecetab.Doc"

/* doc userdata */
typedef struct lpt_Doc {
    pt_Cursor C;     /* long-term dirty cursor; sole text entry */
    ut_Tree  *ut;    /* undotree version tree */
    lc_Cache *lc;    /* linecache; doc owns it, never destroyed */
    ut_Vid    lcvid; /* lc aligned to this commit node, NULL=empty */
    int       lck;   /* journal entries consumed by lc */
} lpt_Doc;

/* clang-format off */
static lpt_Doc *lpt_checkdoc(lua_State *L, int idx)
{ return (lpt_Doc *)luaL_checkudata(L, idx, LPT_DOC_TYPE); }
/* clang-format on */

static int lpt_checkerror(lua_State *L, int err) {
    assert(err != PT_ERRPARAM);
    switch (err) {
    case PT_OK: return 0;
    case PT_ERRPARAM: return luaL_error(L, "piecetab: invalid parameter");
    case PT_ERRMEM: return luaL_error(L, "piecetab: out of memory");
    default: return luaL_error(L, "piecetab: unknown error(%d)", err);
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
    if (*l == 0) return "";
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

static int Lbuf_delete(lua_State *L) {
    pt_Buffer *pb = (pt_Buffer *)lua_touserdata(L, 1);
    if (pb && *pb) pt_release(*pb), *pb = NULL;
    return 0;
}

static int Lbuf_read(lua_State *L) {
    pt_Buffer   b = lpt_checkbuffer(L, 1);
    lua_Integer cnt, off = luaL_checkinteger(L, 2);
    size_t      o, n, total = pt_bytes(b);
    pt_Cursor   C;
    luaL_argcheck(L, off >= 0, 2, "offset must be non-negative");
    if ((o = (size_t)off) >= total) return lua_pushliteral(L, ""), 1;
    cnt = luaL_optinteger(L, 3, (lua_Integer)(total - o));
    luaL_argcheck(L, cnt >= 0, 3, "length must be non-negative");
    return n = (size_t)cnt, pt_seek(&C, b, off), lpt_readstring(L, &C, n);
}

static int Lbuf_pieceiter(lua_State *L) {
    pt_Cursor  *C = (pt_Cursor *)lua_touserdata(L, lua_upvalueindex(1));
    size_t      len;
    const char *s;
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

static int Lbuf_dump(lua_State *L) {
    pt_Buffer b = lpt_checkbuffer(L, 1);
    size_t    total = pt_bytes(b);
    pt_Cursor C;
    if (total == 0) return lua_pushliteral(L, ""), 1;
    return pt_seek(&C, b, 0), lpt_readstring(L, &C, total);
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
            {"__gc", Lbuf_delete}, {"__close", Lbuf_delete},
            {"__len", Lbuf_len},
#define ENTRY(name) {#name, Lbuf_##name}
            ENTRY(delete),         ENTRY(len),
            ENTRY(read),           ENTRY(pieces),
            ENTRY(compact),        ENTRY(dump),
            ENTRY(cursor),
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
    pt_Cursor  *C = lpt_checkcursor(L, 1);
    lua_Integer len = luaL_checkinteger(L, 2);
    luaL_argcheck(L, len >= 0, 2, "length must be non-negative");
    return lpt_readstring(L, C, (size_t)len);
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

/* doc */

#define LPT_UNL ((size_t)-1)

typedef struct lpt_ScanCtx {
    pt_Cursor C;
    size_t    end;      /* byte limit, LPT_UNL = unlimited */
    size_t    max_line; /* line limit, LPT_UNL = unlimited */
    size_t    cur_line; /* lines produced */
} lpt_ScanCtx;

/* clang-format off */
static void lpt_ut_cleaner(void *ud, ut_Payload *p)
{ (void)ud, pt_release((pt_Buffer)p); }

static int lpt_pushvid(lua_State *L, ut_Vid v)
{ return lua_pushinteger(L, pt_version((pt_Buffer)ut_payload(v))), 1; }
/* clang-format on */

static ut_Vid lpt_checkvid(lua_State *L, int idx, int didx, ut_Vid def) {
    lua_Integer num = luaL_optinteger(L, idx, 0);
    ut_Vid      v;
    if (num == 0) return def;
    lua_getuservalue(L, didx);
    lua_pushvalue(L, lpt_relindex(idx, 1)), lua_gettable(L, -2);
    v = (ut_Vid)lua_touserdata(L, -1);
    lua_pop(L, 2);
    return luaL_argcheck(L, v, idx, "piecetab: invalid vid"), v;
}

static void lpt_setvid(lua_State *L, int didx, lua_Integer vid, ut_Vid node) {
    lua_getuservalue(L, didx);
    lua_pushinteger(L, vid);
    lua_pushlightuserdata(L, (void *)node);
    lua_settable(L, -3);
    lua_pop(L, 1);
}

static unsigned lpt_scanline(void *ud, size_t pos) {
    lpt_ScanCtx *ctx = (lpt_ScanCtx *)ud;
    unsigned     total = 0;
    size_t       i, len, n;
    const char  *data = pt_piece(&ctx->C, &len);
    if (ctx->cur_line >= ctx->max_line) return 0;
    for (; data && pos < ctx->end; data = pt_next(&ctx->C, &len), pos += n) {
        if (pos + len > ctx->end) len = ctx->end - pos;
        for (i = 0; i < len && data[i] != '\n'; i++) continue;
        n = i + (i < len), total += (unsigned)n;
        if (i < len) return ctx->cur_line++, pt_advance(&ctx->C, n), total;
    }
    return 0;
}

static int lpt_hunkapply(lc_Cache *lc, const ut_Hunk *hs, int n, pt_Buffer b) {
    int         i, r = LC_OK;
    lpt_ScanCtx ctx;
    unsigned    ins;
    lc_Cursor   C;
    for (i = n - 1; i >= 0; --i) {
        if (hs[i].pa >= lc_bytes(lc)) continue;
        lc_seek(&C, lc, hs[i].pa);
        if ((r = lc_splice(&C, hs[i].pdel, 0)) != LC_OK) break;
        pt_seek(&ctx.C, b, hs[i].ca);
        ctx.end = hs[i].ca + hs[i].cins, ctx.max_line = LPT_UNL,
        ctx.cur_line = 0;
        if ((r = lc_append(&C, 0, lpt_scanline, &ctx)) != LC_OK) break;
        ins = (unsigned)(ctx.cur_line ? (ctx.end - pt_offset(&ctx.C))
                                      : (pt_offset(&ctx.C) - hs[i].ca));
        if ((r = lc_splice(&C, 0, ins)) != LC_OK) break;
    }
    if (r < 0 && n > 0 && hs[0].pa < lc_bytes(lc)) {
        lc_Cursor C1, C2;
        lc_seek(&C1, lc, hs[0].pa), lc_seek(&C2, lc, lc_bytes(lc));
        lc_remove(&C1, &C2);
    }
    return r;
}

static int lpt_docsync(lpt_Doc *d, size_t tol, size_t tob) {
    pt_Buffer   b = pt_buffer(&d->C);
    ut_Vid      cur = ut_current(d->ut);
    lpt_ScanCtx ctx;
    int         r;
    if ((r = ut_diff(d->ut, d->lcvid, cur)) < 0) return r;
    if ((r = lpt_hunkapply(d->lc, ut_hunks(d->ut, NULL), r, b)) < 0) return r;
    d->lcvid = cur;
    if (r > 0) d->lck = 0;
    if ((r = ut_freshdiff(d->ut, d->lck, ut_freshcount(d->ut))) < 0) return r;
    if ((r = lpt_hunkapply(d->lc, ut_hunks(d->ut, NULL), r, b)) < 0) return r;
    d->lck = ut_freshcount(d->ut);
    ctx.cur_line = lc_breaks(d->lc);
    ctx.max_line = tol == LPT_UNL ? tol : tol + 1, ctx.end = tob;
    pt_seek(&ctx.C, b, lc_bytes(d->lc));
    return lc_scan(d->lc, lpt_scanline, &ctx);
}

static int Ldoc_gc(lua_State *L) {
    lpt_Doc   *d = (lpt_Doc *)lua_touserdata(L, 1);
    lpt_State *S = lpt_state(L);
    if (!d || !d->ut) return 0;
    pt_release(pt_rollback(&d->C));
    ut_deltree(S->US, d->ut), d->ut = NULL;
    if (d->lc) lc_delcache(S->LS, d->lc), d->lc = NULL;
    return 0;
}

static int Ldoc_len(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    return lua_pushinteger(L, (lua_Integer)pt_bytes(pt_buffer(&d->C))), 1;
}

static pt_Buffer lpt_tobuffer(lua_State *L, int idx) {
    lpt_State  *S = lpt_state(L);
    size_t      len;
    pt_Cursor   C;
    pt_Buffer   b = NULL;
    const char *lit;
    int         r;
    luaL_checklstring(L, idx, &len);
    if (len == 0) return pt_empty(S->PS);
    pt_seek(&C, pt_empty(S->PS), 0);
    lpt_checkmem(L, lit = lpt_toliteral(L, idx, &len, &C));
    if ((r = pt_insert(&C, lit, len)) < 0 || !(b = pt_commit(&C)))
        pt_release(pt_rollback(&C)), lpt_checkerror(L, r ? r : PT_ERRMEM);
    return b;
}

static lpt_Doc *lpt_newdoc(lua_State *L, pt_Buffer b) {
    lpt_State *S = lpt_state(L);
    lpt_Doc   *d = (lpt_Doc *)lua_newuserdata(L, sizeof(lpt_Doc));
    memset(d, 0, sizeof(lpt_Doc));
    luaL_setmetatable(L, LPT_DOC_TYPE);
    d->ut = ut_newtree(S->US, (ut_Payload *)b);
    if (!d->ut) luaL_error(L, "piecetab: out of memory");
    d->lc = lc_newcache(S->LS);
    if (!d->lc) luaL_error(L, "piecetab: out of memory");
    pt_seek(&d->C, b, 0), d->lck = 0, d->lcvid = ut_root(d->ut);
    lua_createtable(L, 0, 0);
    lua_pushinteger(L, (lua_Integer)pt_version(b));
    lua_pushlightuserdata(L, (void *)ut_root(d->ut));
    lua_settable(L, -3);
    return lua_setuservalue(L, -2), d;
}

static int Lpt_doc(lua_State *L) {
    lpt_State *S = lpt_state(L);
    int        argc = lua_gettop(L);
    pt_Buffer  b = NULL;
    if (argc < 1 || lua_isnil(L, 1))
        b = pt_empty(S->PS);
    else if (lua_isuserdata(L, 1))
        b = lpt_checkbuffer(L, 1), pt_retain(b);
    else if (lua_isstring(L, 1))
        b = lpt_tobuffer(L, 1);
    else
        luaL_argerror(L, 1, "expected nil, Buffer, or string");
    return lpt_newdoc(L, b), 1;
}

static int lpt_seekpos(lua_State *L, lpt_Doc *d, const char *whence) {
    lua_Integer off = luaL_optinteger(L, 3, 0);
    if (whence[0] == 's' && whence[1] == 'e')
        pt_locate(&d->C, (size_t)off);
    else if (whence[0] == 'c')
        pt_advance(&d->C, (pt_Delta)off);
    else if (whence[0] == 'e') {
        size_t n = pt_bytes(pt_buffer(&d->C));
        if (off < 0) n -= (size_t)(-off);
        if (off > 0) n = (size_t)off;
        pt_locate(&d->C, n);
    } else if (whence[0] == 'l') {
        size_t    lnum = (size_t)off, br;
        lc_Cursor C;
        lpt_checkerror(L, lpt_docsync(d, lnum, -1));
        br = lc_breaks(d->lc);
        luaL_argcheck(L, lnum <= br, 3, "line out of range");
        if (lnum == br)
            pt_locate(&d->C, lc_bytes(d->lc));
        else
            lc_seekline(&C, d->lc, lnum), pt_locate(&d->C, lc_lineoffset(&C));
    }
    return lua_pushinteger(L, (lua_Integer)pt_offset(&d->C)), 1;
}

static int Ldoc_seek(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    int      n = lua_gettop(L), t = lua_type(L, 2);
    if (n < 2) return lua_pushinteger(L, (lua_Integer)pt_offset(&d->C)), 1;
    if (t == LUA_TNUMBER) {
        lua_Integer off = lua_tointeger(L, 2);
        luaL_argcheck(L, off >= 0, 2, "offset must be non-negative");
        return pt_locate(&d->C, (size_t)off), lua_settop(L, 2), 1;
    }
    return lpt_seekpos(L, d, luaL_checkstring(L, 2));
}

static int lpt_readbytes(lua_State *L, lpt_Doc *d, size_t len) {
    if (len == 0) return lua_pushliteral(L, ""), 1;
    if (pt_offset(&d->C) >= pt_bytes(pt_buffer(&d->C)))
        return lua_pushliteral(L, ""), 0;
    return lpt_readstring(L, &d->C, len) ? 1 : 0;
}

static int lpt_readline(lua_State *L, lpt_Doc *d, int wantnl) {
    pt_Cursor  *C = &d->C;
    luaL_Buffer B;
    size_t      i, len, n = 0;
    const char *src = pt_piece(C, &len);
    if (src == NULL) return lua_pushliteral(L, ""), 0;
    luaL_buffinit(L, &B);
    for (; src; src = pt_next(C, &len)) {
        char *buf = luaL_prepbuffer(&B);
        for (i = 0; i < len && src[i] != '\n'; i++) buf[n++] = src[i];
        if (i < len) {
            if (wantnl) buf[n++] = '\n';
            pt_advance(C, (pt_Delta)(i + 1));
            return luaL_addsize(&B, n), luaL_pushresult(&B), 1;
        }
        luaL_addsize(&B, n), n = 0;
    }
    return luaL_pushresult(&B), 1;
}

static void lpt_readall(lua_State *L, lpt_Doc *d) {
    size_t total = pt_bytes(pt_buffer(&d->C));
    size_t off = pt_offset(&d->C);
    if (off >= total)
        lua_pushliteral(L, "");
    else
        lpt_readstring(L, &d->C, total - off);
}

static int lpt_read(lua_State *L, lpt_Doc *d, int first) {
    int nargs = lua_gettop(L) - first + 1, n, success;
    if (nargs == 0) {
        success = lpt_readline(L, d, 0), n = first + 1;
    } else {
        luaL_checkstack(L, nargs + LUA_MINSTACK, "too many arguments");
        for (n = first, success = 1; nargs-- && success; n++) {
            if (lua_type(L, n) == LUA_TNUMBER) {
                success = lpt_readbytes(L, d, (size_t)luaL_checkinteger(L, n));
            } else {
                const char *p = luaL_checkstring(L, n);
                if (*p == '*') p++;
                switch (*p) {
                case 'l': success = lpt_readline(L, d, 0); break;
                case 'L': success = lpt_readline(L, d, 1); break;
                case 'a': lpt_readall(L, d), success = 1; break;
                default: return luaL_argerror(L, n, "invalid format");
                }
            }
        }
    }
    if (!success) lua_pop(L, 1), lua_pushnil(L);
    return n - first;
}

static int Ldoc_read(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    return lpt_read(L, d, 2);
}

static int Ldoc_insert(lua_State *L) {
    lpt_Doc    *d = lpt_checkdoc(L, 1);
    size_t      len, off = pt_offset(&d->C);
    int         r;
    const char *s = lpt_toliteral(L, 2, &len, &d->C);
    if (len == 0) return lua_settop(L, 1), 1;
    lpt_checkerror(L, ut_record(d->ut, off, 0, len));
    if ((r = pt_insert(&d->C, s, len)) < 0) ut_unrecord(d->ut, 1);
    return lpt_checkerror(L, r), lua_settop(L, 1), 1;
}

static int Ldoc_edit(lua_State *L) {
    lpt_Doc    *d = lpt_checkdoc(L, 1);
    size_t      del = (size_t)luaL_checkinteger(L, 2);
    size_t      len, off = pt_offset(&d->C);
    const char *s = luaL_checklstring(L, 3, &len);
    int         r;
    if (len > PT_MAX_HOLESIZE) luaL_argerror(L, 3, "string too long for hole");
    if ((r = ut_record(d->ut, off, del, len)) < 0) lpt_checkerror(L, r);
    if ((r = pt_edit(&d->C, del, s, len)) < 0) ut_unrecord(d->ut, 1);
    return lpt_checkerror(L, r), lua_settop(L, 1), 1;
}

static int lpt_docedit(lpt_Doc *d, size_t del, const char *s, size_t len) {
    size_t off = pt_offset(&d->C);
    int    r;
    if ((r = ut_record(d->ut, off, del, len)) < 0) return r;
    if ((r = pt_splice(&d->C, del, s, len)) < 0) ut_unrecord(d->ut, 1);
    return r;
}

static int Ldoc_write(lua_State *L) {
    lpt_Doc    *d = lpt_checkdoc(L, 1);
    size_t      len;
    const char *s = lpt_toliteral(L, 2, &len, &d->C);
    if (len == 0) return lua_settop(L, 1), 1;
    lpt_checkerror(L, lpt_docedit(d, 0, s, len));
    return lua_settop(L, 1), 1;
}

static int Ldoc_splice(lua_State *L) {
    lpt_Doc    *d = lpt_checkdoc(L, 1);
    size_t      del = (size_t)luaL_checkinteger(L, 2);
    size_t      len;
    const char *s = lpt_toliteral(L, 3, &len, &d->C);
    lpt_checkerror(L, lpt_docedit(d, del, s, len));
    return lua_settop(L, 1), 1;
}

static int Ldoc_remove(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    size_t   n = (size_t)luaL_checkinteger(L, 2);
    lpt_checkerror(L, lpt_docedit(d, n, NULL, 0));
    return lua_settop(L, 1), 1;
}

static int Ldoc_offset(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    return lua_pushinteger(L, (lua_Integer)pt_offset(&d->C)), 1;
}

static int Ldoc_column(lua_State *L) {
    lpt_Doc  *d = lpt_checkdoc(L, 1);
    lc_Cursor C;
    lpt_checkerror(L, lpt_docsync(d, LPT_UNL, pt_offset(&d->C)));
    assert(d->lc), lc_seek(&C, d->lc, pt_offset(&d->C));
    return lua_pushinteger(L, (lua_Integer)lc_col(&C)), 1;
}

static int Ldoc_line(lua_State *L) {
    lpt_Doc  *d = lpt_checkdoc(L, 1);
    lc_Cursor C;
    lpt_checkerror(L, lpt_docsync(d, LPT_UNL, pt_offset(&d->C)));
    assert(d->lc), lc_seek(&C, d->lc, pt_offset(&d->C));
    return lua_pushinteger(L, (lua_Integer)lc_line(&C)), 1;
}

static int Ldoc_linelen(lua_State *L) {
    lpt_Doc    *d = lpt_checkdoc(L, 1);
    lua_Integer nu = luaL_optinteger(L, 2, -1);
    lc_Cursor   C;
    size_t      br, lnum;
    if (nu >= 0) {
        lpt_checkerror(L, lpt_docsync(d, nu + 1, LPT_UNL));
        br = lc_breaks(d->lc), lnum = (size_t)nu;
        luaL_argcheck(L, lnum <= br, 2, "line number out of range");
        if (lnum < br) lc_seekline(&C, d->lc, lnum);
        if (lnum == br) lc_seek(&C, d->lc, pt_bytes(pt_buffer(&d->C)));
        return lua_pushinteger(L, (lua_Integer)lc_linelen(&C)), 1;
    }
    lpt_checkerror(L, lpt_docsync(d, LPT_UNL, pt_offset(&d->C)));
    assert(d->lc), lc_seek(&C, d->lc, pt_offset(&d->C));
    lpt_checkerror(L, lpt_docsync(d, lc_line(&C) + 1, LPT_UNL));
    if (lc_line(&C) == lc_breaks(d->lc))
        lc_seek(&C, d->lc, pt_bytes(pt_buffer(&d->C)));
    return lua_pushinteger(L, (lua_Integer)lc_linelen(&C)), 1;
}

static int Ldoc_breaks(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    size_t   total, trailing = 0;
    lpt_checkerror(L, lpt_docsync(d, LPT_UNL, LPT_UNL));
    total = pt_bytes(pt_buffer(&d->C));
    if (lc_bytes(d->lc) < total) trailing = 1;
    return lua_pushinteger(L, (lua_Integer)(lc_breaks(d->lc) + trailing)), 1;
}

static int Ldoc_lineiter(lua_State *L) {
    lpt_Doc *d = (lpt_Doc *)lua_touserdata(L, lua_upvalueindex(1));
    int      nargs = (int)lua_tointeger(L, lua_upvalueindex(2)), i, n;
    for (lua_settop(L, 0), i = 1; i <= nargs; i++)
        lua_pushvalue(L, lua_upvalueindex(2 + i));
    n = lpt_read(L, d, 1);
    return (n > 0 && lua_toboolean(L, -n)) ? n : 0;
}

static int Ldoc_lines(lua_State *L) {
    int n = lua_gettop(L) - 1, i;
    lpt_checkdoc(L, 1);
    luaL_argcheck(L, n <= 250, n + 2, "too many arguments");
    for (lua_pushvalue(L, 1), lua_pushinteger(L, n), i = 1; i <= n; i++)
        lua_pushvalue(L, 1 + i);
    return lua_pushcclosure(L, Ldoc_lineiter, 2 + n), 1;
}

static int Ldoc_commit(lua_State *L) {
    lpt_Doc  *d = lpt_checkdoc(L, 1);
    pt_Buffer b;
    ut_Vid    n;
    size_t    off;
    if (!ut_freshcount(d->ut)) {
        b = (pt_Buffer)ut_payload(ut_current(d->ut));
        return lua_pushinteger(L, (lua_Integer)pt_version(b)), 1;
    }
    off = pt_offset(&d->C);
    b = pt_commit(&d->C);
    if (!b) lpt_checkerror(L, PT_ERRMEM);
    if (d->lck && d->lck < (int)ut_freshcount(d->ut)) {
        int r = ut_freshdiff(d->ut, d->lck, (int)ut_freshcount(d->ut));
        if (r < 0) lpt_checkerror(L, r);
        lpt_checkerror(L, lpt_hunkapply(d->lc, ut_hunks(d->ut, NULL), r, b));
        d->lck = (int)ut_freshcount(d->ut);
    }
    n = ut_commit(d->ut, (ut_Payload *)b);
    if (!n) pt_release(b), lpt_checkerror(L, PT_ERRMEM);
    lpt_setvid(L, 1, (lua_Integer)pt_version(b), n);
    pt_seek(&d->C, b, off), d->lck = 0;
    return lua_pushinteger(L, (lua_Integer)pt_version(b)), 1;
}

static int lpt_switch(lua_State *L, lpt_Doc *d, ut_Vid src, ut_Vid dst) {
    pt_Buffer b = (pt_Buffer)ut_payload(dst);
    size_t    pos = pt_offset(&d->C);
    int       r;
    if ((r = ut_diff(d->ut, src, dst)) < 0) lpt_checkerror(L, r);
    pos = ut_mapoffset(d->ut, pos);
    r = lpt_hunkapply(d->lc, ut_hunks(d->ut, NULL), r, b);
    if (r == LC_OK) d->lcvid = dst;
    d->lck = 0, ut_switch(d->ut, dst);
    return pt_seek(&d->C, b, pos), lpt_pushvid(L, dst);
}

static int Ldoc_version(lua_State *L) {
    lpt_Doc  *d = lpt_checkdoc(L, 1);
    pt_Buffer b = (pt_Buffer)ut_payload(ut_current(d->ut));
    return lua_pushinteger(L, (lua_Integer)pt_version(b)), 1;
}

static int Ldoc_undo(lua_State *L) {
    lpt_Doc  *d = lpt_checkdoc(L, 1);
    ut_Vid    dst, src = ut_current(d->ut);
    pt_Buffer b;
    size_t    pos = pt_offset(&d->C);
    int       r, fc = ut_freshcount(d->ut);
    if (fc) {
        if ((r = ut_freshdiff(d->ut, fc, 0)) < 0) lpt_checkerror(L, r);
        pos = ut_mapoffset(d->ut, pos);
        b = (pt_Buffer)ut_payload(src);
        if (d->lck) {
            if (d->lck != fc && (r = ut_freshdiff(d->ut, d->lck, 0)) < 0)
                lpt_checkerror(L, r);
            r = lpt_hunkapply(d->lc, ut_hunks(d->ut, NULL), r, b);
            lpt_checkerror(L, r), d->lck = 0;
        }
        pt_release(pt_rollback(&d->C)), ut_discard(d->ut);
        pt_seek(&d->C, b, pos);
    }
    dst = lpt_checkvid(L, 2, 1, ut_parent(src));
    if (!dst || dst == src) return lpt_pushvid(L, src);
    return lpt_switch(L, d, src, dst);
}

static int Ldoc_redo(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    ut_Vid   dst, cur = ut_current(d->ut);
    if (ut_freshcount(d->ut))
        luaL_error(L, "piecetab: cannot time-travel with uncommitted changes");
    dst = ut_firstchild(cur);
    return dst ? lpt_switch(L, d, cur, dst) : lpt_pushvid(L, cur);
}

static int Ldoc_earlier(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    ut_Vid   dst, cur = ut_current(d->ut);
    if (ut_freshcount(d->ut))
        luaL_error(L, "piecetab: cannot time-travel with uncommitted changes");
    dst = ut_older(cur);
    return dst ? lpt_switch(L, d, cur, dst) : lpt_pushvid(L, cur);
}

static int Ldoc_later(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    ut_Vid   dst, cur = ut_current(d->ut);
    if (ut_freshcount(d->ut))
        luaL_error(L, "piecetab: cannot time-travel with uncommitted changes");
    dst = ut_younger(cur);
    return dst ? lpt_switch(L, d, cur, dst) : lpt_pushvid(L, cur);
}

static int Ldoc_buffer(lua_State *L) {
    lpt_Doc  *d = lpt_checkdoc(L, 1);
    ut_Vid    cur = ut_current(d->ut);
    pt_Buffer b = (pt_Buffer)ut_payload(lpt_checkvid(L, 2, 1, cur));
    return pt_retain(b), *lpt_newbuffer(L) = b, 1;
}

static int Ldoc_dump(lua_State *L) {
    lpt_Doc  *d = lpt_checkdoc(L, 1);
    size_t    total = pt_bytes(pt_buffer(&d->C));
    pt_Cursor C;
    if (total == 0) return lua_pushliteral(L, ""), 1;
    return pt_seek(&C, pt_buffer(&d->C), 0), lpt_readstring(L, &C, total);
}

static int Ldoc_piece(lua_State *L) {
    lpt_Doc    *d = lpt_checkdoc(L, 1);
    const char *s = luaL_checkstring(L, 2);
    size_t      len = 0;
    if (*s == 'l')
        pt_piece(&d->C, &len);
    else if (*s == 'n')
        pt_next(&d->C, &len);
    else if (*s == 'p')
        pt_prev(&d->C, &len);
    else
        luaL_argerror(
                L, 2,
                "invalid piece operation (expected 'len', 'next', or 'prev')");
    return lua_pushinteger(L, (lua_Integer)len), 1;
}

static void lpt_opendoc(lua_State *L) {
    static const luaL_Reg doc_methods[] = {
            {"__gc", Ldoc_gc},   {"__close", Ldoc_gc},
            {"__len", Ldoc_len}, {"append", Ldoc_write},
#define ENTRY(name) {#name, Ldoc_##name}
            ENTRY(seek),         ENTRY(read),
            ENTRY(write),        ENTRY(insert),
            ENTRY(edit),         ENTRY(splice),
            ENTRY(remove),       ENTRY(offset),
            ENTRY(column),       ENTRY(line),
            ENTRY(linelen),      ENTRY(breaks),
            ENTRY(lines),        ENTRY(commit),
            ENTRY(undo),         ENTRY(redo),
            ENTRY(earlier),      ENTRY(later),
            ENTRY(buffer),       ENTRY(dump),
            ENTRY(version),      ENTRY(piece),
#undef ENTRY
            {NULL, NULL}};
    if (luaL_newmetatable(L, LPT_DOC_TYPE)) {
        luaL_setfuncs(L, doc_methods, 0);
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
    if ((r = pt_insert(&C, s, len)) < 0 || !(*lpt_newbuffer(L) = pt_commit(&C)))
        pt_release(pt_rollback(&C)), lpt_checkerror(L, r ? r : PT_ERRMEM);
    return 1;
}

/* metatable registration */

LUAMOD_API int luaopen_piecetab(lua_State *L) {
    luaL_Reg libs[] = {{"version", NULL}, {"MAX_HOLESIZE", NULL},
#define ENTRY(name) {#name, Lpt_##name}
                       ENTRY(from),       ENTRY(empty),           ENTRY(doc),
#undef ENTRY
                       {NULL, NULL}};
    lpt_state(L), lpt_openbuffer(L), lpt_opencursor(L), lpt_opendoc(L);
    luaL_newlib(L, libs);
    lua_pushliteral(L, LPT_VERSION), lua_setfield(L, -2, "version");
    lua_pushinteger(L, PT_MAX_HOLESIZE), lua_setfield(L, -2, "MAX_HOLESIZE");
    return 1;
}
