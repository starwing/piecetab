#define LUA_LIB
#include <lauxlib.h>
#include <lua.h>

typedef unsigned char u8;
typedef unsigned int  u32;

/* clang-format off */
#if GCC_VERSION >= 5004000 || CLANG_VERSION >= 4000000
#define addu __builtin_add_overflow
#else
static int addu(size_t a, size_t b, size_t *c)
{ return (~(size_t)0 - a < b) ? 0 : (*c = a + b), 1; }
#endif

static u8 utf8_sequence_length(u32 cp)
{ return (cp >= 0x10000) + (cp >= 0x00800) + (cp >= 0x00080) + 1; }
/* clang-format on */

static u32 utf8_encode(u8 out[4], u32 cp) {
    u32 result = 0;
    if (cp <= 0x7F)
        out[0] = cp & 0x7F, result = 1;
    else if (cp <= 0x7FF) {
        result = 2;
        out[0] = ((cp >> 6) & 0x1F) | 0xC0;
        out[1] = ((cp >> 0) & 0x3F) | 0x80;
    } else if (cp <= 0xFFFF) {
        result = 3;
        out[0] = ((cp >> 12) & 0x0F) | 0xE0;
        out[1] = ((cp >> 6) & 0x3F) | 0x80;
        out[2] = ((cp >> 0) & 0x3F) | 0x80;
    } else if (cp <= 0x10FFFF) {
        result = 4;
        out[0] = ((cp >> 18) & 0x07) | 0xF0;
        out[1] = ((cp >> 12) & 0x3F) | 0x80;
        out[2] = ((cp >> 6) & 0x3F) | 0x80;
        out[3] = ((cp >> 0) & 0x3F) | 0x80;
    }
    return result;
}

#define TERMKEY_EXPORT static
#include "external/termkey.c"

#define LTK_NAME "Termkey"

typedef struct ltk_State {
    TermKey    tk;
    TermKeyKey key;
} ltk_State;

static int Ltk_new(lua_State *L) {
    int         fd = (int)luaL_checkinteger(L, 1);
    const char *term = luaL_optstring(L, 2, "");
    int         flags = lua_toboolean(L, 3) ? TERMKEY_FLAG_NOTERMIOS : 0;
    ltk_State  *S = (ltk_State *)lua_newuserdata(L, sizeof(ltk_State));
    int         r = termkey_init_from_fd(&S->tk, fd, flags, (char *)term);
    memset(&S->key, 0, sizeof(TermKeyKey));
    luaL_setmetatable(L, LTK_NAME);
    return r ? 1 : 0;
}

static int Ltk_delete(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    if (S->tk.fd == -1) return 0;
    return termkey_destroy(&S->tk), 0;
}

static int Ltk_start(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    int        flags = lua_toboolean(L, 2) ? TERMKEY_FLAG_NOTERMIOS : 0;
    int        r = termkey_start(&S->tk, flags);
    return r ? (lua_settop(L, 1), 1) : 0;
}

static int Ltk_stop(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    return termkey_stop(&S->tk), lua_settop(L, 1), 1;
}

static int Ltk_release(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    return termkey_release(&S->tk), 0;
}

static int Ltk_setcanonflags(lua_State *L) {
    const char *opts[] = {"delbs", 0};
    ltk_State  *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    int         f = 0;
    switch (luaL_checkoption(L, 2, NULL, opts)) {
    case 0: f = TERMKEY_CANON_DELBS; break;
    }
    termkey_set_canonflags(&S->tk, f);
    return lua_settop(L, 1), 1;
}

static int ltk_pushresult(lua_State *L, TermKeyResult r) {
    switch (r) {
    case TERMKEY_RES_NONE: return lua_pushliteral(L, "NONE"), 1;
    case TERMKEY_RES_KEY: return lua_pushliteral(L, "KEY"), 1;
    case TERMKEY_RES_EOF: return lua_pushliteral(L, "EOF"), 1;
    case TERMKEY_RES_AGAIN: return lua_pushliteral(L, "AGAIN"), 1;
    case TERMKEY_RES_ERROR: return lua_pushliteral(L, "ERROR"), 1;
    default: return lua_pushinteger(L, r), 1;
    }
}

static int ltk_pushtype(lua_State *L, TermKeyType t) {
    switch (t) {
    case TERMKEY_TYPE_UNICODE: return lua_pushliteral(L, "UNICODE"), 1;
    case TERMKEY_TYPE_FUNCTION: return lua_pushliteral(L, "FUNCTION"), 1;
    case TERMKEY_TYPE_KEYSYM: return lua_pushliteral(L, "KEYSYM"), 1;
    case TERMKEY_TYPE_MOUSE: return lua_pushliteral(L, "MOUSE"), 1;
    case TERMKEY_TYPE_POSITION: return lua_pushliteral(L, "POSITION"), 1;
    case TERMKEY_TYPE_MODEREPORT: return lua_pushliteral(L, "MODEREPORT"), 1;
    case TERMKEY_TYPE_DCS: return lua_pushliteral(L, "DCS"), 1;
    case TERMKEY_TYPE_OSC: return lua_pushliteral(L, "OSC"), 1;
    case TERMKEY_TYPE_UNKNOWN_CSI: return lua_pushliteral(L, "UNKNOWN_CSI"), 1;
    default: return lua_pushinteger(L, t), 1;
    }
}

static int ltk_pushmouse(lua_State *L, ltk_State *S) {
    TermKeyMouseEvent evt;
    int               button, line, col;
    assert(S->key.type == TERMKEY_TYPE_MOUSE);
    termkey_interpret_mouse(&S->tk, &S->key, &evt, &button, &line, &col);
    switch (evt) {
    case TERMKEY_MOUSE_PRESS: lua_pushliteral(L, "PRESS"); break;
    case TERMKEY_MOUSE_DRAG: lua_pushliteral(L, "DRAG"); break;
    case TERMKEY_MOUSE_RELEASE: lua_pushliteral(L, "RELEASE"); break;
    default: lua_pushliteral(L, "UNKNOWN"); break;
    }
    lua_pushinteger(L, button);
    lua_pushinteger(L, line);
    lua_pushinteger(L, col);
    return 4;
}

static int ltk_pushposition(lua_State *L, ltk_State *S) {
    int line, col;
    assert(S->key.type == TERMKEY_TYPE_POSITION);
    termkey_key_get_linecol(&S->key, &line, &col);
    lua_pushinteger(L, line);
    lua_pushinteger(L, col);
    return 2;
}

static int ltk_pushmodereport(lua_State *L, ltk_State *S) {
    int initial, mode, value;
    assert(S->key.type == TERMKEY_TYPE_MODEREPORT);
    termkey_interpret_modereport(&S->tk, &S->key, &initial, &mode, &value);
    lua_pushinteger(L, initial);
    lua_pushinteger(L, mode);
    lua_pushinteger(L, value);
    return 3;
}

static int ltk_pushunknowncsi(lua_State *L, ltk_State *S) {
    unsigned long cmd;
    long          args[16];
    size_t        i, nargs = 16;
    assert(S->key.type == TERMKEY_TYPE_UNKNOWN_CSI);
    termkey_interpret_csi(&S->tk, &S->key, args, &nargs, &cmd);
    lua_createtable(L, 3, nargs);
    if ((cmd >> 16))
        lua_pushinteger(L, cmd >> 16), lua_setfield(L, -2, "intermediate");
    if (((cmd >> 8) & 0xFF))
        lua_pushinteger(L, (cmd >> 8) & 0xFF), lua_setfield(L, -2, "initial");
    lua_pushinteger(L, cmd & 0xFF), lua_setfield(L, -2, "cmd");
    for (i = 0; i < nargs; ++i)
        lua_pushinteger(L, args[i]), lua_rawseti(L, -2, i + 1);
    return 1;
}

static int Ltk_getkey(lua_State *L) {
    ltk_State  *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    const char *force = luaL_optstring(L, 2, "");
    if (*force == 'f')
        return ltk_pushresult(L, termkey_getkey_force(&S->tk, &S->key));
    return ltk_pushresult(L, termkey_getkey(&S->tk, &S->key));
}

static int Ltk_advisereadable(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    return ltk_pushresult(L, termkey_advisereadable(&S->tk));
}

static int Ltk_key(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    return ltk_pushtype(L, S->key.type);
}

static int Ltk_data(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    switch (S->key.type) {
    case TERMKEY_TYPE_UNICODE:
        lua_pushstring(L, (char *)S->key.utf8);
        lua_pushinteger(L, S->key.code.codepoint);
        return 2;
    case TERMKEY_TYPE_FUNCTION:
        return lua_pushinteger(L, S->key.code.number), 1;
    case TERMKEY_TYPE_KEYSYM:
        lua_pushstring(L, termkey_get_keyname(&S->tk, S->key.code.sym));
        lua_pushinteger(L, S->key.code.sym);
        return 2;
    case TERMKEY_TYPE_MOUSE: return ltk_pushmouse(L, S);
    case TERMKEY_TYPE_POSITION: return ltk_pushposition(L, S);
    case TERMKEY_TYPE_MODEREPORT: return ltk_pushmodereport(L, S);
    case TERMKEY_TYPE_UNKNOWN_CSI: return ltk_pushunknowncsi(L, S);
    default: return 0;
    }
}

static int Ltk_mod(lua_State *L) {
    ltk_State  *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    const char *mod = luaL_optstring(L, 2, "");
    if (*mod == 0) {
        char ms[3] = "";
        int  len = 0;
        if ((S->key.modifiers & TERMKEY_KEYMOD_SHIFT)) ms[len++] = 'S';
        if ((S->key.modifiers & TERMKEY_KEYMOD_ALT)) ms[len++] = 'A';
        if ((S->key.modifiers & TERMKEY_KEYMOD_CTRL)) ms[len++] = 'C';
        return lua_pushlstring(L, ms, len), 1;
    } else if (*mod == 'S' || *mod == 's')
        return lua_pushboolean(L, (S->key.modifiers & TERMKEY_KEYMOD_SHIFT)), 1;
    else if (*mod == 'A' || *mod == 'a' || *mod == 'M' || *mod == 'm')
        return lua_pushboolean(L, (S->key.modifiers & TERMKEY_KEYMOD_ALT)), 1;
    else if (*mod == 'C' || *mod == 'c')
        return lua_pushboolean(L, (S->key.modifiers & TERMKEY_KEYMOD_CTRL)), 1;
    return 0;
}

static int Ltk_formatflags(lua_State *L) {
#define FORMAT(X) \
    X(LONGMOD)    \
    X(CARETCTRL)  \
    X(ALTISMETA)  \
    X(WRAPBRACKET) X(SPACEMOD) X(LOWERMOD) X(LOWERSPACE) X(MOUSE_POS)
    static const char *opts[] = {
#define X(f) #f,
            FORMAT(X)
#undef X
                    NULL};
    static const int mask[] = {
#define X(f) TERMKEY_FORMAT_##f,
            FORMAT(X)
#undef X
    };
    int i, top, flags = 0;
    for (i = 1, top = lua_gettop(L); i <= top; ++i)
        flags |= mask[luaL_checkoption(L, i, NULL, opts)];
    return flags;
}

static int Ltk_format(lua_State *L) {
    ltk_State  *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    int         format = luaL_optinteger(L, 2, 0);
    luaL_Buffer B;
    size_t      len;
    luaL_buffinit(L, &B);
    len = termkey_strfkey(
            &S->tk, luaL_prepbuffer(&B), LUAL_BUFFERSIZE, &S->key, format);
    luaL_addsize(&B, len);
    return luaL_pushresult(&B), 1;
}

static int Ltk_parse(lua_State *L) {
    ltk_State  *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    const char *s = luaL_checkstring(L, 2);
    int         format = (int)luaL_optinteger(L, 3, 0);
    const char *e = termkey_strpkey(&S->tk, s, &S->key, format);
    if (e == NULL) return 0;
    return lua_pushboolean(L, 1), lua_pushinteger(L, e - s + 1), 2;
}

static int Ltk_canonicalise(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    termkey_canonicalise(&S->tk, &S->key);
    return lua_settop(L, 1), 1;
}

static int Ltk_waitkey(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    return ltk_pushresult(L, termkey_waitkey(&S->tk, &S->key));
}

static int Ltk_setwaittime(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    int msec = (int)luaL_checkinteger(L, 2);
    termkey_set_waittime(&S->tk, msec);
    return lua_settop(L, 1), 1;
}

static int Ltk_getwaittime(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
    return lua_pushinteger(L, termkey_get_waittime(&S->tk)), 1;
}

#ifndef _WIN32
#include <fcntl.h>
#endif

static int Ltk_blocking(lua_State *L) {
    ltk_State *S = (ltk_State *)luaL_checkudata(L, 1, LTK_NAME);
#ifndef _WIN32
    int flags = fcntl(S->tk.fd, F_GETFL, 0);
    fcntl(S->tk.fd, F_SETFL, flags & ~O_NONBLOCK);
#endif
    return lua_settop(L, 1), 1;
}

LUALIB_API int luaopen_termkey(lua_State *L) {
    luaL_Reg libs[] = {
            {"__gc", Ltk_delete},
#define ENTRY(name) {#name, Ltk_##name}
            ENTRY(new),           ENTRY(delete),       ENTRY(start),
            ENTRY(stop),          ENTRY(release),      ENTRY(setcanonflags),
            ENTRY(getkey),        ENTRY(waitkey),      ENTRY(setwaittime),
            ENTRY(getwaittime),   ENTRY(canonicalise), ENTRY(advisereadable),
            ENTRY(blocking),      ENTRY(key),          ENTRY(data),
            ENTRY(mod),           ENTRY(formatflags),  ENTRY(format),
            ENTRY(parse),
#undef ENTRY
            {NULL, NULL}};
    if (luaL_newmetatable(L, LTK_NAME)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
    return 1;
}

/*
 * maccc: flags+='-shared -fPIC -DNDEBUG -O2 -undefined dynamic_lookup'
 * cc: output='termkey.so'
 */
