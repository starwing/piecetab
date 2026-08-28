#include <stdio.h>

#if defined(__GNUC__)
# define UD_STATIC static __attribute((unused))
#endif
#define PT_STATIC_API
#include "../lua/unidata.h"
#include "../piecetab.h"

#define PMT_TABSIZE(t) (sizeof(t) / sizeof((t)[0]))

static int tmT_find(range_table *t, size_t size, utfint ch) {
    size_t begin, end;
    begin = 0;
    end = size;
    while (begin < end) {
        size_t mid = (begin + end) / 2;
        if (t[mid].last < ch)
            begin = mid + 1;
        else if (t[mid].first > ch)
            end = mid;
        else
            return (ch - t[mid].first) % t[mid].step == 0;
    }
    return 0;
}

static int pmT_isalpha(utfint ch) {
    return tmT_find(alpha_table, PMT_TABSIZE(alpha_table), ch);
}
static int pmT_iscntrl(utfint ch) {
    return tmT_find(cntrl_table, PMT_TABSIZE(cntrl_table), ch);
}
static int pmT_isdigit(utfint ch) {
    return tmT_find(digit_table, PMT_TABSIZE(digit_table), ch);
}
static int pmT_islower(utfint ch) {
    return tmT_find(lower_table, PMT_TABSIZE(lower_table), ch);
}
static int pmT_ispunct(utfint ch) {
    return tmT_find(punct_table, PMT_TABSIZE(punct_table), ch);
}
static int pmT_isspace(utfint ch) {
    return tmT_find(space_table, PMT_TABSIZE(space_table), ch);
}
static int pmT_iscompose(utfint ch) {
    return tmT_find(compose_table, PMT_TABSIZE(compose_table), ch);
}
static int pmT_isupper(utfint ch) {
    return tmT_find(upper_table, PMT_TABSIZE(upper_table), ch);
}
static int pmT_isxdigit(utfint ch) {
    return tmT_find(xdigit_table, PMT_TABSIZE(xdigit_table), ch);
}
static int pmT_isgraph(utfint ch) {
    if (tmT_find(space_table, PMT_TABSIZE(space_table), ch)) return 0;
    if (tmT_find(graph_table, PMT_TABSIZE(graph_table), ch)) return 1;
    if (tmT_find(compose_table, PMT_TABSIZE(compose_table), ch)) return 1;
    return 0;
}
static int pmT_isalnum(utfint ch) {
    if (tmT_find(alpha_table, PMT_TABSIZE(alpha_table), ch)) return 1;
    if (tmT_find(alnum_extend_table, PMT_TABSIZE(alnum_extend_table), ch))
        return 1;
    return 0;
}

#define TM_IS(cat, c) pmT_is##cat(c)

#define TM_STATIC_API
#include "../textmatch.h"
#include "tests.h"

typedef struct {
    const char *s;
    size_t      len;
} PMText;

typedef struct {
    pt_Buffer b;
    pt_Cursor C;
} PMPiece;

typedef struct {
    const char *a;
    size_t      la;
    const char *b;
    size_t      lb;
} PMSplit;

typedef struct {
    const char *s;
    size_t      len;
    char        cell;
} PMFrag;

TEST_STATIC void   tmT_setflags(tm_State *S, int f) { tm_setflags(S, f); }
TEST_STATIC size_t tmT_offset(tm_State *S) { return tm_offset(S); }
TEST_STATIC size_t tmT_matchend(tm_State *S) { return tm_matchend(S); }
TEST_STATIC int    tmT_captures(tm_State *S) { return tm_captures(S); }

TEST_STATIC tm_Slice tm_read_text(void *ud, size_t *poff) {
    PMText *t = (PMText *)ud;
    if (*poff >= t->len) return tm_slice(NULL, 0);
    *poff = 0;
    return tm_slice(t->s, t->len);
}

TEST_STATIC tm_Slice tm_read_piece(void *ud, size_t *poff) {
    PMPiece    *p = (PMPiece *)ud;
    const char *s;
    size_t      len;
    if (*poff >= pt_bytes(p->b)) return tm_slice(NULL, 0);
    pt_seek(&p->C, p->b, *poff);
    s = pt_piece(&p->C, &len);
    if (!s) return tm_slice(NULL, 0);
    *poff = p->C.off;
    return tm_slice(s - p->C.poff, len + p->C.poff);
}

TEST_STATIC tm_Slice tm_read_split(void *ud, size_t *poff) {
    PMSplit *sp = (PMSplit *)ud;
    if (*poff < sp->la) {
        *poff = 0;
        return tm_slice(sp->a, sp->la);
    }
    if (*poff < sp->la + sp->lb) {
        *poff = sp->la;
        return tm_slice(sp->b, sp->lb);
    }
    return tm_slice(NULL, 0);
}

TEST_STATIC tm_Slice tm_read_frag(void *ud, size_t *poff) {
    PMFrag *f = (PMFrag *)ud;
    if (*poff >= f->len) return tm_slice(NULL, 0);
    f->cell = f->s[*poff];
    return tm_slice(&f->cell, 1);
}

TEST_STATIC int tm_find_text(
        const char *text, const char *pat, size_t from, size_t *st, size_t *en,
        int flags) {
    PMText   t;
    tm_State S;
    int      r;
    t.s = text;
    t.len = strlen(text);
    tm_init(&S, tm_read_text, &t);
    tmT_setflags(&S, flags);
    asserteq(tm_seek(&S, from), TM_OK);
    asserteq(tm_pattern(&S, tm_string(pat)), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        *st = tmT_offset(&S);
        *en = tmT_matchend(&S) - *st;
    }
    return r;
}

TEST_STATIC int tm_find_frag(
        const char *text, const char *pat, size_t from, size_t *st,
        size_t *en) {
    PMFrag   f;
    tm_State S;
    int      r;
    f.s = text;
    f.len = strlen(text);
    tm_init(&S, tm_read_frag, &f);
    tmT_setflags(&S, TM_LITERAL);
    asserteq(tm_seek(&S, from), TM_OK);
    asserteq(tm_pattern(&S, tm_string(pat)), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        *st = tmT_offset(&S);
        *en = tmT_matchend(&S) - *st;
    }
    return r;
}

TEST(find_basic) {
    size_t st, en;
    asserteq(tm_find_text("hello world", "world", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 6);
    asserteq(en, 5);
    asserteq(tm_find_text("hello world", "l+", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 2);
    asserteq(en, 2);
    asserteq(tm_find_text("abc123", "%d+", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 3);
    asserteq(en, 3);
    asserteq(tm_find_text("héllo", "é", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 1);
    asserteq(en, 2);
    asserteq(tm_find_text("abc", "^b", 1, &st, &en, 0), TM_MATCHED);
    asserteq(st, 1);
    asserteq(en, 1);
    asserteq(tm_find_text("abc", "^b", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("abc", "$", 3, &st, &en, 0), TM_MATCHED);
    asserteq(st, 3);
    asserteq(en, 0);
    asserteq(tm_find_text("abc", "$", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 3);
    asserteq(en, 0);
    asserteq(tm_find_text("abc", "$", 4, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("abc", "^", 4, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("abc", "", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 0);
    asserteq(tm_find_text("abc", "", 4, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("abc", "a.c", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 3);
}

TEST(find_lineanchor) {
    size_t st, en;
    asserteq(
            tm_find_text("a\nb\nc", "^b", 0, &st, &en, TM_LINEANCHOR),
            TM_MATCHED);
    asserteq(st, 2);
    asserteq(en, 1);
    asserteq(
            tm_find_text("a\nb\nc", "b$", 0, &st, &en, TM_LINEANCHOR),
            TM_MATCHED);
    asserteq(st, 2);
    asserteq(en, 1);
    asserteq(tm_find_text("abc", "^b", 0, &st, &en, TM_LINEANCHOR), TM_OK);
    asserteq(tm_find_text("abc", "b$", 0, &st, &en, TM_LINEANCHOR), TM_OK);
    asserteq(tm_find_text("a\nb", "^", 2, &st, &en, TM_LINEANCHOR), TM_MATCHED);
    asserteq(st, 2);
    asserteq(en, 0);
}

TEST(find_literal) {
    size_t st, en;
    asserteq(tm_find_text("a.c", ".", 0, &st, &en, TM_LITERAL), TM_MATCHED);
    asserteq(st, 1);
    asserteq(en, 1);
    asserteq(tm_find_text("abc", "%a", 0, &st, &en, TM_LITERAL), TM_OK);
    asserteq(tm_find_text("a.c", "a.c", 0, &st, &en, TM_LITERAL), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 3);
    asserteq(tm_find_text("aab", "ab", 0, &st, &en, TM_LITERAL), TM_MATCHED);
    asserteq(st, 1);
    asserteq(en, 2);
}

TEST(literal_piece_search) {
    PMSplit  sp;
    tm_State S;
    size_t   st, en;
    int      r;
    sp.a = "x";
    sp.la = 1;
    sp.b = "abc";
    sp.lb = 3;
    tm_init(&S, tm_read_split, &sp);
    tmT_setflags(&S, TM_LITERAL);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string("ab")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 1);
    asserteq(en, 2);
}

TEST(captures_backref) {
    PMText     t;
    tm_State   S;
    tm_Capture cap;
    size_t     st, en;
    int        r;
    t.s = "abcabc";
    t.len = strlen(t.s);
    tm_init(&S, tm_read_text, &t);
    asserteq(tm_pattern(&S, tm_string("(abc)%1")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 6);
    asserteq(tmT_captures(&S), 1);
    asserteq(tm_capture(&S, 0, &cap), TM_OK);
    asserteq(cap.start, 0);
    asserteq(cap.len, 3);
    t.s = "abcabd";
    t.len = strlen(t.s);
    tm_init(&S, tm_read_text, &t);
    asserteq(tm_pattern(&S, tm_string("(abc)%1")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_OK);
}

TEST(backref_followup) {
    PMText   t;
    tm_State S;
    size_t   st, en;
    int      r;
    t.s = "abcabcX";
    t.len = strlen(t.s);
    tm_init(&S, tm_read_text, &t);
    asserteq(tm_pattern(&S, tm_string("(abc)%1X")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 7);
}

TEST(captures_position) {
    PMText     t;
    tm_State   S;
    tm_Capture cap;
    size_t     st, en;
    int        r;
    t.s = "abc";
    t.len = strlen(t.s);
    tm_init(&S, tm_read_text, &t);
    asserteq(tm_pattern(&S, tm_string("()b")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 1);
    asserteq(en, 1);
    asserteq(tmT_captures(&S), 1);
    asserteq(tm_capture(&S, 0, &cap), TM_OK);
    asserteq(cap.start, 1);
    asserteq(cap.len, 0);
}

TEST(backref_edges) {
    PMText   t;
    tm_State S;
    size_t   st, en;
    t.s = "abc";
    t.len = strlen(t.s);
    tm_init(&S, tm_read_text, &t);
    tm_seek(&S, 0);
    asserteq(tm_pattern(&S, tm_string("%0")), TM_OK);
    asserteq(tm_find(&S), TM_ERRPATTERN);
    tm_init(&S, tm_read_text, &t);
    tm_seek(&S, 0);
    asserteq(tm_pattern(&S, tm_string("%1")), TM_OK);
    asserteq(tm_find(&S), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "()%1", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 0);
    asserteq(tm_find_text("b", "(a*)%1", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 0);
    tm_init(&S, tm_read_text, &t);
    tm_seek(&S, 0);
    asserteq(tm_find_text("abcab", "(abc)%1", 0, &st, &en, 0), TM_OK);
}

TEST(backref_long_edges) {
    PMSplit  sp;
    tm_State S;
    char     a[80], b[80], pat[80];
    size_t   i;
    int      r;
    for (i = 0; i < 70; ++i) a[i] = 'a', b[i] = 'b';
    a[70] = '\0';
    b[70] = '\0';
    pat[0] = '(';
    for (i = 0; i < 70; ++i) pat[i + 1] = 'a';
    pat[71] = ')', pat[72] = '%', pat[73] = '1', pat[74] = '\0';
    sp.a = a;
    sp.la = 70;
    sp.b = b;
    sp.lb = 70;
    tm_init(&S, tm_read_split, &sp);
    tm_seek(&S, 0);
    tm_pattern(&S, tm_string(pat));
    r = tm_find(&S);
    asserteq(r, TM_OK);
}

TEST(errors) {
    size_t st, en;
    asserteq(tm_find_text("abc", "(", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "abc%", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "[", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "%b", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "%f", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "(%1)", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", ")", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(
            tm_find_text("abc", "()()()()()()()()()()", 0, &st, &en, 0),
            TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "\xff", 0, &st, &en, 0), TM_OK);
}

TEST(malformed_pattern_edges) {
    size_t st, en;
    asserteq(tm_find_text("abc", "[%", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "[abc", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("a", "[b-d]", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("abc", "%b(", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "%fa", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "%f[abc", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("!", "%!", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("abc", "a?[", 0, &st, &en, 0), TM_ERRPATTERN);
    asserteq(tm_find_text("abc", "(a))", 0, &st, &en, 0), TM_ERRPATTERN);
}

TEST(piece_reader) {
    pt_State *S = pt_open(NULL, NULL);
    pt_Buffer b = pt_from(S, "hello world foo", 15);
    pt_Cursor C;
    PMPiece   p;
    tm_State  P;
    size_t    st, en;
    int       r;
    pt_seek(&C, (assert(b), b), 5);
    pt_edit(&C, 0, "XX", 2);
    b = pt_commit(&C);
    p.b = b;
    tm_init(&P, tm_read_piece, &p);
    asserteq(tm_seek(&P, 0), TM_OK);
    asserteq(tm_pattern(&P, tm_string("world")), TM_OK);
    r = tm_find(&P);
    if (r == TM_MATCHED) {
        st = tmT_offset(&P);
        en = tmT_matchend(&P) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 8);
    asserteq(en, 5);
    asserteq(tm_seek(&P, 0), TM_OK);
    asserteq(tm_pattern(&P, tm_string("XX")), TM_OK);
    r = tm_find(&P);
    if (r == TM_MATCHED) {
        st = tmT_offset(&P);
        en = tmT_matchend(&P) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 5);
    asserteq(en, 2);
    pt_release(b);
    pt_close(S);
}

TEST(split_reader) {
    PMSplit  sp;
    tm_State S;
    size_t   st, en;
    int      r;
    sp.a = "\xC3";
    sp.la = 1;
    sp.b = "\xA9";
    sp.lb = 1;
    tm_init(&S, tm_read_split, &sp);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string("\xC3\xA9")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 2);
}

TEST(split_lazy_backtrack) {
    PMSplit  sp;
    tm_State S;
    size_t   st, en;
    int      r;
    sp.a = "a\xC3";
    sp.la = 2;
    sp.b = "\xA9z";
    sp.lb = 2;
    tm_init(&S, tm_read_split, &sp);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string(".-z")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 4);
}

TEST(literal_cross_piece_overlap) {
    PMSplit  sp;
    tm_State S;
    size_t   st, en;
    int      r;
    sp.a = "abc";
    sp.la = 3;
    sp.b = "x";
    sp.lb = 1;
    tm_init(&S, tm_read_split, &sp);
    tmT_setflags(&S, TM_LITERAL);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string("bcx")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 1);
    asserteq(en, 3);
}

TEST(literal_prefix_mismatch) {
    PMSplit  sp;
    tm_State S;
    size_t   st, en;
    int      r;
    sp.a = "abce";
    sp.la = 4;
    sp.b = "f";
    sp.lb = 1;
    tm_init(&S, tm_read_split, &sp);
    tmT_setflags(&S, TM_LITERAL);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string("bcdf")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_OK);
    asserteq(tm_find_text("abcx", "abcd", 0, &st, &en, TM_LITERAL), TM_OK);
}

TEST(fragmented_literal_search) {
    size_t st, en;
    asserteq(tm_find_frag("abc", "abc", 0, &st, &en), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 3);
    asserteq(tm_find_frag("abc", "bc", 0, &st, &en), TM_MATCHED);
    asserteq(st, 1);
    asserteq(en, 2);
    asserteq(tm_find_frag("abcabc", "cab", 0, &st, &en), TM_MATCHED);
    asserteq(st, 2);
    asserteq(en, 3);
    asserteq(tm_find_frag("abc", "x", 0, &st, &en), TM_OK);
    asserteq(tm_find_frag("aaaa", "aa", 0, &st, &en), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 2);
    asserteq(tm_find_frag("abc", "abcd", 0, &st, &en), TM_OK);
}

TEST(literal_search_edges) {
    PMSplit  sp;
    tm_State S;
    char     a[80], pat[80];
    size_t   i;
    int      r;
    for (i = 0; i < 70; ++i) a[i] = i ? 'b' : 'a';
    a[70] = '\0';
    for (i = 0; i < 71; ++i) pat[i] = i == 0 ? 'a' : (i < 70 ? 'b' : 'X');
    pat[71] = '\0';
    sp.a = a;
    sp.la = 70;
    sp.b = "Y";
    sp.lb = 1;
    tm_init(&S, tm_read_split, &sp);
    tmT_setflags(&S, TM_LITERAL);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string(pat)), TM_OK);
    r = tm_find(&S);
    asserteq(r, TM_OK);
    sp.a = "abac";
    sp.la = 4;
    sp.b = "Y";
    sp.lb = 1;
    tm_init(&S, tm_read_split, &sp);
    tmT_setflags(&S, TM_LITERAL);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string("abacX")), TM_OK);
    r = tm_find(&S);
    asserteq(r, TM_OK);
    sp.a = "aababc";
    sp.la = 6;
    sp.b = "Y";
    sp.lb = 1;
    tm_init(&S, tm_read_split, &sp);
    tmT_setflags(&S, TM_LITERAL);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string("aababcX")), TM_OK);
    r = tm_find(&S);
    asserteq(r, TM_OK);
    sp.a = "aaaaaaaaaa";
    sp.la = 10;
    sp.b = "Y";
    sp.lb = 1;
    tm_init(&S, tm_read_split, &sp);
    tmT_setflags(&S, TM_LITERAL);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string("aaaaaaaaaaX")), TM_OK);
    r = tm_find(&S);
    asserteq(r, TM_OK);
}

TEST(classes_all) {
    size_t st, en;
    asserteq(tm_find_text("a", "%a", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("a", "%A", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("1", "%A", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("\x01", "%c", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("1", "%d", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("a", "%l", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("!", "%p", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text(" ", "%s", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("a", "%t", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("A", "%u", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("A", "%x", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("a", "%w", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("a", "%g", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text(" ", "%g", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("\x01", "%g", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text(" ", "%w", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("0", "%w", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("q", "%q", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("a", "%z", 0, &st, &en, 0), TM_OK);
}

TEST(classes_unknown_upper) {
    size_t st, en;
    asserteq(tm_find_text("Q", "%Q", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 1);
    asserteq(tm_find_text("q", "%Q", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("Q", "[%Q]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 1);
    asserteq(tm_find_text("q", "[%Q]", 0, &st, &en, 0), TM_OK);
}

TEST(brackets) {
    size_t st, en;
    asserteq(tm_find_text("abc", "[b]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 1);
    asserteq(tm_find_text("abc", "[^b]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(tm_find_text("abc", "[a-c]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(tm_find_text("d", "[a-c]", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("123", "[%d]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(tm_find_text("a", "[%a]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("a", "[^%a]", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("abc", "[ab]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(tm_find_text("abc", "[ab]", 1, &st, &en, 0), TM_MATCHED);
    asserteq(st, 1);
    asserteq(tm_find_text("-", "[a-]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(tm_find_text("a", "[a-]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(tm_find_text("b", "[a-cb]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(tm_find_text("-", "[-a]", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
}

TEST(suffixes) {
    size_t st, en;
    asserteq(tm_find_text("abc", "a?b", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("abc", "a?x", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("abc", "a*b", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("abc", "a+b", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("abc", "a-b", 0, &st, &en, 0), TM_MATCHED);
    asserteq(tm_find_text("aaaab", "a+b", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 5);
    asserteq(tm_find_text("aaaab", "a-%w", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(tm_find_text("aaab", "a*ab", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 4);
    asserteq(tm_find_text("c", "a*b", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("a", "a*b", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("c", "a-b", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("aac", "a-b", 0, &st, &en, 0), TM_OK);
}

TEST(optional_backtrack) {
    size_t st, en;
    asserteq(tm_find_text("a", "a?a", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 1);
    asserteq(tm_find_text("ab", "a?ab", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 2);
    asserteq(tm_find_text("a", "a*a", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 1);
    asserteq(tm_find_text("ab", "a*ab", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 2);
    asserteq(tm_find_text("ab", "a-ab", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 2);
}

TEST(balance_frontier) {
    size_t st, en;
    asserteq(tm_find_text("()", "%b()", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 2);
    asserteq(tm_find_text("(a(b))", "%b()", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 6);
    asserteq(tm_find_text("abc", "%f[%a]abc", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(tm_find_text("1abc", "%f[%a]abc", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 1);
    asserteq(tm_find_text("abc", "%f[%d]abc", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("(", "%b()", 0, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("(\xff)", "%b()", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 3);
    asserteq(
            tm_find_text(
                    "\xff"
                    "a",
                    "%f[%a]a", 0, &st, &en, 0),
            TM_OK);
    asserteq(tm_find_text("a\xff", "%f[%a]", 1, &st, &en, 0), TM_OK);
    asserteq(tm_find_text("a\xff", "%f[%a]", 3, &st, &en, 0), TM_OK);
}

TEST(anchor_edges) {
    size_t st, en;
    asserteq(tm_find_text("abc", "$", 3, &st, &en, TM_LINEANCHOR), TM_MATCHED);
    asserteq(st, 3);
    asserteq(en, 0);
    asserteq(tm_find_text("a$b", "$b", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 1);
    asserteq(en, 2);
}

TEST(match_api) {
    PMText   t;
    tm_State S;
    size_t   end;
    int      r;
    t.s = "abc";
    t.len = strlen(t.s);
    tm_init(&S, tm_read_text, &t);
    asserteq(tm_pattern(&S, tm_string("b")), TM_OK);
    asserteq(tm_seek(&S, 1), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_MATCHED);
    asserteq(end, 2);
    asserteq(tm_seek(&S, 0), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_OK);
    asserteq(tm_pattern(&S, tm_string("")), TM_OK);
    asserteq(tm_seek(&S, 3), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_MATCHED);
    asserteq(end, 3);
    asserteq(tm_seek(&S, 4), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_OK);
    tmT_setflags(&S, 0);
    asserteq(tm_pattern(&S, tm_string("$")), TM_OK);
    asserteq(tm_seek(&S, 4), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_OK);
    asserteq(tm_pattern(&S, tm_string("^")), TM_OK);
    asserteq(tm_seek(&S, 4), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_OK);
    tmT_setflags(&S, TM_LITERAL);
    asserteq(tm_pattern(&S, tm_string("")), TM_OK);
    asserteq(tm_seek(&S, 3), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_MATCHED);
    asserteq(end, 3);
    asserteq(tm_seek(&S, 4), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_OK);
    asserteq(tm_pattern(&S, tm_string("b")), TM_OK);
    asserteq(tm_seek(&S, 1), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_MATCHED);
    asserteq(end, 2);
    asserteq(tm_seek(&S, 0), TM_OK);
    r = tm_match(&S);
    end = tmT_matchend(&S);
    asserteq(r, TM_OK);
}

TEST(api_params) {
    PMText     t;
    tm_State   S;
    tm_Capture cap;
    int        r;
    t.s = "abc";
    t.len = strlen(t.s);
    tm_init(&S, tm_read_text, &t);
    r = tm_find(&S);
    asserteq(r, TM_ERRPARAM);
    r = tm_match(&S);
    asserteq(r, TM_ERRPARAM);
    r = tm_capture(&S, 0, &cap);
    asserteq(r, TM_ERRPARAM);
    r = tm_advance(&S, -1);
    asserteq(r, TM_ERRPARAM);
    r = tm_advance(&S, 1);
    asserteq(r, TM_OK);
    asserteq(tmT_offset(&S), 1);
    r = tm_pattern(NULL, tm_string("a"));
    asserteq(r, TM_ERRPARAM);
    r = tm_seek(NULL, 0);
    asserteq(r, TM_ERRPARAM);
}

TEST(api_params_more) {
    PMText     t;
    tm_State   S, N;
    tm_Capture cap;
    tm_Slice   bad;
    char       dummy[2];
    int        r;
    t.s = "abc";
    t.len = strlen(t.s);
    tm_init(&S, tm_read_text, &t);
    tm_init(&N, NULL, NULL);
    asserteq(tm_pattern(&S, tm_slice(NULL, 0)), TM_ERRPARAM);
    bad.s = dummy + 1;
    bad.e = dummy;
    asserteq(tm_pattern(&S, bad), TM_ERRPARAM);
    asserteq(tm_seek(&N, 0), TM_ERRPARAM);
    asserteq(tm_advance(NULL, 0), TM_ERRPARAM);
    asserteq(tm_advance(&N, 0), TM_ERRPARAM);
    asserteq(tm_seek(&S, 1), TM_OK);
    asserteq(tm_advance(&S, -1), TM_OK);
    asserteq(tmT_offset(&S), 0);
    asserteq(tm_match(NULL), TM_ERRPARAM);
    asserteq(tm_match(&N), TM_ERRPARAM);
    asserteq(tm_find(NULL), TM_ERRPARAM);
    asserteq(tm_find(&N), TM_ERRPARAM);
    asserteq(tm_capture(NULL, 0, &cap), TM_ERRPARAM);
    asserteq(tm_capture(&S, -1, &cap), TM_ERRPARAM);
    tm_init(&S, tm_read_text, &t);
    tm_seek(&S, 0);
    tm_pattern(&S, tm_string("(a"));
    r = tm_find(&S);
    asserteq(r, TM_ERRPATTERN);
    asserteq(tm_capture(&S, 0, &cap), TM_ERRPARAM);
}

TEST(depth_complex) {
    size_t st, en;
    char   text[301], pat[1024];
    size_t i;
    for (i = 0; i < 300; ++i) text[i] = 'a';
    text[300] = '\0';
    for (i = 0; i < 300; ++i) pat[i * 2] = 'a', pat[i * 2 + 1] = '?';
    pat[600] = '\0';
    asserteq(tm_find_text(text, pat, 0, &st, &en, 0), TM_ERRCOMPLEX);
}

TEST(find_failure_restore) {
    PMText   t;
    tm_State S;
    int      r;
    t.s = "abc";
    t.len = strlen(t.s);
    tm_init(&S, tm_read_text, &t);
    asserteq(tm_seek(&S, 1), TM_OK);
    asserteq(tm_pattern(&S, tm_string("z")), TM_OK);
    r = tm_find(&S);
    asserteq(r, TM_OK);
    asserteq(tmT_offset(&S), 1);
}

TEST(piece_backref) {
    pt_State *S = pt_open(NULL, NULL);
    pt_Buffer b = pt_from(S, "abcabc", 6);
    pt_Cursor C;
    PMPiece   p;
    tm_State  P;
    size_t    st, en;
    int       r;
    pt_seek(&C, (assert(b), b), 3);
    pt_edit(&C, 0, "X", 1);
    b = pt_commit(&C);
    p.b = b;
    tm_init(&P, tm_read_piece, &p);
    asserteq(tm_pattern(&P, tm_string("(abc)X%1")), TM_OK);
    r = tm_find(&P);
    if (r == TM_MATCHED) {
        st = tmT_offset(&P);
        en = tmT_matchend(&P) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 7);
    pt_release(b);
    pt_close(S);
}

TEST(fragmented_backref) {
    PMFrag   f;
    tm_State S;
    size_t   st, en;
    int      r;
    f.s = "abcabc";
    f.len = 6;
    tm_init(&S, tm_read_frag, &f);
    asserteq(tm_seek(&S, 0), TM_OK);
    asserteq(tm_pattern(&S, tm_string("(abc)%1")), TM_OK);
    r = tm_find(&S);
    if (r == TM_MATCHED) {
        st = tmT_offset(&S);
        en = tmT_matchend(&S) - st;
    }
    asserteq(r, TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 6);
}

TEST(invalid_bytes) {
    size_t st, en;
    int    r = tm_find_text("\xff", "\xff", 0, &st, &en, 0);
    asserteq(r, TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 1);
    asserteq(
            tm_find_text(
                    "a\x80"
                    "b",
                    "a\x80"
                    "b",
                    0, &st, &en, 0),
            TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 3);
    asserteq(
            tm_find_text(
                    "\xf8\x80\x80\x80\x80\x80", "\xf8\x80\x80\x80\x80\x80", 0,
                    &st, &en, 0),
            TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 6);
    asserteq(tm_find_text("\xf8\x41", "\xf8\x41", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 2);
    asserteq(
            tm_find_text(
                    "\xff\x80\x80\x80\x80\x80\x80\x80",
                    "\xff\x80\x80\x80\x80\x80\x80\x80", 0, &st, &en, 0),
            TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 8);
}

TEST(utf8_edges) {
    size_t st, en;
    asserteq(
            tm_find_text(
                    "\xFC\x80\x80\x80\x80\x80", "\xFC\x80\x80\x80\x80\x80", 0,
                    &st, &en, 0),
            TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 6);
    asserteq(
            tm_find_text(
                    "\xC3"
                    "A",
                    "\xC3"
                    "A",
                    0, &st, &en, 0),
            TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 2);
}

TEST(overlong_decodes) {
    size_t st, en;
    asserteq(tm_find_text("\xC0\x80", "%z", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 2);
    asserteq(tm_find_text("\xE0\x80\x80", "%z", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 3);
    asserteq(
            tm_find_text("\xF0\x80\x80\x80", "%z", 0, &st, &en, 0), TM_MATCHED);
    asserteq(st, 0);
    asserteq(en, 4);
}

TEST(prev_continuation) {
    size_t st, en;
    asserteq(
            tm_find_text("\x80\x80\x80\x80\x80\x80", "%f[%a]", 6, &st, &en, 0),
            TM_OK);
}

TEST(mid_char_boundary) {
    PMText   t;
    tm_State S;
    t.s = "\xC3\xA9";
    t.len = 2;
    tm_init(&S, tm_read_text, &t);
    asserteq(tm_seek(&S, 1), TM_OK);
    asserteq(tm_pattern(&S, tm_string("")), TM_OK);
    asserteq(tm_match(&S), TM_OK);
    asserteq(tm_seek(&S, 1), TM_OK);
    asserteq(tm_pattern(&S, tm_string("^")), TM_OK);
    asserteq(tm_match(&S), TM_OK);
}

TEST(continuation_boundaries) {
    PMText   t;
    tm_State S;
    int      i;
    t.s = "\x80\x80\x80";
    t.len = 3;
    tm_init(&S, tm_read_text, &t);
    for (i = 0; i <= 3; ++i) {
        asserteq(tm_seek(&S, (size_t)i), TM_OK);
        asserteq(tm_pattern(&S, tm_string("")), TM_OK);
        asserteq(tm_match(&S), TM_MATCHED);
    }
}

TEST(capture_reset) {
    PMText   t;
    tm_State S;
    t.s = "abc";
    t.len = 3;
    tm_init(&S, tm_read_text, &t);
    tm_seek(&S, 0);
    tm_pattern(&S, tm_string("(b)"));
    asserteq(tm_find(&S), TM_MATCHED);
    asserteq(tmT_captures(&S), 1);
    tmT_setflags(&S, TM_LITERAL);
    tm_pattern(&S, tm_string("b"));
    tm_seek(&S, 0);
    asserteq(tm_find(&S), TM_MATCHED);
    asserteq(tmT_captures(&S), 0);
    tmT_setflags(&S, 0);
    tm_pattern(&S, tm_string(""));
    tm_seek(&S, 0);
    asserteq(tm_match(&S), TM_MATCHED);
    asserteq(tmT_captures(&S), 0);
}

#include "textmatch_test.gen.inc"
