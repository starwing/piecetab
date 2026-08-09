# Investigation: `lines()` and `read()` in piecetab.c

Date: 2026-07-21

---

## 1. `lpt_readstring` (read-into-Lua-string helper)

**File:** `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/piecetab.c`
**Lines:** 157-168

```c
/* read len bytes from C into a new Lua string (clamped by pt_read) */
static int lpt_readstring(lua_State *L, pt_Cursor *C, size_t len) {
    luaL_Buffer B;
    size_t      chunk, got;
    luaL_buffinit(L, &B);
    for (; len > 0; len -= got) {
        chunk = len < LUAL_BUFFERSIZE ? len : LUAL_BUFFERSIZE;
        got = pt_read(C, luaL_prepbuffer(&B), chunk);
        luaL_addsize(&B, got);
        if (got < chunk) break;
    }
    return luaL_pushresult(&B), 1;
}
```

Shared helper used by `Ldoc_readbytes`, `Ldoc_readline`, `Ldoc_read` (all-format), `Ldoc_lineiter`, `Lbuf_read`, and `Lpt_from`.

---

## 2. `Ldoc_read` and its sub-helpers

### `Ldoc_readbytes` (lines 570-576)
```c
static int Ldoc_readbytes(lua_State *L, lpt_Doc *d) {
    size_t len = (size_t)luaL_checkinteger(L, 2);
    if (len == 0) return lua_pushliteral(L, ""), 1;
    if (pt_offset(&d->C) >= pt_bytes(pt_buffer(&d->C)))
        return lua_pushnil(L), 1;
    return lpt_readstring(L, &d->C, len);
}
```
- len=0 → empty string (no advance)
- at/after EOF → nil
- otherwise read `len` bytes via `lpt_readstring`

### `Ldoc_readline` (lines 578-596)
```c
static int Ldoc_readline(lua_State *L, lpt_Doc *d, int wantnl) {
    pt_Cursor  *C = &d->C;
    luaL_Buffer B;
    luaL_buffinit(L, &B);
    size_t      i, len, n = 0;
    char       *buf;
    const char *src = pt_piece(C, &len);
    if (src == NULL) return 0;
    for (buf = luaL_prepbuffer(&B); src; src = pt_next(C, &len)) {
        for (i = 0; i < len && src[i] != '\n'; i++) buf[n++] = src[i];
        if (i < len) {
            if (wantnl) buf[n++] = '\n';
            pt_advance(C, (pt_Delta)(i + 1));
            return luaL_addsize(&B, n), luaL_pushresult(&B), 1;
        }
        luaL_addsize(&B, n), n = 0;
    }
    return luaL_pushresult(&B), 1;
}
```
- Iterates pieces looking for `\n`
- `wantnl=0` (format `"l"`) → exclude newline
- `wantnl=1` (format `"L"`) → include newline
- No `\n` found → return remaining text (no nil)

### `Ldoc_read` (lines 598-616)
```c
static int Ldoc_read(lua_State *L) {
    lpt_Doc *d = lpt_checkdoc(L, 1);
    int      argt = lua_type(L, 2);
    if (argt == LUA_TNUMBER) return Ldoc_readbytes(L, d);
    if (argt == LUA_TSTRING) {
        const char *fmt = lua_tostring(L, 2);
        if (fmt[0] == '*') fmt++;
        if (fmt[0] == 'a') {
            size_t total = pt_bytes(pt_buffer(&d->C));
            size_t off = pt_offset(&d->C);
            if (off >= total) return lua_pushliteral(L, ""), 1;
            return lpt_readstring(L, &d->C, total - off);
        }
        if (fmt[0] == 'l' || fmt[0] == 'L')
            return Ldoc_readline(L, d, fmt[0] == 'L');
        return luaL_error(L, "piecetab: bad read format '%s'", fmt);
    }
    return luaL_error(L, "piecetab: bad read argument #2");
}
```
Dispatch:
- Number arg (N) → `Ldoc_readbytes(L, d)` — read N bytes
- String `"a"` or `"*a"` → read all remaining
- String `"l"` or `"*l"` → read line (no newline)
- String `"L"` → read line (with newline)
- Anything else → error

---

## 3. `lpt_docsync` (lines 453-468)

```c
static int lpt_docsync(lpt_Doc *d, size_t tol, size_t tob) {
    pt_Buffer   b = pt_buffer(&d->C);
    ut_Vid      cur = ut_current(d->ut);
    lpt_ScanCtx ctx;
    int         r;
    if ((r = ut_diff(d->ut, d->lcvid, cur)) < 0) return r;
    if ((r = lpt_hunkapply(d->lc, ut_hunks(d->ut, NULL), r, b)) < 0) return r;
    d->lcvid = cur, d->lck = 0;
    if ((r = ut_freshdiff(d->ut, d->lck, ut_freshcount(d->ut))) < 0) return r;
    if ((r = lpt_hunkapply(d->lc, ut_hunks(d->ut, NULL), r, b)) < 0) return r;
    d->lck = ut_freshcount(d->ut);
    ctx.cur_line = lc_breaks(d->lc);
    ctx.max_line = tol == LPT_UNL ? tol : tol + 1, ctx.end = tob;
    pt_seek(&ctx.C, b, lc_bytes(d->lc));
    return lc_scan(d->lc, lpt_scanline, &ctx);
}
```

Syncs `lc` (linecache) to match current `pt_Buffer` state:
1. **`ut_diff`** — compute hunks from `d->lcvid` → current undo-tree vid
2. **`lpt_hunkapply`** — apply hunks to update linecache line table content
3. Reset `d->lck` (fresh-count anchor) to 0
4. **`ut_freshdiff`** — compute hunks for uncommitted (fresh) edits
5. **`lpt_hunkapply`** — apply fresh-edit hunks too
6. **`lc_scan`** with `lpt_scanline` callback — re-scan from where linecache left off (at `lc_bytes(d->lc)`) up to `tol` lines / `tob` bytes boundary

Noted in `notes/design_luabind.md` (line 385) as exceeding 25/30 line limit.

---

## 4. `Ldoc_lines` + `Ldoc_lineiter` (lines 722-748)

### `Ldoc_lineiter` (lines 722-739)
```c
static int Ldoc_lineiter(lua_State *L) {
    lpt_Doc   *d = (lpt_Doc *)lua_touserdata(L, lua_upvalueindex(3));
    size_t    *pn = (size_t *)lua_touserdata(L, lua_upvalueindex(2));
    lc_Cursor *lcC = (lc_Cursor *)lua_touserdata(L, lua_upvalueindex(1));
    size_t     max = lc_breaks(d->lc), n = *pn;
    pt_Cursor  C;
    if (n > max) return 0;
    lua_pushinteger(L, (lua_Integer)n);
    if (n < max) {
        lc_seekline(lcC, d->lc, n);
        pt_seek(&C, pt_buffer(&d->C), lc_lineoffset(lcC));
        lpt_readstring(L, &C, lc_linelen(lcC) - 1u);
    } else {
        pt_seek(&C, pt_buffer(&d->C), lc_bytes(d->lc));
        lpt_readstring(L, &C, pt_bytes(pt_buffer(&d->C)) - lc_bytes(d->lc));
    }
    return *pn = n + 1, 2;
}
```

Closure-based iterator (3 upvalues):
1. `lc_Cursor` (upvalue index 1) — reused across calls for seek
2. `size_t *pn` (upvalue index 2) — current line number (state)
3. `lpt_Doc *d` (upvalue index 3) — the doc

For each call:
- If `n > max` → return 0 (stop)
- Push line number, push line text
- Lines 0..max-1: read via `lc_linelen - 1` (strip `\n` from linecache-stored length)
- Line `max` (trailing line): read from `lc_bytes` to end of buffer
- Advance `*pn` and return 2 values

### `Ldoc_lines` (lines 741-748)
```c
static int Ldoc_lines(lua_State *L) {
    lpt_Doc   *d = lpt_checkdoc(L, 1);
    lc_Cursor *lcC = (lc_Cursor *)lua_newuserdata(L, sizeof(lc_Cursor));
    size_t    *pn = (size_t *)lua_newuserdata(L, sizeof(size_t));
    lpt_checkerror(L, lpt_docsync(d, LPT_UNL, LPT_UNL));
    memset(lcC, 0, sizeof(lc_Cursor)), *pn = 0, lua_pushvalue(L, 1);
    return lua_pushcclosure(L, Ldoc_lineiter, 3), 1;
}
```

- Calls `lpt_docsync(d, LPT_UNL, LPT_UNL)` to bring linecache fully up to date
- Creates 3 upvalues: zeroed `lc_Cursor`, zeroed counter, doc reference
- Returns the closure

---

## 5. Registration in `doc_methods` table (lines 846-861)

```c
static const luaL_Reg doc_methods[] = {
        {"__gc", Ldoc_gc},   {"__close", Ldoc_gc},
        {"__len", Ldoc_len}, {"append", Ldoc_write},
#define ENTRY(name) {#name, Ldoc_##name}
        ENTRY(seek),         ENTRY(read),
        ENTRY(write),        ENTRY(insert),
        ENTRY(edit),         ENTRY(splice),
        ENTRY(remove),       ENTRY(offset),
        ENTRY(column),       ENTRY(line),
        ENTRY(linelen),      ENTRY(breaks),
        ENTRY(lines),        ENTRY(commit),
        ENTRY(undo),         ENTRY(redo),
        ENTRY(earlier),      ENTRY(later),
        ENTRY(buffer),       ENTRY(dump),
#undef ENTRY
        {NULL, NULL}};
```

Both `ENTRY(read)` → `Ldoc_read` and `ENTRY(lines)` → `Ldoc_lines` registered via the `ENTRY` X-macro, which expands `ENTRY(read)` to `{"read", Ldoc_read}` and `ENTRY(lines)` to `{"lines", Ldoc_lines}`.

Registered at line 862-865 via `luaL_newmetatable` + `luaL_setfuncs`.

---

## 6. Test cases

**File:** `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/tests/lua/test_pt.lua`

### `lines()` tests

| Test function (line) | Description |
|---|---|
| `testLines` (691) | `"a\nb\nc"` → 3 lines: `{0,"a"}`, `{1,"b"}`, `{2,"c"}` |
| `testLinesEmpty` (703) | `""` → 1 iteration (count = 1) |
| `testLinesTrailingNL` (710) | `"x\n"` → 2 lines: `"x"`, `""` |

### `read()` tests on Doc

| Test function (line) | What it tests |
|---|---|
| `testNewEmpty` (254) | `read("a")` on empty doc → `""` |
| `testNewFromString` (260) | `read("a")` on `"hello\nworld"` → full content |
| `testNewFromBuffer` (266) | `read("a")` on buffer-constructed doc |
| `testReadN` (354) | `read(5)` → `"hello"`, `read(6)` → `" world"` |
| `testReadNEof` (360) | `read(1)` at end → `nil` |
| `testReadNZero` (366) | `read(0)` → `""` (both at 0 and end) |
| `testReadLine` (373) | `read("l")` → `"hello"`, `"world"`, `nil` |
| `testReadLineWithNL` (380) | `read("L")` → `"hello\n"`, `"world\n"`, `nil` |
| `testReadLineStar` (387) | `read("*l")` → `"hello"` |
| `testReadAll` (392) | `read("a")` at start, mid, end |
| `testReadBadArg` (817) | `read({})` → error "bad read argument" |
| `testReadBadFormat` (824) | `read("x")` → error "bad read format" |
| `testSeekLineToBreaks` (831) | combo read after seek line |
| `testLineCountEmpty` (686) | `breaks()` = 1 on `""` (related to lines) |

### `read()` tests on Buffer

| Test function (line) | Description |
|---|---|
| `testReadNegOffset` (853) | `b:read(-1)` → error "offset must be non-negative" |

### `read()` tests on Cursor

| Test function (line) | Description |
|---|---|
| `testReadNegLen` (772) | `c:read(-1)` → error "length must be non-negative" |

---

## Summary

- **Core read helper**: `lpt_readstring` (line 157) — piece-aware string extraction
- **Doc read dispatch**: `Ldoc_read` (line 598) → numeric (bytes), `"a"` (all), `"l"/"L"` (line), error otherwise
- **Lines iterator**: `Ldoc_lines` (line 741) creates closure; `Ldoc_lineiter` (line 722) yields `(lnum, text)` for each line + trailing line
- **Linecache sync**: `lpt_docsync` (line 453) called by `Ldoc_lines`; also called by `Ldoc_seek`, `Ldoc_line`, `Ldoc_linelen`, `Ldoc_breaks`
- **Registration**: both via `ENTRY` macro in `doc_methods[]` (lines 850, 856)
- **Tests**: 3 lines tests, 11 read tests, multiple combined tests using `read("a")` for verification
