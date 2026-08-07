#define TF_IMPLEMENTATION
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ─── test framework (inlined, no shared header) ─── */

#define tf_log(...) fprintf(stderr, __VA_ARGS__)

#define tf_assert(e)                                            \
    do {                                                        \
        if (!(e)) {                                             \
            tf_log("FAIL %s:%d: %s\n", __FILE__, __LINE__, #e); \
            abort();                                            \
        }                                                       \
    } while (0)

#define tf_asserteq(a, b)                                             \
    do {                                                              \
        long _a = (long)(a), _b = (long)(b);                          \
        if (_a != _b) {                                               \
            tf_log("FAIL %s:%d: %s == %ld, expected %ld\n", __FILE__, \
                   __LINE__, #a, _a, _b);                             \
            abort();                                                  \
        }                                                             \
    } while (0)

#define tf_assertstreq(a, b)                                              \
    do {                                                                  \
        const char *_sa = (a), *_sb = (b);                                \
        if (strcmp(_sa, _sb) != 0) {                                      \
            tf_log("FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, _sa, \
                   _sb);                                                  \
            abort();                                                      \
        }                                                                 \
    } while (0)

#include "termfeed.h"

/* ─── allocators ─── */

static void *test_alloc(void *ud, void *p, size_t osize, size_t nsize) {
    (void)ud, (void)osize;
    if (nsize == 0) {
        free(p);
        return NULL;
    }
    p = realloc(p, nsize);
    if (!p) abort();
    return p;
}

static void *oom_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    int *cnt = (int *)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    if (!cnt || *cnt <= 0) return NULL;
    (*cnt)--;
    return realloc(ptr, nsize);
}

/* live-byte counter: net allocated bytes must return to 0 after tf_free */
static void *count_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    size_t *live = (size_t *)ud;
    if (nsize == 0) {
        free(ptr);
        *live -= osize;
        return NULL;
    }
    *live += nsize - osize;
    return realloc(ptr, nsize);
}

/* fills newly-grown tail with 0xA5 so uninitialized bytes are detectable */
static void *fill_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    ptr = realloc(ptr, nsize);
    if (!ptr) abort();
    if (nsize > osize) memset((char *)ptr + osize, 0xA5, nsize - osize);
    return ptr;
}

/* ─── mock reader ─── */

typedef struct {
    const char *data;
    size_t      len;
    int         called;
} MockReader;

static const char *mock_reader(void *ud, size_t *plen) {
    MockReader *mr = (MockReader *)ud;
    const char *d;
    mr->called++;
    d = mr->data, *plen = mr->len;
    mr->data = NULL, mr->len = 0;
    return d;
}

/* feed_byte: feed a single byte and call tf_readkey once */
static int feed_byte(tf_State *S, tf_Key *key, char b) {
    MockReader mr;
    mr.data = &b, mr.len = 1, mr.called = 0;
    tf_feed(S, mock_reader, &mr);
    return tf_readkey(S, key);
}

/* feed_seq: feed a sequence of bytes in one chunk, call tf_readkey */
static int feed_seq(tf_State *S, tf_Key *key, const char *seq, size_t len) {
    MockReader mr;
    int        r;
    mr.data = seq, mr.len = len, mr.called = 0;
    tf_feed(S, mock_reader, &mr);
    r = tf_readkey(S, key);
    S->reader_ud = NULL; /* fix VSCode C/C++ warning */
    return r;
}

/* ─── test runner ─── */

typedef struct {
    const char *name;
    void (*fn)(void);
} tf_test_entry;

static int tf_test_main(
        const char *banner, const tf_test_entry *entries, int argc,
        char *argv[]) {
    int i, j;
    fprintf(stderr, "=== %s ===\n", banner);
    if (argc == 1) {
        const tf_test_entry *e = entries;
        while (e->name) {
            fprintf(stderr, "- %s\n", e->name);
            e->fn();
            fprintf(stderr, "  %s OK\n", e->name);
            ++e;
        }
        fprintf(stderr, "\nAll tests passed!\n");
        return 0;
    }
    for (i = 1; i < argc; ++i) {
        const char *name = argv[i];
        size_t      len = strlen(name);
        int         found = 0, only = *name == '@';
        if (only) name++, len--;
        for (j = 0; entries[j].name; ++j) {
            if (strlen(entries[j].name) >= len
                && strncmp(name, entries[j].name, len) == 0) {
                fprintf(stderr, "- %s\n", entries[j].name);
                entries[j].fn();
                fprintf(stderr, "  %s OK\n", entries[j].name);
                found = 1;
                if (only) break;
            }
        }
        if (!found) {
            fprintf(stderr, "Unknown test: %s\n", name);
            return 1;
        }
    }
    return 0;
}

/* ─── Phase 0: lifecycle ─── */

static void test_lifecycle(void) {
    tf_State S;
    tf_init(&S, NULL, NULL);
    tf_assert(S.state == TF_STATE_IDLE);
    tf_assert(S.flags == 0);
    tf_assert(S.pending_mod == 0);
    tf_assert(S.buf_len == 0);
    tf_free(&S);

    /* default allocator: alloc=NULL → uses realloc internally */
    tf_init(&S, NULL, NULL);
    tf_free(&S);

    /* null checks */
    tf_init(NULL, NULL, NULL);
    tf_free(NULL);
}

static void test_flags(void) {
    tf_State S;
    tf_init(&S, NULL, NULL);
    /* set (overwrite) returns previous flags */
    tf_assert(tf_setflag(&S, TF_FLAG_KEEPC0) == 0);
    tf_assert(tf_setflag(&S, TF_FLAG_KEEPC0 | TF_FLAG_DELBS) == TF_FLAG_KEEPC0);
    tf_assert(
            tf_setflag(&S, TF_FLAG_CONVERTKP)
            == (TF_FLAG_KEEPC0 | TF_FLAG_DELBS));
    /* clear one bit: read-modify-write via returned value */
    tf_setflag(&S, TF_FLAG_DELBS | TF_FLAG_CONVERTKP);
    tf_assert(
            tf_setflag(&S, TF_FLAG_DELBS)
            == (TF_FLAG_DELBS | TF_FLAG_CONVERTKP));
    /* clear all */
    tf_assert(tf_setflag(&S, 0) == TF_FLAG_DELBS);

    /* null checks */
    tf_assert(tf_setflag(NULL, 0) == 0);
    tf_assert(tf_setflag(NULL, TF_FLAG_DELBS) == 0);

    tf_free(&S);
}

/* ─── Phase 1: params ─── */

static void test_feed_params(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* feed null */
    tf_feed(NULL, mock_reader, NULL);
    tf_feed(&S, NULL, NULL);

    /* readkey with no reader */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_NONE);

    /* null params */
    tf_assert(tf_readkey(NULL, &key) == TF_ERRPARAM);
    tf_assert(tf_readkey(&S, NULL) == TF_ERRPARAM);
    tf_assert(tf_flush(NULL, &key) == TF_ERRPARAM);
    tf_assert(tf_flush(&S, NULL) == TF_ERRPARAM);

    /* feed always discards the old chunk (even mid-replay) */
    S.replay = 1;
    S.buf[TF_MAX_BUFLEN - 1] = 'X';
    S.buf_len = 0;
    S.p = "chunk";
    S.n = 5;
    tf_feed(&S, NULL, 0);
    tf_asserteq(S.n, 0);
    tf_assert(S.p == NULL);

    tf_free(&S);
}

static void test_flush_params(void) {
    tf_State S;
    tf_Key   key;

    tf_init(&S, NULL, NULL);

    /* flush IDLE → empty key */
    tf_asserteq(tf_flush(&S, &key), TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_NONE);

    tf_free(&S);
}

/* ─── Phase 1: IDLE state ─── */

static void test_idle_printable(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    r = feed_byte(&S, &key, 'a');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.event, TF_EVENT_PRESS);
    tf_asserteq(key.modifiers, 0);
    tf_assertstreq(key.utf8, "a");

    r = feed_byte(&S, &key, 'Z');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'Z');
    tf_assertstreq(key.utf8, "Z");

    r = feed_byte(&S, &key, '0');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, '0');

    r = feed_byte(&S, &key, ' ');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, ' ');
    tf_asserteq(key.type, TF_TYPE_UNICODE);

    r = feed_byte(&S, &key, '~');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, '~');

    tf_free(&S);
}

static void test_idle_c0_default(void) {
    tf_State S;
    tf_Key   key;
    int      r, i;

    tf_init(&S, NULL, NULL);

    /* 0x00 → Ctrl+Space (KEYSYM SPACE + CTRL) */
    r = feed_byte(&S, &key, 0x00);
    tf_asserteq(r, TF_OK);
    if (key.type != TF_TYPE_KEYSYM) {
        tf_log("RAW: key.type=%d\n", (int)key.type);
        abort();
    }
    tf_asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x09 → TAB */
    r = feed_byte(&S, &key, 0x09);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    tf_asserteq(key.modifiers, 0);

    /* 0x0d → ENTER */
    r = feed_byte(&S, &key, 0x0d);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ENTER);
    tf_asserteq(key.modifiers, 0);

    /* 0x1b → ESCAPE state (no key output, returns TF_AGAIN) */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_ESCAPE);

    /* reset */
    tf_init(&S, NULL, NULL);

    /* 0x01-0x1a: Ctrl+A..Z (skip 0x09=TAB, 0x0d=ENTER, tested above) */
    for (i = 1; i <= 0x1a; ++i) {
        int      cp = i + 0x40;
        int      exp = (cp >= 'A' && cp <= 'Z') ? cp + 0x20 : cp;
        char     expstr[2];
        tf_State S2;
        if (i == 0x09 || i == 0x0d) continue;
        tf_init(&S2, NULL, NULL);
        r = feed_byte(&S2, &key, (unsigned char)i);
        tf_asserteq(r, TF_OK);
        tf_asserteq(key.type, TF_TYPE_UNICODE);
        tf_asserteq(key.d.codepoint, exp);
        tf_asserteq(key.modifiers, TF_MOD_CTRL);
        expstr[0] = (char)exp;
        expstr[1] = '\0';
        tf_assertstreq(key.utf8, expstr);
        tf_free(&S2);
    }

    /* 0x1c → Ctrl+\ (UNICODE + CTRL, as-is) */
    r = feed_byte(&S, &key, 0x1c);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '\\');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x1d → Ctrl+] */
    r = feed_byte(&S, &key, 0x1d);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, ']');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x1e → Ctrl+^ */
    r = feed_byte(&S, &key, 0x1e);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, '^');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x1f → Ctrl+_ */
    r = feed_byte(&S, &key, 0x1f);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, '_');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x7f → DEL */
    r = feed_byte(&S, &key, 0x7f);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    tf_free(&S);
}

static void test_idle_keepc0(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    tf_assert(tf_setflag(&S, TF_FLAG_KEEPC0) == 0);

    /* C0 bytes become UNICODE(byte); NUL is always Ctrl-Space (termkey compat)
     */
    r = feed_byte(&S, &key, 0x00);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    r = feed_byte(&S, &key, 0x01);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0x01);
    tf_asserteq(key.modifiers, 0);

    r = feed_byte(&S, &key, 0x08);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0x08);

    r = feed_byte(&S, &key, 0x09);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0x09);
    tf_asserteq(key.type, TF_TYPE_UNICODE); /* not KEYSYM */

    r = feed_byte(&S, &key, 0x0d);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0x0d);
    tf_asserteq(key.type, TF_TYPE_UNICODE); /* not KEYSYM */

    /* 0x1b always goes to ESCAPE state */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_ESCAPE);

    tf_free(&S);
}

static void test_idle_none(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    /* feed empty data → TF_NONE */
    {
        MockReader mr;
        mr.data = NULL, mr.len = 0, mr.called = 0;
        tf_feed(&S, mock_reader, &mr);
        r = tf_readkey(&S, &key);
    }
    tf_asserteq(r, TF_NONE);

    tf_free(&S);
}

/* ─── Phase 1: ESCAPE state ─── */

static void test_escape_basic(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b + 'x' → UNICODE('x') + ALT */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_ESCAPE);
    r = feed_byte(&S, &key, 'x');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* \x1b + 0x01 → Ctrl+'a' + ALT */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x01);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.modifiers, TF_MOD_CTRL | TF_MOD_ALT);

    /* \x1b + 0x09 → TAB + ALT */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x09);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    /* \x1b + 0x0d → ENTER + ALT */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x0d);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ENTER);
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    /* \x1b + 0x7f → DEL + ALT */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x7f);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DELETE);
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

static void test_escape_alt(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b\x1b + 'x' → same as \x1b + 'x' = 'x' + ALT */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_ESCAPE);
    tf_asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_byte(&S, &key, 'x');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.pending_mod, 0);
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

static void test_escape_csi_ss3_cs(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b + '[' → CSI state */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CSI);

    /* \x1b + 'O' → SS3 state */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_SS3);

    /* \x1b + ']' → CS_OSC state */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CS_OSC);

    /* \x1b + 'P' → CS_DCS state */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'P');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CS_DCS);

    /* \x1b + '_' → CS_APC state */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '_');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CS_APC);

    tf_free(&S);
}

/* test: ESCAPE + CSI sequence → state transitions and bytes */
static void test_escape_chunk(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b → ESCAPE state */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_ESCAPE);

    /* '[' → CSI state */
    r = feed_byte(&S, &key, '[');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CSI);

    /* '1' → CSI accumulating (no final yet) */
    r = feed_byte(&S, &key, '1');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CSI);

    /* 'A' → final: dispatch → UP key */
    r = feed_byte(&S, &key, 'A');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* now in IDLE: next byte is plain UNICODE */
    r = feed_byte(&S, &key, 'x');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'x');

    tf_free(&S);
}

/* ─── Phase 1: flush ─── */

static void test_flush_escape(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* enter ESCAPE via \x1b */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);

    /* flush → ESC key */
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);
    tf_asserteq(key.event, TF_EVENT_PRESS);
    tf_asserteq(key.modifiers, 0);
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

static void test_flush_escape_alt(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* \x1b\x1b → ESCAPE with alt_pending */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_ESCAPE);
    tf_asserteq(S.pending_mod, TF_MOD_ALT);

    /* flush → ALT+ESC */
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.state, TF_STATE_IDLE);
    tf_asserteq(S.pending_mod, 0);

    tf_free(&S);
}

static void test_flush_csi_stub(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[ → CSI */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CSI);

    /* flush CSI → ALT+[ (empty buf → straight to IDLE) */
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '[');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* ─── Phase 2: CSI cursor keys ─── */

static void test_csi_cursor(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    r = feed_seq(&S, &key, "\x1b[A", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);

    r = feed_seq(&S, &key, "\x1b[B", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DOWN);

    r = feed_seq(&S, &key, "\x1b[C", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_RIGHT);

    r = feed_seq(&S, &key, "\x1b[D", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_LEFT);

    r = feed_seq(&S, &key, "\x1b[E", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BEGIN);

    r = feed_seq(&S, &key, "\x1b[F", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_END);

    r = feed_seq(&S, &key, "\x1b[H", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    tf_free(&S);
}

/* ─── Phase 2: CSI modifiers ─── */

static void test_csi_modifiers(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1;2A → Shift+UP */
    r = feed_seq(&S, &key, "\x1b[1;2A", 6);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    /* \x1b[1;5A → Ctrl+UP */
    r = feed_seq(&S, &key, "\x1b[1;5A", 6);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    tf_free(&S);
}

/* ─── Phase 2: shift-tab ─── */

static void test_csi_shifttab(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[Z → Shift+TAB (default shift) */
    r = feed_seq(&S, &key, "\x1b[Z", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    /* \x1b[1;5Z → Shift+TAB + Ctrl */
    r = feed_seq(&S, &key, "\x1b[1;5Z", 6);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT | TF_MOD_CTRL);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    tf_free(&S);
}

/* ─── Phase 2: func/editing keys ─── */

static void test_csi_funckey(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[3~ → DELETE */
    r = feed_seq(&S, &key, "\x1b[3~", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    /* \x1b[1~ → FIND */
    r = feed_seq(&S, &key, "\x1b[1~", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_FIND);

    /* \x1b[5~ → PAGEUP */
    r = feed_seq(&S, &key, "\x1b[5~", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_PAGEUP);

    /* \x1b[6~ → PAGEDOWN */
    r = feed_seq(&S, &key, "\x1b[6~", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_PAGEDOWN);

    /* \x1b[7~ → HOME */
    r = feed_seq(&S, &key, "\x1b[7~", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    /* \x1b[8~ → END */
    r = feed_seq(&S, &key, "\x1b[8~", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_END);

    /* \x1b[11~ → F1 */
    r = feed_seq(&S, &key, "\x1b[11~", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 1);

    /* \x1b[12~ → F2 */
    r = feed_seq(&S, &key, "\x1b[12~", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 2);

    /* \x1b[15~ → F5 */
    r = feed_seq(&S, &key, "\x1b[15~", 5);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 5);

    /* \x1b[17~ → F6 */
    r = feed_seq(&S, &key, "\x1b[17~", 5);
    tf_asserteq(key.d.number, 6);

    /* \x1b[18~ → F7 */
    r = feed_seq(&S, &key, "\x1b[18~", 5);
    tf_asserteq(key.d.number, 7);

    /* \x1b[19~ → F8 */
    r = feed_seq(&S, &key, "\x1b[19~", 5);
    tf_asserteq(key.d.number, 8);

    /* \x1b[20~ → F9 */
    r = feed_seq(&S, &key, "\x1b[20~", 5);
    tf_asserteq(key.d.number, 9);

    /* \x1b[21~ → F10 */
    r = feed_seq(&S, &key, "\x1b[21~", 5);
    tf_asserteq(key.d.number, 10);

    /* \x1b[23~ → F11 */
    r = feed_seq(&S, &key, "\x1b[23~", 5);
    tf_asserteq(key.d.number, 11);

    /* \x1b[24~ → F12 */
    r = feed_seq(&S, &key, "\x1b[24~", 5);
    tf_asserteq(key.d.number, 12);

    /* \x1b[28~ → F15 */
    r = feed_seq(&S, &key, "\x1b[28~", 5);
    tf_asserteq(key.d.number, 15);

    /* \x1b[29~ → F16 */
    r = feed_seq(&S, &key, "\x1b[29~", 5);
    tf_asserteq(key.d.number, 16);

    /* \x1b[32~ → F18 */
    r = feed_seq(&S, &key, "\x1b[32~", 5);
    tf_asserteq(key.d.number, 18);

    /* \x1b[34~ → F20 */
    r = feed_seq(&S, &key, "\x1b[34~", 5);
    tf_asserteq(key.d.number, 20);

    /* \x1b[27;5;97~ → Ctrl+'a' (modifyOtherKeys) */
    r = feed_seq(&S, &key, "\x1b[27;5;97~", 10);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* \x1b[27;99~ → modifyOtherKeys with invalid code → UNKNOWN_CSI */
    r = feed_seq(&S, &key, "\x1b[27;99~", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    /* \x1b[42~ → FUNCTION 42 (default) */
    r = feed_seq(&S, &key, "\x1b[42~", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 42);

    /* remaining fkeynum mappings */
    r = feed_seq(&S, &key, "\x1b[13~", 5);
    tf_asserteq(key.d.number, 3);

    r = feed_seq(&S, &key, "\x1b[14~", 5);
    tf_asserteq(key.d.number, 4);

    r = feed_seq(&S, &key, "\x1b[25~", 5);
    tf_asserteq(key.d.number, 13);

    r = feed_seq(&S, &key, "\x1b[26~", 5);
    tf_asserteq(key.d.number, 14);

    r = feed_seq(&S, &key, "\x1b[31~", 5);
    tf_asserteq(key.d.number, 17);

    r = feed_seq(&S, &key, "\x1b[33~", 5);
    tf_asserteq(key.d.number, 19);

    /* \x1b[0~ → FUNCTION 0 (below FIND range) */
    r = feed_seq(&S, &key, "\x1b[0~", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 0);

    /* \x1b[~ → UNKNOWN_CSI (empty function key arg) */
    r = feed_seq(&S, &key, "\x1b[~", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    tf_free(&S);
}

/* ─── Phase 2: event sub-parameters ─── */

static void test_csi_event(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1;2:3~ → DELETE + Shift + RELEASE */
    r = feed_seq(&S, &key, "\x1b[1;2:3~", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_FIND);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);
    tf_asserteq(key.event, TF_EVENT_RELEASE);

    /* \x1b[1;2:2~ → REPEAT */
    r = feed_seq(&S, &key, "\x1b[1;2:2~", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.event, TF_EVENT_REPEAT);

    /* \x1b[1;2:9~ → invalid event → UNKNOWN_CSI */
    r = feed_seq(&S, &key, "\x1b[1;2:9~", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    /* event=1 explicit → PRESS */
    r = feed_seq(&S, &key, "\x1b[1;2:1~", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    /* field leading char in 0x3A-0x40 skipped: \x1b[1;>2~ → SHIFT+FIND */
    r = feed_seq(&S, &key, "\x1b[1;>2~", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_FIND);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);

    tf_free(&S);
}

/* ─── Phase 2: position report ─── */

static void test_csi_position(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[?1;2R → POSITION(1,2) */
    r = feed_seq(&S, &key, "\x1b[?1;2R", 7);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_POSITION);
    tf_asserteq(key.d.pos.line, 1);
    tf_asserteq(key.d.pos.col, 2);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    tf_free(&S);
}

/* ─── Phase 2: kitty report ─── */

static void test_csi_kitty(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[?u → KITTYREPORT (no params) */
    r = feed_seq(&S, &key, "\x1b[?u", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KITTYREPORT);
    tf_asserteq(key.d.number, -1);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    /* \x1b[?1u → KITTYREPORT with flags=1 */
    r = feed_seq(&S, &key, "\x1b[?1u", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KITTYREPORT);
    tf_asserteq(key.d.number, 1);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    tf_free(&S);
}

/* ─── Phase 2: mode report ─── */

static void test_csi_modereport(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1$y → ANSI mode report: mode=1 */
    r = feed_seq(&S, &key, "\x1b[1$y", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MODEREPORT);
    tf_asserteq(key.d.modereport.initial, 0);
    tf_asserteq(key.d.modereport.mode, 1);
    tf_asserteq(key.d.modereport.value, -1);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    /* \x1b[?1;2$y → DEC mode report */
    r = feed_seq(&S, &key, "\x1b[?1;2$y", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MODEREPORT);
    tf_asserteq(key.d.modereport.initial, '?');
    tf_asserteq(key.d.modereport.mode, 1);
    tf_asserteq(key.d.modereport.value, 2);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    tf_free(&S);
}

/* ─── Phase 2: CSI params edge cases ─── */

static void test_csi_params(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[;2A → UP + modifier 2 (empty first field → arg1=-1→dflt 1??) */
    /* Actually empty arg1 means arg1 defaults to -1 for nargs counting,
     * but cursor key handler doesn't check arg1; it uses tfD_event
     * which gets modifiers from arg2 */
    r = feed_seq(&S, &key, "\x1b[;2A", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);

    /* \x1b[A with alt_pending (\x1b\x1b[A) → UP + ALT */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_seq(&S, &key, "[A", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    /* non-digit inside param field: \x1b[1;2<3A → UP + SHIFT
     * (arg2 stops at '<') */
    r = feed_seq(&S, &key, "\x1b[1;2<3A", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);

    /* non-digit in sub-param: \x1b[1;2:3<4A → UP, arg2=2 */
    r = feed_seq(&S, &key, "\x1b[1;2:3<4A", 10);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);

    /* \x1b\x1bOA → ALT+UP (SS3 dispatch merges alt_pending) */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_seq(&S, &key, "OA", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

/* ─── Phase 3: SS3 cursor keys ─── */

static void test_ss3_cursor(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    r = feed_seq(&S, &key, "\x1bOA", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);

    r = feed_seq(&S, &key, "\x1bOB", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DOWN);

    r = feed_seq(&S, &key, "\x1bOC", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_RIGHT);

    r = feed_seq(&S, &key, "\x1bOD", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_LEFT);

    r = feed_seq(&S, &key, "\x1bOH", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    r = feed_seq(&S, &key, "\x1bOF", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_END);

    /* \x1bOE → BEGIN */
    r = feed_seq(&S, &key, "\x1bOE", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BEGIN);

    tf_free(&S);
}

/* ─── Phase 3: SS3 function keys ─── */

static void test_ss3_func(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    r = feed_seq(&S, &key, "\x1bOP", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 1);

    r = feed_seq(&S, &key, "\x1bOQ", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 2);

    r = feed_seq(&S, &key, "\x1bOR", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 3);

    r = feed_seq(&S, &key, "\x1bOS", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 4);

    tf_free(&S);
}

/* ─── Phase 3: SS3 KP keys ─── */

static void test_ss3_kp(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* KPENTER */
    r = feed_seq(&S, &key, "\x1bOM", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPENTER);

    /* KPMULT */
    r = feed_seq(&S, &key, "\x1bOj", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPMULT);

    /* KPPLUS */
    r = feed_seq(&S, &key, "\x1bOk", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPPLUS);

    /* KPMINUS */
    r = feed_seq(&S, &key, "\x1bOm", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPMINUS);

    /* KPDIV */
    r = feed_seq(&S, &key, "\x1bOo", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPDIV);

    /* KPEQUALS */
    r = feed_seq(&S, &key, "\x1bOX", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPEQUALS);

    /* KPCOMMA */
    r = feed_seq(&S, &key, "\x1bOl", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPCOMMA);

    /* KPPERIOD */
    r = feed_seq(&S, &key, "\x1bOn", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPPERIOD);

    /* KP0 */
    r = feed_seq(&S, &key, "\x1bOp", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KP0);

    /* KP9 */
    r = feed_seq(&S, &key, "\x1bOy", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KP9);

    tf_free(&S);
}

/* ─── Phase 3: SS3 CONVERTKP ─── */

static void test_ss3_convertkp(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_CONVERTKP);

    /* KPMULT → '*' */
    r = feed_seq(&S, &key, "\x1bOj", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '*');

    /* KPPLUS → '+' */
    r = feed_seq(&S, &key, "\x1bOk", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '+');

    /* KPMINUS → '-' */
    r = feed_seq(&S, &key, "\x1bOm", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '-');

    /* KPDIV → '/' */
    r = feed_seq(&S, &key, "\x1bOo", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '/');

    /* KPPERIOD → '.' */
    r = feed_seq(&S, &key, "\x1bOn", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '.');

    /* KPCOMMA → ',' */
    r = feed_seq(&S, &key, "\x1bOl", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, ',');

    /* KPEQUALS → '=' */
    r = feed_seq(&S, &key, "\x1bOX", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '=');

    /* KP0 → '0' */
    r = feed_seq(&S, &key, "\x1bOp", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '0');

    /* non-KP final (UP) with CONVERTKP: kpkey short-circuits, KEYSYM */
    r = feed_seq(&S, &key, "\x1bOA", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);

    /* unknown final with CONVERTKP: kpconvert fails → ALT+O peel */
    r = feed_seq(&S, &key, "\x1bOG", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'O');
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    /* consume replayed 'G' */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'G');

    /* KPENTER → still KPENTER (no conversion) */
    r = feed_seq(&S, &key, "\x1bOM", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPENTER);

    tf_free(&S);
}

/* ─── Phase 3: SS3 unknown → REPLAY_BUF ─── */

static void test_ss3_unknown(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1bOG → first key: ALT+O */
    /* feed ['G'] separately because \x1bO is consumed first */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'G');
    /* 'G' is not a valid SS3 final → ALT+O produced, REPLAY_BUF(1) */
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'O');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.replay, 1);

    /* next read → REPLAY_BUF outputs 'G' */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'G');
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* ─── Phase 3: X10 mouse ─── */

static void test_mouse_x10(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[M + 3 raw bytes (button 1, col 2, line 3) */
    r = feed_seq(&S, &key, "\x1b[M\x20\x22\x23", 6);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_asserteq(key.d.mouse.btn, 0);  /* code=0x20 → code-0x20=0, btn=0 */
    tf_asserteq(key.d.mouse.col, 2);  /* 0x22-0x20=2 */
    tf_asserteq(key.d.mouse.line, 3); /* 0x23-0x20=3 */
    tf_asserteq(key.d.mouse.release, 0);

    /* X10 with modifiers: btn=1, shift (btn with mod bit 0x04) */
    /* code = 0x24 → btn=0, mods=(0x24&0x1c)>>2=(0x04)>>2=1=SHIFT */
    r = feed_seq(&S, &key, "\x1b[M\x24\x21\x21", 6);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);
    tf_asserteq(key.d.mouse.col, 1);
    tf_asserteq(key.d.mouse.line, 1);

    tf_free(&S);
}

/* ─── Phase 3: X10 mouse across chunks ─── */

static void test_mouse_x10_chunk(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[M enters MOUSE_X10 (no more bytes in chunk) */
    r = feed_seq(&S, &key, "\x1b[M", 3);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_MOUSE_X10);
    tf_asserteq(S.buf_len, 1); /* buf[0] = 'M', raw to follow */

    /* feed byte 1 → MOUSE_X10: raw at buf[1], still need 2 more */
    r = feed_byte(&S, &key, 0x20);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.buf_len, 2);

    /* feed byte 2 */
    r = feed_byte(&S, &key, 0x21);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.buf_len, 3);

    /* feed byte 3 → complete, decode mouse */
    r = feed_byte(&S, &key, 0x22);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_asserteq(key.d.mouse.col, 1);
    tf_asserteq(key.d.mouse.line, 2);
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* ─── Phase 3: rxvt mouse ─── */

static void test_mouse_rxvt(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[32;5;8M → rxvt: code=32, col=5, line=8 */
    r = feed_seq(&S, &key, "\x1b[32;5;8M", 9);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_asserteq(key.d.mouse.col, 5);
    tf_asserteq(key.d.mouse.line, 8);
    tf_asserteq(key.d.mouse.release, 0);

    tf_free(&S);
}

/* ─── Phase 3: X10 mouse across chunks with alt_pending ─── */

static void test_mouse_x10_chunkalt(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b\x1b → alt_pending=1, then [M in 2-byte chunk → X10 state */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.pending_mod, TF_MOD_ALT);

    /* [M → first byte goes to CSI, 'M' dispatch → X10, S->n<3 → MOUSE_X10 */
    r = feed_seq(&S, &key, "[M", 2);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_MOUSE_X10);

    /* feed X10 bytes one at a time */
    r = feed_byte(&S, &key, 0x20);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x21);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x22);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(key.d.mouse.col, 1);
    tf_asserteq(key.d.mouse.line, 2);
    tf_asserteq(S.state, TF_STATE_IDLE);
    tf_asserteq(S.pending_mod, 0);

    tf_free(&S);
}

/* ─── Phase 3: SGR mouse ─── */

static void test_mouse_sgr(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[<0;5;8M → SGR press: code=0, col=5, line=8 */
    r = feed_seq(&S, &key, "\x1b[<0;5;8M", 9);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_asserteq(key.d.mouse.col, 5);
    tf_asserteq(key.d.mouse.line, 8);
    tf_asserteq(key.d.mouse.release, 0);

    /* \x1b[<0;5;8m → SGR release */
    r = feed_seq(&S, &key, "\x1b[<0;5;8m", 9);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_asserteq(key.d.mouse.release, 1);

    tf_free(&S);
}

/* ─── Phase 3: SS3 unknown prefix byte ─── */

static void test_ss3_unknownprefix(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1bO + '0' (not a final byte) → ALT+O, '0' in REPLAY_BUF */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '0');
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'O');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.replay, 1);
    tf_asserteq(S.buf[TF_MAX_BUFLEN - 1], '0');

    /* replay: '0' */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '0');
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* \x1bO + 0xfe → ALT+O, 0xfe in REPLAY_BUF → 0xFFFD */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0xfe);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'O');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.replay, 1);
    /* replay: high byte → 0xFFFD */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    tf_free(&S);
}

/* ─── Phase 3: X10 mouse with alt_pending ─── */

static void test_mouse_x10_alt(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b\x1b[M\x20\x21\x22 → ALT + MOUSE (via alt_pending) */
    /* feed \x1b\x1b first to set alt_pending */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.pending_mod, TF_MOD_ALT);

    /* feed [M + 3 raw bytes in one 5-byte chunk */
    r = feed_seq(&S, &key, "[M\x20\x21\x22", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(key.d.mouse.col, 1);
    tf_asserteq(key.d.mouse.line, 2);
    tf_asserteq(S.state, TF_STATE_IDLE);
    tf_asserteq(S.pending_mod, 0);

    tf_free(&S);
}

/* ─── Phase 2+3: unknown CSI ─── */

static void test_csi_unknown(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[999@ → UNKNOWN_CSI (final '@' not recognized) */
    r = feed_seq(&S, &key, "\x1b[999@", 7);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    /* plain \x1b[R → F3 */
    r = feed_seq(&S, &key, "\x1b[R", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 3);
    tf_asserteq(key.event, TF_EVENT_PRESS);

    /* control byte in param area short-circuits: \x1b[\x01A → UP */
    r = feed_seq(
            &S, &key,
            "\x1b[\x01"
            "A",
            4);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);

    tf_free(&S);
}

/* ─── Phase 4: Control String ─── */

static void test_cs_osc(void) {
    tf_State S;
    tf_Key   key;
    int      r, len;
    tf_init(&S, NULL, NULL);

    /* OSC \x1b]0;title\a → OSC key, tf_string=="0;title" */
    r = feed_seq(&S, &key, "\x1b]0;title\x07", 11);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_asserteq(key.event, TF_EVENT_PRESS);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 7);
    tf_assert(memcmp(tf_string(&S, &len), "0;title", 7) == 0);
    tf_free(&S);
}

static void test_cs_st(void) {
    tf_State S;
    tf_Key   key;
    int      r, len;
    tf_init(&S, NULL, NULL);

    /* ST (0x9c) termination */
    r = feed_seq(&S, &key, "\x1b]test\x9c", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 4);
    tf_assert(memcmp(tf_string(&S, &len), "test", 4) == 0);
    tf_free(&S);

    /* ST via ESC+\ */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]ab\x1b\x5c", 7);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_assert(tf_string(&S, &len) != NULL);
    /* content "ab": trailing \x1b of ESC+\ removed, '\' not yet buffered */
    tf_asserteq(len, 2);
    tf_assert(memcmp(tf_string(&S, &len), "ab", 2) == 0);

    /* ESC content inside OSC: \x1b]abc\x1b\ → content "abc" */
    /* The \x1b before \ is appended, then ST detection removes it. */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]abc\x1b\x5c", 8);
    tf_asserteq(r, TF_OK);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 3);
    tf_assert(memcmp(tf_string(&S, &len), "abc", 3) == 0);
    tf_free(&S);

    /* content is exactly ESC: \x1b]\x1b\ → empty OSC (cs_len drops to 0) */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]\x1b\x5c", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 0);
    tf_free(&S);
}

static void test_cs_dcs_apc(void) {
    tf_State S;
    tf_Key   key;
    int      r, len;
    tf_init(&S, NULL, NULL);

    /* DCS \x1bP+q...\a */
    r = feed_seq(&S, &key, "\x1bP+qok\x07", 7);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_DCS);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 4);
    tf_assert(memcmp(tf_string(&S, &len), "+qok", 4) == 0);
    tf_free(&S);

    /* APC \x1b_title\a */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b_title\x07", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_APC);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 5);
    tf_assert(memcmp(tf_string(&S, &len), "title", 5) == 0);
    tf_free(&S);
}

static void test_cs_cross(void) {
    tf_State S;
    tf_Key   key;
    int      r, len;
    tf_init(&S, NULL, NULL);

    /* cross-reader: feed ESC+] then AGAIN, then feed content+BEL */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CS_OSC);

    /* feed 'x' → cs_buf alloc, AGAIN */
    r = feed_byte(&S, &key, 'x');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CS_OSC);

    /* feed 'y' → AGAIN */
    r = feed_byte(&S, &key, 'y');
    tf_asserteq(r, TF_AGAIN);

    /* feed BEL → complete */
    r = feed_byte(&S, &key, 0x07);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 2);
    tf_assert(memcmp(tf_string(&S, &len), "xy", 2) == 0);
    tf_free(&S);
}

static void test_cs_alt(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* \x1b\x1b]test\a → OSC + ALT */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_seq(&S, &key, "]test\x07", 7);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_assert(S.pending_mod == 0);
    tf_free(&S);
}

static void test_cs_oom(void) {
    tf_State S;
    tf_Key   key;
    int      r, ec;
    tf_init(&S, test_alloc, NULL);

    /* enter CS: ESC+] */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    tf_asserteq(r, TF_AGAIN);

    /* now use oom_alloc to fail on first byte */
    ec = 0; /* fail immediately */
    tf_init(&S, oom_alloc, &ec);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'x');
    tf_asserteq(r, TF_ERRMEM);
    tf_asserteq(S.state, TF_STATE_IDLE);
    tf_free(&S);
}

static void test_cs_params(void) {
    tf_State S;
    tf_Key   key;
    int      len;
    tf_init(&S, NULL, NULL);

    /* tf_string with NULL/params - no CS key */
    tf_assert(tf_string(&S, &len) == NULL);
    tf_asserteq(len, 0);

    /* plen may be NULL */
    tf_assert(tf_string(&S, NULL) == NULL);
    tf_assert(tf_string(NULL, NULL) == NULL);

    /* plen == NULL with CS content: returns buffer, skips plen */
    tf_assert(tf_string(NULL, &len) == NULL);
    tf_free(&S);

    tf_init(&S, NULL, NULL);
    tf_assert(feed_seq(&S, &key, "\x1b]test\x07", 8) == TF_OK);
    tf_assert(tf_string(&S, NULL) != NULL);
    tf_free(&S);
}

/* ─── Phase 5: UTF-8 decode ─── */

static void test_utf8_decode(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* 2-byte: Ã = 0xC3 0x80 → U+00C0 */
    r = feed_seq(&S, &key, "\xc3\x80", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0xC0);
    tf_asserteq((unsigned char)key.utf8[0], 0xC3);
    tf_asserteq((unsigned char)key.utf8[1], 0x80);
    tf_asserteq(key.utf8[2], '\0');

    /* 3-byte: € = 0xE2 0x82 0xAC → U+20AC */
    r = feed_seq(&S, &key, "\xe2\x82\xac", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0x20AC);
    tf_asserteq((unsigned char)key.utf8[0], 0xE2);
    tf_asserteq((unsigned char)key.utf8[1], 0x82);
    tf_asserteq((unsigned char)key.utf8[2], 0xAC);
    tf_asserteq(key.utf8[3], '\0');

    /* 2-byte: ¡ = 0xC2 0xA1 → U+00A1 */
    r = feed_seq(&S, &key, "\xc2\xa1", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xA1);
    tf_asserteq((unsigned char)key.utf8[0], 0xC2);
    tf_asserteq((unsigned char)key.utf8[1], 0xA1);
    tf_free(&S);
}

static void test_utf8_chunk(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* cross-chunk 2-byte: C3 80 → À */
    r = feed_byte(&S, &key, 0xC3);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_UTF8);
    tf_asserteq(S.buf_len, 1);
    tf_asserteq((unsigned char)S.buf[0], 0xC3);

    r = feed_byte(&S, &key, 0x80);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0xC0);
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* cross-chunk 3-byte: E2 82 AC → € */
    r = feed_byte(&S, &key, 0xE2);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x82);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0xAC);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0x20AC);
    tf_free(&S);
}

static void test_utf8_invalid(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* overlong 2-byte: C0 80 → FFFD */
    r = feed_seq(&S, &key, "\xc0\x80", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0xFFFD);
    tf_asserteq((unsigned char)key.utf8[0], 0xEF);
    tf_asserteq((unsigned char)key.utf8[1], 0xBF);
    tf_asserteq((unsigned char)key.utf8[2], 0xBD);
    tf_asserteq(key.utf8[3], '\0');

    /* overlong 2-byte: C1 BF → FFFD */
    r = feed_seq(&S, &key, "\xc1\xbf", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    /* surrogate: ED A0 80 → FFFD */
    r = feed_seq(&S, &key, "\xed\xa0\x80", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    /* invalid continuation byte: C3 40 → FFFD */
    r = feed_seq(&S, &key, "\xc3\x40", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    /* invalid lead byte: 0xFE → FFFD */
    r = feed_byte(&S, &key, 0xFE);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    /* invalid lead byte: 0xFF → FFFD */
    r = feed_byte(&S, &key, 0xFF);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    /* standalone continuation: 0x80 → FFFD */
    r = feed_byte(&S, &key, 0x80);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    /* 0xFFFE (EF BF BE) → FFFD */
    r = feed_seq(&S, &key, "\xef\xbf\xbe", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    /* 0xFFFF (EF BF BF) → FFFD */
    r = feed_seq(&S, &key, "\xef\xbf\xbf", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    tf_free(&S);
}

static void test_utf8_alt(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* \x1b + UTF-8 lead (one-shot): C3 80 → ALT+À */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_seq(&S, &key, "\xc3\x80", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0xC0);
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* \x1b + UTF-8 lead (cross-chunk): C3 then 80 → ALT+À */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0xC3);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_UTF8);
    tf_asserteq(S.buf_len, 1);
    r = feed_byte(&S, &key, 0x80);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xC0);
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* \x1b\x1b + UTF-8 → ALT+À (alt_pending merge) */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_seq(&S, &key, "\xc3\x80", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xC0);
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.pending_mod, 0);

    /* \x1b\x1b + UTF-8 → ALT+À (cross-chunk with alt_pending=1) */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_byte(&S, &key, 0xC3);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_UTF8);
    r = feed_byte(&S, &key, 0x80);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xC0);
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.pending_mod, 0);

    tf_free(&S);
}

static void test_utf8_params(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* NOINTERPRET: 0x80+ still outputs raw byte */
    tf_setflag(&S, TF_FLAG_KEEPC0);
    r = feed_byte(&S, &key, 0x80);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0x80);

    r = feed_byte(&S, &key, 0xFE);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0xFE);

    /* IDLE: 0x80 → FFFD now (was raw byte before Phase 5) */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x81);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    tf_free(&S);
}

static void test_utf8_456(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* 4-byte: F0 90 8D 88 → U+10348 */
    r = feed_seq(&S, &key, "\xf0\x90\x8d\x88", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0x10348);
    tf_asserteq((unsigned char)key.utf8[0], 0xF0);
    tf_asserteq((unsigned char)key.utf8[3], 0x88);

    /* 5-byte: F8 88 80 80 80 → min valid 5-byte (U+200000) */
    r = feed_seq(&S, &key, "\xf8\x88\x80\x80\x80", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0x200000);
    tf_asserteq((unsigned char)key.utf8[0], 0xF8);

    /* 5-byte cross-chunk */
    r = feed_byte(&S, &key, 0xF8);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x88);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x80);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x80);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x80);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0x200000);

    /* 6-byte: FC 84 80 80 80 80 → min valid 6-byte (U+4000000) */
    r = feed_seq(&S, &key, "\xfc\x84\x80\x80\x80\x80", 6);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 0x4000000);
    tf_asserteq((unsigned char)key.utf8[0], 0xFC);

    tf_free(&S);
}

static void test_cs_expand(void) {
    tf_State S;
    tf_Key   key;
    int      r, len, i;
    char     buf[128];
    tf_init(&S, NULL, NULL);

    /* Feed a long CS content (> 64 bytes) to trigger realloc */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    tf_asserteq(r, TF_AGAIN);

    for (i = 0; i < 128; i++) buf[i] = (unsigned char)('A' + (i % 26));
    r = feed_seq(&S, &key, buf, 128);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CS_OSC);

    /* terminate with BEL */
    r = feed_byte(&S, &key, 0x07);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 128);
    tf_free(&S);

    /* >256-byte CS: several realloc doublings */
    {
        char big[303];
        tf_init(&S, NULL, NULL);
        big[0] = 0x1b, big[1] = ']';
        for (i = 2; i < 302; i++)
            big[i] = (unsigned char)('A' + ((i - 2) % 26));
        big[302] = 0x07;
        r = feed_seq(&S, &key, big, 303);
        tf_asserteq(r, TF_OK);
        tf_asserteq(key.type, TF_TYPE_OSC);
        tf_assert(tf_string(&S, &len) != NULL);
        tf_asserteq(len, 300);
        tf_free(&S);
    }

    /* backslash not preceded by ESC: plain content byte */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]a\\b\x07", 6);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 3);
    tf_assertstreq(tf_string(&S, NULL), "a\\b");
    tf_free(&S);

    /* backslash as very first content byte (cs_len == 0): not an ST */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]\\x\x07", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 2);
    tf_assertstreq(tf_string(&S, NULL), "\\x");
    tf_free(&S);
}

static void test_cs_allocfree(void) {
    tf_State S;
    tf_Key   key;
    int      r, i;
    char     buf[128];
    tf_init(&S, test_alloc, NULL);

    /* OSC with custom allocator → cs_buf allocated via test_alloc */
    r = feed_seq(&S, &key, "\x1b]hello\x07", 9);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);

    /* tf_free should free cs_buf via test_alloc */
    tf_free(&S);

    /* CS realloc via custom allocator: >64 bytes */
    tf_init(&S, test_alloc, NULL);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    tf_asserteq(r, TF_AGAIN);
    for (i = 0; i < 128; i++) buf[i] = (unsigned char)('A' + (i % 26));
    r = feed_seq(&S, &key, buf, 128);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x07);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_free(&S);

    /* OOM on CS realloc: first alloc succeeds, realloc fails */
    {
        int ec = 1; /* allow first alloc, fail second */
        tf_init(&S, oom_alloc, &ec);
        r = feed_byte(&S, &key, 0x1b);
        tf_asserteq(r, TF_AGAIN);
        r = feed_byte(&S, &key, ']');
        tf_asserteq(r, TF_AGAIN);

        /* feed 64 bytes (fits in initial 64) */
        for (i = 0; i < 64; i++) buf[i] = (unsigned char)'X';
        r = feed_seq(&S, &key, buf, 64);
        tf_asserteq(r, TF_AGAIN);

        /* 65th byte triggers realloc → OOM */
        r = feed_byte(&S, &key, 'Y');
        tf_asserteq(r, TF_ERRMEM);
        tf_asserteq(S.state, TF_STATE_IDLE);
        tf_free(&S);
    }
}

/* cs_buf must be NUL-terminated: tf_string consumers treat it as a C string */
static void test_cs_nul(void) {
    tf_State    S;
    tf_Key      key;
    const char *str;
    int         r, len;
    tf_init(&S, fill_alloc, NULL); /* new tail bytes are 0xA5 */
    r = feed_seq(&S, &key, "\x1b]0;hello\x07", 10);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    str = tf_string(&S, &len);
    tf_asserteq(len, 7);
    tf_asserteq((int)str[len], 0); /* pre-fix: 0xA5 beyond the content */
    tf_assertstreq(str, "0;hello");
    tf_free(&S);

    /* kitty text shares the CS buffer: NUL-terminated as well */
    tf_init(&S, fill_alloc, NULL);
    r = feed_seq(&S, &key, "\x1b[97;2;65u", 10);
    tf_asserteq(r, TF_OK);
    str = tf_string(&S, &len);
    tf_asserteq(len, 1);
    tf_asserteq((int)str[len], 0);
    tf_assertstreq(str, "A");
    tf_free(&S);
}

/* ─── mock terminfo lookup ─── */

typedef struct {
    const char *name;
    const char *seq;
} TILookup;

static const char *ti_lookup(void *ud, const char *name) {
    const TILookup *tbl = (const TILookup *)ud;
    int             i;
    for (i = 0; tbl[i].name; i++)
        if (strcmp(tbl[i].name, name) == 0) return tbl[i].seq;
    return NULL;
}

/* ─── Phase 6: trie tests ─── */

static void test_trie_match(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* \x1b\x7f → BACKSPACE via trie (DSA would give DEL+ALT) */
    r = feed_seq(&S, &key, "\x1b\x7f", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    tf_asserteq(key.modifiers, 0);
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

static void test_trie_singlebyte(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_backspace", "\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* lone 0x7f → BACKSPACE via trie (DSA would give DEL) */
    r = feed_seq(&S, &key, "\x7f", 1);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    tf_asserteq(key.modifiers, 0);

    /* 0x08 → BACKSPACE via trie (DSA would give CTRL+'h') */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    tbl[0].seq = "\x08";
    tf_setlookup(&S, ti_lookup, tbl);
    r = feed_seq(&S, &key, "\x08", 1);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);

    /* plain 'a' still a character: trie dead end falls back to DSA */
    r = feed_seq(&S, &key, "a", 1);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');

    /* 0x7f after a trie dead end still matches (IDLE re-arms root) */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    tbl[0].seq = "\x7f";
    tf_setlookup(&S, ti_lookup, tbl);
    r = feed_seq(&S, &key, "a\x7f", 2);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);

    tf_free(&S);
}

static void test_trie_fallback(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* \x1b[A → DSA: UP (trie has no match for \x1bA) */
    r = feed_seq(&S, &key, "\x1b[A", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);

    tf_free(&S);
}

static void test_trie_aftermatch(void) {
    tf_State   S;
    tf_Key     key;
    int        r;
    MockReader mr;
    char       data[] = "\x1b\x7fx";
    TILookup   tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* \x1b\x7fx → BACKSPACE, then 'x' (no misalignment) */
    mr.data = data, mr.len = 3, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    /* next read: 'x' */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'x');

    tf_free(&S);
}

static void test_trie_nolookup(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* no lookup → everything works via DSA */
    r = feed_seq(&S, &key, "\x1b[A", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);

    tf_free(&S);
}

static void test_trie_idle_inactive(void) {
    tf_State   S;
    tf_Key     key;
    int        r;
    MockReader mr;
    char       data[] = "[1~";
    TILookup   tbl[] = {{"key_find", "\x1b[1~"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* "[1~" without ESC must NOT match trie: three plain chars */
    mr.data = data, mr.len = 3, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '[');
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '1');
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '~');

    tf_free(&S);
}

static void test_trie_shifted(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_btab", "\x1b[Z"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* \x1b[Z → Shift+TAB via trie */
    r = feed_seq(&S, &key, "\x1b[Z", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);

    tf_free(&S);
}

static void test_trie_fkeybreak(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_f1", "\x1b[OP"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* key_f1 loaded, f2 missing → only f1 matches */
    r = feed_seq(&S, &key, "\x1bOP", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 1);

    /* key_f2 not loaded → DSA handles \x1bOQ as SS3 F2 */
    r = feed_seq(&S, &key, "\x1bOQ", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 2);

    tf_free(&S);
}

static void test_trie_oom(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    int      ec;
    TILookup tbl[] = {
            {"key_backspace", "\x1b\x7f"}, {"key_f1", "\x1bOP"}, {NULL, NULL}};
    /* fail at each allocation count in turn: build must fail
     * gracefully, DSA must still work on partial/empty trie */
    for (ec = 0; ec <= 8; ec++) {
        int n = ec;
        tf_init(&S, oom_alloc, &n);
        tf_setlookup(&S, ti_lookup, tbl);
        r = feed_seq(&S, &key, "\x1b[A", 3);
        tf_asserteq(r, TF_OK);
        tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
        tf_free(&S);
    }

    /* no remaining allocations: success path */
    {
        int n = 100;
        tf_init(&S, oom_alloc, &n);
        tf_setlookup(&S, ti_lookup, tbl);
        r = feed_seq(&S, &key, "\x1b\x7f", 2);
        tf_asserteq(r, TF_OK);
        tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
        tf_free(&S);
    }
}

/* ─── Phase 7: flush tests ─── */

static void test_flush_csi_full(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1;2 → CSI partial, flush */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '1');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ';');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '2');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CSI);

    /* flush → ALT+[ + REPLAY_BUF of "1;2" */
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '[');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.replay, 3);

    /* replay: '1', ';', '2' */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, '1');

    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, ';');

    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, '2');
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* replay re-parses bytes through the DSA: an ESC inside the residual enters
 * the ESCAPE state (flush yields <Esc>, aligned with Nvim buffer re-parse) */
static void test_replay_escape(void) {
    tf_State   S;
    tf_Key     key;
    MockReader mr;
    int        r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1;\x1b → CSI partial with ESC inside params, flush → replay */
    mr.data = "\x1b[1;\x1b", mr.len = 5, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_AGAIN);
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, '[');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.replay, 3);

    /* replay re-parses "1;\x1b": '1', ';' plain, then ESC → ESCAPE state */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '1');
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, ';');
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_AGAIN); /* ESC pending, replay exhausted */
    tf_asserteq(S.state, TF_STATE_ESCAPE);
    /* flush yields <Esc>, not UNICODE(0x1b) */
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);

    /* \eO + 'G' → ALT+O, 'G' re-parsed plain */
    mr.data = "\x1bOG", mr.len = 3, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'O');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'G');

    tf_free(&S);
}

static void test_flush_ss3(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1bO → SS3, flush */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_SS3);

    /* flush → ALT+O, no replay */
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'O');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

static void test_flush_cs(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b]abc → CS_OSC partial, flush */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    tf_asserteq(r, TF_AGAIN); /* cs_buf allocated on first content byte */
    r = feed_byte(&S, &key, 'a');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'b');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'c');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state >= TF_STATE_CS_DCS && S.state <= TF_STATE_CS_APC, 1);

    /* flush → ALT+] + IDLE re-parse of "abc" */
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, ']');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* re-parse: 'a', 'b', 'c' */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'a');
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'b');
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'c');
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* \x1b] (no content) → flush → ALT+] only, straight IDLE */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state >= TF_STATE_CS_DCS && S.state <= TF_STATE_CS_APC, 1);
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, ']');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* \x1bPabc → CS_DCS, flush → ALT+P + REPLAY */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'P');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'a');
    tf_asserteq(r, TF_AGAIN);
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'P');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(tf_readkey(&S, &key), TF_OK);
    tf_asserteq(key.d.codepoint, 'a');

    /* \x1b_abc → CS_APC, flush → ALT+_ + REPLAY */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '_');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'a');
    tf_asserteq(r, TF_AGAIN);
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, '_');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(tf_readkey(&S, &key), TF_OK);
    tf_asserteq(key.d.codepoint, 'a');

    tf_free(&S);
}

static void test_flush_utf8(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* cross-chunk UTF-8: lead byte only → flush → FFFD */
    r = feed_byte(&S, &key, 0xC3);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_UTF8);

    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0xFFFD);
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

static void test_flush_mousex10(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[M → MOUSE_X10 (no raw bytes yet), flush */
    r = feed_seq(&S, &key, "\x1b[M", 3);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_MOUSE_X10);

    /* flush → ALT+[ + REPLAY_BUF ("M" prepended, buf empty here) */
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '[');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.replay, 1);

    /* replay 'M' */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'M');
    /* buf had no raw bytes → after 'M', straight to IDLE */
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

static void test_flush_mousex10_raw(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[M + 1 raw byte → MOUSE_X10 (1 raw byte in buf) */
    r = feed_seq(&S, &key, "\x1b[M", 3);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x20);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_MOUSE_X10);
    tf_asserteq(S.buf_len, 2); /* 'M' + 1 raw byte */

    /* flush → ALT+[ + REPLAY_BUF("M" + 0x20) */
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, '[');
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    /* replay 'M' */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'M');
    tf_asserteq(S.replay, 1); /* REPLAY_BUF: 1 raw byte pending */

    /* replay raw byte → UNICODE(0x20) */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE); /* 0x20 < 0x7F → UNICODE */
    tf_asserteq(key.d.codepoint, 0x20);
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

static void test_flush_replay(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* flush during replay (replay pending, state IDLE) → no-op */
    S.replay = 2;
    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_NONE);

    tf_free(&S);
}

static void test_flush_escapealt_csi(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b\x1b[1 → flush → ALT+[ (alt_pending consumed in flush) */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_byte(&S, &key, '[');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '1');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CSI);

    r = tf_flush(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '[');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.replay, 1); /* "1" to replay */

    tf_free(&S);
}

static void test_replay_seq(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* set up replay source with 3 bytes: 'a', 0x80, 'b' */
    S.replay = 3;
    S.buf[TF_MAX_BUFLEN - 3] = 'a';
    S.buf[TF_MAX_BUFLEN - 2] = 0x80;
    S.buf[TF_MAX_BUFLEN - 1] = 'b';
    S.buf_len = 0;

    /* 'a' → UNICODE('a') */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');

    /* 0x80 → 0xFFFD */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0xFFFD);

    /* 'b' → UNICODE('b') */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'b');

    /* after last byte → IDLE */
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* setlookup reports trie-build OOM instead of swallowing it */
static void test_setlookup_oom(void) {
    tf_State S;
    int      n;
    TILookup tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    TILookup f1only[] = {{"key_f1", "\x1bOP"}, {NULL, NULL}};
    /* loadtable path: first trie allocation fails */
    n = 0;
    tf_init(&S, oom_alloc, &n);
    tf_asserteq(tf_setlookup(&S, ti_lookup, tbl), TF_ERRMEM);
    tf_free(&S);
    /* loadfkeys path (no keytable entries present) */
    n = 0;
    tf_init(&S, oom_alloc, &n);
    tf_asserteq(tf_setlookup(&S, ti_lookup, f1only), TF_ERRMEM);
    tf_free(&S);
    /* success path */
    n = 100;
    tf_init(&S, oom_alloc, &n);
    tf_asserteq(tf_setlookup(&S, ti_lookup, tbl), TF_OK);
    tf_free(&S);
}

static void test_trie_setlookup_params(void) {
    tf_State S;
    tf_init(&S, NULL, NULL);

    /* tf_setlookup with NULL → no-op, no crash */
    tf_setlookup(NULL, ti_lookup, NULL);
    tf_setlookup(&S, NULL, NULL);
    tf_asserteq(S.root, (struct tf_Node *)NULL);

    /* lookup that stops after the first F-key */
    {
        TILookup f1only[] = {{"key_f1", "\x1bOP"}, {NULL, NULL}};
        tf_setlookup(&S, ti_lookup, f1only);
        tf_asserteq(S.root != NULL, 1);
    }

    /* full F1-F63 table → all keys loaded */
    {
        static char nb[63][16];
        TILookup    fk[64];
        int         i;
        tf_free(&S);
        tf_init(&S, NULL, NULL);
        for (i = 0; i < 63; i++) {
            snprintf(nb[i], sizeof(nb[i]), "key_f%d", i + 1);
            fk[i].name = nb[i];
            fk[i].seq = "\x1bOP";
        }
        fk[63].name = NULL;
        fk[63].seq = NULL;
        tf_setlookup(&S, ti_lookup, fk);
        tf_asserteq(S.root != NULL, 1);
    }

    tf_free(&S);
}

/* ─── Phase 6+7: combined trie + flush ─── */

static void test_trie_withalt(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* \x1b\x1b\x7f → BACKSPACE + ALT (alt_pending merged) */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_byte(&S, &key, 0x7f);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

static void test_trie_multikey(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {
            {"key_f1", "\x1bOP"},
            {"key_home", "\x1bOH"},
            {"key_end", "\x1bOF"},
            {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    r = feed_seq(&S, &key, "\x1bOP", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 1);

    r = feed_seq(&S, &key, "\x1bOH", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    r = feed_seq(&S, &key, "\x1bOF", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_END);

    tf_free(&S);
}

/* test trie extent expansion: keys with different first bytes */
static void test_trie_expand(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_f1", "\x1bOP"}, {"key_up", "\x1b[A"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* \x1bOP → F1 (first byte after \x1b = 'O') */
    r = feed_seq(&S, &key, "\x1bOP", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 1);

    /* \x1b[A → UP (first byte after \x1b = '[' - expands extent left) */
    r = feed_seq(&S, &key, "\x1b[A", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);

    tf_free(&S);
}

/* test trie with custom allocator */
static void test_trie_alloc(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_home", "\x1bOH"}, {NULL, NULL}};
    tf_init(&S, test_alloc, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    r = feed_seq(&S, &key, "\x1bOH", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    tf_free(&S);
}

static void test_trie_allocfree(void) {
    tf_State S;
    TILookup tbl[] = {{"key_home", "\x1bOH"}, {NULL, NULL}};
    tf_init(&S, test_alloc, NULL);
    tf_setlookup(&S, ti_lookup, tbl);
    tf_assert(S.root != NULL);
    tf_free(&S); /* frees trie via test_alloc */
}

/* repeated tf_setlookup must not leak the previous trie */
static void test_trie_reload(void) {
    tf_State S;
    tf_Key   key;
    size_t   live = 0;
    int      r;
    TILookup tbl[] = {
            {"key_home", "\x1b[1~"}, {"key_left", "\x1b[D"}, {NULL, NULL}};
    tf_init(&S, count_alloc, &live);
    tf_setlookup(&S, ti_lookup, tbl);
    tf_setlookup(&S, ti_lookup, tbl);
    tf_assert(live > 0);
    tf_free(&S);
    tf_asserteq((long)live, 0); /* leaked trie nodes keep live > 0 */
    /* reloaded trie still matches */
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);
    r = feed_seq(&S, &key, "\x1b[1~", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_HOME);
    tf_free(&S);
}

/* a terminfo key that is a strict prefix of a later one must not abort
 * the load (KEY node used as trie interior) */
static void test_trie_prefix(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {
            {"key_home", "\x1b[2"}, {"key_left", "\x1b[2x"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl); /* pre-fix: assert abort */
    r = feed_seq(&S, &key, "\x1b[2", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_HOME);
    tf_free(&S);
}

/* ─── Phase 8: canonicalise flags ─── */

static void test_canon_delbs(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_DELBS);

    /* 0x7f → BACKSPACE (DELBS on) */
    r = feed_byte(&S, &key, 0x7f);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    tf_asserteq(key.modifiers, 0);

    /* 0x08 → still Ctrl+H (DELBS does not affect 0x08) */
    r = feed_byte(&S, &key, 0x08);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'h');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* DELBS off: 0x7f → DEL */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x7f);
    tf_asserteq(r, TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    /* \x1b + 0x7f with DELBS: ALT+BACKSPACE */
    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_DELBS);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x7f);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

static void test_canon_spacesym(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_SPACESYMBOL);

    /* 0x20 → SPACE keysym */
    r = feed_byte(&S, &key, 0x20);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    tf_asserteq(key.modifiers, 0);

    /* SPACESYMBOL off: 0x20 → UNICODE(0x20) */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x20);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0x20);

    /* \x1b + 0x20 with SPACESYMBOL: ALT+SPACE */
    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_SPACESYMBOL);
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x20);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

static void test_canon_convertkp(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    /* Verify CONVERTKP behavior (already implemented, regression test) */
    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_CONVERTKP);

    r = feed_seq(&S, &key, "\x1bOp", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '0');

    /* KPENTER stays KPENTER */
    r = feed_seq(&S, &key, "\x1bOM", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPENTER);

    tf_free(&S);
}

/* ─── Phase 8: sym name table ─── */

static void test_symname_basic(void) {
    tf_assert(tf_name((int)TF_SYM_NONE) == NULL);

    tf_assertstreq(tf_name((int)TF_SYM_BACKSPACE), "Backspace");
    tf_assertstreq(tf_name((int)TF_SYM_TAB), "Tab");
    tf_assertstreq(tf_name((int)TF_SYM_ENTER), "Enter");
    tf_assertstreq(tf_name((int)TF_SYM_ESCAPE), "Escape");
    tf_assertstreq(tf_name((int)TF_SYM_SPACE), "Space");
    tf_assertstreq(tf_name((int)TF_SYM_DELETE), "Delete");

    tf_assertstreq(tf_name((int)TF_SYM_UP), "Up");
    tf_assertstreq(tf_name((int)TF_SYM_DOWN), "Down");
    tf_assertstreq(tf_name((int)TF_SYM_LEFT), "Left");
    tf_assertstreq(tf_name((int)TF_SYM_RIGHT), "Right");
}

static void test_symname_editing(void) {
    tf_assertstreq(tf_name((int)TF_SYM_BEGIN), "Begin");
    tf_assertstreq(tf_name((int)TF_SYM_FIND), "Find");
    tf_assertstreq(tf_name((int)TF_SYM_INSERT), "Insert");
    tf_assertstreq(tf_name((int)TF_SYM_DELETE), "Delete");
    tf_assertstreq(tf_name((int)TF_SYM_SELECT), "Select");
    tf_assertstreq(tf_name((int)TF_SYM_PAGEUP), "PageUp");
    tf_assertstreq(tf_name((int)TF_SYM_PAGEDOWN), "PageDown");
    tf_assertstreq(tf_name((int)TF_SYM_HOME), "Home");
    tf_assertstreq(tf_name((int)TF_SYM_END), "End");
}

static void test_symname_terminfo(void) {
    tf_assertstreq(tf_name((int)TF_SYM_CANCEL), "Cancel");
    tf_assertstreq(tf_name((int)TF_SYM_CLEAR), "Clear");
    tf_assertstreq(tf_name((int)TF_SYM_CLOSE), "Close");
    tf_assertstreq(tf_name((int)TF_SYM_COMMAND), "Command");
    tf_assertstreq(tf_name((int)TF_SYM_COPY), "Copy");
    tf_assertstreq(tf_name((int)TF_SYM_EXIT), "Exit");
    tf_assertstreq(tf_name((int)TF_SYM_HELP), "Help");
    tf_assertstreq(tf_name((int)TF_SYM_MARK), "Mark");
    tf_assertstreq(tf_name((int)TF_SYM_MESSAGE), "Message");
    tf_assertstreq(tf_name((int)TF_SYM_MOVE), "Move");
    tf_assertstreq(tf_name((int)TF_SYM_OPEN), "Open");
    tf_assertstreq(tf_name((int)TF_SYM_OPTIONS), "Options");
    tf_assertstreq(tf_name((int)TF_SYM_PRINT), "Print");
    tf_assertstreq(tf_name((int)TF_SYM_REDO), "Redo");
    tf_assertstreq(tf_name((int)TF_SYM_REFERENCE), "Reference");
    tf_assertstreq(tf_name((int)TF_SYM_REFRESH), "Refresh");
    tf_assertstreq(tf_name((int)TF_SYM_REPLACE), "Replace");
    tf_assertstreq(tf_name((int)TF_SYM_RESTART), "Restart");
    tf_assertstreq(tf_name((int)TF_SYM_RESUME), "Resume");
    tf_assertstreq(tf_name((int)TF_SYM_SAVE), "Save");
    tf_assertstreq(tf_name((int)TF_SYM_SUSPEND), "Suspend");
    tf_assertstreq(tf_name((int)TF_SYM_UNDO), "Undo");
}

static void test_symname_kp(void) {
    tf_assertstreq(tf_name((int)TF_SYM_KP0), "k0");
    tf_assertstreq(tf_name((int)TF_SYM_KP1), "k1");
    tf_assertstreq(tf_name((int)TF_SYM_KP9), "k9");
    tf_assertstreq(tf_name((int)TF_SYM_KPENTER), "kEnter");
    tf_assertstreq(tf_name((int)TF_SYM_KPPLUS), "kPlus");
    tf_assertstreq(tf_name((int)TF_SYM_KPMINUS), "kMinus");
    tf_assertstreq(tf_name((int)TF_SYM_KPMULT), "kMultiply");
    tf_assertstreq(tf_name((int)TF_SYM_KPDIV), "kDivide");
    tf_assertstreq(tf_name((int)TF_SYM_KPCOMMA), "kComma");
    tf_assertstreq(tf_name((int)TF_SYM_KPPERIOD), "kPoint");
    tf_assertstreq(tf_name((int)TF_SYM_KPEQUALS), "kEqual");
}

static void test_symname_oob(void) {
    /* out of bounds → NULL */
    tf_assert(tf_name(-1) == NULL);
    tf_assert(tf_name(TF_SYM_COUNT) == NULL);
    tf_assert(tf_name(9999) == NULL);
}

static void test_sym_lookup(void) {
    /* tf_sym: name → sym index */
    tf_asserteq(tf_sym("Backspace"), (int)TF_SYM_BACKSPACE);
    tf_asserteq(tf_sym("Tab"), (int)TF_SYM_TAB);
    tf_asserteq(tf_sym("Enter"), (int)TF_SYM_ENTER);
    tf_asserteq(tf_sym("Escape"), (int)TF_SYM_ESCAPE);
    tf_asserteq(tf_sym("Space"), (int)TF_SYM_SPACE);
    tf_asserteq(tf_sym("Delete"), (int)TF_SYM_DELETE);
    tf_asserteq(tf_sym("Up"), (int)TF_SYM_UP);
    tf_asserteq(tf_sym("Down"), (int)TF_SYM_DOWN);
    tf_asserteq(tf_sym("PageUp"), (int)TF_SYM_PAGEUP);
    tf_asserteq(tf_sym("PageDown"), (int)TF_SYM_PAGEDOWN);
    tf_asserteq(tf_sym("kEnter"), (int)TF_SYM_KPENTER);
    tf_asserteq(tf_sym("kPlus"), (int)TF_SYM_KPPLUS);

    /* not found → -1 */
    tf_asserteq(tf_sym("NoSuchKey"), -1);
    tf_asserteq(tf_sym(""), -1);
    tf_asserteq(tf_sym(NULL), -1);

    /* case-insensitive */
    tf_asserteq(tf_sym("backspace"), (int)TF_SYM_BACKSPACE);
    tf_asserteq(tf_sym("PAGEUP"), (int)TF_SYM_PAGEUP);
    tf_asserteq(tf_sym("kEnter"), (int)TF_SYM_KPENTER);
    /* libtermkey legacy names are aliases in tf_parse, not tf_sym */
    tf_asserteq(tf_sym("KPEnter"), -1);
    {
        tf_Key key;
        tf_asserteq(tf_parse("<KPEnter>", &key), 9);
        tf_asserteq((long)key.d.sym, (long)TF_SYM_KPENTER);
    }
}

static void test_sym_roundtrip(void) {
    int i;
    /* all syms except NONE round-trip */
    for (i = (int)TF_SYM_BACKSPACE; i < TF_SYM_COUNT; i++) {
        const char *name = tf_name(i);
        tf_assert(name != NULL);
        tf_asserteq(tf_sym(name), i);
    }
}

/* ─── Phase 9: tf_format ─── */

static void test_fmt_basic(void) {
    tf_Key key;
    char   buf[64];
    int    n;

    memset(&key, 0, sizeof(key));

    /* KEYSYM */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_ESCAPE;
    n = tf_format(buf, sizeof(buf), &key, 0);
    tf_asserteq(n, 8);
    tf_assertstreq(buf, "<Escape>");

    /* FUNCTION */
    key.type = TF_TYPE_FUNCTION;
    key.d.number = 1;
    n = tf_format(buf, sizeof(buf), &key, 0);
    tf_asserteq(n, 4);
    tf_assertstreq(buf, "<F1>");

    /* UNICODE printable */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'a';
    key.modifiers = 0;
    n = tf_format(buf, sizeof(buf), &key, 0);
    tf_asserteq(n, 1);
    tf_assertstreq(buf, "a");

    /* invalid sym value → empty name, no crash */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = (tf_Sym)9999;
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    tf_asserteq(n, 2);
    tf_assertstreq(buf, "<>");
}

static void test_fmt_mods(void) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    /* C-x → <C-x> (default: no SPACEMOD) */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<C-x>");

    /* S-A-C-x → <S-M-C-x> (default: ALTISMETA) */
    key.modifiers = TF_MOD_SHIFT | TF_MOD_ALT | TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<S-M-C-x>");

    /* D-x → <D-x> */
    key.modifiers = TF_MOD_SUPER;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<D-x>");

    /* T-x → <T-x> */
    key.modifiers = TF_MOD_META;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<T-x>");

    /* Without SPACEMOD, no WRAPBRACKET: C-x (bare) */
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<C-x>");

    /* CARETCTRL: C-x → ^X */
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_CARETCTRL | TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<^X>");

    /* LONGMOD: C-x → Control-x */
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_LONGMOD | TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<Control-x>");

    /* ALTISMETA: A- → M- */
    key.modifiers = TF_MOD_ALT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_ALTISMETA | TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<M-x>");
}

static void test_fmt_spacing(void) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    /* SPACEMOD: C-x → C x */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET | TF_FMT_SPACEMOD);
    tf_assertstreq(buf, "<C x>");

    /* LOWERMOD: D-C-X → d-c-X (default: no LOWERMOD means uppercase mods) */
    key.modifiers = TF_MOD_CTRL | TF_MOD_SUPER;
    key.d.codepoint = 'X';
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<D-C-X>");

    /* LOWERSPACE: PageUp → page up */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_PAGEUP;
    key.modifiers = 0;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET | TF_FMT_LOWERSPACE);
    tf_assertstreq(buf, "<page up>");
}

static void test_fmt_kitty(void) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    /* KEYSYM Escape → <Escape> */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_ESCAPE;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<Escape>");

    /* FUNCTION 13 → <F13> */
    key.type = TF_TYPE_FUNCTION;
    key.d.number = 13;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<F13>");

    /* KEYSYM KP0 → <k0> */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_KP0;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<k0>");

    /* KEYSYM LEVEL5SHIFT → <Level5Shift> */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_LEVEL5_SHIFT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<Level5Shift>");

    /* KEYSYM KPLEFT → <kLeft> */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_KPLEFT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<kLeft>");
}

static void test_fmt_mouse(void) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    /* MOUSE: plain name, data goes through tf_mouse() */
    key.type = TF_TYPE_MOUSE;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<Mouse>");
}

static void test_fmt_types(void) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    key.type = TF_TYPE_POSITION;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<Position>");

    key.type = TF_TYPE_MODEREPORT;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<ModeReport>");

    key.type = TF_TYPE_KITTYREPORT;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<KittyReport>");

    key.type = TF_TYPE_DCS;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<DCS>");

    key.type = TF_TYPE_OSC;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<OSC>");

    key.type = TF_TYPE_APC;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<APC>");

    key.type = TF_TYPE_UNKNOWN_CSI;
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<Unknown CSI>");
}

static void test_fmt_trunc(void) {
    tf_Key key;
    char   buf[8];
    int    n;

    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_ESCAPE;

    /* buffer too small: truncates */
    n = tf_format(buf, 5, &key, 0);
    tf_asserteq(n, 4); /* "<Esc" + NUL */
    tf_assertstreq(buf, "<Esc");

    n = tf_format(buf, 2, &key, 0);
    tf_asserteq(n, 1); /* "<", truncated */
    tf_assertstreq(buf, "<");

    n = tf_format(buf, 1, &key, 0);
    tf_asserteq(n, 0);
}

static void test_fmt_params(void) {
    char buf[16];
    int  n;

    tf_Key key;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'a';

    n = tf_format(buf, sizeof(buf), NULL, 0);
    tf_asserteq(n, TF_ERRPARAM);

    n = tf_format(NULL, sizeof(buf), NULL, 0);
    tf_asserteq(n, TF_ERRPARAM);

    n = tf_format(NULL, sizeof(buf), &key, 0);
    tf_asserteq(n, TF_ERRPARAM);

    n = tf_format(buf, 0, &key, 0);
    tf_asserteq(n, TF_ERRPARAM);

    n = tf_format(buf, -1, &key, 0);
    tf_asserteq(n, TF_ERRPARAM);
}

/* ─── Phase 9: tf_parse ─── */

static void test_parse_basic(void) {
    tf_Key key;
    int    n;

    /* <C-x> → Ctrl+'x' */
    n = tf_parse("<C-x>", &key);
    tf_asserteq(n, 5);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* <Escape> → KEYSYM ESCAPE */
    n = tf_parse("<Escape>", &key);
    tf_asserteq(n, 8);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);

    /* <F1> → FUNCTION 1 */
    n = tf_parse("<F1>", &key);
    tf_asserteq(n, 4);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 1);

    /* <F63> → FUNCTION 63 */
    n = tf_parse("<F63>", &key);
    tf_asserteq(n, 5);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 63);

    /* <F99> out of range → parse fails (falls through to char check) */
    n = tf_parse("<F99>", &key);
    tf_asserteq(n, -1);
}

static void test_parse_mods(void) {
    tf_Key key;
    int    n;

    /* <S-A-C-x> */
    n = tf_parse("<S-A-C-x>", &key);
    tf_asserteq(n, 9);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_SHIFT | TF_MOD_ALT | TF_MOD_CTRL);

    /* <D-T-x> */
    n = tf_parse("<D-T-x>", &key);
    tf_asserteq(n, 7);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_SUPER | TF_MOD_META);

    /* <M-Left> → M=ALT + Left */
    n = tf_parse("<M-Left>", &key);
    tf_asserteq(n, 8);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_LEFT);
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    /* <C-S-Tab> */
    n = tf_parse("<C-S-Tab>", &key);
    tf_asserteq(n, 9);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    tf_asserteq(key.modifiers, TF_MOD_CTRL | TF_MOD_SHIFT);

    /* double separator: <S--x> → Shift + 'x' */
    n = tf_parse("<S--x>", &key);
    tf_asserteq(n, 6);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);
}

static void test_parse_case(void) {
    tf_Key key;
    int    n;

    /* case-insensitive key names */
    n = tf_parse("<pageup>", &key);
    tf_asserteq(n, 8);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_PAGEUP);

    /* space in key name */
    n = tf_parse("<Page Up>", &key);
    tf_asserteq(n, 9);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq(
            (long)key.d.sym, (long)TF_SYM_PAGEUP); /* tf_sym case-insensitive */

    /* <C-s> = Ctrl+'s' (lowercase s = keyname) */
    n = tf_parse("<C-s>", &key);
    tf_asserteq(n, 5);
    tf_asserteq(key.d.codepoint, 's');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* <C-S-x> = Ctrl+Shift+'x' */
    n = tf_parse("<C-S-x>", &key);
    tf_asserteq(n, 7);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_CTRL | TF_MOD_SHIFT);

    /* <C-C> = Ctrl+'C': single-letter key name must not be eaten as a
     * modifier (modifier name only counts when followed by '-'/' ') */
    n = tf_parse("<C-C>", &key);
    tf_asserteq(n, 5);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'C');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);
}

static void test_parse_caret(void) {
    tf_Key key;
    int    n;

    /* <^X> = Ctrl+'X' (^ + uppercase letter) */
    n = tf_parse("<^X>", &key);
    tf_asserteq(n, 4);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'X');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* <^x> = fail (^ must be followed by uppercase letter) */
    n = tf_parse("<^x>", &key);
    tf_asserteq(n, -1);
}

static void test_parse_kitty(void) {
    tf_Key key;
    int    n;

    /* <Esc> → TF_SYM_ESCAPE (kitty name maps to sym) */
    n = tf_parse("<Esc>", &key);
    tf_asserteq(n, 5);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);

    /* <BS> → TF_SYM_BACKSPACE */
    n = tf_parse("<BS>", &key);
    tf_asserteq(n, 4);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);

    /* <CR> → TF_SYM_ENTER */
    n = tf_parse("<CR>", &key);
    tf_asserteq(n, 4);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ENTER);

    /* <k0> → TF_SYM_KP0 (kitty keypad name alias) */
    n = tf_parse("<k0>", &key);
    tf_asserteq(n, 4);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KP0);

    /* <Del> → TF_SYM_DELETE (Vim name alias) */
    n = tf_parse("<Del>", &key);
    tf_asserteq(n, 5);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    /* <kPoint> → TF_SYM_KPPERIOD */
    n = tf_parse("<kPoint>", &key);
    tf_asserteq(n, 8);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPPERIOD);

    /* <kLeft> → TF_SYM_KPLEFT (primary name) */
    n = tf_parse("<kLeft>", &key);
    tf_asserteq(n, 7);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPLEFT);

    /* <CapsLock> → TF_SYM_CAPSLOCK */
    n = tf_parse("<CapsLock>", &key);
    tf_asserteq(n, 10);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_CAPSLOCK);
}

static void test_parse_nobracket(void) {
    tf_Key key;
    int    n;

    /* plain 'a' → UNICODE 'a' */
    n = tf_parse("a", &key);
    tf_asserteq(n, 1);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.modifiers, 0);

    /* multi-byte UTF-8: 'é' (0xC3 0xA9) */
    n = tf_parse("\xc3\xa9", &key);
    tf_asserteq(n, 2);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0xE9);
}

static void test_parse_fail(void) {
    tf_Key key;
    int    n;

    /* single letter without separator is a key name: <S> → 'S' (no modifier) */
    n = tf_parse("<S>", &key);
    tf_asserteq(n, 3);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'S');
    tf_asserteq(key.modifiers, 0);

    /* invalid: <c-x> — lowercase c not a modifier */
    n = tf_parse("<c-x>", &key);
    tf_asserteq(n, -1);

    /* <C> → key name 'C' (modifier requires a separator) */
    n = tf_parse("<C>", &key);
    tf_asserteq(n, 3);
    tf_asserteq(key.d.codepoint, 'C');
    tf_asserteq(key.modifiers, 0);

    /* <^A without closing '>' → fail */
    n = tf_parse("<^A", &key);
    tf_asserteq(n, -1);

    /* overlong UTF-8 in plain string → FFFD + fail */
    n = tf_parse("\xc0\xaf", &key);
    tf_asserteq(n, -1);

    /* empty string */
    n = tf_parse("", &key);
    tf_asserteq(n, -1);

    /* NULL */
    n = tf_parse(NULL, &key);
    tf_asserteq(n, -1);

    /* key NULL */
    n = tf_parse("a", NULL);
    tf_asserteq(n, -1);
}

static void test_parse_longmod(void) {
    tf_Key key;
    int    n;

    /* <Control-x> */
    n = tf_parse("<Control-x>", &key);
    tf_asserteq(n, 11);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* <Shift-Up> */
    n = tf_parse("<Shift-Up>", &key);
    tf_asserteq(n, 10);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);

    /* <Super-x> (S-longname = Super) */
    n = tf_parse("<Super-x>", &key);
    tf_asserteq(n, 9);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_SUPER);
}

static void test_parse_roundtrip(void) {
    tf_Key k1, k2;
    char   buf[128];
    int    n;
    memset(&k1, 0, sizeof(k1));

    /* round-trip: <C-x> */
    k1.type = TF_TYPE_UNICODE;
    k1.d.codepoint = 'x';
    k1.modifiers = TF_MOD_CTRL;
    n = tf_format(buf, sizeof(buf), &k1, TF_FMT_WRAPBRACKET);
    n = tf_parse(buf, &k2);
    tf_assert(n > 0);
    tf_asserteq(k2.type, k1.type);
    tf_asserteq(k2.d.codepoint, k1.d.codepoint);
    tf_asserteq(k2.modifiers, k1.modifiers);

    /* round-trip: <Escape> */
    memset(&k1, 0, sizeof(k1));
    k1.type = TF_TYPE_KEYSYM;
    k1.d.sym = TF_SYM_ESCAPE;
    n = tf_format(buf, sizeof(buf), &k1, TF_FMT_WRAPBRACKET);
    n = tf_parse(buf, &k2);
    tf_assert(n > 0);
    tf_asserteq(k2.type, k1.type);
    tf_asserteq((long)k2.d.sym, (long)k1.d.sym);

    /* round-trip: <F1> */
    memset(&k1, 0, sizeof(k1));
    k1.type = TF_TYPE_FUNCTION;
    k1.d.number = 1;
    n = tf_format(buf, sizeof(buf), &k1, TF_FMT_WRAPBRACKET);
    n = tf_parse(buf, &k2);
    tf_assert(n > 0);
    tf_asserteq(k2.type, k1.type);
    tf_asserteq(k2.d.number, k1.d.number);

    /* round-trip: <D-S-F12> */
    memset(&k1, 0, sizeof(k1));
    k1.type = TF_TYPE_FUNCTION;
    k1.d.number = 12;
    k1.modifiers = TF_MOD_SUPER | TF_MOD_SHIFT;
    n = tf_format(buf, sizeof(buf), &k1, TF_FMT_WRAPBRACKET);
    n = tf_parse(buf, &k2);
    tf_assert(n > 0);
    tf_asserteq(k2.type, k1.type);
    tf_asserteq(k2.d.number, k1.d.number);
    tf_asserteq(k2.modifiers, k1.modifiers);
}

static void test_parse_bracketchar(void) {
    tf_Key key;
    int    n;

    /* <x> = single char 'x' */
    n = tf_parse("<x>", &key);
    tf_asserteq(n, 3);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'x');

    /* <<> = fail (no valid key name) */
    n = tf_parse("<<>", &key);
    tf_asserteq(n, -1);
}

/* ─── Phase 10: Kitty CSI u ─── */

static void test_kitty_basic(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97u → 'a' */
    tf_asserteq(feed_seq(&S, &key, "\x1b[97u", 5), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.modifiers, 0);
    tf_asserteq(key.event, TF_EVENT_PRESS);
    tf_assertstreq(key.utf8, "a");
    tf_free(&S);
}

static void test_kitty_modifiers(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97;2u → 'a' + SHIFT */
    tf_asserteq(feed_seq(&S, &key, "\x1b[97;2u", 7), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);
    tf_asserteq(key.event, TF_EVENT_PRESS);
    tf_free(&S);
}

static void test_kitty_event(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97;5:2u → 'a' + CTRL + REPEAT */
    tf_asserteq(feed_seq(&S, &key, "\x1b[97;5:2u", 9), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);
    tf_asserteq(key.event, TF_EVENT_REPEAT);

    /* default event (no colon) → PRESS */
    tf_init(&S, NULL, NULL);
    tf_asserteq(feed_seq(&S, &key, "\x1b[97;2u", 7), TF_OK);
    tf_asserteq(key.event, TF_EVENT_PRESS);
    tf_free(&S);
}

static void test_kitty_event_fail(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97;1:9u → UNKNOWN_CSI (invalid event) */
    tf_asserteq(feed_seq(&S, &key, "\x1b[97;1:9u", 9), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    tf_free(&S);
}

static void test_kitty_alts(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97:65;2u → 'a' + SHIFT (alts ignored) */
    tf_asserteq(feed_seq(&S, &key, "\x1b[97:65;2u", 10), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);
    tf_asserteq(key.event, TF_EVENT_PRESS);
    tf_free(&S);
}

static void test_kitty_text(void) {
    tf_State S;
    tf_Key   key;
    int      slen;
    tf_init(&S, NULL, NULL);
    /* \x1b[97;2;65u → 'a' + SHIFT + text "A" */
    tf_asserteq(feed_seq(&S, &key, "\x1b[97;2;65u", 10), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);
    {
        const char *s = tf_string(&S, &slen);
        tf_asserteq(slen, 1);
        tf_assert(memcmp(s, "A", 1) == 0);
    }
    tf_free(&S);
}

static void test_kitty_text_multi(void) {
    tf_State S;
    tf_Key   key;
    int      slen;
    tf_init(&S, NULL, NULL);
    /* \x1b[65;1;99:100u → 'A' + text "cd" */
    tf_asserteq(feed_seq(&S, &key, "\x1b[65;1;99:100u", 14), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'A');
    tf_asserteq(key.modifiers, 0);
    {
        const char *s = tf_string(&S, &slen);
        tf_asserteq(slen, 2);
        tf_assert(memcmp(s, "cd", 2) == 0);
    }
    tf_free(&S);
}

static void test_kitty_func(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[57344u → KEYSYM ESCAPE (kitty functional key) */
    tf_asserteq(feed_seq(&S, &key, "\x1b[57344u", 8), TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);
    tf_free(&S);
}

static void test_kitty_map(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);

    /* C0 codepoint: \e[27u → KEYSYM ESCAPE */
    tf_asserteq(feed_seq(&S, &key, "\x1b[27u", 5), TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);

    /* \e[9u → TAB */
    tf_asserteq(feed_seq(&S, &key, "\x1b[9u", 5), TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_TAB);

    /* \e[13u → ENTER */
    tf_asserteq(feed_seq(&S, &key, "\x1b[13u", 5), TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ENTER);

    /* \e[127u → BACKSPACE (kitty backspace key, not DEL) */
    tf_asserteq(feed_seq(&S, &key, "\x1b[127u", 6), TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);

    /* kitty Delete key with modifier: \e[57349;2u → DELETE + SHIFT */
    tf_asserteq(feed_seq(&S, &key, "\x1b[57349;2u", 10), TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DELETE);
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);

    /* F13: \e[57376u → FUNCTION 13 */
    tf_asserteq(feed_seq(&S, &key, "\x1b[57376u", 8), TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 13);

    /* KP0: \e[57399u → KEYSYM KP0 */
    tf_asserteq(feed_seq(&S, &key, "\x1b[57399u", 8), TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KP0);

    /* kLeft: \e[57417u → KEYSYM KPLEFT */
    tf_asserteq(feed_seq(&S, &key, "\x1b[57417u", 8), TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPLEFT);

    /* media key: \e[57428u → KEYSYM MEDIAPLAY */
    tf_asserteq(feed_seq(&S, &key, "\x1b[57428u", 8), TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_MEDIA_PLAY);

    /* modifier key: \e[57454u → KEYSYM LEVEL5SHIFT */
    tf_asserteq(feed_seq(&S, &key, "\x1b[57454u", 8), TF_OK);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_LEVEL5_SHIFT);

    /* unknown PUA codepoint stays UNICODE */
    tf_asserteq(feed_seq(&S, &key, "\x1b[63743u", 8), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 63743);

    tf_free(&S);
}

static void test_ss3_altreplay(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1bOG (unknown SS3) → ALT+O, then G replayed */
    tf_asserteq(feed_seq(&S, &key, "\x1bOG", 3), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'O');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(tf_readkey(&S, &key), TF_OK);
    tf_asserteq(key.d.codepoint, 'G');
    tf_asserteq(key.modifiers, 0);
    tf_free(&S);
}

static void test_csi_altprefix(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b\x1b[A → ALT+UP (alt_pending merged at CSI dispatch) */
    tf_asserteq(feed_byte(&S, &key, 0x1b), TF_AGAIN);
    tf_asserteq(feed_byte(&S, &key, 0x1b), TF_AGAIN);
    tf_asserteq(feed_seq(&S, &key, "[A", 2), TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_free(&S);
}

static void test_kitty_empty_cp(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[;2u → UNKNOWN_CSI (empty codepoint) */
    tf_asserteq(feed_seq(&S, &key, "\x1b[;2u", 5), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    tf_free(&S);
}

static void test_kitty_widecp(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[233u → 'é' (U+00E9, 2-byte UTF-8) */
    tf_asserteq(feed_seq(&S, &key, "\x1b[233u", 7), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 233);
    tf_asserteq(key.modifiers, 0);
    tf_assertstreq(key.utf8, "\xc3\xa9");

    /* \x1b[0x1F600u → emoji (4-byte UTF-8, kitty uses decimal) */
    tf_init(&S, NULL, NULL);
    tf_asserteq(feed_seq(&S, &key, "\x1b[128512u", 9), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 0x1F600);
    tf_assertstreq(key.utf8, "\xf0\x9f\x98\x80");
    tf_free(&S);
}

static void test_kitty_text_long(void) {
    /* long text field within CSI buf limit (TF_MAX_BUFLEN) */
    tf_State S;
    tf_Key   key;
    char     seq[256];
    int      i, slen, pos = 0;
    tf_init(&S, NULL, NULL);
    memcpy(seq, "\x1b[97;2;", 7);
    pos = 7;
    for (i = 65; i < 65 + 15; i++) { /* 15 codepoints: 51 bytes total */
        if (i > 65) seq[pos++] = ':';
        pos += snprintf(seq + pos, sizeof(seq) - (size_t)pos, "%d", i);
    }
    seq[pos++] = 'u';
    tf_asserteq(feed_seq(&S, &key, seq, pos), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    tf_asserteq(key.modifiers, TF_MOD_SHIFT);
    {
        const char *s = tf_string(&S, &slen);
        tf_assert(s != NULL && slen == 15);
    }
    tf_free(&S);
}

static void test_csi_overflow(void) {
    /* CSI param area > TF_MAX_BUFLEN: sequence abandoned, peeled
     * ALT+[ + REPLAY_BUF replays buf, then main loop continues */
    tf_State S;
    tf_Key   key;
    char     seq[256];
    int      i, pos = 0, r;
    tf_init(&S, NULL, NULL);
    memcpy(seq, "\x1b[", 2);
    pos = 2;
    for (i = 0; i < 70; i++) {
        if (i > 0) seq[pos++] = ';';
        seq[pos++] = (char)('1' + (i % 9));
    }
    seq[pos++] = 'x';
    r = feed_seq(&S, &key, seq, (size_t)pos);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '[');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_asserteq(S.replay, TF_MAX_BUFLEN); /* REPLAY_BUF: full buf */

    /* replay first byte of buf */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, seq[2]);
    tf_asserteq(S.replay, TF_MAX_BUFLEN - 1);

    /* drain replay + chunk remainder until 'x' (last byte) */
    do {
        r = tf_readkey(&S, &key);
        tf_asserteq(r, TF_OK);
    } while (key.d.codepoint != 'x');
    tf_asserteq(S.state, TF_STATE_IDLE);
    tf_free(&S);
}

static void test_kitty_text_oom(void) {
    /* OOM during cs_buf allocation for text */
    tf_State S;
    tf_Key   key;
    int      oomcnt = 0;
    tf_init(&S, oom_alloc, &oomcnt);
    tf_asserteq(feed_seq(&S, &key, "\x1b[97;2;65u", 10), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    /* key should still be valid even without text */
    tf_free(&S);
}

static void test_kitty_params(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* NULL/zero checks: feed_seq with empty seq */
    tf_asserteq(feed_seq(&S, &key, "", 0), TF_NONE);
    tf_free(&S);
}

/* ─── Phase 11: waitkey ─── */

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>

static void test_waitkey_basic(void) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "\x1b[A", 3);
    close(pfd[1]);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
    close(pfd[0]);
    tf_free(&S);
}

static void test_waitkey_escape(void) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    /* lone ESC byte: partial sequence, reader drains → flush on timeout */
    write(pfd[1], "\x1b", 1);
    r = tf_waitkey(&S, pfd[0], 20, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);
    close(pfd[1]);
    close(pfd[0]);
    tf_free(&S);
}

static void test_waitkey_timeout_idle(void) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    r = tf_waitkey(&S, pfd[0], 10, &key);
    tf_asserteq(r, TF_AGAIN);
    close(pfd[1]);
    close(pfd[0]);
    tf_free(&S);
}

static void test_waitkey_eintr_stale(void) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "a", 1);
    close(pfd[1]);
    /* stale EINTR from a prior interrupted poll: poll succeeds (r > 0)
     * but errno still reads EINTR — must not skip the readable fd */
    errno = EINTR;
    r = tf_waitkey(&S, pfd[0], 500, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    close(pfd[0]);
    tf_free(&S);
}

static void test_waitkey_params(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    tf_asserteq(tf_waitkey(NULL, 0, 0, &key), TF_ERRPARAM);
    tf_asserteq(tf_waitkey(&S, -1, 0, &key), TF_ERRPARAM);
    tf_asserteq(tf_waitkey(&S, 0, 0, NULL), TF_ERRPARAM);

    /* invalid fd → read error → TF_ERRPARAM */
    tf_asserteq(tf_waitkey(&S, 9999, 0, &key), TF_ERRPARAM);

    /* tf_string NULL plen / S */
    tf_asserteq(tf_string(&S, NULL) == NULL, 1);
    tf_asserteq(tf_string(NULL, NULL) == NULL, 1);

    /* closed pipe → EOF → TF_NONE */
    {
        int      fds[2];
        tf_State S2;
        pipe(fds);
        close(fds[1]);
        tf_init(&S2, NULL, NULL);
        tf_asserteq(tf_waitkey(&S2, fds[0], 0, &key), TF_NONE);
        close(fds[0]);
        tf_free(&S2);
    }

    tf_free(&S);
}

/* trash_stack: overwrite the (freed) stack area of the previous waitkey
 * invocation, so its gone chunk buffer must not be read back */
static void trash_stack(void) {
    volatile char blob[8192];
    memset((void *)blob, 0xA5, sizeof(blob));
    blob[0] = blob[0];
}

static void test_waitkey_keep(void) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "ab", 2);
    close(pfd[1]);
    /* one read may deliver both bytes; the second must survive across
     * the waitkey call boundary */
    r = tf_waitkey(&S, pfd[0], 500, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    trash_stack();
    r = tf_waitkey(&S, pfd[0], 500, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'b');
    close(pfd[0]);
    tf_free(&S);
}

static void test_waitkey_oom(void) {
    tf_State S;
    tf_Key   key;
    int      cnt = 0, pfd[2], r;
    (void)pipe(pfd);
    write(pfd[1], "a", 1);
    close(pfd[1]);
    tf_init(&S, oom_alloc, &cnt);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    tf_asserteq(r, TF_ERRMEM);
    close(pfd[0]);
    tf_free(&S);
}

static void test_waitkey_infinite(void) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "a", 1);
    close(pfd[1]);
    /* timeout < 0: poll indefinitely, data already available */
    r = tf_waitkey(&S, pfd[0], -1, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');
    close(pfd[0]);
    tf_free(&S);
}

/* waitkey surplus bytes live in the parser's chunk (wait buffer); tf_feed
 * switches the source and discards the unconsumed chunk — the caller must
 * re-feed them, the new reader's data follows */
static void test_waitkey_feed_discard(void) {
    tf_State   S;
    tf_Key     key;
    MockReader mr;
    int        pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "ab", 2);
    close(pfd[1]);
    /* one read may deliver both bytes; only 'a' is returned */
    r = tf_waitkey(&S, pfd[0], 500, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'a');
    close(pfd[0]);
    /* switch to a reader: 'b' (waitkey surplus) is dropped by tf_feed */
    mr.data = "Z", mr.len = 1, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'Z');
    tf_free(&S);
}

/* the already-allocated wait buffer is reused across waitkey calls */
static void test_waitkey_reuse(void) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "a", 1);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'a');
    write(pfd[1], "b", 1);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'b');
    close(pfd[1]);
    close(pfd[0]);
    tf_free(&S);
}

static int  eintr_pipew = -1;
static void eintr_writebyte(int sig) {
    (void)sig;
    if (eintr_pipew >= 0) write(eintr_pipew, "x", 1);
}

/* poll interrupted by SIGALRM (EINTR) is retried; the handler's byte
 * is then read and parsed */
static void test_waitkey_poll_eintr(void) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    eintr_pipew = pfd[1];
    signal(SIGALRM, eintr_writebyte);
    alarm(1);
    r = tf_waitkey(&S, pfd[0], -1, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'x');
    alarm(0), signal(SIGALRM, SIG_DFL);
    eintr_pipew = -1;
    close(pfd[1]);
    close(pfd[0]);
    tf_free(&S);
}

/* a readkey error (CS buffer OOM) propagates out of waitkey */
static void test_waitkey_readkey_err(void) {
    tf_State S;
    tf_Key   key;
    int      cnt = 1, pfd[2], r;
    (void)pipe(pfd);
    write(pfd[1], "\x1b]hello", 7);
    close(pfd[1]);
    tf_init(&S, oom_alloc, &cnt);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    tf_asserteq(r, TF_ERRMEM);
    close(pfd[0]);
    tf_free(&S);
}
#else
static void test_waitkey_basic(void) {}
static void test_waitkey_timeout_idle(void) {}
static void test_waitkey_eintr_stale(void) {}
static void test_waitkey_infinite(void) {}
static void test_waitkey_keep(void) {}
static void test_waitkey_oom(void) {}
static void test_waitkey_feed_discard(void) {}
static void test_waitkey_reuse(void) {}
static void test_waitkey_poll_eintr(void) {}
static void test_waitkey_readkey_err(void) {}
static void test_waitkey_params(void) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    tf_asserteq(tf_waitkey(&S, 0, 0, &key), TF_ERRPARAM);
    tf_free(&S);
}
#endif

/* ─── Phase 11: mouse interpretation ─── */

static void test_mouse_press(void) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.btn = 0;
    key.d.mouse.line = 5;
    key.d.mouse.col = 10;

    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_PRESS);
    tf_asserteq(btn, 1);
    tf_asserteq(line, 5);
    tf_asserteq(col, 10);
}

static void test_mouse_drag(void) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.btn = 0x20; /* button 1 drag */

    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_DRAG);
    tf_asserteq(btn, 1);
}

static void test_mouse_release(void) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.btn = 3; /* release */

    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_RELEASE);
    tf_asserteq(btn, 0);
}

static void test_mouse_scroll(void) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;

    /* scroll up: code 64 → btn 4 */
    key.d.mouse.btn = 64;
    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_PRESS);
    tf_asserteq(btn, 4);

    /* scroll down: code 65 → btn 5 */
    key.d.mouse.btn = 65;
    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_PRESS);
    tf_asserteq(btn, 5);

    /* scroll left: code 66 → btn 6 */
    key.d.mouse.btn = 66;
    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_PRESS);
    tf_asserteq(btn, 6);

    /* scroll right: code 67 → btn 7 */
    key.d.mouse.btn = 67;
    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_PRESS);
    tf_asserteq(btn, 7);
}

static void test_mouse_extended(void) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;

    /* button 8: code 128 */
    key.d.mouse.btn = 128;
    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_PRESS);
    tf_asserteq(btn, 8);

    /* button 9: code 129 */
    key.d.mouse.btn = 129;
    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_PRESS);
    tf_asserteq(btn, 9);
}

static void test_mouse_unknown(void) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.btn = 200; /* unknown code, not in any range */

    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_UNKNOWN);
    tf_asserteq(btn, 0);
}

static void test_mouse_sgr_release(void) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.release = 1;
    key.d.mouse.btn = 0;

    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    tf_asserteq(ev, TF_EVENT_RELEASE);
    tf_asserteq(btn, 0);
}

static void test_mouse_params(void) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    /* non-MOUSE type → returns 0 */
    key.type = TF_TYPE_UNICODE;
    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_ERRPARAM);
    /* NULL pointers */
    tf_asserteq(tf_mouse(NULL, &ev, &btn, &line, &col), TF_ERRPARAM);
    key.type = TF_TYPE_MOUSE;
    tf_asserteq(tf_mouse(&key, NULL, &btn, &line, &col), TF_ERRPARAM);
    tf_asserteq(tf_mouse(&key, &ev, NULL, &line, &col), TF_ERRPARAM);
    tf_asserteq(tf_mouse(&key, &ev, &btn, NULL, &col), TF_ERRPARAM);
    tf_asserteq(tf_mouse(&key, &ev, &btn, &line, NULL), TF_ERRPARAM);
}

/* ─── Phase 11: position / modereport ─── */

static void test_position_basic(void) {
    tf_Key key;
    int    line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_POSITION;
    key.d.pos.line = 10;
    key.d.pos.col = 25;

    tf_asserteq(tf_position(&key, &line, &col), TF_OK);
    tf_asserteq(line, 10);
    tf_asserteq(col, 25);
}

static void test_position_params(void) {
    tf_Key key;
    int    line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_UNICODE;
    tf_asserteq(tf_position(&key, &line, &col), TF_ERRPARAM);
    tf_asserteq(tf_position(NULL, &line, &col), TF_ERRPARAM);
    key.type = TF_TYPE_POSITION;
    tf_asserteq(tf_position(&key, NULL, &col), TF_ERRPARAM);
}

static void test_modereport_basic(void) {
    tf_Key key;
    int    init, mode, val;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MODEREPORT;
    key.d.modereport.initial = '?';
    key.d.modereport.mode = 1;
    key.d.modereport.value = 2;

    tf_asserteq(tf_modereport(&key, &init, &mode, &val), TF_OK);
    tf_asserteq(init, '?');
    tf_asserteq(mode, 1);
    tf_asserteq(val, 2);
}

static void test_modereport_params(void) {
    tf_Key key;
    int    init, mode, val;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_UNICODE;
    tf_asserteq(tf_modereport(&key, &init, &mode, &val), TF_ERRPARAM);
    tf_asserteq(tf_modereport(NULL, &init, &mode, &val), TF_ERRPARAM);
    key.type = TF_TYPE_MODEREPORT;
    tf_asserteq(tf_modereport(&key, NULL, &mode, &val), TF_ERRPARAM);
}

/* ─── Phase 11: tf_csi ─── */

static void test_csi_basic(void) {
    tf_State S;
    tf_Key   key;
    int      args[16];
    int      na;
    int      cmd, r;
    tf_init(&S, NULL, NULL);

    /* feed UNKNOWN_CSI: \x1b[1;2;3x */
    tf_asserteq(feed_seq(&S, &key, "\x1b[1;2;3x", 8), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    tf_asserteq(r, 3); /* returns field count */
    tf_asserteq(cmd, 'x');
    tf_asserteq(args[0], 1);
    tf_asserteq(args[1], 2);
    tf_asserteq(args[2], 3);
    tf_free(&S);
}

/* initial+final without parameters must yield 0 args (final is not a
 * parameter field) — \x1b[?c DA1, \x1b[?u kitty, \x1b[?1$y DEC */
static void test_csi_noparams(void) {
    tf_State S;
    tf_Key   key;
    int      args[8], n, cmd;
    tf_init(&S, NULL, NULL);

    tf_asserteq(feed_seq(&S, &key, "\x1b[?c", 4), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    n = tf_csi(&S, args, 8, &cmd);
    tf_asserteq(n, 0); /* pre-fix: 1 (final 'c' counted) */
    tf_asserteq(cmd, 'c' | ('?' << 8));

    /* with a real parameter: count unaffected */
    tf_asserteq(feed_seq(&S, &key, "\x1b[?1c", 5), TF_OK);
    n = tf_csi(&S, args, 8, &cmd);
    tf_asserteq(n, 1);
    tf_asserteq(args[0], 1);

    /* SGR mouse: trailing final not part of any field */
    tf_asserteq(feed_seq(&S, &key, "\x1b[<0;6;7M", 9), TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_asserteq(key.d.mouse.btn, 0);
    tf_asserteq(key.d.mouse.col, 6);
    tf_asserteq(key.d.mouse.line, 7);
    tf_free(&S);
}

static void test_csi_empty_params(void) {
    tf_State S;
    tf_Key   key;
    int      args[16];
    int      na;
    int      cmd;
    tf_init(&S, NULL, NULL);

    /* \x1b[;2;x → empty first field → -1; final 'x' is not a field */
    tf_asserteq(feed_seq(&S, &key, "\x1b[;2;x", 6), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    na = 16;
    tf_asserteq(tf_csi(&S, args, na, &cmd), 2);
    tf_asserteq(cmd, 'x');
    tf_asserteq(args[0], -1);
    tf_asserteq(args[1], 2);
    tf_free(&S);
}

static void test_csi_truncate(void) {
    tf_State S;
    tf_Key   key;
    int      args[3];
    int      na;
    int      cmd;
    tf_init(&S, NULL, NULL);

    /* \x1b[1;2;3;4x → 4 fields, na=3 → truncated to 3 */
    tf_asserteq(feed_seq(&S, &key, "\x1b[1;2;3;4x", 10), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    na = 3;
    tf_asserteq(tf_csi(&S, args, na, &cmd), 3);
    tf_asserteq(cmd, 'x');
    tf_asserteq(args[0], 1);
    tf_asserteq(args[1], 2);
    tf_asserteq(args[2], 3);
    tf_free(&S);
}

static void test_csiparse_params(void) {
    tf_State S;
    tf_Key   key;
    int      args[16];
    int      na;
    int      cmd, r;
    tf_init(&S, NULL, NULL);
    /* no valid cmd → TF_ERRPARAM */
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    tf_asserteq(r, TF_ERRPARAM);

    /* NULL checks */
    tf_asserteq(tf_csi(NULL, args, na, &cmd), TF_ERRPARAM);
    tf_asserteq(tf_csi(&S, NULL, na, &cmd), TF_ERRPARAM);
    tf_asserteq(tf_csi(&S, args, na, NULL), TF_ERRPARAM);

    /* IDLE state buf empty */
    tf_init(&S, NULL, NULL);
    /* set cmd manually via UNKNOWN_CSI with empty buf */
    feed_seq(&S, &key, "\x1b[x", 3);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    tf_asserteq(r, 0); /* final 'x' is not a parameter field */
    tf_asserteq(cmd, 'x');

    /* buf with params but no final (partial CSI) → no snapshot */
    tf_init(&S, NULL, NULL);
    S.buf_len = 2;
    S.buf[0] = '1';
    S.buf[1] = ';';
    r = tf_csi(&S, args, na, &cmd);
    tf_asserteq(r, TF_ERRPARAM);

    /* non-digit char inside a param field is skipped */
    feed_seq(&S, &key, "\x1b[1<x", 5);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    tf_asserteq(r, 1);
    tf_asserteq(cmd, 'x');
    tf_asserteq(args[0], 1);

    tf_free(&S);
}

static void test_csi_with_sub(void) {
    tf_State S;
    tf_Key   key;
    int      args[16];
    int      na;
    int      cmd;
    tf_init(&S, NULL, NULL);

    /* \x1b[1:2;3x → field 1 has sub-params, tf_csi only returns 1,3 */
    tf_asserteq(feed_seq(&S, &key, "\x1b[1:2;3x", 8), TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    na = 16;
    tf_asserteq(tf_csi(&S, args, na, &cmd), 2);
    tf_asserteq(cmd, 'x');
    tf_asserteq(args[0], 1);
    tf_asserteq(args[1], 3);
    tf_free(&S);
}

/* --- Phase 12a: tfK_writecp multi-byte UTF-8 + replacement --- */

static void test_writecp_utf8(void) {
    char buf[32];
    int  n;

    /* 2-byte: U+00FF */
    n = tfK_writecp(buf, sizeof(buf), 0xFF);
    tf_asserteq(n, 2);
    tf_assertstreq(buf, "\xc3\xbf");

    /* 3-byte: U+0800 */
    n = tfK_writecp(buf, sizeof(buf), 0x800);
    tf_asserteq(n, 3);
    tf_assertstreq(buf, "\xe0\xa0\x80");

    /* 4-byte: U+10000 */
    n = tfK_writecp(buf, sizeof(buf), 0x10000);
    tf_asserteq(n, 4);
    tf_assertstreq(buf, "\xf0\x90\x80\x80");

    /* >0x200000 → 0xFFFD replacement */
    n = tfK_writecp(buf, sizeof(buf), 0x200000);
    tf_asserteq(n, 3);
    tf_assertstreq(buf, "\xef\xbf\xbd");

    /* 0xFFFD → direct encoding */
    n = tfK_writecp(buf, sizeof(buf), 0xFFFD);
    tf_asserteq(n, 3);
    tf_assertstreq(buf, "\xef\xbf\xbd");
}

/* --- Phase 12b: tfK_writemods LONGMOD paths --- */

static void test_writemods_long(void) {
    char buf[64];
    int  n;

    /* LONGMOD + SHIFT: "Shift-" prefix */
    n = tfK_writemods(buf, sizeof(buf), TF_MOD_SHIFT, TF_FMT_LONGMOD);
    tf_asserteq(n, 6);
    tf_assertstreq(buf, "Shift-");

    /* LONGMOD + ALT (not ALTISMETA): "Alt-" prefix */
    n = tfK_writemods(buf, sizeof(buf), TF_MOD_ALT, TF_FMT_LONGMOD);
    tf_asserteq(n, 4);
    tf_assertstreq(buf, "Alt-");

    /* LONGMOD + CTRL: "Control-" prefix */
    n = tfK_writemods(buf, sizeof(buf), TF_MOD_CTRL, TF_FMT_LONGMOD);
    tf_asserteq(n, 8);
    tf_assertstreq(buf, "Control-");
}

/* LOWERMOD: long modifier names in lowercase */
static void test_format_lowermod(void) {
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_KEYSYM, key.d.sym = TF_SYM_LEFT;
    key.modifiers = TF_MOD_CTRL | TF_MOD_SHIFT | TF_MOD_ALT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_LONGMOD | TF_FMT_LOWERMOD);
    tf_assertstreq(buf, "shift-alt-control-Left");
    /* short forms lowercased too (libtermkey modprefix semantics) */
    key.type = TF_TYPE_UNICODE, key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERMOD);
    tf_assertstreq(buf, "c-x");
}

/* --- Phase 12c: tf_format without WRAPBRACKET --- */

static void test_format_nobracket(void) {
    tf_Key key;
    char   buf[64];
    int    n;
    memset(&key, 0, sizeof(key));

    /* UNICODE + CTRL, no bracket, no SPACEMOD: C-x */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    n = tf_format(buf, sizeof(buf), &key, 0);
    /* default: WRAPBRACKET, no SPACEMOD → "<C-x>" */
    tf_assertstreq(buf, "<C-x>");

    /* UNICODE + CTRL, no WRAPBRACKET: C-x (bare) */
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    tf_asserteq(n, 3);
    tf_assertstreq(buf, "C-x");

    /* UNICODE + SHIFT+ALT+CTRL, no WRAPBRACKET: S-A-C-x */
    key.modifiers = TF_MOD_SHIFT | TF_MOD_ALT | TF_MOD_CTRL;
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    tf_assertstreq(buf, "S-A-C-x");

    /* UNICODE no modifiers, no WRAPBRACKET: bare 'x' */
    key.modifiers = 0;
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    tf_asserteq(n, 1);
    tf_assertstreq(buf, "x");

    /* FUNCTION with modifier, no WRAPBRACKET: S-F1 */
    key.type = TF_TYPE_FUNCTION;
    key.d.number = 1;
    key.modifiers = TF_MOD_SHIFT;
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    tf_assertstreq(buf, "S-F1");
}

/* --- Phase 12d: tf_format SPACEMOD without WRAPBRACKET --- */

static void test_format_spacesep(void) {
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));

    /* SPACEMOD without WRAPBRACKET: "C x" with trailing space */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_SPACEMOD | TF_FMT_LOWERSPACE);
    tf_assertstreq(buf, "C x");

    /* UNICODE: CARETCTRL without WRAPBRACKET */
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_CARETCTRL | TF_FMT_LOWERSPACE);
    tf_assertstreq(buf, "^X");

    /* UNICODE + CTRL lowercase, CARETCTRL uppers it */
    key.d.codepoint = 'a';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_CARETCTRL | TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<^A>");
}

/* --- Phase 12e: tfK_parsemod A-/D-/Alt-/Meta- forms --- */

static void test_parsemod_altmeta(void) {
    tf_Key key;
    int    n;

    /* <A-x> → ALT + 'x' */
    n = tf_parse("<A-x>", &key);
    tf_asserteq(n, 5);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    /* <D-x> → SUPER + 'x' */
    n = tf_parse("<D-x>", &key);
    tf_asserteq(n, 5);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_SUPER);

    /* <Alt-x> → ALT + 'x' (long form, lowercase) */
    n = tf_parse("<Alt-x>", &key);
    tf_asserteq(n, 7);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_ALT);

    /* <Meta-x> → ALT + 'x' (long form, "Me" prefix) */
    n = tf_parse("<Meta-x>", &key);
    tf_asserteq(n, 8);
    tf_asserteq(key.d.codepoint, 'x');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
}

/* --- Phase 12f: tfU_decode 5/6 byte + edge cases --- */

/* Note: test_utf8_456 already exists, different test from Phase 5
 * This version adds 5/6 byte coverage. Merging into test_utf8_456 above. */

/* --- Phase 12g: tf_readkey REPLAY / REPLAY_BUF idle transitions --- */

static void test_readkey_replay(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* REPLAY_BUF with state=0 is IDLE: no replay, main loop reads reader */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_NONE);
    tf_asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* --- Phase 12h: replay with buf_len transition --- */

static void test_replay_bufmerge(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* Setup replay source: "A" at buf tail */
    S.replay = 1;
    S.buf[TF_MAX_BUFLEN - 1] = 'A';
    S.buf_len = 0;
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'A');
    tf_asserteq(S.state, TF_STATE_IDLE);

    /* Setup: replay source of 3 bytes */
    S.replay = 3;
    S.buf[TF_MAX_BUFLEN - 3] = 'X';
    S.buf[TF_MAX_BUFLEN - 2] = 'Y';
    S.buf[TF_MAX_BUFLEN - 1] = 'Z';
    S.buf_len = 0;
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'X');
    tf_asserteq(S.replay, 2);

    /* consume second byte */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'Y');
    tf_asserteq(S.replay, 1);

    /* consume third byte → IDLE */
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'Z');
    tf_asserteq(S.state, TF_STATE_IDLE);
    tf_asserteq(S.buf_len, 0);
    tf_free(&S);
}

static void test_feed_replay_clear(void) {
    tf_State   S;
    tf_Key     key;
    MockReader mr;
    char       over[67];
    int        r, i;
    tf_init(&S, NULL, NULL);
    memset(over, '1', sizeof(over));
    over[0] = 0x1b, over[1] = '[';
    over[66] = 'A'; /* chunk tail: must be dropped by tf_feed */
    /* \x1b[ + 64×'1' overflows the CSI param buffer → ALT+[ + REPLAY */
    r = feed_seq(&S, &key, over, 67);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '[');
    tf_asserteq(key.modifiers, TF_MOD_ALT);
    tf_assert(S.replay > 0);
    /* new reader mid-replay: old chunk remainder must be discarded */
    mr.data = "B", mr.len = 1, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    for (i = 0; i < 64; ++i) {
        r = tf_readkey(&S, &key);
        tf_asserteq(r, TF_OK);
        tf_asserteq(key.type, TF_TYPE_UNICODE);
    }
    r = tf_readkey(&S, &key);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.d.codepoint, 'B'); /* old 'A' would leak here */
    tf_free(&S);
}

/* --- Phase 12i: tf_parse bracket char name advance s++ --- */

static void test_parse_bracketadv(void) {
    tf_Key key;
    int    n;

    /* <Left> → s++ on match (covers L2065) */
    n = tf_parse("<Left>", &key);
    tf_asserteq(n, 6);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_LEFT);

    /* <Down> → s++ on match */
    n = tf_parse("<Down>", &key);
    tf_asserteq(n, 6);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DOWN);

    /* <Right> */
    n = tf_parse("<Right>", &key);
    tf_asserteq(n, 7);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_RIGHT);

    /* <Up> */
    n = tf_parse("<Up>", &key);
    tf_asserteq(n, 4);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_UP);
}

/* <F N> with space inside the name: spaces are stripped */
static void test_parse_fspace(void) {
    tf_Key key;
    int    n;
    n = tf_parse("<F 1>", &key);
    tf_assert(n > 0); /* pre-fix: -1 */
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 1);
    n = tf_parse("<f 12>", &key);
    tf_assert(n > 0);
    tf_asserteq(key.d.number, 12);
    /* no digits after F: single char falls back to codepoint, space fails */
    n = tf_parse("<F>", &key);
    tf_asserteq(n, 3);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'F');
    tf_asserteq(tf_parse("<F >", &key), -1);
}

/* --- Phase 12j: trie slot left expansion --- */

static void test_trie_leftexpand(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {
            {"key_clear",
             "\x1b"
             "Z"}, /* inserted first → arr min='Z' */
            {"key_end",
             "\x1b"
             "A"}, /* later → left expand */
            {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* Verify both can be matched */
    r = feed_seq(
            &S, &key,
            "\x1b"
            "A",
            2);
    tf_asserteq(r, TF_OK);

    r = feed_seq(
            &S, &key,
            "\x1b"
            "Z",
            2);
    tf_asserteq(r, TF_OK);

    tf_free(&S);
}

/* --- Phase 12k: cs_buf realloc (tfB_append expansion) --- */

static void test_cs_bytexpand(void) {
    tf_State S;
    tf_Key   key;
    int      r, i, len;
    char     buf[256];
    tf_init(&S, NULL, NULL);

    /* enter CS OSC, then feed many bytes to trigger realloc */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    tf_asserteq(r, TF_AGAIN);

    /* feed enough to fill initial 64 bytes + overflow to trigger realloc */
    for (i = 0; i < 200; i++) buf[i] = (unsigned char)('a' + (i % 26));
    r = feed_seq(&S, &key, buf, 200);
    /* should trigger realloc */
    tf_asserteq(r, TF_AGAIN);

    /* terminate */
    r = feed_byte(&S, &key, 0x07);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_OSC);
    tf_assert(tf_string(&S, &len) != NULL);
    tf_asserteq(len, 200);
    tf_free(&S);
}

/* --- Phase 12l: cs_buf realloc OOM (free path) --- */

static void test_cs_oomfree(void) {
    tf_State S;
    tf_Key   key;
    int      r, i;
    char     buf[128];

    /* default allocator: realloc fails → free cs_buf */
    /* We use oom_alloc to simulate failure after initial allocation */
    {
        int ec = 1; /* allow first alloc (64), fail realloc */
        tf_init(&S, oom_alloc, &ec);
        r = feed_byte(&S, &key, 0x1b);
        tf_asserteq(r, TF_AGAIN);
        r = feed_byte(&S, &key, ']');
        tf_asserteq(r, TF_AGAIN);
        /* first byte fills initial 64 → AG=AGAIN */
        for (i = 0; i < 64; i++) buf[i] = 'x';
        r = feed_seq(&S, &key, buf, 64);
        tf_asserteq(r, TF_AGAIN);
        /* next byte triggers realloc which fails with oom_alloc */
        r = feed_byte(&S, &key, 'y');
        tf_asserteq(r, TF_ERRMEM);
        tf_asserteq(S.state, TF_STATE_IDLE);
        tf_free(&S);
    }
}

/* --- Phase 12m: tfC_kitty text field + non-press events --- */

static void test_kitty_textfield(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    /* kitty CSI with text field: 97;5:2;65u → cp=97, ev=REPEAT, text='A' */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[97;5:2;65u", 12);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.event, TF_EVENT_REPEAT);
    tf_asserteq(key.d.codepoint, 97);
    tf_free(&S);

    /* kitty release: 97;5:3u → cp=97, ev=RELEASE */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[97;5:3u", 9);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.event, TF_EVENT_RELEASE);
    tf_free(&S);

    /* kitty with empty text field: field 3 empty (97;5u) covered in kitty_alts
     */
}

/* --- Phase 12n: tfD_cs default:break in terminator --- */

static void test_cs_terminate(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* enter CS: ESC+P (DCS) */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'P');
    tf_asserteq(r, TF_AGAIN);
    tf_asserteq(S.state, TF_STATE_CS_DCS);

    /* terminate with BEL */
    r = feed_byte(&S, &key, 0x07);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_DCS);
    tf_free(&S);

    /* DCS term by ST (ESC \) */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1bP\x1b\\", 4);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_DCS);
    tf_free(&S);

    /* APC (ESC _) terminate with BEL */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b_\x07", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_APC);
    tf_free(&S);
}

/* --- Phase 12o: tfC_kitty numeric text + empty text --- */

static void test_kitty_emptytext(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* kitty with empty text → empty cs_buf */
    r = feed_seq(&S, &key, "\x1b[?77429u", 9);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KITTYREPORT);
    tf_free(&S);
}

static void test_kitty_pua_utf8(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* plain UTF-8 PUA (kitty level 4 / raw text): 57399 → KP0 keysym */
    r = feed_seq(&S, &key, "\xEE\x80\xB7", 3); /* U+E017 = 57399 */
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KP0);

    /* 57415 → KPEquals (U+E047) */
    r = feed_seq(&S, &key, "\xEE\x81\x87", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_KPEQUALS);

    /* 57364 → F1 function (U+E014) */
    r = feed_seq(&S, &key, "\xEE\x80\x94", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 1);

    /* non-PUA codepoint untouched */
    r = feed_seq(&S, &key, "a", 1);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'a');

    tf_free(&S);
}

/* --- Phase 12p: tfM_dispatch uncovered path --- */

static void test_mouse_dispatch(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* CSI M without enough args → enters MOUSE_X10 state */
    /* CSI M is used for classic x10 mouse */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'M');
    tf_asserteq(r, TF_AGAIN);
    /* Now S.state should be MOUSE_X10 */
    tf_asserteq(S.state, TF_STATE_MOUSE_X10);
    tf_free(&S);
}

/* --- Phase 12q: tfC_cursorkey default --- */

static void test_cursorkey_default(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* CSI H (CU) with no modifiers */
    r = feed_seq(&S, &key, "\x1b[H", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_HOME);
    tf_free(&S);
}

/* --- Phase 12r: CSI with byte >= 0x80 in tfD_csi --- */

static void test_csi_utf8byte(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    /* enter CSI */
    r = feed_byte(&S, &key, 0x1b);
    tf_asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    tf_asserteq(r, TF_AGAIN);

    /* feed a byte >= 0x80 in CSI → transitions to UTF8 or triggers flush */
    r = feed_byte(&S, &key, 0xC0); /* non-ASCII byte in CSI */
    (void)r;                       /* coverage only */
    tf_free(&S);
}

/* --- Phase 12s: tfD_canon Ctrl+space and others --- */

static void test_canon_ctrl(void) {
    tf_Key key;

    /* Ctrl+Space (0x00) → KEYSYM SPACE + CTRL */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 0x00, 0);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* Ctrl+? (0x7F = DEL) → KEYSYM DEL */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 0x7F, 0);
    tf_asserteq(key.type, TF_TYPE_KEYSYM);
    tf_asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    /* Printable: 'A' */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 'A', 0);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, 'A');

    /* nointerpret: Ctrl+C (0x03) → code = 0x43='C'+ctrl */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 0x03, 0);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x1c-0x1f range: 0x1c → '\' + ctrl */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 0x1c, 0);
    tf_asserteq(key.type, TF_TYPE_UNICODE);
    tf_asserteq(key.d.codepoint, '\\');
    tf_asserteq(key.modifiers, TF_MOD_CTRL);
}

/* --- Phase 12t: tfC_nextarg iterator branches --- */

static void test_nextarg_edge(void) {
    tf_State    S;
    tf_Key      key;
    int         r, len;
    const char *f;

    tf_init(&S, NULL, NULL);
    /* feed unknown CSI with sub-params */
    r = feed_seq(&S, &key, "\x1b[1;2:3x", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    /* fields: "1", "2:3" (final not included), then none */
    f = NULL;
    tf_assert(tfC_nextarg(&S, &f, &len));
    tf_asserteq(len, 1);
    tf_assert(tfC_nextarg(&S, &f, &len));
    tf_asserteq(len, 3);
    tf_assert(!tfC_nextarg(&S, &f, &len));

    /* subval: present / absent / empty sub */
    tf_asserteq(tfC_subval("2:3x", 4, -1), 3);
    tf_asserteq(tfC_subval("2x", 2, -1), -1);
    tf_asserteq(tfC_subval("2:x", 3, -1), -1); /* empty sub → dflt */

    /* empty first field: [;1x */
    r = feed_seq(&S, &key, "\x1b[;1x", 5);
    tf_asserteq(r, TF_OK);
    f = NULL;
    tf_assert(tfC_nextarg(&S, &f, &len));
    tf_asserteq(len, 0); /* empty field */
    tf_assert(tfC_nextarg(&S, &f, &len));
    tf_asserteq(len, 1); /* "1" */

    /* intermediate byte truncates: "$" */
    r = feed_seq(&S, &key, "\x1b[1$y", 5);
    tf_asserteq(r, TF_OK);
    f = NULL;
    tf_assert(tfC_nextarg(&S, &f, &len));
    tf_asserteq(len, 1);
    tf_assert(!tfC_nextarg(&S, &f, &len));

    /* intermediate right after ';': field 2 absent (not empty) */
    r = feed_seq(&S, &key, "\x1b[1;$y", 6);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MODEREPORT);
    tf_asserteq(key.d.modereport.mode, 1);
    tf_asserteq(key.d.modereport.value, -1);
    f = NULL;
    tf_assert(tfC_nextarg(&S, &f, &len));
    tf_asserteq(len, 1);
    tf_assert(!tfC_nextarg(&S, &f, &len));

    /* initial '?' skipped */
    r = feed_seq(&S, &key, "\x1b[?25x", 6);
    tf_asserteq(r, TF_OK);
    f = NULL;
    tf_assert(tfC_nextarg(&S, &f, &len));
    tf_asserteq(len, 2); /* "25" */
    tf_asserteq(tfC_fieldval(f, len, 0), 25);

    /* control byte in field: neither intermediate nor digit */
    r = feed_seq(&S, &key, "\x1b[\x01x", 4);
    tf_asserteq(r, TF_OK);
    f = NULL;
    tf_assert(tfC_nextarg(&S, &f, &len));
    tf_asserteq(tfC_fieldval(f, len, 0), 0); /* no digits → dflt */

    /* intermediate first byte: field NULL → handler dflt paths */
    r = feed_seq(&S, &key, "\x1b[$y", 5); /* modereport, no params */
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MODEREPORT);
    tf_asserteq(key.d.modereport.mode, -1);
    tf_asserteq(key.d.modereport.value, -1);
    r = feed_seq(&S, &key, "\x1b[$~", 5); /* funckey, no field 1 */
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    r = feed_seq(&S, &key, "\x1b[$u", 5); /* kitty, no field 1 */
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    r = feed_seq(&S, &key, "\x1b[?5R", 6); /* CPR, no field 2 */
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_POSITION);
    tf_asserteq(key.d.pos.line, 5);
    tf_asserteq(key.d.pos.col, 0);

    /* empty params → no fields */
    S.buf_len = 0;
    f = NULL;
    tf_assert(!tfC_nextarg(&S, &f, &len));

    tf_free(&S);
}

/* --- Phase 12u: tfC_fkeynum uncovered branches --- */

static void test_fkeynum_edge(void) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    /* F10 → feed CSI 10~ */
    r = feed_seq(&S, &key, "\x1b[10~", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 10);
    tf_free(&S);

    /* F11 → CSI 23~ */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[23~", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 11);
    tf_free(&S);

    /* CSI 11;11~ → n=11, fkeynum returns 1 (F1) */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[11;11~", 8);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 1); /* 11 maps to F1 */
    tf_free(&S);

    /* F12 → CSI 24~ */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[24~", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 12);
    tf_free(&S);
}

/* --- Phase 12v: tfC_arg / tfC_sub uncovered branches --- */

static void test_csiarg_edge(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    int      args[16];
    int      na;
    int      cmd;

    tf_init(&S, NULL, NULL);
    /* CSI with empty first field: [;1x */
    r = feed_seq(&S, &key, "\x1b[;1x", 5);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    tf_asserteq(r, 2);
    tf_asserteq(cmd, 'x');
    tf_asserteq(args[0], -1); /* empty field → default -1 */
    tf_free(&S);

    /* CSI with sub-params: get raw sub */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[1:2;3x", 8);
    tf_asserteq(r, TF_OK);
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    tf_asserteq(r, 2);
    tf_asserteq(cmd, 'x');
    /* sub-params stripped by tf_csi */
    tf_asserteq(args[0], 1);
    tf_asserteq(args[1], 3);
    tf_free(&S);
}

/* --- Phase 12w: tfD_ss3dispatch uncovered branches --- */

static void test_ss3_edge(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* SS3 O P → no default mapping (depends on trie) */
    r = feed_seq(&S, &key, "\x1bOP", 3);
    tf_asserteq(r, TF_OK);
    /* This should try SS3 lookup (ESC O → SS3 state) */
    tf_free(&S);

    /* SS3 with lookup table */
    tf_init(&S, NULL, NULL);
    {
        TILookup tbl[] = {{"key_f3", "\x1bOR"}, {NULL, NULL}};
        tf_setlookup(&S, ti_lookup, tbl);
    }
    r = feed_seq(&S, &key, "\x1bOR", 3);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_FUNCTION);
    tf_asserteq(key.d.number, 3);
    tf_free(&S);

    /* SS3 unknown: ESC O z → should not match any entry */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1bOz", 3);
    tf_asserteq(r, TF_OK);
    /* Should produce something (idle with some default handling) */
    tf_free(&S);
}

/* --- Phase 12x: tfD_escape edge cases --- */

static void test_escape_edge(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* ESC # (DEC) */
    r = feed_seq(&S, &key, "\x1b#", 2);
    tf_asserteq(r, TF_OK);

    /* ESC % (declare charset) */
    r = feed_seq(&S, &key, "\x1b%", 2);
    tf_asserteq(r, TF_OK);

    /* ESC space (SP) */
    r = feed_seq(&S, &key, "\x1b ", 2);
    tf_asserteq(r, TF_OK);

    tf_free(&S);
}

/* --- Phase 12y: tfT_slot right expansion uncovered --- */

static void test_trie_rightexpand(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {
            {"key_home",
             "\x1b"
             "L"}, /* inserted first → arr min='L' */
            {"key_end",
             "\x1b"
             "Z"}, /* later → right expand */
            {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_setlookup(&S, ti_lookup, tbl);

    /* Verify both can be matched */
    r = feed_seq(
            &S, &key,
            "\x1b"
            "L",
            2);
    tf_asserteq(r, TF_OK);
    r = feed_seq(
            &S, &key,
            "\x1b"
            "Z",
            2);
    tf_asserteq(r, TF_OK);
    tf_free(&S);
}

/* --- Phase 12z: tf_format no-bracket bare name (modpos==0) --- */

static void test_format_barename(void) {
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));

    /* KEYSYM no modifiers, no WRAPBRACKET → just name */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_ESCAPE;
    tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    tf_assertstreq(buf, "escape");
}

static void test_format_none(void) {
    /* memset key (type = TF_TYPE_NONE): not treated as UNICODE,
     * formats as empty brackets instead of garbage */
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));
    tf_format(buf, sizeof(buf), &key, 0);
    tf_assertstreq(buf, "<>");
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    tf_assertstreq(buf, "<>");
}

/* --- Phase 12aa: tfM_dispatch CSI M path --- */

static void test_mouse_x10_raw(void) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* CSI M dispatch + raw bytes → covers tfM_dispatch return 0 path */
    /* Feed CSI M followed by 3 raw bytes in one chunk */
    r = feed_seq(&S, &key, "\x1b[M\x20\x21\x22", 6);
    tf_asserteq(r, TF_OK);
    tf_asserteq(key.type, TF_TYPE_MOUSE);
    tf_free(&S);
}

#define TESTS(X)             \
    X(lifecycle)             \
    X(flags)                 \
    X(feed_params)           \
    X(flush_params)          \
    X(idle_printable)        \
    X(idle_c0_default)       \
    X(idle_keepc0)           \
    X(idle_none)             \
    X(escape_basic)          \
    X(escape_alt)            \
    X(escape_csi_ss3_cs)     \
    X(escape_chunk)          \
    X(flush_escape)          \
    X(flush_escape_alt)      \
    X(flush_csi_stub)        \
    X(csi_cursor)            \
    X(csi_modifiers)         \
    X(csi_shifttab)          \
    X(csi_funckey)           \
    X(csi_event)             \
    X(csi_position)          \
    X(csi_kitty)             \
    X(csi_modereport)        \
    X(csi_params)            \
    X(csi_unknown)           \
    X(ss3_cursor)            \
    X(ss3_func)              \
    X(ss3_kp)                \
    X(ss3_convertkp)         \
    X(ss3_unknown)           \
    X(ss3_unknownprefix)     \
    X(mouse_x10)             \
    X(mouse_x10_chunk)       \
    X(mouse_x10_alt)         \
    X(mouse_x10_chunkalt)    \
    X(mouse_rxvt)            \
    X(mouse_sgr)             \
    X(cs_osc)                \
    X(cs_st)                 \
    X(cs_dcs_apc)            \
    X(cs_cross)              \
    X(cs_alt)                \
    X(cs_oom)                \
    X(cs_params)             \
    X(utf8_decode)           \
    X(utf8_chunk)            \
    X(utf8_invalid)          \
    X(utf8_alt)              \
    X(utf8_params)           \
    X(utf8_456)              \
    X(cs_expand)             \
    X(cs_allocfree)          \
    X(trie_match)            \
    X(trie_singlebyte)       \
    X(trie_fallback)         \
    X(trie_aftermatch)       \
    X(trie_nolookup)         \
    X(trie_idle_inactive)    \
    X(trie_shifted)          \
    X(trie_fkeybreak)        \
    X(trie_oom)              \
    X(trie_setlookup_params) \
    X(setlookup_oom)         \
    X(trie_withalt)          \
    X(trie_multikey)         \
    X(trie_expand)           \
    X(trie_alloc)            \
    X(trie_allocfree)        \
    X(trie_reload)           \
    X(trie_prefix)           \
    X(flush_csi_full)        \
    X(replay_escape)         \
    X(flush_ss3)             \
    X(flush_cs)              \
    X(flush_utf8)            \
    X(flush_mousex10)        \
    X(flush_mousex10_raw)    \
    X(flush_replay)          \
    X(flush_escapealt_csi)   \
    X(replay_seq)            \
    X(canon_delbs)           \
    X(canon_spacesym)        \
    X(canon_convertkp)       \
    X(symname_basic)         \
    X(symname_editing)       \
    X(symname_terminfo)      \
    X(symname_kp)            \
    X(symname_oob)           \
    X(sym_lookup)            \
    X(sym_roundtrip)         \
    X(fmt_basic)             \
    X(fmt_mods)              \
    X(fmt_spacing)           \
    X(fmt_kitty)             \
    X(fmt_mouse)             \
    X(fmt_types)             \
    X(fmt_trunc)             \
    X(fmt_params)            \
    X(parse_basic)           \
    X(parse_mods)            \
    X(parse_case)            \
    X(parse_caret)           \
    X(parse_kitty)           \
    X(parse_nobracket)       \
    X(parse_fail)            \
    X(parse_longmod)         \
    X(parse_roundtrip)       \
    X(parse_bracketchar)     \
    X(kitty_basic)           \
    X(kitty_modifiers)       \
    X(kitty_event)           \
    X(kitty_event_fail)      \
    X(kitty_alts)            \
    X(kitty_text)            \
    X(kitty_text_multi)      \
    X(kitty_func)            \
    X(kitty_map)             \
    X(kitty_empty_cp)        \
    X(ss3_altreplay)         \
    X(csi_altprefix)         \
    X(kitty_params)          \
    X(kitty_widecp)          \
    X(kitty_text_long)       \
    X(kitty_text_oom)        \
    X(csi_overflow)          \
    X(waitkey_params)        \
    X(waitkey_eintr_stale)   \
    X(waitkey_basic)         \
    X(waitkey_escape)        \
    X(waitkey_timeout_idle)  \
    X(waitkey_keep)          \
    X(waitkey_oom)           \
    X(waitkey_infinite)      \
    X(waitkey_feed_discard)  \
    X(waitkey_reuse)         \
    X(waitkey_poll_eintr)    \
    X(waitkey_readkey_err)   \
    X(mouse_press)           \
    X(mouse_drag)            \
    X(mouse_release)         \
    X(mouse_scroll)          \
    X(mouse_extended)        \
    X(mouse_unknown)         \
    X(mouse_sgr_release)     \
    X(mouse_params)          \
    X(position_basic)        \
    X(position_params)       \
    X(modereport_params)     \
    X(modereport_basic)      \
    X(modereport_params)     \
    X(csi_basic)             \
    X(csi_noparams)          \
    X(csi_empty_params)      \
    X(csi_truncate)          \
    X(csiparse_params)       \
    X(csi_with_sub)          \
    X(writecp_utf8)          \
    X(writemods_long)        \
    X(format_lowermod)       \
    X(format_nobracket)      \
    X(format_spacesep)       \
    X(parsemod_altmeta)      \
    X(readkey_replay)        \
    X(replay_bufmerge)       \
    X(feed_replay_clear)     \
    X(parse_bracketadv)      \
    X(parse_fspace)          \
    X(trie_leftexpand)       \
    X(cs_bytexpand)          \
    X(cs_oomfree)            \
    X(cs_nul)                \
    X(kitty_textfield)       \
    X(cs_terminate)          \
    X(kitty_emptytext)       \
    X(kitty_pua_utf8)        \
    X(mouse_dispatch)        \
    X(cursorkey_default)     \
    X(csi_utf8byte)          \
    X(canon_ctrl)            \
    X(nextarg_edge)          \
    X(fkeynum_edge)          \
    X(csiarg_edge)           \
    X(ss3_edge)              \
    X(escape_edge)           \
    X(trie_rightexpand)      \
    X(format_barename)       \
    X(format_none)           \
    X(mouse_x10_raw)

#define X(name) {#name, test_##name},
static const tf_test_entry _test_entries[] = {TESTS(X){NULL, NULL}};
#undef X

int main(int argc, char *argv[]) {
    return tf_test_main("termfeed phase 0-5", _test_entries, argc, argv);
}
