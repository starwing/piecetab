#ifndef textmatch_h
#define textmatch_h

#ifndef TM_NS_BEGIN
# ifdef __cplusplus
#   define TM_NS_BEGIN extern "C" {
#   define TM_NS_END   }
# else
#   define TM_NS_BEGIN
#   define TM_NS_END
# endif
#endif

#ifndef TM_STATIC
# if __GNUC__
#   define TM_STATIC static __attribute((unused))
# else
#   define TM_STATIC static
# endif
#endif

#ifdef TM_STATIC_API
# ifndef TM_IMPLEMENTATION
#   define TM_IMPLEMENTATION
# endif
# define TM_API TM_STATIC
#endif

#if !defined(TM_API) && defined(_WIN32)
# ifdef TM_IMPLEMENTATION
#   define TM_API __declspec(dllexport)
# else
#   define TM_API __declspec(dllimport)
# endif
#endif

#ifndef TM_API
# define TM_API extern
#endif

#include <stddef.h>

#define TM_MATCHED    (1)
#define TM_OK         (0)
#define TM_ERRPARAM   (-1)
#define TM_ERRPATTERN (-2)
#define TM_ERRCOMPLEX (-3)

TM_NS_BEGIN

/* clang-format off */
typedef struct tm_Slice { const char *s, *e; } tm_Slice;
typedef struct tm_Capture { size_t start, len; } tm_Capture;
/* clang-format on */

typedef tm_Slice tm_Reader(void *ud, size_t *poff);

typedef struct tm_State tm_State;

#define TM_LITERAL    (1 << 0)
#define TM_LINEANCHOR (1 << 1)

#ifndef utfint
# define utfint utfint
typedef unsigned int utfint;
#endif /* utfint */

#define TM_UNKNOWN ((utfint)(-2)) /* not decoded yet */
#define TM_EOS     ((utfint)(-1)) /* end of source / before start */

#ifndef TM_MAX_PATTERN_COUNT
# define TM_MAX_PATTERN_COUNT 9
#endif

typedef struct tm_Match {
    tm_Slice pat;   /* current pattern */
    size_t   end;   /* last successful match end */
    int      depth; /* remaining recursion budget */
    int      level; /* open capture count */
    int      flags; /* TM_LITERAL / TM_LINEANCHOR */

    tm_Capture cap[TM_MAX_PATTERN_COUNT]; /* capture scratch */
} tm_Match;

struct tm_State {
    tm_Match m;

    tm_Reader *reader; /* reader callback */
    void      *ud;     /* reader user data */

    tm_Slice    cache; /* cached piece slice */
    size_t      base;  /* cached piece start offset */
    const char *p;     /* current byte in cache; NULL when not materialized */

    size_t off;  /* current source position */
    size_t next; /* offset of current cp+1; valid when current != TM_UNKNOWN */
    utfint current; /* codepoint at pos; TM_UNKNOWN or TM_EOS */
    utfint prev;    /* codepoint before pos; TM_UNKNOWN, 0 = start/NUL */
};

/* initialization */
TM_API void tm_init(tm_State *S, tm_Reader *r, void *ud);

#define tm_flags(S)       ((S) ? (S)->m.flags : 0)
#define tm_setflags(S, f) ((void)((S) && ((S)->m.flags = (f))))

TM_API tm_Slice tm_slice(const char *s, size_t len);
TM_API tm_Slice tm_string(const char *s);

/* seek */
TM_API int tm_seek(tm_State *S, size_t off);
TM_API int tm_advance(tm_State *S, ptrdiff_t delta);

/* matching */
TM_API int tm_pattern(tm_State *S, tm_Slice pattern);
TM_API int tm_match(tm_State *S);
TM_API int tm_find(tm_State *S);

/* result access */
#define tm_offset(S)   ((S) ? (S)->off : 0)
#define tm_matchend(S) ((S) ? (S)->m.end : 0)

/* captures */
#define tm_captures(S) ((S) ? (S)->m.level : 0)

TM_API int tm_capture(const tm_State *S, int i, tm_Capture *out);

TM_NS_END

#endif /* textmatch_h */

/* ======================================================================== */
/*                           IMPLEMENTATION                                 */
/* ======================================================================== */

#if defined(TM_IMPLEMENTATION) && !defined(tm_implemented)
#define tm_implemented

#ifndef TM_MAXCCALLS
# define TM_MAXCCALLS 200
#endif /* TM_MAXCCALLS */

#define TM_UTFMAX    6
#define TM_CMP_CHUNK 64

#define tmC(ch) ((utfint)((ch) & 0xFF))
#define tmOK(x)                        \
    do {                               \
        int tmOK_r = (x);              \
        if (tmOK_r < 0) return tmOK_r; \
    } while (0)

#define TM_ESC      '%'
#define TM_SPECIALS "^$*+?.([%-"

#define TM_CAP_UNFINISHED ((size_t)-1)
#define TM_CAP_POSITION   ((size_t)-2)
#define TM_NOMATCH        ((size_t)-1)

#define TM_CONTINUE (2)

#ifndef TM_IS
# define TM_IS(cat, c) is##cat((int)(c))
#include <ctype.h>
#endif /* TM_IS */

#include <assert.h>
#include <string.h>

TM_NS_BEGIN

/* clang-format off */
TM_API tm_Slice tm_slice(const char *s, size_t len)
{ tm_Slice p; p.s = s; p.e = len ? s + len : s; return p; }

static size_t tmS_len(tm_Slice s) { return (size_t)(s.e - s.s); }

static int tmS_incache(tm_State *S, size_t off)
{ return S->cache.s && S->base <= off && off < S->base + tmS_len(S->cache); }
/* clang-format on */

TM_API tm_Slice tm_string(const char *s) { return tm_slice(s, strlen(s)); }
static int      tmS_empty(tm_Slice s) { return !s.s || s.s >= s.e; }

void tm_init(tm_State *S, tm_Reader *r, void *ud) {
    memset(S, 0, sizeof(*S));
    S->reader = r, S->ud = ud;
    S->m.depth = TM_MAXCCALLS;
    S->current = TM_UNKNOWN, S->prev = TM_UNKNOWN;
}

static size_t tmS_utflen(utfint c) {
    assert(c >= 0x80);
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    if ((c & 0xFC) == 0xF8) return 5;
    if ((c & 0xFE) == 0xFC) return 6;
    return 1;
}

static int tmS_decodelen(tm_Slice *s, size_t need, utfint *val) {
    static const utfint masks[] = {0, 0, 0x1F, 0x0F, 0x07, 0x03, 0x01};
    utfint              cp = *val, old = cp;
    size_t              i;
    assert(tmS_len(*s) >= need);
    for (cp &= masks[need], i = 1; i < need; ++i) {
        utfint cc = tmC(s->s[i]);
        if ((cc & 0xC0) != 0x80) return (*val = old, s->s += 1), 1;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return (*val = cp, s->s += need), (int)need;
}

static int tmS_decode(tm_Slice *s, utfint *val) {
    size_t need;
    utfint cp;
    int    r;
    if (tmS_empty(*s)) return 0;
    *val = (cp = tmC(*s->s));
    r = cp < 0x80 || (need = tmS_utflen(cp)) == 1 || need > tmS_len(*s);
    return r ? (s->s += 1, 1) : tmS_decodelen(s, need, val);
}

/* --- physical source access ------------------------------------------- */

static int tmS_load(tm_State *S, size_t off) {
    size_t   poff = off;
    tm_Slice p;
    if (tmS_incache(S, off)) return 1;
    S->p = NULL;
    p = S->reader(S->ud, &poff);
    if (!tmS_empty(p)) return (S->base = poff, S->cache = p), 1;
    return (S->base = 0, S->cache = tm_slice(NULL, 0)), 0;
}

static tm_Slice tmS_at(tm_State *S, size_t off) {
    size_t prefix;
    if (!tmS_load(S, off)) return tm_slice(NULL, 0);
    prefix = off - S->base;
    return tm_slice(S->cache.s + prefix, tmS_len(S->cache) - prefix);
}

static size_t tmS_copy(tm_State *S, size_t off, char *buf, size_t n) {
    size_t have = 0;
    while (have < n) {
        tm_Slice cur = tmS_at(S, off + have);
        size_t   avail, take;
        if (!cur.s) break;
        avail = tmS_len(cur), take = n - have;
        if (take > avail) take = avail;
        memcpy(buf + have, cur.s, take), have += take;
    }
    return have;
}

static const char *tmU_setp(tm_State *S) {
    if (!tmS_incache(S, S->off)) return S->p = NULL;
    return S->p = S->cache.s + (S->off - S->base);
}

/* --- centralized source cursor ---------------------------------------- */

/* clang-format off */
static void tmU_reset(tm_State *S)
{ S->next = 0, S->current = TM_UNKNOWN, S->prev = TM_UNKNOWN; tmU_setp(S); }

static size_t tmU_offset(tm_Match *M)
{ tm_State *S = (tm_State *)M; return S->off; }
/* clang-format on */

static int tmU_goto(tm_State *S, size_t off) {
    if (off == S->off) return tmU_reset(S), 0;
    S->off = off, S->next = 0, S->current = TM_UNKNOWN, S->prev = TM_UNKNOWN;
    if (!tmS_incache(S, off)) S->base = 0, S->cache = tm_slice(NULL, 0);
    return tmU_setp(S), 1;
}

static utfint tmU_peek(tm_Match *M) {
    char      buf[TM_UTFMAX];
    tm_State *S = ((tm_State *)M);
    size_t    have, need;
    tm_Slice  cur;
    utfint    ch;
    if (S->current != TM_UNKNOWN) return S->current;
    if (S->p == NULL) {
        if (!tmS_load(S, S->off)) return (S->next = 0), S->current = TM_EOS;
        (void)tmU_setp(S);
        assert(S->p != NULL);
    }
    if ((ch = tmC(*S->p)) < 0x80 || (need = tmS_utflen(ch)) == 1)
        return S->next = S->off + 1, S->current = ch;
    if ((have = (size_t)(S->cache.e - S->p)) >= need) {
        cur = tm_slice(S->p, need);
        S->next = S->off + tmS_decodelen(&cur, need, &ch);
        return S->current = ch;
    }
    cur = tm_slice(buf, tmS_copy(S, S->off, buf, need));
    return (S->next = S->off + tmS_decode(&cur, &ch)), S->current = ch;
}

static utfint tmU_next(tm_Match *M) {
    tm_State *S = ((tm_State *)M);
    assert(S->current != TM_UNKNOWN);
    assert(S->current != TM_EOS);
    S->prev = S->current, S->current = TM_UNKNOWN;
    S->off = S->next, S->next = 0;
    return tmU_setp(S), tmU_peek(M);
}

static utfint tmU_prev(tm_Match *M) {
    char      buf[TM_UTFMAX];
    tm_State *S = ((tm_State *)M);
    tm_Slice  cur;
    size_t    pos, start, len, have;
    utfint    val;
    S->prev = TM_UNKNOWN;
    assert(S->off != 0);
    pos = S->off - 1, start = pos;
    for (; start > 0; --start) {
        cur = tmS_at(S, start);
        if (!cur.s || ((unsigned char)*cur.s & 0xC0) != 0x80) break;
    }
    have = tmS_copy(S, start, buf, TM_UTFMAX), cur = tm_slice(buf, have);
    if (!tmS_decode(&cur, &val))
        return (S->next = 0), tmU_setp(S), S->current = 0;
    if (start + (len = (size_t)(cur.s - buf)) <= pos) {
        cur = tmS_at(S, pos);
        assert(cur.s);
        start = pos, len = 1, val = tmC(*cur.s);
    }
    S->off = start, S->next = start + len;
    return tmU_setp(S), S->current = val;
}

/* --- cursor save/restore and predicates ------------------------------- */

typedef struct tm_Save {
    size_t pos, next;
    utfint current, prev;
} tm_Save;

static tm_Save tmU_save(tm_Match *M) {
    tm_Save   r;
    tm_State *S = (tm_State *)M;
    r.pos = S->off, r.next = S->next;
    r.current = S->current, r.prev = S->prev;
    return r;
}

static void tmU_restore(tm_Match *M, const tm_Save *save) {
    tm_State *S = (tm_State *)M;
    S->off = save->pos, S->next = save->next;
    S->current = save->current, S->prev = save->prev;
    if (!tmS_incache(S, S->off)) S->base = 0, S->cache = tm_slice(NULL, 0);
    tmU_setp(S);
}

static utfint tmU_prevcp(tm_Match *M) {
    tm_State *S = (tm_State *)M;
    int       boundary;
    tm_Save   save;
    utfint    p;
    if (S->prev != TM_UNKNOWN) return S->prev;
    if (S->off == 0) return 0;
    save = tmU_save(M);
    p = tmU_prev(M), boundary = (S->next == save.pos);
    tmU_restore(M, &save);
    if (boundary) S->prev = p;
    return p;
}

/* --- byte-range helpers ----------------------------------------------- */

#define tm_min(a, b) ((a) < (b) ? (a) : (b))

static int tmS_equalmem(tm_State *S, size_t off, tm_Slice mem) {
    size_t n;
    for (; mem.s < mem.e; off += n, mem.s += n) {
        tm_Slice cur = tmS_at(S, off);
        if (!cur.s) return 0;
        if ((n = tmS_len(cur)) > tmS_len(mem)) n = tmS_len(mem);
        if (memcmp(cur.s, mem.s, n) != 0) return 0;
    }
    return 1;
}

static int tmS_equal(tm_State *S, size_t a, size_t b, size_t len) {
    while (len > 0) {
        if (tmS_incache(S, a) && tmS_incache(S, b)) {
            const char *pa, *pb;
            size_t      n;
            pa = S->cache.s + (a - S->base);
            pb = S->cache.s + (b - S->base);
            assert(pa <= pb);
            n = (size_t)(S->cache.e - pb);
            if (n > len) n = len;
            if (memcmp(pa, pb, n) != 0) return 0;
            a += n, b += n, len -= n;
        } else {
            char   abuf[TM_CMP_CHUNK], bbuf[TM_CMP_CHUNK];
            size_t n = len > TM_CMP_CHUNK ? TM_CMP_CHUNK : len;
            if (tmS_copy(S, a, abuf, n) != n) return 0;
            if (tmS_copy(S, b, bbuf, n) != n) return 0;
            if (memcmp(abuf, bbuf, n) != 0) return 0;
            a += n, b += n, len -= n;
        }
    }
    return 1;
}

static size_t tmS_find(tm_State *S, size_t from, tm_Slice pat) {
    char     buf[TM_CMP_CHUNK];
    size_t   r, plen = tmS_len(pat), off = from, buf_off = 0;
    tm_Slice cur, suffix;
    assert(plen > 0);
    cur = tmS_at(S, off);
    while (cur.s) {
        const char *p = memchr(cur.s, *pat.s, tmS_len(cur));
        if (!p) {
            off += tmS_len(cur), cur = tmS_at(S, off), buf_off = 0;
            continue;
        }
        off += (size_t)(p - cur.s) + 1, cur.s = p + 1, r = tmS_len(cur);
        if (memcmp(cur.s, pat.s + 1, tm_min(r, plen - 1)) != 0) continue;
        if (r >= plen - 1) return off - 1;
        suffix = tm_slice(pat.s + r + 1, plen - r - 1);
        if (r < TM_CMP_CHUNK && !buf_off) memcpy(buf, cur.s, r), buf_off = off;
        if (tmS_equalmem(S, off + r, suffix)) return off - 1;
        if (r < TM_CMP_CHUNK) cur = tm_slice(buf + (size_t)(off - buf_off), r);
        if (tmS_empty(cur) || !buf_off) cur = tmS_at(S, off), buf_off = 0;
    }
    return TM_NOMATCH;
}

static int tmU_backref(tm_Match *M, utfint ch) {
    tm_State *S = (tm_State *)M;
    int       idx;
    size_t    len;
    if (ch < '1' || ch > '9') return TM_ERRPATTERN;
    idx = (int)(ch - '1');
    if (idx >= M->level || M->cap[idx].len == TM_CAP_UNFINISHED)
        return TM_ERRPATTERN;
    if ((len = M->cap[idx].len) == TM_CAP_POSITION) return TM_CONTINUE;
    if (!tmS_equal(S, M->cap[idx].start, S->off, len)) return TM_OK;
    return (void)(len && tmU_goto(S, S->off + len)), TM_CONTINUE;
}

static int tmU_isboundary(tm_Match *M) {
    tm_State *S = (tm_State *)M;
    tm_Save   save;
    int       ok;
    if (S->off == 0) return 1;
    if (S->prev != TM_UNKNOWN) return 1;
    save = tmU_save(M), (void)tmU_prev(M);
    return (ok = (S->next == save.pos)), tmU_restore(M, &save), ok;
}

static int tmU_islinestart(tm_Match *M) {
    tm_State *S = (tm_State *)M;
    tm_Save   save;
    utfint    p;
    if (S->off == 0) return 1;
    if (S->prev != TM_UNKNOWN) return S->prev == '\n';
    save = tmU_save(M), p = tmU_prev(M), tmU_restore(M, &save);
    return (S->prev = p), p == '\n';
}

/* === match layer begin === */

static int tmM_class(utfint c, utfint cl) {
    utfint orig = cl;
    int    res, neg = (cl >= 'A' && cl <= 'Z');
    if (neg) cl += 0x20;
#ifndef iscompose
# define iscompose(c) ((void)(c), 0)
# define TM_ISCOMPOSE_LOCAL
#endif
    switch (cl) {
    case 'a': res = TM_IS(alpha, c); break;
    case 'c': res = TM_IS(cntrl, c); break;
    case 'd': res = TM_IS(digit, c); break;
    case 'g': res = TM_IS(graph, c); break;
    case 'l': res = TM_IS(lower, c); break;
    case 'p': res = TM_IS(punct, c); break;
    case 's': res = TM_IS(space, c); break;
    case 't': res = TM_IS(compose, c); break;
    case 'u': res = TM_IS(upper, c); break;
    case 'w': res = TM_IS(alnum, c); break;
    case 'x': res = TM_IS(xdigit, c); break;
    case 'z': res = (c == 0); break;
    default: return (orig == c);
    }
#ifdef TM_ISCOMPOSE_LOCAL
# undef iscompose
# undef TM_ISCOMPOSE_LOCAL
#endif
    return neg ? !res : res;
}

static int tmM_match(tm_Match *M, tm_Slice pat);
static int tmM_default(tm_Match *M, tm_Slice *pat);

static int tmM_toclosecapture(tm_Match *M) {
    int level = M->level;
    while (--level >= 0)
        if (M->cap[level].len == TM_CAP_UNFINISHED) return level;
    return TM_ERRPATTERN;
}

static int tmM_classend(tm_Slice *pat) {
    utfint ch, v;
    assert(!tmS_empty(*pat));
    (void)tmS_decode(pat, &ch);
    switch (ch) {
    case TM_ESC:
        assert(!tmS_empty(*pat));
        (void)tmS_decode(pat, &v);
        return 1;
    case '[':
        if (pat->s < pat->e && *pat->s == '^') pat->s++;
        for (;;) {
            if (tmS_empty(*pat)) return TM_ERRPATTERN;
            if (*(pat->s++) == TM_ESC && pat->s < pat->e) pat->s++;
            if (tmS_empty(*pat)) return TM_ERRPATTERN;
            if (*pat->s == ']') break;
        }
        return (pat->s += 1), 1;
    default: return 1;
    }
}

static int tmM_bracketclass(utfint c, tm_Slice pat) {
    int sig = 1;
    assert(*pat.s == '[');
    if (*++pat.s == '^') sig = 0, pat.s++;
    while (pat.s + 1 < pat.e) {
        utfint ch, next;
        assert(!tmS_empty(pat));
        (void)tmS_decode(&pat, &ch);
        if (ch == TM_ESC) {
            assert(!tmS_empty(pat));
            (void)tmS_decode(&pat, &ch);
            if (tmM_class(c, ch)) return sig;
        } else {
            if (ch == c) return sig;
            if (pat.s + 1 < pat.e && *pat.s == '-') {
                if (pat.s + 2 < pat.e) {
                    pat.s += 1;
                    assert(!tmS_empty(pat));
                    (void)tmS_decode(&pat, &next);
                    if (ch <= c && c <= next) return sig;
                }
            }
        }
    }
    return !sig;
}

static int tmM_single(tm_Match *M, tm_Slice pat) {
    utfint ch, pch;
    ch = tmU_peek(M);
    if (ch == TM_EOS) return 0;
    assert(!tmS_empty(pat));
    (void)tmS_decode(&pat, &pch);
    switch (pch) {
    default: return pch == ch;
    case '.': return 1;
    case '[': return pat.s -= 1, tmM_bracketclass(ch, pat);
    case TM_ESC:
        assert(!tmS_empty(pat));
        (void)tmS_decode(&pat, &pch);
        return tmM_class(ch, pch);
    }
}

static int tmM_balance(tm_Match *M, tm_Slice *pat) {
    utfint  ch, l, r;
    int     cont = 1;
    tm_Save save;
    if (!tmS_decode(pat, &l) || !tmS_decode(pat, &r)) return TM_ERRPATTERN;
    if (save = tmU_save(M), (ch = tmU_peek(M)) != TM_EOS && ch == l) {
        while ((ch = tmU_next(M)) != TM_EOS) {
            if (ch == r) {
                if (--cont == 0) return tmU_next(M), TM_MATCHED;
            } else {
                if (ch == l) ++cont;
            }
        }
    }
    return tmU_restore(M, &save), TM_OK;
}

static int tmM_startcapture(tm_Match *M, tm_Slice *pat, size_t what) {
    int r;
    if (M->level >= TM_MAX_PATTERN_COUNT) return TM_ERRPATTERN;
    M->cap[M->level].start = tmU_offset(M);
    M->cap[M->level].len = what;
    if (M->level += 1, (r = tmM_match(M, *pat)) != TM_MATCHED) M->level -= 1;
    return r;
}

static int tmM_endcapture(tm_Match *M, tm_Slice *pat) {
    int r, l = tmM_toclosecapture(M);
    if (l < 0) return l;
    M->cap[l].len = tmU_offset(M) - M->cap[l].start;
    if ((r = tmM_match(M, *pat)) != TM_MATCHED)
        M->cap[l].len = TM_CAP_UNFINISHED;
    return r;
}

/* clang-format off */
static int tmU_islineend(tm_Match *M)
{ utfint c = tmU_peek(M); return c == TM_EOS || c == '\n'; }
/* clang-format on */

static int tmM_basic(tm_Match *M, tm_Slice *pat, utfint ch) {
    const char *e = pat->e;
    if (ch == '(') {
        int    pos = pat->s + 1 < e && *(pat->s + 1) == ')';
        size_t what = pos ? TM_CAP_POSITION : TM_CAP_UNFINISHED;
        return (pat->s += pos ? 2 : 1), tmM_startcapture(M, pat, what);
    }
    if (ch == ')') return (pat->s += 1), tmM_endcapture(M, pat);
    assert(ch == '$');
    if (pat->s + 1 != e) return tmM_default(M, pat);
    if (!tmU_isboundary(M)) return TM_OK;
    if (M->flags & TM_LINEANCHOR) return tmU_islineend(M) ? TM_MATCHED : TM_OK;
    return tmU_peek(M) == TM_EOS ? TM_MATCHED : TM_OK;
}

static int tmM_frontier(tm_Match *M, tm_Slice *pat) {
    const char *old = pat->s;
    tm_Slice    b;
    utfint      previous, current;
    int         rp, rc;
    if (tmS_empty(*pat) || *pat->s != '[') return TM_ERRPATTERN;
    if (tmM_classend(pat) < 0) return TM_ERRPATTERN;
    previous = tmU_prevcp(M), current = tmU_peek(M);
    if (current == TM_EOS) current = 0;
    b = tm_slice(old, pat->s - old);
    rp = tmM_bracketclass(previous, b);
    assert(rp >= 0);
    rc = tmM_bracketclass(current, b);
    assert(rc >= 0);
    if (!rp && rc) return TM_CONTINUE;
    return (pat->s = old), TM_OK;
}

static int tmM_escaped(tm_Match *M, tm_Slice *pat) {
    const char *prev = pat->s++;
    utfint      ch = 0;
    int         r;
    if (assert(*prev == TM_ESC), !tmS_decode(pat, &ch)) return TM_ERRPATTERN;
    if (ch == 'b')
        return (r = tmM_balance(M, pat)) == TM_MATCHED ? TM_CONTINUE : r;
    if (ch == 'f') return tmM_frontier(M, pat);
    if (ch >= '0' && ch <= '9') return tmU_backref(M, ch);
    return (pat->s = prev), tmM_default(M, pat);
}

static int tmM_maxexpand(tm_Match *M, tm_Slice pat, tm_Slice rest) {
    size_t start = tmU_offset(M);
    int    r;
    while ((r = tmM_single(M, pat)) == TM_MATCHED) tmU_next(M);
    assert(r >= 0);
    while ((r = tmM_match(M, rest)) == 0 && tmU_offset(M) > start) tmU_prev(M);
    return r;
}

static int tmM_minexpand(tm_Match *M, tm_Slice pat, tm_Slice rest) {
    int r;
    while ((r = tmM_match(M, rest)) == TM_OK
           && (r = tmM_single(M, pat)) == TM_MATCHED)
        tmU_next(M);
    return r;
}

static int tmM_suffix(tm_Match *M, tm_Slice b, const tm_Save *sv, tm_Slice *p) {
    tm_Slice rest;
    int      r;
    rest.s = b.e + 1, rest.e = p->e;
    if (*b.e == '?') {
        tmOK(r = tmM_match(M, rest));
        if (r == TM_MATCHED) return TM_MATCHED;
        return tmU_restore(M, sv), (p->s = b.e + 1), TM_CONTINUE;
    }
    if (*b.e == '+') return tmM_maxexpand(M, b, rest);
    if (*b.e == '*') return tmU_restore(M, sv), tmM_maxexpand(M, b, rest);
    return assert(*b.e == '-'), tmU_restore(M, sv), tmM_minexpand(M, b, rest);
}

static int tmM_default(tm_Match *M, tm_Slice *pat) {
    const char *old = pat->s;
    tm_Slice    b;
    tm_Save     save;
    int         r;
    tmOK(r = tmM_classend(pat));
    b.s = old, b.e = pat->s;
    r = tmM_single(M, b);
    assert(r >= 0);
    if (!r) {
        if (pat->s != pat->e
            && (*pat->s == '*' || *pat->s == '?' || *pat->s == '-'))
            return (pat->s += 1), TM_CONTINUE;
        return (pat->s = old), TM_OK;
    }
    save = tmU_save(M), (void)tmU_next(M);
    if (pat->s == pat->e) return TM_MATCHED;
    if (*pat->s != '?' && *pat->s != '*' && *pat->s != '+' && *pat->s != '-')
        return TM_CONTINUE;
    return tmM_suffix(M, b, &save, pat);
}

static int tmM_match(tm_Match *M, tm_Slice pat) {
    tm_Save  save = tmU_save(M);
    tm_Slice cur;
    int      r;
    utfint   ch;
    if (M->depth <= 0) return TM_ERRCOMPLEX;
    M->depth -= 1;
    if (pat.s == M->pat.s && pat.s < pat.e && *pat.s == '^') {
        if (!tmU_isboundary(M)
            || ((M->flags & TM_LINEANCHOR) && !tmU_islinestart(M)))
            return (M->depth += 1), tmU_restore(M, &save), TM_OK;
        if (pat.s += 1, tmS_empty(pat)) return (M->depth += 1), TM_MATCHED;
    }
    while (!tmS_empty(pat)) {
        cur = pat, (void)tmS_decode(&cur, &ch);
        switch (ch) { /* clang-format off */
        case '(': case ')': case '$': r = tmM_basic(M, &pat, ch); break;
        case TM_ESC: r = tmM_escaped(M, &pat); break;
        default: r = tmM_default(M, &pat); break;
        } /* clang-format on */
        if (r <= 0) return M->depth++, tmU_restore(M, &save), r;
        if (r != TM_CONTINUE) break;
    }
    return M->depth++, TM_MATCHED;
}

/* === match layer end === */

/* --- public API -------------------------------------------------------- */

TM_API int tm_pattern(tm_State *S, tm_Slice pattern) {
    if (!S || !pattern.s || pattern.s > pattern.e) return TM_ERRPARAM;
    return (S->m.pat = pattern), TM_OK;
}

TM_API int tm_seek(tm_State *S, size_t off) {
    if (!S || !S->reader) return TM_ERRPARAM;
    return tmU_goto(S, off), TM_OK;
}

TM_API int tm_advance(tm_State *S, ptrdiff_t delta) {
    if (!S || !S->reader) return TM_ERRPARAM;
    if (delta < 0 && S->off < (size_t)(-delta)) return TM_ERRPARAM;
    return tmU_goto(S, (size_t)((ptrdiff_t)S->off + delta)), TM_OK;
}

static int tmU_trymatch(tm_State *S) {
    int    r, i;
    size_t restore = S->off;
    S->m.level = 0, S->m.depth = TM_MAXCCALLS, r = tmM_match(&S->m, S->m.pat);
    if (r == TM_MATCHED) {
        for (i = 0; i < S->m.level; ++i)
            if (S->m.cap[i].len == TM_CAP_UNFINISHED) return TM_ERRPATTERN;
        return (S->m.end = S->off, tmU_goto(S, restore), TM_MATCHED);
    }
    return r;
}

TM_API int tm_match(tm_State *S) {
    if (!S || !S->reader || !S->m.pat.s) return TM_ERRPARAM;
    S->m.level = 0;
    if (tmS_empty(S->m.pat)) {
        if (tmU_isboundary(&S->m)) return (S->m.end = S->off), TM_MATCHED;
        return TM_OK;
    }
    if (S->m.flags & TM_LITERAL) {
        if (tmS_equalmem(S, S->off, S->m.pat))
            return (S->m.end = S->off + tmS_len(S->m.pat)), TM_MATCHED;
        return TM_OK;
    }
    return tmU_trymatch(S);
}

TM_API int tm_find(tm_State *S) {
    size_t start;
    int    r;
    if (!S || !S->reader || !S->m.pat.s) return TM_ERRPARAM;
    S->m.level = 0;
    if (tmS_empty(S->m.pat)) {
        if (tmU_isboundary(&S->m)) return (S->m.end = S->off), TM_MATCHED;
        return TM_OK;
    }
    if (S->m.flags & TM_LITERAL) {
        size_t s = tmS_find(S, S->off, S->m.pat);
        if (s == TM_NOMATCH) return TM_OK;
        return (S->m.end = s + tmS_len(S->m.pat)), tmU_goto(S, s), TM_MATCHED;
    }
    start = S->off;
    if (!(S->m.flags & TM_LINEANCHOR) && *S->m.pat.s == '^')
        return tmU_trymatch(S);
    for (;;) {
        if ((r = tmU_trymatch(S)) != TM_OK) return r;
        if (tmU_peek(&S->m) == TM_EOS) return tmU_goto(S, start), TM_OK;
        (void)tmU_next(&S->m);
    }
}

TM_API int tm_capture(const tm_State *S, int i, tm_Capture *out) {
    size_t len;
    if (!S || i < 0 || i >= S->m.level) return TM_ERRPARAM;
    len = S->m.cap[i].len;
    if (len == TM_CAP_POSITION) len = 0;
    if (len == TM_CAP_UNFINISHED) return TM_ERRPARAM;
    return (out->start = S->m.cap[i].start, out->len = len), TM_OK;
}

TM_NS_END

#endif /* TM_IMPLEMENTATION && tm_implemented */
