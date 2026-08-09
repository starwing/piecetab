# Investigation: liolib.c (lua-5.5.0)

Source: `/Users/sw/Work/Sources/lua-5.5.0/src/liolib.c` (841 lines total)

---

## 1. `g_read` — Core read dispatcher (lines 568–616)

```c
static int g_read (lua_State *L, FILE *f, int first) {
  int nargs = lua_gettop(L) - 1;
  int n, success;
  clearerr(f);
  errno = 0;
  if (nargs == 0) {  /* no arguments? */
    success = read_line(L, f, 1);
    n = first + 1;  /* to return 1 result */
  }
  else {
    /* ensure stack space for all results and for auxlib's buffer */
    luaL_checkstack(L, nargs+LUA_MINSTACK, "too many arguments");
    success = 1;
    for (n = first; nargs-- && success; n++) {
      if (lua_type(L, n) == LUA_TNUMBER) {
        size_t l = (size_t)luaL_checkinteger(L, n);
        success = (l == 0) ? test_eof(L, f) : read_chars(L, f, l);
      }
      else {
        const char *p = luaL_checkstring(L, n);
        if (*p == '*') p++;  /* skip optional '*' (for compatibility) */
        switch (*p) {
          case 'n':  /* number */
            success = read_number(L, f);
            break;
          case 'l':  /* line */
            success = read_line(L, f, 1);
            break;
          case 'L':  /* line with end-of-line */
            success = read_line(L, f, 0);
            break;
          case 'a':  /* file */
            read_all(L, f);  /* read entire file */
            success = 1; /* always success */
            break;
          default:
            return luaL_argerror(L, n, "invalid format");
        }
      }
    }
  }
  if (ferror(f))
    return luaL_fileresult(L, 0, NULL);
  if (!success) {
    lua_pop(L, 1);  /* remove last result */
    luaL_pushfail(L);  /* push nil instead */
  }
  return n - first;
}
```

**Key observations:**
- `first` parameter distinguishes `io.read()` (first=1) from `f:read()` (first=2) — the file handle is at stack index 1 for method calls, so results start at index 2.
- When no arguments → reads one line (stripped of newline, `chop=1`).
- Numeric argument N → reads N bytes (or `test_eof` if N=0).
- String arguments: `"*n"` number, `"*l"` line (no newline), `"*L"` line (with newline), `"*a"` entire file.
- Multiple arguments read sequentially; stops at first failure.
- On failure, last result is replaced with `fail` (nil).
- On `ferror`, returns `nil, errmsg, errcode`.

---

## 2. `io_read` — `io.read(...)` (line 619–621)

```c
static int io_read (lua_State *L) {
  return g_read(L, getiofile(L, IO_INPUT), 1);
}
```

**Key observations:**
- Gets the default input file from the registry key `"_IO_input"`.
- Passes `first=1` (Lua stack index where arguments start; file is not on Lua stack).

---

## 3. `f_read` — `file:read(...)` (lines 624–626)

```c
static int f_read (lua_State *L) {
  return g_read(L, tofile(L), 2);
}
```

**Key observations:**
- Uses the file handle from the Lua stack (self).
- Passes `first=2` because the file handle (self) occupies stack index 1.

---

## 4. `read_line` — Line reading (lines 521–539)

```c
static int read_line (lua_State *L, FILE *f, int chop) {
  luaL_Buffer b;
  int c;
  luaL_buffinit(L, &b);
  do {  /* may need to read several chunks to get whole line */
    char *buff = luaL_prepbuffer(&b);  /* preallocate buffer space */
    unsigned i = 0;
    l_lockfile(f);  /* no memory errors can happen inside the lock */
    while (i < LUAL_BUFFERSIZE && (c = l_getc(f)) != EOF && c != '\n')
      buff[i++] = cast_char(c);  /* read up to end of line or buffer limit */
    l_unlockfile(f);
    luaL_addsize(&b, i);
  } while (c != EOF && c != '\n');  /* repeat until end of line */
  if (!chop && c == '\n')  /* want a newline and have one? */
    luaL_addchar(&b, '\n');  /* add ending newline to result */
  luaL_pushresult(&b);  /* close buffer */
  /* return ok if read something (either a newline or something else) */
  return (c == '\n' || lua_rawlen(L, -1) > 0);
}
```

**Key observations:**
- `chop=1` → newline is stripped; `chop=0` → newline is preserved (for `"*L"` format).
- Uses `l_getc` (which is `getc_unlocked` on POSIX, `getc` otherwise) inside `flockfile`/`funlockfile`.
- Reads in chunks of `LUAL_BUFFERSIZE` via `luaL_prepbuffer`/`luaL_addsize` pattern.
- Returns 1 (success) if a newline was hit OR if any characters were read (handles the case of a final line without newline).
- Returns 0 if EOF was hit immediately with no characters read (and no newline).

---

## 5. `read_chars` — Read N bytes (lines 555–565)

```c
static int read_chars (lua_State *L, FILE *f, size_t n) {
  size_t nr;  /* number of chars actually read */
  char *p;
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  p = luaL_prepbuffsize(&b, n);  /* prepare buffer to read whole block */
  nr = fread(p, sizeof(char), n, f);  /* try to read 'n' chars */
  luaL_addsize(&b, nr);
  luaL_pushresult(&b);  /* close buffer */
  return (nr > 0);  /* true iff read something */
}
```

**Key observations:**
- Uses `luaL_prepbuffsize` to preallocate a buffer exactly `n` bytes.
- `fread` reads up to `n` bytes; short read means EOF was hit.
- Returns 1 if at least 1 byte was read, 0 otherwise.

---

## 6. `read_number` — Read a number (lines 479–510)

```c
static int read_number (lua_State *L, FILE *f) {
  RN rn;
  int count = 0;
  int hex = 0;
  char decp[2];
  rn.f = f; rn.n = 0;
  decp[0] = lua_getlocaledecpoint();  /* get decimal point from locale */
  decp[1] = '.';  /* always accept a dot */
  l_lockfile(rn.f);
  do { rn.c = l_getc(rn.f); } while (isspace(rn.c));  /* skip spaces */
  test2(&rn, "-+");  /* optional sign */
  if (test2(&rn, "00")) {
    if (test2(&rn, "xX")) hex = 1;  /* numeral is hexadecimal */
    else count = 1;  /* count initial '0' as a valid digit */
  }
  count += readdigits(&rn, hex);  /* integral part */
  if (test2(&rn, decp))  /* decimal point? */
    count += readdigits(&rn, hex);  /* fractional part */
  if (count > 0 && test2(&rn, (hex ? "pP" : "eE"))) {  /* exponent mark? */
    test2(&rn, "-+");  /* exponent sign */
    readdigits(&rn, 0);  /* exponent digits */
  }
  ungetc(rn.c, rn.f);  /* unread look-ahead char */
  l_unlockfile(rn.f);
  rn.buff[rn.n] = '\0';  /* finish string */
  if (l_likely(lua_stringtonumber(L, rn.buff)))
    return 1;  /* ok, it is a valid number */
  else {  /* invalid format */
   lua_pushnil(L);  /* "result" to be removed */
   return 0;  /* read fails */
  }
}
```

**Key observations:**
- Uses `RN` auxiliary struct (lines 429–434): holds `f`, current char `c`, buffer count `n`, and `buff[L_MAXLENNUM+1]`.
- `nextc` (lines 440–450): saves current char to buffer, reads next. Fails (returns 0) if buffer would overflow (>200 chars).
- `test2` (lines 456–460): accepts current char if in 2-char set.
- `readdigits` (lines 466–471): reads decimal or hex digits.
- Handles: sign, `0x`/`0X` prefix, decimal point (locale-aware with `.` always accepted), exponent `e`/`E` or `p`/`P` for hex.
- Pushes number or nil on Lua stack.

---

## 7. `test_eof` — EOF test (lines 513–518)

```c
static int test_eof (lua_State *L, FILE *f) {
  int c = getc(f);
  ungetc(c, f);  /* no-op when c == EOF */
  lua_pushliteral(L, "");
  return (c != EOF);
}
```

**Key observations:**
- Peeks one character, pushes empty string, returns true if NOT at EOF.

---

## 8. `read_all` — Read entire file (lines 542–552)

```c
static void read_all (lua_State *L, FILE *f) {
  size_t nr;
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  do {  /* read file in chunks of LUAL_BUFFERSIZE bytes */
    char *p = luaL_prepbuffer(&b);
    nr = fread(p, sizeof(char), LUAL_BUFFERSIZE, f);
    luaL_addsize(&b, nr);
  } while (nr == LUAL_BUFFERSIZE);
  luaL_pushresult(&b);  /* close buffer */
}
```

---

## 9. `io_lines` — `io.lines(...)` (lines 388–412)

```c
/*
** Return an iteration function for 'io.lines'. If file has to be
** closed, also returns the file itself as a second result (to be
** closed as the state at the exit of a generic for).
*/
static int io_lines (lua_State *L) {
  int toclose;
  if (lua_isnone(L, 1)) lua_pushnil(L);  /* at least one argument */
  if (lua_isnil(L, 1)) {  /* no file name? */
    lua_getfield(L, LUA_REGISTRYINDEX, IO_INPUT);  /* get default input */
    lua_replace(L, 1);  /* put it at index 1 */
    tofile(L);  /* check that it's a valid file handle */
    toclose = 0;  /* do not close it after iteration */
  }
  else {  /* open a new file */
    const char *filename = luaL_checkstring(L, 1);
    opencheck(L, filename, "r");
    lua_replace(L, 1);  /* put file at index 1 */
    toclose = 1;  /* close it after iteration */
  }
  aux_lines(L, toclose);  /* push iteration function */
  if (toclose) {
    lua_pushnil(L);  /* state */
    lua_pushnil(L);  /* control */
    lua_pushvalue(L, 1);  /* file is the to-be-closed variable (4th result) */
    return 4;
  }
  else
    return 1;
}
```

**Key observations:**
- When called with no arguments (`io.lines()`) → uses default input file, no auto-close.
- When called with a filename (`io.lines("file.txt")`) → opens file, auto-close on finish.
- Returns an iteration function (via `aux_lines`). When `toclose`, also returns the file as the to-be-closed variable (4th result of generic for), ensuring automatic close on loop exit.

---

## 10. `f_lines` — `file:lines(...)` (lines 376–380)

```c
static int f_lines (lua_State *L) {
  tofile(L);  /* check that it's a valid file handle */
  aux_lines(L, 0);
  return 1;
}
```

**Key observations:**
- Validates the file handle is open, then delegates to `aux_lines(L, 0)` (never auto-closes, caller owns the handle).

---

## 11. `aux_lines` — Build lines iterator closure (lines 365–373)

```c
static void aux_lines (lua_State *L, int toclose) {
  int n = lua_gettop(L) - 1;  /* number of arguments to read */
  luaL_argcheck(L, n <= MAXARGLINE, MAXARGLINE + 2, "too many arguments");
  lua_pushvalue(L, 1);  /* file */
  lua_pushinteger(L, n);  /* number of arguments to read */
  lua_pushboolean(L, toclose);  /* close/not close file when finished */
  lua_rotate(L, 2, 3);  /* move the three values to their positions */
  lua_pushcclosure(L, io_readline, 3 + n);
}
```

**Key observations:**
- Pushes a C closure with `io_readline` as the function.
- Upvalues of the closure:
  1. **File handle** (userdata) — the file being read.
  2. **Number of arguments** (integer) — how many read-format args follow.
  3. **toclose** (boolean) — whether to close the file on EOF.
  4. 3+n ... **Format arguments** — the read format strings/numbers passed to `g_read`.
- `MAXARGLINE = 250` (line 354).

---

## 12. `io_readline` — Lines iterator function (lines 632–658)

```c
static int io_readline (lua_State *L) {
  LStream *p = (LStream *)lua_touserdata(L, lua_upvalueindex(1));
  int i;
  int n = (int)lua_tointeger(L, lua_upvalueindex(2));
  if (isclosed(p))  /* file is already closed? */
    return luaL_error(L, "file is already closed");
  lua_settop(L , 1);
  luaL_checkstack(L, n, "too many arguments");
  for (i = 1; i <= n; i++)  /* push arguments to 'g_read' */
    lua_pushvalue(L, lua_upvalueindex(3 + i));
  n = g_read(L, p->f, 2);  /* 'n' is number of results */
  lua_assert(n > 0);  /* should return at least a nil */
  if (lua_toboolean(L, -n))  /* read at least one value? */
    return n;  /* return them */
  else {  /* first result is false: EOF or error */
    if (n > 1) {  /* is there error information? */
      /* 2nd result is error message */
      return luaL_error(L, "%s", lua_tostring(L, -n + 1));
    }
    if (lua_toboolean(L, lua_upvalueindex(3))) {  /* generator created file? */
      lua_settop(L, 0);  /* clear stack */
      lua_pushvalue(L, lua_upvalueindex(1));  /* push file at index 1 */
      aux_close(L);  /* close it */
    }
    return 0;
  }
}
```

**Key observations:**
- Upvalue 1: file handle (LStream userdata).
- Upvalue 2: number of read-format arguments.
- Upvalue 3: toclose flag (boolean).
- Upvalues 4..3+n: read-format arguments (strings like `"*l"`, `"*n"`, or numbers).
- Each iteration: pushes all format arguments, calls `g_read(L, p->f, 2)` (first=2 because the closure pushes `self` at stack index 1 for `g_read`).
- `g_read` returns `n` results (at least 1 — either a value or `fail`).
- If the first result is truthy (success) → returns all results.
- If the first result is falsey (EOF/failure):
  - If `n > 1`, there's an error message → raises Lua error.
  - If `toclose` is true → closes the file (calls `aux_close`).
  - Returns 0 (loop terminates).

---

## 13. Iterator state management

**Line number tracking:** There is **no explicit line number tracking** in liolib.c. The iterator simply reads from the current file position. The `io_readline` function:
- Reads one line per iteration via `g_read` → `read_line`.
- `read_line` increments the file position naturally via `l_getc`.
- No line counter is maintained.

**File position tracking:**
- Relies entirely on the `FILE*` internal file position indicator.
- `read_line` advances it character by character.
- `read_chars` advances it via `fread`.
- `read_number` advances it via `l_getc`.
- `f_seek` (line 702) allows explicit seeking: `"set"`, `"cur"`, `"end"`.

**To close vs not to close:**
- `io.lines("file")` → `toclose=1`: file is closed when EOF is reached (in `io_readline` at line 651–655).
- `io.lines()` (no args) → `toclose=0`: uses default input file, never closed.
- `file:lines()` → `toclose=0`: caller owns the handle.
- When `toclose=1`, `io_lines` returns 4 values (generic for with to-be-closed variable). When `toclose=0`, returns 1 value (just the iterator function).

---

## Summary: Call flow

```
io.read(...)           → io_read → g_read(L, getiofile(L, IO_INPUT), 1)
file:read(...)         → f_read  → g_read(L, tofile(L), 2)
io.lines(...)          → io_lines → aux_lines(L, toclose) → io_readline closure
file:lines(...)        → f_lines  → aux_lines(L, 0)       → io_readline closure
io_readline iteration  → g_read(L, p->f, 2) → read_line / read_chars / read_number / read_all
```

**Argument dispatch in `g_read` (no args → `read_line(L, f, 1)`; number → `read_chars`; string → switch on `*n/*l/*L/*a`).**
