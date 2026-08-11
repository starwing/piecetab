#define LUA_LIB
#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>

/* yyjson vendored single-file library, statically linked (lyyjson
 * convention). API: decode(s)/encode(v)/array(t?)/object(t?). */

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

/* type markers: decoded values carry a metatable whose __jsontype field
 * says what they were; any plain table can opt in via the exported
 * json.array/json.object/json.null metatables. JSON null is the null
 * marker table itself (identity compare works on decoded nulls). */
#define LY_ARR_MT   "json.Array"
#define LY_OBJ_MT   "json.Object"
#define LY_NULL_MT  "json.null"
#define LY_TYPEFLD  "__jsontype"
#define LY_TYPE_ARR "array"
#define LY_TYPE_OBJ "object"
#define LY_TYPE_NUL "null"

#define LY_MAXDEPTH 64

/* short type aliases keep signatures within the 79-col limit */
typedef yyjson_mut_doc lyy_Doc;
typedef yyjson_mut_val lyy_Val;

/* lookup a value's __jsontype (nil when it has no marker metatable) */
static const char *lyy_marker(lua_State *L, int idx) {
    if (!lua_getmetatable(L, idx)) return NULL;
    lua_getfield(L, -1, LY_TYPEFLD);
    const char *m = lua_tostring(L, -1);
    lua_pop(L, 2);
    return m;
}

/* push the registry marker metatable for a type name */
static void lyy_pushmt(lua_State *L, const char *name) {
    luaL_getmetatable(L, name);
}

/* decode: JSON value -> Lua value */

static int lyy_pushval(lua_State *L, yyjson_val *val, int depth) {
    yyjson_arr_iter aiter;
    yyjson_obj_iter oiter;
    yyjson_val     *key;
    size_t          i;
    yyjson_type     type = yyjson_get_type(val);
    if (depth >= LY_MAXDEPTH) return luaL_error(L, "json: nesting too deep");
    switch (type) {
    case YYJSON_TYPE_NULL: return lyy_pushmt(L, LY_NULL_MT), 1;
    case YYJSON_TYPE_BOOL: return lua_pushboolean(L, yyjson_get_bool(val)), 1;
    case YYJSON_TYPE_NUM:
        if (yyjson_is_real(val))
            return lua_pushnumber(L, yyjson_get_real(val)), 1;
        if (yyjson_is_sint(val))
            return lua_pushinteger(L, yyjson_get_sint(val)), 1;
        return lua_pushinteger(L, (lua_Integer)yyjson_get_uint(val)), 1;
    case YYJSON_TYPE_STR:
        return lua_pushlstring(L, yyjson_get_str(val), yyjson_get_len(val)), 1;
    case YYJSON_TYPE_ARR:
        aiter = yyjson_arr_iter_with(val);
        lua_createtable(L, (int)yyjson_arr_size(val), 0);
        luaL_setmetatable(L, LY_ARR_MT);
        for (i = 0; yyjson_arr_iter_has_next(&aiter); ++i) {
            lyy_pushval(L, yyjson_arr_iter_next(&aiter), depth + 1);
            lua_rawseti(L, -2, (int)(i + 1));
        }
        break;
    case YYJSON_TYPE_OBJ:
        oiter = yyjson_obj_iter_with(val);
        lua_createtable(L, 0, (int)yyjson_obj_size(val));
        luaL_setmetatable(L, LY_OBJ_MT);
        while (yyjson_obj_iter_has_next(&oiter)) {
            key = yyjson_obj_iter_next(&oiter);
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
    X(11, literal)

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
            luaL_error(L, "json: object key must be a string");
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
    const char *m;
    int         type = lua_type(L, idx);
    if (depth >= LY_MAXDEPTH) luaL_error(L, "json: nesting too deep");
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
        /* type marker first: any table tagged null/array/object encodes
         * as such (json.null itself is the null marker metatable) */
        m = lyy_marker(L, idx);
        if (m) {
            if (m[0] == 'n') return yyjson_mut_null(d);
            if (m[0] == 'a') return lyy_pusharr(L, idx, d, depth);
            return lyy_pushobj(L, idx, d, depth);
        }
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
    luaL_error(L, "json: cannot encode %s", lua_typename(L, type));
    return NULL;
}

/* encode work: runs inside pcall so the mut doc is freed on error too;
 * doc/out cross the pcall via lightuserdata (out lives in Lencode's frame) */
static int lyy_encoder(lua_State *L) {
    lyy_Doc *d = (lyy_Doc *)lua_touserdata(L, 2);
    char   **out = (char **)lua_touserdata(L, 3);
    size_t   len;
    lyy_Val *root = lyy_mutpush(L, 1, d, 0);
    yyjson_mut_doc_set_root(d, root);
    /* compact output: JSON-RPC wire format */
    *out = yyjson_mut_write_opts(d, 0, NULL, &len, NULL);
    if (*out == NULL) return luaL_error(L, "json: write failed");
    return lua_pushlstring(L, *out, len), 1;
}

static int Lencode(lua_State *L) {
    lyy_Doc *d = yyjson_mut_doc_new(NULL);
    char    *out = NULL;
    int      r;
    if (d == NULL) return luaL_error(L, "json: out of memory");
    lua_settop(L, 1);
    lua_pushcfunction(L, lyy_encoder);
    lua_insert(L, 1);
    lua_pushlightuserdata(L, d);
    lua_pushlightuserdata(L, &out);
    r = lua_pcall(L, 3, LUA_MULTRET, 0);
    yyjson_mut_doc_free(d);
    free(out);
    return r == LUA_OK ? lua_gettop(L) : lua_error(L);
}

/* type constructors: tag t (or a fresh table) with the marker metatable */

static int Larray(lua_State *L) {
    if (lua_isnoneornil(L, 2)) lua_createtable(L, 0, 0);
    return luaL_setmetatable(L, LY_ARR_MT), 1;
}

static int Lobject(lua_State *L) {
    if (lua_isnoneornil(L, 2)) lua_createtable(L, 0, 0);
    return luaL_setmetatable(L, LY_OBJ_MT), 1;
}

static int Ltype(lua_State *L) {
    const char *m;
    int         isint, t = lua_type(L, 1);
    size_t      len;
    switch (t) {
    case LUA_TBOOLEAN: m = "boolean"; break;
    case LUA_TNUMBER:
        lyy_tointegerx(L, 1, &isint);
        m = isint ? "integer" : "number";
        break;
    case LUA_TSTRING: m = "string"; break;
    case LUA_TTABLE:
        if ((m = lyy_marker(L, 1))) return lua_pushstring(L, m), 1;
        len = (size_t)lua_rawlen(L, 1);
        m = len > 0 && lyy_isarray(L, 1, len) ? "array" : "object";
        break;
    default: return lua_pushnil(L), 1;
    }
    return lua_pushstring(L, m), 1;
}

LUALIB_API int luaopen_json(lua_State *L) {
    luaL_Reg libs[] = {{"version", NULL}, {"array", NULL}, {"object", NULL},
#define ENTRY(name) {#name, L##name}
                       ENTRY(decode),     ENTRY(encode),   ENTRY(type),
#undef ENTRY
                       {NULL, NULL}};
    luaL_newlib(L, libs);
    lua_pushliteral(L, YYJSON_VERSION_STRING);
    lua_setfield(L, -2, "version");
    if (luaL_newmetatable(L, LY_ARR_MT)) {
        lua_pushliteral(L, LY_TYPE_ARR);
        lua_setfield(L, -2, LY_TYPEFLD);
        lua_createtable(L, 0, 1);
        lua_pushcfunction(L, Larray);
        lua_setfield(L, -2, "__call");
        lua_setmetatable(L, -2);
    }
    lua_setfield(L, -2, "array");
    if (luaL_newmetatable(L, LY_OBJ_MT)) {
        lua_pushliteral(L, LY_TYPE_OBJ);
        lua_setfield(L, -2, LY_TYPEFLD);
        lua_createtable(L, 0, 1);
        lua_pushcfunction(L, Lobject);
        lua_setfield(L, -2, "__call");
        lua_setmetatable(L, -2);
    }
    lua_setfield(L, -2, "object");
    if (luaL_newmetatable(L, LY_NULL_MT)) {
        lua_pushliteral(L, LY_TYPE_NUL);
        lua_setfield(L, -2, LY_TYPEFLD);
        lua_pushvalue(L, -1);
        lua_setmetatable(L, -2);
    }
    lua_setfield(L, -2, "null");
    return 1;
}
