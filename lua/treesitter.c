#define LUA_LIB
#include <assert.h>
#include <lua.h>
#include <lauxlib.h>

#include <stdio.h>
#include <string.h>
#include <tree_sitter/api.h>

#define lts_TSlice             "tree_sitter.Slice"
#define lts_TParser            "tree_sitter.Parser"
#define lts_TLanguage          "tree_sitter.Language"
#define lts_TTree              "tree_sitter.Tree"
#define lts_TNode              "tree_sitter.Node"
#define lts_TTreeCursor        "tree_sitter.TreeCursor"
#define lts_TQuery             "tree_sitter.Query"
#define lts_TQueryCursor       "tree_sitter.QueryCursor"
#define lts_TLookaheadIterator "tree_sitter.LookaheadIterator"

typedef struct lts_Slice       lts_Slice;
typedef struct lts_Parser      lts_Parser;
typedef struct lts_TreeCursor  lts_TreeCursor;
typedef struct lts_QueryCursor lts_QueryCursor;

/* compat layer (ref: lua-protobuf pb.c, lua/piecetab.c) */

static int lts_relindex(int idx, int offset) {
    return idx < 0 && idx > LUA_REGISTRYINDEX ? idx - offset : idx;
}

#if LUA_VERSION_NUM < 502
# define lua_rawlen               lua_objlen
# define luaL_len(L, idx)         lua_objlen(L, idx)
# define lua_setuservalue(L, idx) lua_setfenv(L, idx)
# define lua_getuservalue(L, idx) lua_getfenv(L, idx)
# if !defined(LUA_GCISRUNNING) /* LuaJIT 2.1 has its own luaL_setfuncs
                                * (with nup support); its luaL_register
                                * drops nup and targets -(nup+2) */
#  define luaL_setfuncs(L, l, n)  (assert(n == 0), luaL_register(L, NULL, l))
# endif
# define luaL_setmetatable(L, name) \
    (luaL_getmetatable((L), (name)), lua_setmetatable(L, -2))
# define luaL_typeerror(L, n, t)  luaL_typerror(L, n, t)

static void lua_rawgetp(lua_State *L, int idx, const void *p) {
    lua_pushlightuserdata(L, (void *)p);
    lua_rawget(L, lts_relindex(idx, 1));
}

static void lua_rawsetp(lua_State *L, int idx, const void *p) {
    lua_pushlightuserdata(L, (void *)p);
    lua_insert(L, -2);
    lua_rawset(L, lts_relindex(idx, 1));
}

# ifndef LUA_GCISRUNNING /* not LuaJIT 2.1 */
#  define luaL_newlib(L, l) (lua_newtable(L), luaL_register(L, NULL, l))
# endif
# define lua_warning(...) ((void)0)
#endif /* LUA_VERSION_NUM < 502 */

/* utils */

#if LUA_VERSION_NUM >= 503
#define lua53_rawgetp lua_rawgetp
#define lua53_rawget  lua_rawget
#else  /* not Lua 5.3 */
static int lua53_rawgetp(lua_State *L, int idx, const void *p) {
    return lua_rawgetp(L, idx, p), lua_type(L, -1);
}
static int lua53_rawget(lua_State *L, int idx) {
    return lua_rawget(L, idx), lua_type(L, -1);
}
#endif /* LUA_VERSION_NUM >= 503 */

#if LUA_VERSION_NUM < 504
#define luaL_pushfail(L) lua_pushnil(L)
#endif

#define lts_returnself(L) do { return (lua_settop(L, 1), 1); } while (0)
#define lts_checkenum(L,e,arr,tname)                      do { \
    if ((e) < 0 || (e) >= sizeof(arr)/sizeof((arr)[0]))        \
        return luaL_error(L, "invalid " #tname ": %d", (e)); } while (0)

static int lts_argferror(lua_State *L, int idx, const char *fmt, ...) {
    va_list l;
    va_start(l, fmt);
    lua_pushvfstring(L, fmt, l);
    va_end(l);
    return luaL_argerror(L, idx, lua_tostring(L, -1));
}

static void lts_pushptrbox(lua_State *L) {
    const void *ptrbox = ((const void*)(ptrdiff_t)0xBEEFB775);
    if (lua53_rawgetp(L, LUA_REGISTRYINDEX, ptrbox) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_createtable(L, 0, 1);
        lua_createtable(L, 0, 1);
        lua_pushliteral(L, "v");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_rawsetp(L, LUA_REGISTRYINDEX, ptrbox);
    }
}

static int lts_fieldedindex(lua_State *L) {
    if (lua_getmetatable(L, 1)) {
        lua_pushvalue(L, 2);
        if (lua53_rawget(L, -2) != LUA_TNIL)
            return 1;
        lua_pop(L, 2);
    }
    lua_pushvalue(L, 2);
    if (lua53_rawget(L, lua_upvalueindex(1)) == LUA_TFUNCTION) {
        lua_CFunction f = lua_tocfunction(L, -1);
        lua_settop(L, 2);
        if (f != NULL) return f(L);
    }
    return 0;
}

static int lts_fieldednewindex(lua_State *L) {
    lua_pushvalue(L, 2);
    if (lua53_rawget(L, lua_upvalueindex(1)) == LUA_TFUNCTION) {
        lua_CFunction f = lua_tocfunction(L, -1);
        lua_settop(L, 3);
        if (f != NULL) return f(L);
    }
    return 0;
}

static int lts_retrieve(lua_State *L, const void *p, const char *tname) {
    lts_pushptrbox(L); /* 1 */
    if (lua53_rawgetp(L, -1, p) != LUA_TUSERDATA) { /* 2 */
        if (tname == NULL) return lua_pop(L, 2), 0;
        *(const void**)lua_newuserdata(L, sizeof(const void*)) = p; /* 3 */
        luaL_setmetatable(L, tname);
        lua_copy(L, -1, -2); /* 3 -> 2 */
        lua_rawsetp(L, -3, p); /* (3) */
    }
    return lua_remove(L, -2), 1;
}

static int lts_pushnode(lua_State *L, int treeidx, TSNode node) {
    if (ts_node_is_null(node))
        return 0;
    lts_pushptrbox(L); /* 1 */
    /* or 1 for distinct with tree (has same address with root node) */
    const void *nodeid = (const void*)((ptrdiff_t)node.id|1);
    if (lua53_rawgetp(L, -1, nodeid) != LUA_TUSERDATA) { /* 2 */
        *(TSNode*)lua_newuserdata(L, sizeof(TSNode)) = node; /* 3 */
        luaL_setmetatable(L, lts_TNode);
        lua_copy(L, -1, -2); /* 3 -> 2 */
        lua_rawsetp(L, -3, nodeid); /* (3) */
        lua_pushvalue(L, lts_relindex(treeidx, 2)); /* 3 */
        lua_setuservalue(L, -2); /* (3) */
    }
    return lua_remove(L, -2), 1;
}

static TSNode *lts_checknode(lua_State *L, int idx) {
    TSNode *pn = (TSNode*)luaL_checkudata(L, idx, lts_TNode);
    luaL_argcheck(L, (pn->id != NULL), idx, "null node");
    return pn;
}

static int lts_pushindex(lua_State *L, uint32_t child_idx)
{ return lua_pushinteger(L, child_idx+1), 1; }

static uint32_t lts_checkindex(lua_State *L, int idx, uint32_t count) {
    lua_Integer i = luaL_checkinteger(L, idx);
    if (i <= 0 || i > count)
        return lts_argferror(L, idx, "invalid index: %d", i);
    return (uint32_t)(i-1);
}

static int lts_pushpoint(lua_State *L, TSPoint p) {
    lts_pushindex(L, p.row);
    lts_pushindex(L, p.column);
    return 2;
}

static TSPoint lts_checkpoint(lua_State *L, int idx) {
    TSPoint p;
    p.row    = lts_checkindex(L, idx+0, UINT32_MAX);
    p.column = lts_checkindex(L, idx+1, UINT32_MAX);
    return p;
}

static int lts_pushsymbol(lua_State *L, TSSymbol symbol)
{ return lua_pushinteger(L, symbol+1), 1; }

static TSSymbol lts_checksymbol(lua_State *L, int idx, const TSLanguage *l) {
    int type = lua_type(L, idx);
    TSSymbol symbol;
    if (type == LUA_TSTRING) {
        size_t len;
        const char *s = luaL_checklstring(L, idx, &len);
        int is_named = lua_toboolean(L, 3);
        symbol = ts_language_symbol_for_name(l, s, (uint32_t)len, is_named);
    } else if (type == LUA_TNUMBER) {
        lua_Integer i = luaL_checkinteger(L, idx);
        if (i <= 0 || i > ts_language_symbol_count(l)) {
            (void)lts_argferror(L, idx, "invalid symbol: %d", i);
            return 0;
        }
        symbol = (TSSymbol)(i-1);
    } else {
        (void)luaL_typeerror(L, idx, "number/string");
        return 0;
    }
    return symbol;
}

static int lts_pushfieldid(lua_State *L, TSFieldId field_id)
{ return lua_pushinteger(L, field_id+1), 1; }

static TSFieldId lts_checkfieldid(lua_State *L, int idx, const TSLanguage *l) {
    lua_Integer i = luaL_checkinteger(L, idx);
    if (i <= 0 || i > ts_language_field_count(l)) {
        (void)lts_argferror(L, idx, "invalid TSFieldId: %d", i);
        return 0;
    }
    return (TSFieldId)(i-1);
}

static int lts_pushstateid(lua_State *L, TSStateId state_id)
{ return lua_pushinteger(L, state_id+1), 1; }

static TSStateId lts_checkstateid(lua_State *L, int idx, const TSLanguage *l) {
    lua_Integer i = luaL_checkinteger(L, idx);
    if (i <= 0 || i > ts_language_state_count(l)) {
        (void)lts_argferror(L, idx, "invalid TSStateId: %d", i);
        return 0;
    }
    return (TSStateId)(i-1);
}

static TSInputEdit lts_checkinputedit(lua_State *L, int idx) {
    TSInputEdit edit;
    edit.start_byte           = lts_checkindex(L, idx+0, UINT32_MAX);
    edit.old_end_byte         = lts_checkindex(L, idx+1, UINT32_MAX);
    edit.new_end_byte         = lts_checkindex(L, idx+2, UINT32_MAX);
    edit.start_point.row      = lts_checkindex(L, idx+3, UINT32_MAX);
    edit.start_point.column   = lts_checkindex(L, idx+4, UINT32_MAX);
    edit.old_end_point.row    = lts_checkindex(L, idx+5, UINT32_MAX);
    edit.old_end_point.column = lts_checkindex(L, idx+6, UINT32_MAX);
    edit.new_end_point.row    = lts_checkindex(L, idx+7, UINT32_MAX);
    edit.new_end_point.column = lts_checkindex(L, idx+8, UINT32_MAX);
    return edit;
}

static TSParser *lts_checkparser(lua_State *L, int idx) {
    TSParser **pp = luaL_checkudata(L, idx, lts_TParser);
    luaL_argcheck(L, (*pp != NULL), idx, "null parser");
    return *pp;
}

static TSTree *lts_checktree(lua_State *L, int idx) {
    TSTree **pt = luaL_checkudata(L, idx, lts_TTree);
    luaL_argcheck(L, (*pt != NULL), idx, "null tree");
    return *pt;
}

static TSTree *lts_opttree(lua_State *L, int idx, TSTree *t)
{ return lua_isnoneornil(L, idx) ? t : lts_checktree(L, idx); }

static TSTreeCursor *lts_checktreecursor(lua_State *L, int idx) {
    TSTreeCursor *ptc = (TSTreeCursor*)luaL_checkudata(L, idx, lts_TTreeCursor);
    luaL_argcheck(L, (ptc->id != NULL), idx, "null tree cursor");
    return ptc;
}

static TSQuery *lts_checkquery(lua_State *L, int idx) {
    TSQuery **pq = (TSQuery**)luaL_checkudata(L, idx, lts_TQuery);
    luaL_argcheck(L, (*pq != NULL), idx, "null query");
    return *pq;
}

static lts_QueryCursor *lts_checkquerycursor(lua_State *L, int idx) {
    TSQueryCursor **pqc = (TSQueryCursor**)
        luaL_checkudata(L, idx, lts_TQueryCursor);
    luaL_argcheck(L, (*pqc != NULL), idx, "null query cursor");
    return (lts_QueryCursor*)pqc;
}

static const TSLanguage *lts_checklanguage(lua_State *L, int idx) {
    const TSLanguage **pl = luaL_checkudata(L, idx, lts_TLanguage);
    luaL_argcheck(L, (*pl != NULL), idx, "null language");
    return *pl;
}

static const TSLanguage *lts_optlanguage(lua_State *L, int idx, const TSLanguage *l)
{ return lua_isnoneornil(L, idx) ? l : lts_checklanguage(L, idx); }

static TSLookaheadIterator *lts_checklookaheaditerator(lua_State *L, int idx) {
    TSLookaheadIterator **pi = luaL_checkudata(L, idx, lts_TLookaheadIterator);
    luaL_argcheck(L, (*pi != NULL), idx, "null lookahead iterator");
    return *pi;
}

/* slice */

struct lts_Slice {
    const char *tname;
    void      (*free)  (lua_State *L, lts_Slice *s);
    int       (*index) (lua_State *L, lts_Slice *s, uint32_t idx);
    const void *content;
    uint32_t    count;
};

static int lts_newslice(lua_State *L, lts_Slice slice) {
    lts_Slice *s = (lts_Slice*)lua_newuserdata(L, sizeof(lts_Slice));
    *s = slice;
    luaL_setmetatable(L, lts_TSlice);
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushvalue(L, -3);
    return 4;
}

static int LtsS_delete(lua_State *L) {
    lts_Slice *s = (lts_Slice*)luaL_checkudata(L, 1, lts_TSlice);
    if (s->content != NULL) {
        if (s->free) s->free(L, s);
        memset(s, 0, sizeof(lts_Slice));
    }
    return 0;
}

static int LtsS_tostring(lua_State *L) {
    lts_Slice *s = (lts_Slice*)luaL_checkudata(L, 1, lts_TSlice);
    return lua_pushfstring(L, "%sSlice: %p[%d]",
            s->tname, s->content, s->count), 1;
}

static int LtsS_len(lua_State *L) {
    lts_Slice *s = (lts_Slice*)luaL_checkudata(L, 1, lts_TSlice);
    return lua_pushinteger(L, s->count), 1;
}

static int LtsS_index(lua_State *L) {
    int type = lua_type(L, 2);
    if (type == LUA_TNUMBER) {
        lts_Slice *s = (lts_Slice*)luaL_checkudata(L, 1, lts_TSlice);
        uint32_t idx = (uint32_t)luaL_optinteger(L, 2, 1);
        if (idx > s->count) return 0;
        return s->index(L, s, idx-1);
    }
    if (lua_getmetatable(L, 1)) {
        lua_pushvalue(L, 2);
        lua_rawget(L, -2);
        return 1;
    }
    return 0;
}

static int LtsS_next(lua_State *L) {
    lts_Slice *s = (lts_Slice*)luaL_checkudata(L, 1, lts_TSlice);
    uint32_t idx = (uint32_t)luaL_optinteger(L, 3, 0)+1;
    if (idx > s->count) return 0;
    return s->index(L, s, idx-1);
}

static void open_slice(lua_State *L) {
    luaL_Reg libs[] = {
        { "__gc",       LtsS_delete   },
        { "__close",    LtsS_delete   },
        { "__index",    LtsS_index    },
        { "__len",      LtsS_len      },
        { "__call",     LtsS_next     },
        { "__tostring", LtsS_tostring },
#define ENTRY(name) { #name, LtsS_##name }
        ENTRY(delete),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, lts_TSlice))
        luaL_setfuncs(L, libs, 0);
    lua_pop(L, 1);
}

/* parser */

struct lts_Parser {
    TSParser *p;
    int logger_ref;
    lua_State *L;
};

static void lts_freerange(lua_State *L, lts_Slice *s) { (void)L, free((void *)s->content); }

static int lts_indexrange(lua_State *L, lts_Slice *s, uint32_t idx) {
    const TSRange *r = (const TSRange*)s->content;
    lts_pushindex(L, r[idx].start_byte);
    lts_pushindex(L, r[idx].end_byte);
    lts_pushindex(L, r[idx].start_point.row);
    lts_pushindex(L, r[idx].start_point.column);
    lts_pushindex(L, r[idx].end_point.row);
    lts_pushindex(L, r[idx].end_point.column);
    return 6;
}

static int lts_newrangeslice(lua_State *L, const TSRange *range, uint32_t count) {
    lts_Slice s = { "TSRange", lts_freerange, lts_indexrange, range, count };
    return lts_newslice(L, s);
}

static int LtsP_new(lua_State *L) {
    lts_Parser *p = (lts_Parser*)lua_newuserdata(L, sizeof(lts_Parser));
    memset(p, 0, sizeof(lts_Parser));
    p->p = ts_parser_new();
    p->logger_ref = LUA_REFNIL;
    p->L = L;
    return luaL_setmetatable(L, lts_TParser), 1;
}

static int LtsP_delete(lua_State *L) {
    lts_Parser *p = (lts_Parser*)luaL_checkudata(L, 1, lts_TParser);
    if (p->p != NULL) {
        luaL_unref(L, LUA_REGISTRYINDEX, p->logger_ref);
        ts_parser_delete(p->p);
        p->p = NULL;
        p->logger_ref = LUA_REFNIL;
    }
    return 0;
}

static int LtsP_reset(lua_State *L) {
    TSParser *p = lts_checkparser(L, 1);
    ts_parser_reset(p);
    lts_returnself(L);
}

static int LtsP_print_dot_graphs(lua_State *L) {
    TSParser *p = lts_checkparser(L, 1);
#if LUA_VERSION_NUM < 502
    const char *path = luaL_checkstring(L, 2);
    FILE *f = fopen(path, "wb");
    if (f == NULL) return luaL_error(L, "cannot open '%s'", path);
    ts_parser_print_dot_graphs(p, fileno(f));
    fclose(f);
#else
    luaL_Stream *s = luaL_checkudata(L, 2, LUA_FILEHANDLE);
#ifdef _MSC_VER
    ts_parser_print_dot_graphs(p, _fileno(s->f));
#else
    ts_parser_print_dot_graphs(p, fileno(s->f));
#endif
#endif
    lts_returnself(L);
}

static const char *lts_read(void *payload, uint32_t byte_index, TSPoint position, uint32_t *bytes_read) {
    lua_State *L = (lua_State*)payload;
    lua_pushvalue(L, 3);
    lts_pushindex(L, byte_index);
    lts_pushindex(L, position.row);
    lts_pushindex(L, position.column);
    if (lua_pcall(L, 3, 2, 0) == LUA_OK) {
        size_t len;
        const char *s = luaL_checklstring(L, -2, &len);
        size_t off = (size_t)luaL_optinteger(L, -1, 1);
        if (off > len) off = len + 1;
        *bytes_read = (uint32_t)(len - (off - 1));
        return s + (off - 1);
    }
    lua_warning(L, lua_tostring(L, -1), 0);
    lua_pop(L, 1);
    *bytes_read = 0;
    return NULL;
}

static int LtsP_parse(lua_State *L) {
    TSParser *p = lts_checkparser(L, 1);
    const TSTree *old_tree = lts_opttree(L, 2, NULL);
    const char *encs[] = {"utf8", "utf16", NULL};
    TSInputEncoding enc = (TSInputEncoding)luaL_checkoption(L, 4, "utf8", encs);
    int type = lua_type(L, 3);
    TSTree *tree = NULL;
    if (type == LUA_TSTRING) {
        size_t len;
        const char *s = luaL_checklstring(L, 3, &len);
        tree = ts_parser_parse_string_encoding(p, old_tree, s, (uint32_t)len, enc);
    } else if (type == LUA_TFUNCTION) {
        TSInput input = { L, lts_read, enc, NULL };
        tree = ts_parser_parse(p, old_tree, input);
    } else {
        return luaL_typeerror(L, 3, "string/function");
    }
    return lts_retrieve(L, tree, lts_TTree);
}

static int LtsP_language(lua_State *L) {
    TSParser *p = lts_checkparser(L, 1);
    if (lua_isnone(L, 3)) {
        const TSLanguage *l = ts_parser_language(p);
        return lts_retrieve(L, (const void*)l, lts_TLanguage);
    } else {
        const TSLanguage *l = lts_optlanguage(L, 3, NULL);
        ts_parser_set_language(p, l);
    }
    return 0;
}

static TSLogType ltsP_getlogtype(lua_State *L, int idx) {
    const char *s = luaL_checkstring(L, idx);
    switch (*s) {
    case 'p': case 'P': return TSLogTypeParse;
    case 'l': case 'L': return TSLogTypeLex;
    }
    return lts_argferror(L, idx, "bad TSLogType: %s", s);
}

static int ltsP_calllogger(lua_State *L) {
    typedef void LogF(void *, TSLogType, const char *);
    void *payload = lua_touserdata(L, lua_upvalueindex(1));
    LogF *logf = (LogF*)lua_touserdata(L, lua_upvalueindex(2));
    logf(payload, ltsP_getlogtype(L, 1), luaL_checkstring(L, 2));
    return 0;
}

static void ltsP_dologger(void *payload, TSLogType lt, const char *s) {
    lts_Parser *p = (lts_Parser*)payload;
    lua_State *L = p->L;
    if (L == NULL) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, p->logger_ref);
    lua_pushstring(L, lt ? "lex" : "parse");
    lua_pushstring(L, s);
    if (lua_pcall(L, 2, 0, 0) != LUA_TNIL) {
        lua_warning(L, lua_tostring(L, -1), 0);
        lua_pop(L, 1);
    }
}

static int LtsP_logger(lua_State *L) {
    TSParser *p = lts_checkparser(L, 1);
    int type = lua_type(L, 3);
    TSLogger logger = { NULL, NULL };
    if (type == LUA_TNONE) {
        TSLogger logger = ts_parser_logger(p);
        lua_pushlightuserdata(L, logger.payload);
        lua_pushlightuserdata(L, logger.log);
        return lua_pushcclosure(L, ltsP_calllogger, 2), 1;
    } else if (type != LUA_TNIL) {
        logger.payload = p;
        logger.log = ltsP_dologger;
        luaL_checktype(L, 3, LUA_TFUNCTION);
    }
    return ts_parser_set_logger(p, logger), 0;
}

static int LtsP_included_ranges(lua_State *L) {
    TSParser *p = lts_checkparser(L, 1);
    if (lua_isnone(L, 3)) {
        uint32_t count;
        const TSRange *range = ts_parser_included_ranges(p, &count);
        return lts_newrangeslice(L, range, count);
    } else {
        uint32_t count = 0;
        luaL_Buffer B;
        luaL_buffinit(L, &B);
        luaL_checktype(L, 3, LUA_TFUNCTION);
        for (;;) {
            TSRange r;
            lua_pushvalue(L, 3);
            lua_call(L, 0, 6);
            if (lua_isnil(L, -6)) break;
            r.start_byte         = lts_checkindex(L, -6, UINT32_MAX);
            r.end_byte           = lts_checkindex(L, -5, UINT32_MAX);
            r.start_point.row    = lts_checkindex(L, -4, UINT32_MAX);
            r.start_point.column = lts_checkindex(L, -3, UINT32_MAX);
            r.end_point.row      = lts_checkindex(L, -2, UINT32_MAX);
            r.end_point.column   = lts_checkindex(L, -1, UINT32_MAX);
            *(TSRange*)luaL_prepbuffer(&B) = r;
            lua_pop(L, 6);
            luaL_addsize(&B, sizeof(TSRange));
            count += 1;
        }
        luaL_pushresult(&B);
        ts_parser_set_included_ranges(p,
                (const TSRange*)lua_tostring(L, -1), count);
    }
    return 0;
}

static void open_parser(lua_State *L) {
    luaL_Reg accs[] = {
        { "__index",    lts_fieldedindex    },
        { "__newindex", lts_fieldednewindex },
        { NULL,         NULL                }
    };
    luaL_Reg fields[] = {
#define ENTRY(name) { #name, LtsP_##name }
        ENTRY(language),
        ENTRY(logger),
        ENTRY(included_ranges),
        { NULL, NULL }
    };
    luaL_Reg libs[] = {
        { "__gc",       LtsP_delete },
        { "__close",    LtsP_delete },
        { "__index",    NULL        },
        { "__newindex", NULL        },
        ENTRY(new),
        ENTRY(delete),
        ENTRY(reset),
        ENTRY(parse),
        ENTRY(print_dot_graphs),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, lts_TParser)) {
        luaL_setfuncs(L, libs, 0);
        luaL_newlib(L, fields);
        luaL_setfuncs(L, accs, 1);
    }
    lua_setfield(L, -2, "parser");
}

/* tree */

static int LtsT_delete(lua_State *L) {
    TSTree **pt = (TSTree**)luaL_checkudata(L, 1, lts_TTree);
    if (*pt != NULL) {
        ts_tree_delete(*pt);
        *pt = NULL;
    }
    return 0;
}

static int LtsT_copy(lua_State *L) {
    TSTree *t = lts_checktree(L, 1);
    *(TSTree**)lua_newuserdata(L, sizeof(TSTree*)) = ts_tree_copy(t);
    return luaL_setmetatable(L, lts_TTree), 1;
}

static int LtsT_edit(lua_State *L) {
    TSTree *t = lts_checktree(L, 1);
    TSInputEdit edit = lts_checkinputedit(L, 2);
    ts_tree_edit(t, &edit);
    lts_returnself(L);
}

static int LtsT_root_with_offset(lua_State *L) {
    TSTree *t = lts_checktree(L, 1);
    uint32_t offset_byte = lts_checkindex(L, 2, UINT32_MAX);
    TSPoint offset_extend = lts_checkpoint(L, 3);
    TSNode node = ts_tree_root_node_with_offset(t, offset_byte, offset_extend);
    return lts_pushnode(L, 1, node);
}

static int LtsT_changed_ranges(lua_State *L) {
    TSTree *t        = lts_checktree(L, 1);
    TSTree *old_tree = lts_checktree(L, 2);
    uint32_t length;
    TSRange *range = ts_tree_get_changed_ranges(old_tree, t, &length);
    return lts_newrangeslice(L, range, length);
}

static int LtsT_print_dot_graph(lua_State *L) {
    TSTree *t = lts_checktree(L, 1);
#if LUA_VERSION_NUM < 502
    const char *path = luaL_checkstring(L, 2);
    FILE *f = fopen(path, "wb");
    if (f == NULL) return luaL_error(L, "cannot open '%s'", path);
    ts_tree_print_dot_graph(t, fileno(f));
    fclose(f);
#else
    luaL_Stream *s = luaL_checkudata(L, 2, LUA_FILEHANDLE);
#ifdef _MSC_VER
    ts_tree_print_dot_graph(t, _fileno(s->f));
#else
    ts_tree_print_dot_graph(t, fileno(s->f));
#endif
#endif
    lts_returnself(L);
}

static int LtsT_root(lua_State *L) {
    TSTree *t = lts_checktree(L, 1);
    TSNode node = ts_tree_root_node(t);
    return lts_pushnode(L, 1, node);
}

static int LtsT_language(lua_State *L) {
    TSTree *t = lts_checktree(L, 1);
    return lts_retrieve(L, (const void*)ts_tree_language(t), lts_TLanguage);
}

static int LtsT_included_ranges(lua_State *L) {
    TSTree *t = lts_checktree(L, 1);
    uint32_t length;
    TSRange *range = ts_tree_included_ranges(t, &length);
    return lts_newrangeslice(L, range, length);
}

static void open_tree(lua_State *L) {
    luaL_Reg libs[] = {
        { "__gc",    LtsT_delete },
        { "__close", LtsT_delete },
        { "__index", NULL        },
#define ENTRY(name) { #name, LtsT_##name }
        ENTRY(copy),
        ENTRY(delete),
        ENTRY(edit),
        ENTRY(root_with_offset),
        ENTRY(changed_ranges),
        ENTRY(print_dot_graph),
        { NULL, NULL }
    };
    luaL_Reg fields[] = {
        ENTRY(root),
        ENTRY(language),
        ENTRY(included_ranges),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, lts_TTree)) {
        luaL_setfuncs(L, libs, 0);
        luaL_newlib(L, fields);
        lua_pushcclosure(L, lts_fieldedindex, 1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

/* node */

static int ltsN_index(lua_State *L) {
    int type = lua_type(L, 2);
    if (type == LUA_TNUMBER) {
        TSNode r, *n = lts_checknode(L, 1);
        lua_Integer i = luaL_checkinteger(L, 2);
        if (i > 0 && i <= ts_node_named_child_count(*n)) {
            r = ts_node_named_child(*n, (uint32_t)(i-1));
            return lts_pushnode(L, 1, r);
        }
    }
    return lts_fieldedindex(L);
}

static int LtsN_tostring(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    char *buf = ts_node_string(*n);
    lua_pushstring(L, buf);
    free(buf);
    return 1;
}

static int LtsN_equal(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    TSNode *o = lts_checknode(L, 2);
    return lua_pushboolean(L, ts_node_eq(*n, *o)), 1;
}

static int LtsN_start_point(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushpoint(L, ts_node_start_point(*n));
}

static int LtsN_end_point(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushpoint(L, ts_node_end_point(*n));
}

static int LtsN_child_count(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushinteger(L, ts_node_child_count(*n)), 1;
}

static int LtsN_child(lua_State *L) {
    TSNode r, *n = lts_checknode(L, 1);
    int type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        size_t len;
        const char *s = luaL_checklstring(L, 2, &len);
        r = ts_node_child_by_field_name(*n, s, (uint32_t)len);
    } else if (type == LUA_TNUMBER) {
        r = ts_node_child(*n, lts_checkindex(L, 2, ts_node_child_count(*n)));
    } else {
        return luaL_typeerror(L, 2, "number/string");
    }
    return lts_pushnode(L, 1, r);
}

static int LtsN_named_child_count(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushinteger(L, ts_node_named_child_count(*n)), 1;
}

static int LtsN_named_child(lua_State *L) {
    TSNode r, *n = lts_checknode(L, 1);
    r = ts_node_named_child(*n,
            lts_checkindex(L, 2, ts_node_named_child_count(*n)));
    return lts_pushnode(L, 1, r);
}

static int LtsN_child_by_field_id(lua_State *L) {
    TSNode r, *n = lts_checknode(L, 1);
    r = ts_node_child_by_field_id(*n,
            lts_checkfieldid(L, 2, ts_tree_language(n->tree)));
    return lts_pushnode(L, 1, r);
}

static int LtsN_field_name_for_child(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    const char *s = ts_node_field_name_for_child(*n,
            lts_checkindex(L, 2, ts_node_child_count(*n)));
    return lua_pushstring(L, s), 1;
}

static int LtsN_first_child_for_byte(lua_State *L) {
    TSNode r, *n = lts_checknode(L, 1);
    r = ts_node_first_child_for_byte(*n, lts_checkindex(L, 2, UINT32_MAX));
    return lts_pushnode(L, 1, r);
}

static int LtsN_first_named_child_for_byte(lua_State *L) {
    TSNode r, *n = lts_checknode(L, 1);
    r = ts_node_first_named_child_for_byte(*n, lts_checkindex(L, 2, UINT32_MAX));
    return lts_pushnode(L, 1, r);
}

static int LtsN_descendant_for_byte_range(lua_State *L) {
    TSNode r, *n = lts_checknode(L, 1);
    r = ts_node_descendant_for_byte_range(*n,
            lts_checkindex(L, 2, UINT32_MAX),
            lts_checkindex(L, 3, UINT32_MAX));
    return lts_pushnode(L, 1, r);
}

static int LtsN_descendant_for_point_range(lua_State *L) {
    TSNode r, *n = lts_checknode(L, 1);
    r = ts_node_descendant_for_point_range(*n,
            lts_checkpoint(L, 2), lts_checkpoint(L, 4));
    return lts_pushnode(L, 1, r);
}

static int LtsN_named_descendant_for_byte_range(lua_State *L) {
    TSNode r, *n = lts_checknode(L, 1);
    r = ts_node_named_descendant_for_byte_range(*n,
            lts_checkindex(L, 2, UINT32_MAX),
            lts_checkindex(L, 3, UINT32_MAX));
    return lts_pushnode(L, 1, r);
}

static int LtsN_named_descendant_for_point_range(lua_State *L) {
    TSNode r, *n = lts_checknode(L, 1);
    r = ts_node_named_descendant_for_point_range(*n,
            lts_checkpoint(L, 2), lts_checkpoint(L, 4));
    return lts_pushnode(L, 1, r);
}

static int LtsN_edit(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    TSInputEdit e = lts_checkinputedit(L, 2);
    ts_node_edit(n, &e);
    lts_returnself(L);
}

static int LtsN_tree(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_retrieve(L, n->tree, lts_TTree);
}

static int LtsN_language(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_retrieve(L, ts_node_language(*n), lts_TLanguage);
}

static int LtsN_type(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushstring(L, ts_node_type(*n)), 1;
}

static int LtsN_symbol(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushsymbol(L, ts_node_symbol(*n));
}

static int LtsN_symbol_name(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    const char *s = ts_language_symbol_name(
            ts_node_language(*n), ts_node_symbol(*n));
    return lua_pushstring(L, s), 1;
}

static int LtsN_grammar_type(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushstring(L, ts_node_grammar_type(*n)), 1;
}

static int LtsN_grammar_symbol(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushsymbol(L, ts_node_grammar_symbol(*n));
}

static int LtsN_grammar_symbol_name(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    const char *s = ts_language_symbol_name(
            ts_node_language(*n), ts_node_grammar_symbol(*n));
    return lua_pushstring(L, s), 1;
}

static int LtsN_start_byte(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushindex(L, ts_node_start_byte(*n));
}

static int LtsN_end_byte(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushindex(L, ts_node_end_byte(*n));
}

static int LtsN_is_null(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushboolean(L, ts_node_is_null(*n)), 1;
}

static int LtsN_is_named(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushboolean(L, ts_node_is_named(*n)), 1;
}

static int LtsN_is_missing(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushboolean(L, ts_node_is_missing(*n)), 1;
}

static int LtsN_is_extra(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushboolean(L, ts_node_is_extra(*n)), 1;
}

static int LtsN_has_changes(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushboolean(L, ts_node_has_changes(*n)), 1;
}

static int LtsN_has_error(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushboolean(L, ts_node_has_error(*n)), 1;
}

static int LtsN_is_error(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushboolean(L, ts_node_is_error(*n)), 1;
}

static int LtsN_parse_state(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushstateid(L, ts_node_parse_state(*n));
}

static int LtsN_next_parse_state(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushstateid(L, ts_node_next_parse_state(*n));
}

static int LtsN_parent(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushnode(L, 1, ts_node_parent(*n));
}

static int LtsN_next_sibling(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushnode(L, 1, ts_node_next_sibling(*n));
}

static int LtsN_prev_sibling(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushnode(L, 1, ts_node_prev_sibling(*n));
}

static int LtsN_next_named_sibling(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushnode(L, 1, ts_node_next_named_sibling(*n));
}

static int LtsN_prev_named_sibling(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lts_pushnode(L, 1, ts_node_prev_named_sibling(*n));
}

static int LtsN_descendant_count(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    return lua_pushinteger(L, ts_node_descendant_count(*n)), 1;
}

static int LtsC_new(lua_State *L);

static void open_node(lua_State *L) {
    luaL_Reg libs[] = {
        { "__index",    NULL                   },
        { "__len",      LtsN_named_child_count },
        { "__eq",       LtsN_equal             },
#define ENTRY(name) { #name, LtsN_##name }
        { "cursor", LtsC_new },
        ENTRY(start_point),
        ENTRY(end_point),
        ENTRY(child),
        ENTRY(child_by_field_id),
        ENTRY(named_child),
        ENTRY(field_name_for_child),
        ENTRY(first_child_for_byte),
        ENTRY(first_named_child_for_byte),
        ENTRY(descendant_for_byte_range),
        ENTRY(descendant_for_point_range),
        ENTRY(named_descendant_for_byte_range),
        ENTRY(named_descendant_for_point_range),
        ENTRY(edit),
        ENTRY(tostring),
        ENTRY(equal),
        { NULL, NULL }
    };
    luaL_Reg fields[] = {
        ENTRY(tree),
        ENTRY(language),
        ENTRY(type),
        ENTRY(symbol),
        ENTRY(symbol_name),
        ENTRY(grammar_type),
        ENTRY(grammar_symbol),
        ENTRY(grammar_symbol_name),
        ENTRY(start_byte),
        ENTRY(end_byte),
        ENTRY(is_null),
        ENTRY(is_named),
        ENTRY(is_missing),
        ENTRY(is_extra),
        ENTRY(has_changes),
        ENTRY(has_error),
        ENTRY(is_error),
        ENTRY(parse_state),
        ENTRY(next_parse_state),
        ENTRY(parent),
        ENTRY(child_count),
        ENTRY(named_child_count),
        ENTRY(next_sibling),
        ENTRY(prev_sibling),
        ENTRY(next_named_sibling),
        ENTRY(prev_named_sibling),
        ENTRY(descendant_count),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, lts_TNode)) {
        luaL_setfuncs(L, libs, 0);
        luaL_newlib(L, fields);
        lua_pushcclosure(L, ltsN_index, 1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

/* tree cursor */

struct lts_TreeCursor {
    TSTreeCursor base;
    TSNode       node;
};

static int LtsC_new(lua_State *L) {
    TSNode *n = lts_checknode(L, 1);
    lts_TreeCursor *ltc = (lts_TreeCursor*)
        lua_newuserdata(L, sizeof(lts_TreeCursor));
    ltc->base = ts_tree_cursor_new(*n);
    ltc->node = *n;
    luaL_setmetatable(L, lts_TTreeCursor);
    lua_getuservalue(L, 1);
    lua_setuservalue(L, -2);
    return 1;
}

static int LtsC_copy(lua_State *L) {
    lts_TreeCursor *tc = (lts_TreeCursor*)lts_checktreecursor(L, 1);
    lts_TreeCursor *copied = (lts_TreeCursor*)
        lua_newuserdata(L, sizeof(lts_TreeCursor));
    memset(copied, 0, sizeof(lts_TreeCursor));
    copied->base = ts_tree_cursor_copy(&tc->base);
    copied->node = tc->node;
    luaL_setmetatable(L, lts_TTreeCursor);
    lua_getuservalue(L, 1);
    lua_setuservalue(L, -2);
    return 1;
}

static int LtsC_delete(lua_State *L) {
    lts_TreeCursor *tc = (lts_TreeCursor*)lts_checktreecursor(L, 1);
    if (tc->base.tree != NULL) {
        ts_tree_cursor_delete(&tc->base);
        memset(tc, 0, sizeof(TSTreeCursor));
    }
    return 0;
}

static int LtsC_reset(lua_State *L) {
    TSTreeCursor *tc = lts_checktreecursor(L, 1);
    TSNode *n = lts_checknode(L, 2);
    ts_tree_cursor_reset(tc, *n);
    lua_getuservalue(L, 1);
    lua_setuservalue(L, -2);
    lts_returnself(L);
}

static int LtsC_goto_parent(lua_State *L) {
    TSTreeCursor *tc = lts_checktreecursor(L, 1);
    ts_tree_cursor_goto_parent(tc);
    lts_returnself(L);
}

static int LtsC_goto_next_sibling(lua_State *L) {
    TSTreeCursor *tc = lts_checktreecursor(L, 1);
    ts_tree_cursor_goto_next_sibling(tc);
    lts_returnself(L);
}

static int LtsC_goto_prev_sibling(lua_State *L) {
    TSTreeCursor *tc = lts_checktreecursor(L, 1);
    ts_tree_cursor_goto_previous_sibling(tc);
    lts_returnself(L);
}

static int LtsC_goto_first_child(lua_State *L) {
    TSTreeCursor *tc = lts_checktreecursor(L, 1);
    ts_tree_cursor_goto_first_child(tc);
    lts_returnself(L);
}

static int LtsC_goto_last_child(lua_State *L) {
    TSTreeCursor *tc = lts_checktreecursor(L, 1);
    ts_tree_cursor_goto_last_child(tc);
    lts_returnself(L);
}

static int LtsC_goto_descendant(lua_State *L) {
    lts_TreeCursor *tc = (lts_TreeCursor*)lts_checktreecursor(L, 1);
    ts_tree_cursor_goto_descendant(&tc->base,
            lts_checkindex(L, 2, ts_node_descendant_count(tc->node)));
    lts_returnself(L);
}

static int LtsC_goto_first_child_for_byte(lua_State *L) {
    TSTreeCursor *tc = lts_checktreecursor(L, 1);
    ts_tree_cursor_goto_first_child_for_byte(tc,
            lts_checkindex(L, 2, UINT32_MAX));
    lts_returnself(L);
}

static int LtsC_goto_first_child_for_point(lua_State *L) {
    TSTreeCursor *tc = lts_checktreecursor(L, 1);
    ts_tree_cursor_goto_first_child_for_point(tc, lts_checkpoint(L, 2));
    lts_returnself(L);
}

static int LtsC_node(lua_State *L) {
    lts_TreeCursor *tc = (lts_TreeCursor*)lts_checktreecursor(L, 1);
    TSNode n = ts_tree_cursor_current_node(&tc->base);
    lts_pushnode(L, 1, tc->node);
    return lts_pushnode(L, -1, n);
}

static int LtsC_field(lua_State *L) {
    lts_TreeCursor *tc = (lts_TreeCursor*)lts_checktreecursor(L, 1);
    return lts_pushfieldid(L, ts_tree_cursor_current_field_id(&tc->base)), 1;
}

static int LtsC_field_name(lua_State *L) {
    lts_TreeCursor *tc = (lts_TreeCursor*)lts_checktreecursor(L, 1);
    return lua_pushstring(L, ts_tree_cursor_current_field_name(&tc->base)), 1;
}

static int LtsC_descendant_index(lua_State *L) {
    lts_TreeCursor *tc = (lts_TreeCursor*)lts_checktreecursor(L, 1);
    return lts_pushindex(L, ts_tree_cursor_current_descendant_index(&tc->base));
}

static int LtsC_depth(lua_State *L) {
    lts_TreeCursor *tc = (lts_TreeCursor*)lts_checktreecursor(L, 1);
    return lts_pushindex(L, ts_tree_cursor_current_depth(&tc->base));
}

static void open_tree_cursor(lua_State *L) {
    luaL_Reg libs[] = {
        { "__gc",    LtsC_delete },
        { "__close", LtsC_delete },
        { "__index", NULL        },
#define ENTRY(name) { #name, LtsC_##name }
        ENTRY(new),
        ENTRY(copy),
        ENTRY(delete),
        ENTRY(reset),
        ENTRY(goto_parent),
        ENTRY(goto_next_sibling),
        ENTRY(goto_prev_sibling),
        ENTRY(goto_first_child),
        ENTRY(goto_last_child),
        ENTRY(goto_descendant),
        ENTRY(goto_first_child_for_byte),
        ENTRY(goto_first_child_for_point),
        { NULL, NULL }
    };
    luaL_Reg fields[] = {
        ENTRY(node),
        ENTRY(field),
        ENTRY(field_name),
        ENTRY(descendant_index),
        ENTRY(depth),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, lts_TTreeCursor)) {
        luaL_setfuncs(L, libs, 0);
        luaL_newlib(L, fields);
        lua_pushcclosure(L, lts_fieldedindex, 1);
        lua_setfield(L, -2, "__index");
    }
    lua_setfield(L, -2, "tree_cursor");
}

/* query */

static int LtsV_new(lua_State *L);

static int lts_indexpredicate(lua_State *L, lts_Slice *s, uint32_t idx) {
    static const char *types[] = { "done", "capture", "string" };
    const TSQueryPredicateStep *steps =
        (const TSQueryPredicateStep*)s->content;
    lts_checkenum(L, steps[idx].type, types, TSQueryPredicateStepType);
    lua_pushstring(L, types[steps[idx].type]);
    lts_pushindex(L, steps[idx].value_id);
    return 2;
}

static int lts_newpredicateslice(lua_State *L, const TSQueryPredicateStep *steps, uint32_t count) {
    lts_Slice s = {"TSQueryPredicateStep", NULL,
        lts_indexpredicate, steps, count};
    int r = lts_newslice(L, s);
    lua_pushvalue(L, 1);
    lua_setuservalue(L, -r-1);
    return r;
}

static int LtsQ_new(lua_State *L) {
    static const char *errors[] = {
        "none", "syntax", "node_type", "field",
        "capture", "structure", "language",
    };
    const TSLanguage *l = lts_checklanguage(L, 1);
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);
    uint32_t error_offset;
    TSQueryError error_type;
    TSQuery **pq = (TSQuery**)lua_newuserdata(L, sizeof(TSQuery*));
    *pq = ts_query_new(l, s, (uint32_t)len, &error_offset, &error_type);
    if (*pq == NULL) {
        if (error_type < 0 || error_type >= sizeof(errors)/sizeof(errors[0]))
            return luaL_error(L, "invalid error type: %d", error_type);
        luaL_pushfail(L);
        lua_pushstring(L, errors[error_type]);
        lts_pushindex(L, error_offset);
        return 3;
    }
    luaL_setmetatable(L, lts_TQuery);
    return 1;
}

static int LtsQ_delete(lua_State *L) {
    TSQuery **pq = (TSQuery**)luaL_checkudata(L, 1, lts_TQuery);
    if (*pq != NULL) {
        ts_query_delete(*pq);
        *pq = NULL;
    }
    return 0;
}

static int LtsQ_exec(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    TSNode *n = lts_checknode(L, 2);
    TSQueryCursor *qc;
    LtsV_new(L);
    qc = *(TSQueryCursor**)lua_touserdata(L, -1);
    ts_query_cursor_exec(qc, q, *n);
    lua_pushvalue(L, 2);
    lua_setuservalue(L, -2);
    return 1;
}

static int LtsQ_start_byte_for_pattern(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    uint32_t pattern_idx = lts_checkindex(L, 2, ts_query_pattern_count(q));
    return lts_pushindex(L, ts_query_start_byte_for_pattern(q, pattern_idx));
}

static int LtsQ_predicates_for_pattern(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    uint32_t pattern_idx = lts_checkindex(L, 2, ts_query_pattern_count(q));
    uint32_t step_count;
    const TSQueryPredicateStep *steps = ts_query_predicates_for_pattern(q,
            pattern_idx, &step_count);
    return lts_newpredicateslice(L, steps, step_count);
}

static int LtsQ_is_pattern_rooted(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    uint32_t pattern_idx = lts_checkindex(L, 2, ts_query_pattern_count(q));
    return lua_pushboolean(L, ts_query_is_pattern_rooted(q, pattern_idx)), 1;
}

static int LtsQ_is_pattern_non_local(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    uint32_t pattern_idx = lts_checkindex(L, 2, ts_query_pattern_count(q));
    return lua_pushboolean(L, ts_query_is_pattern_non_local(q, pattern_idx)), 1;
}

static int LtsQ_is_pattern_guaranteed_at_step(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    uint32_t byte_offset = lts_checkindex(L, 2, UINT32_MAX);
    return lua_pushboolean(L,
            ts_query_is_pattern_guaranteed_at_step(q, byte_offset)), 1;
}

static int LtsQ_capture_name_for_id(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    uint32_t index = lts_checkindex(L, 2,
            ts_query_pattern_count(q) + ts_query_string_count(q));
    uint32_t length;
    const char *s = ts_query_capture_name_for_id(q, index, &length);
    return lua_pushlstring(L, s, length), 1;
}

static int LtsQ_capture_quantifier_for_id(lua_State *L) {
    static const char *quantifiers[] = {
        "zero", "zero_or_one", "zero_or_more",
        "one", "one_or_more",
    };
    TSQuery *q = lts_checkquery(L, 1);
    uint32_t pattern_idx = lts_checkindex(L, 2, ts_query_pattern_count(q));
    uint32_t capture_idx = lts_checkindex(L, 3, ts_query_capture_count(q));
    TSQuantifier quant = ts_query_capture_quantifier_for_id(q, pattern_idx, capture_idx);
    if (quant < 0 || quant >= sizeof(quantifiers)/sizeof(quantifiers[0]))
        return luaL_error(L, "invalid quantifier: %d", quant);
    return lua_pushstring(L, quantifiers[quant]), 1;
}

static int LtsQ_string_value_for_id(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    uint32_t index = lts_checkindex(L, 2,
            ts_query_pattern_count(q) + ts_query_string_count(q));
    uint32_t length;
    const char *s = ts_query_string_value_for_id(q, index, &length);
    return lua_pushlstring(L, s, length), 1;
}

static int LtsQ_disable_capture(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);
    ts_query_disable_capture(q, s, (uint32_t)len);
    lts_returnself(L);
}

static int LtsQ_disable_pattern(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    uint32_t pattern_idx = lts_checkindex(L, 2, ts_query_pattern_count(q));
    ts_query_disable_pattern(q, pattern_idx);
    lts_returnself(L);
}

static int LtsQ_pattern_count(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    return lua_pushinteger(L, ts_query_pattern_count(q)), 1;
}

static int LtsQ_capture_count(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    return lua_pushinteger(L, ts_query_capture_count(q)), 1;
}

static int LtsQ_string_count(lua_State *L) {
    TSQuery *q = lts_checkquery(L, 1);
    return lua_pushinteger(L, ts_query_string_count(q)), 1;
}

static void open_query(lua_State *L) {
    luaL_Reg libs[] = {
        { "__gc",    LtsQ_delete },
        { "__close", LtsQ_delete },
        { "__index", NULL        },
#define ENTRY(name) { #name, LtsQ_##name }
        ENTRY(new),
        ENTRY(delete),
        ENTRY(exec),
        ENTRY(start_byte_for_pattern),
        ENTRY(predicates_for_pattern),
        ENTRY(is_pattern_rooted),
        ENTRY(is_pattern_non_local),
        ENTRY(is_pattern_guaranteed_at_step),
        ENTRY(capture_name_for_id),
        ENTRY(capture_quantifier_for_id),
        ENTRY(string_value_for_id),
        ENTRY(disable_capture),
        ENTRY(disable_pattern),
        { NULL, NULL }
    };
    luaL_Reg fields[] = {
        ENTRY(pattern_count),
        ENTRY(capture_count),
        ENTRY(string_count),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, lts_TQuery)) {
        luaL_setfuncs(L, libs, 0);
        luaL_newlib(L, fields);
        lua_pushcclosure(L, lts_fieldedindex, 1);
        lua_setfield(L, -2, "__index");
    }
    lua_setfield(L, -2, "query");
}

/* query cursor */

struct lts_QueryCursor {
    TSQueryCursor *cursor;
    TSQueryMatch   match;
};

static int LtsV_new(lua_State *L) {
    lts_QueryCursor *qc = (lts_QueryCursor*)
        lua_newuserdata(L, sizeof(lts_QueryCursor));
    qc->cursor = ts_query_cursor_new();
    memset(&qc->match, 0, sizeof(TSQueryMatch));
    luaL_setmetatable(L, lts_TQueryCursor);
    return 1;
}

static int LtsV_delete(lua_State *L) {
    lts_QueryCursor *qc = (lts_QueryCursor*)
        luaL_testudata(L, 1, lts_TQueryCursor);
    if (qc->cursor != NULL) {
        ts_query_cursor_delete(qc->cursor);
        qc->cursor = NULL;
    }
    return 0;
}

static int LtsV_index(lua_State *L) {
    int type = lua_type(L, 2);
    if (type == LUA_TNUMBER) {
        lts_QueryCursor *qc = lts_checkquerycursor(L, 1);
        lua_Integer i = luaL_checkinteger(L, 2);
        if (i > 0 && i <= qc->match.capture_count)
            return lts_pushnode(L, 1, qc->match.captures[(uint32_t)(i-1)].node);
    }
    return lts_fieldedindex(L);
}

static int LtsV_len(lua_State *L) {
    lts_QueryCursor *qc = lts_checkquerycursor(L, 1);
    return lua_pushinteger(L, qc->match.capture_count), 1;
}

static int LtsV_exec(lua_State *L) {
    TSQueryCursor *qc = lts_checkquerycursor(L, 1)->cursor;
    TSQuery *q = lts_checkquery(L, 2);
    TSNode *n = lts_checknode(L, 3);
    ts_query_cursor_exec(qc, q, *n);
    lua_pushvalue(L, 3);
    lua_setuservalue(L, 1);
    lts_returnself(L);
}

static int LtsV_set_byte_range(lua_State *L) {
    TSQueryCursor *qc = lts_checkquerycursor(L, 1)->cursor;
    uint32_t start_byte = lts_checkindex(L, 2, UINT32_MAX);
    uint32_t end_byte   = lts_checkindex(L, 3, UINT32_MAX);
    ts_query_cursor_set_byte_range(qc, start_byte, end_byte);
    lts_returnself(L);
}

static int LtsV_set_point_range(lua_State *L) {
    TSQueryCursor *qc = lts_checkquerycursor(L, 1)->cursor;
    TSPoint start_point = lts_checkpoint(L, 2);
    TSPoint end_point = lts_checkpoint(L, 4);
    ts_query_cursor_set_point_range(qc, start_point, end_point);
    lts_returnself(L);
}

static int LtsV_next_match(lua_State *L) {
    lts_QueryCursor *qc = lts_checkquerycursor(L, 1);
    if (!ts_query_cursor_next_match(qc->cursor, &qc->match))
        return lua_pushnil(L), 1;
    lts_returnself(L);
}

static int LtsV_remove_match(lua_State *L) {
    TSQueryCursor *qc = lts_checkquerycursor(L, 1)->cursor;
    uint32_t match_id = lts_checkindex(L, 2, UINT32_MAX);
    ts_query_cursor_remove_match(qc, match_id);
    lts_returnself(L);
}

static int LtsV_next_capture(lua_State *L) {
    lts_QueryCursor *qc = lts_checkquerycursor(L, 1);
    uint32_t capture_index;
    if (!ts_query_cursor_next_capture(qc->cursor, &qc->match, &capture_index))
        return lua_pushnil(L), 1;
    return lts_pushindex(L, capture_index);
}

static int LtsV_captures(lua_State *L) {
    lts_QueryCursor *qc = lts_checkquerycursor(L, 1);
    uint32_t index = lts_checkindex(L, 2, qc->match.capture_count);
    lts_pushnode(L, 1, qc->match.captures[index].node);
    lts_pushindex(L, qc->match.captures[index].index);
    return 2;
}

static int LtsV_max_start_depth(lua_State *L) {
    TSQueryCursor *qc = lts_checkquerycursor(L, 1)->cursor;
    if (lua_isnone(L, 3)) return luaL_error(L, "writeonly property");
    ts_query_cursor_set_max_start_depth(qc, lts_checkindex(L, 3, UINT32_MAX));
    return 0;
}

static int LtsV_did_exceed_match_limit(lua_State *L) {
    TSQueryCursor *qc = lts_checkquerycursor(L, 1)->cursor;
    return lua_pushboolean(L, ts_query_cursor_did_exceed_match_limit(qc)), 1;
}

static int LtsV_match_limit(lua_State *L) {
    TSQueryCursor *qc = lts_checkquerycursor(L, 1)->cursor;
    if (lua_isnone(L, 3)) {
        uint32_t match_limit = ts_query_cursor_match_limit(qc);
        return lua_pushinteger(L, match_limit), 1;
    }
    ts_query_cursor_set_match_limit(qc, (uint32_t)luaL_checkinteger(L, 3));
    return 0;
}

static int LtsV_match_id(lua_State *L) {
    lts_QueryCursor *qc = lts_checkquerycursor(L, 1);
    return lts_pushindex(L, qc->match.id);
}

static int LtsV_pattern_index(lua_State *L) {
    lts_QueryCursor *qc = lts_checkquerycursor(L, 1);
    return lts_pushindex(L, qc->match.pattern_index);
}

static void open_query_cursor(lua_State *L) {
    luaL_Reg accs[] = {
        { "__index",    LtsV_index          },
        { "__newindex", lts_fieldednewindex },
        { NULL,         NULL                }
    };
    luaL_Reg libs[] = {
        { "__gc",    LtsV_delete },
        { "__close", LtsV_delete },
        { "__index", NULL        },
        { "__len",   LtsV_len    },
#define ENTRY(name) { #name, LtsV_##name }
        ENTRY(new),
        ENTRY(delete),
        ENTRY(exec),
        ENTRY(set_byte_range),
        ENTRY(set_point_range),
        ENTRY(next_match),
        ENTRY(remove_match),
        ENTRY(next_capture),
        ENTRY(captures),
        { NULL, NULL }
    };
    luaL_Reg fields[] = {
        ENTRY(max_start_depth),
        ENTRY(did_exceed_match_limit),
        ENTRY(match_limit),
        ENTRY(match_id),
        ENTRY(pattern_index),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, lts_TQueryCursor)) {
        luaL_setfuncs(L, libs, 0);
        luaL_newlib(L, fields);
        luaL_setfuncs(L, accs, 1);
    }
    lua_setfield(L, -2, "query_cursor");
}

/* language */

static int LtsL_field(lua_State *L) {
    const TSLanguage *l = lts_checklanguage(L, 1);
    int type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        size_t len;
        const char *s = luaL_checklstring(L, 2, &len);
        return lts_pushfieldid(L, ts_language_field_id_for_name(l, s, (uint32_t)len));
    } else if (type == LUA_TNUMBER) {
        TSFieldId field_id = lts_checkfieldid(L, 2, l);
        return lua_pushstring(L, ts_language_field_name_for_id(l, field_id)), 1;
    }
    return luaL_typeerror(L, 2, "number/string");
}

static int LtsL_symbol(lua_State *L) {
    const TSLanguage *l = lts_checklanguage(L, 1);
    int type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        size_t len;
        const char *s = luaL_checklstring(L, 2, &len);
        int is_named = lua_toboolean(L, 3);
        return lts_pushsymbol(L, ts_language_symbol_for_name(l, s, (uint32_t)len, is_named));
    } else if (type == LUA_TNUMBER) {
        lua_Integer i = luaL_checkinteger(L, 2);
        if (i <= 0 || i > ts_language_symbol_count(l))
            return lts_argferror(L, 2, "invalid symbol: %d", i);
        return lua_pushstring(L, ts_language_symbol_name(l, (TSSymbol)(i-1))), 1;
    }
    return luaL_typeerror(L, 2, "number/string");
}

static int LtsL_symbol_type(lua_State *L) {
    const TSLanguage *l = lts_checklanguage(L, 1);
    const char *types[] = { "regular", "anonymous", "auxiliary" };
    const size_t tcount = sizeof(types)/sizeof(types[0]);
    TSSymbolType symbol_type = ts_language_symbol_type(l, lts_checksymbol(L, 2, l));
    if (symbol_type >= tcount) return luaL_error(L, "unknown symbol type '%d'", symbol_type);
    return lua_pushstring(L, types[symbol_type]), 1;
}

static int LtsL_next_state(lua_State *L) {
    const TSLanguage *l = lts_checklanguage(L, 1);
    TSStateId state = lts_checkstateid(L, 2, l);
    TSSymbol symbol = lts_checksymbol(L, 3, l);
    return lts_pushstateid(L, ts_language_next_state(l, state, symbol));
}

static int LtsL_version(lua_State *L) {
    const TSLanguage *l = lts_checklanguage(L, 1);
    return lua_pushinteger(L, ts_language_abi_version(l)), 1;
}

static int LtsL_symbol_count(lua_State *L) {
    const TSLanguage *l = lts_checklanguage(L, 1);
    return lua_pushinteger(L, ts_language_symbol_count(l)), 1;
}

static int LtsL_field_count(lua_State *L) {
    const TSLanguage *l = lts_checklanguage(L, 1);
    return lua_pushinteger(L, ts_language_field_count(l)), 1;
}

static int LtsL_state_count(lua_State *L) {
    const TSLanguage *l = lts_checklanguage(L, 1);
    return lua_pushinteger(L, ts_language_state_count(l)), 1;
}

static int LtsI_new(lua_State *L);

static void open_language(lua_State *L) {
    luaL_Reg libs[] = {
        { "__index", NULL },
#define ENTRY(name) { #name, LtsL_##name }
        { "cursor", LtsV_new },
        { "query",  LtsQ_new },
        { "lookahead_iterator", LtsI_new },
        ENTRY(field),
        ENTRY(symbol),
        ENTRY(symbol_type),
        ENTRY(next_state),
        { NULL, NULL }
    };
    luaL_Reg fields[] = {
        ENTRY(version),
        ENTRY(symbol_count),
        ENTRY(field_count),
        ENTRY(state_count),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, lts_TLanguage)) {
        luaL_setfuncs(L, libs, 0);
        luaL_newlib(L, fields);
        lua_pushcclosure(L, lts_fieldedindex, 1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

/* lookahead iterator */

static int LtsI_new(lua_State *L) {
    const TSLanguage *l = lts_checklanguage(L, 1);
    TSStateId state = lts_checkstateid(L, 2, l);
    TSLookaheadIterator **pi = (TSLookaheadIterator**)
        lua_newuserdata(L, sizeof(TSLookaheadIterator*));
    *pi = ts_lookahead_iterator_new(l, state);
    luaL_setmetatable(L, lts_TLookaheadIterator);
    return 1;
}

static int LtsI_delete(lua_State *L) {
    TSLookaheadIterator **pi = (TSLookaheadIterator**)luaL_checkudata(L, 1,
            lts_TLookaheadIterator);
    if (*pi != NULL) {
        ts_lookahead_iterator_delete(*pi);
        *pi = NULL;
    }
    return 0;
}

static int LtsI_next(lua_State *L) {
    TSLookaheadIterator *i = lts_checklookaheaditerator(L, 1);
    if (!ts_lookahead_iterator_next(i))
        return 0;
    lts_returnself(L);
}

static int LtsI_reset(lua_State *L) {
    TSLookaheadIterator *i = lts_checklookaheaditerator(L, 1);
    int top = lua_gettop(L);
    if (top == 2) {
        TSStateId state = lts_checkstateid(L, 2,
                ts_lookahead_iterator_language(i));
        lua_pushboolean(L, ts_lookahead_iterator_reset_state(i, state));
        return 1;
    } else if (top == 3) {
        const TSLanguage *l = lts_checklanguage(L, 2);
        TSStateId state = lts_checkstateid(L, 3, l);
        lua_pushboolean(L, ts_lookahead_iterator_reset_state(i, state));
        return 1;
    }
    return luaL_error(L, "argument invalid, ([language, ]state) expected");
}

static int LtsI_language(lua_State *L) {
    TSLookaheadIterator *i = lts_checklookaheaditerator(L, 1);
    return lts_retrieve(L, ts_lookahead_iterator_language(i), lts_TLanguage);
}

static int LtsI_symbol(lua_State *L) {
    TSLookaheadIterator *i = lts_checklookaheaditerator(L, 1);
    return lts_pushsymbol(L, ts_lookahead_iterator_current_symbol(i));
}

static int LtsI_symbol_name(lua_State *L) {
    TSLookaheadIterator *i = lts_checklookaheaditerator(L, 1);
    return lua_pushstring(L, ts_lookahead_iterator_current_symbol_name(i)), 1;
}

static void open_lookahead_iterator(lua_State *L) {
    luaL_Reg libs[] = {
        { "__close",  LtsI_delete },
        { "__delete", LtsI_delete },
        { "__delete", LtsI_next   },
#define ENTRY(name) { #name, LtsI_##name }
        ENTRY(new),
        ENTRY(next),
        ENTRY(delete),
        ENTRY(reset),
        { NULL, NULL }
    };
    luaL_Reg fields[] = {
        ENTRY(language),
        ENTRY(symbol),
        ENTRY(symbol_name),
#undef  ENTRY
        { NULL, NULL }
    };
    if (luaL_newmetatable(L, lts_TLookaheadIterator)) {
        luaL_setfuncs(L, libs, 0);
        luaL_newlib(L, fields);
        lua_pushcclosure(L, lts_fieldedindex, 1);
        lua_setfield(L, -2, "__index");
    }
    lua_setfield(L, -2, "lookahead_iterator");
}

/* loader */

static const char *lts_checkmodpath(lua_State *L, int idx, const char **pname) {
    size_t len;
    const char *mod = luaL_checklstring(L, idx, &len);
    const char *name = luaL_optstring(L, idx + 1, NULL);
    const char *end = mod + len;
    const char *ext = end;
    luaL_Buffer B;
    while (mod < ext && *--ext != '.')
        ;
    if (mod == ext) {
        /* bare name: symbol "tree_sitter_<mod>", path "<moddir>/grammar/<mod>.so" */
        if (name == NULL) {
            luaL_buffinit(L, &B);
            luaL_addstring(&B, "tree_sitter_");
            luaL_addlstring(&B, mod, len);
            luaL_pushresult(&B);
            *pname = lua_tostring(L, -1);
        }
        luaL_buffinit(L, &B);
        lua_getfield(L, LUA_REGISTRYINDEX, "_TS_MODDIR");
        luaL_addvalue(&B);
        luaL_addstring(&B, "/grammar/");
        luaL_addlstring(&B, mod, len);
        luaL_addstring(&B, ".so");
        luaL_pushresult(&B);
        mod = lua_tostring(L, -1);
    }
    return mod;
}

static void lts_clibs(lua_State *L) {  /* push registry._CLIBS, create if absent */
    lua_getfield(L, LUA_REGISTRYINDEX, "_CLIBS");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "_CLIBS");
    }
}

static void *lts_checkclib(lua_State *L, const char *path) {
    void *plib;
    lts_clibs(L);
    lua_getfield(L, -1, path);
    plib = lua_touserdata(L, -1);
    lua_pop(L, 2);
    return plib;
}

#ifdef _WIN32
# define WIN32_LEAN_AND_MEAN
# include <Windows.h>

int lts_getlasterror(lua_State *L) {
    DWORD err_code = GetLastError();
    LPSTR msg_buff = NULL;
    size_t size;

    if (err_code == 0) {
        return lua_pushliteral(L, "no error"), 1;
    }
    
    size = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, err_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)&msg_buff, 0, NULL);
    lua_pushlstring(L, msg_buff, size);
    LocalFree(msg_buff);
    return 1;
}

static int LtsL_require(lua_State *L) {
    typedef const TSLanguage *LoadFunc();
    const char *name, *path = lts_checkmodpath(L, 1, &name);
    HMODULE h = (HMODULE)lts_checkclib(L, path);
    if (h == NULL)
        h = LoadLibraryA(path);
    if (h == NULL) {
        lts_getlasterror(L);
        return luaL_argerror(L, 1, lua_tostring(L, -1));
    }
    LoadFunc *lf = (LoadFunc*)(void*)GetProcAddress(h, name);
    if (lf == NULL) {
        FreeLibrary(h);
        return lts_argferror(L, 2, "cannot find module '%s'", name);
    }
    lts_clibs(L);
    lua_pushlightuserdata(L, h);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, path);
    lua_rawseti(L, -2, (int)luaL_len(L, -2) + 1);
    lua_pop(L, 1);
    return lts_retrieve(L, (const void*)lf(), lts_TLanguage);
}

#else

#include <dlfcn.h>
#include <errno.h>

static int LtsL_require(lua_State *L) {
    typedef const TSLanguage *LoadFunc();
    const char *name, *path = lts_checkmodpath(L, 1, &name);
    void *h = lts_checkclib(L, path);
    if (h == NULL)
        h = dlopen(path, RTLD_NOW|RTLD_LOCAL);
    if (h == NULL) 
        return luaL_argerror(L, 1, strerror(errno));
    LoadFunc *lf = (LoadFunc*)(void*)dlsym(h, name);
    if (lf == NULL) {
        dlclose(h);
        return lts_argferror(L, 2, "cannot find module '%s'", name);
    }
    lts_clibs(L);
    lua_pushlightuserdata(L, h);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, path);
    lua_rawseti(L, -2, (int)luaL_len(L, -2) + 1);
    lua_pop(L, 1);
    return lts_retrieve(L, (const void*)lf(), lts_TLanguage);
}


#endif /* _WIN32 */

/* entry */

static void lts_stash_moddir(lua_State *L) {
    char dir[512] = ".";
    int found = 0;
    const char *modname = "treesitter.so";
    lua_getglobal(L, "package");   /* +1 package */
    lua_getfield(L, -1, "cpath");  /* +1 cpath */
    if (lua_isstring(L, -1)) {
        const char *seg = lua_tostring(L, -1);
        while (*seg != '\0' && !found) {
            const char *q, *end = strchr(seg, ';');
            size_t seglen = end != NULL ? (size_t)(end - seg) : strlen(seg);
            FILE *f;
            luaL_Buffer B;
            luaL_buffinit(L, &B);
            for (q = seg; q < seg + seglen; ++q) {
                if (*q == '?') luaL_addstring(&B, modname);
                else luaL_addchar(&B, *q);
            }
            luaL_pushresult(&B);   /* +1 candidate path */
            f = fopen(lua_tostring(L, -1), "rb");
            if (f != NULL) {
                const char *path = lua_tostring(L, -1);
                const char *slash = NULL;
                fclose(f);
                for (q = path; *q != '\0'; ++q) {
                    if (*q == '/' || *q == '\\') slash = q;
                }
                if (slash != NULL
                    && (size_t)(slash - path) < sizeof(dir) - 1) {
                    memcpy(dir, path, (size_t)(slash - path));
                    dir[slash - path] = '\0';
                    found = 1;
                }
            }
            lua_pop(L, 1);         /* -1 pop candidate path */
            if (end != NULL) seg = end + 1;
            else break;
        }
    }
    lua_pop(L, 2);                 /* -2 pop cpath, package */
    lua_pushstring(L, dir);
    lua_setfield(L, LUA_REGISTRYINDEX, "_TS_MODDIR");
}

LUALIB_API int luaopen_treesitter(lua_State *L) {
    luaL_Reg libs[] = {
        { "require",                         LtsL_require },
        { "parser",                          NULL         },
        { "tree_cursor",                     NULL         },
        { "query",                           NULL         },
        { "query_cursor",                    NULL         },
        { "lookahead_iterator",              NULL         },
        { "LANGUAGE_VERSION",                NULL         },
        { "MIN_COMPATIBLE_LANGUAGE_VERSION", NULL         },
        { NULL, NULL }
    };
    lts_stash_moddir(L);
    luaL_newlib(L, libs);
    lua_pushinteger(L, TREE_SITTER_LANGUAGE_VERSION);
    lua_setfield(L, -2, "LANGUAGE_VERSION");
    lua_pushinteger(L, TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION);
    lua_setfield(L, -2, "MIN_COMPATIBLE_LANGUAGE_VERSION");
    open_slice(L);
    open_parser(L);
    open_tree(L);
    open_node(L);
    open_tree_cursor(L);
    open_query(L);
    open_query_cursor(L);
    open_language(L);
    open_lookahead_iterator(L);
    return 1;
}

/* win32cc: flags+='-ggdb -O3 -mdll -DLUA_BUILD_AS_DLL'
 * win32cc: libs+='-llua54 -ltree-sitter' output='ts.dll'
 * maccc: flags+='-O2 -shared -undefined dynamic_lookup'
 * linuxcc: flags+='-O2 -shared -fPIC'
 * unixcc: libs+='-ltree-sitter' output='ts.so' */

