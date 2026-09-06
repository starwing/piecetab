#ifdef _MSC_VER
#define _CRT_SECURE_NO_DEPRECATE 1
#define _CRT_SECURE_NO_WARNINGS  1
#endif

#define LUA_LIB
#define SP_STATIC_API
#define CP_STATIC_API

#include "spantree.h"

#include <assert.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

union cp_OpsCursor {
    struct {
        sp_Cursor span;
        int       primed;
    } sp;
    int dummy;
};

#include "compositor.h"

/* compat layer (ref: lua-protobuf pb.c, piecetab.c) */
#if LUA_VERSION_NUM < 502
# define lua_setuservalue(L, idx) lua_setfenv(L, idx)
# define lua_getuservalue(L, idx) lua_getfenv(L, idx)
# define luaL_setfuncs(L, l, n)   (assert(n == 0), luaL_register(L, NULL, l))
# define luaL_setmetatable(L, name) \
    (luaL_getmetatable((L), (name)), lua_setmetatable(L, -2))
# define lua_rawlen lua_objlen
#endif /* LUA_VERSION_NUM < 502 */

#if LUA_VERSION_NUM < 503
# define lsp_rawgeti(L, i, n) lua_rawgeti((L), (i), (int)(n))
# define lsp_rawseti(L, i, n) lua_rawseti((L), (i), (int)(n))
#else
# define lsp_rawgeti(L, i, n) lua_rawgeti((L), (i), (n))
# define lsp_rawseti(L, i, n) lua_rawseti((L), (i), (n))
#endif

static int lsp_error(lua_State *L, int rc) {
    switch (rc) {
    case CP_ERRPARAM: return luaL_error(L, "spantree: invalid parameter");
    case CP_ERRMEM: return luaL_error(L, "spantree: out of memory");
    case CP_ESTALE: return luaL_error(L, "spantree: stale cursor");
    case CP_ELIMIT: return luaL_error(L, "spantree: namespace limit reached");
    default: return luaL_error(L, "spantree: unknown error(%d)", rc);
    }
}

#define LSP_STATE_KEY  ((void *)0x5A17A0B1)
#define LSP_STATE_TYPE "spantree.State"
#define LSP_COMP_TYPE  "spantree.Compositor"
#define LSP_TREE_TYPE  "spantree.Tree"
#define LSP_ITER_TYPE  "spantree.StyledIter"

#define LSP_NS_EPH      (1u << 0)
#define LSP_NS_IDENTITY (1u << 1)
#define LSP_NS_SENS     (1u << 2)
#define LSP_NS_STRICT   (1u << 3)
#define LSP_NS_MASK     (LSP_NS_EPH | LSP_NS_IDENTITY | LSP_NS_SENS)
#define LSP_PRIO_SHIFT  1000000
#define LSP_MARK_BASE   ((cp_Id)1 << 48)

typedef struct lsp_NS {
    lua_Number prio;
    unsigned   flags;
    int        active;
} lsp_NS;

typedef struct lsp_Comp {
    lua_State *L;
    int        ref_byattr; /* hash key -> external attr id */
    int        ref_attrs;  /* external attr id -> attr table */
    int        ref_nsb;    /* ns name -> comp ns id */
    int        ref_trees;  /* weak table of bound Tree userdata */
    char      *fields;     /* '\0'-joined canon field names */
    unsigned   nfields;
    size_t     next; /* next external attr id */
    size_t     ordn; /* active ordinary namespace count */
    lsp_NS    *nsv;
    size_t     nslen;
    size_t     nscap;
} lsp_Comp;

typedef struct lsp_Attr {
    cp_Attr internal;
    cp_Attr ext;
    int     nsid;
} lsp_Attr;

typedef struct cp_SpStore {
    cp_StoreOps base;
    sp_State   *spS;
    sp_Tree    *tree;
    cp_State   *S;
    cp_NS       cur_ns;
    cp_Leaf     cur_clear;
    int         mode;     /* 0 write, 1 clearleaf, 2 clearall */
    int         clearing; /* inside sp_clear: in == 0 means ns removal */
} cp_SpStore;

typedef struct lsp_NsMap {
    int   nsid;
    cp_NS cns;
} lsp_NsMap;

typedef struct lsp_Tree {
    lua_State *L;
    lsp_Comp  *comp;
    cp_State   cp;
    cp_SpStore sp;
    lsp_NsMap *nsmap; /* used comp ns -> cp_NS entries */
    size_t     nsmapn;
    size_t     nsmapcap;
    lsp_Attr  *attrs;
    size_t     attrn;
    size_t     attrcap;
    cp_Attr    nextattr;
} lsp_Tree;

/* ---- tiny vector helpers (malloc based) ---- */

static void *lsp_realloc(void *p, size_t n) {
    return n == 0 ? (free(p), NULL) : realloc(p, n);
}

static int lsp_grow(void **pp, size_t *cap, size_t need, size_t sz) {
    size_t nc = *cap ? *cap : 4;
    void  *np;
    if (need <= *cap) return 0;
    while (nc < need) nc += nc >> 1;
    np = lsp_realloc(*pp, nc * sz);
    if (np == NULL) return -1;
    *pp = np, *cap = nc;
    return 0;
}

/* ---- attribute registry (Compositor userdata) ---- */

static const char *lsp_default_fields[] = {
        "bg",      "bold",      "dim",    "fg",    "italic",
        "reverse", "underline", "vstyle", "vtext",
};

static void lsp_free_fields(lsp_Comp *c) {
    free(c->fields), c->fields = NULL, c->nfields = 0;
}

static int lsp_add_field(lsp_Comp *c, const char *name) {
    const char *p = c->fields;
    size_t      i, cur = 0, nl = strlen(name), need;
    char       *buf;
    for (i = 0; i < c->nfields; ++i, p += strlen(p) + 1)
        if (strcmp(p, name) == 0) return 0;
    for (i = 0, p = c->fields; i < c->nfields; ++i, p += strlen(p) + 1)
        cur += strlen(p) + 1;
    need = cur + nl + 1;
    buf = (char *)malloc(need ? need : 1);
    if (buf == NULL) return -1;
    if (cur > 0) memcpy(buf, c->fields, cur);
    memcpy(buf + cur, name, nl + 1);
    free(c->fields), c->fields = buf, c->nfields += 1;
    return 0;
}

static void lsp_canon(lua_State *L, lsp_Comp *c, int attr) {
    luaL_Buffer b;
    const char *p = c->fields;
    int         n = 0, i;
    luaL_buffinit(L, &b);
    for (i = 0; i < (int)c->nfields; ++i) {
        size_t plen = strlen(p);
        lua_pushstring(L, p);
        lua_rawget(L, attr);
        switch (lua_type(L, -1)) {
        case LUA_TNIL: lua_pop(L, 1); break;
        case LUA_TBOOLEAN:
            if (n++ > 0) luaL_addchar(&b, ',');
            luaL_addstring(&b, p), luaL_addchar(&b, ':');
            luaL_addchar(&b, lua_toboolean(L, -1) ? 'T' : 'F');
            lua_pop(L, 1);
            break;
        case LUA_TNUMBER:
        case LUA_TSTRING:
            if (n++ > 0) luaL_addchar(&b, ',');
            luaL_addstring(&b, p), luaL_addchar(&b, ':'), luaL_addvalue(&b);
            break;
        default: luaL_error(L, "spantree: unexpected type in field '%s'", p);
        }
        p += plen + 1;
    }
    luaL_pushresult(&b);
}

static size_t lsp_intern(lua_State *L, lsp_Comp *c, int attr) {
    size_t id;
    int    isnew;
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_byattr);
    if (luaL_getmetafield(L, attr, "__hash") != LUA_TNIL) {
        lua_pushvalue(L, attr);
        lua_call(L, 1, 1);
        if (lua_type(L, -1) != LUA_TSTRING)
            luaL_argcheck(L, 0, attr, "__hash must return a string");
        lua_pushliteral(L, "h:");
        lua_insert(L, -2);
        lua_concat(L, 2);
    } else {
        lsp_canon(L, c, attr);
        lua_pushliteral(L, "a:");
        lua_insert(L, -2);
        lua_concat(L, 2);
    }
    lua_pushvalue(L, -1);
    lua_rawget(L, -3);
    if (!lua_isnil(L, -1)) {
        id = (size_t)lua_tointeger(L, -1);
        lua_pop(L, 3); /* value, key, key table */
        return id;
    }
    lua_pop(L, 1);
    id = c->next++;
    lua_pushinteger(L, (lua_Integer)id);
    lua_rawset(L, -3);
    lua_pop(L, 1); /* key table */
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_attrs);
    lua_pushinteger(L, (lua_Integer)id);
    lua_pushvalue(L, attr);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    (void)isnew;
    return id;
}

static void lsp_comp_init(lua_State *L, lsp_Comp *c) {
    size_t i, nf = sizeof(lsp_default_fields) / sizeof(lsp_default_fields[0]);
    memset(c, 0, sizeof(*c));
    c->L = L;
    lua_newtable(L), c->ref_byattr = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), c->ref_attrs = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), c->ref_nsb = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    lua_pushliteral(L, "__mode"), lua_pushliteral(L, "k"), lua_rawset(L, -3);
    c->ref_trees = luaL_ref(L, LUA_REGISTRYINDEX);
    for (i = 0; i < nf; ++i)
        if (lsp_add_field(c, lsp_default_fields[i]) != 0)
            luaL_error(L, "spantree: out of memory");
    lua_newtable(L);
    (void)lsp_intern(L, c, lua_gettop(L));
    lua_pop(L, 1);
}

static void lsp_comp_free(lua_State *L, lsp_Comp *c) {
    luaL_unref(L, LUA_REGISTRYINDEX, c->ref_byattr);
    luaL_unref(L, LUA_REGISTRYINDEX, c->ref_attrs);
    luaL_unref(L, LUA_REGISTRYINDEX, c->ref_nsb);
    luaL_unref(L, LUA_REGISTRYINDEX, c->ref_trees);
    lsp_free_fields(c);
    free(c->nsv), c->nsv = NULL, c->nslen = c->nscap = 0;
}

/* ---- namespace registry ---- */

static int lsp_nsid(lua_State *L, lsp_Comp *c, const char *name) {
    int id = 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_nsb);
    lua_pushstring(L, name);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) id = (int)lua_tointeger(L, -1);
    lua_pop(L, 2);
    return id;
}

static lsp_NS *lsp_ns(lsp_Comp *c, int id) {
    return id > 0 && (size_t)id <= c->nslen ? &c->nsv[id - 1] : NULL;
}

static int lsp_ns_new(
        lua_State *L, lsp_Comp *c, const char *name, lua_Number p,
        unsigned flags) {
    lsp_NS *n;
    int     id;
    if (lsp_grow((void **)&c->nsv, &c->nscap, c->nslen + 1, sizeof(lsp_NS))
        != 0)
        luaL_error(L, "spantree: out of memory");
    id = (int)(c->nslen + 1);
    c->nslen += 1;
    n = &c->nsv[id - 1];
    memset(n, 0, sizeof(*n));
    n->prio = p, n->flags = flags, n->active = 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_nsb);
    lua_pushstring(L, name);
    lua_pushinteger(L, id);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    if (!(flags & LSP_NS_EPH)) c->ordn += 1;
    return id;
}

/* ---- spantree StoreOps adapter ---- */

#define SP_CLEAR_TOKEN ((cp_Leaf)1 << 31)

static sp_Mask lsp_mask_of(cp_State *S, cp_Leaf id) {
    cp_RawPart parts[CP_LEAF_MAX];
    sp_Mask    m = 0;
    int        n = cpR_expand(S, id, parts, CP_LEAF_MAX), i;
    for (i = 0; i < n; ++i)
        if (parts[i].ns > 0) sp_addns(&m, (int)parts[i].ns);
    return m;
}

static sp_Id lsp_arb(void *ud, sp_Id in, sp_Id old, sp_Mask *mask) {
    cp_SpStore *b = (cp_SpStore *)ud;
    cp_Store    st;
    cp_Leaf     ret;
    st.ops = &b->base, st.S = b->S;
    if (in == old) {
        if (in != 0) cp_touch(&st, (cp_Leaf)in);
        return in;
    }
    if (in == 0) {
        if (b->clearing) {
            ret = cp_mergeclear(&st, b->cur_ns, (cp_Leaf)old);
            *mask = lsp_mask_of(b->S, ret);
            return (sp_Id)ret;
        }
        cp_died(&st, (cp_Leaf)old);
        *mask = 0;
        return 0;
    }
    if (old == 0) {
        ret = cp_born(&st, (cp_Leaf)in);
        *mask = lsp_mask_of(b->S, ret);
        return (sp_Id)ret;
    }
    if (b->mode == 1)
        ret = cp_mergecut(&st, b->cur_ns, b->cur_clear, (cp_Leaf)old);
    else
        ret = cp_mergewrite(&st, b->cur_ns, (cp_Leaf)in, (cp_Leaf)old);
    *mask = lsp_mask_of(b->S, ret);
    return (sp_Id)ret;
}

static int lsp_sp_write(cp_Store *st, cp_NS ns, cp_Leaf id, cp_Range r) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    sp_Cursor   C;
    int         rc;
    if (b == NULL || b->tree == NULL) return CP_ERRPARAM;
    b->S = st->S, b->cur_ns = ns, b->mode = 0;
    rc = sp_seek(&C, b->tree, r.start);
    if (rc != SP_OK) return rc;
    rc = sp_fill(&C, (sp_Id)id, (size_t)(r.end - r.start));
    return rc;
}

static int lsp_sp_clearleaf(cp_Store *st, cp_NS ns, cp_Leaf id) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    int         rc;
    if (b == NULL || b->tree == NULL) return CP_ERRPARAM;
    b->S = st->S, b->cur_ns = ns, b->cur_clear = id, b->mode = 1;
    b->clearing = 1;
    rc = sp_clear(b->tree, (int)ns, (sp_Id)SP_CLEAR_TOKEN);
    b->clearing = 0;
    return rc;
}

static int lsp_sp_clearall(cp_Store *st, cp_NS ns) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    int         rc;
    if (b == NULL || b->tree == NULL) return CP_ERRPARAM;
    b->S = st->S, b->cur_ns = ns, b->mode = 2;
    b->clearing = 1;
    rc = sp_clear(b->tree, (int)ns, 0);
    b->clearing = 0;
    return rc;
}

static int lsp_sp_seek(cp_Store *st, cp_NS ns, size_t off, cp_StoreCursor *c) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    sp_Cursor  *C;
    sp_Mask     m;
    size_t      len;
    sp_Id       id;
    if (b == NULL || b->tree == NULL || c == NULL) return CP_ERRPARAM;
    C = &c->ops.sp.span;
    if (sp_seek(C, b->tree, off) != SP_OK) return CP_ERRPARAM;
    c->ops.sp.primed = 0;
    id = sp_style(C, &len, &m);
    if (id == SP_NONE) return CP_OK;
    if (C->poff > 0) sp_advance(C, -(sp_Delta)C->poff);
    for (;;) {
        id = sp_style(C, &len, &m);
        if (id == SP_NONE || len == 0) return CP_OK;
        if (ns == 0 || sp_hasns(&m, (int)ns)) {
            c->ops.sp.primed = 1;
            return CP_OK;
        }
        sp_advance(C, (sp_Delta)len);
    }
}

static int lsp_sp_next(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    sp_Cursor  *C;
    sp_Mask     m;
    size_t      len;
    sp_Id       id;
    if (b == NULL || b->tree == NULL || c == NULL || o == NULL)
        return CP_ERRPARAM;
    C = &c->ops.sp.span;
    for (;;) {
        id = sp_style(C, &len, &m);
        if (id == SP_NONE || len == 0) return 0;
        if (c->ops.sp.primed || ns == 0 || sp_hasns(&m, (int)ns)) {
            size_t start = sp_offset(C);
            o->range.start = start, o->range.end = start + len;
            o->id = (cp_Leaf)id;
            sp_advance(C, (sp_Delta)len);
            c->ops.sp.primed = 0;
            return 1;
        }
        sp_advance(C, (sp_Delta)len);
    }
}

static int lsp_sp_prev(cp_Store *st, cp_NS ns, cp_StoreCursor *c, cp_Seg *o) {
    cp_SpStore *b = (cp_SpStore *)cp_ops(st);
    sp_Cursor  *C;
    size_t      len;
    sp_Id       id;
    if (b == NULL || b->tree == NULL || c == NULL || o == NULL)
        return CP_ERRPARAM;
    C = &c->ops.sp.span;
    if (c->ops.sp.primed) c->ops.sp.primed = 0;
    id = sp_prev(C, (int)ns, &len);
    if (id == SP_NONE) return 0;
    o->range.start = sp_offset(C);
    o->range.end = o->range.start + len;
    o->id = (cp_Leaf)id;
    return 1;
}

static void lsp_sp_init(cp_SpStore *b) {
    memset(b, 0, sizeof(*b));
    b->spS = sp_open(NULL, NULL);
    b->tree = sp_newtree(b->spS);
    sp_setarbiter(b->tree, lsp_arb, b);
    b->base.nsmax = SP_MASK_BITS;
    b->base.write = lsp_sp_write;
    b->base.clearleaf = lsp_sp_clearleaf;
    b->base.clearall = lsp_sp_clearall;
    b->base.seek = lsp_sp_seek;
    b->base.next = lsp_sp_next;
    b->base.prev = lsp_sp_prev;
}

static void lsp_sp_free(cp_SpStore *b) {
    if (b->tree) sp_freetree(b->tree), b->tree = NULL;
    if (b->spS) sp_close(b->spS), b->spS = NULL;
}

/* ---- Tree helpers ---- */

static lsp_Comp *lsp_comp_check(lua_State *L, int idx) {
    return (lsp_Comp *)luaL_checkudata(L, idx, LSP_COMP_TYPE);
}

static lsp_Tree *lsp_tree_check(lua_State *L, int idx) {
    return (lsp_Tree *)luaL_checkudata(L, idx, LSP_TREE_TYPE);
}

static int lsp_name_id(lua_State *L, lsp_Comp *c, int idx) {
    const char *name;
    int         id;
    if (lua_isnoneornil(L, idx)) return 0;
    if (lua_type(L, idx) != LUA_TSTRING)
        luaL_argcheck(L, 0, idx, "spantree: namespace name must be a string");
    name = lua_tostring(L, idx);
    if (name[0] == '\0')
        luaL_argcheck(L, 0, idx, "spantree: invalid namespace name");
    id = lsp_nsid(L, c, name);
    if (id == 0) luaL_error(L, "spantree: unknown namespace");
    return id;
}

static cp_NS lsp_nsmap_get(const lsp_Tree *t, int nsid) {
    size_t i;
    for (i = 0; i < t->nsmapn; ++i)
        if (t->nsmap[i].nsid == nsid) return t->nsmap[i].cns;
    return 0;
}

static void lsp_nsmap_remove(lsp_Tree *t, int nsid) {
    size_t i = 0;
    while (i < t->nsmapn) {
        if (t->nsmap[i].nsid == nsid) {
            t->nsmap[i] = t->nsmap[t->nsmapn - 1];
            t->nsmapn -= 1;
        } else {
            i += 1;
        }
    }
}

static cp_NS lsp_tree_ns(lua_State *L, lsp_Tree *t, int nsid) {
    lsp_NS   *n;
    cp_NS     cns;
    lsp_NsMap e;
    int       cflags;
    cns = lsp_nsmap_get(t, nsid);
    if (cns != 0) return cns;
    n = lsp_ns(t->comp, nsid);
    if (n == NULL || !n->active) luaL_error(L, "spantree: unknown namespace");
    cflags = (n->flags & LSP_NS_IDENTITY) ? CP_IDENTITY : 0;
    if (n->flags & LSP_NS_SENS) cflags |= CP_SENSITIVE;
    cns = cp_nsnew(
            &t->cp, (n->flags & LSP_NS_EPH) ? cp_spanvec(&t->cp) : &t->sp.base,
            cflags);
    if (cns < 0) return lsp_error(L, cns), 0;
    if (lsp_grow(
                (void **)&t->nsmap, &t->nsmapcap, t->nsmapn + 1,
                sizeof(lsp_NsMap))
        != 0)
        return luaL_error(L, "spantree: out of memory"), 0;
    e.nsid = nsid, e.cns = cns;
    t->nsmap[t->nsmapn++] = e;
    return cns;
}

static int lsp_mapped_prio(const lsp_NS *n) {
    if (n->flags & LSP_NS_EPH) {
        if (n->prio >= 0) return (int)(n->prio - LSP_PRIO_SHIFT);
        return (int)(n->prio + LSP_PRIO_SHIFT);
    }
    return (int)n->prio;
}

static cp_Attr lsp_tree_attr(lua_State *L, lsp_Tree *t, int nsid, cp_Attr ext) {
    lsp_NS  *n = lsp_ns(t->comp, nsid);
    lsp_Attr a;
    size_t   i;
    for (i = 0; i < t->attrn; ++i)
        if (t->attrs[i].nsid == nsid && t->attrs[i].ext == ext)
            return t->attrs[i].internal;
    if (lsp_grow(
                (void **)&t->attrs, &t->attrcap, t->attrn + 1, sizeof(lsp_Attr))
        != 0)
        luaL_error(L, "spantree: out of memory");
    a.internal = t->nextattr++;
    a.ext = ext;
    a.nsid = nsid;
    t->attrs[t->attrn++] = a;
    if (cp_setpriority(&t->cp, a.internal, lsp_mapped_prio(n)) != CP_OK)
        luaL_error(L, "spantree: out of memory");
    return a.internal;
}

static cp_Attr lsp_attr_ext(lsp_Tree *t, cp_Attr internal) {
    size_t i;
    for (i = 0; i < t->attrn; ++i)
        if (t->attrs[i].internal == internal) return t->attrs[i].ext;
    return 0;
}

static int lsp_ns_cpflags(lsp_Comp *c, int nsid) {
    lsp_NS *n = lsp_ns(c, nsid);
    int     f = 0;
    if (n == NULL || !n->active) return 0;
    if (n->flags & LSP_NS_IDENTITY) f |= CP_IDENTITY;
    if (n->flags & LSP_NS_SENS) f |= CP_SENSITIVE;
    return f;
}

/* ---- bound tree methods ---- */

static void lsp_commit(lua_State *L, lsp_Tree *t) {
    int rc;
    rc = cp_commit(&t->cp);
    if (rc != CP_OK) lsp_error(L, rc);
}

static int Lsp_tree_mark(lua_State *L) {
    lsp_Tree   *t = lsp_tree_check(L, 1);
    lsp_Comp   *c = t->comp;
    int         nsid = lsp_name_id(L, c, 2);
    lua_Integer off = luaL_checkinteger(L, 4);
    lua_Integer ln = luaL_checkinteger(L, 5);
    lua_Integer aid;
    cp_Attr     ext;
    cp_Range    r;
    cp_Id       id;
    cp_NS       cns;
    int         rc;
    if (off < 0) luaL_argcheck(L, 0, 4, "spantree: invalid offset");
    if (ln < 0) luaL_argcheck(L, 0, 5, "spantree: invalid length");
    if (lsp_ns_cpflags(c, nsid) & CP_IDENTITY && ln == 0)
        luaL_error(L, "spantree: identity mark length must be positive");
    if (lua_type(L, 3) == LUA_TTABLE)
        ext = (cp_Attr)lsp_intern(L, c, 3);
    else {
        aid = luaL_checkinteger(L, 3);
        if (aid < 0 || (size_t)aid >= c->next)
            luaL_argcheck(L, 0, 3, "spantree: unknown style id");
        ext = (cp_Attr)aid;
    }
    cns = lsp_tree_ns(L, t, nsid);
    if (!(lsp_ns_cpflags(c, nsid) & CP_IDENTITY)) {
        if (ext == 0) luaL_argcheck(L, 0, 3, "spantree: invalid style id");
        ext = lsp_tree_attr(L, t, nsid, ext);
    }
    r.start = (size_t)off, r.end = (size_t)(off + ln);
    rc = cp_mark(&t->cp, cns, ext, r, &id);
    if (rc != CP_OK) lsp_error(L, rc);
    if (!(lsp_ns_cpflags(c, nsid) & CP_IDENTITY))
        id = (cp_Id)lsp_attr_ext(t, (cp_Attr)id);
    return lua_pushinteger(L, (lua_Integer)id), 1;
}

static int lsp_clear_range_token(
        lua_State *L, lsp_Tree *t, cp_NS cns, size_t off, size_t len);

static int lsp_clear_tree(lua_State *L, lsp_Tree *t, size_t off, size_t len) {
    sp_Cursor C;
    size_t    i, n;
    int       rc;
    rc = sp_seek(&C, t->sp.tree, off);
    if (rc != SP_OK) return lsp_error(L, rc), 0;
    rc = sp_fill(&C, 0, len);
    if (rc != SP_OK) return lsp_error(L, rc), 0;
    n = cpV_len(t->cp.nsv);
    for (i = 0; i < n; ++i) {
        cp_NSReg *r = &t->cp.nsv[i];
        if (!r->active || r->store.ops != &cpB_ops) continue;
        (void)lsp_clear_range_token(L, t, (cp_NS)(i + 1), off, len);
    }
    return 1;
}

typedef struct lsp_SegRec {
    cp_Id    id;
    cp_Range r;
} lsp_SegRec;

static int lsp_sub_range(cp_Range r, size_t a, size_t b, cp_Range out[2]) {
    int n = 0;
    if (r.start < a && a < r.end) out[n].start = r.start, out[n].end = a, ++n;
    if (r.end > b && b > r.start) out[n].start = b, out[n].end = r.end, ++n;
    return n;
}

static int lsp_collect_ns(lsp_Tree *t, cp_NS ns, lsp_SegRec **out, size_t *pn) {
    cp_MarkCursor MC;
    cp_Mark       m = {{0, 0}, 0, 0};
    lsp_SegRec   *v = NULL;
    size_t        n = 0, cap = 0;
    int           rc;
    rc = cp_seek(&MC, &t->cp, ns, 0);
    if (rc != CP_OK) return rc;
    while ((rc = cp_next(&MC, &m)) == 1) {
        lsp_SegRec s;
        s.id = m.id, s.r = m.range;
        if (lsp_grow((void **)&v, &cap, n + 1, sizeof(*v)) != CP_OK) {
            free(v);
            return CP_ERRMEM;
        }
        v[n++] = s;
    }
    if (rc < 0) {
        free(v);
        return rc;
    }
    *out = v, *pn = n;
    return CP_OK;
}

static int lsp_clear_range_token(
        lua_State *L, lsp_Tree *t, cp_NS cns, size_t off, size_t len) {
    lsp_SegRec *segs = NULL;
    size_t      n = 0, i;
    char       *seen;
    size_t      end = off + len;
    int         rc;
    rc = lsp_collect_ns(t, cns, &segs, &n);
    if (rc != CP_OK) return lsp_error(L, rc), 0;
    seen = (char *)calloc(n ? n : 1, 1);
    if (seen == NULL) {
        free(segs);
        return luaL_error(L, "spantree: out of memory"), 0;
    }
    for (i = 0; i < n; ++i) {
        cp_Id     id;
        cp_Attr   attr;
        size_t    j, m = 0, on = 0;
        cp_Range *outs;
        int       hit = 0;
        if (seen[i]) continue;
        id = segs[i].id;
        for (j = 0; j < n; ++j)
            if (segs[j].id == id) m += 1;
        outs = (cp_Range *)malloc((m * 2 + 1) * sizeof(*outs));
        if (outs == NULL) {
            free(seen), free(segs);
            return luaL_error(L, "spantree: out of memory"), 0;
        }
        for (j = 0; j < n; ++j) {
            if (segs[j].id != id) continue;
            seen[j] = 1;
            if (segs[j].r.start < end && segs[j].r.end > off) {
                cp_Range tmp[2];
                int      k = lsp_sub_range(segs[j].r, off, end, tmp), q;
                hit = 1;
                for (q = 0; q < k; ++q) outs[on++] = tmp[q];
            } else {
                outs[on++] = segs[j].r;
            }
        }
        if (hit) {
            attr = cp_attr(&t->cp, id);
            rc = cp_unmark(&t->cp, id);
            if (rc < 0) {
                free(outs), free(seen), free(segs);
                return lsp_error(L, rc), 0;
            }
            for (j = 0; j < on; ++j) {
                cp_Id dummy;
                rc = cp_mark(&t->cp, cns, attr, outs[j], &dummy);
                if (rc != CP_OK) {
                    free(outs), free(seen), free(segs);
                    return lsp_error(L, rc), 0;
                }
            }
        }
        free(outs);
    }
    free(seen), free(segs);
    return 1;
}

static int lsp_clear_ns_range(
        lua_State *L, lsp_Tree *t, int nsid, size_t off, size_t len) {
    cp_NS cns = lsp_tree_ns(L, t, nsid);
    if (lsp_ns_cpflags(t->comp, nsid) & CP_IDENTITY)
        return luaL_error(L, "spantree: identity range clear unsupported"), 0;
    return lsp_clear_range_token(L, t, cns, off, len);
}

static int lsp_clear_ns_full(lua_State *L, lsp_Tree *t, int nsid) {
    cp_NS cns = lsp_tree_ns(L, t, nsid);
    int   rc = cp_nsclear(&t->cp, cns);
    if (rc != CP_OK) lsp_error(L, rc);
    return 1;
}

static int Lsp_tree_clear(lua_State *L) {
    lsp_Tree   *t = lsp_tree_check(L, 1);
    lua_Integer off = 0, len = 0;
    int         n = lua_gettop(L), nsid = 0;
    if (n == 2) {
        nsid = lsp_name_id(L, t->comp, 2);
        if (nsid == 0) return lsp_error(L, SP_ERRPARAM), 0;
        return lsp_clear_ns_full(L, t, nsid);
    }
    if (n > 2) {
        nsid = lsp_name_id(L, t->comp, 2);
        off = luaL_checkinteger(L, 3);
        len = luaL_checkinteger(L, 4);
        if (off < 0) luaL_argcheck(L, 0, 3, "spantree: invalid offset");
        if (len < 0) luaL_argcheck(L, 0, 4, "spantree: invalid length");
        if (nsid > 0)
            return lsp_clear_ns_range(L, t, nsid, (size_t)off, (size_t)len);
        return lsp_clear_tree(L, t, (size_t)off, (size_t)len);
    }
    return lsp_clear_tree(L, t, 0, (size_t)sp_bytes(t->sp.tree));
}

static int Lsp_tree_splice(lua_State *L) {
    lsp_Tree   *t = lsp_tree_check(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    lua_Integer del = luaL_checkinteger(L, 3);
    lua_Integer ins = luaL_checkinteger(L, 4);
    sp_Cursor   C;
    int         rc;
    if (off < 0) luaL_argcheck(L, 0, 2, "spantree: invalid offset");
    if (del < 0) luaL_argcheck(L, 0, 3, "spantree: invalid length");
    if (ins < 0) luaL_argcheck(L, 0, 4, "spantree: invalid length");
    rc = sp_seek(&C, t->sp.tree, (size_t)off);
    if (rc != SP_OK) lsp_error(L, rc);
    rc = sp_splice(&C, (size_t)del, (size_t)ins);
    if (rc != SP_OK) lsp_error(L, rc);
    lsp_commit(L, t);
    return lua_settop(L, 1), 1;
}

/* ---- styled iterator ---- */

typedef struct lsp_Iter {
    cp_StyledCursor sc;
    lsp_Tree       *t;
    size_t          qstart;
    size_t          qend;
    size_t          last;
    size_t          empty_end;
    cp_Range        group;
    cp_Attr        *buf;
    size_t          bufn;
    size_t          bufcap;
    int             started;
    int             done;
    int             have_empty;
} lsp_Iter;

static void lsp_push_empty(lua_State *L, lsp_Iter *it, size_t s, size_t e) {
    lua_pushinteger(L, (lua_Integer)s);
    lua_pushinteger(L, (lua_Integer)(e - s));
    lua_rawgeti(L, LUA_REGISTRYINDEX, it->t->comp->ref_attrs);
    lua_pushinteger(L, 0);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    lua_pushinteger(L, 0);
}

static void lsp_push_folded(lua_State *L, lsp_Iter *it) {
    int    foldix, i;
    size_t id;
    lua_newtable(L);
    foldix = lua_gettop(L);
    for (i = 0; i < (int)it->bufn; ++i) {
        cp_Attr ext = lsp_attr_ext(it->t, it->buf[i]);
        lua_rawgeti(L, LUA_REGISTRYINDEX, it->t->comp->ref_attrs);
        lua_pushinteger(L, (lua_Integer)ext);
        lua_rawget(L, -2);
        lua_remove(L, -2);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, foldix);
            lua_pop(L, 1); /* original value; key remains for next */
        }
        lua_pop(L, 1); /* attr table */
    }
    id = lsp_intern(L, it->t->comp, foldix);
    lua_pushinteger(L, (lua_Integer)id);
}

static int lsp_styled_next(lua_State *L) {
    lsp_Iter *it = (lsp_Iter *)lua_touserdata(L, lua_upvalueindex(1));
    cp_Styled sty;
    int       rc;
    if (it->t == NULL || it->done) return 0;
    if (!it->started) {
        rc = cp_seekstyled(&it->sc, &it->t->cp, it->qstart);
        if (rc != CP_OK) return lsp_error(L, rc), 0;
        it->started = 1;
        it->last = it->qstart;
    }
    if (it->bufn == 0) {
        for (;;) {
            rc = cp_nextstyled(&it->sc, &sty);
            if (rc < 0) return lsp_error(L, rc), 0;
            if (rc == 0) {
                if (it->last < it->qend) {
                    lsp_push_empty(L, it, it->last, it->qend);
                    it->last = it->qend;
                    return 4;
                }
                it->done = 1;
                return 0;
            }
            if (it->bufn == 0) {
                it->group = sty.range;
                if (sty.range.start >= it->qend) {
                    if (it->last < it->qend) {
                        lsp_push_empty(L, it, it->last, it->qend);
                        it->last = it->qend;
                        return 4;
                    }
                    it->done = 1;
                    return 0;
                }
                if (sty.range.start > it->last) {
                    it->empty_end = sty.range.start;
                    it->have_empty = 1;
                }
            }
            if (lsp_grow(
                        (void **)&it->buf, &it->bufcap, it->bufn + 1,
                        sizeof(cp_Attr))
                != 0)
                return luaL_error(L, "spantree: out of memory"), 0;
            it->buf[it->bufn++] = sty.attr;
            if (sty.last) break;
        }
        if (it->have_empty) {
            lsp_push_empty(L, it, it->last, it->empty_end);
            it->last = it->empty_end;
            it->have_empty = 0;
            return 4;
        }
    }
    {
        size_t s = it->group.start > it->last ? it->group.start : it->last;
        size_t e = it->group.end < it->qend ? it->group.end : it->qend;
        if (e <= s) {
            it->bufn = 0;
            return lsp_styled_next(L);
        }
        lua_pushinteger(L, (lua_Integer)s);
        lua_pushinteger(L, (lua_Integer)(e - s));
        lsp_push_folded(L, it);
        it->last = e;
        it->bufn = 0;
        if (e >= it->qend) it->done = 1;
        return 4;
    }
}

static int Lsp_tree_styled(lua_State *L) {
    lsp_Tree   *t = lsp_tree_check(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    lua_Integer len = luaL_checkinteger(L, 3);
    lsp_Iter   *it;
    if (off < 0) luaL_argcheck(L, 0, 2, "spantree: invalid offset");
    if (len < 0) luaL_argcheck(L, 0, 3, "spantree: invalid length");
    it = (lsp_Iter *)lua_newuserdata(L, sizeof(lsp_Iter));
    memset(it, 0, sizeof(*it));
    it->t = t;
    it->qstart = (size_t)off;
    it->qend = (size_t)(off + len);
    it->sc.pos = (size_t)off;
    lua_newtable(L);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "tree");
    lua_setuservalue(L, -2);
    luaL_setmetatable(L, LSP_ITER_TYPE);
    lua_pushvalue(L, -1);
    lua_pushcclosure(L, lsp_styled_next, 1);
    return 1;
}

/* ---- GC and factories ---- */

static int Lsp_iter_gc(lua_State *L) {
    lsp_Iter *it = (lsp_Iter *)luaL_checkudata(L, 1, LSP_ITER_TYPE);
    free(it->buf);
    return 0;
}

static int Lsp_comp_gc(lua_State *L) {
    lsp_Comp *c = lsp_comp_check(L, 1);
    lsp_comp_free(L, c);
    return 0;
}

static int Lsp_compositor(lua_State *L) {
    lsp_Comp *c = (lsp_Comp *)lua_newuserdata(L, sizeof(lsp_Comp));
    lsp_comp_init(L, c);
    luaL_setmetatable(L, LSP_COMP_TYPE);
    return 1;
}

static int Lsp_tree_gc(lua_State *L) {
    lsp_Tree *t = lsp_tree_check(L, 1);
    free(t->nsmap);
    free(t->attrs);
    cp_free(&t->cp);
    lsp_sp_free(&t->sp);
    return 0;
}

static int Lsp_new(lua_State *L) {
    lsp_Comp *c = lsp_comp_check(L, 1);
    lsp_Tree *t = (lsp_Tree *)lua_newuserdata(L, sizeof(lsp_Tree));
    memset(t, 0, sizeof(*t));
    t->L = L;
    t->comp = c;
    t->nextattr = 1;
    if (cp_init(&t->cp, NULL, NULL) != CP_OK)
        return luaL_error(L, "spantree: out of memory"), 0;
    lsp_sp_init(&t->sp);
    t->sp.S = &t->cp;
    /* anchor comp userdata */
    lua_newtable(L);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "comp");
    lua_setuservalue(L, -2);
    luaL_setmetatable(L, LSP_TREE_TYPE);
    /* weak registration for namespace propagation */
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_trees);
    lua_pushvalue(L, -2), lua_pushboolean(L, 1), lua_rawset(L, -3);
    lua_pop(L, 1);
    return 1;
}

/* ---- namespace Lua methods ---- */

static int Lsp_comp_fields(lua_State *L);
static int Lsp_comp_intern(lua_State *L);
static int Lsp_comp_attr(lua_State *L);
static int Lsp_comp_namespace(lua_State *L);

static int lsp_ns_query(lua_State *L, lsp_Comp *c, int id) {
    lsp_NS *n = lsp_ns(c, id);
    if (n == NULL || !n->active) return lua_pushnil(L), 1;
    lua_pushnumber(L, n->prio);
    if (n->flags & LSP_NS_EPH)
        lua_pushliteral(L, "ephemeral");
    else
        lua_pushnil(L);
    if (n->flags & LSP_NS_IDENTITY)
        lua_pushstring(
                L,
                (n->flags & LSP_NS_SENS) ? "identity+sensitive" : "identity");
    return (n->flags & LSP_NS_IDENTITY) ? 3 : 2;
}

static int lsp_parse_flags(
        lua_State *L, int idx, unsigned *pflags, int *peph, int *pstrict) {
    const char *s = luaL_checklstring(L, idx, NULL);
    unsigned    flags = 0;
    int         eph = 0, strict = 0;
    for (; *s; ++s) {
        if (*s == 'c')
            strict = 1;
        else if (*s == 'e')
            eph = 1;
        else if (*s == 'i')
            flags |= LSP_NS_IDENTITY;
        else if (*s == 's')
            flags |= LSP_NS_SENS;
        else
            luaL_error(L, "spantree: invalid namespace flags");
    }
    if (eph && (flags & LSP_NS_IDENTITY))
        luaL_error(L, "spantree: invalid namespace flags");
    if ((flags & LSP_NS_SENS) && !(flags & LSP_NS_IDENTITY))
        luaL_error(L, "spantree: invalid namespace flags");
    *pflags = flags, *peph = eph, *pstrict = strict;
    return 0;
}

static void lsp_propagate_ns(lua_State *L, lsp_Comp *c, int nsid) {
    /* Re-prio the per-tree attrs of this ns in every bound tree. */
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_trees);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lsp_Tree *t = (lsp_Tree *)lua_touserdata(L, -2);
        size_t    i;
        if (t != NULL && lsp_nsmap_get(t, nsid) != 0) {
            lsp_NS *n = lsp_ns(c, nsid);
            for (i = 0; i < t->attrn; ++i)
                if (t->attrs[i].nsid == nsid
                    && cp_setpriority(
                               &t->cp, t->attrs[i].internal, lsp_mapped_prio(n))
                               != CP_OK)
                    luaL_error(L, "spantree: out of memory");
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static int lsp_ns_register(lua_State *L, lsp_Comp *c, const char *name) {
    int        top = lua_gettop(L), nsid, eph = 0, strict = 0, existed;
    lua_Number p = luaL_checknumber(L, 3);
    unsigned   flags = 0, oldflags = 0;
    lsp_NS    *n;
    lua_Number old = 0;
    nsid = lsp_nsid(L, c, name);
    existed = nsid != 0;
    if (existed) {
        n = lsp_ns(c, nsid);
        old = n->prio, oldflags = n->flags;
        flags = oldflags;
    }
    if (top >= 4) {
        lsp_parse_flags(L, 4, &flags, &eph, &strict);
        if (existed) {
            unsigned newmode = (eph ? LSP_NS_EPH : 0)
                             | (flags & LSP_NS_IDENTITY)
                             | (flags & LSP_NS_SENS);
            unsigned oldmode = oldflags
                             & (LSP_NS_EPH | LSP_NS_IDENTITY | LSP_NS_SENS);
            if (newmode != oldmode)
                luaL_error(L, "spantree: flags cannot change in place");
        }
    } else if (!existed) {
        flags = 0, eph = 0;
    }
    if (strict && existed)
        luaL_error(L, "spantree: namespace already registered");
    if (!existed) {
        if (!eph && c->ordn >= (size_t)SP_MASK_BITS)
            luaL_error(L, "spantree: namespace limit reached");
        flags |= eph ? LSP_NS_EPH : 0;
        nsid = lsp_ns_new(L, c, name, p, flags);
        lua_pushnil(L);
    } else {
        n = lsp_ns(c, nsid);
        n->prio = p;
        lsp_propagate_ns(L, c, nsid);
        lua_pushnumber(L, old);
    }
    return 1;
}

static int Lsp_comp_namespace(lua_State *L) {
    lsp_Comp   *c = lsp_comp_check(L, 1);
    const char *name = luaL_checklstring(L, 2, NULL);
    int         id, rc;
    if (name[0] == '\0')
        luaL_argcheck(L, 0, 2, "spantree: invalid namespace name");
    id = lsp_nsid(L, c, name);
    if (lua_gettop(L) == 2) return lsp_ns_query(L, c, id);
    if (lua_isnil(L, 3)) {
        lsp_NS    *n = lsp_ns(c, id);
        lua_Number old;
        if (n == NULL || !n->active)
            luaL_error(L, "spantree: unknown namespace");
        old = n->prio;
        /* clear from all bound trees */
        lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_trees);
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            lsp_Tree *t = (lsp_Tree *)lua_touserdata(L, -2);
            if (t != NULL) {
                cp_NS cns = lsp_nsmap_get(t, id);
                if (cns != 0) {
                    rc = cp_nsdel(&t->cp, cns);
                    if (rc != CP_OK) return lsp_error(L, rc), 0;
                    lsp_nsmap_remove(t, id);
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        n->active = 0;
        if (!(n->flags & LSP_NS_EPH)) c->ordn -= 1;
        lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_nsb);
        lua_pushstring(L, name);
        lua_pushnil(L);
        lua_rawset(L, -3);
        lua_pop(L, 1);
        return lua_pushnumber(L, old), 1;
    }
    return lsp_ns_register(L, c, name);
}

/* ---- compositor methods ---- */

static int Lsp_comp_intern(lua_State *L) {
    lsp_Comp *c = lsp_comp_check(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    return lua_pushinteger(L, (lua_Integer)lsp_intern(L, c, 2)), 1;
}

static int Lsp_comp_attr(lua_State *L) {
    lsp_Comp   *c = lsp_comp_check(L, 1);
    lua_Integer id = luaL_checkinteger(L, 2);
    if (id < 0 || (size_t)id >= c->next) return lua_pushnil(L), 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_attrs);
    lsp_rawgeti(L, -1, (lua_Integer)id);
    return 1;
}

static int lsp_fieldswrite(lua_State *L, lsp_Comp *c, int idx, int add) {
    int n, i;
    luaL_checktype(L, idx, LUA_TTABLE);
    n = (int)lua_rawlen(L, idx);
    if (!add) lsp_free_fields(c);
    for (i = 0; i < n; ++i) {
        lsp_rawgeti(L, idx, i + 1);
        luaL_checktype(L, -1, LUA_TSTRING);
        if (lsp_add_field(c, lua_tostring(L, -1)) != 0)
            return luaL_error(L, "spantree: out of memory"), 0;
        lua_pop(L, 1);
    }
    return lua_settop(L, 1), 1;
}

static int Lsp_comp_fields(lua_State *L) {
    lsp_Comp *c = lsp_comp_check(L, 1);
    int       t = lua_type(L, 2);
    if (t == LUA_TNIL || t == LUA_TNONE) {
        const char *p = c->fields;
        unsigned    i;
        lua_createtable(L, (int)c->nfields, 0);
        for (i = 0; i < c->nfields; ++i, p += strlen(p) + 1)
            lua_pushstring(L, p), lsp_rawseti(L, -2, i + 1);
        return 1;
    }
    if (t == LUA_TTABLE) return lsp_fieldswrite(L, c, 2, 0);
    luaL_argcheck(
            L, *luaL_checkstring(L, 2) == 'a', 2,
            "spantree: unknown fields mode");
    return lsp_fieldswrite(L, c, 3, 1);
}

/* ---- module registration ---- */

static void lsp_open_comp(lua_State *L) {
    static const luaL_Reg libs[] = {
            {"__gc", Lsp_comp_gc},
            {"fields", Lsp_comp_fields},
            {"intern", Lsp_comp_intern},
            {"attr", Lsp_comp_attr},
            {"namespace", Lsp_comp_namespace},
            {NULL, NULL}};
    if (luaL_newmetatable(L, LSP_COMP_TYPE)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
        lua_pop(L, 1);
    }
}

static void lsp_open_iter(lua_State *L) {
    static const luaL_Reg libs[] = {{"__gc", Lsp_iter_gc}, {NULL, NULL}};
    if (luaL_newmetatable(L, LSP_ITER_TYPE)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
        lua_pop(L, 1);
    }
}

static void lsp_open_tree(lua_State *L) {
    static const luaL_Reg libs[] = {
            {"__gc", Lsp_tree_gc},          {"splice", Lsp_tree_splice},
            {"mark", Lsp_tree_mark},        {"clear", Lsp_tree_clear},
            {"styled", Lsp_tree_styled},    {"new", Lsp_new},
            {"compositor", Lsp_compositor}, {NULL, NULL}};
    if (luaL_newmetatable(L, LSP_TREE_TYPE)) {
        luaL_setfuncs(L, libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
}

LUALIB_API int luaopen_spantree(lua_State *L) {
    lsp_open_comp(L), lsp_open_iter(L), lsp_open_tree(L);
    return 1;
}
