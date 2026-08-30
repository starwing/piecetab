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

#define TM_NOLIMIT ((size_t)-1)

#define TM_CAP_UNFINISHED ((size_t)-1)
#define TM_CAP_POSITION   ((size_t)-2)

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

/* initialization */
TM_API int tm_reset(tm_State *S, tm_Reader *r, void *ud);

#define tm_flags(S)       ((S) ? (S)->m.flags : 0)
#define tm_setflags(S, f) ((void)((S) && ((S)->m.flags = (f))))

TM_API tm_Slice tm_slice(const char *s, size_t len);
TM_API tm_Slice tm_string(const char *s);

/* seek */
TM_API int tm_seek(tm_State *S, size_t off);

/* matching */
TM_API int tm_match(tm_State *S, tm_Slice pattern);
TM_API int tm_find(tm_State *S, tm_Slice pattern, size_t limit);

#define tm_offset(S)   ((S) ? (S)->off : 0)
#define tm_matchend(S) ((S) ? (S)->m.end : 0)

/* captures */
TM_API int tm_capture(const tm_State *S, int i, tm_Capture *out);

#define tm_captures(S) ((S) ? (S)->m.level : 0)

/* read */
TM_API size_t tm_copy(tm_State *S, size_t off, char *buf, size_t n);

/* structure */

#define TM_UNKNOWN ((utfint)(-2)) /* not decoded yet */
#define TM_EOS     ((utfint)(-1)) /* end of source / before start */

#ifndef TM_MAX_PATTERN_COUNT
# define TM_MAX_PATTERN_COUNT 9
#endif

typedef struct tm_Match {
    size_t end;   /* last successful match end */
    int    depth; /* remaining recursion budget */
    int    level; /* open capture count */
    int    flags; /* TM_LITERAL / TM_LINEANCHOR */

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
    size_t noff; /* offset of current cp+1; valid when current != TM_UNKNOWN */
    utfint current; /* codepoint at pos; TM_UNKNOWN or TM_EOS */
    utfint prev;    /* codepoint before pos; TM_UNKNOWN, 0 = start/NUL */
};

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

#define TM_NOMATCH ((size_t)-1)

#define TM_CONTINUE (2)

#ifndef TM_IS
# define TM_IS(cat, c) is##cat((int)(c))
#include <ctype.h>
#endif /* TM_IS */

#include <assert.h>
#include <string.h>

#define tm_min(a, b) ((a) < (b) ? (a) : (b))

TM_NS_BEGIN

/* clang-format off */
TM_API tm_Slice tm_slice(const char *s, size_t len)
{ tm_Slice p; p.s = s; p.e = len ? s + len : s; return p; }

static size_t tmL_len(tm_Slice s) { return (size_t)(s.e - s.s); }

static int tmR_incache(tm_State *S, size_t off)
{ return S->cache.s && S->base <= off && off < S->base + tmL_len(S->cache); }
/* clang-format on */

TM_API tm_Slice tm_string(const char *s) { return tm_slice(s, strlen(s)); }
static int      tmL_empty(tm_Slice s) { return !s.s || s.s >= s.e; }

static size_t tmU_utflen(utfint c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    if ((c & 0xFC) == 0xF8) return 5;
    if ((c & 0xFE) == 0xFC) return 6;
    return 1;
}

static int tmU_decodelen(tm_Slice *s, size_t need, utfint *val) {
    static const utfint masks[] = {0, 0xFF, 0x1F, 0x0F, 0x07, 0x03, 0x01};
    utfint              cp = *val, old = cp;
    size_t              i;
    for (cp &= masks[need], i = 1; i < need; ++i) {
        utfint cc = tmC(s->s[i]);
        if ((cc & 0xC0) != 0x80) return (*val = old, s->s += 1), 1;
        cp = (cp << 6) | (cc & 0x3F);
    }
    return (*val = cp, s->s += need), (int)need;
}

static int tmU_decode(tm_Slice *s, utfint *val) {
    size_t need;
    utfint cp;
    int    r;
    if (tmL_empty(*s)) return 0;
    *val = (cp = tmC(*s->s));
    r = (need = tmU_utflen(cp)) == 1 || need > tmL_len(*s);
    return r ? (s->s += 1, 1) : tmU_decodelen(s, need, val);
}

/* --- physical source access ------------------------------------------- */

static int tmR_load(tm_State *S, size_t off) {
    size_t   poff = off;
    tm_Slice p;
    if (tmR_incache(S, off)) return 1;
    S->p = NULL, p = S->reader(S->ud, &poff);
    if (!tmL_empty(p)) return (S->base = poff, S->cache = p), 1;
    return (S->base = 0, S->cache = tm_slice(NULL, 0)), 0;
}

static tm_Slice tmR_at(tm_State *S, size_t off, size_t endoff) {
    size_t prefix, plen, len = (assert(endoff >= off), endoff - off);
    if (!tmR_load(S, off) || len == 0) return tm_slice(NULL, 0);
    prefix = off - S->base, plen = tmL_len(S->cache) - prefix;
    return tm_slice(S->cache.s + prefix, tm_min(plen, len));
}

TM_API size_t tm_copy(tm_State *S, size_t off, char *buf, size_t n) {
    size_t avail, take, have = 0;
    if (S == NULL || S->reader == NULL || buf == NULL || n == 0) return 0;
    while (have < n) {
        tm_Slice cur = tmR_at(S, off + have, TM_NOLIMIT);
        if (!cur.s) break;
        avail = tmL_len(cur), take = tm_min(n - have, avail);
        memcpy(buf + have, cur.s, take), have += take;
    }
    return have;
}

/* --- centralized source cursor ---------------------------------------- */

static const char *tmK_setp(tm_State *S) {
    if (!tmR_incache(S, S->off)) return S->p = NULL;
    return S->p = S->cache.s + (S->off - S->base);
}

/* clang-format off */
static void tmK_reset(tm_State *S)
{ S->noff = 0, S->current = TM_UNKNOWN, S->prev = TM_UNKNOWN; tmK_setp(S); }

static size_t tmK_offset(tm_Match *M)
{ tm_State *S = (tm_State *)M; return S->off; }
/* clang-format on */

TM_API int tm_seek(tm_State *S, size_t off) {
    if (S == NULL) return TM_ERRPARAM;
    if (off == S->off) return tmK_reset(S), TM_OK;
    S->off = off, S->noff = 0, S->current = TM_UNKNOWN, S->prev = TM_UNKNOWN;
    if (!tmR_incache(S, off)) S->cache = tm_slice(NULL, 0);
    return tmK_setp(S), TM_OK;
}

static utfint tmK_peek(tm_Match *M) {
    char      buf[TM_UTFMAX];
    tm_State *S = ((tm_State *)M);
    size_t    have, need;
    tm_Slice  cur;
    utfint    ch;
    if (S->current != TM_UNKNOWN) return S->current;
    if (S->p == NULL) {
        if (!tmR_load(S, S->off)) return (S->noff = 0), S->current = TM_EOS;
        (void)tmK_setp(S), assert(S->p != NULL);
    }
    if ((ch = tmC(*S->p)) < 0x80 || (need = tmU_utflen(ch)) == 1)
        return S->noff = S->off + 1, S->current = ch;
    if ((have = (size_t)(S->cache.e - S->p)) >= need) {
        cur = tm_slice(S->p, need);
        S->noff = S->off + tmU_decodelen(&cur, need, &ch);
        return S->current = ch;
    }
    cur = tm_slice(buf, tm_copy(S, S->off, buf, need));
    return (S->noff = S->off + tmU_decode(&cur, &ch)), S->current = ch;
}

static utfint tmK_next(tm_Match *M) {
    tm_State *S = ((tm_State *)M);
    assert(S->current != TM_UNKNOWN);
    assert(S->current != TM_EOS);
    S->prev = S->current, S->current = TM_UNKNOWN;
    S->off = S->noff, S->noff = 0;
    return tmK_setp(S), tmK_peek(M);
}

static utfint tmK_prev(tm_Match *M) {
    char      buf[TM_UTFMAX];
    tm_State *S = ((tm_State *)M);
    size_t l, n = 0, pos = (assert(S->off > 0), S->prev = TM_UNKNOWN, --S->off);
    tm_Slice cur = tm_slice(NULL, 0);
    utfint   ch, val;
    if (S->p && S->p > S->cache.s && (ch = tmC(S->p[-1])) < 0x80)
        return (S->off = pos, S->noff = pos + 1, S->p -= 1), S->current = ch;
    if (S->p) cur = tm_slice(S->cache.s, (size_t)(S->p - S->cache.s));
    if (tmL_empty(cur) && !tmR_load(S, pos))
        return (S->off = pos + 1, S->noff = 0), tmK_setp(S), S->current = 0;
    cur = tm_slice(S->cache.s, pos + 1 - S->base);
    for (val = tmC(cur.e[-1]); n < TM_UTFMAX; --cur.e, ++n) {
        if (tmL_empty(cur) && (!S->base || !tmR_load(S, S->base - 1))) break;
        if (tmL_empty(cur)) cur = tm_slice(S->cache.s, tmL_len(S->cache));
        if (((ch = tmC(cur.e[-1])) & 0xC0) != 0x80) break;
    }
    if (tmL_empty(cur) || n == TM_UTFMAX)
        return (S->off = pos, S->noff = pos + 1), tmK_setp(S), S->current = val;
    S->off = pos - n, cur.s = cur.e - 1, cur.e = S->cache.e, l = tmU_utflen(ch);
    if (l > tmL_len(cur)) cur = tm_slice(buf, tm_copy(S, S->off, buf, l));
    if (S->off + (l = tmU_decode(&cur, &ch)) <= pos)
        return (S->off = pos, S->noff = pos + 1), tmK_setp(S), S->current = val;
    return (S->noff = S->off + l), tmK_setp(S), S->current = ch;
}

static utfint tmK_prevcp(tm_Match *M) {
    tm_State *S = (tm_State *)M;
    size_t    off;
    utfint    p;
    if (S->prev != TM_UNKNOWN) return S->prev;
    if (S->off == 0) return 0;
    off = S->off, (void)off;
    p = tmK_prev(M), assert(S->noff == off);
    return (void)tmK_next(M), p;
}

/* --- cursor save/restore and predicates ------------------------------- */

typedef struct tm_Save {
    size_t off, noff;
    utfint current, prev;
} tm_Save;

static tm_Save tmK_save(tm_Match *M) {
    tm_Save   r;
    tm_State *S = (tm_State *)M;
    r.off = S->off, r.noff = S->noff;
    r.current = S->current, r.prev = S->prev;
    return r;
}

static int tmK_restore(tm_Match *M, const tm_Save *save) {
    tm_State *S = (tm_State *)M;
    S->off = save->off, S->noff = save->noff;
    S->current = save->current, S->prev = save->prev;
    if (!tmR_incache(S, S->off))
        return S->p = NULL, S->cache = tm_slice(NULL, 0), 0;
    return S->p = S->cache.s + (S->off - S->base), 1;
}

/* --- byte-range helpers ----------------------------------------------- */

static int tmR_equalmem(tm_State *S, size_t off, tm_Slice mem) {
    size_t n;
    for (; mem.s < mem.e; off += n, mem.s += n) {
        tm_Slice cur = tmR_at(S, off, TM_NOLIMIT);
        if (!cur.s) return 0;
        if ((n = tmL_len(cur)) > tmL_len(mem)) n = tmL_len(mem);
        if (memcmp(cur.s, mem.s, n) != 0) return 0;
    }
    return 1;
}

static int tmR_equal(tm_State *S, size_t a, size_t b, size_t len) {
    while (len > 0) {
        if (tmR_incache(S, a) && tmR_incache(S, b)) {
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
            size_t r, n = len > TM_CMP_CHUNK ? TM_CMP_CHUNK : len;
            r = tm_copy(S, a, abuf, n), assert(r == n), (void)r;
            if (tm_copy(S, b, bbuf, n) != n) return 0;
            if (memcmp(abuf, bbuf, n) != 0) return 0;
            a += n, b += n, len -= n;
        }
    }
    return 1;
}

static size_t tmR_find(tm_State *S, size_t from, size_t to, tm_Slice pat) {
    char     buf[TM_CMP_CHUNK];
    size_t   r, boff = 0, plen = tmL_len(pat);
    tm_Slice cur, suffix;
    assert(plen > 0), cur = tmR_at(S, from, to);
    while (cur.s) {
        const char *p = memchr(cur.s, *pat.s, tmL_len(cur));
        if (!p) {
            from += tmL_len(cur), cur = tmR_at(S, from, to), boff = 0;
            continue;
        }
        from += (size_t)(p - cur.s) + 1, cur.s = p + 1, r = tmL_len(cur);
        if (memcmp(cur.s, pat.s + 1, tm_min(r, plen - 1)) != 0) continue;
        if (r >= plen - 1) return from - 1;
        suffix = tm_slice(pat.s + r + 1, plen - r - 1);
        if (r < TM_CMP_CHUNK && !boff) memcpy(buf, cur.s, r), boff = from;
        if (tmR_equalmem(S, from + r, suffix)) return from - 1;
        if (r < TM_CMP_CHUNK) cur = tm_slice(buf + (size_t)(from - boff), r);
        if (tmL_empty(cur) || !boff) cur = tmR_at(S, from, to), boff = 0;
    }
    return TM_NOMATCH;
}

static int tmM_backref(tm_Match *M, utfint ch) {
    tm_State *S = (tm_State *)M;
    int       idx;
    size_t    len;
    assert(ch >= '0' && ch <= '9');
    if (ch == '0') return TM_ERRPATTERN;
    idx = (int)(ch - '1');
    if (idx >= M->level || M->cap[idx].len == TM_CAP_UNFINISHED)
        return TM_ERRPATTERN;
    if ((len = M->cap[idx].len) == TM_CAP_POSITION) return TM_CONTINUE;
    if (!tmR_equal(S, M->cap[idx].start, S->off, len)) return TM_OK;
    if (len) tm_seek(S, S->off + len);
    return TM_CONTINUE;
}

static int tmK_islinestart(tm_Match *M) {
    tm_State *S = (tm_State *)M;
    if (S->off == 0 || (M->flags & TM_LINEANCHOR) == 0) return 1;
    return assert(S->prev != TM_UNKNOWN), S->prev == '\n';
}

/* === match layer begin === */

static int tmM_class(utfint c, utfint cl) {
    utfint orig = cl;
    int    r, neg = (cl >= 'A' && cl <= 'Z');
    if (neg) cl += 0x20;
#ifndef iscompose
# define iscompose(c) ((void)(c), 0)
# define TM_ISCOMPOSE_LOCAL
#endif
    switch (cl) {
    case 'a': r = TM_IS(alpha, c); break;
    case 'c': r = TM_IS(cntrl, c); break;
    case 'd': r = TM_IS(digit, c); break;
    case 'g': r = TM_IS(graph, c); break;
    case 'l': r = TM_IS(lower, c); break;
    case 'p': r = TM_IS(punct, c); break;
    case 's': r = TM_IS(space, c); break;
    case 't': r = TM_IS(compose, c); break;
    case 'u': r = TM_IS(upper, c); break;
    case 'w': r = TM_IS(alnum, c); break;
    case 'x': r = TM_IS(xdigit, c); break;
    case 'z': r = (c == 0); break;
    default: return (orig == c);
    }
#ifdef TM_ISCOMPOSE_LOCAL
# undef iscompose
# undef TM_ISCOMPOSE_LOCAL
#endif
    return neg ? !r : (r != 0);
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
    assert(!tmL_empty(*pat));
    (void)tmU_decode(pat, &ch);
    switch (ch) {
    default: return 1;
    case TM_ESC: return assert(!tmL_empty(*pat)), (void)tmU_decode(pat, &v), 1;
    case '[':
        if (pat->s < pat->e && *pat->s == '^') pat->s++;
        for (;;) {
            if (tmL_empty(*pat)) return TM_ERRPATTERN;
            if (*(pat->s++) == TM_ESC && pat->s < pat->e) pat->s++;
            if (tmL_empty(*pat)) return TM_ERRPATTERN;
            if (*pat->s == ']') break;
        }
        return (pat->s += 1), 1;
    }
}

static int tmM_bracketclass(utfint c, tm_Slice pat) {
    int sig = 1;
    assert(*pat.s == '[');
    if (*++pat.s == '^') sig = 0, pat.s++;
    while (pat.s + 1 < pat.e) {
        utfint ch, next;
        assert(!tmL_empty(pat)), (void)tmU_decode(&pat, &ch);
        if (ch == TM_ESC) {
            assert(!tmL_empty(pat)), (void)tmU_decode(&pat, &ch);
            if (tmM_class(c, ch)) return sig;
        } else {
            if (ch == c) return sig;
            if (pat.s + 1 < pat.e && *pat.s == '-') {
                if (pat.s + 2 < pat.e) {
                    pat.s += 1;
                    assert(!tmL_empty(pat)), (void)tmU_decode(&pat, &next);
                    if (ch <= c && c <= next) return sig;
                }
            }
        }
    }
    return !sig;
}

static int tmM_single(tm_Match *M, tm_Slice pat) {
    utfint ch, pch;
    ch = tmK_peek(M);
    if (ch == TM_EOS) return 0;
    assert(!tmL_empty(pat)), (void)tmU_decode(&pat, &pch);
    switch (pch) {
    default: return pch == ch;
    case '.': return 1;
    case '[': return pat.s -= 1, tmM_bracketclass(ch, pat);
    case TM_ESC:
        assert(!tmL_empty(pat)), (void)tmU_decode(&pat, &pch);
        return tmM_class(ch, pch);
    }
}

static int tmM_balance(tm_Match *M, tm_Slice *pat) {
    utfint  ch, l, r;
    int     cont = 1;
    tm_Save save;
    if (!tmU_decode(pat, &l) || !tmU_decode(pat, &r)) return TM_ERRPATTERN;
    if (save = tmK_save(M), (ch = tmK_peek(M)) != TM_EOS && ch == l) {
        while ((ch = tmK_next(M)) != TM_EOS) {
            if (ch == r) {
                if (--cont == 0) return tmK_next(M), TM_MATCHED;
            } else {
                if (ch == l) ++cont;
            }
        }
    }
    return tmK_restore(M, &save), TM_OK;
}

static int tmM_startcapture(tm_Match *M, tm_Slice *pat, size_t what) {
    int r;
    if (M->level >= TM_MAX_PATTERN_COUNT) return TM_ERRPATTERN;
    M->cap[M->level].start = tmK_offset(M);
    M->cap[M->level].len = what;
    if (M->level += 1, (r = tmM_match(M, *pat)) != TM_MATCHED) M->level -= 1;
    return r;
}

static int tmM_endcapture(tm_Match *M, tm_Slice *pat) {
    int r, l = tmM_toclosecapture(M);
    if (l < 0) return l;
    M->cap[l].len = tmK_offset(M) - M->cap[l].start;
    if ((r = tmM_match(M, *pat)) != TM_MATCHED)
        M->cap[l].len = TM_CAP_UNFINISHED;
    return r;
}

static int tmK_islineend(tm_Match *M) {
    utfint c = tmK_peek(M);
    return c == TM_EOS || ((M->flags & TM_LINEANCHOR) && c == '\n');
}

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
    return tmK_islineend(M) ? TM_MATCHED : TM_OK;
}

static int tmM_frontier(tm_Match *M, tm_Slice *pat) {
    const char *old = pat->s;
    tm_Slice    b;
    utfint      previous, current;
    int         rp, rc;
    if (tmL_empty(*pat) || *pat->s != '[') return TM_ERRPATTERN;
    if (tmM_classend(pat) < 0) return TM_ERRPATTERN;
    previous = tmK_prevcp(M), current = tmK_peek(M);
    if (current == TM_EOS) current = 0;
    b = tm_slice(old, pat->s - old);
    rp = tmM_bracketclass(previous, b), assert(rp >= 0);
    rc = tmM_bracketclass(current, b), assert(rc >= 0);
    if (!rp && rc) return TM_CONTINUE;
    return (pat->s = old), TM_OK;
}

static int tmM_escaped(tm_Match *M, tm_Slice *pat) {
    const char *prev = pat->s++;
    utfint      ch = 0;
    int         r;
    if (assert(*prev == TM_ESC), !tmU_decode(pat, &ch)) return TM_ERRPATTERN;
    if (ch == 'b')
        return (r = tmM_balance(M, pat)) == TM_MATCHED ? TM_CONTINUE : r;
    if (ch == 'f') return tmM_frontier(M, pat);
    if (ch >= '0' && ch <= '9') return tmM_backref(M, ch);
    return (pat->s = prev), tmM_default(M, pat);
}

static int tmM_maxexpand(tm_Match *M, tm_Slice pat, tm_Slice rest) {
    size_t start = tmK_offset(M);
    int    r;
    while ((r = tmM_single(M, pat)) == TM_MATCHED) tmK_next(M);
    assert(r >= 0);
    while ((r = tmM_match(M, rest)) == 0 && tmK_offset(M) > start) tmK_prev(M);
    return r;
}

static int tmM_minexpand(tm_Match *M, tm_Slice pat, tm_Slice rest) {
    int r;
    while ((r = tmM_match(M, rest)) == TM_OK
           && (r = tmM_single(M, pat)) == TM_MATCHED)
        tmK_next(M);
    return r;
}

static int tmM_default(tm_Match *M, tm_Slice *pat) {
    const char *old = pat->s;
    tm_Slice    h, t;
    tm_Save     save;
    int         r;
    tmOK(r = tmM_classend(pat));
    h.s = old, h.e = pat->s, t.s = pat->s + 1, t.e = pat->e;
    if (r = tmM_single(M, h), assert(r >= 0), r == TM_OK) {
        if (!tmL_empty(*pat)
            && (*pat->s == '*' || *pat->s == '?' || *pat->s == '-'))
            return (pat->s += 1), TM_CONTINUE;
        return (pat->s = old), TM_OK;
    }
    save = tmK_save(M), (void)tmK_next(M);
    if (tmL_empty(*pat)) return TM_MATCHED;
    if (*pat->s == '?') {
        tmOK(r = tmM_match(M, t));
        if (r == TM_MATCHED) return TM_MATCHED;
        return tmK_restore(M, &save), (pat->s += 1), TM_CONTINUE;
    }
    if (*pat->s == '+') return tmM_maxexpand(M, h, t);
    if (*pat->s == '*') return tmK_restore(M, &save), tmM_maxexpand(M, h, t);
    if (*pat->s == '-') return tmK_restore(M, &save), tmM_minexpand(M, h, t);
    return TM_CONTINUE;
}

static int tmM_match(tm_Match *M, tm_Slice pat) {
    tm_Save  save = tmK_save(M);
    tm_Slice cur;
    int      r;
    utfint   ch;
    if (M->depth <= 0) return TM_ERRCOMPLEX;
    M->depth -= 1;
    while (!tmL_empty(pat)) {
        cur = pat, (void)tmU_decode(&cur, &ch);
        switch (ch) { /* clang-format off */
        case '(': case ')': case '$': r = tmM_basic(M, &pat, ch); break;
        case TM_ESC: r = tmM_escaped(M, &pat); break;
        default: r = tmM_default(M, &pat); break;
        } /* clang-format on */
        if (r <= 0) return M->depth++, tmK_restore(M, &save), r;
        if (r != TM_CONTINUE) break;
    }
    return M->depth++, TM_MATCHED;
}

/* === match layer end === */

/* --- public API -------------------------------------------------------- */

TM_API int tm_reset(tm_State *S, tm_Reader *r, void *ud) {
    if (S == NULL || r == NULL) return TM_ERRPARAM;
    memset(S, 0, sizeof(*S)), S->reader = r, S->ud = ud;
    return (S->current = TM_UNKNOWN, S->prev = TM_UNKNOWN), TM_OK;
}

static int tmK_isboundary(tm_Match *M) {
    tm_State *S = (tm_State *)M;
    tm_Save   save;
    if (S->off == 0 || S->prev != TM_UNKNOWN) return 1;
    save = tmK_save(M), (void)tmK_prev(M);
    if (S->noff == save.off) return (void)tmK_next(M), 1;
    return tmK_restore(M, &save), 0;
}

static int tmM_trymatch(tm_State *S, tm_Slice pat) {
    int    r, i;
    size_t restore = S->off;
    assert(S->m.level == 0), assert(tmK_isboundary(&S->m));
    if (tmL_empty(pat)) return (S->m.end = S->off), TM_MATCHED;
    S->m.depth = TM_MAXCCALLS;
    if (*pat.s == '^') {
        if (!tmK_islinestart(&S->m)) return TM_OK;
        pat.s++;
    }
    if ((r = tmM_match(&S->m, pat)) == TM_MATCHED) {
        for (i = 0; i < S->m.level; ++i)
            if (S->m.cap[i].len == TM_CAP_UNFINISHED) return TM_ERRPATTERN;
        return (S->m.end = S->off, tm_seek(S, restore), TM_MATCHED);
    }
    return r;
}

#define tmK_hasdata(M) (tmK_peek(M) != TM_EOS || tmK_isboundary(M))

TM_API int tm_match(tm_State *S, tm_Slice p) {
    if (!S || !p.s) return TM_ERRPARAM;
    S->m.level = 0;
    if (S->m.flags & TM_LITERAL) {
        int r = tmL_empty(p) ? tmK_hasdata(&S->m) : tmR_equalmem(S, S->off, p);
        return r ? S->m.end = S->off + tmL_len(p), TM_MATCHED : TM_OK;
    }
    return tmK_isboundary(&S->m) ? tmM_trymatch(S, p) : TM_OK;
}

TM_API int tm_find(tm_State *S, tm_Slice p, size_t endoff) {
    size_t s;
    int    r;
    if (!S || !p.s) return TM_ERRPARAM;
    S->m.level = 0, s = S->off;
    if (S->off >= endoff) return TM_OK;
    if (S->m.flags & TM_LITERAL) {
        r = tmL_empty(p) ? tmK_hasdata(&S->m)
                         : (s = tmR_find(S, s, endoff, p)) != TM_NOMATCH;
        return r ? tm_seek(S, s), S->m.end = s + tmL_len(p), TM_MATCHED : TM_OK;
    }
    if (!tmK_isboundary(&S->m)) {
        if (tmK_peek(&S->m) == TM_EOS) return TM_OK;
        (void)tmK_prev(&S->m), (void)tmK_next(&S->m);
    }
    if (!(S->m.flags & TM_LINEANCHOR) && *p.s == '^') return tmM_trymatch(S, p);
    for (; S->off < endoff; tmK_next(&S->m)) {
        if ((r = tmM_trymatch(S, p)) != TM_OK) return r;
        if (tmK_peek(&S->m) == TM_EOS) break;
    }
    return tm_seek(S, s), TM_OK;
}

TM_API int tm_capture(const tm_State *S, int i, tm_Capture *out) {
    if (!S || !out || i < 0 || i >= S->m.level) return TM_ERRPARAM;
    *out = S->m.cap[i];
    return (out->len == TM_CAP_UNFINISHED) ? TM_ERRPARAM : TM_OK;
}

TM_NS_END

#endif /* TM_IMPLEMENTATION && tm_implemented */
