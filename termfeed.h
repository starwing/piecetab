#ifndef termfeed_h
#define termfeed_h

#ifndef TF_NS_BEGIN
# ifdef __cplusplus
#   define TF_NS_BEGIN extern "C" {
#   define TF_NS_END   }
# else
#   define TF_NS_BEGIN
#   define TF_NS_END
# endif
#endif /* TF_NS_BEGIN */

#ifndef TF_STATIC
# if __GNUC__
#   define TF_STATIC static __attribute((unused))
# else
#   define TF_STATIC static
# endif
#endif /* TF_STATIC */

#ifdef TF_STATIC_API
# ifndef TF_IMPLEMENTATION
#   define TF_IMPLEMENTATION
# endif
# define TF_API TF_STATIC
#endif /* TF_STATIC_API */

#if !defined(TF_API) && defined(_WIN32)
# ifdef TF_IMPLEMENTATION
#   define TF_API __declspec(dllexport)
# else
#   define TF_API __declspec(dllimport)
# endif
#endif /* TF_API */

#ifndef TF_API
# define TF_API extern
#endif

#include <stddef.h>

#define TF_OK       (0)
#define TF_NONE     (1)
#define TF_AGAIN    (2)
#define TF_ERRPARAM (-1)
#define TF_ERRMEM   (-2)

TF_NS_BEGIN

typedef void       *tf_Alloc(void *ud, void *ptr, size_t osize, size_t nsize);
typedef const char *tf_Reader(void *ud, size_t *plen);
typedef const char *tf_Lookup(void *ud, const char *name);

typedef struct tf_State tf_State;
typedef struct tf_Key   tf_Key;

/* lifecycle */

TF_API void tf_init(tf_State *S, tf_Alloc *allocf, void *alloc_ud);
TF_API void tf_free(tf_State *S);

/* parsing */

TF_API void tf_feed(tf_State *S, tf_Reader *r, void *ud);
TF_API int  tf_readkey(tf_State *S, tf_Key *key);
TF_API int  tf_flush(tf_State *S, tf_Key *key);
TF_API int  tf_waitkey(tf_State *S, int fd, int timeout_ms, tf_Key *key);

/* attributes */

TF_API int tf_setflag(tf_State *S, int flag);
TF_API int tf_load(tf_State *S, tf_Lookup *lookup, void *lookup_ud);

/* names & format */

TF_API const char *tf_name(int sym);

TF_API int tf_sym(const char *name);
TF_API int tf_format(char *buf, int len, const tf_Key *key, int fmt);
TF_API int tf_parse(const char *str, tf_Key *key);

/* interpretation */

TF_API int tf_mouse(const tf_Key *key, int *ev, int *btn, int *line, int *col);
TF_API int tf_position(const tf_Key *key, int *line, int *col);
TF_API int tf_modereport(const tf_Key *key, int *init, int *mode, int *val);
TF_API int tf_csi(const tf_State *S, int args[], int nargs, int *cmd);

/* control strings */

TF_API const char *tf_string(const tf_State *S, int *plen);

/* enums & structures */

/* clang-format off */
typedef enum tf_Flag {
    TF_FLAG_KEEPC0      = 1 << 0, /* C0 (0x00-0x1F) raw, no Ctrl mapping */
    TF_FLAG_CONVERTKP   = 1 << 1, /* keypad (SS3 'p'-'y') to ordinary keys */
    TF_FLAG_SPACESYMBOL = 1 << 2, /* space (0x20) as Space keysym */
    TF_FLAG_DELBS       = 1 << 3  /* DEL (0x7f) as Backspace keysym */
} tf_Flag;

typedef enum tf_Fmt {
    TF_FMT_LONGMOD     = 1 << 0, /* Shift-A rather than S-A */
    TF_FMT_CARETCTRL   = 1 << 1, /* ^X rather than C-X */
    TF_FMT_ALTISMETA   = 1 << 2, /* M- rather than A- */
    TF_FMT_WRAPBRACKET = 1 << 3, /* <Escape> brackets special keys */
    TF_FMT_SPACEMOD    = 1 << 4, /* M Foo rather than M-Foo */
    TF_FMT_LOWERMOD    = 1 << 5, /* meta rather than Meta */
    TF_FMT_LOWERSPACE  = 1 << 6  /* page down rather than PageDown */
} tf_Fmt;

typedef enum tf_Mod {
    TF_MOD_SHIFT = 1 << 0,
    TF_MOD_ALT   = 1 << 1,
    TF_MOD_CTRL  = 1 << 2,
    TF_MOD_SUPER = 1 << 3,
    TF_MOD_HYPER = 1 << 4,
    TF_MOD_META  = 1 << 5,
    TF_MOD_CAPS  = 1 << 6,
    TF_MOD_NUM   = 1 << 7
} tf_Mod;

#define TF_SYMS(X)                                                            \
    X(BACKSPACE, "Backspace") X(TAB,     "Tab")     X(ENTER,     "Enter")     \
    X(ESCAPE,    "Escape")    X(SPACE,   "Space")   X(UP,        "Up")        \
    X(DOWN,      "Down")      X(LEFT,    "Left")    X(RIGHT,     "Right")     \
    X(BEGIN,     "Begin")     X(FIND,    "Find")    X(INSERT,    "Insert")    \
    X(DELETE,    "Delete")    X(SELECT,  "Select")  X(PAGEUP,    "PageUp")    \
    X(PAGEDOWN,  "PageDown")  X(HOME,    "Home")    X(END,       "End")       \
    X(CANCEL,    "Cancel")    X(CLEAR,   "Clear")   X(CLOSE,     "Close")     \
    X(COMMAND,   "Command")   X(COPY,    "Copy")    X(EXIT,      "Exit")      \
    X(HELP,      "Help")      X(MARK,    "Mark")    X(MESSAGE,   "Message")   \
    X(MOVE,      "Move")      X(OPEN,    "Open")    X(OPTIONS,   "Options")   \
    X(PRINT,     "Print")     X(REDO,    "Redo")    X(REFERENCE, "Reference") \
    X(REFRESH,   "Refresh")   X(REPLACE, "Replace") X(RESTART,   "Restart")   \
    X(RESUME,    "Resume")    X(SAVE,    "Save")    X(SUSPEND,   "Suspend")   \
    X(UNDO,      "Undo")      \
    X(CAPSLOCK, "CapsLock") X(SCROLLLOCK,  "ScrollLock")  \
    X(NUMLOCK,  "NumLock")  X(PRINTSCREEN, "PrintScreen") \
    X(PAUSE,    "Pause")    X(MENU,        "Menu")        \
    X(KPENTER,  "kEnter") X(KPEQUALS, "kEqual")  X(KPMULT,  "kMultiply") \
    X(KPPLUS,   "kPlus")  X(KPCOMMA,  "kComma")  X(KPMINUS, "kMinus")    \
    X(KPPERIOD, "kPoint") X(KPDIV,    "kDivide") \
    X(KP0, "k0") X(KP1, "k1") X(KP2, "k2") X(KP3, "k3") X(KP4, "k4") \
    X(KP5, "k5") X(KP6, "k6") X(KP7, "k7") X(KP8, "k8") X(KP9, "k9") \
    X(KPLEFT,   "kLeft") X(KPRIGHT,  "kRight")  X(KPUP,       "kUp")       \
    X(KPDOWN,   "kDown") X(KPPAGEUP, "kPageUp") X(KPPAGEDOWN, "kPageDown") \
    X(KPHOME,   "kHome") X(KPEND,    "kEnd")    X(KPINSERT,   "kInsert")   \
    X(KPDELETE, "kDel")  X(KPORIGIN, "kOrigin") \
    X(MEDIA_PLAY,       "MediaPlay")      X(MEDIA_PAUSE,   "MediaPause")   \
    X(MEDIA_PLAY_PAUSE, "MediaPlayPause") X(MEDIA_REVERSE, "MediaReverse") \
    X(MEDIA_STOP,       "MediaStop")      \
    X(MEDIA_FAST_FORWARD, "MediaFastForward") X(MEDIA_REWIND, "MediaRewind") \
    X(MEDIA_TRACK_NEXT,     "MediaTrackNext")     \
    X(MEDIA_TRACK_PREVIOUS, "MediaTrackPrevious") \
    X(MEDIA_RECORD, "MediaRecord") X(LOWER_VOLUME, "LowerVolume") \
    X(RAISE_VOLUME, "RaiseVolume") X(MUTE_VOLUME,  "MuteVolume")  \
    X(LEFT_SHIFT,   "LeftShift")   X(LEFT_CTRL,    "LeftCtrl")   \
    X(LEFT_ALT,     "LeftAlt")     X(LEFT_SUPER,   "LeftSuper")  \
    X(LEFT_HYPER,   "LeftHyper")   X(LEFT_META,    "LeftMeta")   \
    X(RIGHT_SHIFT,  "RightShift")  X(RIGHT_CTRL,   "RightCtrl")  \
    X(RIGHT_ALT,    "RightAlt")    X(RIGHT_SUPER,  "RightSuper") \
    X(RIGHT_HYPER,  "RightHyper")  X(RIGHT_META,   "RightMeta")  \
    X(LEVEL3_SHIFT, "Level3Shift") X(LEVEL5_SHIFT, "Level5Shift")
/* clang-format on */

typedef enum tf_Sym {
    TF_SYM_NONE = 0,
#define TFX(name, str) TF_SYM_##name,
    TF_SYMS(TFX)
#undef TFX
            TF_SYM_COUNT
} tf_Sym;

typedef enum tf_Type {
    TF_TYPE_NONE = 0,   /* uninitialized key (memset) */
    TF_TYPE_UNICODE,    /* codepoint */
    TF_TYPE_FUNCTION,   /* number */
    TF_TYPE_KEYSYM,     /* sym */
    TF_TYPE_MOUSE,      /* mouse {btn,line,col,release} */
    TF_TYPE_POSITION,   /* pos {line,col} */
    TF_TYPE_MODEREPORT, /* modereport {initial,mode,value} */
    TF_TYPE_DCS,        /* control string, tf_string */
    TF_TYPE_OSC,
    TF_TYPE_APC,
    TF_TYPE_KITTYREPORT, /* kitty negotiation response */
    TF_TYPE_UNKNOWN_CSI  /* raw CSI, tf_csi */
} tf_Type;

typedef enum tf_Event {
    TF_EVENT_UNKNOWN, /* tf_mouse output: uninterpretable code */
    TF_EVENT_PRESS,
    TF_EVENT_REPEAT,
    TF_EVENT_RELEASE,
    TF_EVENT_DRAG /* tf_mouse output: drag (code 0x20 bit) */
} tf_Event;

/* UTF-8 buffer size: longest sequence (6) + NUL */
#define TF_UTF8SZ 7

struct tf_Key {
    tf_Type  type;
    tf_Event event;
    union {
        int    codepoint; /* UNICODE */
        int    number;    /* FUNCTION / KITTYREPORT */
        tf_Sym sym;       /* KEYSYM */
        struct {
            int btn, line, col, release;
        } mouse; /* MOUSE */
        struct {
            int line, col;
        } pos; /* POSITION */
        struct {
            int initial, mode, value;
        } modereport; /* MODEREPORT */
    } d;
    tf_Mod modifiers;
    char   utf8[TF_UTF8SZ]; /* UNICODE key: UTF-8 encoding */
};

typedef struct tf_Node {
    size_t        size; /* ARR: physical slot count (may exceed range) */
    int           sym;  /* KEY: tf_Sym / FUNCTION number; NONE = ARR node */
    int           mod;  /* KEY: static modifiers */
    int           type; /* KEY: TF_TYPE_KEYSYM / TF_TYPE_FUNCTION */
    unsigned char min;  /* ARR: extent lower bound */
    unsigned char max;  /* ARR: extent upper bound */
} tf_Node;

#define TF_STATIC_ASSERT(cond)     TFSA_0(cond, tfSA_, __LINE__)
#define TFSA_0(cond, prefix, line) TFSA_1(cond, prefix, line)
#define TFSA_1(cond, prefix, line) typedef char prefix##line[(cond) ? 1 : -1]

#ifndef TF_MAX_BUFLEN
# define TF_MAX_BUFLEN 64
#endif /* TF_MAX_BUFLEN */

TF_STATIC_ASSERT(TF_MAX_BUFLEN >= 6); /* longest UTF-8 sequence */

#ifndef TF_WAIT_BUFSIZE /* tf_waitkey read buffer */
# define TF_WAIT_BUFSIZE 1024
#endif /* TF_WAIT_BUFSIZE */

typedef struct tf_WaitCtx {
    char  *buf; /* tf_waitkey: read target, lazy TF_WAIT_BUFSIZE */
    size_t len; /* bytes read in, pending delivery (waitkey-owned) */
} tf_WaitCtx;

struct tf_State {
    tf_Alloc *allocf;      /* allocator (realloc-style) */
    void     *alloc_ud;    /* allocator userdata */
    int       flags;       /* TF_FLAG_xxx */
    int       state;       /* DSA state (TF_STATE_xxx) */
    int       pending_mod; /* ESC-prefix pending (peeled \e) */

    /* partial sequence: CSI params / UTF-8 / X10 raw (progress = buf_len) */
    int  buf_len;
    char buf[TF_MAX_BUFLEN];

    /* replay: residual bytes re-parsed as fresh input (flush / CSI overflow /
     * SS3 reject). Source sits at tail: buf[TF_MAX_BUFLEN - replay .. -1]
     * 0 = none. */
    int replay;

    /* control string / kitty text buffer */
    int   cs_len, cs_cap;
    char *cs_buf;

    const char *p;      /* current chunk / replay source */
    size_t      n;      /* bytes left in chunk / replay count */
    tf_Node    *root;   /* trie root (after ESC) */
    tf_Node    *node;   /* current trie node, NULL = dead */
    tf_Reader  *reader; /* current reader (tf_feed) */
    void       *reader_ud;

    /* tf_waitkey: read target, injected via tf_feed — standard chunk
     * path, not a separate input source */
    tf_WaitCtx wait;
};

TF_NS_END

#endif /* termfeed_h */

/* ======================================================================== */
/*                           IMPLEMENTATION                                 */
/* ======================================================================== */

#if defined(TF_IMPLEMENTATION) && !defined(tf_implemented)
#define tf_implemented

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
# include <errno.h>
# include <poll.h>
# include <unistd.h>
#endif

/* DSA states (tf_State.state) */
#define TF_STATE_IDLE      0
#define TF_STATE_ESCAPE    1 /* after \e, awaiting prefix */
#define TF_STATE_CSI       2 /* params accumulating in buf */
#define TF_STATE_SS3       3 /* \eO + one final byte */
#define TF_STATE_CS_DCS    4 /* control strings: \eP ... */
#define TF_STATE_CS_OSC    5 /* \e] ... */
#define TF_STATE_CS_APC    6 /* \e_ ... */
#define TF_STATE_UTF8      7 /* multi-byte sequence in buf */
#define TF_STATE_MOUSE_X10 8 /* 3 raw mouse bytes in buf */

#define tfB(ch) ((ch) & 0xFF)

#define tf_tablesize(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

TF_NS_BEGIN

/* UTF-8 helpers */

static int tfU_utf8len(int b) {
    if (b < 0x80) return 1;
    if (b < 0xC0 || b > 0xFD) return 0;
    if (b < 0xE0) return 2;
    if (b < 0xF0) return 3;
    if (b < 0xF8) return 4;
    if (b < 0xFC) return 5;
    return 6;
}

static int tfU_tocp(const char *s, int len) {
    static const int mask[] = {0, 0x7F, 0x1F, 0x0F, 0x07, 0x03, 0x01};
    static const int min[] = {0, 0, 0x80, 0x800, 0x10000, 0x200000, 0x4000000};
    int              cp, i;
    assert(len >= 1 && len <= 6);
    for (i = 1; i < len; ++i)
        if ((s[i] & 0xC0) != 0x80) return 0xFFFD;
    cp = s[0] & mask[len];
    for (i = 1; i < len; ++i) cp = (cp << 6) | (s[i] & 0x3F);
    if (cp < min[len]) return 0xFFFD;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0xFFFD;
    if (cp == 0xFFFE || cp == 0xFFFF) return 0xFFFD;
    return cp;
}

static int tfU_decode(char *out, const char *seq, int len) {
    int cp = tfU_tocp(seq, len);
    if (cp == 0xFFFD)
        memcpy(out, "\xEF\xBF\xBD", 4);
    else
        memcpy(out, seq, len), out[len] = '\0';
    return cp;
}

static int tfU_encode(char *out, int cp) {
    static const int  bound[] = {0x80, 0x800, 0x10000, 0x110000};
    static const char lead[] = {0x00, 0xC0, 0xE0, 0xF0};
    int               i, len = 1;
    if (cp >= 0x110000) cp = 0xFFFD;
    while (cp >= bound[len - 1]) ++len;
    for (i = len; i > 1; --i) out[i - 1] = (char)(0x80 | (cp & 0x3F)), cp >>= 6;
    out[0] = (char)(lead[len - 1] | cp), out[len] = '\0';
    return len;
}

/* key builders: type + core field (+ utf8); event/modifiers by caller */

/* clang-format off */
static void tfK_keysym(tf_Key *key, tf_Sym sym)
{ key->type = TF_TYPE_KEYSYM, key->d.sym = sym; }

static void tfK_function(tf_Key *key, int n)
{ key->type = TF_TYPE_FUNCTION, key->d.number = n; }

static void tfK_flushmod(tf_State *S, tf_Key *key)
{ key->modifiers |= S->pending_mod, S->pending_mod = 0; }

/* kitty PUA table (defined below with TF_KITTYKEYS): forward decls */
typedef struct tf_KittyEntry { int cp, val, type; } tf_KittyEntry;
/* clang-format on */

static const tf_KittyEntry *tfK_kittyfind(int cp);

static void tfK_utf8(tf_Key *key, const char *seq, int len) {
    const tf_KittyEntry *ke;
    key->type = TF_TYPE_UNICODE;
    /* verbatim: keeps 5/6-byte sequences, unlike tfK_codepoint's re-encode */
    key->d.codepoint = tfU_decode(key->utf8, seq, len);
    if (!(ke = tfK_kittyfind(key->d.codepoint))) return;
    if (ke->type == TF_TYPE_FUNCTION)
        tfK_function(key, ke->val);
    else
        tfK_keysym(key, (tf_Sym)ke->val);
}

static void tfK_codepoint(tf_Key *key, int cp) {
    key->type = TF_TYPE_UNICODE, key->d.codepoint = cp;
    tfU_encode(key->utf8, cp);
}

static void tfK_mouse(tf_Key *key, int code, int col, int line, int rel) {
    key->type = TF_TYPE_MOUSE, key->event = TF_EVENT_PRESS;
    key->d.mouse.release = rel;
    key->modifiers = (code & 0x1c) >> 2;
    key->d.mouse.btn = code & ~0x1c;
    key->d.mouse.col = col, key->d.mouse.line = line;
}

/* trie helpers */

typedef struct tf_KeyEntry {
    const char *name; /* terminfo entry name */
    int         sym;  /* tf_Sym / FUNCTION number */
    int         mod;  /* static modifiers */
    int         type; /* TF_TYPE_KEYSYM / TF_TYPE_FUNCTION */
} tf_KeyEntry;

/* clang-format off */
static const tf_KeyEntry tfT_keytable[] = {
/* terminfo keys are all KEYSYM; F1-F63 load via tfT_loadfkeys */
#define ENTRY(name, sym, mod) {"key_" #name, TF_SYM_##sym, mod, TF_TYPE_KEYSYM}
        ENTRY(backspace,  BACKSPACE, 0), ENTRY(beg,        BEGIN,     0),
        ENTRY(btab,       TAB,       1), ENTRY(clear,      CLEAR,     0),
        ENTRY(dc,         DELETE,    0), ENTRY(end,        END,       0),
        ENTRY(find,       FIND,      0), ENTRY(home,       HOME,      0),
        ENTRY(ic,         INSERT,    0), ENTRY(left,       LEFT,      0),
        ENTRY(npage,      PAGEDOWN,  0), ENTRY(ppage,      PAGEUP,    0),
        ENTRY(right,      RIGHT,     0), ENTRY(select,     SELECT,    0),
        ENTRY(suspend,    SUSPEND,   0), ENTRY(undo,       UNDO,      0),
        ENTRY(sbackspace, BACKSPACE, 1), ENTRY(sdc,        DELETE,    1),
        ENTRY(send,       END,       1), ENTRY(shome,      HOME,      1),
        ENTRY(sic,        INSERT,    1), ENTRY(sleft,      LEFT,      1),
        ENTRY(snext,      PAGEDOWN,  1), ENTRY(sprevious,  PAGEUP,    1),
        ENTRY(sright,     RIGHT,     1),
#undef  ENTRY
};
/* clang-format on */

#define tfT_isarr(n)  ((n)->sym == TF_SYM_NONE)
#define tfT_idx(n, b) ((tf_Node **)((n) + 1))[(int)(b) - (n)->min]

static tf_Node *tfT_mkarr(tf_State *S, int min, int max) {
    tf_Node *n;
    int      nslot = max - min + 1;
    size_t   sz = sizeof(tf_Node) + nslot * sizeof(tf_Node *);
    n = (tf_Node *)S->allocf(S->alloc_ud, NULL, 0, sz);
    if (!n) return NULL;
    memset(n, 0, sz), n->min = (unsigned char)min, n->max = (unsigned char)max;
    return n->size = nslot, n;
}

static tf_Node *tfT_mkkey(tf_State *S, int sym, int mod, int type) {
    tf_Node *n = (tf_Node *)S->allocf(S->alloc_ud, NULL, 0, sizeof(*n));
    if (!n) return NULL;
    return n->size = 0, n->sym = sym, n->mod = mod, n->type = type, n;
}

static tf_Node *tfT_resize(tf_State *S, tf_Node *n, int nmin, int nmax) {
    int      ne = (nmax - nmin) + 1, shift = n->min - nmin;
    size_t   osz = n->size * sizeof(tf_Node *);
    size_t   sz = ne * sizeof(tf_Node *);
    tf_Node *nn = (tf_Node *)S->allocf(S->alloc_ud, NULL, 0, sizeof(*n) + sz);
    if (!nn) return NULL;
    memset(nn, 0, sizeof(*n) + sz);
    nn->min = (unsigned char)nmin, nn->max = (unsigned char)nmax, nn->size = ne;
    memcpy(&tfT_idx(nn, nmin + shift), &tfT_idx(n, n->min), osz);
    S->allocf(S->alloc_ud, n, sizeof(*n) + osz, 0);
    return nn;
}

static tf_Node **tfT_slot(tf_State *S, tf_Node **pn, int b) {
    tf_Node *n = *pn;
    if (b >= n->min && (size_t)b - n->min < n->size) return &tfT_idx(n, b);
    n = tfT_resize(S, n, b < n->min ? b : n->min, b > n->max ? b : n->max);
    if (!n) return NULL;
    return (*pn = n), &tfT_idx(n, b);
}

static int tfT_feed(tf_State *S, tf_Key *key, int b) {
    struct tf_Node *n = S->node, *child;
    int             restart = S->state == TF_STATE_ESCAPE && b == 0x1b;
    if (!n || restart) {
        /* IDLE matches from any first byte; the ESCAPE+ESC ALT-peel
         * also starts a fresh sequence at the root */
        if (S->state != TF_STATE_IDLE && !restart) return 0;
        if (!(n = S->root)) return 0;
    }
    assert(tfT_isarr(n));
    if (b < n->min || (size_t)b - n->min >= n->size) return S->node = NULL, 0;
    child = tfT_idx(n, b);
    if (child && !tfT_isarr(child)) {
        key->type = child->type, key->event = TF_EVENT_PRESS;
        if (child->type == TF_TYPE_FUNCTION)
            key->d.number = child->sym;
        else
            key->d.sym = child->sym;
        key->modifiers = child->mod, tfK_flushmod(S, key);
        S->state = TF_STATE_IDLE, S->node = S->root, S->buf_len = 0;
        return 1;
    }
    return S->node = child, 0;
}

static void tfT_freeall(tf_State *S, tf_Node *n) {
    size_t i, sz;
    if (!n) return;
    sz = tfT_isarr(n) ? n->size * sizeof(tf_Node *) : 0;
    for (i = 0; i < (size_t)n->size; ++i)
        tfT_freeall(S, tfT_idx(n, n->min + i));
    S->allocf(S->alloc_ud, n, sizeof(*n) + sz, 0);
}

/* tf_load */

static int tfT_loadseq(tf_State *S, const char *seq, const tf_KeyEntry *ke) {
    tf_Node **cur;
    int       i, len = (int)strlen(seq);
    cur = &S->root;
    for (i = 0; i < len; ++i) {
        int       b = tfB(seq[i]);
        tf_Node **sl;
        if (!*cur && !(*cur = tfT_mkarr(S, b, b))) return TF_ERRMEM;
        assert(tfT_isarr(*cur));
        if (!(sl = tfT_slot(S, cur, b))) return TF_ERRMEM;
        if (i == len - 1) {
            if (*sl) return TF_OK; /* slot taken: first-loaded key wins */
            *sl = tfT_mkkey(S, ke->sym, ke->mod, ke->type);
        } else if (*sl && !tfT_isarr(*sl))
            return TF_OK; /* prefix conflict: longer key unreachable */
        else if (!*sl)
            *sl = tfT_mkarr(S, tfB(seq[i + 1]), tfB(seq[i + 1]));
        if (!*sl) return TF_ERRMEM;
        cur = sl;
    }
    return TF_OK;
}

static int tfT_loadtable(tf_State *S, tf_Lookup *lu, void *ud) {
    int i, r;
    for (i = 0; i < tf_tablesize(tfT_keytable); ++i) {
        const char *seq = lu(ud, tfT_keytable[i].name);
        if (seq && (r = tfT_loadseq(S, seq, &tfT_keytable[i])) != TF_OK)
            return r;
    }
    return TF_OK;
}

static int tfT_loadfkeys(tf_State *S, tf_Lookup *lu, void *ud) {
    int k;
    for (k = 1; k <= 63; ++k) {
        char        fnbuf[16];
        const char *seq;
        tf_KeyEntry ke;
        snprintf(fnbuf, sizeof(fnbuf), "key_f%d", k);
        seq = lu(ud, fnbuf);
        if (!seq) return TF_OK;
        ke.name = fnbuf, ke.sym = k, ke.mod = 0, ke.type = TF_TYPE_FUNCTION;
        if (tfT_loadseq(S, seq, &ke) != TF_OK) return TF_ERRMEM;
    }
    return TF_OK;
}

/* CS buffer helpers */

static int tfD_append(tf_State *S, const char *data, int dlen) {
    int   need, nsz;
    void *p;
    need = S->cs_len + dlen;
    if (need > S->cs_cap) {
        nsz = S->cs_cap ? S->cs_cap * 2 : 64;
        while (nsz < need) nsz *= 2;
        p = S->allocf(S->alloc_ud, S->cs_buf, S->cs_cap, nsz + 1);
        if (!p) return TF_ERRMEM;
        S->cs_buf = (char *)p, S->cs_cap = nsz;
    }
    memcpy(S->cs_buf + S->cs_len, data, dlen);
    S->cs_len += dlen, S->cs_buf[S->cs_len] = '\0'; /* NUL-terminated */
    return TF_OK;
}

static void tfD_canon(tf_Key *key, int b, int keepc0) {
    key->event = TF_EVENT_PRESS, key->modifiers = 0;
    if (b == 0x00)
        tfK_keysym(key, TF_SYM_SPACE), key->modifiers = TF_MOD_CTRL;
    else if ((b >= 0x20 && b < 0x7f) || keepc0)
        tfK_codepoint(key, b);
    else if (b >= 0x80)
        tfK_codepoint(key, 0xFFFD);
    else if (b == 0x7f)
        tfK_keysym(key, TF_SYM_DELETE);
    else {
        b += 0x40, tfK_codepoint(key, b >= 'A' && b <= 'Z' ? b + 0x20 : b);
        key->modifiers = TF_MOD_CTRL;
    }
}

static int tfD_symbyte(tf_State *S, tf_Key *key, int b, int keepc0) {
    tf_Sym sym = TF_SYM_NONE;
    if (!keepc0 && b == 0x09)
        sym = TF_SYM_TAB;
    else if (!keepc0 && b == 0x0d)
        sym = TF_SYM_ENTER;
    else if (b == 0x7f && (S->flags & TF_FLAG_DELBS))
        sym = TF_SYM_BACKSPACE;
    else if (b == 0x20 && (S->flags & TF_FLAG_SPACESYMBOL))
        sym = TF_SYM_SPACE;
    if (sym == TF_SYM_NONE) return 0;
    return tfK_keysym(key, sym), (key->modifiers = 0), 1;
}

static void tfD_processbyte(tf_State *S, tf_Key *key, int b) {
    int kc = S->flags & TF_FLAG_KEEPC0;
    if (!tfD_symbyte(S, key, b, kc)) tfD_canon(key, b, kc);
}

static int tfD_utf8start(tf_State *S, tf_Key *key, int b) {
    char seq[6];
    int  len = tfU_utf8len(b);
    assert(len >= 2 && len <= 6);
    if (S->n < (size_t)(len - 1)) {
        S->buf[0] = tfB(b), S->buf_len = 1;
        return (S->state = TF_STATE_UTF8, S->node = NULL), TF_AGAIN;
    }
    seq[0] = tfB(b), memcpy(seq + 1, S->p, len - 1);
    tfK_utf8(key, seq, len), tfK_flushmod(S, key);
    S->p += len - 1, S->n -= len - 1;
    return (S->state = TF_STATE_IDLE), TF_OK;
}

static int tfD_utf8(tf_State *S, tf_Key *key, int b) {
    int len;
    assert(S->buf_len > 0 && S->buf_len < 7);
    S->buf[S->buf_len++] = tfB(b);
    len = tfU_utf8len(tfB(S->buf[0]));
    if (S->buf_len < len) return TF_AGAIN;
    tfK_utf8(key, S->buf, len), tfK_flushmod(S, key);
    S->buf_len = 0;
    return (S->state = TF_STATE_IDLE), TF_OK;
}

static int tfD_idle(tf_State *S, tf_Key *key, int b) {
    if (b == 0x1b) return (S->state = TF_STATE_ESCAPE), TF_AGAIN;
    if (tfU_utf8len(b) >= 2) return tfD_utf8start(S, key, b);
    return tfD_processbyte(S, key, b), TF_OK;
}

static int tfD_csstart(tf_State *S, int st) {
    return S->node = NULL, S->cs_len = 0, S->state = st, TF_AGAIN;
}

static int tfD_escape(tf_State *S, tf_Key *key, int b) {
    if (b == 0x1b) return (S->pending_mod = TF_MOD_ALT), TF_AGAIN;
    switch (b) {
    case '[': return (S->buf_len = 0, S->state = TF_STATE_CSI), TF_AGAIN;
    case 'O': return (S->state = TF_STATE_SS3), TF_AGAIN;
    case ']': return tfD_csstart(S, TF_STATE_CS_OSC);
    case 'P': return tfD_csstart(S, TF_STATE_CS_DCS);
    case '_': return tfD_csstart(S, TF_STATE_CS_APC);
    default:
        if (tfU_utf8len(b) >= 2)
            return (S->pending_mod = TF_MOD_ALT), tfD_utf8start(S, key, b);
        tfD_processbyte(S, key, b);
        key->modifiers |= TF_MOD_ALT, S->pending_mod = 0;
        return (S->state = TF_STATE_IDLE), TF_OK;
    }
}

static void tfD_cskey(tf_State *S, tf_Key *key) {
    key->event = TF_EVENT_PRESS;
    assert(S->state >= TF_STATE_CS_DCS && S->state <= TF_STATE_CS_APC);
    switch (S->state) {
    case TF_STATE_CS_DCS: key->type = TF_TYPE_DCS; break;
    case TF_STATE_CS_OSC: key->type = TF_TYPE_OSC; break;
    case TF_STATE_CS_APC: key->type = TF_TYPE_APC; break;
    }
    tfK_flushmod(S, key), S->state = TF_STATE_IDLE;
}

static int tfD_cs(tf_State *S, tf_Key *key, int b) {
    char ch;
    /* termination: BEL, ST (0x9c), or \e\ (trailing \e already in cs_buf) */
    if (b == 0x07 || b == 0x9c) return tfD_cskey(S, key), TF_OK;
    if (b == 0x5c && S->cs_len > 0 && tfB(S->cs_buf[S->cs_len - 1]) == 0x1b) {
        S->cs_len -= 1; /* drop the \e of \e\ (current \ never buffered) */
        return tfD_cskey(S, key), TF_OK;
    }
    if (ch = tfB(b), tfD_append(S, &ch, 1) != TF_OK)
        return S->state = TF_STATE_IDLE, TF_ERRMEM;
    return TF_AGAIN;
}

/* CSI parameter helpers */

static int tfC_mods(int m) { return m > 0 ? m - 1 : 0; }

static int tfC_fieldval(const char *f, int len, int dflt) {
    int i, val = 0;
    for (i = 0; i < len && f[i] != ':' && (f[i] < '0' || f[i] > '9'); ++i)
        continue; /* skip leading non-digits */
    if (i >= len || f[i] == ':') return dflt;
    for (; i < len && f[i] >= '0' && f[i] <= '9'; ++i)
        val = val * 10 + (f[i] - '0');
    return val;
}

static int tfC_subval(const char *f, int len, int dflt) {
    int i;
    for (i = 0; i < len && f[i] != ':'; ++i) continue;
    return (i < len) ? tfC_fieldval(f + i + 1, len - i - 1, dflt) : dflt;
}

static int tfC_nextarg(const tf_State *S, const char **pf, int *plen) {
    const char *f = *pf, *end = S->buf + (S->buf_len > 0 ? S->buf_len - 1 : 0);
    int         c;
    if (!f) f = S->buf, *plen = 0; /* first call */
    f += *plen, c = f < end ? tfB(*f) : 0;
    if (*pf ? c == ';' : c >= 0x3C && c <= 0x3F)
        c = ++f < end ? tfB(*f) : 0; /* skip initial / separator */
    if (f >= end || (c >= 0x20 && c <= 0x2F))
        return *pf = end, *plen = 0, 0; /* exhausted: idempotent, no restart */
    *pf = f;
    while (f < end && (c = tfB(*f)) != ';' && !(c >= 0x20 && c <= 0x2F)) ++f;
    return (*plen = (int)(f - *pf)), 1;
}

static int tfD_event(tf_Key *key, int mods, int ev) {
    key->modifiers = tfC_mods(mods);
    if (ev == 1 || ev == 0)
        key->event = TF_EVENT_PRESS;
    else if (ev == 2)
        key->event = TF_EVENT_REPEAT;
    else if (ev == 3)
        key->event = TF_EVENT_RELEASE;
    else
        return key->type = TF_TYPE_UNKNOWN_CSI, 0;
    return 1;
}

static int tfC_cursorkey(tf_Key *key, int cmd) {
    tf_Sym sym;
    switch (cmd) {
    case 'A': sym = TF_SYM_UP; break;
    case 'B': sym = TF_SYM_DOWN; break;
    case 'C': sym = TF_SYM_RIGHT; break;
    case 'D': sym = TF_SYM_LEFT; break;
    case 'E': sym = TF_SYM_BEGIN; break;
    case 'F': sym = TF_SYM_END; break;
    case 'H': sym = TF_SYM_HOME; break;
    default: return 0;
    }
    return tfK_keysym(key, sym), 1;
}

static int tfC_modifykey(tf_Key *key, int mods, int code) {
    key->modifiers = tfC_mods(mods);
    key->type = TF_TYPE_UNKNOWN_CSI;
    return code >= 0 ? (tfK_codepoint(key, code), 1) : 0;
}

static int tfC_funckey(const tf_State *S, tf_Key *key) {
    /* xterm CSI 11-34 → F number (unmapped slots keep n) */
    static const int fkeymap[] = {1,  2,  3,  4,  5,  16, 6,  7,
                                  8,  9,  10, 22, 11, 12, 13, 14,
                                  27, 15, 16, 30, 17, 18, 19, 20};

    const char *f = NULL;
    int         len, n, mods = 0, ev = 0, code = -1;
    tfC_nextarg(S, &f, &len); /* field 1 always exists */
    n = tfC_fieldval(f, len, -1);
    if (n < 0) return key->type = TF_TYPE_UNKNOWN_CSI, 0;
    if (tfC_nextarg(S, &f, &len))
        mods = tfC_fieldval(f, len, 0), ev = tfC_subval(f, len, 0);
    if (tfC_nextarg(S, &f, &len)) code = tfC_fieldval(f, len, -1);
    if (n == 27) return tfC_modifykey(key, mods, code);
    if (n >= 1 && n <= 8)
        tfK_keysym(key, TF_SYM_FIND + (n - 1));
    else
        tfK_function(key, n >= 11 && n <= 34 ? fkeymap[n - 11] : n);
    return tfD_event(key, mods, ev), 1;
}

/* kitty CSI u */

/* kitty keyboard protocol: codepoint → key mapping. */
#define TF_KITTYKEYS(X)                           \
    X(9, TF_SYM_TAB, KEYSYM)                      \
    X(13, TF_SYM_ENTER, KEYSYM)                   \
    X(27, TF_SYM_ESCAPE, KEYSYM)                  \
    X(127, TF_SYM_BACKSPACE, KEYSYM)              \
    X(57344, TF_SYM_ESCAPE, KEYSYM)               \
    X(57345, TF_SYM_ENTER, KEYSYM)                \
    X(57346, TF_SYM_TAB, KEYSYM)                  \
    X(57347, TF_SYM_BACKSPACE, KEYSYM)            \
    X(57348, TF_SYM_INSERT, KEYSYM)               \
    X(57349, TF_SYM_DELETE, KEYSYM)               \
    X(57350, TF_SYM_LEFT, KEYSYM)                 \
    X(57351, TF_SYM_RIGHT, KEYSYM)                \
    X(57352, TF_SYM_UP, KEYSYM)                   \
    X(57353, TF_SYM_DOWN, KEYSYM)                 \
    X(57354, TF_SYM_PAGEUP, KEYSYM)               \
    X(57355, TF_SYM_PAGEDOWN, KEYSYM)             \
    X(57356, TF_SYM_HOME, KEYSYM)                 \
    X(57357, TF_SYM_END, KEYSYM)                  \
    X(57358, TF_SYM_CAPSLOCK, KEYSYM)             \
    X(57359, TF_SYM_SCROLLLOCK, KEYSYM)           \
    X(57360, TF_SYM_NUMLOCK, KEYSYM)              \
    X(57361, TF_SYM_PRINTSCREEN, KEYSYM)          \
    X(57362, TF_SYM_PAUSE, KEYSYM)                \
    X(57363, TF_SYM_MENU, KEYSYM)                 \
    X(57364, 1, FUNCTION)                         \
    X(57365, 2, FUNCTION)                         \
    X(57366, 3, FUNCTION)                         \
    X(57367, 4, FUNCTION)                         \
    X(57368, 5, FUNCTION)                         \
    X(57369, 6, FUNCTION)                         \
    X(57370, 7, FUNCTION)                         \
    X(57371, 8, FUNCTION)                         \
    X(57372, 9, FUNCTION)                         \
    X(57373, 10, FUNCTION)                        \
    X(57374, 11, FUNCTION)                        \
    X(57375, 12, FUNCTION)                        \
    X(57376, 13, FUNCTION)                        \
    X(57377, 14, FUNCTION)                        \
    X(57378, 15, FUNCTION)                        \
    X(57379, 16, FUNCTION)                        \
    X(57380, 17, FUNCTION)                        \
    X(57381, 18, FUNCTION)                        \
    X(57382, 19, FUNCTION)                        \
    X(57383, 20, FUNCTION)                        \
    X(57384, 21, FUNCTION)                        \
    X(57385, 22, FUNCTION)                        \
    X(57386, 23, FUNCTION)                        \
    X(57387, 24, FUNCTION)                        \
    X(57388, 25, FUNCTION)                        \
    X(57389, 26, FUNCTION)                        \
    X(57390, 27, FUNCTION)                        \
    X(57391, 28, FUNCTION)                        \
    X(57392, 29, FUNCTION)                        \
    X(57393, 30, FUNCTION)                        \
    X(57394, 31, FUNCTION)                        \
    X(57395, 32, FUNCTION)                        \
    X(57396, 33, FUNCTION)                        \
    X(57397, 34, FUNCTION)                        \
    X(57398, 35, FUNCTION)                        \
    X(57399, TF_SYM_KP0, KEYSYM)                  \
    X(57400, TF_SYM_KP1, KEYSYM)                  \
    X(57401, TF_SYM_KP2, KEYSYM)                  \
    X(57402, TF_SYM_KP3, KEYSYM)                  \
    X(57403, TF_SYM_KP4, KEYSYM)                  \
    X(57404, TF_SYM_KP5, KEYSYM)                  \
    X(57405, TF_SYM_KP6, KEYSYM)                  \
    X(57406, TF_SYM_KP7, KEYSYM)                  \
    X(57407, TF_SYM_KP8, KEYSYM)                  \
    X(57408, TF_SYM_KP9, KEYSYM)                  \
    X(57409, TF_SYM_KPPERIOD, KEYSYM)             \
    X(57410, TF_SYM_KPDIV, KEYSYM)                \
    X(57411, TF_SYM_KPMULT, KEYSYM)               \
    X(57412, TF_SYM_KPMINUS, KEYSYM)              \
    X(57413, TF_SYM_KPPLUS, KEYSYM)               \
    X(57414, TF_SYM_KPENTER, KEYSYM)              \
    X(57415, TF_SYM_KPEQUALS, KEYSYM)             \
    X(57416, TF_SYM_KPCOMMA, KEYSYM)              \
    X(57417, TF_SYM_KPLEFT, KEYSYM)               \
    X(57418, TF_SYM_KPRIGHT, KEYSYM)              \
    X(57419, TF_SYM_KPUP, KEYSYM)                 \
    X(57420, TF_SYM_KPDOWN, KEYSYM)               \
    X(57421, TF_SYM_KPPAGEUP, KEYSYM)             \
    X(57422, TF_SYM_KPPAGEDOWN, KEYSYM)           \
    X(57423, TF_SYM_KPHOME, KEYSYM)               \
    X(57424, TF_SYM_KPEND, KEYSYM)                \
    X(57425, TF_SYM_KPINSERT, KEYSYM)             \
    X(57426, TF_SYM_KPDELETE, KEYSYM)             \
    X(57427, TF_SYM_KPORIGIN, KEYSYM)             \
    X(57428, TF_SYM_MEDIA_PLAY, KEYSYM)           \
    X(57429, TF_SYM_MEDIA_PAUSE, KEYSYM)          \
    X(57430, TF_SYM_MEDIA_PLAY_PAUSE, KEYSYM)     \
    X(57431, TF_SYM_MEDIA_REVERSE, KEYSYM)        \
    X(57432, TF_SYM_MEDIA_STOP, KEYSYM)           \
    X(57433, TF_SYM_MEDIA_FAST_FORWARD, KEYSYM)   \
    X(57434, TF_SYM_MEDIA_REWIND, KEYSYM)         \
    X(57435, TF_SYM_MEDIA_TRACK_NEXT, KEYSYM)     \
    X(57436, TF_SYM_MEDIA_TRACK_PREVIOUS, KEYSYM) \
    X(57437, TF_SYM_MEDIA_RECORD, KEYSYM)         \
    X(57438, TF_SYM_LOWER_VOLUME, KEYSYM)         \
    X(57439, TF_SYM_RAISE_VOLUME, KEYSYM)         \
    X(57440, TF_SYM_MUTE_VOLUME, KEYSYM)          \
    X(57441, TF_SYM_LEFT_SHIFT, KEYSYM)           \
    X(57442, TF_SYM_LEFT_CTRL, KEYSYM)            \
    X(57443, TF_SYM_LEFT_ALT, KEYSYM)             \
    X(57444, TF_SYM_LEFT_SUPER, KEYSYM)           \
    X(57445, TF_SYM_LEFT_HYPER, KEYSYM)           \
    X(57446, TF_SYM_LEFT_META, KEYSYM)            \
    X(57447, TF_SYM_RIGHT_SHIFT, KEYSYM)          \
    X(57448, TF_SYM_RIGHT_CTRL, KEYSYM)           \
    X(57449, TF_SYM_RIGHT_ALT, KEYSYM)            \
    X(57450, TF_SYM_RIGHT_SUPER, KEYSYM)          \
    X(57451, TF_SYM_RIGHT_HYPER, KEYSYM)          \
    X(57452, TF_SYM_RIGHT_META, KEYSYM)           \
    X(57453, TF_SYM_LEVEL3_SHIFT, KEYSYM)         \
    X(57454, TF_SYM_LEVEL5_SHIFT, KEYSYM)

static const tf_KittyEntry *tfK_kittyfind(int cp) {
    static const tf_KittyEntry table[] = {
#define X(cp, val, ty) {cp, val, TF_TYPE_##ty},
            TF_KITTYKEYS(X)
#undef X
    };
    int n = tf_tablesize(table), lo = 0, hi = n;
    while (lo < hi) {
        int mid = lo + ((hi - lo) >> 1);
        if (table[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < n && table[lo].cp == cp) ? &table[lo] : NULL;
}

static void tfC_kittytext(tf_State *S, const char *fp, int fl) {
    int  cp, r, i = 0;
    char tmp[TF_UTF8SZ];
    S->cs_len = 0;
    while (i < fl) {
        for (cp = 0; i < fl && fp[i] >= '0' && fp[i] <= '9'; ++i)
            cp = cp * 10 + (fp[i] - '0');
        if (cp > 0 && cp <= 0x10FFFF) {
            r = tfD_append(S, tmp, tfU_encode(tmp, cp));
            if (r != TF_OK) return;
        }
        while (i < fl && !(fp[i] >= '0' && fp[i] <= '9')) ++i;
    }
}

static int tfC_kitty(tf_State *S, tf_Key *key) {
    const tf_KittyEntry *ke;
    const char          *f = NULL, *text = NULL;
    int                  len, cp = -1, mods = 0, ev = 0, textn = 0;
    tfC_nextarg(S, &f, &len); /* field 1 always exists */
    cp = tfC_fieldval(f, len, -1);
    if (cp < 0) return (key->type = TF_TYPE_UNKNOWN_CSI), 0;
    if (tfC_nextarg(S, &f, &len))
        mods = tfC_fieldval(f, len, 0), ev = tfC_subval(f, len, 0);
    if (!tfD_event(key, mods, ev)) return 0; /* invalid event */
    if (!(ke = tfK_kittyfind(cp))) {
        tfK_codepoint(key, cp);
        if (tfC_nextarg(S, &f, &len)) text = f, textn = len;
        if (textn > 0) tfC_kittytext(S, text, textn);
    } else if (ke->type == TF_TYPE_FUNCTION)
        tfK_function(key, ke->val);
    else
        tfK_keysym(key, ke->val);
    return 1;
}

/* mouse decode */

static void tfM_decodeX10(tf_Key *key, const char *raw) {
    int k = tfB(raw[0]) - 0x20, c = tfB(raw[1]) - 0x20, l = tfB(raw[2]) - 0x20;
    tfK_mouse(key, k, c, l, 0);
}

static int tfM_args(const tf_State *S, int v[3]) {
    const char *f = NULL;
    int         len, n;
    for (n = 0; n < 3 && tfC_nextarg(S, &f, &len); ++n)
        v[n] = tfC_fieldval(f, len, 0);
    return n;
}

static void tfM_dispatch(tf_State *S, tf_Key *key) {
    int v[3] = {0};
    if (tfM_args(S, v) >= 3)
        tfK_mouse(key, v[0], v[1], v[2], 0);
    else if (S->n >= 3) {
        tfM_decodeX10(key, S->p);
        S->p += 3, S->n -= 3;
    } else {
        /* "M" + raw: replay-ready from the start */
        S->buf[0] = 'M', S->buf_len = 1, S->node = NULL;
        S->state = TF_STATE_MOUSE_X10;
    }
}

/* clang-format off */
static void tfM_csi(const tf_State *S, tf_Key *key, int rel)
{ int v[3] = {0}; tfM_args(S, v), tfK_mouse(key, v[0], v[1], v[2], rel); }
/* clang-format on */

/* CSI main dispatch */

static int tfC_cmd(const tf_State *S, int final) {
    int i, cmd = final, b = tfB(S->buf[0]);
    if (S->buf_len > 0 && b >= 0x3C && b <= 0x3F) cmd |= b << 8;
    for (i = 0; i < S->buf_len; ++i)
        if (b = tfB(S->buf[i]), b >= 0x20 && b <= 0x2F) return cmd | (b << 16);
    return cmd;
}

static void tfC_shifttab(const tf_State *S, tf_Key *key) {
    const char *f = NULL;
    int         len, mods = 0;
    tfC_nextarg(S, &f, &len); /* field 1 always exists */
    if (tfC_nextarg(S, &f, &len)) mods = tfC_fieldval(f, len, 0);
    tfK_keysym(key, TF_SYM_TAB), key->modifiers = TF_MOD_SHIFT | tfC_mods(mods);
}

static void tfC_kittyreport(const tf_State *S, tf_Key *key) {
    const char *f = NULL;
    int         len;
    tfC_nextarg(S, &f, &len); /* field 1 always exists */
    key->type = TF_TYPE_KITTYREPORT;
    key->d.number = tfC_fieldval(f, len, -1);
}

static void tfC_cpr(const tf_State *S, tf_Key *key) {
    const char *f = NULL;
    int         len;
    tfC_nextarg(S, &f, &len); /* field 1 always exists */
    key->type = TF_TYPE_POSITION;
    key->d.pos.line = tfC_fieldval(f, len, 0), key->d.pos.col = 0;
    if (tfC_nextarg(S, &f, &len)) key->d.pos.col = tfC_fieldval(f, len, 0);
}

static void tfC_modereport(const tf_State *S, tf_Key *key, int cmd) {
    const char *f = NULL;
    int         len;
    key->type = TF_TYPE_MODEREPORT;
    key->d.modereport.initial = tfB(cmd >> 8) == '?' ? '?' : 0;
    key->d.modereport.mode = -1, key->d.modereport.value = -1;
    if (tfC_nextarg(S, &f, &len))
        key->d.modereport.mode = tfC_fieldval(f, len, -1);
    if (tfC_nextarg(S, &f, &len))
        key->d.modereport.value = tfC_fieldval(f, len, -1);
}

static void tfC_cursormods(const tf_State *S, tf_Key *key) {
    const char *f = NULL;
    int         len;
    tfC_nextarg(S, &f, &len); /* skip field 1 */
    if (tfC_nextarg(S, &f, &len))
        tfD_event(key, tfC_fieldval(f, len, 0), tfC_subval(f, len, 0));
}

static int tfD_csidispatch(tf_State *S, tf_Key *key) {
    int cmd = tfC_cmd(S, S->buf[S->buf_len - 1]);
    key->event = TF_EVENT_PRESS, key->modifiers = 0;
    switch (cmd) {
    case 'Z': tfC_shifttab(S, key); break;
    case '~': tfC_funckey(S, key); break;
    case 'u': tfC_kitty(S, key); break;
    case 'u' | ('?' << 8): tfC_kittyreport(S, key); break;
    case 'M': tfM_dispatch(S, key); break;
    case 'M' | ('<' << 8): tfM_csi(S, key, 0); break;
    case 'm' | ('<' << 8): tfM_csi(S, key, 1); break;
    case 'R' | ('?' << 8): tfC_cpr(S, key); break;
    case 'R': tfK_function(key, 3); break;
    case 'y' | ('$' << 16):
    case 'y' | ('?' << 8) | ('$' << 16): tfC_modereport(S, key, cmd); break;
    default:
        key->type = TF_TYPE_UNKNOWN_CSI;
        if (tfC_cursorkey(key, cmd)) tfC_cursormods(S, key);
    }
    /* 'M' X10 continuation: state switched, key still pending */
    if (S->state == TF_STATE_MOUSE_X10) return TF_AGAIN;
    tfK_flushmod(S, key);
    return S->state = TF_STATE_IDLE, TF_OK;
}

/* CSI / MOUSE_X10 state handlers */

static int tfD_csi(tf_State *S, tf_Key *key, int b) {
    if (S->buf_len < TF_MAX_BUFLEN - 1 || b >= 0x40) {
        S->buf[S->buf_len++] = tfB(b);
        return b >= 0x40 ? tfD_csidispatch(S, key) : TF_AGAIN;
    }
    S->buf[S->buf_len++] = tfB(b); /* reserved slot: replay includes b */
    tfK_codepoint(key, '['), key->modifiers = TF_MOD_ALT, S->pending_mod = 0;
    memmove(S->buf + TF_MAX_BUFLEN - S->buf_len, S->buf,
            (size_t)S->buf_len); /* full buf: tail move is a no-op */
    S->replay = S->buf_len, S->buf_len = 0;
    return S->state = TF_STATE_IDLE, TF_OK;
}

static int tfD_mousex10(tf_State *S, tf_Key *key, int b) {
    assert(S->buf_len < 4); /* buf[0] = 'M', raw at [1..3] */
    S->buf[S->buf_len++] = tfB(b);
    if (S->buf_len < 4) return TF_AGAIN;
    tfM_decodeX10(key, S->buf + 1), tfK_flushmod(S, key);
    return (S->state = TF_STATE_IDLE, S->buf_len = 0), TF_OK;
}

/* SS3 helpers */

static int tfS_kpkey(int cmd, int ckp, tf_Key *key) {
    tf_Sym sym;
    switch (cmd) {
    case 'M': sym = TF_SYM_KPENTER; break;
    case 'X': sym = TF_SYM_KPEQUALS; break;
    case 'j': sym = TF_SYM_KPMULT; break;
    case 'k': sym = TF_SYM_KPPLUS; break;
    case 'l': sym = TF_SYM_KPCOMMA; break;
    case 'm': sym = TF_SYM_KPMINUS; break;
    case 'n': sym = TF_SYM_KPPERIOD; break;
    case 'o': sym = TF_SYM_KPDIV; break;
    default:
        if (cmd < 'p' || cmd > 'y') return 0;
        sym = (TF_SYM_KP0 + (cmd - 'p'));
    }
    tfK_keysym(key, sym);
    if (ckp && cmd != 'M')
        tfK_codepoint(key, "=*+,-./0123456789"[sym - TF_SYM_KPEQUALS]);
    return 1;
}

static void tfS_altreplay(tf_State *S, tf_Key *key, int b) {
    tfK_codepoint(key, 'O');
    key->modifiers = TF_MOD_ALT;
    S->buf[TF_MAX_BUFLEN - 1] = tfB(b);
    S->replay = 1, S->buf_len = 0;
    S->pending_mod = 0, S->state = TF_STATE_IDLE;
}

static int tfD_ss3dispatch(tf_State *S, tf_Key *key, int cmd) {
    int ckp = S->flags & TF_FLAG_CONVERTKP;
    key->event = TF_EVENT_PRESS, key->modifiers = 0;
    if (cmd >= 'P' && cmd <= 'S')
        tfK_function(key, cmd - 'P' + 1);
    else if (!tfC_cursorkey(key, cmd) && !tfS_kpkey(cmd, ckp, key))
        return tfS_altreplay(S, key, cmd), TF_OK;
    tfK_flushmod(S, key);
    return S->state = TF_STATE_IDLE, TF_OK;
}

static int tfD_ss3(tf_State *S, tf_Key *key, int b) {
    if (b >= 0x40 && b <= 0x7E) return tfD_ss3dispatch(S, key, b);
    return tfS_altreplay(S, key, b), TF_OK; /* unknown prefix */
}

/* public API */

/* clang-format off */
TF_API int tf_setflag(tf_State *S, int flag)
{ int old; return S ? (old = S->flags, S->flags = flag, old) : 0; }
/* clang-format on */

static void *tfS_defalloc(void *ud, void *p, size_t osize, size_t nsize) {
    void *np;
    (void)ud, (void)osize;
    if (nsize == 0) return (void)free(p), NULL;
    return (np = realloc(p, nsize)) ? np : ((void)abort(), NULL);
}

TF_API void tf_init(tf_State *S, tf_Alloc *allocf, void *alloc_ud) {
    if (!S) return;
    memset(S, 0, sizeof(*S));
    S->allocf = allocf ? allocf : tfS_defalloc;
    S->alloc_ud = alloc_ud;
}

TF_API void tf_free(tf_State *S) {
    if (!S) return;
    tfT_freeall(S, S->root);
    if (S->cs_buf) S->allocf(S->alloc_ud, S->cs_buf, S->cs_cap, 0);
    if (S->wait.buf) S->allocf(S->alloc_ud, S->wait.buf, TF_WAIT_BUFSIZE, 0);
    memset(S, 0, sizeof(*S));
}

TF_API int tf_load(tf_State *S, tf_Lookup *lu, void *ud) {
    int r;
    if (!S || !lu) return TF_ERRPARAM;
    tfT_freeall(S, S->root); /* re-entrant: drop the previous trie */
    S->root = NULL, S->node = NULL;
    if ((r = tfT_loadtable(S, lu, ud)) != TF_OK) return r;
    return tfT_loadfkeys(S, lu, ud);
}

TF_API void tf_feed(tf_State *S, tf_Reader *r, void *ud) {
    if (!S) return;
    S->p = NULL, S->n = 0; /* switching readers discards the old chunk */
    S->reader = r, S->reader_ud = ud;
}

static int tfZ_nextbyte(tf_State *S) {
    if (S->replay > 0) {
        /* replay source: buf tail, index independent of buf_len */
        int b = tfB(S->buf[TF_MAX_BUFLEN - S->replay]);
        return S->replay--, b;
    }
    if (S->n == 0) {
        const char *chunk;
        size_t      len = 0;
        if (!S->reader) return -1;
        chunk = S->reader(S->reader_ud, &len);
        if (!chunk || len == 0) return -1;
        S->p = chunk, S->n = len;
    }
    return S->n--, tfB(*(S->p++));
}

TF_API int tf_readkey(tf_State *S, tf_Key *key) {
    int b, r = TF_AGAIN;
    if (!S || !key) return TF_ERRPARAM;
    key->type = TF_TYPE_NONE, key->event = TF_EVENT_PRESS, key->modifiers = 0;
    while (r == TF_AGAIN) { /* main loop: trie + DSA */
        if ((b = tfZ_nextbyte(S)) < 0)
            return S->state == TF_STATE_IDLE ? TF_NONE : TF_AGAIN;
        if (tfT_feed(S, key, b)) return TF_OK;
        switch (S->state) {
        case TF_STATE_IDLE: r = tfD_idle(S, key, b); break;
        case TF_STATE_ESCAPE: r = tfD_escape(S, key, b); break;
        case TF_STATE_CSI: r = tfD_csi(S, key, b); break;
        case TF_STATE_SS3: r = tfD_ss3(S, key, b); break;
        case TF_STATE_MOUSE_X10: r = tfD_mousex10(S, key, b); break;
        case TF_STATE_CS_DCS:
        case TF_STATE_CS_OSC:
        case TF_STATE_CS_APC: r = tfD_cs(S, key, b); break;
        case TF_STATE_UTF8: r = tfD_utf8(S, key, b); break;
        }
    }
    return r;
}

static void tfD_flushkey(tf_State *S, tf_Key *key, int c) {
    tfK_codepoint(key, c); /* c is always ASCII */
    key->modifiers = TF_MOD_ALT, S->pending_mod = 0;
    S->node = S->root;
}

TF_API int tf_flush(tf_State *S, tf_Key *key) {
    if (!S || !key) return TF_ERRPARAM;
    key->type = TF_TYPE_NONE, key->event = TF_EVENT_PRESS;
    if (S->state == TF_STATE_IDLE) return tfK_keysym(key, TF_SYM_NONE), TF_OK;
    if (S->state == TF_STATE_ESCAPE)
        tfK_keysym(key, TF_SYM_ESCAPE), tfK_flushmod(S, key);
    else if (S->state == TF_STATE_UTF8)
        S->buf_len = 0, S->pending_mod = 0, tfK_codepoint(key, 0xFFFD);
    else if (S->state == TF_STATE_SS3)
        tfD_flushkey(S, key, 'O');
    else if (S->state >= TF_STATE_CS_DCS && S->state <= TF_STATE_CS_APC) {
        assert(S->n == 0);
        if (S->cs_len) S->p = S->cs_buf, S->n = S->cs_len;
        tfD_flushkey(S, key, "P]_"[S->state - TF_STATE_CS_DCS]);
        return S->state = TF_STATE_IDLE, TF_OK; /* cs_buf re-parsed via p/n */
    } else { /* CSI/X10: buf tail re-parsed, S->p/S->n untouched */
        tfD_flushkey(S, key, '[');
        memmove(S->buf + TF_MAX_BUFLEN - S->buf_len, S->buf,
                (size_t)S->buf_len);
        S->replay = S->buf_len, S->buf_len = 0;
        return S->state = TF_STATE_IDLE, TF_OK;
    }
    return S->node = S->root, S->state = TF_STATE_IDLE, TF_OK;
}

TF_API const char *tf_string(const tf_State *S, int *plen) {
    if (!S) return NULL;
    if (S->cs_buf) return (void)(plen && (*plen = S->cs_len)), S->cs_buf;
    return (void)(plen && (*plen = 0)), NULL;
}

TF_API int tf_mouse(const tf_Key *key, int *ev, int *btn, int *line, int *col) {
    int code, pure;
    if (!key || !ev || !btn || !line || !col) return TF_ERRPARAM;
    if (key->type != TF_TYPE_MOUSE) return TF_ERRPARAM;
    *line = key->d.mouse.line, *col = key->d.mouse.col;
    code = key->d.mouse.btn;
    if (key->d.mouse.release) return *btn = 0, *ev = TF_EVENT_RELEASE, TF_OK;
    pure = code & ~0x20;
    *ev = (code & 0x20) ? TF_EVENT_DRAG : TF_EVENT_PRESS;
    if (pure >= 0 && pure <= 2)
        *btn = pure + 1;
    else if (pure == 3)
        *btn = 0, *ev = TF_EVENT_RELEASE;
    else if (pure >= 64 && pure <= 67)
        *btn = pure - 60;
    else if (pure == 128 || pure == 129)
        *btn = pure - 120;
    else
        *btn = 0, *ev = TF_EVENT_UNKNOWN;
    return TF_OK;
}

TF_API int tf_position(const tf_Key *key, int *line, int *col) {
    if (!key || !line || !col) return TF_ERRPARAM;
    if (key->type != TF_TYPE_POSITION) return TF_ERRPARAM;
    *line = key->d.pos.line, *col = key->d.pos.col;
    return TF_OK;
}

TF_API int tf_modereport(const tf_Key *key, int *init, int *mode, int *val) {
    if (!key || !init || !mode || !val) return TF_ERRPARAM;
    if (key->type != TF_TYPE_MODEREPORT) return TF_ERRPARAM;
    *init = key->d.modereport.initial;
    *mode = key->d.modereport.mode;
    *val = key->d.modereport.value;
    return TF_OK;
}

TF_API int tf_csi(const tf_State *S, int args[], int nargs, int *cmd) {
    const char *f = NULL;
    int         len, count = 0;
    if (!S || !args || !cmd) return TF_ERRPARAM;
    if (S->buf_len <= 0 || S->buf[S->buf_len - 1] < 0x40) return TF_ERRPARAM;
    *cmd = tfC_cmd(S, S->buf[S->buf_len - 1]);
    while (count < nargs && tfC_nextarg(S, &f, &len))
        args[count++] = tfC_fieldval(f, len, -1);
    return count;
}

/* sym name table (indexed by tf_Sym, TF_SYM_NONE = NULL) */

static const char *tfK_symnames[] = {
        NULL, /* TF_SYM_NONE */
#define TFX(name, str) str,
        TF_SYMS(TFX)
#undef TFX
};

TF_API const char *tf_name(int sym) {
    if (sym <= 0 || sym >= TF_SYM_COUNT) return NULL;
    return tfK_symnames[sym];
}

static int tfK_streqi(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        int ca = tfB(*a), cb = tfB(*b);
        if (ca >= 'A' && ca <= 'Z') ca += 0x20;
        if (cb >= 'A' && cb <= 'Z') cb += 0x20;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

TF_API int tf_sym(const char *name) {
    int i;
    if (!name || !*name) return -1;
    for (i = 1; i < TF_SYM_COUNT; ++i) {
        if (tfK_streqi(name, tfK_symnames[i])) return i;
    }
    return -1;
}

/* tf_format helpers */

#define TF_FMT_SZ  256
#define TF_NAME_SZ 64 /* name/modifier buffer size */

static int tfK_writes(char *buf, int len, const char *s) {
    int n;
    if (len <= 0) return 0;
    n = (int)strlen(s);
    if (n >= len) n = len - 1;
    memcpy(buf, s, n), buf[n] = '\0';
    return n;
}

/* clang-format off */
static int tfK_writecp(char *buf, int len, int cp)
{ char tmp[TF_UTF8SZ]; tfU_encode(tmp, cp); return tfK_writes(buf, len, tmp); }

static void tfK_functionname(char *out, const tf_Key *key)
{ snprintf(out, TF_NAME_SZ, "F%d", key->d.number); }
/* clang-format on */

static int tfK_isdigit(char c) { return c >= '0' && c <= '9'; }
static int tfK_isupper(char c) { return c >= 'A' && c <= 'Z'; }
static int tfK_tolower(char c) { return c >= 'A' && c <= 'Z' ? c + 0x20 : c; }
static int tfK_islower(char c) { return c >= 'a' && c <= 'z'; }

typedef struct tf_ModToken {
    int         bit;   /* TF_MOD_xxx */
    int         lflag; /* long-name fmt flag */
    int         oflag; /* overriding fmt flag */
    const char *tok;   /* default token */
    const char *ltok;  /* long-name token */
    const char *otok;  /* overriding token */
} tf_ModToken;

static const tf_ModToken tfK_modtok[] = {
        {TF_MOD_SUPER, 0, 0, "D-", NULL, NULL},
        {TF_MOD_META, 0, 0, "T-", NULL, NULL},
        {TF_MOD_SHIFT, TF_FMT_LONGMOD, 0, "S-", "Shift-", NULL},
        {TF_MOD_ALT, TF_FMT_LONGMOD, TF_FMT_ALTISMETA, "A-", "Alt-", "M-"},
        {TF_MOD_CTRL, TF_FMT_LONGMOD, TF_FMT_CARETCTRL, "C-", "Control-", "^"},
};

static int tfK_writemods(char *buf, int len, int mods, int fmt) {
    const tf_ModToken *m;
    const char        *tok;
    char               tmp[TF_NAME_SZ], lower[16];
    int                i, pos = 0;
    tmp[0] = '\0';
    for (m = tfK_modtok; m < tfK_modtok + tf_tablesize(tfK_modtok); ++m) {
        if (!(mods & m->bit)) continue;
        tok = m->tok;
        if (m->lflag && (fmt & m->lflag)) tok = m->ltok;
        if (m->oflag && (fmt & m->oflag)) tok = m->otok;
        if (fmt & TF_FMT_LOWERMOD) {
            for (i = 0; tok[i]; ++i) lower[i] = (char)tfK_tolower(tok[i]);
            lower[i] = '\0', tok = lower;
        }
        pos += tfK_writes(tmp + pos, TF_NAME_SZ - pos, tok);
    }
    return tfK_writes(buf, len, tmp);
}

static void tfK_lowerspace(char *out, const char *nm) {
    int i, j;
    for (i = j = 0; nm[i]; ++i) {
        char cc = nm[i], pc = i ? nm[i - 1] : 0;
        if (!tfK_isupper(cc))
            out[j++] = cc;
        else {
            if (tfK_isdigit(pc) || tfK_islower(pc)) out[j++] = ' ';
            out[j++] = (char)(cc + 0x20);
        }
    }
    out[j] = '\0';
}

static int tfK_keysymname(char *out, const tf_Key *key, int fmt) {
    const char *nm = tf_name(key->d.sym);
    if (!nm) return (out[0] = '\0'), 0;
    if (!(fmt & TF_FMT_LOWERSPACE))
        tfK_writes(out, TF_NAME_SZ, nm);
    else {
        char lowerbuf[TF_NAME_SZ];
        tfK_lowerspace(lowerbuf, nm);
        tfK_writes(out, TF_NAME_SZ, lowerbuf);
    }
    return 1;
}

static void tfK_unicodename(char *out, const tf_Key *key, int fmt) {
    int cp = key->d.codepoint, caret = (fmt & TF_FMT_CARETCTRL);
    if (caret && (key->modifiers & TF_MOD_CTRL) && cp >= 'a' && cp <= 'z')
        cp -= 0x20;
    tfK_writecp(out, TF_NAME_SZ, cp);
}

static void tfK_keyname(char *out, const tf_Key *key, int fmt) {
    switch (key->type) {
    case TF_TYPE_KEYSYM: tfK_keysymname(out, key, fmt); break;
    case TF_TYPE_FUNCTION: tfK_functionname(out, key); break;
    case TF_TYPE_UNICODE: tfK_unicodename(out, key, fmt); break;
    case TF_TYPE_MOUSE: tfK_writes(out, TF_NAME_SZ, "Mouse"); break;
    case TF_TYPE_POSITION: tfK_writes(out, TF_NAME_SZ, "Position"); break;
    case TF_TYPE_MODEREPORT: tfK_writes(out, TF_NAME_SZ, "ModeReport"); break;
    case TF_TYPE_KITTYREPORT: tfK_writes(out, TF_NAME_SZ, "KittyReport"); break;
    case TF_TYPE_DCS: tfK_writes(out, TF_NAME_SZ, "DCS"); break;
    case TF_TYPE_OSC: tfK_writes(out, TF_NAME_SZ, "OSC"); break;
    case TF_TYPE_APC: tfK_writes(out, TF_NAME_SZ, "APC"); break;
    case TF_TYPE_UNKNOWN_CSI: tfK_writes(out, TF_NAME_SZ, "Unknown CSI"); break;
    default: out[0] = '\0';
    }
}

static int tfK_assemble(char *buf, int len, int fmt, const char *parts[2]) {
    const char *mods = parts[0];
    const char *name = parts[1];
    char        tmp[TF_FMT_SZ];
    int         tpos = 0, ml = (int)strlen(mods);
    if (!(fmt & TF_FMT_WRAPBRACKET)) {
        if (ml > 0) {
            memcpy(tmp, mods, ml), memcpy(tmp + ml, name, strlen(name) + 1);
            if (fmt & TF_FMT_SPACEMOD) tmp[ml - 1] = ' ';
            return tfK_writes(buf, len, tmp);
        }
        return tfK_writes(buf, len, name);
    }
    tmp[tpos++] = '<';
    if (ml > 0) {
        memcpy(tmp + tpos, mods, ml), tpos += ml;
        if (fmt & TF_FMT_SPACEMOD) tmp[tpos - 1] = ' ';
    }
    tpos += tfK_writes(tmp + tpos, TF_FMT_SZ - tpos, name);
    tmp[tpos++] = '>';
    return tmp[tpos] = '\0', tfK_writes(buf, len, tmp);
}

TF_API int tf_format(char *buf, int len, const tf_Key *key, int fmt) {
    char        namebuf[TF_NAME_SZ], modbuf[TF_NAME_SZ];
    const char *parts[2];
    if (!key || !buf || len <= 0) return TF_ERRPARAM;
    buf[0] = '\0';
    /* default format: Vim-compatible (WRAPBRACKET | ALTISMETA) */
    if (fmt == 0) fmt = TF_FMT_WRAPBRACKET | TF_FMT_ALTISMETA;
    tfK_keyname(namebuf, key, fmt);
    tfK_writemods(modbuf, sizeof(modbuf), key->modifiers, fmt);
    /* bare key: no brackets */
    if ((key->type == TF_TYPE_UNICODE && key->modifiers == 0)
        || (key->type == TF_TYPE_FUNCTION && key->modifiers == 0
            && !(fmt & TF_FMT_WRAPBRACKET)))
        return tfK_writes(buf, len, namebuf);
    parts[0] = modbuf, parts[1] = namebuf;
    return tfK_assemble(buf, len, fmt, parts);
}

/* tf_parse */

/* modifier name table: single letters first (same prefix tried first) */

/* clang-format off */
typedef struct tf_ModEntry { const char *name; int len; int mod; } tf_ModEntry;

static const tf_ModEntry tfK_modtable[] = {
#define ENTRY(name, mod) {(name), (int)(sizeof(name) - 1), (mod)}
        ENTRY("C",     TF_MOD_CTRL),  ENTRY("Control", TF_MOD_CTRL),
        ENTRY("S",     TF_MOD_SHIFT), ENTRY("Shift",   TF_MOD_SHIFT),
        ENTRY("Super", TF_MOD_SUPER), ENTRY("A",       TF_MOD_ALT),
        ENTRY("Alt",   TF_MOD_ALT),   ENTRY("M",       TF_MOD_ALT),
        ENTRY("Meta",  TF_MOD_ALT),   ENTRY("D",       TF_MOD_SUPER),
        ENTRY("T",     TF_MOD_META),
#undef  ENTRY
};
/* clang-format on */

static int tfK_modget(const char **ps) {
    const char *s = *ps;
    int         i;
    for (i = 0; i < tf_tablesize(tfK_modtable); ++i) {
        const char *name = tfK_modtable[i].name;
        int         len = tfK_modtable[i].len;
        if (strncmp(s, name, len) == 0 && (s[len] == '-' || s[len] == ' ')) {
            for (s += len; *s == '-' || *s == ' '; ++s) continue;
            return (*ps = s), tfK_modtable[i].mod;
        }
    }
    return 0;
}

static int tfK_parsemods(const char **ps) {
    const char *s = *ps;
    int         m, mods = 0;
    while (*s == '-' || *s == ' ') ++s;
    while ((m = tfK_modget(&s))) mods |= m;
    return (*ps = s), mods;
}

/* Vim-style names aliases; libtermkey names kept for backward compat */

/* clang-format off */
typedef struct tf_KeyAlias { const char *name; int sym; } tf_KeyAlias;

static const tf_KeyAlias tfK_alias[] = {
#define ENTRY(name, sym) {name, TF_SYM_##sym}
        ENTRY("Esc",       ESCAPE),    ENTRY("CR",       ENTER),
        ENTRY("BS",        BACKSPACE), ENTRY("Del",      DELETE),
        ENTRY("k0",        KP0),       ENTRY("k1",       KP1),
        ENTRY("k2",        KP2),       ENTRY("k3",       KP3),
        ENTRY("k4",        KP4),       ENTRY("k5",       KP5),
        ENTRY("k6",        KP6),       ENTRY("k7",       KP7),
        ENTRY("k8",        KP8),       ENTRY("k9",       KP9),
        ENTRY("kPoint",    KPPERIOD),  ENTRY("kDivide",  KPDIV),
        ENTRY("kMultiply", KPMULT),    ENTRY("kMinus",   KPMINUS),
        ENTRY("kPlus",     KPPLUS),    ENTRY("kEnter",   KPENTER),
        ENTRY("kEqual",    KPEQUALS),  ENTRY("kComma",   KPCOMMA),
        ENTRY("KP0",       KP0),       ENTRY("KP1",      KP1),
        ENTRY("KP2",       KP2),       ENTRY("KP3",      KP3),
        ENTRY("KP4",       KP4),       ENTRY("KP5",      KP5),
        ENTRY("KP6",       KP6),       ENTRY("KP7",      KP7),
        ENTRY("KP8",       KP8),       ENTRY("KP9",      KP9),
        ENTRY("KPEnter",   KPENTER),   ENTRY("KPPlus",   KPPLUS),
        ENTRY("KPMinus",   KPMINUS),   ENTRY("KPMult",   KPMULT),
        ENTRY("KPDiv",     KPDIV),     ENTRY("KPComma",  KPCOMMA),
        ENTRY("KPPeriod",  KPPERIOD),  ENTRY("KPEquals", KPEQUALS),
#undef  ENTRY
};
/* clang-format on */

static int tfK_parsename(const char *name, int klen, tf_Key *key) {
    char nosp[TF_NAME_SZ];
    int  si, sym, fn, n, i, ni = 0;
    for (si = 0; si < klen; si++)
        if (name[si] != ' ') nosp[ni++] = name[si];
    nosp[ni] = '\0';
    if ((sym = tf_sym(nosp)) >= 0) return tfK_keysym(key, sym), 1;
    for (i = 0; i < tf_tablesize(tfK_alias); ++i)
        if (tfK_streqi(nosp, tfK_alias[i].name))
            return tfK_keysym(key, tfK_alias[i].sym), 1;
    if (nosp[0] == 'F' || nosp[0] == 'f') {
        for (fn = 0, n = 1; nosp[n] && tfK_isdigit(nosp[n]); ++n)
            fn = fn * 10 + (nosp[n] - '0');
        if (fn >= 1 && fn <= 63 && nosp[n] == '\0')
            return tfK_function(key, fn), 1;
    }
    if (klen == 1 && name[0] != '<' && name[0] != '>')
        return tfK_codepoint(key, tfB(name[0])), 1;
    return 0;
}

static int tfK_parseplain(const char *str, tf_Key *key) {
    int ulen = str[0] ? tfU_utf8len(tfB(str[0])) : 0;
    if (ulen <= 0) return -1;
    tfK_utf8(key, str, ulen);
    if (key->d.codepoint == 0xFFFD && ulen > 1) return -1;
    return ulen;
}

static int tfK_parsecaret(const char **ps, int mods, tf_Key *key) {
    const char *s = *ps;
    if (tfK_isupper(*++s)) {
        tfK_codepoint(key, tfB(*s));
        key->modifiers = mods | TF_MOD_CTRL;
        if (*++s == '>') return (*ps = s), 0;
    }
    return -1;
}

TF_API int tf_parse(const char *str, tf_Key *key) {
    const char *s;

    int  r, mods, len;
    char namebuf[TF_NAME_SZ];
    if (!str || !key) return -1;
    key->event = TF_EVENT_PRESS, key->modifiers = 0;
    if (str[0] != '<') return tfK_parseplain(str, key);
    s = str + 1; /* '<' form: modifiers, optional ^X, then key name */
    mods = tfK_parsemods(&s);
    if (*s == '^') {
        if ((r = tfK_parsecaret(&s, mods, key)) < 0) return r;
        return (int)(s - str + 1);
    }
    for (len = 0; s[len] && s[len] != '>'; ++len) namebuf[len] = s[len];
    namebuf[len] = '\0';
    if (len == 0) return -1; /* empty key name (e.g. <S>) */
    if (!tfK_parsename(namebuf, len, key)) return -1;
    key->modifiers = mods;
    if (s += len, *s != '>') return -1;
    return (int)(s - str + 1);
}

/* waitkey (POSIX) */

static const char *tfZ_waitread(void *ud, size_t *plen) {
    tf_WaitCtx *w = (tf_WaitCtx *)ud;
    *plen = w->len;
    w->len = 0; /* drained chunks are re-called only: safe to hand out once */
    return w->buf;
}

#ifndef _WIN32

static ssize_t tfZ_readfd(tf_WaitCtx *x, int fd) {
    ssize_t n;
    do n = read(fd, x->buf, TF_WAIT_BUFSIZE);
    while (n < 0 && (errno == EINTR || errno == EAGAIN));
    return n;
}

TF_API int tf_waitkey(tf_State *S, int fd, int timeout_ms, tf_Key *key) {
    struct pollfd pfd;
    tf_WaitCtx   *x;
    ssize_t       nread;
    int           r;
    if (!S || !key || fd < 0) return TF_ERRPARAM;
    x = &S->wait, pfd.fd = fd, pfd.events = POLLIN;
    for (;;) {
        int pr;
        if ((r = tf_readkey(S, key)) != TF_NONE && r != TF_AGAIN) return r;
        pr = poll(&pfd, 1, timeout_ms < 0 ? -1 : timeout_ms);
        /* timeout: r encodes partial (AGAIN → flush) vs clean (NONE) */
        if (!pr) return r == TF_AGAIN ? tf_flush(S, key) : TF_AGAIN;
        if (pr < 0 && errno != EINTR) return TF_ERRPARAM;
        if (pr < 0) continue; /* EINTR: poll again */
        if (!x->buf) {
            x->buf = (char *)S->allocf(S->alloc_ud, NULL, 0, TF_WAIT_BUFSIZE);
            if (!x->buf) return TF_ERRMEM;
        }
        if ((nread = tfZ_readfd(x, fd)) == 0) return TF_NONE; /* EOF */
        if (nread < 0) return TF_ERRPARAM;
        x->len = (size_t)nread, tf_feed(S, tfZ_waitread, x);
    }
}

#else /* _WIN32 */
TF_API int tf_waitkey(tf_State *S, int fd, int timeout_ms, tf_Key *key) {
    (void)S, (void)fd, (void)timeout_ms, (void)key;
    return TF_ERRPARAM;
}

#endif /* !_WIN32 */

TF_NS_END

#endif /* TF_IMPLEMENTATION */
