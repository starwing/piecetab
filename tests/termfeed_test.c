#define _DEFAULT_SOURCE /* glibc: declare snprintf under strict C89 */
#define TF_STATIC_API
#include "termfeed.h"

#include "tests.h"

/* --- allocators --- */

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

/* --- mock reader --- */

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
static int feed_byte(tf_State *S, tf_Key *key, int b) {
    MockReader mr;
    char       cb = (char)b;
    mr.data = &cb, mr.len = 1, mr.called = 0;
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

/* --- Phase 0: lifecycle --- */

TEST(lifecycle) {
    tf_State S;
    tf_init(&S, NULL, NULL);
    asserteq(S.state, TF_STATE_IDLE);
    asserteq(S.flags, 0);
    asserteq(S.pending_mod, 0);
    asserteq(S.buf_len, 0);
    tf_free(&S);

    /* default allocator: alloc=NULL -> uses realloc internally */
    tf_init(&S, NULL, NULL);
    tf_free(&S);

    /* null checks */
    tf_init(NULL, NULL, NULL);
    tf_free(NULL);
}

TEST(flags) {
    tf_State S;
    tf_init(&S, NULL, NULL);
    /* set (overwrite) returns previous flags */
    asserteq(tf_setflag(&S, TF_FLAG_KEEPC0), 0);
    asserteq(tf_setflag(&S, TF_FLAG_KEEPC0 | TF_FLAG_DELBS), TF_FLAG_KEEPC0);
    asserteq(
            tf_setflag(&S, TF_FLAG_CONVERTKP),
            (TF_FLAG_KEEPC0 | TF_FLAG_DELBS));
    /* clear one bit: read-modify-write via returned value */
    tf_setflag(&S, TF_FLAG_DELBS | TF_FLAG_CONVERTKP);
    asserteq(
            tf_setflag(&S, TF_FLAG_DELBS), (TF_FLAG_DELBS | TF_FLAG_CONVERTKP));
    /* clear all */
    asserteq(tf_setflag(&S, 0), TF_FLAG_DELBS);

    /* null checks */
    asserteq(tf_setflag(NULL, 0), 0);
    asserteq(tf_setflag(NULL, TF_FLAG_DELBS), 0);

    tf_free(&S);
}

/* --- Phase 1: params --- */

TEST(feed_params) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* feed null */
    tf_feed(NULL, mock_reader, NULL);
    tf_feed(&S, NULL, NULL);

    /* readkey with no reader */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_NONE);

    /* null params */
    asserteq(tf_readkey(NULL, &key), TF_ERRPARAM);
    asserteq(tf_readkey(&S, NULL), TF_ERRPARAM);
    asserteq(tf_flush(NULL, &key), TF_ERRPARAM);
    asserteq(tf_flush(&S, NULL), TF_ERRPARAM);

    /* feed always discards the old chunk (even mid-replay) */
    S.replay = 1;
    S.buf[TF_MAX_BUFLEN - 1] = 'X';
    S.buf_len = 0;
    S.p = "chunk";
    S.n = 5;
    tf_feed(&S, NULL, 0);
    asserteq(S.n, 0);
    asserteq(S.p, NULL);

    tf_free(&S);
}

TEST(flush_params) {
    tf_State S;
    tf_Key   key;

    tf_init(&S, NULL, NULL);

    /* flush IDLE -> empty key */
    asserteq(tf_flush(&S, &key), TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_NONE);

    tf_free(&S);
}

/* --- Phase 1: IDLE state --- */

TEST(idle_printable) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    r = feed_byte(&S, &key, 'a');
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.event, TF_EVENT_PRESS);
    asserteq(key.modifiers, 0);
    assertstreq(key.utf8, "a");

    r = feed_byte(&S, &key, 'Z');
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'Z');
    assertstreq(key.utf8, "Z");

    r = feed_byte(&S, &key, '0');
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, '0');

    r = feed_byte(&S, &key, ' ');
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, ' ');
    asserteq(key.type, TF_TYPE_UNICODE);

    r = feed_byte(&S, &key, '~');
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, '~');

    tf_free(&S);
}

TEST(idle_c0_default) {
    tf_State S;
    tf_Key   key;
    int      r, i;

    tf_init(&S, NULL, NULL);

    /* 0x00 -> Ctrl+Space (KEYSYM SPACE + CTRL) */
    r = feed_byte(&S, &key, 0x00);
    asserteq(r, TF_OK);
    if (key.type != TF_TYPE_KEYSYM) {
        test_log("RAW: key.type=%d\n", (int)key.type);
        abort();
    }
    asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x09 -> TAB */
    r = feed_byte(&S, &key, 0x09);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    asserteq(key.modifiers, 0);

    /* 0x0d -> ENTER */
    r = feed_byte(&S, &key, 0x0d);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ENTER);
    asserteq(key.modifiers, 0);

    /* 0x1b -> ESCAPE state (no key output, returns TF_AGAIN) */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_ESCAPE);

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
        asserteq(r, TF_OK);
        asserteq(key.type, TF_TYPE_UNICODE);
        asserteq(key.d.codepoint, exp);
        asserteq(key.modifiers, TF_MOD_CTRL);
        expstr[0] = (char)exp;
        expstr[1] = '\0';
        assertstreq(key.utf8, expstr);
        tf_free(&S2);
    }

    /* 0x1c -> Ctrl+\ (UNICODE + CTRL, as-is) */
    r = feed_byte(&S, &key, 0x1c);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '\\');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x1d -> Ctrl+] */
    r = feed_byte(&S, &key, 0x1d);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, ']');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x1e -> Ctrl+^ */
    r = feed_byte(&S, &key, 0x1e);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, '^');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x1f -> Ctrl+_ */
    r = feed_byte(&S, &key, 0x1f);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, '_');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x7f -> DEL */
    r = feed_byte(&S, &key, 0x7f);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    tf_free(&S);
}

TEST(idle_keepc0) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    asserteq(tf_setflag(&S, TF_FLAG_KEEPC0), 0);

    /* C0 bytes become UNICODE(byte); NUL is always Ctrl-Space (termkey compat)
     */
    r = feed_byte(&S, &key, 0x00);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    asserteq(key.modifiers, TF_MOD_CTRL);

    r = feed_byte(&S, &key, 0x01);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0x01);
    asserteq(key.modifiers, 0);

    r = feed_byte(&S, &key, 0x08);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0x08);

    r = feed_byte(&S, &key, 0x09);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0x09);
    asserteq(key.type, TF_TYPE_UNICODE); /* not KEYSYM */

    r = feed_byte(&S, &key, 0x0d);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0x0d);
    asserteq(key.type, TF_TYPE_UNICODE); /* not KEYSYM */

    /* 0x1b always goes to ESCAPE state */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_ESCAPE);

    tf_free(&S);
}

TEST(idle_none) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    /* feed empty data -> TF_NONE */
    {
        MockReader mr;
        mr.data = NULL, mr.len = 0, mr.called = 0;
        tf_feed(&S, mock_reader, &mr);
        r = tf_readkey(&S, &key);
    }
    asserteq(r, TF_NONE);

    tf_free(&S);
}

/* --- Phase 1: ESCAPE state --- */

TEST(escape_basic) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b + 'x' -> UNICODE('x') + ALT */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_ESCAPE);
    r = feed_byte(&S, &key, 'x');
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.state, TF_STATE_IDLE);

    /* \x1b + 0x01 -> Ctrl+'a' + ALT */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x01);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.modifiers, TF_MOD_CTRL | TF_MOD_ALT);

    /* \x1b + 0x09 -> TAB + ALT */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x09);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    asserteq(key.modifiers, TF_MOD_ALT);

    /* \x1b + 0x0d -> ENTER + ALT */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x0d);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ENTER);
    asserteq(key.modifiers, TF_MOD_ALT);

    /* \x1b + 0x7f -> DEL + ALT */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x7f);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_DELETE);
    asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

TEST(escape_alt) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b\x1b + 'x' -> same as \x1b + 'x' = 'x' + ALT */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_ESCAPE);
    asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_byte(&S, &key, 'x');
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.pending_mod, 0);
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

TEST(escape_csi_ss3_cs) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b + '[' -> CSI state */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CSI);

    /* \x1b + 'O' -> SS3 state */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_SS3);

    /* \x1b + ']' -> CS_OSC state */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CS_OSC);

    /* \x1b + 'P' -> CS_DCS state */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'P');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CS_DCS);

    /* \x1b + '_' -> CS_APC state */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '_');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CS_APC);

    tf_free(&S);
}

/* test: ESCAPE + CSI sequence -> state transitions and bytes */
TEST(escape_chunk) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b -> ESCAPE state */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_ESCAPE);

    /* '[' -> CSI state */
    r = feed_byte(&S, &key, '[');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CSI);

    /* '1' -> CSI accumulating (no final yet) */
    r = feed_byte(&S, &key, '1');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CSI);

    /* 'A' -> final: dispatch -> UP key */
    r = feed_byte(&S, &key, 'A');
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(S.state, TF_STATE_IDLE);

    /* now in IDLE: next byte is plain UNICODE */
    r = feed_byte(&S, &key, 'x');
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'x');

    tf_free(&S);
}

/* --- Phase 1: flush --- */

TEST(flush_escape) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* enter ESCAPE via \x1b */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);

    /* flush -> ESC key */
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);
    asserteq(key.event, TF_EVENT_PRESS);
    asserteq(key.modifiers, 0);
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

TEST(flush_escape_alt) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* \x1b\x1b -> ESCAPE with alt_pending */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_ESCAPE);
    asserteq(S.pending_mod, TF_MOD_ALT);

    /* flush -> ALT+ESC */
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.state, TF_STATE_IDLE);
    asserteq(S.pending_mod, 0);

    tf_free(&S);
}

TEST(flush_csi_stub) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[ -> CSI */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CSI);

    /* flush CSI -> ALT+[ (empty buf -> straight to IDLE) */
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '[');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* --- Phase 2: CSI cursor keys --- */

TEST(csi_cursor) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    r = feed_seq(&S, &key, "\x1b[A", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);

    r = feed_seq(&S, &key, "\x1b[B", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_DOWN);

    r = feed_seq(&S, &key, "\x1b[C", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_RIGHT);

    r = feed_seq(&S, &key, "\x1b[D", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_LEFT);

    r = feed_seq(&S, &key, "\x1b[E", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_BEGIN);

    r = feed_seq(&S, &key, "\x1b[F", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_END);

    r = feed_seq(&S, &key, "\x1b[H", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    tf_free(&S);
}

/* --- Phase 2: CSI modifiers --- */

TEST(csi_modifiers) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1;2A -> Shift+UP */
    r = feed_seq(&S, &key, "\x1b[1;2A", 6);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(key.modifiers, TF_MOD_SHIFT);
    asserteq(key.event, TF_EVENT_PRESS);

    /* \x1b[1;5A -> Ctrl+UP */
    r = feed_seq(&S, &key, "\x1b[1;5A", 6);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(key.modifiers, TF_MOD_CTRL);

    tf_free(&S);
}

/* --- Phase 2: shift-tab --- */

TEST(csi_shifttab) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[Z -> Shift+TAB (default shift) */
    r = feed_seq(&S, &key, "\x1b[Z", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    asserteq(key.modifiers, TF_MOD_SHIFT);
    asserteq(key.event, TF_EVENT_PRESS);

    /* \x1b[1;5Z -> Shift+TAB + Ctrl */
    r = feed_seq(&S, &key, "\x1b[1;5Z", 6);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    asserteq(key.modifiers, TF_MOD_SHIFT | TF_MOD_CTRL);
    asserteq(key.event, TF_EVENT_PRESS);

    tf_free(&S);
}

/* --- Phase 2: func/editing keys --- */

TEST(csi_funckey) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[3~ -> DELETE */
    r = feed_seq(&S, &key, "\x1b[3~", 4);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    /* \x1b[1~ -> FIND */
    r = feed_seq(&S, &key, "\x1b[1~", 4);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_FIND);

    /* \x1b[5~ -> PAGEUP */
    r = feed_seq(&S, &key, "\x1b[5~", 4);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_PAGEUP);

    /* \x1b[6~ -> PAGEDOWN */
    r = feed_seq(&S, &key, "\x1b[6~", 4);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_PAGEDOWN);

    /* \x1b[7~ -> HOME */
    r = feed_seq(&S, &key, "\x1b[7~", 4);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    /* \x1b[8~ -> END */
    r = feed_seq(&S, &key, "\x1b[8~", 4);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_END);

    /* \x1b[11~ -> F1 */
    r = feed_seq(&S, &key, "\x1b[11~", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 1);

    /* \x1b[12~ -> F2 */
    r = feed_seq(&S, &key, "\x1b[12~", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 2);

    /* \x1b[15~ -> F5 */
    r = feed_seq(&S, &key, "\x1b[15~", 5);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 5);

    /* \x1b[17~ -> F6 */
    r = feed_seq(&S, &key, "\x1b[17~", 5);
    asserteq(key.d.number, 6);

    /* \x1b[18~ -> F7 */
    r = feed_seq(&S, &key, "\x1b[18~", 5);
    asserteq(key.d.number, 7);

    /* \x1b[19~ -> F8 */
    r = feed_seq(&S, &key, "\x1b[19~", 5);
    asserteq(key.d.number, 8);

    /* \x1b[20~ -> F9 */
    r = feed_seq(&S, &key, "\x1b[20~", 5);
    asserteq(key.d.number, 9);

    /* \x1b[21~ -> F10 */
    r = feed_seq(&S, &key, "\x1b[21~", 5);
    asserteq(key.d.number, 10);

    /* \x1b[23~ -> F11 */
    r = feed_seq(&S, &key, "\x1b[23~", 5);
    asserteq(key.d.number, 11);

    /* \x1b[24~ -> F12 */
    r = feed_seq(&S, &key, "\x1b[24~", 5);
    asserteq(key.d.number, 12);

    /* \x1b[28~ -> F15 */
    r = feed_seq(&S, &key, "\x1b[28~", 5);
    asserteq(key.d.number, 15);

    /* \x1b[29~ -> F16 */
    r = feed_seq(&S, &key, "\x1b[29~", 5);
    asserteq(key.d.number, 16);

    /* \x1b[32~ -> F18 */
    r = feed_seq(&S, &key, "\x1b[32~", 5);
    asserteq(key.d.number, 18);

    /* \x1b[34~ -> F20 */
    r = feed_seq(&S, &key, "\x1b[34~", 5);
    asserteq(key.d.number, 20);

    /* \x1b[27;5;97~ -> Ctrl+'a' (modifyOtherKeys) */
    r = feed_seq(&S, &key, "\x1b[27;5;97~", 10);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* \x1b[27;99~ -> modifyOtherKeys with invalid code -> UNKNOWN_CSI */
    r = feed_seq(&S, &key, "\x1b[27;99~", 8);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    /* \x1b[42~ -> FUNCTION 42 (default) */
    r = feed_seq(&S, &key, "\x1b[42~", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 42);

    /* remaining fkeynum mappings */
    r = feed_seq(&S, &key, "\x1b[13~", 5);
    asserteq(key.d.number, 3);

    r = feed_seq(&S, &key, "\x1b[14~", 5);
    asserteq(key.d.number, 4);

    r = feed_seq(&S, &key, "\x1b[25~", 5);
    asserteq(key.d.number, 13);

    r = feed_seq(&S, &key, "\x1b[26~", 5);
    asserteq(key.d.number, 14);

    r = feed_seq(&S, &key, "\x1b[31~", 5);
    asserteq(key.d.number, 17);

    r = feed_seq(&S, &key, "\x1b[33~", 5);
    asserteq(key.d.number, 19);

    /* \x1b[0~ -> FUNCTION 0 (below FIND range) */
    r = feed_seq(&S, &key, "\x1b[0~", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 0);

    /* \x1b[~ -> UNKNOWN_CSI (empty function key arg) */
    r = feed_seq(&S, &key, "\x1b[~", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    tf_free(&S);
}

/* --- Phase 2: event sub-parameters --- */

TEST(csi_event) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1;2:3~ -> DELETE + Shift + RELEASE */
    r = feed_seq(&S, &key, "\x1b[1;2:3~", 8);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_FIND);
    asserteq(key.modifiers, TF_MOD_SHIFT);
    asserteq(key.event, TF_EVENT_RELEASE);

    /* \x1b[1;2:2~ -> REPEAT */
    r = feed_seq(&S, &key, "\x1b[1;2:2~", 8);
    asserteq(r, TF_OK);
    asserteq(key.event, TF_EVENT_REPEAT);

    /* \x1b[1;2:9~ -> invalid event -> UNKNOWN_CSI */
    r = feed_seq(&S, &key, "\x1b[1;2:9~", 8);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    /* event=1 explicit -> PRESS */
    r = feed_seq(&S, &key, "\x1b[1;2:1~", 8);
    asserteq(r, TF_OK);
    asserteq(key.event, TF_EVENT_PRESS);

    /* field leading char in 0x3A-0x40 skipped: \x1b[1;>2~ -> SHIFT+FIND */
    r = feed_seq(&S, &key, "\x1b[1;>2~", 8);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_FIND);
    asserteq(key.modifiers, TF_MOD_SHIFT);

    tf_free(&S);
}

/* --- Phase 2: position report --- */

TEST(csi_position) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[?1;2R -> POSITION(1,2) */
    r = feed_seq(&S, &key, "\x1b[?1;2R", 7);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_POSITION);
    asserteq(key.d.pos.line, 1);
    asserteq(key.d.pos.col, 2);
    asserteq(key.event, TF_EVENT_PRESS);

    tf_free(&S);
}

/* --- Phase 2: kitty report --- */

TEST(csi_kitty) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[?u -> KITTYREPORT (no params) */
    r = feed_seq(&S, &key, "\x1b[?u", 4);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KITTYREPORT);
    asserteq(key.d.number, -1);
    asserteq(key.event, TF_EVENT_PRESS);

    /* \x1b[?1u -> KITTYREPORT with flags=1 */
    r = feed_seq(&S, &key, "\x1b[?1u", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KITTYREPORT);
    asserteq(key.d.number, 1);
    asserteq(key.event, TF_EVENT_PRESS);

    tf_free(&S);
}

/* --- Phase 2: mode report --- */

TEST(csi_modereport) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1$y -> ANSI mode report: mode=1 */
    r = feed_seq(&S, &key, "\x1b[1$y", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MODEREPORT);
    asserteq(key.d.modereport.initial, 0);
    asserteq(key.d.modereport.mode, 1);
    asserteq(key.d.modereport.value, -1);
    asserteq(key.event, TF_EVENT_PRESS);

    /* \x1b[?1;2$y -> DEC mode report */
    r = feed_seq(&S, &key, "\x1b[?1;2$y", 8);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MODEREPORT);
    asserteq(key.d.modereport.initial, '?');
    asserteq(key.d.modereport.mode, 1);
    asserteq(key.d.modereport.value, 2);
    asserteq(key.event, TF_EVENT_PRESS);

    tf_free(&S);
}

/* --- Phase 2: CSI params edge cases --- */

TEST(csi_params) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[;2A -> UP + modifier 2 (empty first field -> arg1=-1->dflt 1??) */
    /* Actually empty arg1 means arg1 defaults to -1 for nargs counting,
     * but cursor key handler doesn't check arg1; it uses tfD_event
     * which gets modifiers from arg2 */
    r = feed_seq(&S, &key, "\x1b[;2A", 5);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(key.modifiers, TF_MOD_SHIFT);

    /* \x1b[A with alt_pending (\x1b\x1b[A) -> UP + ALT */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_seq(&S, &key, "[A", 2);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(key.modifiers, TF_MOD_ALT);

    /* non-digit inside param field: \x1b[1;2<3A -> UP + SHIFT
     * (arg2 stops at '<') */
    r = feed_seq(&S, &key, "\x1b[1;2<3A", 8);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(key.modifiers, TF_MOD_SHIFT);

    /* non-digit in sub-param: \x1b[1;2:3<4A -> UP, arg2=2 */
    r = feed_seq(&S, &key, "\x1b[1;2:3<4A", 10);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(key.modifiers, TF_MOD_SHIFT);

    /* \x1b\x1bOA -> ALT+UP (SS3 dispatch merges alt_pending) */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_seq(&S, &key, "OA", 2);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

/* --- Phase 3: SS3 cursor keys --- */

TEST(ss3_cursor) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    r = feed_seq(&S, &key, "\x1bOA", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);

    r = feed_seq(&S, &key, "\x1bOB", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_DOWN);

    r = feed_seq(&S, &key, "\x1bOC", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_RIGHT);

    r = feed_seq(&S, &key, "\x1bOD", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_LEFT);

    r = feed_seq(&S, &key, "\x1bOH", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    r = feed_seq(&S, &key, "\x1bOF", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_END);

    /* \x1bOE -> BEGIN */
    r = feed_seq(&S, &key, "\x1bOE", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_BEGIN);

    tf_free(&S);
}

/* --- Phase 3: SS3 function keys --- */

TEST(ss3_func) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    r = feed_seq(&S, &key, "\x1bOP", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 1);

    r = feed_seq(&S, &key, "\x1bOQ", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 2);

    r = feed_seq(&S, &key, "\x1bOR", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 3);

    r = feed_seq(&S, &key, "\x1bOS", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 4);

    tf_free(&S);
}

/* --- Phase 3: SS3 KP keys --- */

TEST(ss3_kp) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* KPENTER */
    r = feed_seq(&S, &key, "\x1bOM", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_KPENTER);

    /* KPMULT */
    r = feed_seq(&S, &key, "\x1bOj", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KPMULT);

    /* KPPLUS */
    r = feed_seq(&S, &key, "\x1bOk", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KPPLUS);

    /* KPMINUS */
    r = feed_seq(&S, &key, "\x1bOm", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KPMINUS);

    /* KPDIV */
    r = feed_seq(&S, &key, "\x1bOo", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KPDIV);

    /* KPEQUALS */
    r = feed_seq(&S, &key, "\x1bOX", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KPEQUALS);

    /* KPCOMMA */
    r = feed_seq(&S, &key, "\x1bOl", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KPCOMMA);

    /* KPPERIOD */
    r = feed_seq(&S, &key, "\x1bOn", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KPPERIOD);

    /* KP0 */
    r = feed_seq(&S, &key, "\x1bOp", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KP0);

    /* KP9 */
    r = feed_seq(&S, &key, "\x1bOy", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KP9);

    tf_free(&S);
}

/* --- Phase 3: SS3 CONVERTKP --- */

TEST(ss3_convertkp) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_CONVERTKP);

    /* KPMULT -> '*' */
    r = feed_seq(&S, &key, "\x1bOj", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '*');

    /* KPPLUS -> '+' */
    r = feed_seq(&S, &key, "\x1bOk", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '+');

    /* KPMINUS -> '-' */
    r = feed_seq(&S, &key, "\x1bOm", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '-');

    /* KPDIV -> '/' */
    r = feed_seq(&S, &key, "\x1bOo", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '/');

    /* KPPERIOD -> '.' */
    r = feed_seq(&S, &key, "\x1bOn", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '.');

    /* KPCOMMA -> ',' */
    r = feed_seq(&S, &key, "\x1bOl", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, ',');

    /* KPEQUALS -> '=' */
    r = feed_seq(&S, &key, "\x1bOX", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '=');

    /* KP0 -> '0' */
    r = feed_seq(&S, &key, "\x1bOp", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '0');

    /* non-KP final (UP) with CONVERTKP: kpkey short-circuits, KEYSYM */
    r = feed_seq(&S, &key, "\x1bOA", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);

    /* unknown final with CONVERTKP: kpconvert fails -> ALT+O peel */
    r = feed_seq(&S, &key, "\x1bOG", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'O');
    asserteq(key.modifiers, TF_MOD_ALT);

    /* consume replayed 'G' */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'G');

    /* KPENTER -> still KPENTER (no conversion) */
    r = feed_seq(&S, &key, "\x1bOM", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_KPENTER);

    tf_free(&S);
}

/* --- Phase 3: SS3 unknown -> REPLAY_BUF --- */

TEST(ss3_unknown) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1bOG -> first key: ALT+O */
    /* feed ['G'] separately because \x1bO is consumed first */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'G');
    /* 'G' is not a valid SS3 final -> ALT+O produced, REPLAY_BUF(1) */
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'O');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.replay, 1);

    /* next read -> REPLAY_BUF outputs 'G' */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'G');
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* --- Phase 3: X10 mouse --- */

TEST(mouse_x10) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[M + 3 raw bytes (button 1, col 2, line 3) */
    r = feed_seq(&S, &key, "\x1b[M\x20\x22\x23", 6);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    asserteq(key.d.mouse.btn, 0);  /* code=0x20 -> code-0x20=0, btn=0 */
    asserteq(key.d.mouse.col, 2);  /* 0x22-0x20=2 */
    asserteq(key.d.mouse.line, 3); /* 0x23-0x20=3 */
    asserteq(key.d.mouse.release, 0);

    /* X10 with modifiers: btn=1, shift (btn with mod bit 0x04) */
    /* code = 0x24 -> btn=0, mods=(0x24&0x1c)>>2=(0x04)>>2=1=SHIFT */
    r = feed_seq(&S, &key, "\x1b[M\x24\x21\x21", 6);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    asserteq(key.modifiers, TF_MOD_SHIFT);
    asserteq(key.d.mouse.col, 1);
    asserteq(key.d.mouse.line, 1);

    tf_free(&S);
}

/* --- Phase 3: X10 mouse across chunks --- */

TEST(mouse_x10_chunk) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[M enters MOUSE_X10 (no more bytes in chunk) */
    r = feed_seq(&S, &key, "\x1b[M", 3);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_MOUSE_X10);
    asserteq(S.buf_len, 1); /* buf[0] = 'M', raw to follow */

    /* feed byte 1 -> MOUSE_X10: raw at buf[1], still need 2 more */
    r = feed_byte(&S, &key, 0x20);
    asserteq(r, TF_AGAIN);
    asserteq(S.buf_len, 2);

    /* feed byte 2 */
    r = feed_byte(&S, &key, 0x21);
    asserteq(r, TF_AGAIN);
    asserteq(S.buf_len, 3);

    /* feed byte 3 -> complete, decode mouse */
    r = feed_byte(&S, &key, 0x22);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    asserteq(key.d.mouse.col, 1);
    asserteq(key.d.mouse.line, 2);
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* --- Phase 3: rxvt mouse --- */

TEST(mouse_rxvt) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[32;5;8M -> rxvt: code=32, col=5, line=8 */
    r = feed_seq(&S, &key, "\x1b[32;5;8M", 9);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    asserteq(key.d.mouse.col, 5);
    asserteq(key.d.mouse.line, 8);
    asserteq(key.d.mouse.release, 0);

    tf_free(&S);
}

/* --- Phase 3: X10 mouse across chunks with alt_pending --- */

TEST(mouse_x10_chunkalt) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b\x1b -> alt_pending=1, then [M in 2-byte chunk -> X10 state */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.pending_mod, TF_MOD_ALT);

    /* [M -> first byte goes to CSI, 'M' dispatch -> X10, S->n<3 -> MOUSE_X10 */
    r = feed_seq(&S, &key, "[M", 2);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_MOUSE_X10);

    /* feed X10 bytes one at a time */
    r = feed_byte(&S, &key, 0x20);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x21);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x22);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(key.d.mouse.col, 1);
    asserteq(key.d.mouse.line, 2);
    asserteq(S.state, TF_STATE_IDLE);
    asserteq(S.pending_mod, 0);

    tf_free(&S);
}

/* --- Phase 3: SGR mouse --- */

TEST(mouse_sgr) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[<0;5;8M -> SGR press: code=0, col=5, line=8 */
    r = feed_seq(&S, &key, "\x1b[<0;5;8M", 9);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    asserteq(key.d.mouse.col, 5);
    asserteq(key.d.mouse.line, 8);
    asserteq(key.d.mouse.release, 0);

    /* \x1b[<0;5;8m -> SGR release */
    r = feed_seq(&S, &key, "\x1b[<0;5;8m", 9);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    asserteq(key.d.mouse.release, 1);

    tf_free(&S);
}

/* --- Phase 3: SS3 unknown prefix byte --- */

TEST(ss3_unknownprefix) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1bO + '0' (not a final byte) -> ALT+O, '0' in REPLAY_BUF */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '0');
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'O');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.replay, 1);
    asserteq(S.buf[TF_MAX_BUFLEN - 1], '0');

    /* replay: '0' */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '0');
    asserteq(S.state, TF_STATE_IDLE);

    /* \x1bO + 0xfe -> ALT+O, 0xfe in REPLAY_BUF -> 0xFFFD */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0xfe);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'O');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.replay, 1);
    /* replay: high byte -> 0xFFFD */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0xFFFD);

    tf_free(&S);
}

/* --- Phase 3: X10 mouse with alt_pending --- */

TEST(mouse_x10_alt) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b\x1b[M\x20\x21\x22 -> ALT + MOUSE (via alt_pending) */
    /* feed \x1b\x1b first to set alt_pending */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.pending_mod, TF_MOD_ALT);

    /* feed [M + 3 raw bytes in one 5-byte chunk */
    r = feed_seq(&S, &key, "[M\x20\x21\x22", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(key.d.mouse.col, 1);
    asserteq(key.d.mouse.line, 2);
    asserteq(S.state, TF_STATE_IDLE);
    asserteq(S.pending_mod, 0);

    tf_free(&S);
}

/* --- Phase 2+3: unknown CSI --- */

TEST(csi_unknown) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[999@ -> UNKNOWN_CSI (final '@' not recognized) */
    r = feed_seq(&S, &key, "\x1b[999@", 7);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    /* plain \x1b[R -> F3 */
    r = feed_seq(&S, &key, "\x1b[R", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 3);
    asserteq(key.event, TF_EVENT_PRESS);

    /* control byte in param area short-circuits: \x1b[\x01A -> UP */
    r = feed_seq(
            &S, &key,
            "\x1b[\x01"
            "A",
            4);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);

    tf_free(&S);
}

/* --- Phase 4: Control String --- */

TEST(cs_osc) {
    tf_State S;
    tf_Key   key;
    int      r, len;
    tf_init(&S, NULL, NULL);

    /* OSC \x1b]0;title\a -> OSC key, tf_string=="0;title" */
    r = feed_seq(&S, &key, "\x1b]0;title\x07", 11);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    asserteq(key.event, TF_EVENT_PRESS);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 7);
    asserteq(memcmp(tf_string(&S, &len), "0;title", 7), 0);
    tf_free(&S);
}

TEST(cs_st) {
    tf_State S;
    tf_Key   key;
    int      r, len;
    tf_init(&S, NULL, NULL);

    /* ST (0x9c) termination */
    r = feed_seq(&S, &key, "\x1b]test\x9c", 8);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 4);
    asserteq(memcmp(tf_string(&S, &len), "test", 4), 0);
    tf_free(&S);

    /* ST via ESC+\ */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]ab\x1b\x5c", 7);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    assertok(tf_string(&S, &len) != NULL);
    /* content "ab": trailing \x1b of ESC+\ removed, '\' not yet buffered */
    asserteq(len, 2);
    asserteq(memcmp(tf_string(&S, &len), "ab", 2), 0);

    /* ESC content inside OSC: \x1b]abc\x1b\ -> content "abc" */
    /* The \x1b before \ is appended, then ST detection removes it. */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]abc\x1b\x5c", 8);
    asserteq(r, TF_OK);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 3);
    asserteq(memcmp(tf_string(&S, &len), "abc", 3), 0);
    tf_free(&S);

    /* content is exactly ESC: \x1b]\x1b\ -> empty OSC (cs_len drops to 0) */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]\x1b\x5c", 4);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 0);
    tf_free(&S);
}

TEST(cs_dcs_apc) {
    tf_State S;
    tf_Key   key;
    int      r, len;
    tf_init(&S, NULL, NULL);

    /* DCS \x1bP+q...\a */
    r = feed_seq(&S, &key, "\x1bP+qok\x07", 7);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_DCS);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 4);
    asserteq(memcmp(tf_string(&S, &len), "+qok", 4), 0);
    tf_free(&S);

    /* APC \x1b_title\a */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b_title\x07", 8);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_APC);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 5);
    asserteq(memcmp(tf_string(&S, &len), "title", 5), 0);
    tf_free(&S);
}

TEST(cs_cross) {
    tf_State S;
    tf_Key   key;
    int      r, len;
    tf_init(&S, NULL, NULL);

    /* cross-reader: feed ESC+] then AGAIN, then feed content+BEL */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CS_OSC);

    /* feed 'x' -> cs_buf alloc, AGAIN */
    r = feed_byte(&S, &key, 'x');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CS_OSC);

    /* feed 'y' -> AGAIN */
    r = feed_byte(&S, &key, 'y');
    asserteq(r, TF_AGAIN);

    /* feed BEL -> complete */
    r = feed_byte(&S, &key, 0x07);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 2);
    asserteq(memcmp(tf_string(&S, &len), "xy", 2), 0);
    tf_free(&S);
}

TEST(cs_alt) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* \x1b\x1b]test\a -> OSC + ALT */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_seq(&S, &key, "]test\x07", 7);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.pending_mod, 0);
    tf_free(&S);
}

TEST(cs_oom) {
    tf_State S;
    tf_Key   key;
    int      r, ec;
    tf_init(&S, test_alloc, NULL);

    /* enter CS: ESC+] */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    asserteq(r, TF_AGAIN);

    /* now use oom_alloc to fail on first byte */
    ec = 0; /* fail immediately */
    tf_init(&S, oom_alloc, &ec);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'x');
    asserteq(r, TF_ERRMEM);
    asserteq(S.state, TF_STATE_IDLE);
    tf_free(&S);
}

TEST(cs_params) {
    tf_State S;
    tf_Key   key;
    int      len;
    tf_init(&S, NULL, NULL);

    /* tf_string with NULL/params - no CS key */
    asserteq(tf_string(&S, &len), NULL);
    asserteq(len, 0);

    /* plen may be NULL */
    asserteq(tf_string(&S, NULL), NULL);
    asserteq(tf_string(NULL, NULL), NULL);

    /* plen == NULL with CS content: returns buffer, skips plen */
    asserteq(tf_string(NULL, &len), NULL);
    tf_free(&S);

    tf_init(&S, NULL, NULL);
    asserteq(feed_seq(&S, &key, "\x1b]test\x07", 8), TF_OK);
    assertok(tf_string(&S, NULL) != NULL);
    tf_free(&S);
}

/* --- Phase 5: UTF-8 decode --- */

TEST(utf8_decode) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* 2-byte: A-tilde = 0xC3 0x80 -> U+00C0 */
    r = feed_seq(&S, &key, "\xc3\x80", 2);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0xC0);
    asserteq((unsigned char)key.utf8[0], 0xC3);
    asserteq((unsigned char)key.utf8[1], 0x80);
    asserteq(key.utf8[2], '\0');

    /* 3-byte: Euro = 0xE2 0x82 0xAC -> U+20AC */
    r = feed_seq(&S, &key, "\xe2\x82\xac", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0x20AC);
    asserteq((unsigned char)key.utf8[0], 0xE2);
    asserteq((unsigned char)key.utf8[1], 0x82);
    asserteq((unsigned char)key.utf8[2], 0xAC);
    asserteq(key.utf8[3], '\0');

    /* 2-byte: ! = 0xC2 0xA1 -> U+00A1 */
    r = feed_seq(&S, &key, "\xc2\xa1", 2);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xA1);
    asserteq((unsigned char)key.utf8[0], 0xC2);
    asserteq((unsigned char)key.utf8[1], 0xA1);
    tf_free(&S);
}

TEST(utf8_chunk) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* cross-chunk 2-byte: C3 80 -> A-grave */
    r = feed_byte(&S, &key, 0xC3);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_UTF8);
    asserteq(S.buf_len, 1);
    asserteq((unsigned char)S.buf[0], 0xC3);

    r = feed_byte(&S, &key, 0x80);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0xC0);
    asserteq(S.state, TF_STATE_IDLE);

    /* cross-chunk 3-byte: E2 82 AC -> Euro */
    r = feed_byte(&S, &key, 0xE2);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x82);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0xAC);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0x20AC);
    tf_free(&S);
}

TEST(utf8_invalid) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* overlong 2-byte: C0 80 -> FFFD */
    r = feed_seq(&S, &key, "\xc0\x80", 2);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0xFFFD);
    asserteq((unsigned char)key.utf8[0], 0xEF);
    asserteq((unsigned char)key.utf8[1], 0xBF);
    asserteq((unsigned char)key.utf8[2], 0xBD);
    asserteq(key.utf8[3], '\0');

    /* overlong 2-byte: C1 BF -> FFFD */
    r = feed_seq(&S, &key, "\xc1\xbf", 2);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xFFFD);

    /* surrogate: ED A0 80 -> FFFD */
    r = feed_seq(&S, &key, "\xed\xa0\x80", 3);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xFFFD);

    /* invalid continuation byte: C3 40 -> FFFD */
    r = feed_seq(&S, &key, "\xc3\x40", 2);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xFFFD);

    /* invalid lead byte: 0xFE -> FFFD */
    r = feed_byte(&S, &key, 0xFE);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xFFFD);

    /* invalid lead byte: 0xFF -> FFFD */
    r = feed_byte(&S, &key, 0xFF);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xFFFD);

    /* standalone continuation: 0x80 -> FFFD */
    r = feed_byte(&S, &key, 0x80);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xFFFD);

    /* 0xFFFE (EF BF BE) -> FFFD */
    r = feed_seq(&S, &key, "\xef\xbf\xbe", 3);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xFFFD);

    /* 0xFFFF (EF BF BF) -> FFFD */
    r = feed_seq(&S, &key, "\xef\xbf\xbf", 3);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xFFFD);

    tf_free(&S);
}

TEST(utf8_alt) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* \x1b + UTF-8 lead (one-shot): C3 80 -> ALT+A-grave */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_seq(&S, &key, "\xc3\x80", 2);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0xC0);
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.state, TF_STATE_IDLE);

    /* \x1b + UTF-8 lead (cross-chunk): C3 then 80 -> ALT+A-grave */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0xC3);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_UTF8);
    asserteq(S.buf_len, 1);
    r = feed_byte(&S, &key, 0x80);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xC0);
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.state, TF_STATE_IDLE);

    /* \x1b\x1b + UTF-8 -> ALT+A-grave (alt_pending merge) */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_seq(&S, &key, "\xc3\x80", 2);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xC0);
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.pending_mod, 0);

    /* \x1b\x1b + UTF-8 -> ALT+A-grave (cross-chunk with alt_pending=1) */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_byte(&S, &key, 0xC3);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_UTF8);
    r = feed_byte(&S, &key, 0x80);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xC0);
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.pending_mod, 0);

    tf_free(&S);
}

TEST(utf8_params) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* NOINTERPRET: 0x80+ still outputs raw byte */
    tf_setflag(&S, TF_FLAG_KEEPC0);
    r = feed_byte(&S, &key, 0x80);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0x80);

    r = feed_byte(&S, &key, 0xFE);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0xFE);

    /* IDLE: 0x80 -> FFFD now (was raw byte before Phase 5) */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x81);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0xFFFD);

    tf_free(&S);
}

TEST(utf8_456) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* 4-byte: F0 90 8D 88 -> U+10348 */
    r = feed_seq(&S, &key, "\xf0\x90\x8d\x88", 4);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0x10348);
    asserteq((unsigned char)key.utf8[0], 0xF0);
    asserteq((unsigned char)key.utf8[3], 0x88);

    /* 5-byte: F8 88 80 80 80 -> min valid 5-byte (U+200000) */
    r = feed_seq(&S, &key, "\xf8\x88\x80\x80\x80", 5);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0x200000);
    asserteq((unsigned char)key.utf8[0], 0xF8);

    /* 5-byte cross-chunk */
    r = feed_byte(&S, &key, 0xF8);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x88);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x80);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x80);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x80);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0x200000);

    /* 6-byte: FC 84 80 80 80 80 -> min valid 6-byte (U+4000000) */
    r = feed_seq(&S, &key, "\xfc\x84\x80\x80\x80\x80", 6);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 0x4000000);
    asserteq((unsigned char)key.utf8[0], 0xFC);

    tf_free(&S);
}

TEST(cs_expand) {
    tf_State S;
    tf_Key   key;
    int      r, len, i;
    char     buf[128];
    tf_init(&S, NULL, NULL);

    /* Feed a long CS content (> 64 bytes) to trigger realloc */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    asserteq(r, TF_AGAIN);

    for (i = 0; i < 128; i++) buf[i] = (unsigned char)('A' + (i % 26));
    r = feed_seq(&S, &key, buf, 128);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CS_OSC);

    /* terminate with BEL */
    r = feed_byte(&S, &key, 0x07);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 128);
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
        asserteq(r, TF_OK);
        asserteq(key.type, TF_TYPE_OSC);
        assertok(tf_string(&S, &len) != NULL);
        asserteq(len, 300);
        tf_free(&S);
    }

    /* backslash not preceded by ESC: plain content byte */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]a\\b\x07", 6);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 3);
    assertstreq(tf_string(&S, NULL), "a\\b");
    tf_free(&S);

    /* backslash as very first content byte (cs_len == 0): not an ST */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b]\\x\x07", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 2);
    assertstreq(tf_string(&S, NULL), "\\x");
    tf_free(&S);
}

TEST(cs_allocfree) {
    tf_State S;
    tf_Key   key;
    int      r, i;
    char     buf[128];
    tf_init(&S, test_alloc, NULL);

    /* OSC with custom allocator -> cs_buf allocated via test_alloc */
    r = feed_seq(&S, &key, "\x1b]hello\x07", 9);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);

    /* tf_free should free cs_buf via test_alloc */
    tf_free(&S);

    /* CS realloc via custom allocator: >64 bytes */
    tf_init(&S, test_alloc, NULL);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    asserteq(r, TF_AGAIN);
    for (i = 0; i < 128; i++) buf[i] = (unsigned char)('A' + (i % 26));
    r = feed_seq(&S, &key, buf, 128);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x07);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    tf_free(&S);

    /* OOM on CS realloc: first alloc succeeds, realloc fails */
    {
        int ec = 1; /* allow first alloc, fail second */
        tf_init(&S, oom_alloc, &ec);
        r = feed_byte(&S, &key, 0x1b);
        asserteq(r, TF_AGAIN);
        r = feed_byte(&S, &key, ']');
        asserteq(r, TF_AGAIN);

        /* feed 64 bytes (fits in initial 64) */
        for (i = 0; i < 64; i++) buf[i] = (unsigned char)'X';
        r = feed_seq(&S, &key, buf, 64);
        asserteq(r, TF_AGAIN);

        /* 65th byte triggers realloc -> OOM */
        r = feed_byte(&S, &key, 'Y');
        asserteq(r, TF_ERRMEM);
        asserteq(S.state, TF_STATE_IDLE);
        tf_free(&S);
    }
}

/* cs_buf must be NUL-terminated: tf_string consumers treat it as a C string */
TEST(cs_nul) {
    tf_State    S;
    tf_Key      key;
    const char *str;
    int         r, len;
    tf_init(&S, fill_alloc, NULL); /* new tail bytes are 0xA5 */
    r = feed_seq(&S, &key, "\x1b]0;hello\x07", 10);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    str = tf_string(&S, &len);
    asserteq(len, 7);
    asserteq((int)str[len], 0); /* pre-fix: 0xA5 beyond the content */
    assertstreq(str, "0;hello");
    tf_free(&S);

    /* kitty text shares the CS buffer: NUL-terminated as well */
    tf_init(&S, fill_alloc, NULL);
    r = feed_seq(&S, &key, "\x1b[97;2;65u", 10);
    asserteq(r, TF_OK);
    str = tf_string(&S, &len);
    asserteq(len, 1);
    asserteq((int)str[len], 0);
    assertstreq(str, "A");
    tf_free(&S);
}

/* --- mock terminfo lookup --- */

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

/* --- Phase 6: trie tests --- */

TEST(trie_match) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* \x1b\x7f -> BACKSPACE via trie (DSA would give DEL+ALT) */
    r = feed_seq(&S, &key, "\x1b\x7f", 2);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    asserteq(key.modifiers, 0);
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

TEST(trie_singlebyte) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_backspace", "\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* lone 0x7f -> BACKSPACE via trie (DSA would give DEL) */
    r = feed_seq(&S, &key, "\x7f", 1);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    asserteq(key.modifiers, 0);

    /* 0x08 -> BACKSPACE via trie (DSA would give CTRL+'h') */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    tbl[0].seq = "\x08";
    tf_load(&S, ti_lookup, tbl);
    r = feed_seq(&S, &key, "\x08", 1);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);

    /* plain 'a' still a character: trie dead end falls back to DSA */
    r = feed_seq(&S, &key, "a", 1);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');

    /* 0x7f after a trie dead end still matches (IDLE re-arms root) */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    tbl[0].seq = "\x7f";
    tf_load(&S, ti_lookup, tbl);
    r = feed_seq(&S, &key, "a\x7f", 2);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);

    tf_free(&S);
}

TEST(trie_fallback) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* \x1b[A -> DSA: UP (trie has no match for \x1bA) */
    r = feed_seq(&S, &key, "\x1b[A", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);

    tf_free(&S);
}

TEST(trie_aftermatch) {
    tf_State   S;
    tf_Key     key;
    int        r;
    MockReader mr;
    char       data[] = "\x1b\x7fx";
    TILookup   tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* \x1b\x7fx -> BACKSPACE, then 'x' (no misalignment) */
    mr.data = data, mr.len = 3, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    /* next read: 'x' */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'x');

    tf_free(&S);
}

TEST(trie_nolookup) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* no lookup -> everything works via DSA */
    r = feed_seq(&S, &key, "\x1b[A", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);

    tf_free(&S);
}

TEST(trie_idle_inactive) {
    tf_State   S;
    tf_Key     key;
    int        r;
    MockReader mr;
    char       data[] = "[1~";
    TILookup   tbl[] = {{"key_find", "\x1b[1~"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* "[1~" without ESC must NOT match trie: three plain chars */
    mr.data = data, mr.len = 3, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '[');
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '1');
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '~');

    tf_free(&S);
}

TEST(trie_shifted) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_btab", "\x1b[Z"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* \x1b[Z -> Shift+TAB via trie */
    r = feed_seq(&S, &key, "\x1b[Z", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    asserteq(key.modifiers, TF_MOD_SHIFT);

    tf_free(&S);
}

TEST(trie_fkeybreak) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_f1", "\x1b[OP"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* key_f1 loaded, f2 missing -> only f1 matches */
    r = feed_seq(&S, &key, "\x1bOP", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 1);

    /* key_f2 not loaded -> DSA handles \x1bOQ as SS3 F2 */
    r = feed_seq(&S, &key, "\x1bOQ", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 2);

    tf_free(&S);
}

TEST(trie_oom) {
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
        tf_load(&S, ti_lookup, tbl);
        r = feed_seq(&S, &key, "\x1b[A", 3);
        asserteq(r, TF_OK);
        asserteq((long)key.d.sym, (long)TF_SYM_UP);
        tf_free(&S);
    }

    /* no remaining allocations: success path */
    {
        int n = 100;
        tf_init(&S, oom_alloc, &n);
        tf_load(&S, ti_lookup, tbl);
        r = feed_seq(&S, &key, "\x1b\x7f", 2);
        asserteq(r, TF_OK);
        asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
        tf_free(&S);
    }
}

/* --- Phase 7: flush tests --- */

TEST(flush_csi_full) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1;2 -> CSI partial, flush */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '1');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ';');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '2');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CSI);

    /* flush -> ALT+[ + REPLAY_BUF of "1;2" */
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '[');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.replay, 3);

    /* replay: '1', ';', '2' */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, '1');

    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, ';');

    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, '2');
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* replay re-parses bytes through the DSA: an ESC inside the residual enters
 * the ESCAPE state (flush yields <Esc>, aligned with Nvim buffer re-parse) */
TEST(replay_escape) {
    tf_State   S;
    tf_Key     key;
    MockReader mr;
    int        r;

    tf_init(&S, NULL, NULL);

    /* \x1b[1;\x1b -> CSI partial with ESC inside params, flush -> replay */
    mr.data = "\x1b[1;\x1b", mr.len = 5, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    asserteq(r, TF_AGAIN);
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, '[');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.replay, 3);

    /* replay re-parses "1;\x1b": '1', ';' plain, then ESC -> ESCAPE state */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '1');
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, ';');
    r = tf_readkey(&S, &key);
    asserteq(r, TF_AGAIN); /* ESC pending, replay exhausted */
    asserteq(S.state, TF_STATE_ESCAPE);
    /* flush yields <Esc>, not UNICODE(0x1b) */
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);

    /* \eO + 'G' -> ALT+O, 'G' re-parsed plain */
    mr.data = "\x1bOG", mr.len = 3, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'O');
    asserteq(key.modifiers, TF_MOD_ALT);
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'G');

    tf_free(&S);
}

TEST(flush_ss3) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1bO -> SS3, flush */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'O');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_SS3);

    /* flush -> ALT+O, no replay */
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'O');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

TEST(flush_cs) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b]abc -> CS_OSC partial, flush */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    asserteq(r, TF_AGAIN); /* cs_buf allocated on first content byte */
    r = feed_byte(&S, &key, 'a');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'b');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'c');
    asserteq(r, TF_AGAIN);
    asserteq(S.state >= TF_STATE_CS_DCS && S.state <= TF_STATE_CS_APC, 1);

    /* flush -> ALT+] + IDLE re-parse of "abc" */
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, ']');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.state, TF_STATE_IDLE);

    /* re-parse: 'a', 'b', 'c' */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'a');
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'b');
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'c');
    asserteq(S.state, TF_STATE_IDLE);

    /* \x1b] (no content) -> flush -> ALT+] only, straight IDLE */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    asserteq(r, TF_AGAIN);
    asserteq(S.state >= TF_STATE_CS_DCS && S.state <= TF_STATE_CS_APC, 1);
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, ']');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.state, TF_STATE_IDLE);

    /* \x1bPabc -> CS_DCS, flush -> ALT+P + REPLAY */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'P');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'a');
    asserteq(r, TF_AGAIN);
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'P');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(tf_readkey(&S, &key), TF_OK);
    asserteq(key.d.codepoint, 'a');

    /* \x1b_abc -> CS_APC, flush -> ALT+_ + REPLAY */
    tf_free(&S);
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '_');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'a');
    asserteq(r, TF_AGAIN);
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, '_');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(tf_readkey(&S, &key), TF_OK);
    asserteq(key.d.codepoint, 'a');

    tf_free(&S);
}

TEST(flush_utf8) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* cross-chunk UTF-8: lead byte only -> flush -> FFFD */
    r = feed_byte(&S, &key, 0xC3);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_UTF8);

    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0xFFFD);
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

TEST(flush_mousex10) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[M -> MOUSE_X10 (no raw bytes yet), flush */
    r = feed_seq(&S, &key, "\x1b[M", 3);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_MOUSE_X10);

    /* flush -> ALT+[ + REPLAY_BUF ("M" prepended, buf empty here) */
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '[');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.replay, 1);

    /* replay 'M' */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'M');
    /* buf had no raw bytes -> after 'M', straight to IDLE */
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

TEST(flush_mousex10_raw) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b[M + 1 raw byte -> MOUSE_X10 (1 raw byte in buf) */
    r = feed_seq(&S, &key, "\x1b[M", 3);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x20);
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_MOUSE_X10);
    asserteq(S.buf_len, 2); /* 'M' + 1 raw byte */

    /* flush -> ALT+[ + REPLAY_BUF("M" + 0x20) */
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, '[');
    asserteq(key.modifiers, TF_MOD_ALT);

    /* replay 'M' */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'M');
    asserteq(S.replay, 1); /* REPLAY_BUF: 1 raw byte pending */

    /* replay raw byte -> UNICODE(0x20) */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE); /* 0x20 < 0x7F -> UNICODE */
    asserteq(key.d.codepoint, 0x20);
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

TEST(flush_replay) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* flush during replay (replay pending, state IDLE) -> no-op */
    S.replay = 2;
    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_NONE);

    tf_free(&S);
}

TEST(flush_escapealt_csi) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* \x1b\x1b[1 -> flush -> ALT+[ (alt_pending consumed in flush) */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_byte(&S, &key, '[');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '1');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CSI);

    r = tf_flush(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '[');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.replay, 1); /* "1" to replay */

    tf_free(&S);
}

TEST(replay_seq) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);

    /* set up replay source with 3 bytes: 'a', 0x80, 'b' */
    S.replay = 3;
    S.buf[TF_MAX_BUFLEN - 3] = 'a';
    S.buf[TF_MAX_BUFLEN - 2] = test_byte(0x80);
    S.buf[TF_MAX_BUFLEN - 1] = 'b';
    S.buf_len = 0;

    /* 'a' -> UNICODE('a') */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');

    /* 0x80 -> 0xFFFD */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0xFFFD);

    /* 'b' -> UNICODE('b') */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'b');

    /* after last byte -> IDLE */
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* tf_load reports trie-build OOM instead of swallowing it */
TEST(trie_load_oom) {
    tf_State S;
    int      n;
    TILookup tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    TILookup f1only[] = {{"key_f1", "\x1bOP"}, {NULL, NULL}};
    /* loadtable path: first trie allocation fails */
    n = 0;
    tf_init(&S, oom_alloc, &n);
    asserteq(tf_load(&S, ti_lookup, tbl), TF_ERRMEM);
    tf_free(&S);
    /* loadfkeys path (no keytable entries present) */
    n = 0;
    tf_init(&S, oom_alloc, &n);
    asserteq(tf_load(&S, ti_lookup, f1only), TF_ERRMEM);
    tf_free(&S);
    /* success path */
    n = 100;
    tf_init(&S, oom_alloc, &n);
    asserteq(tf_load(&S, ti_lookup, tbl), TF_OK);
    tf_free(&S);
}

TEST(trie_load_params) {
    tf_State S;
    tf_init(&S, NULL, NULL);

    /* tf_load with NULL -> TF_ERRPARAM, no crash, trie untouched */
    asserteq(tf_load(NULL, ti_lookup, NULL), TF_ERRPARAM);
    asserteq(tf_load(&S, NULL, NULL), TF_ERRPARAM);
    asserteq(S.root == NULL, 1);

    /* lookup that stops after the first F-key */
    {
        TILookup f1only[] = {{"key_f1", "\x1bOP"}, {NULL, NULL}};
        tf_load(&S, ti_lookup, f1only);
        asserteq(S.root != NULL, 1);
    }

    /* full F1-F63 table -> all keys loaded */
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
        tf_load(&S, ti_lookup, fk);
        asserteq(S.root != NULL, 1);
    }

    tf_free(&S);
}

/* --- Phase 6+7: combined trie + flush --- */

TEST(trie_withalt) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_backspace", "\x1b\x7f"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* \x1b\x1b\x7f -> BACKSPACE + ALT (alt_pending merged) */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    asserteq(S.pending_mod, TF_MOD_ALT);
    r = feed_byte(&S, &key, 0x7f);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

TEST(trie_multikey) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {
            {"key_f1", "\x1bOP"},
            {"key_home", "\x1bOH"},
            {"key_end", "\x1bOF"},
            {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    r = feed_seq(&S, &key, "\x1bOP", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 1);

    r = feed_seq(&S, &key, "\x1bOH", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    r = feed_seq(&S, &key, "\x1bOF", 3);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_END);

    tf_free(&S);
}

/* test trie extent expansion: keys with different first bytes */
TEST(trie_expand) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_f1", "\x1bOP"}, {"key_up", "\x1b[A"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* \x1bOP -> F1 (first byte after \x1b = 'O') */
    r = feed_seq(&S, &key, "\x1bOP", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 1);

    /* \x1b[A -> UP (first byte after \x1b = '[' - expands extent left) */
    r = feed_seq(&S, &key, "\x1b[A", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);

    tf_free(&S);
}

/* test trie with custom allocator */
TEST(trie_alloc) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {{"key_home", "\x1bOH"}, {NULL, NULL}};
    tf_init(&S, test_alloc, NULL);
    tf_load(&S, ti_lookup, tbl);

    r = feed_seq(&S, &key, "\x1bOH", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_HOME);

    tf_free(&S);
}

TEST(trie_allocfree) {
    tf_State S;
    TILookup tbl[] = {{"key_home", "\x1bOH"}, {NULL, NULL}};
    tf_init(&S, test_alloc, NULL);
    tf_load(&S, ti_lookup, tbl);
    assertok(S.root != NULL);
    tf_free(&S); /* frees trie via test_alloc */
}

/* repeated tf_load must not leak the previous trie */
TEST(trie_reload) {
    tf_State S;
    tf_Key   key;
    Count    c = {0};
    int      r;
    TILookup tbl[] = {
            {"key_home", "\x1b[1~"}, {"key_left", "\x1b[D"}, {NULL, NULL}};
    tf_init(&S, count_alloc, &c);
    tf_load(&S, ti_lookup, tbl);
    tf_load(&S, ti_lookup, tbl);
    assertok(c.live > 0);
    tf_free(&S);
    asserteq(c.live, 0); /* leaked trie nodes keep live > 0 */
    /* reloaded trie still matches */
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);
    r = feed_seq(&S, &key, "\x1b[1~", 4);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_HOME);
    tf_free(&S);
}

/* a terminfo key that is a strict prefix of a later one must not abort
 * the load (KEY node used as trie interior) */
TEST(trie_prefix) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {
            {"key_home", "\x1b[2"}, {"key_left", "\x1b[2x"}, {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl); /* pre-fix: assert abort */
    r = feed_seq(&S, &key, "\x1b[2", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_HOME);
    tf_free(&S);
}

/* --- Phase 8: canonicalise flags --- */

TEST(canon_delbs) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_DELBS);

    /* 0x7f -> BACKSPACE (DELBS on) */
    r = feed_byte(&S, &key, 0x7f);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    asserteq(key.modifiers, 0);

    /* 0x08 -> still Ctrl+H (DELBS does not affect 0x08) */
    r = feed_byte(&S, &key, 0x08);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'h');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* DELBS off: 0x7f -> DEL */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x7f);
    asserteq(r, TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    /* \x1b + 0x7f with DELBS: ALT+BACKSPACE */
    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_DELBS);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x7f);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);
    asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

TEST(canon_spacesym) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_SPACESYMBOL);

    /* 0x20 -> SPACE keysym */
    r = feed_byte(&S, &key, 0x20);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    asserteq(key.modifiers, 0);

    /* SPACESYMBOL off: 0x20 -> UNICODE(0x20) */
    tf_init(&S, NULL, NULL);
    r = feed_byte(&S, &key, 0x20);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0x20);

    /* \x1b + 0x20 with SPACESYMBOL: ALT+SPACE */
    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_SPACESYMBOL);
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 0x20);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    asserteq(key.modifiers, TF_MOD_ALT);

    tf_free(&S);
}

TEST(canon_convertkp) {
    tf_State S;
    tf_Key   key;
    int      r;

    /* Verify CONVERTKP behavior (already implemented, regression test) */
    tf_init(&S, NULL, NULL);
    tf_setflag(&S, TF_FLAG_CONVERTKP);

    r = feed_seq(&S, &key, "\x1bOp", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '0');

    /* KPENTER stays KPENTER */
    r = feed_seq(&S, &key, "\x1bOM", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_KPENTER);

    tf_free(&S);
}

/* --- Phase 8: sym name table --- */

TEST(symname_basic) {
    asserteq(tf_name((int)TF_SYM_NONE), NULL);

    assertstreq(tf_name((int)TF_SYM_BACKSPACE), "Backspace");
    assertstreq(tf_name((int)TF_SYM_TAB), "Tab");
    assertstreq(tf_name((int)TF_SYM_ENTER), "Enter");
    assertstreq(tf_name((int)TF_SYM_ESCAPE), "Escape");
    assertstreq(tf_name((int)TF_SYM_SPACE), "Space");
    assertstreq(tf_name((int)TF_SYM_DELETE), "Delete");

    assertstreq(tf_name((int)TF_SYM_UP), "Up");
    assertstreq(tf_name((int)TF_SYM_DOWN), "Down");
    assertstreq(tf_name((int)TF_SYM_LEFT), "Left");
    assertstreq(tf_name((int)TF_SYM_RIGHT), "Right");
}

TEST(symname_editing) {
    assertstreq(tf_name((int)TF_SYM_BEGIN), "Begin");
    assertstreq(tf_name((int)TF_SYM_FIND), "Find");
    assertstreq(tf_name((int)TF_SYM_INSERT), "Insert");
    assertstreq(tf_name((int)TF_SYM_DELETE), "Delete");
    assertstreq(tf_name((int)TF_SYM_SELECT), "Select");
    assertstreq(tf_name((int)TF_SYM_PAGEUP), "PageUp");
    assertstreq(tf_name((int)TF_SYM_PAGEDOWN), "PageDown");
    assertstreq(tf_name((int)TF_SYM_HOME), "Home");
    assertstreq(tf_name((int)TF_SYM_END), "End");
}

TEST(symname_terminfo) {
    assertstreq(tf_name((int)TF_SYM_CANCEL), "Cancel");
    assertstreq(tf_name((int)TF_SYM_CLEAR), "Clear");
    assertstreq(tf_name((int)TF_SYM_CLOSE), "Close");
    assertstreq(tf_name((int)TF_SYM_COMMAND), "Command");
    assertstreq(tf_name((int)TF_SYM_COPY), "Copy");
    assertstreq(tf_name((int)TF_SYM_EXIT), "Exit");
    assertstreq(tf_name((int)TF_SYM_HELP), "Help");
    assertstreq(tf_name((int)TF_SYM_MARK), "Mark");
    assertstreq(tf_name((int)TF_SYM_MESSAGE), "Message");
    assertstreq(tf_name((int)TF_SYM_MOVE), "Move");
    assertstreq(tf_name((int)TF_SYM_OPEN), "Open");
    assertstreq(tf_name((int)TF_SYM_OPTIONS), "Options");
    assertstreq(tf_name((int)TF_SYM_PRINT), "Print");
    assertstreq(tf_name((int)TF_SYM_REDO), "Redo");
    assertstreq(tf_name((int)TF_SYM_REFERENCE), "Reference");
    assertstreq(tf_name((int)TF_SYM_REFRESH), "Refresh");
    assertstreq(tf_name((int)TF_SYM_REPLACE), "Replace");
    assertstreq(tf_name((int)TF_SYM_RESTART), "Restart");
    assertstreq(tf_name((int)TF_SYM_RESUME), "Resume");
    assertstreq(tf_name((int)TF_SYM_SAVE), "Save");
    assertstreq(tf_name((int)TF_SYM_SUSPEND), "Suspend");
    assertstreq(tf_name((int)TF_SYM_UNDO), "Undo");
}

TEST(symname_kp) {
    assertstreq(tf_name((int)TF_SYM_KP0), "k0");
    assertstreq(tf_name((int)TF_SYM_KP1), "k1");
    assertstreq(tf_name((int)TF_SYM_KP9), "k9");
    assertstreq(tf_name((int)TF_SYM_KPENTER), "kEnter");
    assertstreq(tf_name((int)TF_SYM_KPPLUS), "kPlus");
    assertstreq(tf_name((int)TF_SYM_KPMINUS), "kMinus");
    assertstreq(tf_name((int)TF_SYM_KPMULT), "kMultiply");
    assertstreq(tf_name((int)TF_SYM_KPDIV), "kDivide");
    assertstreq(tf_name((int)TF_SYM_KPCOMMA), "kComma");
    assertstreq(tf_name((int)TF_SYM_KPPERIOD), "kPoint");
    assertstreq(tf_name((int)TF_SYM_KPEQUALS), "kEqual");
}

TEST(symname_oob) {
    /* out of bounds -> NULL */
    asserteq(tf_name(-1), NULL);
    asserteq(tf_name(TF_SYM_COUNT), NULL);
    asserteq(tf_name(9999), NULL);
}

TEST(sym_lookup) {
    /* tf_sym: name -> sym index */
    asserteq(tf_sym("Backspace"), (int)TF_SYM_BACKSPACE);
    asserteq(tf_sym("Tab"), (int)TF_SYM_TAB);
    asserteq(tf_sym("Enter"), (int)TF_SYM_ENTER);
    asserteq(tf_sym("Escape"), (int)TF_SYM_ESCAPE);
    asserteq(tf_sym("Space"), (int)TF_SYM_SPACE);
    asserteq(tf_sym("Delete"), (int)TF_SYM_DELETE);
    asserteq(tf_sym("Up"), (int)TF_SYM_UP);
    asserteq(tf_sym("Down"), (int)TF_SYM_DOWN);
    asserteq(tf_sym("PageUp"), (int)TF_SYM_PAGEUP);
    asserteq(tf_sym("PageDown"), (int)TF_SYM_PAGEDOWN);
    asserteq(tf_sym("kEnter"), (int)TF_SYM_KPENTER);
    asserteq(tf_sym("kPlus"), (int)TF_SYM_KPPLUS);

    /* not found -> -1 */
    asserteq(tf_sym("NoSuchKey"), -1);
    asserteq(tf_sym(""), -1);
    asserteq(tf_sym(NULL), -1);

    /* case-insensitive */
    asserteq(tf_sym("backspace"), (int)TF_SYM_BACKSPACE);
    asserteq(tf_sym("PAGEUP"), (int)TF_SYM_PAGEUP);
    asserteq(tf_sym("kEnter"), (int)TF_SYM_KPENTER);
    /* libtermkey legacy names are aliases in tf_parse, not tf_sym */
    asserteq(tf_sym("KPEnter"), -1);
    {
        tf_Key key;
        asserteq(tf_parse("<KPEnter>", &key), 9);
        asserteq((long)key.d.sym, (long)TF_SYM_KPENTER);
    }
}

TEST(sym_roundtrip) {
    int i;
    /* all syms except NONE round-trip */
    for (i = (int)TF_SYM_BACKSPACE; i < TF_SYM_COUNT; i++) {
        const char *name = tf_name(i);
        assertok(name != NULL);
        asserteq(tf_sym(name), i);
    }
}

/* --- Phase 9: tf_format --- */

TEST(fmt_basic) {
    tf_Key key;
    char   buf[64];
    int    n;

    memset(&key, 0, sizeof(key));

    /* KEYSYM */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_ESCAPE;
    n = tf_format(buf, sizeof(buf), &key, 0);
    asserteq(n, 8);
    assertstreq(buf, "<Escape>");

    /* FUNCTION */
    key.type = TF_TYPE_FUNCTION;
    key.d.number = 1;
    n = tf_format(buf, sizeof(buf), &key, 0);
    asserteq(n, 4);
    assertstreq(buf, "<F1>");

    /* UNICODE printable */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'a';
    key.modifiers = 0;
    n = tf_format(buf, sizeof(buf), &key, 0);
    asserteq(n, 1);
    assertstreq(buf, "a");

    /* invalid sym value -> empty name, no crash */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = (tf_Sym)9999;
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    asserteq(n, 2);
    assertstreq(buf, "<>");
}

TEST(fmt_mods) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    /* C-x -> <C-x> (default: no SPACEMOD) */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<C-x>");

    /* S-A-C-x -> <S-M-C-x> (default: ALTISMETA) */
    key.modifiers = TF_MOD_SHIFT | TF_MOD_ALT | TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<S-M-C-x>");

    /* D-x -> <D-x> */
    key.modifiers = TF_MOD_SUPER;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<D-x>");

    /* T-x -> <T-x> */
    key.modifiers = TF_MOD_META;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<T-x>");

    /* Without SPACEMOD, no WRAPBRACKET: C-x (bare) */
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<C-x>");

    /* CARETCTRL: C-x -> ^X */
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_CARETCTRL | TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<^X>");

    /* LONGMOD: C-x -> Control-x */
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_LONGMOD | TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<Control-x>");

    /* ALTISMETA: A- -> M- */
    key.modifiers = TF_MOD_ALT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_ALTISMETA | TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<M-x>");
}

TEST(fmt_spacing) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    /* SPACEMOD: C-x -> C x */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET | TF_FMT_SPACEMOD);
    assertstreq(buf, "<C x>");

    /* LOWERMOD: D-C-X -> d-c-X (default: no LOWERMOD means uppercase mods) */
    key.modifiers = TF_MOD_CTRL | TF_MOD_SUPER;
    key.d.codepoint = 'X';
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<D-C-X>");

    /* LOWERSPACE: PageUp -> page up */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_PAGEUP;
    key.modifiers = 0;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET | TF_FMT_LOWERSPACE);
    assertstreq(buf, "<page up>");
}

TEST(fmt_kitty) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    /* KEYSYM Escape -> <Escape> */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_ESCAPE;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<Escape>");

    /* FUNCTION 13 -> <F13> */
    key.type = TF_TYPE_FUNCTION;
    key.d.number = 13;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<F13>");

    /* KEYSYM KP0 -> <k0> */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_KP0;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<k0>");

    /* KEYSYM LEVEL5SHIFT -> <Level5Shift> */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_LEVEL5_SHIFT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<Level5Shift>");

    /* KEYSYM KPLEFT -> <kLeft> */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_KPLEFT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<kLeft>");
}

TEST(fmt_mouse) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    /* MOUSE: plain name, data goes through tf_mouse() */
    key.type = TF_TYPE_MOUSE;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<Mouse>");
}

TEST(fmt_types) {
    tf_Key key;
    char   buf[64];

    memset(&key, 0, sizeof(key));

    key.type = TF_TYPE_POSITION;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<Position>");

    key.type = TF_TYPE_MODEREPORT;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<ModeReport>");

    key.type = TF_TYPE_KITTYREPORT;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<KittyReport>");

    key.type = TF_TYPE_DCS;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<DCS>");

    key.type = TF_TYPE_OSC;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<OSC>");

    key.type = TF_TYPE_APC;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<APC>");

    key.type = TF_TYPE_UNKNOWN_CSI;
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<Unknown CSI>");
}

TEST(fmt_trunc) {
    tf_Key key;
    char   buf[8];
    int    n;

    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_ESCAPE;

    /* buffer too small: truncates */
    n = tf_format(buf, 5, &key, 0);
    asserteq(n, 4); /* "<Esc" + NUL */
    assertstreq(buf, "<Esc");

    n = tf_format(buf, 2, &key, 0);
    asserteq(n, 1); /* "<", truncated */
    assertstreq(buf, "<");

    n = tf_format(buf, 1, &key, 0);
    asserteq(n, 0);
}

TEST(fmt_params) {
    char buf[16];
    int  n;

    tf_Key key;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'a';

    n = tf_format(buf, sizeof(buf), NULL, 0);
    asserteq(n, TF_ERRPARAM);

    n = tf_format(NULL, sizeof(buf), NULL, 0);
    asserteq(n, TF_ERRPARAM);

    n = tf_format(NULL, sizeof(buf), &key, 0);
    asserteq(n, TF_ERRPARAM);

    n = tf_format(buf, 0, &key, 0);
    asserteq(n, TF_ERRPARAM);

    n = tf_format(buf, -1, &key, 0);
    asserteq(n, TF_ERRPARAM);
}

/* --- Phase 9: tf_parse --- */

TEST(parse_basic) {
    tf_Key key;
    int    n;

    /* <C-x> -> Ctrl+'x' */
    n = tf_parse("<C-x>", &key);
    asserteq(n, 5);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* <Escape> -> KEYSYM ESCAPE */
    n = tf_parse("<Escape>", &key);
    asserteq(n, 8);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);

    /* <F1> -> FUNCTION 1 */
    n = tf_parse("<F1>", &key);
    asserteq(n, 4);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 1);

    /* <F63> -> FUNCTION 63 */
    n = tf_parse("<F63>", &key);
    asserteq(n, 5);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 63);

    /* <F99> out of range -> parse fails (falls through to char check) */
    n = tf_parse("<F99>", &key);
    asserteq(n, -1);
}

TEST(parse_mods) {
    tf_Key key;
    int    n;

    /* <S-A-C-x> */
    n = tf_parse("<S-A-C-x>", &key);
    asserteq(n, 9);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_SHIFT | TF_MOD_ALT | TF_MOD_CTRL);

    /* <D-T-x> */
    n = tf_parse("<D-T-x>", &key);
    asserteq(n, 7);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_SUPER | TF_MOD_META);

    /* <M-Left> -> M=ALT + Left */
    n = tf_parse("<M-Left>", &key);
    asserteq(n, 8);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_LEFT);
    asserteq(key.modifiers, TF_MOD_ALT);

    /* <C-S-Tab> */
    n = tf_parse("<C-S-Tab>", &key);
    asserteq(n, 9);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_TAB);
    asserteq(key.modifiers, TF_MOD_CTRL | TF_MOD_SHIFT);

    /* double separator: <S--x> -> Shift + 'x' */
    n = tf_parse("<S--x>", &key);
    asserteq(n, 6);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_SHIFT);
}

TEST(parse_case) {
    tf_Key key;
    int    n;

    /* case-insensitive key names */
    n = tf_parse("<pageup>", &key);
    asserteq(n, 8);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_PAGEUP);

    /* space in key name */
    n = tf_parse("<Page Up>", &key);
    asserteq(n, 9);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq(
            (long)key.d.sym, (long)TF_SYM_PAGEUP); /* tf_sym case-insensitive */

    /* <C-s> = Ctrl+'s' (lowercase s = keyname) */
    n = tf_parse("<C-s>", &key);
    asserteq(n, 5);
    asserteq(key.d.codepoint, 's');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* <C-S-x> = Ctrl+Shift+'x' */
    n = tf_parse("<C-S-x>", &key);
    asserteq(n, 7);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_CTRL | TF_MOD_SHIFT);

    /* <C-C> = Ctrl+'C': single-letter key name must not be eaten as a
     * modifier (modifier name only counts when followed by '-'/' ') */
    n = tf_parse("<C-C>", &key);
    asserteq(n, 5);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'C');
    asserteq(key.modifiers, TF_MOD_CTRL);
}

TEST(parse_caret) {
    tf_Key key;
    int    n;

    /* <^X> = Ctrl+'X' (^ + uppercase letter) */
    n = tf_parse("<^X>", &key);
    asserteq(n, 4);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'X');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* <^x> = fail (^ must be followed by uppercase letter) */
    n = tf_parse("<^x>", &key);
    asserteq(n, -1);
}

TEST(parse_kitty) {
    tf_Key key;
    int    n;

    /* <Esc> -> TF_SYM_ESCAPE (kitty name maps to sym) */
    n = tf_parse("<Esc>", &key);
    asserteq(n, 5);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);

    /* <BS> -> TF_SYM_BACKSPACE */
    n = tf_parse("<BS>", &key);
    asserteq(n, 4);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);

    /* <CR> -> TF_SYM_ENTER */
    n = tf_parse("<CR>", &key);
    asserteq(n, 4);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ENTER);

    /* <k0> -> TF_SYM_KP0 (kitty keypad name alias) */
    n = tf_parse("<k0>", &key);
    asserteq(n, 4);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_KP0);

    /* <Del> -> TF_SYM_DELETE (Vim name alias) */
    n = tf_parse("<Del>", &key);
    asserteq(n, 5);
    asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    /* <kPoint> -> TF_SYM_KPPERIOD */
    n = tf_parse("<kPoint>", &key);
    asserteq(n, 8);
    asserteq((long)key.d.sym, (long)TF_SYM_KPPERIOD);

    /* <kLeft> -> TF_SYM_KPLEFT (primary name) */
    n = tf_parse("<kLeft>", &key);
    asserteq(n, 7);
    asserteq((long)key.d.sym, (long)TF_SYM_KPLEFT);

    /* <CapsLock> -> TF_SYM_CAPSLOCK */
    n = tf_parse("<CapsLock>", &key);
    asserteq(n, 10);
    asserteq((long)key.d.sym, (long)TF_SYM_CAPSLOCK);
}

TEST(parse_nobracket) {
    tf_Key key;
    int    n;

    /* plain 'a' -> UNICODE 'a' */
    n = tf_parse("a", &key);
    asserteq(n, 1);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.modifiers, 0);

    /* multi-byte UTF-8: 'e' (0xC3 0xA9) */
    n = tf_parse("\xc3\xa9", &key);
    asserteq(n, 2);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0xE9);
}

TEST(parse_fail) {
    tf_Key key;
    int    n;

    /* single letter without separator is a key name: <S> -> 'S' (no modifier)
     */
    n = tf_parse("<S>", &key);
    asserteq(n, 3);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'S');
    asserteq(key.modifiers, 0);

    /* invalid: <c-x> -- lowercase c not a modifier */
    n = tf_parse("<c-x>", &key);
    asserteq(n, -1);

    /* <C> -> key name 'C' (modifier requires a separator) */
    n = tf_parse("<C>", &key);
    asserteq(n, 3);
    asserteq(key.d.codepoint, 'C');
    asserteq(key.modifiers, 0);

    /* <^A without closing '>' -> fail */
    n = tf_parse("<^A", &key);
    asserteq(n, -1);

    /* overlong UTF-8 in plain string -> FFFD + fail */
    n = tf_parse("\xc0\xaf", &key);
    asserteq(n, -1);

    /* empty string */
    n = tf_parse("", &key);
    asserteq(n, -1);

    /* NULL */
    n = tf_parse(NULL, &key);
    asserteq(n, -1);

    /* key NULL */
    n = tf_parse("a", NULL);
    asserteq(n, -1);
}

TEST(parse_longmod) {
    tf_Key key;
    int    n;

    /* <Control-x> */
    n = tf_parse("<Control-x>", &key);
    asserteq(n, 11);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* <Shift-Up> */
    n = tf_parse("<Shift-Up>", &key);
    asserteq(n, 10);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(key.modifiers, TF_MOD_SHIFT);

    /* <Super-x> (S-longname = Super) */
    n = tf_parse("<Super-x>", &key);
    asserteq(n, 9);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_SUPER);
}

TEST(parse_roundtrip) {
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
    assertok(n > 0);
    asserteq(k2.type, k1.type);
    asserteq(k2.d.codepoint, k1.d.codepoint);
    asserteq(k2.modifiers, k1.modifiers);

    /* round-trip: <Escape> */
    memset(&k1, 0, sizeof(k1));
    k1.type = TF_TYPE_KEYSYM;
    k1.d.sym = TF_SYM_ESCAPE;
    n = tf_format(buf, sizeof(buf), &k1, TF_FMT_WRAPBRACKET);
    n = tf_parse(buf, &k2);
    assertok(n > 0);
    asserteq(k2.type, k1.type);
    asserteq((long)k2.d.sym, (long)k1.d.sym);

    /* round-trip: <F1> */
    memset(&k1, 0, sizeof(k1));
    k1.type = TF_TYPE_FUNCTION;
    k1.d.number = 1;
    n = tf_format(buf, sizeof(buf), &k1, TF_FMT_WRAPBRACKET);
    n = tf_parse(buf, &k2);
    assertok(n > 0);
    asserteq(k2.type, k1.type);
    asserteq(k2.d.number, k1.d.number);

    /* round-trip: <D-S-F12> */
    memset(&k1, 0, sizeof(k1));
    k1.type = TF_TYPE_FUNCTION;
    k1.d.number = 12;
    k1.modifiers = TF_MOD_SUPER | TF_MOD_SHIFT;
    n = tf_format(buf, sizeof(buf), &k1, TF_FMT_WRAPBRACKET);
    n = tf_parse(buf, &k2);
    assertok(n > 0);
    asserteq(k2.type, k1.type);
    asserteq(k2.d.number, k1.d.number);
    asserteq(k2.modifiers, k1.modifiers);
}

TEST(parse_bracketchar) {
    tf_Key key;
    int    n;

    /* <x> = single char 'x' */
    n = tf_parse("<x>", &key);
    asserteq(n, 3);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'x');

    /* <<> = fail (no valid key name) */
    n = tf_parse("<<>", &key);
    asserteq(n, -1);
}

/* --- Phase 10: Kitty CSI u --- */

TEST(kitty_basic) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97u -> 'a' */
    asserteq(feed_seq(&S, &key, "\x1b[97u", 5), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.modifiers, 0);
    asserteq(key.event, TF_EVENT_PRESS);
    assertstreq(key.utf8, "a");
    tf_free(&S);
}

TEST(kitty_modifiers) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97;2u -> 'a' + SHIFT */
    asserteq(feed_seq(&S, &key, "\x1b[97;2u", 7), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.modifiers, TF_MOD_SHIFT);
    asserteq(key.event, TF_EVENT_PRESS);
    tf_free(&S);
}

TEST(kitty_event) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97;5:2u -> 'a' + CTRL + REPEAT */
    asserteq(feed_seq(&S, &key, "\x1b[97;5:2u", 9), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.modifiers, TF_MOD_CTRL);
    asserteq(key.event, TF_EVENT_REPEAT);

    /* default event (no colon) -> PRESS */
    tf_init(&S, NULL, NULL);
    asserteq(feed_seq(&S, &key, "\x1b[97;2u", 7), TF_OK);
    asserteq(key.event, TF_EVENT_PRESS);
    tf_free(&S);
}

TEST(kitty_event_fail) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97;1:9u -> UNKNOWN_CSI (invalid event) */
    asserteq(feed_seq(&S, &key, "\x1b[97;1:9u", 9), TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    tf_free(&S);
}

TEST(kitty_alts) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[97:65;2u -> 'a' + SHIFT (alts ignored) */
    asserteq(feed_seq(&S, &key, "\x1b[97:65;2u", 10), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.modifiers, TF_MOD_SHIFT);
    asserteq(key.event, TF_EVENT_PRESS);
    tf_free(&S);
}

TEST(kitty_text) {
    tf_State S;
    tf_Key   key;
    int      slen;
    tf_init(&S, NULL, NULL);
    /* \x1b[97;2;65u -> 'a' + SHIFT + text "A" */
    asserteq(feed_seq(&S, &key, "\x1b[97;2;65u", 10), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.modifiers, TF_MOD_SHIFT);
    {
        const char *s = tf_string(&S, &slen);
        asserteq(slen, 1);
        asserteq(memcmp(s, "A", 1), 0);
    }
    tf_free(&S);
}

TEST(kitty_text_multi) {
    tf_State S;
    tf_Key   key;
    int      slen;
    tf_init(&S, NULL, NULL);
    /* \x1b[65;1;99:100u -> 'A' + text "cd" */
    asserteq(feed_seq(&S, &key, "\x1b[65;1;99:100u", 14), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'A');
    asserteq(key.modifiers, 0);
    {
        const char *s = tf_string(&S, &slen);
        asserteq(slen, 2);
        asserteq(memcmp(s, "cd", 2), 0);
    }
    tf_free(&S);
}

TEST(kitty_func) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[57344u -> KEYSYM ESCAPE (kitty functional key) */
    asserteq(feed_seq(&S, &key, "\x1b[57344u", 8), TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);
    tf_free(&S);
}

TEST(kitty_map) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);

    /* C0 codepoint: \e[27u -> KEYSYM ESCAPE */
    asserteq(feed_seq(&S, &key, "\x1b[27u", 5), TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);

    /* \e[9u -> TAB */
    asserteq(feed_seq(&S, &key, "\x1b[9u", 5), TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_TAB);

    /* \e[13u -> ENTER */
    asserteq(feed_seq(&S, &key, "\x1b[13u", 5), TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_ENTER);

    /* \e[127u -> BACKSPACE (kitty backspace key, not DEL) */
    asserteq(feed_seq(&S, &key, "\x1b[127u", 6), TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_BACKSPACE);

    /* kitty Delete key with modifier: \e[57349;2u -> DELETE + SHIFT */
    asserteq(feed_seq(&S, &key, "\x1b[57349;2u", 10), TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_DELETE);
    asserteq(key.modifiers, TF_MOD_SHIFT);

    /* F13: \e[57376u -> FUNCTION 13 */
    asserteq(feed_seq(&S, &key, "\x1b[57376u", 8), TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 13);

    /* KP0: \e[57399u -> KEYSYM KP0 */
    asserteq(feed_seq(&S, &key, "\x1b[57399u", 8), TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_KP0);

    /* kLeft: \e[57417u -> KEYSYM KPLEFT */
    asserteq(feed_seq(&S, &key, "\x1b[57417u", 8), TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_KPLEFT);

    /* media key: \e[57428u -> KEYSYM MEDIAPLAY */
    asserteq(feed_seq(&S, &key, "\x1b[57428u", 8), TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_MEDIA_PLAY);

    /* modifier key: \e[57454u -> KEYSYM LEVEL5SHIFT */
    asserteq(feed_seq(&S, &key, "\x1b[57454u", 8), TF_OK);
    asserteq((long)key.d.sym, (long)TF_SYM_LEVEL5_SHIFT);

    /* unknown PUA codepoint stays UNICODE */
    asserteq(feed_seq(&S, &key, "\x1b[63743u", 8), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 63743);

    tf_free(&S);
}

TEST(ss3_altreplay) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1bOG (unknown SS3) -> ALT+O, then G replayed */
    asserteq(feed_seq(&S, &key, "\x1bOG", 3), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'O');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(tf_readkey(&S, &key), TF_OK);
    asserteq(key.d.codepoint, 'G');
    asserteq(key.modifiers, 0);
    tf_free(&S);
}

TEST(csi_altprefix) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b\x1b[A -> ALT+UP (alt_pending merged at CSI dispatch) */
    asserteq(feed_byte(&S, &key, 0x1b), TF_AGAIN);
    asserteq(feed_byte(&S, &key, 0x1b), TF_AGAIN);
    asserteq(feed_seq(&S, &key, "[A", 2), TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    asserteq(key.modifiers, TF_MOD_ALT);
    tf_free(&S);
}

TEST(kitty_empty_cp) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[;2u -> UNKNOWN_CSI (empty codepoint) */
    asserteq(feed_seq(&S, &key, "\x1b[;2u", 5), TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    tf_free(&S);
}

TEST(kitty_widecp) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* \x1b[233u -> 'e' (U+00E9, 2-byte UTF-8) */
    asserteq(feed_seq(&S, &key, "\x1b[233u", 7), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 233);
    asserteq(key.modifiers, 0);
    assertstreq(key.utf8, "\xc3\xa9");

    /* \x1b[0x1F600u -> emoji (4-byte UTF-8, kitty uses decimal) */
    tf_init(&S, NULL, NULL);
    asserteq(feed_seq(&S, &key, "\x1b[128512u", 9), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 0x1F600);
    assertstreq(key.utf8, "\xf0\x9f\x98\x80");
    tf_free(&S);
}

TEST(kitty_text_long) {
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
    asserteq(feed_seq(&S, &key, seq, pos), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    asserteq(key.modifiers, TF_MOD_SHIFT);
    {
        const char *s = tf_string(&S, &slen);
        assertok(s != NULL);
        asserteq(slen, 15);
    }
    tf_free(&S);
}

TEST(csi_overflow) {
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
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '[');
    asserteq(key.modifiers, TF_MOD_ALT);
    asserteq(S.replay, TF_MAX_BUFLEN); /* REPLAY_BUF: full buf */

    /* replay first byte of buf */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, seq[2]);
    asserteq(S.replay, TF_MAX_BUFLEN - 1);

    /* drain replay + chunk remainder until 'x' (last byte) */
    do {
        r = tf_readkey(&S, &key);
        asserteq(r, TF_OK);
    } while (key.d.codepoint != 'x');
    asserteq(S.state, TF_STATE_IDLE);
    tf_free(&S);
}

TEST(kitty_text_oom) {
    /* OOM during cs_buf allocation for text */
    tf_State S;
    tf_Key   key;
    int      oomcnt = 0;
    tf_init(&S, oom_alloc, &oomcnt);
    asserteq(feed_seq(&S, &key, "\x1b[97;2;65u", 10), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    /* key should still be valid even without text */
    tf_free(&S);
}

TEST(kitty_params) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* NULL/zero checks: feed_seq with empty seq */
    asserteq(feed_seq(&S, &key, "", 0), TF_NONE);
    tf_free(&S);
}

/* --- Phase 11: waitkey --- */

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>

TEST(waitkey_basic) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "\x1b[A", 3);
    close(pfd[1]);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
    close(pfd[0]);
    tf_free(&S);
}

TEST(waitkey_escape) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    /* lone ESC byte: partial sequence, reader drains -> flush on timeout */
    write(pfd[1], "\x1b", 1);
    r = tf_waitkey(&S, pfd[0], 20, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_ESCAPE);
    close(pfd[1]);
    close(pfd[0]);
    tf_free(&S);
}

TEST(waitkey_timeout_idle) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    r = tf_waitkey(&S, pfd[0], 10, &key);
    asserteq(r, TF_AGAIN);
    close(pfd[1]);
    close(pfd[0]);
    tf_free(&S);
}

TEST(waitkey_eintr_stale) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "a", 1);
    close(pfd[1]);
    /* stale EINTR from a prior interrupted poll: poll succeeds (r > 0)
     * but errno still reads EINTR -- must not skip the readable fd */
    errno = EINTR;
    r = tf_waitkey(&S, pfd[0], 500, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    close(pfd[0]);
    tf_free(&S);
}

TEST(waitkey_params) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    asserteq(tf_waitkey(NULL, 0, 0, &key), TF_ERRPARAM);
    asserteq(tf_waitkey(&S, -1, 0, &key), TF_ERRPARAM);
    asserteq(tf_waitkey(&S, 0, 0, NULL), TF_ERRPARAM);

    /* invalid fd -> read error -> TF_ERRPARAM */
    asserteq(tf_waitkey(&S, 9999, 0, &key), TF_ERRPARAM);

    /* tf_string NULL plen / S */
    asserteq(tf_string(&S, NULL) == NULL, 1);
    asserteq(tf_string(NULL, NULL) == NULL, 1);

    /* closed pipe -> EOF -> TF_NONE */
    {
        int      fds[2];
        tf_State S2;
        pipe(fds);
        close(fds[1]);
        tf_init(&S2, NULL, NULL);
        asserteq(tf_waitkey(&S2, fds[0], 0, &key), TF_NONE);
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

TEST(waitkey_keep) {
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
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    trash_stack();
    r = tf_waitkey(&S, pfd[0], 500, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'b');
    close(pfd[0]);
    tf_free(&S);
}

TEST(waitkey_oom) {
    tf_State S;
    tf_Key   key;
    int      cnt = 0, pfd[2], r;
    (void)pipe(pfd);
    write(pfd[1], "a", 1);
    close(pfd[1]);
    tf_init(&S, oom_alloc, &cnt);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    asserteq(r, TF_ERRMEM);
    close(pfd[0]);
    tf_free(&S);
}

TEST(waitkey_infinite) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "a", 1);
    close(pfd[1]);
    /* timeout < 0: poll indefinitely, data already available */
    r = tf_waitkey(&S, pfd[0], -1, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');
    close(pfd[0]);
    tf_free(&S);
}

/* waitkey surplus bytes live in the parser's chunk (wait buffer); tf_feed
 * switches the source and discards the unconsumed chunk -- the caller must
 * re-feed them, the new reader's data follows */
TEST(waitkey_feed_discard) {
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
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'a');
    close(pfd[0]);
    /* switch to a reader: 'b' (waitkey surplus) is dropped by tf_feed */
    mr.data = "Z", mr.len = 1, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'Z');
    tf_free(&S);
}

/* the already-allocated wait buffer is reused across waitkey calls */
TEST(waitkey_reuse) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    write(pfd[1], "a", 1);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'a');
    write(pfd[1], "b", 1);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'b');
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
TEST(waitkey_poll_eintr) {
    tf_State S;
    tf_Key   key;
    int      pfd[2], r;
    (void)pipe(pfd);
    tf_init(&S, NULL, NULL);
    eintr_pipew = pfd[1];
    signal(SIGALRM, eintr_writebyte);
    alarm(1);
    r = tf_waitkey(&S, pfd[0], -1, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'x');
    alarm(0), signal(SIGALRM, SIG_DFL);
    eintr_pipew = -1;
    close(pfd[1]);
    close(pfd[0]);
    tf_free(&S);
}

/* a readkey error (CS buffer OOM) propagates out of waitkey */
TEST(waitkey_readkey_err) {
    tf_State S;
    tf_Key   key;
    int      cnt = 1, pfd[2], r;
    (void)pipe(pfd);
    write(pfd[1], "\x1b]hello", 7);
    close(pfd[1]);
    tf_init(&S, oom_alloc, &cnt);
    r = tf_waitkey(&S, pfd[0], 500, &key);
    asserteq(r, TF_ERRMEM);
    close(pfd[0]);
    tf_free(&S);
}
#else
TEST(waitkey_basic) {}
TEST(waitkey_timeout_idle) {}
TEST(waitkey_eintr_stale) {}
TEST(waitkey_infinite) {}
TEST(waitkey_keep) {}
TEST(waitkey_oom) {}
TEST(waitkey_feed_discard) {}
TEST(waitkey_reuse) {}
TEST(waitkey_poll_eintr) {}
TEST(waitkey_readkey_err) {}
TEST(waitkey_params) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    asserteq(tf_waitkey(&S, 0, 0, &key), TF_ERRPARAM);
    tf_free(&S);
}
#endif

/* --- Phase 11: mouse interpretation --- */

TEST(mouse_press) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.btn = 0;
    key.d.mouse.line = 5;
    key.d.mouse.col = 10;

    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_PRESS);
    asserteq(btn, 1);
    asserteq(line, 5);
    asserteq(col, 10);
}

TEST(mouse_drag) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.btn = 0x20; /* button 1 drag */

    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_DRAG);
    asserteq(btn, 1);
}

TEST(mouse_release) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.btn = 3; /* release */

    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_RELEASE);
    asserteq(btn, 0);
}

TEST(mouse_scroll) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;

    /* scroll up: code 64 -> btn 4 */
    key.d.mouse.btn = 64;
    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_PRESS);
    asserteq(btn, 4);

    /* scroll down: code 65 -> btn 5 */
    key.d.mouse.btn = 65;
    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_PRESS);
    asserteq(btn, 5);

    /* scroll left: code 66 -> btn 6 */
    key.d.mouse.btn = 66;
    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_PRESS);
    asserteq(btn, 6);

    /* scroll right: code 67 -> btn 7 */
    key.d.mouse.btn = 67;
    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_PRESS);
    asserteq(btn, 7);
}

TEST(mouse_extended) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;

    /* button 8: code 128 */
    key.d.mouse.btn = 128;
    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_PRESS);
    asserteq(btn, 8);

    /* button 9: code 129 */
    key.d.mouse.btn = 129;
    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_PRESS);
    asserteq(btn, 9);
}

TEST(mouse_unknown) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.btn = 200; /* unknown code, not in any range */

    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_UNKNOWN);
    asserteq(btn, 0);
}

TEST(mouse_sgr_release) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;
    key.d.mouse.release = 1;
    key.d.mouse.btn = 0;

    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_RELEASE);
    asserteq(btn, 0);
}

TEST(mouse_params) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    /* non-MOUSE type -> returns 0 */
    key.type = TF_TYPE_UNICODE;
    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_ERRPARAM);
    /* NULL pointers */
    asserteq(tf_mouse(NULL, &ev, &btn, &line, &col), TF_ERRPARAM);
    key.type = TF_TYPE_MOUSE;
    asserteq(tf_mouse(&key, NULL, &btn, &line, &col), TF_ERRPARAM);
    asserteq(tf_mouse(&key, &ev, NULL, &line, &col), TF_ERRPARAM);
    asserteq(tf_mouse(&key, &ev, &btn, NULL, &col), TF_ERRPARAM);
    asserteq(tf_mouse(&key, &ev, &btn, &line, NULL), TF_ERRPARAM);
}

/* --- Phase 11: position / modereport --- */

TEST(position_basic) {
    tf_Key key;
    int    line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_POSITION;
    key.d.pos.line = 10;
    key.d.pos.col = 25;

    asserteq(tf_position(&key, &line, &col), TF_OK);
    asserteq(line, 10);
    asserteq(col, 25);
}

TEST(position_params) {
    tf_Key key;
    int    line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_UNICODE;
    asserteq(tf_position(&key, &line, &col), TF_ERRPARAM);
    asserteq(tf_position(NULL, &line, &col), TF_ERRPARAM);
    key.type = TF_TYPE_POSITION;
    asserteq(tf_position(&key, NULL, &col), TF_ERRPARAM);
}

TEST(modereport_basic) {
    tf_Key key;
    int    init, mode, val;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MODEREPORT;
    key.d.modereport.initial = '?';
    key.d.modereport.mode = 1;
    key.d.modereport.value = 2;

    asserteq(tf_modereport(&key, &init, &mode, &val), TF_OK);
    asserteq(init, '?');
    asserteq(mode, 1);
    asserteq(val, 2);
}

TEST(modereport_params) {
    tf_Key key;
    int    init, mode, val;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_UNICODE;
    asserteq(tf_modereport(&key, &init, &mode, &val), TF_ERRPARAM);
    asserteq(tf_modereport(NULL, &init, &mode, &val), TF_ERRPARAM);
    key.type = TF_TYPE_MODEREPORT;
    asserteq(tf_modereport(&key, NULL, &mode, &val), TF_ERRPARAM);
}

/* --- Phase 11: tf_csi --- */

TEST(csi_basic) {
    tf_State S;
    tf_Key   key;
    int      args[16];
    int      na;
    int      cmd, r;
    tf_init(&S, NULL, NULL);

    /* feed UNKNOWN_CSI: \x1b[1;2;3x */
    asserteq(feed_seq(&S, &key, "\x1b[1;2;3x", 8), TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    asserteq(r, 3); /* returns field count */
    asserteq(cmd, 'x');
    asserteq(args[0], 1);
    asserteq(args[1], 2);
    asserteq(args[2], 3);
    tf_free(&S);
}

/* initial+final without parameters must yield 0 args (final is not a
 * parameter field) -- \x1b[?c DA1, \x1b[?u kitty, \x1b[?1$y DEC */
TEST(csi_noparams) {
    tf_State S;
    tf_Key   key;
    int      args[8], n, cmd;
    tf_init(&S, NULL, NULL);

    asserteq(feed_seq(&S, &key, "\x1b[?c", 4), TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    n = tf_csi(&S, args, 8, &cmd);
    asserteq(n, 0); /* pre-fix: 1 (final 'c' counted) */
    asserteq(cmd, 'c' | ('?' << 8));

    /* with a real parameter: count unaffected */
    asserteq(feed_seq(&S, &key, "\x1b[?1c", 5), TF_OK);
    n = tf_csi(&S, args, 8, &cmd);
    asserteq(n, 1);
    asserteq(args[0], 1);

    /* SGR mouse: trailing final not part of any field */
    asserteq(feed_seq(&S, &key, "\x1b[<0;6;7M", 9), TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    asserteq(key.d.mouse.btn, 0);
    asserteq(key.d.mouse.col, 6);
    asserteq(key.d.mouse.line, 7);
    tf_free(&S);
}

TEST(csi_empty_params) {
    tf_State S;
    tf_Key   key;
    int      args[16];
    int      na;
    int      cmd;
    tf_init(&S, NULL, NULL);

    /* \x1b[;2;x -> empty first field -> -1; final 'x' is not a field */
    asserteq(feed_seq(&S, &key, "\x1b[;2;x", 6), TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    na = 16;
    asserteq(tf_csi(&S, args, na, &cmd), 2);
    asserteq(cmd, 'x');
    asserteq(args[0], -1);
    asserteq(args[1], 2);
    tf_free(&S);
}

TEST(csi_truncate) {
    tf_State S;
    tf_Key   key;
    int      args[3];
    int      na;
    int      cmd;
    tf_init(&S, NULL, NULL);

    /* \x1b[1;2;3;4x -> 4 fields, na=3 -> truncated to 3 */
    asserteq(feed_seq(&S, &key, "\x1b[1;2;3;4x", 10), TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    na = 3;
    asserteq(tf_csi(&S, args, na, &cmd), 3);
    asserteq(cmd, 'x');
    asserteq(args[0], 1);
    asserteq(args[1], 2);
    asserteq(args[2], 3);
    tf_free(&S);
}

TEST(csiparse_params) {
    tf_State S;
    tf_Key   key;
    int      args[16];
    int      na;
    int      cmd, r;
    tf_init(&S, NULL, NULL);
    /* no valid cmd -> TF_ERRPARAM */
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    asserteq(r, TF_ERRPARAM);

    /* NULL checks */
    asserteq(tf_csi(NULL, args, na, &cmd), TF_ERRPARAM);
    asserteq(tf_csi(&S, NULL, na, &cmd), TF_ERRPARAM);
    asserteq(tf_csi(&S, args, na, NULL), TF_ERRPARAM);

    /* IDLE state buf empty */
    tf_init(&S, NULL, NULL);
    /* set cmd manually via UNKNOWN_CSI with empty buf */
    feed_seq(&S, &key, "\x1b[x", 3);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    asserteq(r, 0); /* final 'x' is not a parameter field */
    asserteq(cmd, 'x');

    /* buf with params but no final (partial CSI) -> no snapshot */
    tf_init(&S, NULL, NULL);
    S.buf_len = 2;
    S.buf[0] = '1';
    S.buf[1] = ';';
    r = tf_csi(&S, args, na, &cmd);
    asserteq(r, TF_ERRPARAM);

    /* non-digit char inside a param field is skipped */
    feed_seq(&S, &key, "\x1b[1<x", 5);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    asserteq(r, 1);
    asserteq(cmd, 'x');
    asserteq(args[0], 1);

    tf_free(&S);
}

TEST(csi_with_sub) {
    tf_State S;
    tf_Key   key;
    int      args[16];
    int      na;
    int      cmd;
    tf_init(&S, NULL, NULL);

    /* \x1b[1:2;3x -> field 1 has sub-params, tf_csi only returns 1,3 */
    asserteq(feed_seq(&S, &key, "\x1b[1:2;3x", 8), TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    na = 16;
    asserteq(tf_csi(&S, args, na, &cmd), 2);
    asserteq(cmd, 'x');
    asserteq(args[0], 1);
    asserteq(args[1], 3);
    tf_free(&S);
}

/* --- Phase 12a: tfK_writecp multi-byte UTF-8 + replacement --- */

TEST(writecp_utf8) {
    char buf[32];
    int  n;

    /* 2-byte: U+00FF */
    n = tfK_writecp(buf, sizeof(buf), 0xFF);
    asserteq(n, 2);
    assertstreq(buf, "\xc3\xbf");

    /* 3-byte: U+0800 */
    n = tfK_writecp(buf, sizeof(buf), 0x800);
    asserteq(n, 3);
    assertstreq(buf, "\xe0\xa0\x80");

    /* 4-byte: U+10000 */
    n = tfK_writecp(buf, sizeof(buf), 0x10000);
    asserteq(n, 4);
    assertstreq(buf, "\xf0\x90\x80\x80");

    /* >0x200000 -> 0xFFFD replacement */
    n = tfK_writecp(buf, sizeof(buf), 0x200000);
    asserteq(n, 3);
    assertstreq(buf, "\xef\xbf\xbd");

    /* 0xFFFD -> direct encoding */
    n = tfK_writecp(buf, sizeof(buf), 0xFFFD);
    asserteq(n, 3);
    assertstreq(buf, "\xef\xbf\xbd");
}

/* --- Phase 12b: tfK_writemods LONGMOD paths --- */

TEST(writemods_long) {
    char buf[64];
    int  n;

    /* LONGMOD + SHIFT: "Shift-" prefix */
    n = tfK_writemods(buf, sizeof(buf), TF_MOD_SHIFT, TF_FMT_LONGMOD);
    asserteq(n, 6);
    assertstreq(buf, "Shift-");

    /* LONGMOD + ALT (not ALTISMETA): "Alt-" prefix */
    n = tfK_writemods(buf, sizeof(buf), TF_MOD_ALT, TF_FMT_LONGMOD);
    asserteq(n, 4);
    assertstreq(buf, "Alt-");

    /* LONGMOD + CTRL: "Control-" prefix */
    n = tfK_writemods(buf, sizeof(buf), TF_MOD_CTRL, TF_FMT_LONGMOD);
    asserteq(n, 8);
    assertstreq(buf, "Control-");
}

/* LOWERMOD: long modifier names in lowercase */
TEST(format_lowermod) {
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_KEYSYM, key.d.sym = TF_SYM_LEFT;
    key.modifiers = TF_MOD_CTRL | TF_MOD_SHIFT | TF_MOD_ALT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_LONGMOD | TF_FMT_LOWERMOD);
    assertstreq(buf, "shift-alt-control-Left");
    /* short forms lowercased too (libtermkey modprefix semantics) */
    key.type = TF_TYPE_UNICODE, key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERMOD);
    assertstreq(buf, "c-x");
}

/* --- Phase 12c: tf_format without WRAPBRACKET --- */

TEST(format_nobracket) {
    tf_Key key;
    char   buf[64];
    int    n;
    memset(&key, 0, sizeof(key));

    /* UNICODE + CTRL, no bracket, no SPACEMOD: C-x */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    n = tf_format(buf, sizeof(buf), &key, 0);
    /* default: WRAPBRACKET, no SPACEMOD -> "<C-x>" */
    assertstreq(buf, "<C-x>");

    /* UNICODE + CTRL, no WRAPBRACKET: C-x (bare) */
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    asserteq(n, 3);
    assertstreq(buf, "C-x");

    /* UNICODE + SHIFT+ALT+CTRL, no WRAPBRACKET: S-A-C-x */
    key.modifiers = TF_MOD_SHIFT | TF_MOD_ALT | TF_MOD_CTRL;
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    assertstreq(buf, "S-A-C-x");

    /* UNICODE no modifiers, no WRAPBRACKET: bare 'x' */
    key.modifiers = 0;
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    asserteq(n, 1);
    assertstreq(buf, "x");

    /* FUNCTION with modifier, no WRAPBRACKET: S-F1 */
    key.type = TF_TYPE_FUNCTION;
    key.d.number = 1;
    key.modifiers = TF_MOD_SHIFT;
    n = tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    assertstreq(buf, "S-F1");
}

/* --- Phase 12d: tf_format SPACEMOD without WRAPBRACKET --- */

TEST(format_spacesep) {
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));

    /* SPACEMOD without WRAPBRACKET: "C x" with trailing space */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'x';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_SPACEMOD | TF_FMT_LOWERSPACE);
    assertstreq(buf, "C x");

    /* UNICODE: CARETCTRL without WRAPBRACKET */
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_CARETCTRL | TF_FMT_LOWERSPACE);
    assertstreq(buf, "^X");

    /* UNICODE + CTRL lowercase, CARETCTRL uppers it */
    key.d.codepoint = 'a';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_CARETCTRL | TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<^A>");
}

/* --- Phase 12e: tfK_parsemod A-/D-/Alt-/Meta- forms --- */

TEST(parsemod_altmeta) {
    tf_Key key;
    int    n;

    /* <A-x> -> ALT + 'x' */
    n = tf_parse("<A-x>", &key);
    asserteq(n, 5);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_ALT);

    /* <D-x> -> SUPER + 'x' */
    n = tf_parse("<D-x>", &key);
    asserteq(n, 5);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_SUPER);

    /* <Alt-x> -> ALT + 'x' (long form, lowercase) */
    n = tf_parse("<Alt-x>", &key);
    asserteq(n, 7);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_ALT);

    /* <Meta-x> -> ALT + 'x' (long form, "Me" prefix) */
    n = tf_parse("<Meta-x>", &key);
    asserteq(n, 8);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_ALT);
}

/* --- Phase 12f: tfU_decode 5/6 byte + edge cases --- */

/* Note: test_utf8_456 already exists, different test from Phase 5
 * This version adds 5/6 byte coverage. Merging into test_utf8_456 above. */

/* --- Phase 12g: tf_readkey REPLAY / REPLAY_BUF idle transitions --- */

TEST(readkey_replay) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* REPLAY_BUF with state=0 is IDLE: no replay, main loop reads reader */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_NONE);
    asserteq(S.state, TF_STATE_IDLE);

    tf_free(&S);
}

/* --- Phase 12h: replay with buf_len transition --- */

TEST(replay_bufmerge) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* Setup replay source: "A" at buf tail */
    S.replay = 1;
    S.buf[TF_MAX_BUFLEN - 1] = 'A';
    S.buf_len = 0;
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'A');
    asserteq(S.state, TF_STATE_IDLE);

    /* Setup: replay source of 3 bytes */
    S.replay = 3;
    S.buf[TF_MAX_BUFLEN - 3] = 'X';
    S.buf[TF_MAX_BUFLEN - 2] = 'Y';
    S.buf[TF_MAX_BUFLEN - 1] = 'Z';
    S.buf_len = 0;
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'X');
    asserteq(S.replay, 2);

    /* consume second byte */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'Y');
    asserteq(S.replay, 1);

    /* consume third byte -> IDLE */
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'Z');
    asserteq(S.state, TF_STATE_IDLE);
    asserteq(S.buf_len, 0);
    tf_free(&S);
}

TEST(feed_replay_clear) {
    tf_State   S;
    tf_Key     key;
    MockReader mr;
    char       over[67];
    int        r, i;
    tf_init(&S, NULL, NULL);
    memset(over, '1', sizeof(over));
    over[0] = 0x1b, over[1] = '[';
    over[66] = 'A'; /* chunk tail: must be dropped by tf_feed */
    /* \x1b[ + 64x'1' overflows the CSI param buffer -> ALT+[ + REPLAY */
    r = feed_seq(&S, &key, over, 67);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '[');
    asserteq(key.modifiers, TF_MOD_ALT);
    assertok(S.replay > 0);
    /* new reader mid-replay: old chunk remainder must be discarded */
    mr.data = "B", mr.len = 1, mr.called = 0;
    tf_feed(&S, mock_reader, &mr);
    for (i = 0; i < 64; ++i) {
        r = tf_readkey(&S, &key);
        asserteq(r, TF_OK);
        asserteq(key.type, TF_TYPE_UNICODE);
    }
    r = tf_readkey(&S, &key);
    asserteq(r, TF_OK);
    asserteq(key.d.codepoint, 'B'); /* old 'A' would leak here */
    tf_free(&S);
}

/* --- Phase 12i: tf_parse bracket char name advance s++ --- */

TEST(parse_bracketadv) {
    tf_Key key;
    int    n;

    /* <Left> -> s++ on match (covers L2065) */
    n = tf_parse("<Left>", &key);
    asserteq(n, 6);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_LEFT);

    /* <Down> -> s++ on match */
    n = tf_parse("<Down>", &key);
    asserteq(n, 6);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_DOWN);

    /* <Right> */
    n = tf_parse("<Right>", &key);
    asserteq(n, 7);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_RIGHT);

    /* <Up> */
    n = tf_parse("<Up>", &key);
    asserteq(n, 4);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_UP);
}

/* <F N> with space inside the name: spaces are stripped */
TEST(parse_fspace) {
    tf_Key key;
    int    n;
    n = tf_parse("<F 1>", &key);
    assertok(n > 0); /* pre-fix: -1 */
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 1);
    n = tf_parse("<f 12>", &key);
    assertok(n > 0);
    asserteq(key.d.number, 12);
    /* no digits after F: single char falls back to codepoint, space fails */
    n = tf_parse("<F>", &key);
    asserteq(n, 3);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'F');
    asserteq(tf_parse("<F >", &key), -1);
}

/* --- Phase 12j: trie slot left expansion --- */

TEST(trie_leftexpand) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {
            {"key_clear",
             "\x1b"
             "Z"}, /* inserted first -> arr min='Z' */
            {"key_end",
             "\x1b"
             "A"}, /* later -> left expand */
            {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* Verify both can be matched */
    r = feed_seq(
            &S, &key,
            "\x1b"
            "A",
            2);
    asserteq(r, TF_OK);

    r = feed_seq(
            &S, &key,
            "\x1b"
            "Z",
            2);
    asserteq(r, TF_OK);

    tf_free(&S);
}

/* --- Phase 12k: cs_buf realloc (tfB_append expansion) --- */

TEST(cs_bytexpand) {
    tf_State S;
    tf_Key   key;
    int      r, i, len;
    char     buf[256];
    tf_init(&S, NULL, NULL);

    /* enter CS OSC, then feed many bytes to trigger realloc */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, ']');
    asserteq(r, TF_AGAIN);

    /* feed enough to fill initial 64 bytes + overflow to trigger realloc */
    for (i = 0; i < 200; i++) buf[i] = (unsigned char)('a' + (i % 26));
    r = feed_seq(&S, &key, buf, 200);
    /* should trigger realloc */
    asserteq(r, TF_AGAIN);

    /* terminate */
    r = feed_byte(&S, &key, 0x07);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_OSC);
    assertok(tf_string(&S, &len) != NULL);
    asserteq(len, 200);
    tf_free(&S);
}

/* --- Phase 12l: cs_buf realloc OOM (free path) --- */

TEST(cs_oomfree) {
    tf_State S;
    tf_Key   key;
    int      r, i;
    char     buf[128];

    /* default allocator: realloc fails -> free cs_buf */
    /* We use oom_alloc to simulate failure after initial allocation */
    {
        int ec = 1; /* allow first alloc (64), fail realloc */
        tf_init(&S, oom_alloc, &ec);
        r = feed_byte(&S, &key, 0x1b);
        asserteq(r, TF_AGAIN);
        r = feed_byte(&S, &key, ']');
        asserteq(r, TF_AGAIN);
        /* first byte fills initial 64 -> AG=AGAIN */
        for (i = 0; i < 64; i++) buf[i] = 'x';
        r = feed_seq(&S, &key, buf, 64);
        asserteq(r, TF_AGAIN);
        /* next byte triggers realloc which fails with oom_alloc */
        r = feed_byte(&S, &key, 'y');
        asserteq(r, TF_ERRMEM);
        asserteq(S.state, TF_STATE_IDLE);
        tf_free(&S);
    }
}

/* --- Phase 12m: tfC_kitty text field + non-press events --- */

TEST(kitty_textfield) {
    tf_State S;
    tf_Key   key;
    int      r;

    /* kitty CSI with text field: 97;5:2;65u -> cp=97, ev=REPEAT, text='A' */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[97;5:2;65u", 12);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.event, TF_EVENT_REPEAT);
    asserteq(key.d.codepoint, 97);
    tf_free(&S);

    /* kitty release: 97;5:3u -> cp=97, ev=RELEASE */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[97;5:3u", 9);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.event, TF_EVENT_RELEASE);
    tf_free(&S);

    /* kitty with empty text field: field 3 empty (97;5u) covered in kitty_alts
     */
}

/* --- Phase 12n: tfD_cs default:break in terminator --- */

TEST(cs_terminate) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* enter CS: ESC+P (DCS) */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'P');
    asserteq(r, TF_AGAIN);
    asserteq(S.state, TF_STATE_CS_DCS);

    /* terminate with BEL */
    r = feed_byte(&S, &key, 0x07);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_DCS);
    tf_free(&S);

    /* DCS term by ST (ESC \) */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1bP\x1b\\", 4);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_DCS);
    tf_free(&S);

    /* APC (ESC _) terminate with BEL */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b_\x07", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_APC);
    tf_free(&S);
}

/* --- Phase 12o: tfC_kitty numeric text + empty text --- */

TEST(kitty_emptytext) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* kitty with empty text -> empty cs_buf */
    r = feed_seq(&S, &key, "\x1b[?77429u", 9);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KITTYREPORT);
    tf_free(&S);
}

TEST(kitty_pua_utf8) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* plain UTF-8 PUA (kitty level 4 / raw text): 57399 -> KP0 keysym */
    r = feed_seq(&S, &key, "\xEE\x80\xB7", 3); /* U+E017 = 57399 */
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_KP0);

    /* 57415 -> KPEquals (U+E047) */
    r = feed_seq(&S, &key, "\xEE\x81\x87", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_KPEQUALS);

    /* 57364 -> F1 function (U+E014) */
    r = feed_seq(&S, &key, "\xEE\x80\x94", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 1);

    /* non-PUA codepoint untouched */
    r = feed_seq(&S, &key, "a", 1);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'a');

    tf_free(&S);
}

/* --- Phase 12p: tfM_dispatch uncovered path --- */

TEST(mouse_dispatch) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* CSI M without enough args -> enters MOUSE_X10 state */
    /* CSI M is used for classic x10 mouse */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, 'M');
    asserteq(r, TF_AGAIN);
    /* Now S.state should be MOUSE_X10 */
    asserteq(S.state, TF_STATE_MOUSE_X10);
    tf_free(&S);
}

/* --- Phase 12q: tfC_cursorkey default --- */

TEST(cursorkey_default) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* CSI H (CU) with no modifiers */
    r = feed_seq(&S, &key, "\x1b[H", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_HOME);
    tf_free(&S);
}

/* --- Phase 12r: CSI with byte >= 0x80 in tfD_csi --- */

TEST(csi_utf8byte) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    /* enter CSI */
    r = feed_byte(&S, &key, 0x1b);
    asserteq(r, TF_AGAIN);
    r = feed_byte(&S, &key, '[');
    asserteq(r, TF_AGAIN);

    /* feed a byte >= 0x80 in CSI -> transitions to UTF8 or triggers flush */
    r = feed_byte(&S, &key, 0xC0); /* non-ASCII byte in CSI */
    (void)r;                       /* coverage only */
    tf_free(&S);
}

/* --- Phase 12s: tfD_canon Ctrl+space and others --- */

TEST(canon_ctrl) {
    tf_Key key;

    /* Ctrl+Space (0x00) -> KEYSYM SPACE + CTRL */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 0x00, 0);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_SPACE);
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* Ctrl+? (0x7F = DEL) -> KEYSYM DEL */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 0x7F, 0);
    asserteq(key.type, TF_TYPE_KEYSYM);
    asserteq((long)key.d.sym, (long)TF_SYM_DELETE);

    /* Printable: 'A' */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 'A', 0);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'A');

    /* nointerpret: Ctrl+C (0x03) -> code = 0x43='C'+ctrl */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 0x03, 0);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* 0x1c-0x1f range: 0x1c -> '\' + ctrl */
    memset(&key, 0, sizeof(key));
    tfD_canon(&key, 0x1c, 0);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, '\\');
    asserteq(key.modifiers, TF_MOD_CTRL);
}

/* --- Phase 12t: tfC_nextarg iterator branches --- */

TEST(nextarg_edge) {
    tf_State    S;
    tf_Key      key;
    int         r, len;
    const char *f;

    tf_init(&S, NULL, NULL);
    /* feed unknown CSI with sub-params */
    r = feed_seq(&S, &key, "\x1b[1;2:3x", 8);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);

    /* fields: "1", "2:3" (final not included), then none */
    f = NULL;
    assertok(tfC_nextarg(&S, &f, &len));
    asserteq(len, 1);
    assertok(tfC_nextarg(&S, &f, &len));
    asserteq(len, 3);
    assertok(!tfC_nextarg(&S, &f, &len));

    /* subval: present / absent / empty sub */
    asserteq(tfC_subval("2:3x", 4, -1), 3);
    asserteq(tfC_subval("2x", 2, -1), -1);
    asserteq(tfC_subval("2:x", 3, -1), -1); /* empty sub -> dflt */

    /* empty first field: [;1x */
    r = feed_seq(&S, &key, "\x1b[;1x", 5);
    asserteq(r, TF_OK);
    f = NULL;
    assertok(tfC_nextarg(&S, &f, &len));
    asserteq(len, 0); /* empty field */
    assertok(tfC_nextarg(&S, &f, &len));
    asserteq(len, 1); /* "1" */

    /* intermediate byte truncates: "$" */
    r = feed_seq(&S, &key, "\x1b[1$y", 5);
    asserteq(r, TF_OK);
    f = NULL;
    assertok(tfC_nextarg(&S, &f, &len));
    asserteq(len, 1);
    assertok(!tfC_nextarg(&S, &f, &len));

    /* intermediate right after ';': field 2 absent (not empty) */
    r = feed_seq(&S, &key, "\x1b[1;$y", 6);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MODEREPORT);
    asserteq(key.d.modereport.mode, 1);
    asserteq(key.d.modereport.value, -1);
    f = NULL;
    assertok(tfC_nextarg(&S, &f, &len));
    asserteq(len, 1);
    assertok(!tfC_nextarg(&S, &f, &len));

    /* initial '?' skipped */
    r = feed_seq(&S, &key, "\x1b[?25x", 6);
    asserteq(r, TF_OK);
    f = NULL;
    assertok(tfC_nextarg(&S, &f, &len));
    asserteq(len, 2); /* "25" */
    asserteq(tfC_fieldval(f, len, 0), 25);

    /* control byte in field: neither intermediate nor digit */
    r = feed_seq(&S, &key, "\x1b[\x01x", 4);
    asserteq(r, TF_OK);
    f = NULL;
    assertok(tfC_nextarg(&S, &f, &len));
    asserteq(tfC_fieldval(f, len, 0), 0); /* no digits -> dflt */

    /* intermediate first byte: field NULL -> handler dflt paths */
    r = feed_seq(&S, &key, "\x1b[$y", 5); /* modereport, no params */
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MODEREPORT);
    asserteq(key.d.modereport.mode, -1);
    asserteq(key.d.modereport.value, -1);
    r = feed_seq(&S, &key, "\x1b[$~", 5); /* funckey, no field 1 */
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    r = feed_seq(&S, &key, "\x1b[$u", 5); /* kitty, no field 1 */
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    r = feed_seq(&S, &key, "\x1b[?5R", 6); /* CPR, no field 2 */
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_POSITION);
    asserteq(key.d.pos.line, 5);
    asserteq(key.d.pos.col, 0);

    /* empty params -> no fields */
    S.buf_len = 0;
    f = NULL;
    assertok(!tfC_nextarg(&S, &f, &len));

    tf_free(&S);
}

/* --- Phase 12u: tfC_fkeynum uncovered branches --- */

TEST(fkeynum_edge) {
    tf_State S;
    tf_Key   key;
    int      r;

    tf_init(&S, NULL, NULL);
    /* F10 -> feed CSI 10~ */
    r = feed_seq(&S, &key, "\x1b[10~", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 10);
    tf_free(&S);

    /* F11 -> CSI 23~ */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[23~", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 11);
    tf_free(&S);

    /* CSI 11;11~ -> n=11, fkeynum returns 1 (F1) */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[11;11~", 8);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 1); /* 11 maps to F1 */
    tf_free(&S);

    /* F12 -> CSI 24~ */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[24~", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 12);
    tf_free(&S);
}

/* --- Phase 12v: tfC_arg / tfC_sub uncovered branches --- */

TEST(csiarg_edge) {
    tf_State S;
    tf_Key   key;
    int      r;
    int      args[16];
    int      na;
    int      cmd;

    tf_init(&S, NULL, NULL);
    /* CSI with empty first field: [;1x */
    r = feed_seq(&S, &key, "\x1b[;1x", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    asserteq(r, 2);
    asserteq(cmd, 'x');
    asserteq(args[0], -1); /* empty field -> default -1 */
    tf_free(&S);

    /* CSI with sub-params: get raw sub */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1b[1:2;3x", 8);
    asserteq(r, TF_OK);
    na = 16;
    r = tf_csi(&S, args, na, &cmd);
    asserteq(r, 2);
    asserteq(cmd, 'x');
    /* sub-params stripped by tf_csi */
    asserteq(args[0], 1);
    asserteq(args[1], 3);
    tf_free(&S);
}

/* --- Phase 12w: tfD_ss3dispatch uncovered branches --- */

TEST(ss3_edge) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* SS3 O P -> no default mapping (depends on trie) */
    r = feed_seq(&S, &key, "\x1bOP", 3);
    asserteq(r, TF_OK);
    /* This should try SS3 lookup (ESC O -> SS3 state) */
    tf_free(&S);

    /* SS3 with lookup table */
    tf_init(&S, NULL, NULL);
    {
        TILookup tbl[] = {{"key_f3", "\x1bOR"}, {NULL, NULL}};
        tf_load(&S, ti_lookup, tbl);
    }
    r = feed_seq(&S, &key, "\x1bOR", 3);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_FUNCTION);
    asserteq(key.d.number, 3);
    tf_free(&S);

    /* SS3 unknown: ESC O z -> should not match any entry */
    tf_init(&S, NULL, NULL);
    r = feed_seq(&S, &key, "\x1bOz", 3);
    asserteq(r, TF_OK);
    /* Should produce something (idle with some default handling) */
    tf_free(&S);
}

/* --- Phase 12x: tfD_escape edge cases --- */

TEST(escape_edge) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* ESC # (DEC) */
    r = feed_seq(&S, &key, "\x1b#", 2);
    asserteq(r, TF_OK);

    /* ESC % (declare charset) */
    r = feed_seq(&S, &key, "\x1b%", 2);
    asserteq(r, TF_OK);

    /* ESC space (SP) */
    r = feed_seq(&S, &key, "\x1b ", 2);
    asserteq(r, TF_OK);

    tf_free(&S);
}

/* --- Phase 12y: tfT_slot right expansion uncovered --- */

TEST(trie_rightexpand) {
    tf_State S;
    tf_Key   key;
    int      r;
    TILookup tbl[] = {
            {"key_home",
             "\x1b"
             "L"}, /* inserted first -> arr min='L' */
            {"key_end",
             "\x1b"
             "Z"}, /* later -> right expand */
            {NULL, NULL}};
    tf_init(&S, NULL, NULL);
    tf_load(&S, ti_lookup, tbl);

    /* Verify both can be matched */
    r = feed_seq(
            &S, &key,
            "\x1b"
            "L",
            2);
    asserteq(r, TF_OK);
    r = feed_seq(
            &S, &key,
            "\x1b"
            "Z",
            2);
    asserteq(r, TF_OK);
    tf_free(&S);
}

/* --- Phase 12z: tf_format no-bracket bare name (modpos==0) --- */

TEST(format_barename) {
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));

    /* KEYSYM no modifiers, no WRAPBRACKET -> just name */
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_ESCAPE;
    tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    assertstreq(buf, "escape");
}

TEST(format_none) {
    /* memset key (type = TF_TYPE_NONE): not treated as UNICODE,
     * formats as empty brackets instead of garbage */
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));
    tf_format(buf, sizeof(buf), &key, 0);
    assertstreq(buf, "<>");
    tf_format(buf, sizeof(buf), &key, TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<>");
}

/* --- Phase 12aa: tfM_dispatch CSI M path --- */

TEST(mouse_x10_raw) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);

    /* CSI M dispatch + raw bytes -> covers tfM_dispatch return 0 path */
    /* Feed CSI M followed by 3 raw bytes in one chunk */
    r = feed_seq(&S, &key, "\x1b[M\x20\x21\x22", 6);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_MOUSE);
    tf_free(&S);
}

/* --- Phase 12bb: public-API branch coverage additions --- */

TEST(branch_unknown_csi_colon) {
    tf_State S;
    tf_Key   key;
    int      r;
    tf_init(&S, NULL, NULL);
    /* \x1b[:2u -> UNKNOWN_CSI (colon before digits) */
    r = feed_seq(&S, &key, "\x1b[:2u", 5);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    tf_free(&S);
}

TEST(branch_kitty_cp_bounds) {
    tf_State S;
    tf_Key   key;
    tf_init(&S, NULL, NULL);
    /* cp=0: text is skipped, key stays UNICODE 97 */
    asserteq(feed_seq(&S, &key, "\x1b[97;2;0u", 9), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 97);
    tf_free(&S);

    /* cp>0x10FFFF: text is skipped, key stays UNICODE 97 */
    tf_init(&S, NULL, NULL);
    asserteq(feed_seq(&S, &key, "\x1b[97;2;1114112u", 15), TF_OK);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 97);
    tf_free(&S);
}

TEST(branch_csi_full_final) {
    tf_State S;
    tf_Key   key;
    char     seq[128];
    int      i, p = 0, r;
    tf_init(&S, NULL, NULL);
    seq[p++] = '\x1b';
    seq[p++] = '[';
    for (i = 0; i < 63; ++i) seq[p++] = '1';
    seq[p++] = 'x';
    /* 63 param bytes + final byte: dispatch while buffer is exactly full */
    r = feed_seq(&S, &key, seq, (size_t)p);
    asserteq(r, TF_OK);
    asserteq(key.type, TF_TYPE_UNKNOWN_CSI);
    tf_free(&S);
}

TEST(branch_mouse_decode) {
    tf_Key key;
    int    ev, btn, line, col;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MOUSE;

    /* negative button: pure>=0 false */
    key.d.mouse.btn = -1;
    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_UNKNOWN);
    asserteq(btn, 0);

    /* button 4 raw code: pure>=64 false -> unknown */
    key.d.mouse.btn = 4;
    asserteq(tf_mouse(&key, &ev, &btn, &line, &col), TF_OK);
    asserteq(ev, TF_EVENT_UNKNOWN);
    asserteq(btn, 0);
}

TEST(branch_position_modereport_nulls) {
    tf_Key key;
    int    line, init, mode, val;
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_POSITION;
    key.d.pos.line = 1;
    key.d.pos.col = 2;
    asserteq(tf_position(&key, &line, NULL), TF_ERRPARAM);

    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_MODEREPORT;
    key.d.modereport.initial = 0;
    key.d.modereport.mode = 1;
    key.d.modereport.value = 2;
    asserteq(tf_modereport(&key, &init, NULL, &val), TF_ERRPARAM);
    asserteq(tf_modereport(&key, &init, &mode, NULL), TF_ERRPARAM);
}

TEST(branch_fmt_lowerspace_level5) {
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));
    key.type = TF_TYPE_KEYSYM;
    key.d.sym = TF_SYM_LEVEL5_SHIFT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_LOWERSPACE);
    assertstreq(buf, "level5 shift");
}

TEST(branch_fmt_caretctrl_extra) {
    tf_Key key;
    char   buf[64];
    memset(&key, 0, sizeof(key));

    /* CARETCTRL without CTRL: caret branch false */
    key.type = TF_TYPE_UNICODE;
    key.d.codepoint = 'a';
    key.modifiers = TF_MOD_SHIFT;
    tf_format(buf, sizeof(buf), &key, TF_FMT_CARETCTRL | TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<S-a>");

    /* CARETCTRL + CTRL + uppercase: cp>='a' false */
    key.d.codepoint = 'A';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_CARETCTRL | TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<^A>");

    /* CARETCTRL + CTRL + '{': cp>='a' true, cp<='z' false */
    key.d.codepoint = '{';
    key.modifiers = TF_MOD_CTRL;
    tf_format(buf, sizeof(buf), &key, TF_FMT_CARETCTRL | TF_FMT_WRAPBRACKET);
    assertstreq(buf, "<^{>");
}

TEST(branch_parse_modspace) {
    tf_Key key;
    int    n;

    /* modifier followed by space */
    n = tf_parse("<C x>", &key);
    asserteq(n, 5);
    asserteq(key.type, TF_TYPE_UNICODE);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* leading space before modifiers */
    n = tf_parse("< C-x>", &key);
    asserteq(n, 6);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_CTRL);

    /* leading dash before modifiers */
    n = tf_parse("< -C-x>", &key);
    asserteq(n, 7);
    asserteq(key.d.codepoint, 'x');
    asserteq(key.modifiers, TF_MOD_CTRL);
}

TEST(branch_parse_f_trailing) {
    tf_Key key;
    /* <F1x> -> F-number has trailing non-digit */
    asserteq(tf_parse("<F1x>", &key), -1);
}

TEST(branch_parse_empty_unclosed) {
    tf_Key key;
    /* empty key name */
    asserteq(tf_parse("<>", &key), -1);
    /* missing closing '>' */
    asserteq(tf_parse("<F1", &key), -1);
}

#include "termfeed_test.gen.inc"
