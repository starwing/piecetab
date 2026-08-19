#ifdef _MSC_VER
#define _CRT_SECURE_NO_DEPRECATE 1
#define _CRT_SECURE_NO_WARNINGS  1
#endif

#define LUA_LIB
#include <assert.h>
#include <lauxlib.h>
#include <limits.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SP_STATIC_API
#include "spantree.h"

/* compat layer (ref: lua-protobuf pb.c, piecetab.c) */

#if LUA_VERSION_NUM < 502
# define lua_setuservalue(L, idx) lua_setfenv(L, idx)
# define lua_getuservalue(L, idx) lua_getfenv(L, idx)
# define luaL_setfuncs(L, l, n)   (assert(n == 0), luaL_register(L, NULL, l))
# define luaL_setmetatable(L, name) \
    (luaL_getmetatable((L), (name)), lua_setmetatable(L, -2))
# define lua_rawlen lua_objlen

static void lua_rawgetp(lua_State *L, int idx, const void *p) {
    lua_pushlightuserdata(L, (void *)p);
    lua_rawget(L, idx);
}

static void lua_rawsetp(lua_State *L, int idx, const void *p) {
    lua_pushlightuserdata(L, (void *)p);
    lua_insert(L, -2);
    lua_rawset(L, idx);
}
#endif /* LUA_VERSION_NUM < 502 */

#if LUA_VERSION_NUM >= 503
# define lua53_rawgetp lua_rawgetp
#else  /* not Lua 5.3 */
static int lua53_rawgetp(lua_State *L, int idx, const void *p) {
    return lua_rawgetp(L, idx, p), lua_type(L, -1);
}
#endif /* LUA_VERSION_NUM >= 503 */

/* lua_rawgeti/lua_rawseti take int (5.1/5.2) or lua_Integer (5.3+) */
#if LUA_VERSION_NUM < 503
# define lsp_rawgeti(L, i, n) lua_rawgeti((L), (i), (int)(n))
# define lsp_rawseti(L, i, n) lua_rawseti((L), (i), (int)(n))
#else
# define lsp_rawgeti(L, i, n) lua_rawgeti((L), (i), (n))
# define lsp_rawseti(L, i, n) lua_rawseti((L), (i), (n))
#endif

/* ---- types ---- */

#define LSP_STATE_KEY  ((void *)0x5A17A0B1)
#define LSP_STATE_TYPE "spantree.State"
#define LSP_COMP_TYPE  "spantree.Compositor"
#define LSP_TREE_TYPE  "spantree.Tree"
#define LSP_CUR_TYPE   "spantree.Cursor"

#define LSP_EPOCH_MSG "spantree: cursor invalidated by tree edit"

/* id domains: attr ids and op ids interleave in [0, CP_COMP_START)
 * (plain region, tree segments tell them apart by mask: mask 0 = flat
 * attr segment, mask != 0 = single-ns op segment); composite ids start
 * at CP_COMP_START (dense k = id - CP_COMP_START, two chain slots each) */
#define CP_COMP_START ((sp_Id)1 << 24)

typedef struct lsp_State {
    sp_State *S;
} lsp_State;

/* compositor id domains (plain region [0, CP_COMP_START)) */
typedef int      cp_NS;   /* namespace id (0 = unaffiliated) */
typedef unsigned cp_Attr; /* attr id (0 = empty attr, prebuilt) */

/* op kinds: WRITE(ns, attr), CLEAR(ns), REORDER (payload ignored) */
typedef enum { CP_K_WRITE = 1, CP_K_CLEAR, CP_K_REORDER } cp_Kind;

/* one op payload: kind + ns + attr (REORDER ignores ns/attr) */
typedef struct cp_Op {
    cp_Kind kind;
    cp_NS   ns;
    cp_Attr attr;
} cp_Op;

/* one folded (ns -> attr) slot contribution */
typedef struct cp_NSAttr {
    cp_NS   ns;
    cp_Attr attr;
} cp_NSAttr;

/* one ns registration: priority (sort key) + sequence (tie-break) */
typedef struct cp_NSReg {
    lua_Number prio;
    unsigned   reg;
} cp_NSReg;

/* one composite slot: a = chain head (a composite id or the plain
 * op/attr start), b = op leaf (plain op id); idfree = freelist link
 * (next free slot + 1, 0 = tail); refcnt = tree references */
typedef struct cp_Slot {
    sp_Id    a, b;
    unsigned idfree;
    unsigned refcnt;
} cp_Slot;

/* minispan: one flat segment per ephemeral ns (ordered, disjoint,
 * adjacent ids differ; stV_ vec of sv_Span; no mask, no B+ structure) */
typedef struct sv_Span {
    size_t  off; /* segment start offset */
    size_t  len; /* segment length */
    cp_Attr id;  /* segment attr id */
} sv_Span;

/* vec handle for one ephemeral-ns segment list (grow mutates the
 * handle: writers take sv_List *, readers take sv_Span *) */
typedef sv_Span *sv_List;

/* cp: compositor state, one per Compositor userdata (sp.compositor()
 * owns a fresh state; trees bound to that Compositor share it, trees
 * of other Compositors are isolated). Plain region: attr intern
 * (ref_byattr/ref_attrs) + op space ((ns, attr) intern via ref_byop).
 * ns registry: name -> ns id (ref_nsb), prio/regseq per ns, ids
 * 1..SP_MASK_BITS ordinary (mask bit ns-1), beyond = ephemeral.
 * Composite region: chain slot pairs + freelist (idfree links) +
 * composite-slot refcnt (pure C counters, zero Lua calls on the ref
 * path) + chain-structure reuse table (ref_chainhash). All vecs are
 * stV_ (malloc); the hash tables are Lua registry refs freed by
 * cp_free. */
typedef struct cp_State {
    unsigned  next;       /* next plain id (0 = empty attr, prebuilt) */
    unsigned  nfields;    /* canon whitelist length */
    int       ref_byattr; /* registry hash: canon/hash key -> attr id */
    int      *attrs;      /* vec: attr id -> luaL_ref of attr table */
    char     *fields;     /* '\0'-joined canon whitelist field names */
    cp_NSReg  nsreg[SP_MASK_BITS + 1]; /* ordinary ns regs (ns 1..64) */
    cp_NSReg *ensreg; /* vec: ephemeral ns regs (index = ephidx) */
    unsigned  regcnt; /* registration counter (tie-break) */
    int       nsstack[SP_MASK_BITS]; /* freed ordinary ns ids */
    int       nsstackn;              /* nsstack length */
    unsigned  nsnext;                /* next ordinary ns id (1..SP_MASK_BITS) */
    unsigned  ephnext;   /* next ephemeral ns id (SP_MASK_BITS+1 up) */
    int       ref_nsb;   /* registry hash: ns name -> ns id */
    cp_Op    *ops;       /* vec: op id -> cp_Op {kind, ns, attr} */
    int       ref_byop;  /* registry hash: op payload key -> op id */
    cp_Slot  *slots;     /* vec: composite slot {a, b, idfree, refcnt} */
    size_t    chainnext; /* first never-used composite slot */
    unsigned  freehead;  /* freelist head + 1 (0 = empty) */
    int ref_chainhash;   /* registry hash: chain structure -> composite id */
} cp_State;

/* Compositor userdata: owns one cp_State (inline, zero second-level
 * allocation) plus a registry weak table of bound Tree userdata keys
 * (ns unregister pruning + re-prio refold targets; entries vanish on
 * Tree gc). sp.compositor() creates a fresh state per call; trees of
 * different Compositors share nothing. */
typedef struct lst_Comp {
    cp_State   cp;        /* compositor state (inline, one per comp) */
    lua_State *L;         /* error context */
    int        ref_trees; /* registry: weak table of bound Tree userdata */
} lst_Comp;

typedef struct lst_Tree {
    sp_Tree   *T;      /* span storage + arbiter + edit sync */
    lua_State *L;      /* arb pcall context */
    cp_State  *cp;     /* bound Compositor's state (set at sp.new) */
    sv_List   *ephs;   /* vec: one sv_Span list per ephemeral ns */
    size_t     ephcnt; /* highest eph slot index + 1 */
    size_t     epoch;  /* tree edit counter (cursor invalidation) */
    cp_NSAttr *rtmp;   /* vec: styled fold scratch pairs */
} lst_Tree;

/* cursor mode: 0 = user handle (seek/locate/style/next/prev/edits),
 * LSP_ITER_SPAN = mark-stream iterator, LSP_ITER_STYLED = style-stream
 * iterator */
enum { LSP_ITER_SPAN = 1, LSP_ITER_STYLED };

typedef struct lst_Cur {
    sp_Cursor C;
    lst_Tree *tree;  /* current bound tree (epoch check fast path) */
    size_t    epoch; /* synced on create/seek/self-edit */
    size_t    endoff;
    size_t    mcur; /* stream iteration position (iterators only) */
    int       nsid; /* span filter (0 = any mark, > 0 = one ns) */
    int       mode; /* 0 = handle, LSP_ITER_* = iterator */
    size_t    midx; /* in-segment mark index (valid while mlen > 0) */
    size_t    mbase;
    size_t    mlen; /* current segment [mbase, mbase+mlen); 0 = none */
} lst_Cur;

/* ================================================================== */
/* stV_ block: malloc-backed vec (port of undotree.h utV_, lua_Alloc */
/* -> malloc/realloc/free). Shared by cp/sv/lst — keeps cp/sv free of */
/* any Lua allocf dependency; memory lives in the owning state and is */
/* freed whole by its __gc. OOM returns -1, callers luaL_error.      */
/* ================================================================== */

typedef struct {
    unsigned len, cap;
} stV_Header;

#define stV_len(A)      ((A) ? ((stV_Header *)(A) - 1)->len : 0u)
#define stV_hdr(A)      ((stV_Header *)(A) - 1)
#define stV_sz(cap, sz) (sizeof(stV_Header) + (size_t)(cap) * (sz))
#define stV_end(A)      ((A) + stV_len(A))
#define stV_init(A)     ((A) = NULL)

#define stV_free(A)       stV_resize((void **)&(A), 0u, sizeof(*(A)))
#define stV_reserve(A, N) stV_grow((void **)&(A), (unsigned)(N), sizeof(*(A)))
#define stV_keep(A, N)    stV_keep_((void **)&(A), (unsigned)(N), sizeof(*(A)))
#define stV_push(A, V)                         \
    (stV_grow((void **)&(A), 1u, sizeof(*(A))) \
             ? -1                              \
             : (*stV_end(A) = (V), ++stV_hdr(A)->len, 0))

static int stV_resize(void **pA, unsigned cap, size_t objsz) {
    stV_Header *hdr, *old = (*pA ? (stV_Header *)*pA - 1 : NULL);
    if (old == NULL && cap == 0) return 0;
    if (cap == 0) return free(old), *pA = NULL, 0;
    hdr = (stV_Header *)realloc(old, stV_sz(cap, objsz));
    if (hdr == NULL) return -1;
    if (old == NULL) hdr->len = 0;
    hdr->cap = cap;
    *pA = hdr + 1;
    return 0;
}

static int stV_grow(void **pA, unsigned need, size_t objsz) {
    stV_Header *hdr = (*pA ? (stV_Header *)*pA - 1 : NULL);
    unsigned    c = 0, e = need, nc = 4;
    if (hdr) c = hdr->cap, e += hdr->len;
    if (c >= e) return 0;
    while (nc < e && nc < INT_MAX / objsz) nc += nc >> 1;
    if (nc < e) return -1;
    return stV_resize(pA, nc, objsz);
}

static int stV_keep_(void **pA, unsigned need, size_t objsz) {
    stV_Header *hdr = (*pA ? (stV_Header *)*pA - 1 : NULL);
    unsigned    c = hdr ? hdr->cap : 0;
    if (need == 0) return 0;
    if (need > c && stV_resize(pA, need, objsz) != 0) return -1;
    hdr = (stV_Header *)*pA - 1;
    if (hdr->len < need) hdr->len = need;
    return 0;
}

/* ---- state singleton (module-wide sp_State, lua_Alloc adapted) ---- */

static int Lstate_gc(lua_State *L) {
    lsp_State *S = (lsp_State *)lua_touserdata(L, 1);
    if (S->S) sp_close(S->S), S->S = NULL;
    return 0;
}

static sp_State *lst_state(lua_State *L) {
    lsp_State *S;
    void      *ud;
    lua_Alloc  f;
    if (lua53_rawgetp(L, LUA_REGISTRYINDEX, LSP_STATE_KEY) != LUA_TNIL) {
        S = (lsp_State *)lua_touserdata(L, -1);
        return lua_pop(L, 1), S->S;
    }
    lua_pop(L, 1);
    S = (lsp_State *)lua_newuserdata(L, sizeof(lsp_State));
    f = lua_getallocf(L, &ud);
    S->S = sp_open((sp_Alloc *)f, ud);
    if (luaL_newmetatable(L, LSP_STATE_TYPE)) {
        lua_pushcfunction(L, Lstate_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_setmetatable(L, -2);
    return lua_rawsetp(L, LUA_REGISTRYINDEX, LSP_STATE_KEY), S->S;
}

/* ---- registry table helpers ---- */

static void lst_setuv(lua_State *L, const char *key) {
    lua_newtable(L);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, key);
    lua_setuservalue(L, -2);
}

static int lst_checkerror(lua_State *L, int r) {
    if (r) luaL_error(L, "spantree: error(%d)", r);
    return r;
}

/* ================================================================== */
/* cp_ block: compositor library — attr intern + ns registry + op  */
/* space + composite id system. Public API cp_*, internal cpX_*   */
/* (design_spantree_lua.md §3.6). One cp_State per Compositor     */
/* userdata; vecs are stV_ (malloc, zero Lua allocf).             */
/* ================================================================== */

/* default canon whitelist: SGR field set + vtext payload, sorted */
static const char *cp_defaults[] = {
        "bg",      "bold",      "dim",    "fg",    "italic",
        "reverse", "underline", "vstyle", "vtext",
};

static void     cp_resetfields(cp_State *S);
static int      cp_addfield(cp_State *S, const char *name);
static unsigned cp_internattr(lua_State *L, cp_State *S, int attr);

static void cp_init(lua_State *L, cp_State *S) {
    int i, nf = (int)(sizeof(cp_defaults) / sizeof(cp_defaults[0]));
    lua_newtable(L), S->ref_byattr = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), S->ref_nsb = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), S->ref_byop = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), S->ref_chainhash = luaL_ref(L, LUA_REGISTRYINDEX);
    S->nsnext = 1, S->ephnext = SP_MASK_BITS + 1;
    for (i = 0; i < nf; ++i)
        if (cp_addfield(S, cp_defaults[i]) != 0)
            luaL_error(L, "spantree: out of memory");
    lua_newtable(L);
    cp_internattr(L, S, lua_gettop(L));
    lua_pop(L, 1);
}

static void cp_free(lua_State *L, cp_State *S) {
    size_t i;
    for (i = 0; i < stV_len(S->attrs); ++i)
        luaL_unref(L, LUA_REGISTRYINDEX, S->attrs[i]);
    stV_free(S->attrs);
    free(S->fields);
    stV_free(S->ensreg);
    stV_free(S->ops);
    stV_free(S->slots);
    luaL_unref(L, LUA_REGISTRYINDEX, S->ref_byattr);
    luaL_unref(L, LUA_REGISTRYINDEX, S->ref_nsb);
    luaL_unref(L, LUA_REGISTRYINDEX, S->ref_byop);
    luaL_unref(L, LUA_REGISTRYINDEX, S->ref_chainhash);
}

/* ---- cp style service ---- */

static void cp_resetfields(cp_State *S) {
    free(S->fields), S->fields = NULL, S->nfields = 0;
}

static int cp_addfield(cp_State *S, const char *name) {
    const char *p = S->fields;
    size_t      n = S->nfields, i, cur = 0, nl = strlen(name);
    char       *buf;
    for (i = 0; i < n; ++i, p += strlen(p) + 1) {
        if (strcmp(p, name) == 0) return 0; /* duplicate: no-op */
        cur += strlen(p) + 1;
    }
    buf = (char *)malloc(cur + nl + 1);
    if (buf == NULL) return -1;
    if (cur > 0) memcpy(buf, S->fields, cur);
    memcpy(buf + cur, name, nl + 1);
    free(S->fields), S->fields = buf, S->nfields = (unsigned)n + 1;
    return 0;
}

static void cpC_canon(lua_State *L, cp_State *S, int attr) {
    luaL_Buffer b;
    const char *p = S->fields;
    int         n = 0, i;
    luaL_buffinit(L, &b);
    for (i = 0; i < (int)S->nfields; ++i) {
        size_t plen = strlen(p);
        lua_pushstring(L, p);
        lua_rawget(L, attr);
        switch (lua_type(L, -1)) {
        case LUA_TNIL: lua_pop(L, 1); break; /* absent field: skip */
        case LUA_TBOOLEAN:
            if (n++ > 0) luaL_addchar(&b, ',');
            luaL_addstring(&b, p), luaL_addchar(&b, ':');
            luaL_addchar(&b, lua_toboolean(L, -1) ? 'T' : 'F'), lua_pop(L, 1);
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

static cp_Attr cpL_lookup(lua_State *L, int tab, unsigned *next, int *isnew) {
    cp_Attr id;
    lua_pushvalue(L, -1);
    lua_rawget(L, tab);
    if (!lua_isnil(L, -1)) {
        id = (cp_Attr)lua_tointeger(L, -1);
        lua_pop(L, 2);
        *isnew = 0;
        return id;
    }
    lua_pop(L, 1);
    id = (*next)++;
    lua_pushinteger(L, (lua_Integer)id);
    lua_rawset(L, tab);
    *isnew = 1;
    return id;
}

static cp_Attr cp_internattr(lua_State *L, cp_State *S, int attr) {
    cp_Attr id;
    int     isnew, ok, tab;
    if (luaL_getmetafield(L, attr, "__hash") != LUA_TNIL) {
        lua_pushvalue(L, attr);
        lua_call(L, 1, 1);
        ok = lua_type(L, -1) == LUA_TSTRING;
        luaL_argcheck(L, ok, attr, "__hash must return a string");
        lua_pushliteral(L, "h:");
        lua_insert(L, -2);
        lua_concat(L, 2);
    } else {
        cpC_canon(L, S, attr);
        lua_pushliteral(L, "a:");
        lua_insert(L, -2);
        lua_concat(L, 2);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, S->ref_byattr);
    lua_insert(L, -2);
    tab = lua_gettop(L) - 1;
    id = cpL_lookup(L, tab, &S->next, &isnew);
    lua_pop(L, 1);
    if (!isnew) return id;
    if (stV_reserve(S->attrs, S->next) != 0)
        luaL_error(L, "spantree: out of memory");
    if (stV_len(S->attrs) < S->next) stV_hdr(S->attrs)->len = S->next;
    lua_pushvalue(L, attr);
    S->attrs[id] = luaL_ref(L, LUA_REGISTRYINDEX);
    return id;
}

static int     cp_expand(const cp_State *S, sp_Id id, cp_NSAttr *ps);
static cp_Attr cpF_fold(lua_State *L, cp_State *S, const cp_NSAttr *p, int n);
static void    cp_refup(cp_State *S, sp_Id id);
static void    cp_refdown(lua_State *L, cp_State *S, sp_Id id);

static void cp_attr(lua_State *L, cp_State *S, sp_Id id) {
    cp_NSAttr ps[SP_MASK_BITS + 1];
    int       n;
    cp_Attr   aid;
    if (id < (sp_Id)stV_len(S->ops) && S->ops[id].kind != 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, S->attrs[S->ops[id].attr]);
        return;
    }
    if (id < (sp_Id)S->next) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, S->attrs[id]);
        return;
    }
    if (id >= CP_COMP_START && (size_t)(id - CP_COMP_START) < stV_len(S->slots)
        && (n = cp_expand(S, id, ps)) > 0) {
        aid = cpF_fold(L, S, ps, n);
        lua_rawgeti(L, LUA_REGISTRYINDEX, S->attrs[aid]);
        return;
    }
    lua_pushnil(L);
}

/* ---- cp op space ---- */

static cp_Op cp_opmake(cp_Kind k, cp_NS ns, cp_Attr a) {
    cp_Op op;
    return op.kind = k, op.ns = ns, op.attr = a, op;
}

static sp_Id cpO_opkey(lua_State *L, cp_State *S, cp_Op op) {
    sp_Id id;
    int   isnew, tab;
    lua_rawgeti(L, LUA_REGISTRYINDEX, S->ref_byop);
    lua_insert(L, -2);
    tab = lua_gettop(L) - 1;
    id = cpL_lookup(L, tab, &S->next, &isnew);
    lua_pop(L, 1);
    if (!isnew) return id;
    if (stV_keep(S->ops, (unsigned)id + 1) != 0)
        luaL_error(L, "spantree: out of memory");
    S->ops[id] = op;
    return id;
}

static sp_Id cp_op(lua_State *L, cp_State *S, cp_Op op) {
    char nb[48];
    if (op.kind == CP_K_REORDER) {
        lua_pushliteral(L, "r");
    } else if (op.kind == CP_K_WRITE) {
        snprintf(nb, sizeof(nb), "w:%u:%u", (unsigned)op.ns, op.attr);
        lua_pushstring(L, nb);
    } else {
        snprintf(nb, sizeof(nb), "c:%u", (unsigned)op.ns);
        lua_pushstring(L, nb);
    }
    return cpO_opkey(L, S, op);
}

/* ---- cp composite region ---- */

static int cp_expand(const cp_State *S, sp_Id id, cp_NSAttr *ps) {
    sp_Id     cur, b;
    size_t    k;
    cp_NSAttr v;
    int       n = 0, i;
    cur = id;
    while (cur >= CP_COMP_START) {
        k = (size_t)(cur - CP_COMP_START);
        b = S->slots[k].b;
        ps[n].ns = (cp_NS)S->ops[b].ns, ps[n].attr = S->ops[b].attr;
        n += 1;
        cur = S->slots[k].a;
    }
    if (cur < (sp_Id)stV_len(S->ops) && S->ops[cur].kind != 0)
        ps[n].ns = (cp_NS)S->ops[cur].ns, ps[n].attr = S->ops[cur].attr;
    else
        ps[n].ns = 0, ps[n].attr = (cp_Attr)cur;
    n += 1;
    for (i = 0; i < n / 2; ++i)
        v = ps[i], ps[i] = ps[n - 1 - i], ps[n - 1 - i] = v;
    return n;
}

static void cp_foldinto(lua_State *L, int fold) {
    int tab = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, tab)) {
        lua_pushvalue(L, lua_gettop(L) - 1);
        lua_pushvalue(L, -2);
        lua_rawset(L, fold);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void cp_foldover(lua_State *L, cp_State *S, sp_Id id, int fold) {
    cp_attr(L, S, id);
    cp_foldinto(L, fold);
}

static cp_Attr cpF_fold(lua_State *L, cp_State *S, const cp_NSAttr *p, int n) {
    int     i, fold = lua_gettop(L) + 1;
    cp_Attr id;
    lua_newtable(L);
    for (i = 0; i < n; ++i) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, S->attrs[p[i].attr]);
        cp_foldinto(L, fold);
    }
    id = cp_internattr(L, S, fold);
    lua_pop(L, 1);
    return id;
}

static int cpP_hashkey(const cp_NSAttr *ps, int n, char *buf, size_t cap) {
    size_t off = 0;
    int    i, w;
    for (i = 0; i < n; ++i) {
        w = snprintf(buf + off, cap - off, "%u:%u;", ps[i].ns, ps[i].attr);
        off += (size_t)w;
    }
    return (int)off;
}

static sp_Id cp_composite(lua_State *L, cp_State *S, cp_NSAttr *p, int n) {
    char   key[1400];
    int    i;
    sp_Id  a, b, id;
    size_t k;
    cpP_hashkey(p, n, key, sizeof(key));
    lua_rawgeti(L, LUA_REGISTRYINDEX, S->ref_chainhash);
    lua_pushstring(L, key);
    lua_rawget(L, -2);
    id = lua_isnil(L, -1) ? 0 : (sp_Id)lua_tointeger(L, -1);
    if (id != 0
        && (S->slots[(size_t)(id - CP_COMP_START)].a != 0
            || S->slots[(size_t)(id - CP_COMP_START)].b != 0)) {
        lua_pop(L, 2);
        return id;
    }
    lua_pop(L, 1);
    if (p[0].ns == 0)
        a = p[0].attr;
    else
        a = cp_op(L, S, cp_opmake(CP_K_WRITE, p[0].ns, p[0].attr));
    for (i = 0; i < n - 1; ++i) {
        b = cp_op(L, S, cp_opmake(CP_K_WRITE, p[i + 1].ns, p[i + 1].attr));
        if (S->freehead > 0)
            k = (size_t)(S->freehead - 1), S->freehead = S->slots[k].idfree + 1;
        else {
            k = S->chainnext++;
            if (stV_keep(S->slots, (unsigned)k + 1) != 0)
                luaL_error(L, "spantree: out of memory");
        }
        S->slots[k].a = a, S->slots[k].b = b, S->slots[k].refcnt = 0;
        a = (sp_Id)(k + CP_COMP_START);
        if (i < n - 2) cp_refup(S, a);
    }
    lua_pushstring(L, key), lua_pushinteger(L, (lua_Integer)a);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return a;
}

static void cpP_release(lua_State *L, cp_State *S, sp_Id id) {
    cp_NSAttr ps[SP_MASK_BITS + 1];
    char      key[1400];
    size_t    k = (size_t)(id - CP_COMP_START);
    sp_Id     a = S->slots[k].a, b = S->slots[k].b;
    int       n;
    if (a == 0 && b == 0) return;
    n = cp_expand(S, id, ps);
    cpP_hashkey(ps, n, key, sizeof(key));
    lua_rawgeti(L, LUA_REGISTRYINDEX, S->ref_chainhash);
    lua_pushstring(L, key);
    lua_pushnil(L);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    S->slots[k].a = S->slots[k].b = 0;
    S->slots[k].idfree = S->freehead - 1;
    S->freehead = (unsigned)k + 1;
    cp_refdown(L, S, a);
    cp_refdown(L, S, b);
}

static void cp_refup(cp_State *S, sp_Id id) {
    if (id < CP_COMP_START) return;
    S->slots[(size_t)(id - CP_COMP_START)].refcnt += 1;
}

static void cp_refdown(lua_State *L, cp_State *S, sp_Id id) {
    if (id < CP_COMP_START) return;
    if (S->slots[(size_t)(id - CP_COMP_START)].refcnt == 0) return;
    S->slots[(size_t)(id - CP_COMP_START)].refcnt -= 1;
    if (S->slots[(size_t)(id - CP_COMP_START)].refcnt == 0)
        cpP_release(L, S, id);
}

/* ---- cp ns registry ---- */

static cp_NSReg *cpP_nsreg(cp_State *S, cp_NS ns) {
    if (ns > (int)SP_MASK_BITS) return S->ensreg + (ns - SP_MASK_BITS - 1);
    return S->nsreg + ns;
}

static lua_Number cp_prio(const cp_State *S, cp_NS ns) {
    if (ns > (int)SP_MASK_BITS) return S->ensreg[ns - SP_MASK_BITS - 1].prio;
    return S->nsreg[ns].prio;
}

static unsigned cp_reg(const cp_State *S, cp_NS ns) {
    if (ns > (int)SP_MASK_BITS) return S->ensreg[ns - SP_MASK_BITS - 1].reg;
    return S->nsreg[ns].reg;
}

static cp_NS cpN_salloc(lua_State *L, cp_State *S, int eph) {
    if (eph) {
        if (stV_keep(S->ensreg, S->ephnext - SP_MASK_BITS) != 0)
            return luaL_error(L, "spantree: out of memory"), 0;
        return (cp_NS)S->ephnext++;
    }
    if (S->nsstackn > 0) return (cp_NS)S->nsstack[--S->nsstackn];
    if (S->nsnext > SP_MASK_BITS)
        return luaL_error(L, "spantree: namespace limit reached"), 0;
    return (cp_NS)S->nsnext++;
}

static cp_NS cp_nsget(lua_State *L, cp_State *S, const char *name) {
    cp_NS ns = 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, S->ref_nsb);
    lua_pushstring(L, name);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) ns = (cp_NS)lua_tointeger(L, -1);
    return lua_pop(L, 2), ns;
}

static void cpN_sset(lua_State *L, cp_State *S, const char *name, cp_NS ns) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, S->ref_nsb);
    lua_pushstring(L, name);
    lua_pushinteger(L, ns);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

static void cpN_sdel(lua_State *L, cp_State *S, const char *name) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, S->ref_nsb);
    lua_pushstring(L, name);
    lua_pushnil(L);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

/* one ns registration argument set */
typedef struct cp_NsArg {
    const char *nm;
    lua_Number  p;
    int         e;
} cp_NsArg;

static int cp_nsreg(lua_State *L, cp_State *S, const cp_NsArg *a) {
    cp_NS ns = cp_nsget(L, S, a->nm);
    if (ns != 0) {
        cpP_nsreg(S, ns)->prio = a->p;
        return 1;
    }
    ns = cpN_salloc(L, S, a->e);
    cpN_sset(L, S, a->nm, ns);
    cpP_nsreg(S, ns)->prio = a->p;
    cpP_nsreg(S, ns)->reg = ++S->regcnt;
    return 0;
}

static lua_Number cp_nsunreg(lua_State *L, cp_State *S, const char *nm) {
    cp_NS      ns = cp_nsget(L, S, nm);
    lua_Number old;
    if (ns == 0) return luaL_error(L, "spantree: unknown namespace"), 0;
    old = cp_prio(S, ns);
    if (ns > (cp_NS)SP_MASK_BITS) {
        if ((unsigned)ns + 1 == S->ephnext) S->ephnext -= 1;
    } else {
        S->nsstack[S->nsstackn++] = ns;
    }
    cpN_sdel(L, S, nm);
    cpP_nsreg(S, ns)->prio = 0, cpP_nsreg(S, ns)->reg = 0;
    return old;
}

static int cpS_less(const cp_State *S, cp_NSAttr a, cp_NSAttr b) {
    if (a.ns == 0) return b.ns != 0;
    if (b.ns == 0) return 0;
    if (cp_prio(S, a.ns) != cp_prio(S, b.ns))
        return cp_prio(S, a.ns) < cp_prio(S, b.ns);
    return cp_reg(S, a.ns) < cp_reg(S, b.ns);
}

static void cp_sort(const cp_State *S, cp_NSAttr *ps, int n) {
    int       i, j;
    cp_NSAttr v;
    for (i = 1; i < n; ++i) {
        v = ps[i];
        for (j = i; j > 0 && cpS_less(S, v, ps[j - 1]); --j) ps[j] = ps[j - 1];
        ps[j] = v;
    }
}

static void cp_apply(const cp_State *S, sp_Id in, cp_NSAttr *tmp, int *pn) {
    int i, n = *pn;
    if (S->ops[in].kind == CP_K_WRITE) {
        for (i = 0; i < n && tmp[i].ns != (cp_NS)S->ops[in].ns; ++i) continue;
        if (i < n)
            tmp[i].attr = S->ops[in].attr;
        else
            tmp[n].ns = (cp_NS)S->ops[in].ns, tmp[n].attr = S->ops[in].attr,
            n += 1;
    } else if (S->ops[in].kind == CP_K_CLEAR) {
        for (i = 0; i < n && tmp[i].ns != (cp_NS)S->ops[in].ns; ++i) continue;
        if (i < n) tmp[i] = tmp[n - 1], n -= 1;
    }
    *pn = n;
}

static void sv_cover(const sv_Span *S, size_t x, sv_Span *out);

static size_t cp_split(const cp_State *S, cp_NSAttr *rt, size_t n) {
    size_t    i, nb = 0;
    cp_NSAttr v;
    for (i = 0; i < n; ++i)
        if (cp_prio(S, rt[i].ns) >= 0) {
            v = rt[i];
            rt[i] = rt[nb];
            rt[nb] = v;
            nb += 1;
        }
    return nb;
}

/* ================================================================== */
/* sv_ block: minispan library — flat segment vec per ephemeral ns    */
/* (ordered, disjoint, adjacent ids differ; no mask, no B+ structure).*/
/* Public API sv_*, internal svX_* (§3.6). The vec is stV_ (malloc,   */
/* zero Lua allocf); segment count = stV_len(S). Writers take the vec */
/* handle sv_List * (grow reallocs the handle), readers sv_Span *.   */
/* ================================================================== */

static size_t sv_upper(const sv_Span *S, size_t x) {
    size_t lo = 0, hi = stV_len(S);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (S[mid].off + S[mid].len <= x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static size_t sv_lower(const sv_Span *S, size_t x) {
    size_t lo = 0, hi = stV_len(S);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (S[mid].off < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void svN_norm(sv_Span *S, size_t j) {
    size_t n = stV_len(S);
    if (j > 0 && S[j - 1].id == S[j].id
        && S[j - 1].off + S[j - 1].len == S[j].off) {
        S[j - 1].len += S[j].len;
        memmove(S + j, S + j + 1, (n - j - 1) * sizeof(sv_Span));
        n -= 1, j -= 1;
    }
    if (j + 1 < n && S[j].id == S[j + 1].id
        && S[j].off + S[j].len == S[j + 1].off) {
        S[j].len += S[j + 1].len;
        memmove(S + j + 1, S + j + 2, (n - j - 2) * sizeof(sv_Span));
        n -= 1;
    }
    stV_hdr(S)->len = (unsigned)n;
}

static int sv_fill(sv_List *S, size_t off, size_t len, cp_Attr id) {
    sv_Span *p = *S;
    size_t   s, t, ohlen, n;
    cp_Attr  ohid;
    int      left, right;
    if (len == 0) return 0;
    s = sv_upper(p, off), t = sv_lower(p, off + len);
    if (stV_reserve(*S, 2) != 0) return -1;
    p = *S;
    n = stV_len(p);
    left = s < n && p[s].off < off;
    right = t > 0 && p[t - 1].off + p[t - 1].len > off + len;
    ohid = right ? p[t - 1].id : 0;
    ohlen = right ? p[t - 1].off + p[t - 1].len - (off + len) : 0;
    if (left) p[s].len = off - p[s].off;
    s += left;
    memmove(p + s, p + t, (n - t) * sizeof(sv_Span));
    n = s + (n - t);
    if (right) {
        memmove(p + s + 2, p + s, (n - s) * sizeof(sv_Span));
        p[s].off = off, p[s].len = len, p[s].id = id;
        p[s + 1].off = off + len, p[s + 1].len = ohlen, p[s + 1].id = ohid;
        n += 2;
    } else {
        memmove(p + s + 1, p + s, (n - s) * sizeof(sv_Span));
        p[s].off = off, p[s].len = len, p[s].id = id;
        n += 1;
    }
    stV_hdr(p)->len = (unsigned)n;
    svN_norm(p, s);
    return 0;
}

static void sv_clear(sv_Span *S, size_t off, size_t len) {
    size_t s, t, n = stV_len(S);
    int    left, right;
    if (len == 0) return;
    s = sv_upper(S, off), t = sv_lower(S, off + len);
    if (s == t) return;
    left = S[s].off < off;
    right = S[t - 1].off + S[t - 1].len > off + len;
    if (left && right && t == s + 1) {
        memmove(S + t + 1, S + t, (n - t) * sizeof(sv_Span));
        S[t].off = off + len;
        S[t].len = S[s].off + S[s].len - (off + len);
        S[t].id = S[s].id;
        S[s].len = off - S[s].off;
        stV_hdr(S)->len = (unsigned)(n + 1);
        return;
    }
    if (left) S[s].len = off - S[s].off, s += 1;
    if (right) {
        S[t - 1].len = S[t - 1].off + S[t - 1].len - (off + len);
        S[t - 1].off = off + len, t -= 1;
    }
    if (s >= t) return;
    memmove(S + s, S + t, (n - t) * sizeof(sv_Span));
    stV_hdr(S)->len = (unsigned)(n - (t - s));
}

static void sv_cover(const sv_Span *S, size_t x, sv_Span *out) {
    size_t s = sv_upper(S, x);
    if (s < stV_len(S) && S[s].off <= x) {
        *out = S[s];
    } else {
        out->off = x, out->len = 0, out->id = 0;
    }
}

static void sv_clamp(const sv_Span *ph, size_t x, size_t *ps, size_t *pe) {
    size_t n = stV_len(ph), s0, ee;
    if (n == 0) return;
    s0 = sv_upper(ph, x);
    if (s0 >= n) {
        ee = ph[n - 1].off + ph[n - 1].len;
        if (ee > *ps) *ps = ee;
    } else if (ph[s0].off > x) {
        if (ph[s0].off < *pe) *pe = ph[s0].off;
        if (s0 > 0) {
            ee = ph[s0 - 1].off + ph[s0 - 1].len;
            if (ee > *ps) *ps = ee;
        }
    } else {
        if (ph[s0].off > *ps) *ps = ph[s0].off;
        ee = ph[s0].off + ph[s0].len;
        if (ee < *pe) *pe = ee;
    }
}

static size_t sv_coverpairs(sv_List *e, size_t n, size_t x, cp_NSAttr *rt) {
    size_t i, c = 0;
    for (i = 0; i < n; ++i) {
        const sv_Span *ph = e[i];
        sv_Span        cv;
        if (stV_len(ph) == 0) continue;
        sv_cover(ph, x, &cv);
        if (cv.id == 0) continue;
        rt[c].ns = (cp_NS)(SP_MASK_BITS + 1 + i);
        rt[c].attr = cv.id;
        c += 1;
    }
    return c;
}

static void sv_reset(sv_Span *S) {
    if (S) stV_hdr(S)->len = 0;
}

/* ================================================================== */
/* lst block: binding integration (cp + sv + sp into the Lua API).    */
/* ================================================================== */

/* ---- tree helpers ---- */

#define lst_iseph(ns)  ((ns) > (int)SP_MASK_BITS)
#define lst_ephidx(ns) ((size_t)((ns) - (int)SP_MASK_BITS - 1))

static lst_Comp *lcomp_check(lua_State *L, int idx) {
    return (lst_Comp *)luaL_checkudata(L, idx, LSP_COMP_TYPE);
}

static lst_Tree *ltree_check(lua_State *L, int idx) {
    return (lst_Tree *)luaL_checkudata(L, idx, LSP_TREE_TYPE);
}

static lst_Cur *lcur_check(lua_State *L, int idx) {
    lst_Cur *c = (lst_Cur *)luaL_checkudata(L, idx, LSP_CUR_TYPE);
    if (c->tree == NULL || c->tree->epoch != c->epoch)
        return luaL_error(L, LSP_EPOCH_MSG), NULL;
    return c;
}

static int lst_nsid(lua_State *L, lst_Tree *t, int idx) {
    int ns;
    if (lua_isnoneornil(L, idx)) return 0;
    luaL_argcheck(
            L, lua_type(L, idx) == LUA_TSTRING, idx,
            "spantree: namespace name must be a string");
    luaL_argcheck(
            L, lua_rawlen(L, idx) > 0, idx, "spantree: invalid namespace name");
    lua_rawgeti(L, LUA_REGISTRYINDEX, t->cp->ref_nsb);
    lua_pushvalue(L, idx);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1))
        return luaL_error(L, "spantree: unknown namespace"), 0;
    ns = (int)lua_tointeger(L, -1);
    lua_pop(L, 2); /* id, table */
    return ns;
}

static sv_List *lst_ephslot(lua_State *L, lst_Tree *t, cp_NS ns) {
    size_t idx = lst_ephidx(ns), old = stV_len(t->ephs);
    if (idx + 1 > t->ephcnt) t->ephcnt = idx + 1;
    if (stV_keep(t->ephs, (unsigned)idx + 1) != 0)
        luaL_error(L, "spantree: out of memory");
    while (old < idx + 1) t->ephs[old++] = NULL;
    return &t->ephs[idx];
}

static void lst_ephresetall(lst_Tree *t) {
    size_t i;
    for (i = 0; i < t->ephcnt; ++i) sv_reset(t->ephs[i]);
}

/* ---- merge core ---- */

static int lst_merge(lua_State *L) {
    lst_Tree *t = (lst_Tree *)lua_touserdata(L, 1);
    sp_Id     in = (sp_Id)luaL_checkinteger(L, 2);
    sp_Id     old = (sp_Id)luaL_checkinteger(L, 3);
    cp_NSAttr ps[SP_MASK_BITS + 1];
    sp_Id     ret;
    int       n, k = 0, i;
    n = cp_expand(t->cp, old, ps);
    cp_apply(t->cp, in, ps, &n);
    if (n == 0) {
        lua_pushinteger(L, 0);
        lua_pushinteger(L, 0);
        return 2;
    }
    cp_sort(t->cp, ps, n);
    if (n == 1 && ps[0].ns == 0)
        ret = ps[0].attr;
    else if (n == 1)
        ret = cp_op(L, t->cp, cp_opmake(CP_K_WRITE, ps[0].ns, ps[0].attr));
    else
        ret = cp_composite(L, t->cp, ps, n);
    lua_pushinteger(L, (lua_Integer)ret);
    for (i = 0; i < n; ++i)
        if (ps[i].ns > 0) k += 1;
    lua_pushinteger(L, k);
    for (i = 0; i < n; ++i)
        if (ps[i].ns > 0) lua_pushinteger(L, ps[i].ns);
    return k + 2;
}

static sp_Mask lst_segmask(cp_State *S, sp_Id id) {
    cp_NSAttr ps[SP_MASK_BITS + 1];
    sp_Mask   m = 0;
    int       n, i;
    if (id >= CP_COMP_START) {
        n = cp_expand(S, id, ps);
        for (i = 0; i < n; ++i)
            if (ps[i].ns > 0) sp_addns(&m, (int)ps[i].ns);
    } else if (id < (sp_Id)stV_len(S->ops) && S->ops[id].kind != 0) {
        if (S->ops[id].ns > 0) sp_addns(&m, (int)S->ops[id].ns);
    }
    return m;
}

static sp_Id lst_arb(void *ud, sp_Id in, sp_Id old, sp_Mask *mask) {
    lst_Tree  *t = (lst_Tree *)ud;
    lua_State *L = t->L;
    sp_Id      ret;
    int        n, i, base;
    if (in == 0) { /* pad (old == 0) / death (old != 0) */
        if (old == 0) return *mask = 0, 0;
        cp_refdown(L, t->cp, old);
        return *mask = 0, 0;
    }
    if (old == 0) { /* birth: CLEAR/REORDER on an empty slot is a no-op */
        if (in < (sp_Id)stV_len(t->cp->ops)
            && t->cp->ops[in].kind != CP_K_WRITE)
            return *mask = 0, 0;
        cp_refup(t->cp, in);
        return *mask = lst_segmask(t->cp, in), in;
    }
    base = lua_gettop(L);
    lua_pushcfunction(L, lst_merge);
    lua_pushlightuserdata(L, ud);
    lua_pushinteger(L, (lua_Integer)in);
    lua_pushinteger(L, (lua_Integer)old);
    if (lua_pcall(L, 3, LUA_MULTRET, 0) != 0)
        return lua_settop(L, base), old; /* segment keeps old, mask untouched */
    ret = (sp_Id)lua_tointeger(L, base + 1);
    if (ret != 0) cp_refup(t->cp, ret);
    cp_refdown(L, t->cp, old); /* ret == 0 = the old segment died */
    if (ret == 0) return *mask = 0, lua_settop(L, base), 0;
    n = (int)lua_tointeger(L, base + 2);
    *mask = 0;
    for (i = 0; i < n; ++i) sp_addns(mask, (int)lua_tointeger(L, base + 3 + i));
    return lua_settop(L, base), ret;
}

/* ---- styled flow ---- */

static sp_Id lst_fold(lua_State *L, lst_Tree *t, sp_Id id, size_t b, size_t a) {
    cp_NSAttr *rt = t->rtmp;
    int        fold;
    size_t     k;
    sp_Id      rid;
    lua_newtable(L);
    fold = lua_gettop(L);
    cp_sort(t->cp, rt, (int)b);
    for (k = 0; k < b; ++k) cp_foldover(L, t->cp, rt[k].attr, fold);
    cp_foldover(L, t->cp, id, fold);
    cp_sort(t->cp, rt + b, (int)a);
    for (k = 0; k < a; ++k) cp_foldover(L, t->cp, rt[b + k].attr, fold);
    rid = cp_internattr(L, t->cp, fold);
    lua_pop(L, 1);
    return rid;
}

static sp_Id lst_sty(lua_State *L, lst_Cur *c, size_t *pe) {
    lst_Tree  *t = c->tree;
    sp_Cursor *C = &c->C;
    size_t     x = c->mcur, ts, te, rem, need, nb = 0, na = 0, tot, i;
    sp_Id      treeid;
    if (x >= sp_bytes(t->T)) return *pe = x, 0;
    for (;;) {
        treeid = sp_style(C, &rem, NULL);
        te = C->off + C->poff + rem;
        if (x < te) break;
        sp_next(C, 0, &rem);
    }
    ts = C->off;
    for (i = 0; i < t->ephcnt; ++i) sv_clamp(t->ephs[i], x, &ts, &te);
    *pe = te;
    need = t->ephcnt + 1;
    if (stV_keep(t->rtmp, (unsigned)need) != 0)
        luaL_error(L, "spantree: out of memory");
    tot = sv_coverpairs(t->ephs, t->ephcnt, x, t->rtmp);
    nb = cp_split(t->cp, t->rtmp, tot);
    na = tot - nb;
    if (nb + na == 0) return treeid;
    return lst_fold(L, t, treeid, nb, na);
}

/* ---- mark flow ---- */

static sv_Span lst_span(size_t off, size_t len, sp_Id id) {
    sv_Span s;
    return s.off = off, s.len = len, s.id = (cp_Attr)id, s;
}

static int lst_pushspan(lua_State *L, lst_Tree *t, sv_Span s) {
    lua_pushinteger(L, (lua_Integer)s.off);
    lua_pushinteger(L, (lua_Integer)s.len);
    cp_attr(L, t->cp, s.id);
    lua_pushinteger(L, (lua_Integer)s.id);
    return 4;
}

/* ---- edit helper ---- */

static void lst_edit(lua_State *L, lst_Cur *c) {
    c->tree->epoch += 1;
    c->epoch = c->tree->epoch;
    (void)L;
}

/* ---- ns lifecycle (Compositor-side: every bound tree) ---- */

typedef void (*lst_walkf)(lua_State *L, lst_Tree *t, void *ctx);

static void lst_compwalk(lua_State *L, lst_Comp *c, lst_walkf fn, void *ctx) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->ref_trees);
    lua_pushnil(L);
    while (lua_next(L, -2)) {
        lst_Tree *t = (lst_Tree *)lua_touserdata(L, -2);
        if (t != NULL) fn(L, t, ctx);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static void lst_comprefold(lua_State *L, lst_Tree *t, void *ctx) {
    sp_Cursor C;
    sp_Id     op;
    (void)ctx;
    if (sp_bytes(t->T) == 0) return;
    sp_seek(&C, t->T, 0);
    op = cp_op(L, t->cp, cp_opmake(CP_K_REORDER, 0, 0));
    lst_checkerror(L, sp_fill(&C, op, sp_bytes(t->T)));
    t->epoch += 1;
}

static void lst_compprunens(lua_State *L, lst_Tree *t, void *ctx) {
    int   ns = *(int *)ctx;
    sp_Id op;
    op = cp_op(L, t->cp, cp_opmake(CP_K_CLEAR, ns, 0));
    lst_checkerror(L, sp_clear(t->T, ns, op));
    t->epoch += 1;
}

static void lst_compfreeeph(lua_State *L, lst_Tree *t, void *ctx) {
    cp_NS    ns = *(cp_NS *)ctx;
    sv_List *e = lst_ephslot(L, t, ns);
    (void)L;
    stV_free(*e);
    if (lst_ephidx(ns) + 1 == t->ephcnt) t->ephcnt = lst_ephidx(ns);
}

static lua_Number lst_compnsunreg(lua_State *L, lst_Comp *c, const char *name) {
    cp_State  *cp = &c->cp;
    cp_NS      ns = cp_nsget(L, cp, name);
    lua_Number old;
    if (ns == 0) return luaL_error(L, "spantree: unknown namespace"), 0;
    old = cp_prio(cp, ns);
    if (!lst_iseph(ns)) lst_compwalk(L, c, lst_compprunens, &ns);
    cp_nsunreg(L, cp, name);
    if (lst_iseph(ns)) lst_compwalk(L, c, lst_compfreeeph, &ns);
    return old;
}

/* ---- tree methods ---- */

static int Lcur_iter(lua_State *L);

static int Ltree_gc(lua_State *L) {
    lst_Tree *t = ltree_check(L, 1);
    size_t    i;
    (void)L;
    for (i = 0; i < stV_len(t->ephs); ++i) stV_free(t->ephs[i]);
    stV_free(t->ephs);
    stV_free(t->rtmp);
    if (t->T) sp_freetree(t->T), t->T = NULL;
    return 0;
}

static int Ltree_bytes(lua_State *L) {
    return lua_pushinteger(L, (lua_Integer)sp_bytes(ltree_check(L, 1)->T)), 1;
}

static int Lcomp_intern(lua_State *L) {
    lst_Comp *comp = lcomp_check(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    return lua_pushinteger(L, (lua_Integer)cp_internattr(L, &comp->cp, 2)), 1;
}

static int Lcomp_attr(lua_State *L) {
    lst_Comp   *comp = lcomp_check(L, 1);
    lua_Integer id = luaL_checkinteger(L, 2);
    cp_attr(L, &comp->cp, (sp_Id)id);
    return 1;
}

static int lst_fieldswrite(lua_State *L, cp_State *cp, int idx, int add) {
    int n, i;
    luaL_checktype(L, idx, LUA_TTABLE);
    n = (int)lua_rawlen(L, idx);
    if (!add) cp_resetfields(cp);
    for (i = 0; i < n; ++i) {
        lsp_rawgeti(L, idx, i + 1);
        luaL_checktype(L, -1, LUA_TSTRING); /* element must be a string */
        if (cp_addfield(cp, lua_tostring(L, -1)) != 0)
            return luaL_error(L, "spantree: out of memory"), 0;
    }
    lua_pop(L, n);
    return lua_settop(L, 1), 1;
}

static int Lcomp_fields(lua_State *L) {
    lst_Comp *comp = lcomp_check(L, 1);
    int       t = lua_type(L, 2);
    if (t == LUA_TNIL || t == LUA_TNONE) {
        const char *p = comp->cp.fields;
        unsigned    i;
        lua_createtable(L, comp->cp.nfields, 0);
        for (i = 0; i < comp->cp.nfields; ++i, p += strlen(p) + 1)
            lua_pushstring(L, p), lsp_rawseti(L, -2, i + 1);
        return 1;
    }
    if (t == LUA_TTABLE) return lst_fieldswrite(L, &comp->cp, 2, 0);
    luaL_argcheck(
            L, *luaL_checkstring(L, 2) == 'a', 2,
            "spantree: unknown fields mode");
    return lst_fieldswrite(L, &comp->cp, 3, 1);
}

static int Ltree_mark(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    int         nsid = lst_nsid(L, t, 2);
    lua_Integer off = luaL_checkinteger(L, 4);
    lua_Integer len = luaL_checkinteger(L, 5);
    sp_Cursor   C;
    sp_Id       op;
    unsigned    a;
    luaL_argcheck(L, off >= 0, 4, "spantree: invalid offset");
    luaL_argcheck(L, len >= 0, 5, "spantree: invalid length");
    if (lua_type(L, 3) == LUA_TTABLE)
        a = cp_internattr(L, t->cp, 3);
    else {
        a = (unsigned)luaL_checkinteger(L, 3);
        luaL_argcheck(L, a < t->cp->next, 3, "spantree: unknown style id");
    }
    if (lst_iseph(nsid)) {
        if (sv_fill(lst_ephslot(L, t, nsid), (size_t)off, (size_t)len, a) != 0)
            luaL_error(L, "spantree: out of memory");
        return lua_pushinteger(L, (lua_Integer)a), 1;
    }
    op = cp_op(L, t->cp, cp_opmake(CP_K_WRITE, nsid, a));
    sp_seek(&C, t->T, (size_t)off);
    lst_checkerror(L, sp_fill(&C, op, (size_t)len));
    t->epoch += 1;
    return lua_pushinteger(L, (lua_Integer)a), 1;
}

static int lst_cleartree(lua_State *L, lst_Tree *t, size_t off, size_t len) {
    sp_Cursor C;
    size_t    i;
    sp_seek(&C, t->T, off);
    lst_checkerror(L, sp_fill(&C, 0, len));
    for (i = 0; i < t->ephcnt; ++i) sv_clear(t->ephs[i], off, len);
    return lua_settop(L, 1), t->epoch += 1, 1;
}

static int Ltree_clear(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lua_Integer off = 0, len = 0;
    int         n = lua_gettop(L), nsid = 0;
    sp_Id       op = 0;
    sp_Cursor   C;
    if (n == 2) {
        nsid = lst_nsid(L, t, 2);
        if (lst_iseph(nsid))
            return sv_reset(*lst_ephslot(L, t, nsid)), lua_settop(L, 1), 1;
        op = cp_op(L, t->cp, cp_opmake(CP_K_CLEAR, nsid, 0));
        lst_checkerror(L, sp_clear(t->T, nsid, op));
        return lua_settop(L, 1), t->epoch += 1, 1;
    }
    if (n > 2) {
        nsid = lst_nsid(L, t, 2);
        off = luaL_checkinteger(L, 3), len = luaL_checkinteger(L, 4);
        luaL_argcheck(L, off >= 0, 3, "spantree: invalid offset");
        luaL_argcheck(L, len >= 0, 4, "spantree: invalid length");
        if (lst_iseph(nsid)) {
            sv_clear(*lst_ephslot(L, t, nsid), (size_t)off, (size_t)len);
            return lua_settop(L, 1), 1;
        }
        if (nsid > 0) op = cp_op(L, t->cp, cp_opmake(CP_K_CLEAR, nsid, 0));
    } else
        len = (lua_Integer)sp_bytes(t->T);
    if (nsid > 0) {
        sp_seek(&C, t->T, (size_t)off);
        lst_checkerror(L, sp_fill(&C, op, (size_t)len));
        return lua_settop(L, 1), t->epoch += 1, 1;
    }
    if (n > 2) return lst_cleartree(L, t, (size_t)off, (size_t)len);
    sp_seek(&C, t->T, (size_t)off);
    lst_checkerror(L, sp_fill(&C, 0, (size_t)len));
    lst_ephresetall(t);
    return lua_settop(L, 1), t->epoch += 1, 1;
}

static int Ltree_splice(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    lua_Integer del = luaL_checkinteger(L, 3);
    lua_Integer ins = luaL_checkinteger(L, 4);
    sp_Cursor   C;
    luaL_argcheck(L, off >= 0, 2, "spantree: invalid offset");
    luaL_argcheck(L, del >= 0, 3, "spantree: invalid length");
    luaL_argcheck(L, ins >= 0, 4, "spantree: invalid length");
    lst_ephresetall(t);
    sp_seek(&C, t->T, (size_t)off);
    lst_checkerror(L, sp_splice(&C, (size_t)del, (size_t)ins));
    t->epoch += 1;
    return lua_settop(L, 1), 1;
}

static int Ltree_append(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    lua_Integer ins = luaL_checkinteger(L, 3);
    sp_Cursor   C;
    luaL_argcheck(L, off >= 0, 2, "spantree: invalid offset");
    luaL_argcheck(L, ins >= 0, 3, "spantree: invalid length");
    lst_ephresetall(t);
    sp_seek(&C, t->T, (size_t)off);
    lst_checkerror(L, sp_append(&C, (size_t)ins));
    t->epoch += 1;
    return lua_settop(L, 1), 1;
}

static int Ltree_insert(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    lua_Integer ins = luaL_checkinteger(L, 3);
    sp_Cursor   C;
    luaL_argcheck(L, off >= 0, 2, "spantree: invalid offset");
    luaL_argcheck(L, ins >= 0, 3, "spantree: invalid length");
    lst_ephresetall(t);
    sp_seek(&C, t->T, (size_t)off);
    lst_checkerror(L, sp_insert(&C, (size_t)ins));
    t->epoch += 1;
    return lua_settop(L, 1), 1;
}

static int Ltree_remove(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    lua_Integer len = luaL_checkinteger(L, 3);
    sp_Cursor   A, B;
    luaL_argcheck(L, off >= 0, 2, "spantree: invalid offset");
    luaL_argcheck(L, len >= 0, 3, "spantree: invalid length");
    lst_ephresetall(t);
    sp_seek(&A, t->T, (size_t)off);
    sp_seek(&B, t->T, (size_t)(off + len));
    lst_checkerror(L, sp_remove(&A, &B));
    t->epoch += 1;
    return lua_settop(L, 1), 1;
}

static int Ltree_unmark(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    cp_NSAttr   ps[SP_MASK_BITS + 1];
    sp_Cursor   C;
    lua_Integer id = luaL_checkinteger(L, 2);
    size_t      count = 0, len;
    sp_Id       op;
    int         n, i;
    sp_seek(&C, t->T, 0);
    for (;;) { /* pass 1: count matching segments */
        sp_Id sid = sp_style(&C, &len, NULL);
        if (sid == 0 && len == 0) break;
        n = cp_expand(t->cp, sid, ps);
        for (i = 0; i < n && (lua_Integer)ps[i].attr != id; ++i) continue;
        if (i < n) count += 1;
        sp_next(&C, 0, NULL);
    }
    sp_seek(&C, t->T, 0);
    for (;;) { /* pass 2: per-seg CLEAR(ns); other marks in the ns live */
        sp_Id  sid = sp_style(&C, &len, NULL);
        size_t segstart;
        if (sid == 0 && len == 0) break;
        segstart = sp_offset(&C) - C.poff;
        n = cp_expand(t->cp, sid, ps);
        for (i = 0; i < n; ++i) {
            if ((lua_Integer)ps[i].attr != id) continue;
            sp_locate(&C, segstart);
            op = cp_op(L, t->cp, cp_opmake(CP_K_CLEAR, ps[i].ns, 0));
            lst_checkerror(L, sp_fill(&C, op, len));
        }
        sp_seek(&C, t->T, segstart + len); /* fills move the cursor; seek
                                            * to the next seg explicitly */
    }
    if (count > 0) t->epoch += 1;
    return lua_pushinteger(L, (lua_Integer)count), 1;
}

static int Ltree_span(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lst_Cur    *c;
    lua_Integer off, len;
    int         nsid = 0, top = lua_gettop(L), aoff = 2, alen = 3;
    if (top == 3) {
        off = luaL_checkinteger(L, 2);
        len = luaL_checkinteger(L, 3);
    } else {
        nsid = lst_nsid(L, t, 2);
        aoff = 3, alen = 4;
        off = luaL_checkinteger(L, 3);
        len = luaL_checkinteger(L, 4);
    }
    luaL_argcheck(L, off >= 0, aoff, "spantree: invalid offset");
    luaL_argcheck(L, len >= 0, alen, "spantree: invalid length");
    c = (lst_Cur *)lua_newuserdata(L, sizeof(lst_Cur));
    sp_seek(&c->C, t->T, (size_t)off);
    c->tree = t, c->epoch = t->epoch;
    c->endoff = (size_t)(off + len), c->nsid = nsid, c->mcur = (size_t)off;
    c->mode = LSP_ITER_SPAN, c->midx = 0, c->mbase = 0, c->mlen = 0;
    lst_setuv(L, "tree");
    luaL_setmetatable(L, LSP_CUR_TYPE);
    lua_pushcfunction(L, Lcur_iter);
    lua_pushvalue(L, -2);
    return 2;
}

static int Ltree_styled(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lst_Cur    *c;
    lua_Integer off = luaL_checkinteger(L, 2);
    lua_Integer len = luaL_checkinteger(L, 3);
    luaL_argcheck(L, off >= 0, 2, "spantree: invalid offset");
    luaL_argcheck(L, len >= 0, 3, "spantree: invalid length");
    c = (lst_Cur *)lua_newuserdata(L, sizeof(lst_Cur));
    sp_seek(&c->C, t->T, (size_t)off);
    c->tree = t, c->epoch = t->epoch;
    c->endoff = (size_t)(off + len), c->nsid = 0, c->mcur = (size_t)off;
    c->mode = LSP_ITER_STYLED, c->midx = 0, c->mbase = 0, c->mlen = 0;
    lst_setuv(L, "tree");
    luaL_setmetatable(L, LSP_CUR_TYPE);
    lua_pushcfunction(L, Lcur_iter);
    lua_pushvalue(L, -2);
    return 2;
}

static int Ltree_cursor(lua_State *L) {
    lst_Tree *t = ltree_check(L, 1);
    lst_Cur  *c = (lst_Cur *)lua_newuserdata(L, sizeof(lst_Cur));
    sp_seek(&c->C, t->T, 0);
    c->tree = t, c->epoch = t->epoch;
    c->endoff = 0, c->nsid = 0, c->mcur = 0, c->mode = 0;
    c->midx = 0, c->mbase = 0, c->mlen = 0;
    lst_setuv(L, "tree");
    luaL_setmetatable(L, LSP_CUR_TYPE);
    return 1;
}

static int Ltree_seek(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    lst_Cur    *c;
    luaL_argcheck(L, off >= 0, 2, "spantree: invalid offset");
    if (lua_gettop(L) >= 3) {
        c = (lst_Cur *)luaL_checkudata(L, 3, LSP_CUR_TYPE);
    } else {
        c = (lst_Cur *)lua_newuserdata(L, sizeof(lst_Cur));
        lst_setuv(L, "tree");
        luaL_setmetatable(L, LSP_CUR_TYPE);
    }
    sp_seek(&c->C, t->T, (size_t)off);
    c->tree = t, c->epoch = t->epoch;
    c->endoff = 0, c->nsid = 0, c->mcur = 0, c->mode = 0;
    c->midx = 0, c->mbase = 0, c->mlen = 0;
    lua_getuservalue(L, -1);
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "tree");
    lua_pop(L, 1);
    return 1;
}

static int lst_flags(lua_State *L, int idx, int *peph) {
    const char *flags = luaL_checklstring(L, idx, NULL), *fp;
    int         strict = 0;
    for (fp = flags; *fp; ++fp) {
        if (*fp == 'c')
            strict = 1;
        else if (*fp == 'e')
            *peph = 1;
        else
            luaL_error(L, "spantree: invalid namespace flags");
    }
    return strict;
}

static int Lcomp_namespace(lua_State *L) {
    lst_Comp   *comp = lcomp_check(L, 1);
    cp_State   *cp = &comp->cp;
    int         top = lua_gettop(L), ns, eph = 0, strict = 0, existed;
    const char *name = luaL_checklstring(L, 2, NULL);
    lua_Number  p, old;
    cp_NsArg    a;
    luaL_argcheck(
            L, lua_rawlen(L, 2) > 0, 2, "spantree: invalid namespace name");
    if (top == 2) { /* query */
        ns = cp_nsget(L, cp, name);
        if (ns == 0) return lua_pushnil(L), 1;
        lua_pushnumber(L, cp_prio(cp, ns));
        if (lst_iseph(ns))
            lua_pushliteral(L, "ephemeral");
        else
            lua_pushnil(L);
        return 2;
    }
    if (lua_isnil(L, 3)) /* unregister */
        return lua_pushnumber(L, lst_compnsunreg(L, comp, name)), 1;
    p = luaL_checknumber(L, 3);
    if (top >= 4) strict = lst_flags(L, 4, &eph);
    ns = cp_nsget(L, cp, name);
    if (strict && ns != 0)
        return luaL_error(L, "spantree: namespace already registered"), 0;
    existed = ns != 0 && lst_iseph(ns) != eph;
    if (existed)
        old = lst_compnsunreg(L, comp, name);
    else if (ns != 0)
        old = cp_prio(cp, ns);
    a.nm = name, a.p = p, a.e = eph;
    if (cp_nsreg(L, cp, &a)) {
        if (!eph && p != old) lst_compwalk(L, comp, lst_comprefold, NULL);
        return lua_pushnumber(L, old), 1;
    }
    if (existed) return lua_pushnumber(L, old), 1;
    return lua_pushnil(L), 1;
}

/* ---- iterator body ---- */

static int lst_iterstyled(lua_State *L, lst_Cur *c) {
    size_t e, len;
    sp_Id  id = lst_sty(L, c, &e);
    if (e <= c->mcur) return 0;
    len = e - c->mcur;
    if (len > c->endoff - c->mcur) len = c->endoff - c->mcur;
    lst_pushspan(L, c->tree, lst_span(c->mcur, len, id));
    c->mcur += len;
    return 4;
}

static int lst_itereph(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    sv_Span  *p = *lst_ephslot(L, t, c->nsid);
    size_t    x = c->mcur, s = sv_upper(p, x), e, start;
    if (s >= stV_len(p)) return 0;
    if (p[s].off >= c->endoff) return 0;
    e = p[s].off + p[s].len;
    if (e > c->endoff) e = c->endoff;
    start = p[s].off > x ? p[s].off : x;
    lst_pushspan(L, t, lst_span(start, e - start, p[s].id));
    c->mcur = e;
    return 4;
}

static int lst_iterns(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    cp_NSAttr ps[SP_MASK_BITS + 1];
    size_t    len, start, len2;
    sp_Id     id;
    int       i, n;
    for (;;) {
        id = sp_style(&c->C, &len, NULL); /* first call: the seek segment
                                           * is included (query semantics) */
        if (id == 0 && len == 0) return 0;
        n = cp_expand(t->cp, id, ps);
        for (i = 0; i < n && (int)ps[i].ns != c->nsid; ++i) continue;
        if (i < n) break;
        if (sp_next(&c->C, c->nsid, &len) == 0) return 0;
    }
    start = sp_offset(&c->C) - c->C.poff; /* segment start */
    if (start >= c->endoff) return 0;
    len2 = c->C.poff + len; /* full segment length (style gives the rest) */
    if (len2 > c->endoff - start) len2 = c->endoff - start;
    lst_pushspan(L, t, lst_span(start, len2, ps[i].attr));
    sp_next(&c->C, c->nsid, NULL); /* step past the emitted segment */
    return 4;
}

static int lst_iterany(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    cp_NSAttr ps[SP_MASK_BITS + 1];
    size_t    len, len2;
    sp_Id     id;
    int       n;
    for (;;) {
        if (c->mlen == 0) {
            id = sp_style(&c->C, &len, NULL);
            if (id == 0 && len == 0) return 0;
            c->mbase = sp_offset(&c->C) - c->C.poff;
            c->mlen = c->C.poff + len;
            c->mcur = c->mbase;
            c->midx = 0;
        } else {
            sp_locate(&c->C, c->mbase);
            id = sp_style(&c->C, &len, NULL);
        }
        n = cp_expand(t->cp, id, ps);
        if (c->midx < (size_t)n) {
            if (c->mbase >= c->endoff) return 0; /* seg starts past window */
            break;
        }
        c->mlen = 0;
        sp_next(&c->C, 0, NULL); /* hole segs read id 0: style-based stop */
    }
    len2 = c->mlen;
    if (len2 > c->endoff - c->mbase) len2 = c->endoff - c->mbase;
    lst_pushspan(L, t, lst_span(c->mbase, len2, ps[c->midx].attr));
    c->midx += 1;
    return 4;
}

static int Lcur_iter(lua_State *L) {
    lst_Cur *c = lcur_check(L, 1);
    if (c->mcur >= c->endoff) return 0;
    if (c->mode == LSP_ITER_STYLED) return lst_iterstyled(L, c);
    if (lst_iseph(c->nsid)) return lst_itereph(L, c);
    if (c->nsid > 0) return lst_iterns(L, c);
    return lst_iterany(L, c);
}

/* ---- cursor methods ---- */

static int Lcur_gc(lua_State *L) {
    (void)L; /* cursor holds no resources */
    return 0;
}

static int Lcur_seek(lua_State *L) {
    lst_Cur    *c = (lst_Cur *)luaL_checkudata(L, 1, LSP_CUR_TYPE);
    lst_Tree   *t = ltree_check(L, 2);
    lua_Integer off = luaL_checkinteger(L, 3);
    luaL_argcheck(L, off >= 0, 3, "spantree: invalid offset");
    sp_seek(&c->C, t->T, (size_t)off);
    c->tree = t, c->epoch = t->epoch, c->endoff = 0, c->nsid = 0;
    c->mode = 0, c->midx = 0, c->mbase = 0, c->mlen = 0;
    lua_getuservalue(L, 1);
    lua_pushvalue(L, 2);
    lua_setfield(L, -2, "tree");
    lua_pop(L, 1);
    return lua_settop(L, 1), 1;
}

static int Lcur_locate(lua_State *L) {
    lst_Cur    *c = lcur_check(L, 1);
    lua_Integer off = luaL_checkinteger(L, 2);
    luaL_argcheck(L, off >= 0, 2, "spantree: invalid offset");
    sp_locate(&c->C, (size_t)off);
    return lua_settop(L, 1), 1;
}

static int Lcur_advance(lua_State *L) {
    lst_Cur    *c = lcur_check(L, 1);
    lua_Integer d = luaL_checkinteger(L, 2);
    sp_advance(&c->C, (sp_Delta)d);
    return lua_settop(L, 1), 1;
}

static int Lcur_offset(lua_State *L) {
    lst_Cur *c = lcur_check(L, 1);
    return lua_pushinteger(L, (lua_Integer)sp_offset(&c->C)), 1;
}

static int Lcur_style(lua_State *L) {
    lst_Cur  *c = lcur_check(L, 1);
    cp_NSAttr ps[SP_MASK_BITS + 1];
    sp_Id     id;
    size_t    rem, base, len, x = sp_offset(&c->C);
    int       n;
    if (x >= sp_bytes(c->tree->T)) return 0;
    id = sp_style(&c->C, &rem, NULL);
    if (id == 0 || rem == 0) return 0;
    base = c->C.off, len = c->C.poff + rem;
    n = cp_expand(c->tree->cp, id, ps);
    if (c->mlen == 0 || c->mbase != base || c->mlen != len)
        c->mbase = base, c->mlen = len, c->midx = 0;
    if (c->midx >= (size_t)n) return 0;
    lst_pushspan(L, c->tree, lst_span(base, len, ps[c->midx].attr));
    return 4;
}

static int lst_nexteph(lua_State *L, lst_Cur *c, int nsid) {
    lst_Tree *t = c->tree;
    sv_Span  *p = *lst_ephslot(L, t, nsid);
    size_t    e, start, x = sp_offset(&c->C), s = sv_upper(p, x);
    if (s >= stV_len(p)) return 0;
    e = p[s].off + p[s].len;
    start = p[s].off > x ? p[s].off : x;
    lst_pushspan(L, t, lst_span(start, e - start, p[s].id));
    sp_advance(&c->C, (sp_Delta)(e - x));
    return 4;
}

static int lst_nextns(lua_State *L, lst_Cur *c, int nsid) {
    lst_Tree *t = c->tree;
    cp_NSAttr ps[SP_MASK_BITS + 1];
    sp_Id     id;
    size_t    len;
    int       i, n;
    for (;;) {
        id = sp_next(&c->C, nsid, &len);
        if (id == 0) return 0;
        n = cp_expand(t->cp, id, ps);
        for (i = 0; i < n; ++i)
            if ((int)ps[i].ns == nsid) break;
        if (i < n) break;
    }
    lst_pushspan(
            L, t, lst_span(sp_offset(&c->C), len, ps[i].attr)); /* seg start */
    return 4;
}

static int lst_nextany(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    cp_NSAttr ps[SP_MASK_BITS + 1];
    sp_Id     id;
    size_t    rem;
    int       n;
    if (c->mlen > 0) {
        sp_locate(&c->C, c->mbase);
        id = sp_style(&c->C, &rem, NULL);
        n = cp_expand(t->cp, id, ps);
        if (c->midx + 1 < (size_t)n) {
            c->midx += 1;
            lst_pushspan(L, t, lst_span(c->mbase, c->mlen, ps[c->midx].attr));
            return 4;
        }
    }
    sp_next(&c->C, 0, &rem);
    id = sp_style(&c->C, &rem, NULL);
    if (id == 0 && rem == 0) return 0;
    n = cp_expand(t->cp, id, ps);
    c->mbase = c->C.off, c->mlen = c->C.poff + rem, c->midx = 0;
    lst_pushspan(L, t, lst_span(c->mbase, c->mlen, ps[0].attr));
    return 4;
}

static int Lcur_next(lua_State *L) {
    lst_Cur *c = lcur_check(L, 1);
    int      nsid = lst_nsid(L, c->tree, 2);
    if (nsid == 0) return lst_nextany(L, c);
    if (lst_iseph(nsid)) return lst_nexteph(L, c, nsid);
    return lst_nextns(L, c, nsid);
}

static int lst_preveph(lua_State *L, lst_Cur *c, int nsid) {
    lst_Tree *t = c->tree;
    sv_Span  *p = *lst_ephslot(L, t, nsid);
    size_t    j, x = sp_offset(&c->C), s = sv_upper(p, x);
    if (stV_len(p) == 0) return 0;
    j = (s < stV_len(p) && p[s].off < x) ? s : s - 1;
    if (j >= stV_len(p)) return 0;
    sp_locate(&c->C, p[j].off);
    lst_pushspan(L, t, lst_span(p[j].off, p[j].len, p[j].id));
    return 4;
}

static int lst_prevns(lua_State *L, lst_Cur *c, int nsid) {
    lst_Tree *t = c->tree;
    cp_NSAttr ps[SP_MASK_BITS + 1];
    sp_Id     id;
    size_t    len;
    int       i, n;
    for (;;) {
        id = sp_prev(&c->C, nsid, &len);
        if (id == 0) return 0;
        id = sp_style(&c->C, &len, NULL); /* full seg len: sp_prev gives a
                                           * partial len mid-segment */
        if (id == 0) return 0;
        n = cp_expand(t->cp, id, ps);
        for (i = 0; i < n; ++i)
            if ((int)ps[i].ns == nsid) break;
        if (i < n) break;
    }
    lst_pushspan(L, t, lst_span(sp_offset(&c->C), len, ps[i].attr));
    return 4;
}

static int lst_prevany(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    cp_NSAttr ps[SP_MASK_BITS + 1];
    sp_Id     id;
    size_t    len;
    int       n;
    if (c->mlen > 0 && c->midx > 0) {
        sp_locate(&c->C, c->mbase);
        id = sp_style(&c->C, &len, NULL);
        n = cp_expand(t->cp, id, ps);
        c->midx -= 1;
        lst_pushspan(L, t, lst_span(c->mbase, c->mlen, ps[c->midx].attr));
        return 4;
    }
    sp_prev(&c->C, 0, &len);
    if (len == 0) return 0; /* tree head: stop (id 0 = a real empty seg) */
    id = sp_style(&c->C, &len, NULL);
    n = cp_expand(t->cp, id, ps);
    c->mbase = c->C.off, c->mlen = c->C.poff + len, c->midx = (size_t)n - 1;
    lst_pushspan(L, t, lst_span(c->mbase, c->mlen, ps[n - 1].attr));
    return 4;
}

static int Lcur_prev(lua_State *L) {
    lst_Cur *c = lcur_check(L, 1);
    int      nsid = lst_nsid(L, c->tree, 2);
    if (nsid == 0) return lst_prevany(L, c);
    if (lst_iseph(nsid)) return lst_preveph(L, c, nsid);
    return lst_prevns(L, c, nsid);
}

static int Lcur_mark(lua_State *L) {
    lst_Cur    *c = lcur_check(L, 1);
    int         nsid = lst_nsid(L, c->tree, 2);
    lua_Integer len = luaL_checkinteger(L, 4);
    sp_Id       op;
    unsigned    a, max;
    luaL_argcheck(L, len >= 0, 4, "spantree: invalid length");
    if (lua_type(L, 3) == LUA_TTABLE)
        a = cp_internattr(L, c->tree->cp, 3);
    else {
        a = (unsigned)luaL_checkinteger(L, 3);
        max = c->tree->cp->next;
        luaL_argcheck(L, a < max, 3, "spantree: unknown style id");
    }
    if (lst_iseph(nsid)) {
        sv_List *sl = lst_ephslot(L, c->tree, nsid);
        if (sv_fill(sl, sp_offset(&c->C), (size_t)len, a) != 0)
            luaL_error(L, "spantree: out of memory");
        return lua_pushinteger(L, (lua_Integer)a), 1;
    }
    op = cp_op(L, c->tree->cp, cp_opmake(CP_K_WRITE, nsid, a));
    lst_checkerror(L, sp_fill(&c->C, op, (size_t)len));
    lst_edit(L, c);
    return lua_pushinteger(L, (lua_Integer)a), 1;
}

static int Lcur_clear(lua_State *L) {
    lst_Cur    *c = lcur_check(L, 1);
    lua_Integer len;
    size_t      i;
    sp_Id       op = 0;
    int         nsid;
    if (lua_gettop(L) == 2) { /* all layers */
        len = luaL_checkinteger(L, 2);
        for (i = 0; i < c->tree->ephcnt; ++i)
            sv_clear(c->tree->ephs[i], sp_offset(&c->C), (size_t)len);
    } else {
        nsid = lst_nsid(L, c->tree, 2);
        len = luaL_checkinteger(L, 3);
        if (lst_iseph(nsid)) {
            sv_List *sl = lst_ephslot(L, c->tree, nsid);
            sv_clear(*sl, sp_offset(&c->C), (size_t)len);
            return lua_settop(L, 1), 1;
        }
        if (nsid > 0)
            op = cp_op(L, c->tree->cp, cp_opmake(CP_K_CLEAR, nsid, 0));
        if (nsid == 0)
            for (i = 0; i < c->tree->ephcnt; ++i)
                sv_clear(c->tree->ephs[i], sp_offset(&c->C), (size_t)len);
    }
    luaL_argcheck(L, len >= 0, 2, "spantree: invalid length");
    lst_checkerror(L, sp_fill(&c->C, op, (size_t)len));
    lst_edit(L, c);
    return lua_settop(L, 1), 1;
}

static int Lcur_splice(lua_State *L) {
    lst_Cur    *c = lcur_check(L, 1);
    lua_Integer del = luaL_checkinteger(L, 2);
    lua_Integer ins = luaL_checkinteger(L, 3);
    luaL_argcheck(L, del >= 0, 2, "spantree: invalid length");
    luaL_argcheck(L, ins >= 0, 3, "spantree: invalid length");
    lst_ephresetall(c->tree);
    lst_checkerror(L, sp_splice(&c->C, (size_t)del, (size_t)ins));
    lst_edit(L, c);
    return lua_settop(L, 1), 1;
}

static int Lcur_append(lua_State *L) {
    lst_Cur    *c = lcur_check(L, 1);
    lua_Integer ins = luaL_checkinteger(L, 2);
    luaL_argcheck(L, ins >= 0, 2, "spantree: invalid length");
    lst_ephresetall(c->tree);
    lst_checkerror(L, sp_append(&c->C, (size_t)ins));
    lst_edit(L, c);
    return lua_settop(L, 1), 1;
}

static int Lcur_insert(lua_State *L) {
    lst_Cur    *c = lcur_check(L, 1);
    lua_Integer ins = luaL_checkinteger(L, 2);
    luaL_argcheck(L, ins >= 0, 2, "spantree: invalid length");
    lst_ephresetall(c->tree);
    lst_checkerror(L, sp_insert(&c->C, (size_t)ins));
    lst_edit(L, c);
    return lua_settop(L, 1), 1;
}

static int Lcur_remove(lua_State *L) {
    lst_Cur    *c = lcur_check(L, 1);
    lua_Integer len = luaL_checkinteger(L, 2);
    sp_Cursor   R;
    luaL_argcheck(L, len >= 0, 2, "spantree: invalid length");
    lst_ephresetall(c->tree);
    sp_seek(&R, c->tree->T, sp_offset(&c->C));
    sp_advance(&R, (sp_Delta)len);
    lst_checkerror(L, sp_remove(&c->C, &R));
    lst_edit(L, c);
    return lua_settop(L, 1), 1;
}

/* ---- factories ---- */

static int Lcomp_gc(lua_State *L) {
    lst_Comp *c = lcomp_check(L, 1);
    cp_free(L, &c->cp);
    luaL_unref(L, LUA_REGISTRYINDEX, c->ref_trees);
    return 0;
}

static int Lsp_compositor(lua_State *L) {
    lst_Comp *S = (lst_Comp *)lua_newuserdata(L, sizeof(lst_Comp));
    memset(S, 0, sizeof(*S));
    S->L = L;
    cp_init(L, &S->cp);
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "__mode"), lua_pushliteral(L, "k"), lua_rawset(L, -3);
    S->ref_trees = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_setmetatable(L, LSP_COMP_TYPE);
    return 1;
}

static int Lsp_new(lua_State *L) {
    lst_Comp *comp = lcomp_check(L, 1);
    lst_Tree *t = (lst_Tree *)lua_newuserdata(L, sizeof(lst_Tree));
    memset(t, 0, sizeof(*t));
    t->cp = &comp->cp;
    t->L = L;
    t->T = sp_newtree(lst_state(L));
    sp_setarbiter(t->T, lst_arb, t);
    lst_setuv(L, "comp"); /* anchor the bound compositor: its cp state
                           * must outlive this tree */
    luaL_setmetatable(L, LSP_TREE_TYPE);
    lua_rawgeti(L, LUA_REGISTRYINDEX, comp->ref_trees);
    lua_pushvalue(L, -2), lua_pushboolean(L, 1), lua_rawset(L, -3);
    lua_pop(L, 1);
    return 1;
}

/* ---- module registration ---- */

void lst_opentree(lua_State *L) {
    const luaL_Reg tree_libs[] = {
            {"__gc", Ltree_gc},
#define ENTRY(name) {#name, Ltree_##name}
            ENTRY(bytes),       ENTRY(mark),
            ENTRY(clear),       ENTRY(splice),
            ENTRY(append),      ENTRY(insert),
            ENTRY(remove),      ENTRY(span),
            ENTRY(styled),      ENTRY(unmark),
            ENTRY(cursor),      ENTRY(seek),
#undef ENTRY
            {"new", Lsp_new},   {"compositor", Lsp_compositor},
            {NULL, NULL}};
    if (luaL_newmetatable(L, LSP_TREE_TYPE)) {
        luaL_setfuncs(L, tree_libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
}

static void lst_opencomp(lua_State *L) {
    static const luaL_Reg comp_libs[] = {{"__gc", Lcomp_gc},
#define ENTRY(name) {#name, Lcomp_##name}
                                         ENTRY(fields),      ENTRY(intern),
                                         ENTRY(attr),        ENTRY(namespace),
#undef ENTRY
                                         {NULL, NULL}};
    if (luaL_newmetatable(L, LSP_COMP_TYPE)) {
        luaL_setfuncs(L, comp_libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
        lua_pop(L, 1);
    }
}

static void lst_opencursor(lua_State *L) {
    static const luaL_Reg cur_libs[] = {
            {"__gc", Lcur_gc},
#define ENTRY(name) {#name, Lcur_##name}
            ENTRY(seek),       ENTRY(locate), ENTRY(advance), ENTRY(offset),
            ENTRY(style),      ENTRY(next),   ENTRY(prev),    ENTRY(mark),
            ENTRY(clear),      ENTRY(splice), ENTRY(append),  ENTRY(insert),
            ENTRY(remove),
#undef ENTRY
            {NULL, NULL}};
    if (luaL_newmetatable(L, LSP_CUR_TYPE)) {
        luaL_setfuncs(L, cur_libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
        lua_pop(L, 1);
    }
}

LUALIB_API int luaopen_spantree(lua_State *L) {
    lst_state(L), lst_opencomp(L), lst_opencursor(L);
    return lst_opentree(L), 1; /* module table (Tree metatable) on top */
}
