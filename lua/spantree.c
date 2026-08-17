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

/* tostring() with the string result on the stack top, replacing the
 * original value (caller pushes the value first). Numbers/booleans are
 * converted explicitly: lua_tolstring's push behaviour varies across
 * runtimes and must not be relied upon. */
static const char *lst_tolstring(lua_State *L, int idx, size_t *len) {
    const char *s;
    char        nb[32];
    lua_Number  num;
    int         b = 0, rpos = idx - 1; /* original after push (idx is -1) */
    (void)len;
    if (luaL_callmeta(L, idx, "__tostring")) {
        s = lua_tolstring(L, -1, NULL);
        if (s != NULL) return lua_replace(L, rpos), s;
        lua_pop(L, 1); /* __tostring returned a non-string: degrade */
        return lua_pushliteral(L, "nil"), lua_replace(L, rpos), "nil";
    }
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        return lua_pushliteral(L, "nil"), lua_replace(L, rpos), "nil";
    case LUA_TSTRING: return lua_tolstring(L, idx, NULL);
    case LUA_TBOOLEAN:
        b = lua_toboolean(L, idx);
        lua_pushstring(L, b ? "true" : "false");
        return lua_replace(L, rpos), b ? "true" : "false";
    default: /* number: integral check keeps the format across runtimes */
        num = lua_tonumber(L, idx);
        if (num == (lua_Number)(lua_Integer)num)
            snprintf(nb, sizeof(nb), "%lld", (long long)num);
        else
            snprintf(nb, sizeof(nb), "%.14g", num);
        lua_pushstring(L, nb);
        return lua_replace(L, rpos), lua_tostring(L, -1);
    }
}

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
#define LSP_CP_KEY     ((void *)0x5A17A0B2)
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

/* ns layer pair: one (ns -> attr) slot contribution */
typedef struct lst_Pair {
    unsigned ns, attr;
} lst_Pair;

/* op kinds: WRITE(ns, attr), CLEAR(ns), REORDER (payload 0) */
typedef enum { LTP_K_WRITE = 1, LTP_K_CLEAR, LTP_K_REORDER } ltp_Kind;

/* one composite id owns exactly two slots: a = chain head (a composite
 * id or the plain op/attr start), b = op leaf (plain op id) */
typedef struct cp_Pair {
    sp_Id a, b;
} cp_Pair;

/* minispan: one flat segment array per ephemeral ns (ordered, disjoint,
 * adjacent ids differ; memmove-maintained; no mask, no B+ structure) */
typedef struct sv_List {
    size_t   *off;
    size_t   *len;
    unsigned *id;
    size_t    n, cap;
} sv_List;

/* cp: compositor global state, per-lua_State singleton (registry
 * anchored, shared by every Tree). No lua_State member: all accessors
 * take L. Plain region: attr intern (ref_attrs/by_attr hash) + op
 * space ((ns, attr) intern via ref_byop). ns registry: name -> ns id
 * (ref_nsb), prio/regseq per ns, ids 1..SP_MASK_BITS ordinary (mask
 * bit ns-1), beyond = ephemeral. Composite region: chain slot pairs
 * + freelist (idfree links) + composite-slot refcnt (pure C
 * counters, zero Lua calls on the ref path) + chain-structure reuse
 * table (ref_chainhash: serialized (ns, attr) sequence -> composite
 * id; same chain same id, ref++). */
typedef struct cp_State {
    unsigned       next;    /* next attr id (0 = empty attr, prebuilt) */
    unsigned       nfields; /* canon whitelist length */
    int            ref_byattr, ref_attrs, ref_fields;
    lua_Number    *prio;
    unsigned      *regseq;
    size_t         nscap;
    unsigned       regcnt;
    int            nsstack[SP_MASK_BITS];
    int            nsstackn;
    unsigned       nsnext;  /* ordinary ids: 1..SP_MASK_BITS + freelist */
    unsigned       ephnext; /* ephemeral ids: SP_MASK_BITS+1 upward */
    int            ref_nsb;
    unsigned char *opkind;
    unsigned      *opns, *opattr;
    size_t         opcap;
    int            ref_byop;
    cp_Pair       *chain;
    unsigned      *idfree;
    unsigned      *refcnt;
    size_t         chaincap;  /* composite slot capacity (k < chaincap) */
    size_t         chainnext; /* first never-used slot (linear fresh
                               * allocator; freed slots live in idfree) */
    unsigned freehead;        /* freed-slot list head + 1 (0 = empty);
                               * idfree[k] = next free slot + 1 (0 =
                               * tail). Separate from idfree[0]: slot
                               * 0 must not double as the head */
    int ref_chainhash;
} cp_State;

typedef struct lst_Tree {
    sp_Tree   *T;
    lua_State *L;  /* arb pcall context (v3 carried it per-tree) */
    cp_State  *cp; /* registry singleton, shared by all trees */
    sv_List   *ephs;
    size_t     ephcnt, ephcap;
    size_t     epoch;
    lst_Pair  *rtmp; /* styled fold scratch pairs */
    size_t     rtmpcap;
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

static int lst_tab(lua_State *L, int ref) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    return lua_gettop(L);
}

/* store the stack-top value into the registry table at [id] */
static void lst_store(lua_State *L, int ref, lua_Integer id) {
    lst_tab(L, ref);
    lua_insert(L, -2);      /* table below the value */
    lsp_rawseti(L, -2, id); /* pops the value */
    lua_pop(L, 1);
}

/* userdata uservalue = table anchoring the owner (LuaJIT's 5.1 setfenv
 * requires a table; 5.2+ accepts any value, the table works everywhere) */
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

/* ---- id lookup ---- */

static int lst_strcmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* intern the key string (stack top) into the table at tab; pops the
 * key. Returns the existing id (isnew = 0) or a fresh one (isnew = 1,
 * caller fills the payload). */
static unsigned lst_lookup(lua_State *L, int tab, unsigned *next, int *isnew) {
    unsigned id;
    lua_pushvalue(L, -1); /* key copy */
    lua_rawget(L, tab);
    if (!lua_isnil(L, -1)) {
        id = (unsigned)lua_tointeger(L, -1);
        lua_pop(L, 2); /* id, key */
        *isnew = 0;
        return id;
    }
    lua_pop(L, 1); /* nil */
    id = (*next)++;
    lua_pushinteger(L, (lua_Integer)id);
    lua_rawset(L, tab);
    *isnew = 1;
    return id;
}

/* ================================================================== */
/* cp_ block: compositor global state (attr intern + ns registry + op */
/* space + composite id system). Shared by every Tree of one lua_State. */
/* ================================================================== */
/*
 * Functions and contracts:
 *
 * cp_get(L) -> cp_State*
 *   Lazy singleton: registry[LSP_CP_KEY] anchors the cp userdata; the
 *   first call builds ref_byattr/ref_attrs/ref_fields/ref_nsb tables,
 *   sets nsnext = 1, ephnext = SP_MASK_BITS + 1, grows
 *   the ns arrays to SP_MASK_BITS, installs the default field
 *   whitelist (lst_defaults, sorted) and pre-interns the empty attr
 *   as id 0. Must be called before any other cp_ function.
 *
 * cp_attr(L, cp, id) -> pushes the attr table for any id domain:
 *   id < cp->next                     -> attrs[id]
 *   id is a valid op (opkind set)     -> attrs[opattr[id]]
 *   id >= CP_COMP_START (composite)   -> fold: expand the chain, merge
 *     the per-slot attr tables in priority order (later overrides),
 *     intern (idempotent) -> attrs[that id]. Never copies user tables.
 *   unknown id -> nil.
 *   Contract: never returns a copy; the caller must not modify it.
 *
 * cp_setfields(L, cp) -> arg 2 = table of field name strings (any
 *   order; C side qsorts once); unlisted fields are ignored by canon.
 *   attr(id) still returns the original table.
 *
 * cp_canon(L, cp, attr) -> pushes the whitelist-ordered "k:v,k:v"
 *   join (empty -> ""). __hash metafield short-circuits: its (string)
 *   return value is the intern key directly ("h:"-prefixed); a non-
 *   string return raises "__hash must return a string".
 *
 * cp_internattr(L, cp, attr) -> unsigned attr id (interns the table at
 *   stack idx attr; canon/hash reuse, same attr -> same id).
 *
 * cp_nsgrow(L, cp, ns) -> grow prio/regseq to hold ns.
 * cp_opgrow(L, cp, id) -> grow opkind/opns/opattr to hold id.
 * cp_chaingrow(L, cp, k) -> grow chain/idfree/refcnt to hold
 *   composite slot k (k = id - CP_COMP_START; chain[k].a/.b are the
 *   two parents, idfree[k] the freelist link, refcnt[k] the tree
 *   reference count; chaincap = slot capacity). Fresh slots come from
 *   the linear chainnext allocator (freed slots via the idfree
 *   freelist): allocating at chaincap itself would double-grow every
 *   node and skip the grown range.
 *
 * cp_apply(cp, in, ps, &n) -> apply one op to the pair list (pure C):
 *   WRITE   -> replace the same-ns slot's attr or append
 *   CLEAR   -> drop the same-ns slot (swap-with-last removal)
 *   REORDER -> pairs untouched (caller re-sorts)
 *
 * cp_expand(L, cp, id, ps) -> int n: decode a tree segment id into a
 *   pair list in priority order. Composite: walk the a chain to the
 *   plain start collecting b op leaves ((opns, opattr) each), then
 *   reverse. Plain: opkind[id] != 0 -> single (opns[id], opattr[id])
 *   slot; else single (0, id) flat slot. The chain is compressed
 *   (sorted, deduped); ps must hold SP_MASK_BITS + 1 entries.
 *
 * cp_foldattr(L, cp, ps, n) -> unsigned: fold the pair list into a
 *   fresh table (per-slot attr tables merged in list order, later
 *   overrides) and intern it (plain region, idempotent). For the
 *   styled read path only: zero refcnt / composite participation.
 *
 * cp_hashkey(cp, ps, n, buf, cap) -> serialize the pair sequence as
 *   "%u:%u;%u:%u;..." (ns, attr pairs in list order) for the chain
 *   reuse table. Same sequence -> same key -> same composite id.
 *
 * cp_composite(L, cp, ps, n) -> sp_Id: n >= 2 (single-slot forms are
 *   handled by the caller). ref_chainhash lookup; on hit return the
 *   existing id untouched (the arb exit counts the tree reference).
 *   On miss: build n-1 left-slanted cons nodes (((o1,o2),o3)...)
 *   allocating n-1 composite slots (freelist reuse); each non-final
 *   node ref++ as the structural chain reference of its outer node
 *   (the final node's tree reference is the arb exit's); record key
 *   -> final id, return the final id.
 *
 * cp_release(L, cp, id) -> recursive composite teardown, called when
 *   refcnt[id] hits 0 (only composite ids recycle): drop the chain
 *   hash entry (a freed slot must never be handed out by a stale
 *   reuse hit), slot pair k into the idfree freelist, zero both slots,
 *   refdown the a head (recursing when composite and hitting 0) and
 *   the b op leaf. Plain ids never recycle (open item: caller handles
 *   may dangle).
 *
 * cp_refup(L, cp, id) / cp_refdown(L, cp, id) -> refcnt[k] += 1 /
 *   refcnt[k] -= 1 (floor 0) for composite ids; plain ids are
 *   no-ops (they never recycle: caller handles may dangle). PURE C,
 *   no Lua calls. refdown triggers cp_release at 0.
 *
 * cp_op(L, cp, k, ns, a) -> unsigned op id: intern the op payload in
 *   the global op space (ref_byop keys "w:<ns>:<a>" / "c:<ns>" / "r"
 *   + payload arrays). REORDER payload is (0, 0).
 *
 * cp_nsid(L, cp, idx) -> int ns id for the name at stack idx (nil ->
 *   0 unaffiliated; empty/unknown names raise, see lst_nsid).
 *
 * cp_nsalloc(L, cp, eph) -> int ns id (ordinary: freelist then
 *   1..SP_MASK_BITS with "spantree: namespace limit reached" at
 *   exhaustion; ephemeral: monotonic from ephnext).
 *
 * cp_nsget(L, cp, name) -> int ns id or 0.
 * cp_nsset(L, cp, name, ns) / cp_nsdel(L, cp, name) -> registry ops.
 *
 * cp_nsregister(L, cp, name, p, eph, &oldp) -> 1 if the name existed
 *   (old priority in *oldp), 0 fresh (regseq = ++regcnt, eph slot
 *   alloc on eph). Re-register = set prio (caller re-folds).
 *
 * cp_nsunregister(L, cp, name) -> lua_Number old priority: ordinary =
 *   prune-clear the ns then push its id to nsstack; ephemeral = free
 *   the (tree-local) list slot, no id reuse; registry entry removed.
 *
 * lst_less(cp, a, b) -> 1 if pair a orders before b: ns 0 first, then
 *   (prio, regseq).
 * lst_sort(cp, ps, n) -> insertion sort (n <= SP_MASK_BITS + 1).
 */

/* ---- cp singleton ---- */

/* default canon whitelist: SGR field set, sorted */
static const char *lst_defaults[] = {
        "bg", "bold", "dim", "fg", "italic", "reverse", "underline",
};

static void cp_setfields(lua_State *L, cp_State *cp, const char **fs, int n);
static unsigned cp_internattr(lua_State *L, cp_State *cp, int attr);
static void     cp_chaingrow(lua_State *L, cp_State *cp, size_t k);

static void cp_nsgrow(lua_State *L, cp_State *cp, int ns) {
    lua_Alloc f;
    void     *ud;
    size_t    ncap;
    if ((size_t)ns < cp->nscap) return;
    ncap = cp->nscap ? cp->nscap * 2 : SP_MASK_BITS + 1;
    while ((size_t)ns >= ncap) ncap *= 2;
    f = lua_getallocf(L, &ud);
    cp->prio = (lua_Number *)f(
            ud, cp->prio, cp->nscap * sizeof(lua_Number),
            ncap * sizeof(lua_Number));
    cp->regseq = (unsigned *)f(
            ud, cp->regseq, cp->nscap * sizeof(unsigned),
            ncap * sizeof(unsigned));
    memset(cp->prio + cp->nscap, 0, (ncap - cp->nscap) * sizeof(lua_Number));
    memset(cp->regseq + cp->nscap, 0, (ncap - cp->nscap) * sizeof(unsigned));
    cp->nscap = ncap;
}

static cp_State *cp_get(lua_State *L) {
    cp_State *cp;
    if (lua53_rawgetp(L, LUA_REGISTRYINDEX, LSP_CP_KEY) != LUA_TNIL) {
        cp = (cp_State *)lua_touserdata(L, -1);
        return lua_pop(L, 1), cp;
    }
    lua_pop(L, 1);
    cp = (cp_State *)lua_newuserdata(L, sizeof(cp_State));
    memset(cp, 0, sizeof(*cp));
    lua_newtable(L), cp->ref_byattr = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), cp->ref_attrs = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), cp->ref_fields = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), cp->ref_nsb = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), cp->ref_byop = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L), cp->ref_chainhash = luaL_ref(L, LUA_REGISTRYINDEX);
    cp->nsnext = 1; /* ordinary ns ids start at 1; 0 = unaffiliated */
    cp->ephnext = SP_MASK_BITS + 1;
    cp_nsgrow(L, cp, SP_MASK_BITS);
    cp_chaingrow(L, cp, 0);
    cp_setfields(
            L, cp, lst_defaults,
            (int)(sizeof(lst_defaults) / sizeof(lst_defaults[0])));
    lua_newtable(L);
    cp_internattr(L, cp, lua_gettop(L));
    lua_pop(L, 1);
    return lua_rawsetp(L, LUA_REGISTRYINDEX, LSP_CP_KEY), cp;
}

static void cp_opgrow(lua_State *L, cp_State *cp, unsigned id) {
    size_t    ncap = (size_t)id + 1;
    lua_Alloc f;
    void     *ud;
    if (ncap <= cp->opcap) return;
    ncap *= 2;
    f = lua_getallocf(L, &ud);
    cp->opkind = (unsigned char *)f(ud, cp->opkind, cp->opcap, ncap);
    cp->opns = (unsigned *)f(
            ud, cp->opns, cp->opcap * sizeof(unsigned),
            ncap * sizeof(unsigned));
    cp->opattr = (unsigned *)f(
            ud, cp->opattr, cp->opcap * sizeof(unsigned),
            ncap * sizeof(unsigned));
    memset(cp->opkind + cp->opcap, 0, ncap - cp->opcap);
    memset(cp->opns + cp->opcap, 0, (ncap - cp->opcap) * sizeof(unsigned));
    memset(cp->opattr + cp->opcap, 0, (ncap - cp->opcap) * sizeof(unsigned));
    cp->opcap = ncap;
}

static void cp_chaingrow(lua_State *L, cp_State *cp, size_t k) {
    lua_Alloc f;
    void     *ud;
    size_t    ncap;
    if (k < cp->chaincap) return;
    ncap = cp->chaincap ? cp->chaincap * 2 : 64;
    while (k >= ncap && ncap <= (size_t)-1 / 2) ncap *= 2;
    f = lua_getallocf(L, &ud);
    cp->chain = (cp_Pair *)f(
            ud, cp->chain, cp->chaincap * sizeof(cp_Pair),
            ncap * sizeof(cp_Pair));
    cp->idfree = (unsigned *)f(
            ud, cp->idfree, cp->chaincap * sizeof(unsigned),
            ncap * sizeof(unsigned));
    cp->refcnt = (unsigned *)f(
            ud, cp->refcnt, cp->chaincap * sizeof(unsigned),
            ncap * sizeof(unsigned));
    memset(cp->chain + cp->chaincap, 0,
           (ncap - cp->chaincap) * sizeof(cp_Pair));
    memset(cp->idfree + cp->chaincap, 0,
           (ncap - cp->chaincap) * sizeof(unsigned));
    memset(cp->refcnt + cp->chaincap, 0,
           (ncap - cp->chaincap) * sizeof(unsigned));
    cp->chaincap = ncap;
}

/* ---- cp style service ---- */

static void cp_setfields(lua_State *L, cp_State *cp, const char **fs, int n) {
    int i;
    if (n > 1) qsort(fs, (size_t)n, sizeof(char *), lst_strcmp);
    lst_tab(L, cp->ref_fields);
    for (i = 0; i < n; ++i) lua_pushstring(L, fs[i]), lsp_rawseti(L, -2, i + 1);
    lua_pop(L, 1);
    cp->nfields = (unsigned)n;
}

static void cp_part(lua_State *L, int sep) {
    int k = lua_gettop(L) - 1, v = k + 1;
    if (sep) lua_pushliteral(L, ",");
    lua_pushvalue(L, k);
    lst_tolstring(L, -1, NULL);
    if (sep) lua_concat(L, 2);
    if (lua_type(L, v) == LUA_TTABLE) {
        lua_pushliteral(L, ":rgb(");
        lua_getfield(L, v, "r");
        lst_tolstring(L, -1, NULL);
        lua_pushliteral(L, ",");
        lua_getfield(L, v, "g");
        lst_tolstring(L, -1, NULL);
        lua_pushliteral(L, ",");
        lua_getfield(L, v, "b");
        lst_tolstring(L, -1, NULL);
        lua_pushliteral(L, ")");
        lua_concat(L, 8);
    } else if (lua_type(L, v) != LUA_TBOOLEAN) {
        lua_pushliteral(L, ":");
        lua_pushvalue(L, v);
        lst_tolstring(L, -1, NULL);
        lua_concat(L, 3);
    }
}

static void cp_canon(lua_State *L, cp_State *cp, int attr) {
    int n = 0, i, f;
    f = lst_tab(L, cp->ref_fields);
    for (i = 0; i < (int)cp->nfields; ++i) {
        lsp_rawgeti(L, f, i + 1);
        lua_pushvalue(L, -1);
        lua_rawget(L, attr);
        if (lua_isnil(L, -1) || !lua_toboolean(L, -1)) {
            lua_pop(L, 2); /* value, key */
            continue;
        }
        cp_part(L, n > 0);
        lua_remove(L, -2); /* value */
        lua_remove(L, -2); /* key: the piece stays */
        n += 1;
    }
    lua_remove(L, f);
    lua_concat(L, n);
}

static unsigned cp_internattr(lua_State *L, cp_State *cp, int attr) {
    unsigned id;
    int      isnew, tab;
    if (luaL_getmetafield(L, attr, "__hash") != LUA_TNIL) {
        lua_pushvalue(L, attr);
        lua_call(L, 1, 1);
        luaL_argcheck(
                L, lua_type(L, -1) == LUA_TSTRING, attr,
                "__hash must return a string");
        lua_pushliteral(L, "h:");
        lua_insert(L, -2);
        lua_concat(L, 2);
    } else {
        cp_canon(L, cp, attr);
        lua_pushliteral(L, "a:");
        lua_insert(L, -2);
        lua_concat(L, 2);
    }
    lst_tab(L, cp->ref_byattr);
    lua_insert(L, -2);
    tab = lua_gettop(L) - 1;
    id = lst_lookup(L, tab, &cp->next, &isnew);
    lua_pop(L, 1);
    if (!isnew) return id;
    lua_pushvalue(L, attr);
    lst_store(L, cp->ref_attrs, id);
    return id;
}

static int      cp_expand(lua_State *L, cp_State *cp, sp_Id id, lst_Pair *ps);
static unsigned cp_foldattr(
        lua_State *L, cp_State *cp, const lst_Pair *ps, int n);
static void cp_refup(lua_State *L, cp_State *cp, sp_Id id);
static void cp_refdown(lua_State *L, cp_State *cp, sp_Id id);

static void cp_attr(lua_State *L, cp_State *cp, sp_Id id) {
    lst_Pair ps[SP_MASK_BITS + 1];
    int      n;
    unsigned aid;
    if (id < cp->opcap && cp->opkind[id] != 0) {
        lsp_rawgeti(L, lst_tab(L, cp->ref_attrs), cp->opattr[id]);
        lua_remove(L, -2);
        return;
    }
    if (id < cp->next) {
        lsp_rawgeti(L, lst_tab(L, cp->ref_attrs), id);
        lua_remove(L, -2);
        return;
    }
    if (id >= CP_COMP_START && id - CP_COMP_START < cp->chaincap
        && (n = cp_expand(L, cp, id, ps)) > 0) {
        aid = cp_foldattr(L, cp, ps, n);
        lsp_rawgeti(L, lst_tab(L, cp->ref_attrs), aid);
        lua_remove(L, -2);
        return;
    }
    lua_pushnil(L);
}

/* ---- cp op space ---- */

static unsigned cp_opkey(
        lua_State *L, cp_State *cp, ltp_Kind k, unsigned ns, unsigned a) {
    unsigned id;
    int      isnew, tab;
    lst_tab(L, cp->ref_byop);
    lua_insert(L, -2);
    tab = lua_gettop(L) - 1;
    /* op ids share the attr counter: the two spaces never collide
     * (design: interleaved allocation is harmless with one counter) */
    id = lst_lookup(L, tab, &cp->next, &isnew);
    lua_pop(L, 1);
    if (!isnew) return id;
    cp_opgrow(L, cp, id);
    cp->opkind[id] = (unsigned char)k, cp->opns[id] = ns, cp->opattr[id] = a;
    return id;
}

static unsigned cp_op(
        lua_State *L, cp_State *cp, ltp_Kind k, unsigned ns, unsigned a) {
    char nb[48];
    if (k == LTP_K_WRITE) {
        snprintf(nb, sizeof(nb), "w:%u:%u", ns, a);
        lua_pushstring(L, nb);
    } else if (k == LTP_K_CLEAR) {
        snprintf(nb, sizeof(nb), "c:%u", ns);
        lua_pushstring(L, nb);
    } else {
        lua_pushliteral(L, "r");
    }
    return cp_opkey(L, cp, k, ns, a);
}

/* ---- cp composite region ---- */

static int cp_expand(lua_State *L, cp_State *cp, sp_Id id, lst_Pair *ps) {
    sp_Id    cur, b;
    size_t   k;
    lst_Pair v;
    int      n = 0, i;
    (void)L;
    cur = id;
    while (cur >= CP_COMP_START) {
        k = (size_t)(cur - CP_COMP_START);
        b = cp->chain[k].b;
        ps[n].ns = cp->opns[b], ps[n].attr = cp->opattr[b];
        n += 1;
        cur = cp->chain[k].a;
    }
    if (cur < cp->opcap && cp->opkind[cur] != 0)
        ps[n].ns = cp->opns[cur], ps[n].attr = cp->opattr[cur];
    else
        ps[n].ns = 0, ps[n].attr = (unsigned)cur;
    n += 1;
    for (i = 0; i < n / 2; ++i)
        v = ps[i], ps[i] = ps[n - 1 - i], ps[n - 1 - i] = v;
    return n;
}

static unsigned cp_foldattr(
        lua_State *L, cp_State *cp, const lst_Pair *ps, int n) {
    int      i, at, fold = lua_gettop(L) + 1;
    unsigned id;
    lua_newtable(L);
    for (i = 0; i < n; ++i) {
        lsp_rawgeti(L, lst_tab(L, cp->ref_attrs), ps[i].attr);
        at = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, at)) {
            lua_pushvalue(L, lua_gettop(L) - 1); /* key */
            lua_pushvalue(L, -2);                /* value */
            lua_rawset(L, fold);
            lua_pop(L, 1); /* value; key stays for lua_next */
        }
        lua_pop(L, 2); /* attr table + ref_attrs table */
    }
    id = cp_internattr(L, cp, fold);
    lua_pop(L, 1); /* fold table */
    return id;
}

static int cp_hashkey(
        const cp_State *cp, const lst_Pair *ps, int n, char *buf, size_t cap) {
    size_t off = 0;
    int    i, w;
    (void)cp;
    for (i = 0; i < n; ++i) {
        w = snprintf(buf + off, cap - off, "%u:%u;", ps[i].ns, ps[i].attr);
        off += (size_t)w;
    }
    return (int)off;
}

static sp_Id cp_composite(
        lua_State *L, cp_State *cp, const lst_Pair *ps, int n) {
    char   key[1400];
    int    i, tab;
    sp_Id  a, b, id;
    size_t k;
    cp_hashkey(cp, ps, n, key, sizeof(key));
    tab = lst_tab(L, cp->ref_chainhash);
    lua_pushstring(L, key);
    lua_rawget(L, tab);
    id = lua_isnil(L, -1) ? 0 : (sp_Id)lua_tointeger(L, -1);
    if (id != 0) {
        size_t hk = (size_t)(id - CP_COMP_START);
        if (cp->chain[hk].a != 0 || cp->chain[hk].b != 0)
            return lua_pop(L, 2), id; /* live reuse */
    }
    lua_pop(L, 1); /* miss or ghost: the table stays for the rawset */
    a = ps[0].ns == 0 ? ps[0].attr
                      : cp_op(L, cp, LTP_K_WRITE, ps[0].ns, ps[0].attr);
    for (i = 0; i < n - 1; ++i) {
        b = cp_op(L, cp, LTP_K_WRITE, ps[i + 1].ns, ps[i + 1].attr);
        if (cp->freehead > 0)
            k = (size_t)(cp->freehead - 1), cp->freehead = cp->idfree[k] + 1;
        else
            k = cp->chainnext++, cp_chaingrow(L, cp, k);
        cp->chain[k].a = a, cp->chain[k].b = b;
        a = (sp_Id)(k + CP_COMP_START);
        if (i < n - 2) cp_refup(L, cp, a);
    }
    lua_pushstring(L, key), lua_pushinteger(L, (lua_Integer)a);
    lua_rawset(L, tab), lua_pop(L, 1);
    return a;
}

static void cp_release(lua_State *L, cp_State *cp, sp_Id id) {
    lst_Pair ps[SP_MASK_BITS + 1];
    char     key[1400];
    size_t   k = (size_t)(id - CP_COMP_START);
    sp_Id    a = cp->chain[k].a, b = cp->chain[k].b;
    int      n, tab;
    if (a == 0 && b == 0) return; /* already released */
    n = cp_expand(L, cp, id, ps);
    cp_hashkey(cp, ps, n, key, sizeof(key));
    tab = lst_tab(L, cp->ref_chainhash);
    lua_pushstring(L, key);
    lua_pushnil(L);
    lua_rawset(L, tab);
    lua_pop(L, 1);
    cp->chain[k].a = cp->chain[k].b = 0;
    cp->idfree[k] = cp->freehead - 1;
    cp->freehead = (unsigned)k + 1;
    cp_refdown(L, cp, a);
    cp_refdown(L, cp, b);
}

static void cp_refup(lua_State *L, cp_State *cp, sp_Id id) {
    size_t k;
    (void)L;
    if (id < CP_COMP_START) return; /* plain ids never recycle */
    k = (size_t)(id - CP_COMP_START);
    cp->refcnt[k] += 1;
}

static void cp_refdown(lua_State *L, cp_State *cp, sp_Id id) {
    size_t k;
    if (id < CP_COMP_START) return;
    k = (size_t)(id - CP_COMP_START);
    if (cp->refcnt[k] == 0) return;
    cp->refcnt[k] -= 1;
    if (cp->refcnt[k] == 0) cp_release(L, cp, id);
}

/* ---- cp ns registry ---- */

static int cp_nsalloc(lua_State *L, cp_State *cp, int eph) {
    if (eph) {
        cp_nsgrow(L, cp, (int)cp->ephnext);
        return (int)cp->ephnext++;
    }
    if (cp->nsstackn > 0) return cp->nsstack[--cp->nsstackn];
    if (cp->nsnext > SP_MASK_BITS)
        return luaL_error(L, "spantree: namespace limit reached"), 0;
    return (int)cp->nsnext++;
}

static int cp_nsget(lua_State *L, cp_State *cp, const char *name) {
    int ns = 0;
    lst_tab(L, cp->ref_nsb);
    lua_pushstring(L, name);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) ns = (int)lua_tointeger(L, -1);
    return lua_pop(L, 2), ns;
}

static void cp_nsset(lua_State *L, cp_State *cp, const char *name, int ns) {
    lst_tab(L, cp->ref_nsb);
    lua_pushstring(L, name);
    lua_pushinteger(L, ns);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

static void cp_nsdel(lua_State *L, cp_State *cp, const char *name) {
    lst_tab(L, cp->ref_nsb);
    lua_pushstring(L, name);
    lua_pushnil(L);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

static int cp_nsregister(
        lua_State *L, cp_State *cp, const char *name, lua_Number p, int eph,
        lua_Number *oldp) {
    int ns = cp_nsget(L, cp, name);
    if (ns != 0) {
        *oldp = cp->prio[ns];
        cp->prio[ns] = p;
        return 1;
    }
    ns = cp_nsalloc(L, cp, eph);
    cp_nsset(L, cp, name, ns);
    cp->prio[ns] = p;
    cp->regseq[ns] = ++cp->regcnt;
    return 0;
}

static lua_Number cp_nsunregister(
        lua_State *L, cp_State *cp, const char *name) {
    int        ns = cp_nsget(L, cp, name);
    lua_Number old;
    if (ns == 0) return luaL_error(L, "spantree: unknown namespace"), 0;
    old = cp->prio[ns];
    if (ns > (int)SP_MASK_BITS) {
        if ((unsigned)ns + 1 == cp->ephnext) cp->ephnext -= 1;
    } else {
        cp->nsstack[cp->nsstackn++] = ns;
    }
    cp_nsdel(L, cp, name);
    cp->prio[ns] = 0, cp->regseq[ns] = 0;
    return old;
}

static int lst_less(const cp_State *cp, lst_Pair a, lst_Pair b) {
    if (a.ns == 0) return b.ns != 0;
    if (b.ns == 0) return 0;
    if (cp->prio[a.ns] != cp->prio[b.ns])
        return cp->prio[a.ns] < cp->prio[b.ns];
    return cp->regseq[a.ns] < cp->regseq[b.ns];
}

static void lst_sort(const cp_State *cp, lst_Pair *ps, int n) {
    int      i, j;
    lst_Pair v;
    for (i = 1; i < n; ++i) {
        v = ps[i];
        for (j = i; j > 0 && lst_less(cp, v, ps[j - 1]); --j) ps[j] = ps[j - 1];
        ps[j] = v;
    }
}

static void cp_apply(const cp_State *cp, sp_Id in, lst_Pair *tmp, int *pn) {
    int i, n = *pn;
    if (cp->opkind[in] == LTP_K_WRITE) {
        for (i = 0; i < n && tmp[i].ns != cp->opns[in]; ++i) continue;
        if (i < n)
            tmp[i].attr = cp->opattr[in];
        else
            tmp[n].ns = cp->opns[in], tmp[n].attr = cp->opattr[in], n += 1;
    } else if (cp->opkind[in] == LTP_K_CLEAR) {
        for (i = 0; i < n && tmp[i].ns != cp->opns[in]; ++i) continue;
        if (i < n) tmp[i] = tmp[n - 1], n -= 1;
    } /* REORDER: pairs untouched */
    *pn = n;
}

/* ================================================================== */
/* sv_ block: minispan (flat per-ns segment arrays for ephemeral ns). */
/* ================================================================== */
/*
 * Functions and contracts (migrated from v3 lsp_eph*, algorithms
 * unchanged; see notes/reports/handoff_spantree_v4.md section 5):
 *
 * sv_segrow(L, e, need) -> grow off/len/id so n + need fits.
 *
 * sv_upper(e, x) -> size_t: first index with off + len > x (covers x
 *   or starts past it), binary search.
 * sv_lower(e, x) -> size_t: first index with off >= x, binary search.
 *
 * sv_norm(e, j) -> merge seg j with same-id contiguous neighbors (at
 *   most one step per side; MUST check off[j-1]+len[j-1] == off[j],
 *   otherwise holes get wrongly merged).
 *
 * sv_fill(L, e, off, len, id) -> cover-cut + splice + normalize.
 *   No right overhang: memmove the tail one step right BEFORE writing
 *   the new segment (else the write covers unmoved data). Split case
 *   (left && right && t == s+1): single segment split in two.
 *
 * sv_clear(e, off, len) -> range cut. Single-segment double trim =
 *   split in two; trim right first, the right trim uses the ORIGINAL
 *   length; clear always leaves a hole (no norm needed).
 *
 * sv_cover(e, x, &id, &ps, &pe) -> segment covering x: id, [ps, pe);
 *   no cover -> id = 0, ps = pe = x.
 */

static void sv_segrow(lua_State *L, sv_List *e, size_t need) {
    lua_Alloc f;
    void     *ud;
    size_t    ncap;
    if (e->n + need <= e->cap) return;
    ncap = e->cap ? e->cap * 2 : 16;
    while (e->n + need > ncap) ncap *= 2;
    f = lua_getallocf(L, &ud);
    e->off = (size_t *)f(
            ud, e->off, e->cap * sizeof(size_t), ncap * sizeof(size_t));
    e->len = (size_t *)f(
            ud, e->len, e->cap * sizeof(size_t), ncap * sizeof(size_t));
    e->id = (unsigned *)f(
            ud, e->id, e->cap * sizeof(unsigned), ncap * sizeof(unsigned));
    e->cap = ncap;
}

static size_t sv_upper(const sv_List *e, size_t x) {
    size_t lo = 0, hi = e->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e->off[mid] + e->len[mid] <= x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static size_t sv_lower(const sv_List *e, size_t x) {
    size_t lo = 0, hi = e->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e->off[mid] < x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void sv_norm(sv_List *e, size_t j) {
    if (j > 0 && e->id[j - 1] == e->id[j]
        && e->off[j - 1] + e->len[j - 1] == e->off[j]) {
        e->len[j - 1] += e->len[j];
        memmove(e->off + j, e->off + j + 1, (e->n - j - 1) * sizeof(size_t));
        memmove(e->len + j, e->len + j + 1, (e->n - j - 1) * sizeof(size_t));
        memmove(e->id + j, e->id + j + 1, (e->n - j - 1) * sizeof(unsigned));
        e->n -= 1, j -= 1;
    }
    if (j + 1 < e->n && e->id[j] == e->id[j + 1]
        && e->off[j] + e->len[j] == e->off[j + 1]) {
        e->len[j] += e->len[j + 1];
        memmove(e->off + j + 1, e->off + j + 2,
                (e->n - j - 2) * sizeof(size_t));
        memmove(e->len + j + 1, e->len + j + 2,
                (e->n - j - 2) * sizeof(size_t));
        memmove(e->id + j + 1, e->id + j + 2,
                (e->n - j - 2) * sizeof(unsigned));
        e->n -= 1;
    }
}

static void sv_fill(
        lua_State *L, sv_List *e, size_t off, size_t len, unsigned id) {
    size_t   s, t, ohlen;
    unsigned ohid;
    int      left, right;
    if (len == 0) return;
    s = sv_upper(e, off), t = sv_lower(e, off + len);
    sv_segrow(L, e, 2);
    left = s < e->n && e->off[s] < off;
    right = t > 0 && e->off[t - 1] + e->len[t - 1] > off + len;
    ohid = right ? e->id[t - 1] : 0;
    ohlen = right ? e->off[t - 1] + e->len[t - 1] - (off + len) : 0;
    if (left) e->len[s] = off - e->off[s];
    s += left;
    memmove(e->off + s, e->off + t, (e->n - t) * sizeof(size_t));
    memmove(e->len + s, e->len + t, (e->n - t) * sizeof(size_t));
    memmove(e->id + s, e->id + t, (e->n - t) * sizeof(unsigned));
    e->n = s + (e->n - t);
    if (right) {
        memmove(e->off + s + 2, e->off + s, (e->n - s) * sizeof(size_t));
        memmove(e->len + s + 2, e->len + s, (e->n - s) * sizeof(size_t));
        memmove(e->id + s + 2, e->id + s, (e->n - s) * sizeof(unsigned));
        e->off[s] = off, e->len[s] = len, e->id[s] = id;
        e->off[s + 1] = off + len, e->len[s + 1] = ohlen;
        e->id[s + 1] = ohid;
        e->n += 2;
    } else {
        memmove(e->off + s + 1, e->off + s, (e->n - s) * sizeof(size_t));
        memmove(e->len + s + 1, e->len + s, (e->n - s) * sizeof(size_t));
        memmove(e->id + s + 1, e->id + s, (e->n - s) * sizeof(unsigned));
        e->off[s] = off, e->len[s] = len, e->id[s] = id;
        e->n += 1;
    }
    sv_norm(e, s);
}

static void sv_clear(sv_List *e, size_t off, size_t len) {
    size_t s, t;
    int    left, right;
    if (len == 0) return;
    s = sv_upper(e, off), t = sv_lower(e, off + len);
    if (s == t) return;
    left = e->off[s] < off;
    right = e->off[t - 1] + e->len[t - 1] > off + len;
    if (left && right && t == s + 1) { /* one seg split in two */
        memmove(e->off + t + 1, e->off + t, (e->n - t) * sizeof(size_t));
        memmove(e->len + t + 1, e->len + t, (e->n - t) * sizeof(size_t));
        memmove(e->id + t + 1, e->id + t, (e->n - t) * sizeof(unsigned));
        e->off[t] = off + len;
        e->len[t] = e->off[s] + e->len[s] - (off + len);
        e->id[t] = e->id[s];
        e->len[s] = off - e->off[s];
        e->n += 1;
        return;
    }
    if (left) e->len[s] = off - e->off[s], s += 1;
    if (right) {
        e->len[t - 1] = e->off[t - 1] + e->len[t - 1] - (off + len);
        e->off[t - 1] = off + len, t -= 1;
    }
    if (s >= t) return;
    memmove(e->off + s, e->off + t, (e->n - t) * sizeof(size_t));
    memmove(e->len + s, e->len + t, (e->n - t) * sizeof(size_t));
    memmove(e->id + s, e->id + t, (e->n - t) * sizeof(unsigned));
    e->n -= t - s;
}

static void sv_cover(
        const sv_List *e, size_t x, unsigned *id, size_t *ps, size_t *pe) {
    size_t s = sv_upper(e, x);
    if (s < e->n && e->off[s] <= x) {
        *id = e->id[s];
        *ps = e->off[s];
        *pe = e->off[s] + e->len[s];
    } else {
        *id = 0;
        *ps = x, *pe = x;
    }
}

/* ================================================================== */
/* lst block: binding integration (cp + sv + sp into the Lua API).    */
/* ================================================================== */
/*
 * Module: require "spantree" -> { new = function -> Tree }. The cp
 * state is a per-lua_State singleton shared by all Trees.
 *
 * Tree methods (design_spantree_lua.md section 2):
 *   bytes() -> n
 *   mark(ns, attr_or_id, off, len) -> attr id (payload dual form:
 *     table -> intern / number -> reuse; eph ns -> sv_fill zero epoch;
 *     plain ns -> sp_fill WRITE op; nil ns = 0 unaffiliated)
 *   clear(ns) / clear(ns, off, len) / clear(nil, off, len) / clear()
 *   splice/append/insert/remove -> sv_resetall-equivalent first, then
 *     the tree edit (edit verbs clear every eph list)
 *   span(ns?, off, len) -> iterator: (off, len, table, id) mark stream
 *     (id = attr id; one merged segment with k marks emits k times,
 *     priority order; ns filter = one slot; eph ns = sv list walk)
 *   styled(off, len) -> iterator: (off, len, table, id) rendered
 *     composite stream (tree + every eph layer boundary-split; fold =
 *     attr black-box override + plain intern; single-layer spans
 *     return the tree id untouched)
 *   unmark(id) -> n cleared segments (scan + expand + arb CLEAR per
 *     matching slot; unknown id -> 0)
 *   cursor() / seek(off[, c])
 *   namespace(name) -> p, mode | nil / namespace(name, p[, flags]) ->
 *     oldp / namespace(name, nil) -> oldp (unregister)
 *   setfields/intern/attr (cp face, Tree-provided)
 *
 * Cursor methods:
 *   seek/locate/advance/offset (v3 semantics; seek rebinds cross-tree)
 *   style() -> off, len, table, id | nil (current mark: midx into the
 *     current segment)
 *   next(ns?) / prev(ns?) -> mark stream (same segment midx +/- 1 or
 *     the next/prev segment's first/last mark)
 *   mark(ns, attr_or_id, len) -> attr id
 *   clear(len) / clear(ns, len)
 *   splice/append/insert/remove (edit verbs clear eph first)
 *
 * Internal read-path contracts:
 *
 * lst_merge(L) -> (ret, n, ns...) C function under pcall from lst_arb
 *   (merge path only: in != 0 && old != 0): args (tree_ud, in, old).
 *   Expand old -> pairs; apply the op; empty -> 0; sort; single
 *   ns==0 slot -> attr id; single ns>0 slot -> op id (zero
 *   composite); else cp_composite (hash reuse, ref++ on hit). Return
 *   ret + the ns list for the out-mask. No refcnt work here: the arb
 *   exit counts unconditionally outside the pcall.
 *
 * lst_arb(ud, in, old, mask) -> sp_Id (three-state contract,
 *   design_spantree_lua.md 3.2; the tree guarantees every id enters/
 *   leaves via arb):
 *   arb(0, 0) pad            -> return 0, *mask = 0 (values unused)
 *   arb(id, 0) birth         -> refup(id), return id unchanged
 *   arb(0, id) death         -> refdown(id), return 0, *mask = 0
 *   arb(id, old) merge       -> pcall(lst_merge); on error keep old
 *     (segment unchanged, refcnt untouched); on success rebuild *mask
 *     from the returned ns list, then count unconditionally:
 *     ret != 0 -> refup(ret); old != 0 -> refdown(old) (add before
 *     drop: ret == old nets zero). refup/refdown are pure C, outside
 *     the pcall.
 *
 * lst_markflow(L, t, &C, x) -> (id, len, ps, pe): mark stream
 *   iteration core. Steps sp_next over tree segments; per segment
 *   cp_expand -> pair list (priority order). The iterator/cursor walk
 *   the pairs with midx. eph ns -> sv list step (no expansion).
 *
 * lst_styledflow(L, t, &C, x, &s, &e) -> id: rendered composite
 *   stream core (v3 mergedwalk + mergecalc, black-box fold): boundary
 *   scan every eph layer (sv_cover edges cap s/e), fold = p>=0 eph
 *   attrs (asc) -> t attr of the tree id -> p<0 eph attrs (asc),
 *   later overrides; no eph cover -> the tree id untouched.
 *
 * lst_edit(L, c) -> bump tree epoch + self-sync the cursor.
 *
 * lst_nsid(L, t, idx) -> ns id for arg idx (nil = 0; name must be a
 *   non-empty registered string, else the documented errors).
 *
 * Iterators: Ltree_span/Ltree_styled build an internal lst_Cur
 *   userdata (mode set, one per call) and return (Lcur_iter, state).
 *   Lcur_iter branches: SPAN -> mark stream (nsid 0/plain/eph three
 *   ways), STYLED -> lst_styledflow walk. mcur/endoff window-clip.
 */

/* ---- tree helpers ---- */

#define lst_iseph(ns)  ((ns) > (int)SP_MASK_BITS)
#define lst_ephidx(ns) ((size_t)((ns) - (int)SP_MASK_BITS - 1))

static lst_Tree *ltree_check(lua_State *L, int idx) {
    return (lst_Tree *)luaL_checkudata(L, idx, LSP_TREE_TYPE);
}

/* cursor read/write guard: tree edit bumps epoch, mismatch = dangling
 * paths. seek rebinds without the check (sp_seek rebuilds paths). */
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
    lst_tab(L, t->cp->ref_nsb);
    lua_pushvalue(L, idx);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1))
        return luaL_error(L, "spantree: unknown namespace"), 0;
    ns = (int)lua_tointeger(L, -1);
    lua_pop(L, 2); /* id, table */
    return ns;
}

static sv_List *lst_ephslot(lua_State *L, lst_Tree *t, int ns) {
    lua_Alloc f;
    void     *ud;
    size_t    idx = lst_ephidx(ns), ncap;
    if (idx + 1 > t->ephcnt) t->ephcnt = idx + 1;
    if (idx < t->ephcap) return t->ephs + idx;
    ncap = t->ephcap ? t->ephcap * 2 : 8;
    while (idx >= ncap) ncap *= 2;
    f = lua_getallocf(L, &ud);
    t->ephs = (sv_List *)f(
            ud, t->ephs, t->ephcap * sizeof(sv_List), ncap * sizeof(sv_List));
    memset(t->ephs + t->ephcap, 0, (ncap - t->ephcap) * sizeof(sv_List));
    t->ephcap = ncap;
    return t->ephs + idx;
}

static void lst_ephresetall(lst_Tree *t) {
    size_t i;
    for (i = 0; i < t->ephcnt; ++i) t->ephs[i].n = 0;
}

/* ---- merge core ---- */

static int lst_merge(lua_State *L) {
    lst_Tree *t = (lst_Tree *)lua_touserdata(L, 1);
    sp_Id     in = (sp_Id)luaL_checkinteger(L, 2);
    sp_Id     old = (sp_Id)luaL_checkinteger(L, 3);
    lst_Pair  ps[SP_MASK_BITS + 1];
    sp_Id     ret;
    int       n, k = 0, i;
    n = cp_expand(L, t->cp, old, ps);
    cp_apply(t->cp, in, ps, &n);
    if (n == 0) {
        lua_pushinteger(L, 0);
        lua_pushinteger(L, 0);
        return 2;
    }
    lst_sort(t->cp, ps, n);
    if (n == 1 && ps[0].ns == 0)
        ret = ps[0].attr;
    else if (n == 1)
        ret = cp_op(L, t->cp, LTP_K_WRITE, ps[0].ns, ps[0].attr);
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

static sp_Mask lst_segmask(cp_State *cp, sp_Id id) {
    lst_Pair ps[SP_MASK_BITS + 1];
    sp_Mask  m = 0;
    int      n, i;
    if (id >= CP_COMP_START) {
        n = cp_expand(NULL, cp, id, ps);
        for (i = 0; i < n; ++i)
            if (ps[i].ns > 0) sp_addns(&m, (int)ps[i].ns);
    } else if (id < cp->opcap && cp->opkind[id] != 0) {
        if (cp->opns[id] > 0) sp_addns(&m, (int)cp->opns[id]);
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
    if (old == 0) { /* birth */
        cp_refup(L, t->cp, in);
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
    if (ret != 0) cp_refup(L, t->cp, ret);
    cp_refdown(L, t->cp, old); /* ret == 0 = the old segment died */
    if (ret == 0) return *mask = 0, lua_settop(L, base), 0;
    n = (int)lua_tointeger(L, base + 2);
    *mask = 0;
    for (i = 0; i < n; ++i) sp_addns(mask, (int)lua_tointeger(L, base + 3 + i));
    return lua_settop(L, base), ret;
}

/* ---- styled flow ---- */

static void lst_foldover(lua_State *L, cp_State *cp, sp_Id at, int fold) {
    int tab;
    cp_attr(L, cp, at);
    tab = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, tab)) {
        lua_pushvalue(L, lua_gettop(L) - 1); /* key */
        lua_pushvalue(L, -2);                /* value */
        lua_rawset(L, fold);
        lua_pop(L, 1); /* value; key stays for lua_next */
    }
    lua_pop(L, 1); /* attr table */
}

static sp_Id lst_foldmake(
        lua_State *L, lst_Tree *t, sp_Id treeid, lst_Pair *rt, size_t nb,
        size_t na) {
    int    fold;
    size_t k;
    sp_Id  id;
    lua_newtable(L);
    fold = lua_gettop(L);
    lst_sort(t->cp, rt, (int)nb);
    for (k = 0; k < nb; ++k) lst_foldover(L, t->cp, rt[k].attr, fold);
    lst_foldover(L, t->cp, treeid, fold);
    lst_sort(t->cp, rt + nb, (int)na);
    for (k = 0; k < na; ++k) lst_foldover(L, t->cp, rt[nb + k].attr, fold);
    id = cp_internattr(L, t->cp, fold);
    lua_pop(L, 1); /* fold table */
    return id;
}

static void lst_boundscan(const lst_Tree *t, size_t x, size_t *ps, size_t *pe) {
    size_t s = *ps, e = *pe, s0, ee, i;
    for (i = 0; i < t->ephcnt; ++i) {
        const sv_List *ph = &t->ephs[i];
        if (ph->n == 0) continue;
        s0 = sv_upper(ph, x);
        if (s0 >= ph->n) {
            ee = ph->off[ph->n - 1] + ph->len[ph->n - 1];
            if (ee > s) s = ee;
        } else if (ph->off[s0] > x) {
            if (ph->off[s0] < e) e = ph->off[s0];
            if (s0 > 0) {
                ee = ph->off[s0 - 1] + ph->len[s0 - 1];
                if (ee > s) s = ee;
            }
        } else {
            if (ph->off[s0] > s) s = ph->off[s0];
            ee = ph->off[s0] + ph->len[s0];
            if (ee < e) e = ee;
        }
    }
    *ps = s, *pe = e;
}

static void lst_coverpairs(lst_Tree *t, size_t x, size_t *pnb, size_t *pna) {
    size_t   nb = 0, na = 0, qs, qe, i;
    unsigned id;
    for (i = 0; i < t->ephcnt; ++i) {
        const sv_List *ph = &t->ephs[i];
        unsigned       ns = (unsigned)(SP_MASK_BITS + 1 + i);
        if (ph->n == 0) continue;
        sv_cover(ph, x, &id, &qs, &qe);
        if (id == 0) continue;
        if (t->cp->prio[ns] >= 0) {
            t->rtmp[nb].ns = ns, t->rtmp[nb].attr = id;
            nb += 1;
        }
    }
    for (i = 0; i < t->ephcnt; ++i) {
        const sv_List *ph = &t->ephs[i];
        unsigned       ns = (unsigned)(SP_MASK_BITS + 1 + i);
        if (ph->n == 0) continue;
        sv_cover(ph, x, &id, &qs, &qe);
        if (id == 0) continue;
        if (t->cp->prio[ns] < 0) {
            t->rtmp[nb + na].ns = ns, t->rtmp[nb + na].attr = id;
            na += 1;
        }
    }
    *pnb = nb, *pna = na;
}

static sp_Id lst_mergecalc(
        lua_State *L, lst_Tree *t, sp_Id treeid, size_t ts, size_t te, size_t x,
        size_t *ps, size_t *pe) {
    size_t need, nb = 0, na = 0;
    lst_boundscan(t, x, &ts, &te);
    *ps = ts, *pe = te;
    need = t->ephcnt + 1;
    if (need > t->rtmpcap) {
        lua_Alloc f;
        void     *ud;
        size_t    ncap = t->rtmpcap ? t->rtmpcap * 2 : 128;
        while (need > ncap) ncap *= 2;
        f = lua_getallocf(L, &ud);
        t->rtmp = (lst_Pair *)f(
                ud, t->rtmp, t->rtmpcap * sizeof(lst_Pair),
                ncap * sizeof(lst_Pair));
        t->rtmpcap = ncap;
    }
    lst_coverpairs(t, x, &nb, &na);
    if (nb + na == 0) return treeid;
    return lst_foldmake(L, t, treeid, t->rtmp, nb, na);
}

static sp_Id lst_styledflow(
        lua_State *L, lst_Tree *t, sp_Cursor *C, size_t x, size_t *ps,
        size_t *pe) {
    size_t rem, te;
    sp_Id  treeid;
    if (x >= sp_bytes(t->T)) return *ps = x, *pe = x, 0;
    for (;;) {
        treeid = sp_style(C, &rem, NULL);
        te = C->off + C->poff + rem;
        if (x < te) break;
        sp_next(C, 0, &rem);
    }
    return lst_mergecalc(L, t, treeid, C->off, te, x, ps, pe);
}

/* ---- mark flow ---- */

static int lst_segmarks(lua_State *L, lst_Tree *t, sp_Id id, lst_Pair *ps) {
    return cp_expand(L, t->cp, id, ps);
}

static int lst_markpush(
        lua_State *L, lst_Tree *t, size_t off, size_t len, sp_Id attr) {
    lua_pushinteger(L, (lua_Integer)off);
    lua_pushinteger(L, (lua_Integer)len);
    cp_attr(L, t->cp, attr);
    lua_pushinteger(L, (lua_Integer)attr);
    return 4;
}

/* ---- edit helper ---- */

static void lst_edit(lua_State *L, lst_Cur *c) {
    c->tree->epoch += 1;
    c->epoch = c->tree->epoch;
    (void)L;
}

/* ---- ns lifecycle (tree-side: prune / slot release / refold) ---- */

static void lst_reprio(lua_State *L, lst_Tree *t) {
    sp_Cursor C;
    if (sp_bytes(t->T) == 0) return;
    sp_seek(&C, t->T, 0);
    lst_checkerror(
            L,
            sp_fill(&C, cp_op(L, t->cp, LTP_K_REORDER, 0, 0), sp_bytes(t->T)));
    t->epoch += 1;
}

static lua_Number lst_nsunreg(lua_State *L, lst_Tree *t, const char *name) {
    int        ns = cp_nsget(L, t->cp, name);
    lua_Number old;
    lua_Alloc  f;
    void      *ud;
    if (ns == 0) return luaL_error(L, "spantree: unknown namespace"), 0;
    old = t->cp->prio[ns];
    if (!lst_iseph(ns)) {
        lst_checkerror(
                L, sp_clear(
                           t->T, ns,
                           cp_op(L, t->cp, LTP_K_CLEAR, (unsigned)ns, 0)));
        t->epoch += 1;
    }
    cp_nsunregister(L, t->cp, name);
    if (lst_iseph(ns)) {
        sv_List *e = lst_ephslot(L, t, ns);
        f = lua_getallocf(L, &ud);
        f(ud, e->off, e->cap * sizeof(size_t), 0);
        f(ud, e->len, e->cap * sizeof(size_t), 0);
        f(ud, e->id, e->cap * sizeof(unsigned), 0);
        e->off = NULL, e->len = NULL, e->id = NULL, e->n = e->cap = 0;
        if (lst_ephidx(ns) + 1 == t->ephcnt) t->ephcnt = lst_ephidx(ns);
    }
    return old;
}

/* ---- tree methods ---- */

static int Lcur_iter(lua_State *L);

static int Ltree_gc(lua_State *L) {
    lst_Tree *t = ltree_check(L, 1);
    lua_Alloc f;
    void     *ud;
    size_t    i;
    f = lua_getallocf(L, &ud);
    for (i = 0; i < t->ephcap; ++i) {
        f(ud, t->ephs[i].off, t->ephs[i].cap * sizeof(size_t), 0);
        f(ud, t->ephs[i].len, t->ephs[i].cap * sizeof(size_t), 0);
        f(ud, t->ephs[i].id, t->ephs[i].cap * sizeof(unsigned), 0);
    }
    f(ud, t->ephs, t->ephcap * sizeof(sv_List), 0);
    f(ud, t->rtmp, t->rtmpcap * sizeof(lst_Pair), 0);
    if (t->T) sp_freetree(t->T), t->T = NULL;
    return 0;
}

static int Ltree_bytes(lua_State *L) {
    return lua_pushinteger(L, (lua_Integer)sp_bytes(ltree_check(L, 1)->T)), 1;
}

static int Ltree_intern(lua_State *L) {
    lst_Tree *t = ltree_check(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    return lua_pushinteger(L, (lua_Integer)cp_internattr(L, t->cp, 2)), 1;
}

static int Ltree_attr(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lua_Integer id = luaL_checkinteger(L, 2);
    cp_attr(L, t->cp, (sp_Id)id);
    return 1;
}

static int Ltree_setfields(lua_State *L) {
    lst_Tree    *t = ltree_check(L, 1);
    int          n, i;
    const char **fs;
    lua_Alloc    f;
    void        *ud;
    luaL_checktype(L, 2, LUA_TTABLE);
    n = (int)lua_rawlen(L, 2);
    for (i = 0; i < n; ++i) {
        lsp_rawgeti(L, 2, i + 1);
        luaL_checktype(L, -1, LUA_TSTRING); /* element must be a string */
        /* collected names stay on the stack: fs pointers need them alive */
    }
    if (n > 0) {
        f = lua_getallocf(L, &ud);
        fs = (const char **)f(ud, NULL, 0, (size_t)n * sizeof(char *));
        for (i = 0; i < n; ++i) fs[i] = lua_tostring(L, 3 + i);
        cp_setfields(L, t->cp, fs, n);
        lua_pop(L, n);
        f(ud, fs, (size_t)n * sizeof(char *), 0);
    } else
        cp_setfields(L, t->cp, NULL, 0);
    return lua_settop(L, 1), 1;
}

static int Ltree_mark(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    int         nsid = lst_nsid(L, t, 2);
    lua_Integer off = luaL_checkinteger(L, 4);
    lua_Integer len = luaL_checkinteger(L, 5);
    sp_Cursor   C;
    unsigned    op, a;
    luaL_argcheck(L, off >= 0, 4, "spantree: invalid offset");
    luaL_argcheck(L, len >= 0, 5, "spantree: invalid length");
    if (lua_type(L, 3) == LUA_TTABLE)
        a = cp_internattr(L, t->cp, 3);
    else {
        a = (unsigned)luaL_checkinteger(L, 3);
        luaL_argcheck(
                L, (lua_Integer)a < (lua_Integer)t->cp->next, 3,
                "spantree: unknown style id");
    }
    if (lst_iseph(nsid)) {
        sv_fill(L, lst_ephslot(L, t, nsid), (size_t)off, (size_t)len, a);
        return lua_pushinteger(L, (lua_Integer)a), 1;
    }
    op = cp_op(L, t->cp, LTP_K_WRITE, (unsigned)nsid, a);
    sp_seek(&C, t->T, (size_t)off);
    lst_checkerror(L, sp_fill(&C, op, (size_t)len));
    t->epoch += 1;
    return lua_pushinteger(L, (lua_Integer)a), 1;
}

static int lst_cleartree(
        lua_State *L, lst_Tree *t, size_t off, size_t len, int rng) {
    sp_Cursor C;
    size_t    i;
    sp_seek(&C, t->T, off);
    lst_checkerror(L, sp_fill(&C, 0, len));
    if (rng) {
        for (i = 0; i < t->ephcnt; ++i) sv_clear(&t->ephs[i], off, len);
    } else
        lst_ephresetall(t);
    return lua_settop(L, 1), t->epoch += 1, 1;
}

static int Ltree_clear(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    lua_Integer off = 0, len = 0;
    int         n = lua_gettop(L), nsid = 0;
    unsigned    op = 0;
    if (n == 2) {
        nsid = lst_nsid(L, t, 2);
        if (lst_iseph(nsid))
            return lst_ephslot(L, t, nsid)->n = 0, lua_settop(L, 1), 1;
        lst_checkerror(
                L, sp_clear(
                           t->T, nsid,
                           cp_op(L, t->cp, LTP_K_CLEAR, (unsigned)nsid, 0)));
        return lua_settop(L, 1), t->epoch += 1, 1;
    }
    if (n > 2) {
        nsid = lst_nsid(L, t, 2);
        off = luaL_checkinteger(L, 3), len = luaL_checkinteger(L, 4);
        luaL_argcheck(L, off >= 0, 3, "spantree: invalid offset");
        luaL_argcheck(L, len >= 0, 4, "spantree: invalid length");
        if (lst_iseph(nsid))
            return sv_clear(lst_ephslot(L, t, nsid), (size_t)off, (size_t)len),
                   lua_settop(L, 1), 1;
        if (nsid > 0) op = cp_op(L, t->cp, LTP_K_CLEAR, (unsigned)nsid, 0);
    } else
        len = (lua_Integer)sp_bytes(t->T);
    if (nsid > 0) {
        sp_Cursor C;
        sp_seek(&C, t->T, (size_t)off);
        lst_checkerror(L, sp_fill(&C, op, (size_t)len));
        return lua_settop(L, 1), t->epoch += 1, 1;
    }
    return lst_cleartree(L, t, (size_t)off, (size_t)len, n > 2);
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
    lst_Pair    ps[SP_MASK_BITS + 1];
    sp_Cursor   C;
    lua_Integer id = luaL_checkinteger(L, 2);
    size_t      count = 0, len;
    int         n, i;
    sp_seek(&C, t->T, 0);
    for (;;) { /* pass 1: count matching segments */
        sp_Id sid = sp_style(&C, &len, NULL);
        if (sid == 0 && len == 0) break;
        n = cp_expand(L, t->cp, sid, ps);
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
        n = cp_expand(L, t->cp, sid, ps);
        for (i = 0; i < n; ++i) {
            if ((lua_Integer)ps[i].attr != id) continue;
            sp_locate(&C, segstart);
            lst_checkerror(
                    L,
                    sp_fill(&C,
                            cp_op(L, t->cp, LTP_K_CLEAR, (unsigned)ps[i].ns, 0),
                            len));
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

static int lst_nsquery(lua_State *L, lst_Tree *t, const char *name) {
    int ns = cp_nsget(L, t->cp, name);
    if (ns == 0) return lua_pushnil(L), 1;
    lua_pushnumber(L, t->cp->prio[ns]);
    if (lst_iseph(ns))
        lua_pushliteral(L, "ephemeral");
    else
        lua_pushnil(L);
    return 2;
}

static int Ltree_namespace(lua_State *L) {
    lst_Tree   *t = ltree_check(L, 1);
    int         top = lua_gettop(L), ns, eph = 0, strict = 0, existed;
    const char *name = luaL_checklstring(L, 2, NULL);
    const char *flags, *fp;
    lua_Number  p, old;
    luaL_argcheck(
            L, lua_rawlen(L, 2) > 0, 2, "spantree: invalid namespace name");
    if (top == 2) return lst_nsquery(L, t, name);
    if (lua_isnil(L, 3)) /* unregister */
        return lua_pushnumber(L, lst_nsunreg(L, t, name)), 1;
    p = luaL_checknumber(L, 3);
    if (top >= 4) {
        flags = luaL_checklstring(L, 4, NULL);
        for (fp = flags; *fp; ++fp) {
            if (*fp == 'c')
                strict = 1;
            else if (*fp == 'e')
                eph = 1;
            else
                luaL_error(L, "spantree: invalid namespace flags");
        }
    }
    ns = cp_nsget(L, t->cp, name);
    if (strict && ns != 0)
        return luaL_error(L, "spantree: namespace already registered"), 0;
    existed = ns != 0 && lst_iseph(ns) != eph;
    if (existed) old = lst_nsunreg(L, t, name);
    if (cp_nsregister(L, t->cp, name, p, eph, &old)) {
        if (!eph && p != old) lst_reprio(L, t);
        return lua_pushnumber(L, old), 1;
    }
    if (existed) return lua_pushnumber(L, old), 1;
    return lua_pushnil(L), 1;
}

/* ---- iterator body ---- */

static int lst_iterstyled(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    sp_Id     id;
    size_t    x = c->mcur, s, e, len;
    id = lst_styledflow(L, t, &c->C, x, &s, &e);
    if (e <= x) return 0;
    len = e - x;
    if (len > c->endoff - x) len = c->endoff - x;
    lst_markpush(L, t, x, len, id);
    c->mcur = x + len;
    return 4;
}

static int lst_itereph(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    sv_List  *ph = lst_ephslot(L, t, c->nsid);
    size_t    x = c->mcur, s = sv_upper(ph, x), e, start;
    if (s >= ph->n) return 0;
    if (ph->off[s] >= c->endoff) return 0;
    e = ph->off[s] + ph->len[s];
    if (e > c->endoff) e = c->endoff;
    start = ph->off[s] > x ? ph->off[s] : x;
    lst_markpush(L, t, start, e - start, ph->id[s]);
    c->mcur = e;
    return 4;
}

static int lst_iterns(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    lst_Pair  ps[SP_MASK_BITS + 1];
    size_t    len, start, len2;
    sp_Id     id;
    int       i, n;
    for (;;) {
        id = sp_next(&c->C, c->nsid, &len);
        if (id == 0) return 0;
        n = lst_segmarks(L, t, id, ps);
        for (i = 0; i < n; ++i)
            if ((int)ps[i].ns == c->nsid) break;
        if (i < n) break;
    }
    start = sp_offset(&c->C); /* sp_next lands at the segment start */
    if (start >= c->endoff) return 0;
    len2 = len;
    if (len2 > c->endoff - start) len2 = c->endoff - start;
    lst_markpush(L, t, start, len2, ps[i].attr);
    return 4;
}

static int lst_iterany(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    lst_Pair  ps[SP_MASK_BITS + 1];
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
        n = lst_segmarks(L, t, id, ps);
        if (c->midx < (size_t)n) {
            if (c->mbase >= c->endoff) return 0; /* seg starts past window */
            break;
        }
        c->mlen = 0;
        sp_next(&c->C, 0, NULL); /* hole segs read id 0: style-based stop */
    }
    len2 = c->mlen;
    if (len2 > c->endoff - c->mbase) len2 = c->endoff - c->mbase;
    lst_markpush(L, t, c->mbase, len2, ps[c->midx].attr);
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
    lst_Cur *c = lcur_check(L, 1);
    lst_Pair ps[SP_MASK_BITS + 1];
    sp_Id    id;
    size_t   rem, base, len, x = sp_offset(&c->C);
    int      n;
    if (x >= sp_bytes(c->tree->T)) return 0;
    id = sp_style(&c->C, &rem, NULL);
    if (id == 0 || rem == 0) return 0;
    base = c->C.off, len = c->C.poff + rem;
    n = lst_segmarks(L, c->tree, id, ps);
    if (c->mlen == 0 || c->mbase != base || c->mlen != len)
        c->mbase = base, c->mlen = len, c->midx = 0;
    if (c->midx >= (size_t)n) return 0;
    lst_markpush(L, c->tree, base, len, ps[c->midx].attr);
    return 4;
}

static int lst_nexteph(lua_State *L, lst_Cur *c, int nsid) {
    lst_Tree *t = c->tree;
    sv_List  *ph = lst_ephslot(L, t, nsid);
    size_t    e, start, x = sp_offset(&c->C), s = sv_upper(ph, x);
    if (s >= ph->n) return 0;
    e = ph->off[s] + ph->len[s];
    start = ph->off[s] > x ? ph->off[s] : x;
    lst_markpush(L, t, start, e - start, ph->id[s]);
    sp_advance(&c->C, (sp_Delta)(e - x));
    return 4;
}

static int lst_nextns(lua_State *L, lst_Cur *c, int nsid) {
    lst_Tree *t = c->tree;
    lst_Pair  ps[SP_MASK_BITS + 1];
    sp_Id     id;
    size_t    len;
    int       i, n;
    for (;;) {
        id = sp_next(&c->C, nsid, &len);
        if (id == 0) return 0;
        n = lst_segmarks(L, t, id, ps);
        for (i = 0; i < n; ++i)
            if ((int)ps[i].ns == nsid) break;
        if (i < n) break;
    }
    lst_markpush(L, t, sp_offset(&c->C), len, ps[i].attr); /* seg start */
    return 4;
}

static int lst_nextany(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    lst_Pair  ps[SP_MASK_BITS + 1];
    sp_Id     id;
    size_t    rem;
    int       n;
    if (c->mlen > 0) {
        sp_locate(&c->C, c->mbase);
        id = sp_style(&c->C, &rem, NULL);
        n = lst_segmarks(L, t, id, ps);
        if (c->midx + 1 < (size_t)n) {
            c->midx += 1;
            lst_markpush(L, t, c->mbase, c->mlen, ps[c->midx].attr);
            return 4;
        }
    }
    sp_next(&c->C, 0, &rem);
    id = sp_style(&c->C, &rem, NULL);
    if (id == 0 && rem == 0) return 0;
    n = lst_segmarks(L, t, id, ps);
    c->mbase = c->C.off, c->mlen = c->C.poff + rem, c->midx = 0;
    lst_markpush(L, t, c->mbase, c->mlen, ps[0].attr);
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
    sv_List  *ph = lst_ephslot(L, t, nsid);
    size_t    j, x = sp_offset(&c->C), s = sv_upper(ph, x);
    if (ph->n == 0) return 0;
    j = (s < ph->n && ph->off[s] < x) ? s : s - 1;
    if (j >= ph->n) return 0;
    sp_locate(&c->C, ph->off[j]);
    lst_markpush(L, t, ph->off[j], ph->len[j], ph->id[j]);
    return 4;
}

static int lst_prevns(lua_State *L, lst_Cur *c, int nsid) {
    lst_Tree *t = c->tree;
    lst_Pair  ps[SP_MASK_BITS + 1];
    sp_Id     id;
    size_t    len;
    int       i, n;
    for (;;) {
        id = sp_prev(&c->C, nsid, &len);
        if (id == 0) return 0;
        id = sp_style(&c->C, &len, NULL); /* full seg len: sp_prev gives a
                                           * partial len mid-segment */
        if (id == 0) return 0;
        n = lst_segmarks(L, t, id, ps);
        for (i = 0; i < n; ++i)
            if ((int)ps[i].ns == nsid) break;
        if (i < n) break;
    }
    lst_markpush(L, t, sp_offset(&c->C), len, ps[i].attr);
    return 4;
}

static int lst_prevany(lua_State *L, lst_Cur *c) {
    lst_Tree *t = c->tree;
    lst_Pair  ps[SP_MASK_BITS + 1];
    sp_Id     id;
    size_t    len;
    int       n;
    if (c->mlen > 0 && c->midx > 0) {
        sp_locate(&c->C, c->mbase);
        id = sp_style(&c->C, &len, NULL);
        n = lst_segmarks(L, t, id, ps);
        c->midx -= 1;
        lst_markpush(L, t, c->mbase, c->mlen, ps[c->midx].attr);
        return 4;
    }
    sp_prev(&c->C, 0, &len);
    if (len == 0) return 0; /* tree head: stop (id 0 = a real empty seg) */
    id = sp_style(&c->C, &len, NULL);
    n = lst_segmarks(L, t, id, ps);
    c->mbase = c->C.off, c->mlen = c->C.poff + len, c->midx = (size_t)n - 1;
    lst_markpush(L, t, c->mbase, c->mlen, ps[n - 1].attr);
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
    unsigned    op, a;
    luaL_argcheck(L, len >= 0, 4, "spantree: invalid length");
    if (lua_type(L, 3) == LUA_TTABLE)
        a = cp_internattr(L, c->tree->cp, 3);
    else {
        a = (unsigned)luaL_checkinteger(L, 3);
        luaL_argcheck(
                L, (lua_Integer)a < (lua_Integer)c->tree->cp->next, 3,
                "spantree: unknown style id");
    }
    if (lst_iseph(nsid)) {
        sv_fill(L, lst_ephslot(L, c->tree, nsid), sp_offset(&c->C), (size_t)len,
                a);
        return lua_pushinteger(L, (lua_Integer)a), 1;
    }
    op = cp_op(L, c->tree->cp, LTP_K_WRITE, (unsigned)nsid, a);
    lst_checkerror(L, sp_fill(&c->C, op, (size_t)len));
    lst_edit(L, c);
    return lua_pushinteger(L, (lua_Integer)a), 1;
}

static int Lcur_clear(lua_State *L) {
    lst_Cur    *c = lcur_check(L, 1);
    lua_Integer len;
    size_t      i;
    unsigned    op = 0;
    int         nsid;
    if (lua_gettop(L) == 2) { /* all layers */
        len = luaL_checkinteger(L, 2);
        for (i = 0; i < c->tree->ephcnt; ++i)
            sv_clear(&c->tree->ephs[i], sp_offset(&c->C), (size_t)len);
    } else {
        nsid = lst_nsid(L, c->tree, 2);
        len = luaL_checkinteger(L, 3);
        if (lst_iseph(nsid)) {
            sv_clear(
                    lst_ephslot(L, c->tree, nsid), sp_offset(&c->C),
                    (size_t)len);
            return lua_settop(L, 1), 1;
        }
        if (nsid > 0)
            op = cp_op(L, c->tree->cp, LTP_K_CLEAR, (unsigned)nsid, 0);
        if (nsid == 0)
            for (i = 0; i < c->tree->ephcnt; ++i)
                sv_clear(&c->tree->ephs[i], sp_offset(&c->C), (size_t)len);
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

static int Lsp_new(lua_State *L) {
    lst_Tree *t = (lst_Tree *)lua_newuserdata(L, sizeof(lst_Tree));
    memset(t, 0, sizeof(*t));
    t->cp = cp_get(L);
    t->L = L;
    t->T = sp_newtree(lst_state(L));
    sp_setarbiter(t->T, lst_arb, t);
    lst_setuv(L, "tree");
    luaL_setmetatable(L, LSP_TREE_TYPE);
    return 1;
}

/* ---- module registration ---- */

static const luaL_Reg tree_libs[] = {
        {"__gc", Ltree_gc},
        {"bytes", Ltree_bytes},
        {"namespace", Ltree_namespace},
        {"mark", Ltree_mark},
        {"clear", Ltree_clear},
        {"splice", Ltree_splice},
        {"append", Ltree_append},
        {"insert", Ltree_insert},
        {"remove", Ltree_remove},
        {"span", Ltree_span},
        {"styled", Ltree_styled},
        {"unmark", Ltree_unmark},
        {"cursor", Ltree_cursor},
        {"seek", Ltree_seek},
        {"intern", Ltree_intern},
        {"attr", Ltree_attr},
        {"setfields", Ltree_setfields},
        {NULL, NULL}};

static const luaL_Reg cur_libs[] = {{"__gc", Lcur_gc},
                                    {"seek", Lcur_seek},
                                    {"locate", Lcur_locate},
                                    {"advance", Lcur_advance},
                                    {"offset", Lcur_offset},
                                    {"style", Lcur_style},
                                    {"next", Lcur_next},
                                    {"prev", Lcur_prev},
                                    {"mark", Lcur_mark},
                                    {"clear", Lcur_clear},
                                    {"splice", Lcur_splice},
                                    {"append", Lcur_append},
                                    {"insert", Lcur_insert},
                                    {"remove", Lcur_remove},
                                    {NULL, NULL}};

LUALIB_API int luaopen_spantree(lua_State *L) {
    luaL_Reg libs[] = {{"new", Lsp_new}, {NULL, NULL}};
    lst_state(L);
    if (luaL_newmetatable(L, LSP_TREE_TYPE)) {
        luaL_setfuncs(L, tree_libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
    if (luaL_newmetatable(L, LSP_CUR_TYPE)) {
        luaL_setfuncs(L, cur_libs, 0);
        lua_pushvalue(L, -1), lua_setfield(L, -2, "__index");
    }
    luaL_setfuncs(L, libs, 0);
    return 1;
}
