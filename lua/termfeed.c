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
#else
# include <io.h>
# include <windows.h>
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
    int        rawfd;      /* raw() fd, -1 = not in raw mode */
#ifndef _WIN32
    struct termios oldtio;
#else
    DWORD oldmode;
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
    memset(st, 0, sizeof(*st));
    st->L = L;
    st->lookup_ref = LUA_NOREF;
    st->rawfd = -1;
    tf_init(&st->S, NULL, NULL);
    return luaL_setmetatable(L, LTF_STATE_TYPE), 1;
}

static void ltf_restore(ltf_State *st) {
    if (st->rawfd >= 0) {
#ifdef _WIN32
        intptr_t ih = _get_osfhandle(st->rawfd);
        if (ih != -1) SetConsoleMode((HANDLE)ih, st->oldmode);
#else
        tcsetattr(st->rawfd, TCSANOW, &st->oldtio);
#endif
        st->rawfd = -1;
    }
}

static int Ltf_delete(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    ltf_freelookup(L, st);
    ltf_restore(st);
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
static int Ltf_raw(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    int        fd = (int)luaL_optinteger(L, 2, 0);
    intptr_t   ih = _get_osfhandle(fd);
    HANDLE     h;
    DWORD      mode;
    if (ih == -1)
        return luaL_error(L, "termfeed: _get_osfhandle(%d) failed", fd);
    h = (HANDLE)ih;
    if (!GetConsoleMode(h, &st->oldmode))
        return luaL_error(L, "termfeed: GetConsoleMode(%d) failed", fd);
    mode = st->oldmode;
    mode &= (DWORD) ~(
            ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    if (!SetConsoleMode(h, mode))
        return luaL_error(L, "termfeed: SetConsoleMode(%d) failed", fd);
    st->rawfd = fd;
    return lua_settop(L, 1), 1;
}

static int Ltf_cooked(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    return ltf_restore(st), lua_settop(L, 1), 1;
}
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

#ifdef _WIN32

static const struct {
    int    vk;
    tf_Sym sym;
} ltf_console_syms[] = {
        {VK_UP, TF_SYM_UP},         {VK_DOWN, TF_SYM_DOWN},
        {VK_RIGHT, TF_SYM_RIGHT},   {VK_LEFT, TF_SYM_LEFT},
        {VK_HOME, TF_SYM_HOME},     {VK_END, TF_SYM_END},
        {VK_INSERT, TF_SYM_INSERT}, {VK_DELETE, TF_SYM_DELETE},
        {VK_PRIOR, TF_SYM_PAGEUP},  {VK_NEXT, TF_SYM_PAGEDOWN},
};

static const struct {
    int vk;
    int fn;
} ltf_console_fns[] = {
        {VK_F1, 1}, {VK_F2, 2},   {VK_F3, 3},   {VK_F4, 4},
        {VK_F5, 5}, {VK_F6, 6},   {VK_F7, 7},   {VK_F8, 8},
        {VK_F9, 9}, {VK_F10, 10}, {VK_F11, 11}, {VK_F12, 12},
};

static tf_Sym ltf_console_sym(int vk) {
    int i, n = (int)(sizeof(ltf_console_syms) / sizeof(ltf_console_syms[0]));
    for (i = 0; i < n; ++i)
        if (ltf_console_syms[i].vk == vk) return ltf_console_syms[i].sym;
    return TF_SYM_NONE;
}

static int ltf_console_fn(int vk) {
    int i, n = (int)(sizeof(ltf_console_fns) / sizeof(ltf_console_fns[0]));
    for (i = 0; i < n; ++i)
        if (ltf_console_fns[i].vk == vk) return ltf_console_fns[i].fn;
    return 0;
}

static int ltf_console_special(ltf_State *st, int vk) {
    tf_Key *key = &st->key;
    if (vk == VK_BACK) {
        tf_Sym sym = TF_SYM_DELETE;
        if (st->S.flags & TF_FLAG_DELBS) sym = TF_SYM_BACKSPACE;
        return tfK_keysym(key, sym), 1;
    }
    if (vk == VK_TAB) return tfK_keysym(key, TF_SYM_TAB), 1;
    if (vk == VK_RETURN) return tfK_keysym(key, TF_SYM_ENTER), 1;
    if (vk == VK_ESCAPE) return tfK_keysym(key, TF_SYM_ESCAPE), 1;
    if (vk == VK_SPACE) {
        if (st->S.flags & TF_FLAG_SPACESYMBOL)
            tfK_keysym(key, TF_SYM_SPACE);
        else
            tfK_codepoint(key, ' ');
        return 1;
    }
    return 0;
}

static int ltf_console_key(ltf_State *st, const KEY_EVENT_RECORD *ke) {
    DWORD   stt = ke->dwControlKeyState;
    DWORD   am = (DWORD)LEFT_ALT_PRESSED | (DWORD)RIGHT_ALT_PRESSED;
    DWORD   cm = (DWORD)LEFT_CTRL_PRESSED | (DWORD)RIGHT_CTRL_PRESSED;
    int     alt = (stt & am) != 0;
    int     ctl = (stt & cm) != 0;
    WCHAR   wc = ke->uChar.UnicodeChar;
    tf_Sym  sym = ltf_console_sym(ke->wVirtualKeyCode);
    int     fn = ltf_console_fn(ke->wVirtualKeyCode);
    tf_Key *key = &st->key;
    memset(key, 0, sizeof(*key));
    key->event = TF_EVENT_PRESS;
    if (alt) key->modifiers |= TF_MOD_ALT;
    if (ctl) key->modifiers |= TF_MOD_CTRL;
    if (sym != TF_SYM_NONE) return tfK_keysym(key, sym), 1;
    if (fn) return tfK_function(key, fn), 1;
    if (ltf_console_special(st, ke->wVirtualKeyCode)) return 1;
    if (wc) {
        int cp = (int)wc;
        if (ctl) {
            if (cp >= 'A' && cp <= 'Z')
                cp = cp + 0x20;
            else if (cp >= 1 && cp <= 26)
                cp = cp + 'a' - 1;
        }
        return tfK_codepoint(key, cp), 1;
    }
    return 0;
}

static int ltf_console_wait(ltf_State *st, int fd, int timeout_ms) {
    intptr_t     ih = _get_osfhandle(fd);
    HANDLE       h;
    DWORD        ms = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    DWORD        mode, n = 0, wr;
    INPUT_RECORD rec;
    if (ih == -1) return TF_ERRPARAM;
    if (h = (HANDLE)ih, !GetConsoleMode(h, &mode)) return TF_ERRPARAM;
    for (;;) {
        wr = WaitForSingleObject(h, ms);
        if (wr == (DWORD)WAIT_TIMEOUT) return TF_AGAIN;
        if (wr != (DWORD)WAIT_OBJECT_0) return TF_ERRPARAM;
        if (!ReadConsoleInputW(h, &rec, 1, &n) || n == 0u) return TF_ERRPARAM;
        if (rec.EventType != (WORD)KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
            continue;
        if (ltf_console_key(st, &rec.Event.KeyEvent)) return TF_OK;
    }
}

#endif

static int Ltf_waitkey(lua_State *L) {
    ltf_State *st = ltf_check(L, 1);
    int        fd = (int)luaL_optinteger(L, 2, 0);
    int        timeout = (int)luaL_optinteger(L, 3, -1);
#ifndef _WIN32
    int r = tf_waitkey(&st->S, fd, timeout, &st->key);
#else
    int r = ltf_console_wait(st, fd, timeout);
#endif
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
        if (nargs < 0) return luaL_error(L, "termfeed: bad CSI sequence");
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
    lua_pushcfunction(L, Ltf_name), lua_setfield(L, -2, "name");
    lua_pushcfunction(L, Ltf_sym), lua_setfield(L, -2, "sym");
#ifndef WIN32
    lua_pushliteral(L, "POSIX");
#else
    lua_pushliteral(L, "Windows");
#endif
    lua_setfield(L, -2, "platform");
    return 1;
}
