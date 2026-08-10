#define LUA_LIB
#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>

/* yyjson vendored single-file library, statically linked (lyyjson
 * convention). API: decode(s)/encode(v)/load(path)/dump(path, v). */

#define yyjson_api static
#include "yyjson/yyjson.c"
#include "yyjson/yyjson.h"

/* compat layer (ref: lua-protobuf pb.c, lua/piecetab.c, treesitter.c) */

#if LUA_VERSION_NUM < 502
# define lua_rawlen             lua_objlen
# define luaL_setfuncs(L, l, n) (assert(n == 0), luaL_register(L, NULL, l))
# ifndef LUA_GCISRUNNING /* not LuaJIT 2.1 */
#  define luaL_newlib(L, l) (lua_newtable(L), luaL_register(L, NULL, l))
# endif
#endif /* LUA_VERSION_NUM < 502 */

/* integer-or-float probe (LuaJIT's lua_tointegerx truncates floats) */
#if LUA_VERSION_NUM >= 502
# define lyy_tointegerx lua_tointegerx
#else
static lua_Integer lyy_tointegerx(lua_State *L, int idx, int *pisint) {
    lua_Number n = lua_tonumber(L, idx);
    int        isint = lua_isnumber(L, idx) && n == (double)(lua_Integer)n;
    if (pisint) *pisint = isint;
    return isint ? (lua_Integer)n : 0;
}
#endif

/* relative -> absolute stack index (lua_next/rawgeti need stable idx) */
static int lyy_absindex(lua_State *L, int idx) {
    return idx > 0 || idx <= LUA_REGISTRYINDEX ? idx : lua_gettop(L) + idx + 1;
}

/* null sentinel: fixed registry table (identity compare, encode-testable) */
#define LY_NSENTINEL "yyjson.null"

#define LY_MAXDEPTH 64

/* short type aliases keep signatures within the 79-col limit */
typedef yyjson_mut_doc lyy_Doc;
typedef yyjson_mut_val lyy_Val;

/* decode: JSON value -> Lua value */

static int lyy_pushval(lua_State *L, yyjson_val *val, int depth) {
    yyjson_arr_iter aiter;
    yyjson_obj_iter oiter;
    yyjson_val     *key;
    size_t          i;
    yyjson_type     type = yyjson_get_type(val);
    if (depth >= LY_MAXDEPTH) return luaL_error(L, "yyjson: nesting too deep");
    switch (type) {
    case YYJSON_TYPE_NULL:
        lua_getfield(L, LUA_REGISTRYINDEX, LY_NSENTINEL);
        break;
    case YYJSON_TYPE_BOOL: lua_pushboolean(L, yyjson_get_bool(val)); break;
    case YYJSON_TYPE_NUM:
        if (yyjson_is_real(val))
            lua_pushnumber(L, yyjson_get_real(val));
        else
            lua_pushinteger(
                    L, yyjson_is_sint(val) ? yyjson_get_sint(val)
                                           : (lua_Integer)yyjson_get_uint(val));
        break;
    case YYJSON_TYPE_STR:
        lua_pushlstring(L, yyjson_get_str(val), (size_t)yyjson_get_len(val));
        break;
    case YYJSON_TYPE_ARR:
        aiter = yyjson_arr_iter_with(val);
        i = 0;
        lua_createtable(L, (int)yyjson_arr_size(val), 0);
        while (yyjson_arr_iter_has_next(&aiter)) {
            lyy_pushval(L, yyjson_arr_iter_next(&aiter), depth + 1);
            lua_rawseti(L, -2, (int)(i + 1));
            i++;
        }
        break;
    case YYJSON_TYPE_OBJ:
        oiter = yyjson_obj_iter_with(val);
        lua_createtable(L, 0, (int)yyjson_obj_size(val));
        while (yyjson_obj_iter_has_next(&oiter)) {
            key = yyjson_obj_iter_next(&oiter);
            /* keys are always strings in parsed JSON (structural) */
            lyy_pushval(L, yyjson_obj_iter_get_val(key), depth + 1);
            lua_setfield(L, -2, yyjson_get_str(key));
        }
        break;
    }
    return 1;
}

/* error names (mirror yyjson_read_code, yyjson.h) — X macro: one
 * definition feeds the pusherror switch; #name gives the literal */
#define LY_ERRORS(X)           \
    X(1, invalid_parameter)    \
    X(2, memory_allocation)    \
    X(3, empty_content)        \
    X(4, unexpected_content)   \
    X(5, unexpected_end)       \
    X(6, unexpected_character) \
    X(7, json_structure)       \
    X(8, invalid_comment)      \
    X(9, invalid_number)       \
    X(10, invalid_string)      \
    X(11, literal)             \
    X(12, file_open)           \
    X(13, file_read)

static int lyy_loaderr(lua_State *L, yyjson_read_err *err) {
    const char *msg = NULL;
#define LY_ERROR_CASE(code, name) \
    case code: msg = #name, lua_pushliteral(L, #name); break;
    lua_pushnil(L);
    switch (err->code) { LY_ERRORS(LY_ERROR_CASE) }
#undef LY_ERROR_CASE
    if (!msg) lua_pushliteral(L, "unknown");
    lua_pushinteger(L, (lua_Integer)err->pos);
    return 3;
}

/* push nil + error name; frees an optional output buffer */
static int lyy_ioerr(lua_State *L, const char *name) {
    return lua_pushnil(L), lua_pushstring(L, name), 2;
}

/* decode(s) -> v, or nil, err, err_pos on parse failure */
static int Ldecode(lua_State *L) {
    size_t          len;
    yyjson_read_err err;
    const char     *s = luaL_checklstring(L, 1, &len);
    yyjson_doc     *doc = yyjson_read_opts((char *)s, len, 0, NULL, &err);
    if (doc == NULL) return lyy_loaderr(L, &err);
    lyy_pushval(L, yyjson_doc_get_root(doc), 0);
    return yyjson_doc_free(doc), 1;
}

/* encode: Lua value -> JSON string */

/* table -> JSON array iff keys are exactly 1..#t (no holes) */
static int lyy_isarray(lua_State *L, int idx, size_t len) {
    int         isint;
    lua_Integer k;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        k = lyy_tointegerx(L, -2, &isint);
        if (lua_type(L, -2) != LUA_TNUMBER || !isint || k < 1
            || k > (lua_Integer)len) {
            lua_pop(L, 2); /* pop key + value */
            return 0;
        }
        lua_pop(L, 1); /* pop value; key stays on top for lua_next */
    }
    return 1;
}

/* recursion: forward declaration */
static lyy_Val *lyy_mutpush(lua_State *L, int idx, lyy_Doc *d, int depth);

static lyy_Val *lyy_pusharr(lua_State *L, int idx, lyy_Doc *d, int depth) {
    lyy_Val *arr = yyjson_mut_arr(d);
    size_t   i, len = (size_t)lua_rawlen(L, idx);
    for (i = 1; i <= len; i++) {
        lua_rawgeti(L, idx, (int)i);
        yyjson_mut_arr_append(arr, lyy_mutpush(L, -1, d, depth + 1));
        lua_pop(L, 1);
    }
    return arr;
}

static lyy_Val *lyy_pushobj(lua_State *L, int idx, lyy_Doc *d, int depth) {
    size_t      klen;
    const char *kstr;
    lyy_Val    *obj = yyjson_mut_obj(d);
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING)
            luaL_error(L, "yyjson: object key must be a string");
        kstr = lua_tolstring(L, -2, &klen);
        yyjson_mut_obj_add(
                obj, yyjson_mut_strn(d, kstr, klen),
                lyy_mutpush(L, -1, d, depth + 1));
        lua_pop(L, 1);
    }
    return obj;
}

static lyy_Val *lyy_mutpush(lua_State *L, int idx, lyy_Doc *d, int depth) {
    int         isint;
    int         any;
    lua_Integer n;
    size_t      len;
    const char *s;
    int         type = lua_type(L, idx);
    if (depth >= LY_MAXDEPTH) luaL_error(L, "yyjson: nesting too deep");
    switch (type) {
    case LUA_TBOOLEAN: return yyjson_mut_bool(d, (bool)lua_toboolean(L, idx));
    case LUA_TNUMBER:
        n = lyy_tointegerx(L, idx, &isint);
        return isint ? yyjson_mut_sint(d, (int64_t)n)
                     : yyjson_mut_real(d, (double)lua_tonumber(L, idx));
    case LUA_TSTRING:
        s = lua_tolstring(L, idx, &len);
        return yyjson_mut_strn(d, s, len);
    case LUA_TTABLE:
        idx = lyy_absindex(L, idx);
        /* null sentinel: identity check against the registry table */
        lua_getfield(L, LUA_REGISTRYINDEX, LY_NSENTINEL);
        if (lua_rawequal(L, idx, -1)) return lua_pop(L, 1), yyjson_mut_null(d);
        lua_pop(L, 1);
        len = (size_t)lua_rawlen(L, idx);
        if (len > 0 && lyy_isarray(L, idx, len))
            return lyy_pusharr(L, idx, d, depth);
        if (len == 0) {
            /* empty table -> object {} unless it has string keys */
            lua_pushnil(L);
            any = lua_next(L, idx) != 0;
            lua_pop(L, any ? 2 : 0);
            if (!any) return yyjson_mut_obj(d);
        }
        return lyy_pushobj(L, idx, d, depth);
    default: break;
    }
    luaL_error(L, "yyjson: cannot encode %s", lua_typename(L, type));
    return NULL;
}

/* copy out into a Lua string (caller owns out until after pcall) */
static int lyy_strpush(lua_State *L) {
    const char *out = (const char *)lua_touserdata(L, 1);
    size_t      len = (size_t)lua_tointeger(L, 2);
    return lua_pushlstring(L, out, len), 1;
}

/* push a malloc'd string OOM-safely: code after pcall runs on both
 * paths, so one free covers success and failure alike */
static int lyy_pushstr(lua_State *L, const char *out, size_t len) {
    lua_pushcfunction(L, lyy_strpush);
    lua_pushlightuserdata(L, (void *)out);
    lua_pushinteger(L, (lua_Integer)len);
    int r = lua_pcall(L, 2, 1, 0);
    free((void *)out);
    return r == LUA_OK ? 1 : lua_error(L);
}

static int Lencode(lua_State *L) {
    size_t   len;
    lyy_Doc *d = yyjson_mut_doc_new(NULL);
    lyy_Val *root = lyy_mutpush(L, 1, d, 0);
    yyjson_mut_doc_set_root(d, root);
    /* compact output: JSON-RPC wire format (dump keeps PRETTY) */
    char *out = yyjson_mut_write_opts(d, 0, NULL, &len, NULL);
    yyjson_mut_doc_free(d);
    if (out == NULL) return luaL_error(L, "yyjson: write failed");
    return lyy_pushstr(L, out, len);
}

/* load(path) -> v, or nil, err, err_pos on failure */
static int Lload(lua_State *L) {
    const char     *path = luaL_checkstring(L, 1);
    yyjson_read_err err;
    yyjson_doc     *doc;
    FILE           *f = fopen(path, "rb");
    if (f == NULL) return lyy_ioerr(L, "file_open");
    char  *buf = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            char *nb = (char *)realloc(buf, cap);
            if (nb == NULL) return luaL_error(L, "yyjson: out of memory");
            buf = nb;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n < cap - len) break; /* short read: EOF or error */
    }
    fclose(f);
    doc = yyjson_read_opts(buf, len, 0, NULL, &err);
    if (doc == NULL) return free(buf), lyy_loaderr(L, &err);
    lyy_pushval(L, yyjson_doc_get_root(doc), 0);
    yyjson_doc_free(doc);
    free(buf);
    return 1;
}

/* dump(path, v) -> true, or nil, err on failure */
static int Ldump(lua_State *L) {
    size_t      len;
    const char *path = luaL_checkstring(L, 1);
    lyy_Doc    *d = yyjson_mut_doc_new(NULL);
    lyy_Val    *root = lyy_mutpush(L, 2, d, 0);
    yyjson_mut_doc_set_root(d, root);
    char *out = yyjson_mut_write_opts(d, YYJSON_WRITE_PRETTY, NULL, &len, NULL);
    yyjson_mut_doc_free(d);
    if (out == NULL) return luaL_error(L, "yyjson: write failed");
    FILE *f = fopen(path, "wb");
    if (f == NULL) return free(out), lyy_ioerr(L, "file_open");
    int werr = fwrite(out, 1, len, f) != len || fclose(f) != 0;
    if (werr) return free(out), lyy_ioerr(L, "write_failed");
    free(out);
    lua_pushboolean(L, 1);
    return 1;
}

LUALIB_API int luaopen_yyjson(lua_State *L) {
    luaL_Reg libs[] = {
#define ENTRY(name) {#name, L##name}
            ENTRY(decode),
            ENTRY(encode),
            ENTRY(load),
            ENTRY(dump),
#undef ENTRY
            {NULL, NULL}};
    luaL_newlib(L, libs);
    /* null sentinel: fixed table in registry, exposed as yyjson.null */
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, LY_NSENTINEL);
    lua_setfield(L, -2, "null");
    return 1;
}
