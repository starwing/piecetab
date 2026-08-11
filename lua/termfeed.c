#ifdef _MSC_VER
# define _CRT_SECURE_NO_DEPRECATE 1
# define _CRT_SECURE_NO_WARNINGS  1
#endif

#define LUA_LIB
#include <assert.h>
#include <lauxlib.h>
#include <lua.h>
#include <string.h>

#ifndef _WIN32
# include <termios.h>
#endif

#define TF_STATIC_API
#include "termfeed.h"

#define LTF_STATE_TYPE "termfeed.State"

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

#ifndef LUA_NOREF
# define LUA_NOREF (-2)
#endif

#ifndef lua_isinteger
# define lua_isinteger(L, idx) (lua_type((L), (idx)) == LUA_TNUMBER)
#endif

#ifndef lua_isnoneornil
# define lua_isnoneornil(L, idx) (lua_type((L), (idx)) <= 0)
#endif

/* ---- parser userdata ---- */

typedef struct ltf_State {
    lua_State *L;
    tf_State   S;
    tf_Key     key;
    char      *feed; /* pending feed chunk (reader-owned) */
    size_t     feedlen;
    char      *lookup; /* terminfo sequence buffer (lookup callback) */
    size_t     looklen;
    int        lookup_ref; /* Lua lookup callback, LUA_NOREF = none */
#ifndef _WIN32
    int            rawfd; /* raw() fd, -1 = not in raw mode */
    struct termios oldtio;
#endif
} ltf_State;

static ltf_State *ltf_check(lua_State *L, int idx) {
    return (ltf_State *)luaL_checkudata(L, idx, LTF_STATE_TYPE);
}

static int ltf_checkerror(lua_State *L, int r) {
    switch (r) {
    case TF_ERRMEM: return luaL_error(L, "termfeed: out of memory");
    case TF_ERRPARAM: return luaL_error(L, "termfeed: invalid parameter");
    default: return r;
    }
}

static int ltf_pushresult(lua_State *L, int r) {
    switch (r) {
    case TF_OK: lua_pushliteral(L, "KEY"); break;
    case TF_NONE: lua_pushliteral(L, "NONE"); break;
    case TF_AGAIN: lua_pushliteral(L, "AGAIN"); break;
    default: lua_pushinteger(L, r); break;
    }
    return 1;
}

/* ---- lifecycle ---- */

static void ltf_freelookup(lua_State *L, ltf_State *st) {
    if (st->lookup_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, st->lookup_ref);
        st->lookup_ref = LUA_NOREF;
    }
}

static int Ltf_new(lua_State *L) {
    ltf_State *st = (ltf_State *)lua_newuserdata(L, sizeof(ltf_State));
    void      *aud;
    memset(st, 0, sizeof(*st));
    st->L = L;
    st->lookup_ref = LUA_NOREF;
#ifndef _WIN32
    st->rawfd = -1;
#endif
    tf_init(&st->S, lua_getallocf(L, &aud), aud);
    luaL_setmetatable(L, LTF_STATE_TYPE);
    return 1;
}

#ifndef _WIN32
static void ltf_restore(ltf_State *st) {
    if (st->rawfd >= 0) {
        tcsetattr(st->rawfd, TCSANOW, &st->oldtio);
        st->rawfd = -1;
    }
}
#endif

static int Ltf_delete(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    ltf_freelookup(L, st);
#ifndef _WIN32
    ltf_restore(st);
#endif
    if (st->feed) st->S.allocf(st->S.alloc_ud, st->feed, st->feedlen, 0);
    if (st->lookup) st->S.allocf(st->S.alloc_ud, st->lookup, st->looklen, 0);
    st->feed = st->lookup = NULL;
    tf_free(&st->S);
    return 0;
}

/* ---- termios helpers (editor raw-mode convenience) ---- */

#ifndef _WIN32
static int Ltf_raw(lua_State *L) {
    ltf_State     *st = ltf_check(L, 1);
    int            fd = (int)luaL_optinteger(L, 2, 0);
    struct termios t;
    if (tcgetattr(fd, &st->oldtio) < 0)
        return luaL_error(L, "termfeed: tcgetattr(%d) failed", fd);
    t = st->oldtio;
    t.c_iflag &= (tcflag_t) ~(
            IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= (tcflag_t)~OPOST;
    t.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= (tcflag_t) ~(CSIZE | PARENB);
    t.c_cflag |= CS8;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) < 0)
        return luaL_error(L, "termfeed: tcsetattr(%d) failed", fd);
    st->rawfd = fd;
    return lua_settop(L, 1), 1;
}

static int Ltf_cooked(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    ltf_restore(st);
    return lua_settop(L, 1), 1;
}
#else
static int Ltf_raw(lua_State *L) { return lua_settop(L, 1), 1; }

static int Ltf_cooked(lua_State *L) { return lua_settop(L, 1), 1; }
#endif

/* ---- attributes ---- */

static int Ltf_setflag(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    int        flag = (int)luaL_checkinteger(L, 2);
    return lua_pushinteger(L, tf_setflag(&st->S, flag)), 1;
}

/* ---- terminfo lookup (Lua callback) ---- */

static const char *ltf_lookup(void *ud, const char *name) {
    ltf_State  *st = (ltf_State *)ud;
    const char *s;
    size_t      len;
    if (st->lookup_ref == LUA_NOREF) return NULL;
    lua_rawgeti(st->L, LUA_REGISTRYINDEX, st->lookup_ref);
    lua_pushstring(st->L, name);
    if (lua_pcall(st->L, 1, 1, 0) != LUA_OK) lua_error(st->L);
    s = lua_tolstring(st->L, -1, &len);
    lua_pop(st->L, 1);
    if (!s) return NULL;
    st->lookup = (char *)st->S.allocf(
            st->S.alloc_ud, st->lookup, st->looklen, len + 1);
    if (!st->lookup) luaL_error(st->L, "termfeed: out of memory");
    memcpy(st->lookup, s, len + 1);
    st->looklen = len + 1;
    return st->lookup;
}

static int Ltf_load(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    int        r;
    ltf_freelookup(L, st);
    if (lua_isnoneornil(L, 2)) return lua_settop(L, 1), 1;
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    st->lookup_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    r = tf_load(&st->S, ltf_lookup, st);
    if (r == TF_ERRMEM) return luaL_error(L, "termfeed: out of memory");
    return lua_settop(L, 1), 1;
}

/* ---- feeding & parsing ---- */

static const char *ltf_reader(void *ud, size_t *plen) {
    ltf_State *st = (ltf_State *)ud;
    *plen = st->feedlen;
    st->feedlen = 0;
    return st->feed;
}

static int Ltf_feed(lua_State *L) {
    ltf_State  *st = ltf_check(L, 1);
    size_t      len;
    const char *s = luaL_checklstring(L, 2, &len);
    if (st->feed) st->S.allocf(st->S.alloc_ud, st->feed, st->feedlen, 0);
    st->feed = NULL, st->feedlen = 0;
    if (len) {
        st->feed = (char *)st->S.allocf(st->S.alloc_ud, NULL, 0, len);
        if (!st->feed) return luaL_error(L, "termfeed: out of memory");
        memcpy(st->feed, s, len);
        st->feedlen = len;
    }
    tf_feed(&st->S, ltf_reader, st);
    return lua_settop(L, 1), 1;
}

static int Ltf_readkey(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    int        r = tf_readkey(&st->S, &st->key);
    return ltf_pushresult(L, ltf_checkerror(L, r));
}

static int Ltf_waitkey(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    int        fd = (int)luaL_optinteger(L, 2, 0);
    int        timeout = (int)luaL_optinteger(L, 3, -1);
    int        r = tf_waitkey(&st->S, fd, timeout, &st->key);
    return ltf_pushresult(L, ltf_checkerror(L, r));
}

static int Ltf_flush(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    int        r = tf_flush(&st->S, &st->key);
    return ltf_pushresult(L, ltf_checkerror(L, r));
}

/* ---- key accessors ---- */

static int ltf_pushtype(lua_State *L, tf_Type t) {
    switch (t) {
    case TF_TYPE_NONE: lua_pushliteral(L, "NONE"); break;
    case TF_TYPE_UNICODE: lua_pushliteral(L, "UNICODE"); break;
    case TF_TYPE_FUNCTION: lua_pushliteral(L, "FUNCTION"); break;
    case TF_TYPE_KEYSYM: lua_pushliteral(L, "KEYSYM"); break;
    case TF_TYPE_MOUSE: lua_pushliteral(L, "MOUSE"); break;
    case TF_TYPE_POSITION: lua_pushliteral(L, "POSITION"); break;
    case TF_TYPE_MODEREPORT: lua_pushliteral(L, "MODEREPORT"); break;
    case TF_TYPE_DCS: lua_pushliteral(L, "DCS"); break;
    case TF_TYPE_OSC: lua_pushliteral(L, "OSC"); break;
    case TF_TYPE_APC: lua_pushliteral(L, "APC"); break;
    case TF_TYPE_KITTYREPORT: lua_pushliteral(L, "KITTYREPORT"); break;
    case TF_TYPE_UNKNOWN_CSI: lua_pushliteral(L, "UNKNOWN_CSI"); break;
    }
    return 1;
}

static int ltf_pushevent(lua_State *L, int ev) {
    switch (ev) {
    case TF_EVENT_PRESS: lua_pushliteral(L, "PRESS"); break;
    case TF_EVENT_REPEAT: lua_pushliteral(L, "REPEAT"); break;
    case TF_EVENT_RELEASE: lua_pushliteral(L, "RELEASE"); break;
    case TF_EVENT_DRAG: lua_pushliteral(L, "DRAG"); break;
    case TF_EVENT_UNKNOWN: lua_pushliteral(L, "UNKNOWN"); break;
    }
    return 1;
}

static int Ltf_key(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    return ltf_pushtype(L, st->key.type);
}

static int Ltf_data(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    switch (st->key.type) {
    case TF_TYPE_UNICODE:
        lua_pushstring(L, st->key.utf8);
        lua_pushinteger(L, st->key.d.codepoint);
        return 2;
    case TF_TYPE_FUNCTION: return lua_pushinteger(L, st->key.d.number), 1;
    case TF_TYPE_KEYSYM: {
        const char *nm = tf_name(st->key.d.sym);
        if (nm)
            lua_pushstring(L, nm);
        else
            lua_pushnil(L);
        lua_pushinteger(L, st->key.d.sym);
        return 2;
    }
    case TF_TYPE_MOUSE: {
        int ev, btn, line, col;
        assert(st->key.type == TF_TYPE_MOUSE);
        tf_mouse(&st->key, &ev, &btn, &line, &col);
        ltf_pushevent(L, ev);
        lua_pushinteger(L, btn);
        lua_pushinteger(L, line);
        lua_pushinteger(L, col);
        return 4;
    }
    case TF_TYPE_POSITION: {
        int line, col;
        assert(st->key.type == TF_TYPE_POSITION);
        tf_position(&st->key, &line, &col);
        lua_pushinteger(L, line);
        lua_pushinteger(L, col);
        return 2;
    }
    case TF_TYPE_MODEREPORT: {
        int init, mode, val;
        assert(st->key.type == TF_TYPE_MODEREPORT);
        tf_modereport(&st->key, &init, &mode, &val);
        lua_pushinteger(L, init);
        lua_pushinteger(L, mode);
        lua_pushinteger(L, val);
        return 3;
    }
    case TF_TYPE_KITTYREPORT: return lua_pushinteger(L, st->key.d.number), 1;
    case TF_TYPE_UNKNOWN_CSI: {
        int args[16];
        int nargs, cmd, i;
        nargs = tf_csi(&st->S, args, 16, &cmd);
        if (nargs < 0)
            return luaL_error(L, "termfeed: bad CSI sequence");
        lua_createtable(L, nargs, 3);
        if ((cmd >> 16))
            lua_pushinteger(L, cmd >> 16), lua_setfield(L, -2, "intermediate");
        if (((cmd >> 8) & 0xFF))
            lua_pushinteger(L, (cmd >> 8) & 0xFF),
                    lua_setfield(L, -2, "initial");
        lua_pushinteger(L, cmd & 0xFF), lua_setfield(L, -2, "cmd");
        for (i = 0; i < nargs; ++i)
            lua_pushinteger(L, args[i]), lua_rawseti(L, -2, i + 1);
        return 1;
    }
    case TF_TYPE_DCS:
    case TF_TYPE_OSC:
    case TF_TYPE_APC: {
        int         len;
        const char *s = tf_string(&st->S, &len);
        assert(s); /* control-string key implies a payload buffer */
        return lua_pushlstring(L, s, (size_t)len), 1;
    }
    default: return 0; /* TF_TYPE_NONE: no payload */
    }
}

static int Ltf_mod(lua_State *L) {
    ltf_State  *st = ltf_check(L, 1);
    const char *m = luaL_optstring(L, 2, "");
    if (*m == 0) {
        char ms[8];
        int  len = 0;
        if ((st->key.modifiers & TF_MOD_SHIFT)) ms[len++] = 'S';
        if ((st->key.modifiers & TF_MOD_ALT)) ms[len++] = 'A';
        if ((st->key.modifiers & TF_MOD_CTRL)) ms[len++] = 'C';
        if ((st->key.modifiers & TF_MOD_SUPER)) ms[len++] = 'D';
        if ((st->key.modifiers & TF_MOD_META)) ms[len++] = 'T';
        if ((st->key.modifiers & TF_MOD_HYPER)) ms[len++] = 'H';
        return lua_pushlstring(L, ms, (size_t)len), 1;
    }
    if (*m == 'S' || *m == 's')
        return lua_pushboolean(L, (st->key.modifiers & TF_MOD_SHIFT)), 1;
    if (*m == 'A' || *m == 'a' || *m == 'M' || *m == 'm')
        return lua_pushboolean(L, (st->key.modifiers & TF_MOD_ALT)), 1;
    if (*m == 'C' || *m == 'c')
        return lua_pushboolean(L, (st->key.modifiers & TF_MOD_CTRL)), 1;
    if (*m == 'D' || *m == 'd')
        return lua_pushboolean(L, (st->key.modifiers & TF_MOD_SUPER)), 1;
    if (*m == 'T' || *m == 't')
        return lua_pushboolean(L, (st->key.modifiers & TF_MOD_META)), 1;
    if (*m == 'H' || *m == 'h')
        return lua_pushboolean(L, (st->key.modifiers & TF_MOD_HYPER)), 1;
    return 0;
}

static int Ltf_format(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    int        fmt = (int)luaL_optinteger(L, 2, 0);
    char       buf[TF_FMT_SZ];
    tf_format(buf, sizeof(buf), &st->key, fmt);
    return lua_pushstring(L, buf), 1;
}

static int Ltf_parse(lua_State *L) {
    ltf_State  *st = ltf_check(L, 1);
    const char *s = luaL_checkstring(L, 2);
    int         n = tf_parse(s, &st->key);
    if (n < 0) return 0;
    return lua_pushboolean(L, 1), lua_pushinteger(L, n), 2;
}

static int Ltf_string(lua_State *L) {
    ltf_State  *st = ltf_check(L, 1);
    int         len;
    const char *s = tf_string(&st->S, &len);
    if (!s) return lua_pushliteral(L, ""), 1;
    return lua_pushlstring(L, s, (size_t)len), 1;
}

/* ---- module functions ---- */

static int Ltf_name(lua_State *L) {
    const char *n = tf_name((int)luaL_checkinteger(L, 1));
    if (!n) return 0;
    return lua_pushstring(L, n), 1;
}

static int Ltf_sym(lua_State *L) {
    int r = tf_sym(luaL_checkstring(L, 1));
    if (r < 0) return 0;
    return lua_pushinteger(L, r), 1;
}

/* ---- module registration ---- */

LUALIB_API int luaopen_termfeed(lua_State *L) {
    luaL_Reg libs[] = {
            {"__gc", Ltf_delete},
#define ENTRY(name) {#name, Ltf_##name}
            ENTRY(new),           ENTRY(delete), ENTRY(raw),   ENTRY(cooked),
            ENTRY(setflag),       ENTRY(load),   ENTRY(feed),  ENTRY(readkey),
            ENTRY(waitkey),       ENTRY(flush),  ENTRY(key),   ENTRY(data),
            ENTRY(mod),           ENTRY(format), ENTRY(parse), ENTRY(string),
#undef ENTRY
            {NULL, NULL}};
    if (luaL_newmetatable(L, LTF_STATE_TYPE)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
#define LTF_FLAG(x) \
    lua_pushinteger(L, TF_FLAG_##x), lua_setfield(L, -2, "FLAG_" #x)
    LTF_FLAG(KEEPC0);
    LTF_FLAG(CONVERTKP);
    LTF_FLAG(SPACESYMBOL);
    LTF_FLAG(DELBS);
#undef LTF_FLAG
#define LTF_FMT(x) \
    lua_pushinteger(L, TF_FMT_##x), lua_setfield(L, -2, "FORMAT_" #x)
    LTF_FMT(LONGMOD);
    LTF_FMT(CARETCTRL);
    LTF_FMT(ALTISMETA);
    LTF_FMT(WRAPBRACKET);
    LTF_FMT(SPACEMOD);
    LTF_FMT(LOWERMOD);
    LTF_FMT(LOWERSPACE);
#undef LTF_FMT
    lua_pushinteger(L, TF_FMT_WRAPBRACKET | TF_FMT_ALTISMETA);
    lua_setfield(L, -2, "FORMAT_VIM");
    lua_pushcfunction(L, Ltf_name);
    lua_setfield(L, -2, "name");
    lua_pushcfunction(L, Ltf_sym);
    lua_setfield(L, -2, "sym");
    return 1;
}
