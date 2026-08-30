# textmatch Lua 绑定设计

> 对应实现：`lua/textmatch.c`、`lua/textmatch.d.lua`。
> 目标：在保留 Lua 兼容 1-based 函数式 API 的同时，提供面向 editor 的
> 0-based 有状态对象 API，并把所有功能（`TM_LITERAL`、`TM_LINEANCHOR`、
> `tm_find` 的 `endoff` 范围限制）暴露给 Lua。

## 一、定位

**负责**
- 把 `textmatch.h` 暴露为 Lua 模块 `textmatch`。
- 提供两套 API：
  - Lua 兼容层：`tm.find` / `tm.match` / `tm.gmatch`，索引 1-based；
  - 对象层：`tm.new(source)` 返回 `textmatch.State`，索引 0-based，全功能。
- 接受 Lua string 与 `piecetab.Buffer` 作为文本源。
- 状态化对象为 editor 搜索提供范围限制、lineanchor、区间迭代能力。

**不负责**
- 编辑/替换/gsub。
- 直接接受 `piecetab.Doc`；调用方先 `doc:buffer()`。
- 在绑定层实现业务逻辑或缓存匹配结果；只做 C API 的忠实映射。

## 二、Lua 兼容层（1-based）

```lua
tm.find(src, pattern [, init [, plain]])
tm.match(src, pattern [, init])
tm.gmatch(src, pattern)
```

- 索引语义与 Lua 标准库一致：`init` 支持正数、0、负数；返回的
  `start` 是 1-based，`finish` 是 textmatch 的 0-based exclusive end，
  恰好等于 Lua 的 1-based inclusive end。
- `find` 成功返回 `start, finish, captures...`；`match` 返回整个匹配
  或 captures；`gmatch` 返回 4 个值：迭代器函数、`nil`、`nil`、
  closeable 迭代器 State（第 4 个值可用于 `<close>` 保证清理）。
- `find` / `match` 出错时返回 `nil, errmsg`，不再抛错，方便调用方去掉
  `pcall`。
- `gmatch` 迭代过程中出错仍然抛出；调用方自行 `pcall` 迭代器调用。
- `gmatch` 对前导 `^` 按 Lua 语义处理为字面量（绑定层在 pattern 前补
  `%` 转义），不是 anchor。

## 三、对象层（0-based）

```lua
local st = tm.new(source)
st:reset(source)
st:delete()

st:option("plain"|"lineanchor"[, value])

st:find(pattern, off[, endoff])
st:match(pattern, off)
st:gfind(pattern, off[, endoff])
st:capture([n])
```

- `tm.new(source)` 在构造时绑定 source。
- `st:reset(source)` 换绑 source，并清空 offset、captures、pattern
  引用与 option flags。
- `st:delete()` 释放 retained source/pattern 引用并使 State 失效；
  之后 `find` / `match` / `gfind` 都会抛 `invalid State`；
  只有 `reset(source)` 可复活该 State。
- 没有 `pattern()` / `source()` / `offset()` 方法；pattern 与 off 每次
  调用直接传入。
- `find` / `gfind` 的 `off` 是必填 0-based 起始偏移，`endoff` 是可选
  0-based exclusive 终点；`nil` 或负数表示不限。
- `match` 在指定 `off` 精确匹配（不做向前查找）。

索引与返回语义：

- `pos` / `endpos` 都是 0-based；区间是 `[pos, endpos)`。
- `find` / `match` 成功返回三个值：`pos, endpos, capturecount`。
  调用方用 `capturecount` 决定接下来调几次 `capture(i)`。
- `capture(n)` 使用 0-based 捕获索引；position capture 返回
  `pos, pos`。
- 对象层不返回字符串；需要文本时由调用方用 `readat` 自行读取。

错误处理：

- `find` / `match` 出错返回 `nil, errmsg`。
- `gfind` 迭代出错抛出；调用方 `pcall`。
- 已删除 State 的 `find` / `match` / `gfind` 抛 `invalid State`。

## 四、内部设计

### `ltm_State`

```c
typedef struct ltm_State {
    tm_State S;
    size_t   len;
    size_t   endoff;
    int      srcref;
    int      patref;
    int      deleted;
    void (*freesrc)(struct ltm_State *);
    union {
        ltm_StringSrc str;
        ltm_PieceSrc  piece;
    } u;
} ltm_State;
```

- `S` 是 textmatch 的匹配状态；pattern 存在 `ltm_State.pat`，由 `patref`
  保活。
- `len` 是当前 source 的字节长度。
- `endoff` 是迭代器使用的查找终点；`TM_NOLIMIT` 表示不限。
- `srcref` 用 `luaL_ref` 锚定 source；`patref` 保活 pattern 字符串。
- State 有效性由 `srcref != LUA_NOREF` 判断；`delete()` 会释放 source /
  pattern 并 `ltm_reset`，之后必须 `reset()` 才能复活。
- string source 直接保存 `s` 指针；Buffer source 保存 retained `pt_Buffer b`
  和 `pt_Cursor C`。

### source 绑定

- `ltm_attach`：先经 `ltm_src` 获取并 retain 新 source，成功后才释放旧
  source（`pt_release` / `luaL_unref`），避免非法参数 longjmp 破坏旧状态；
  随后调用 `tm_reset` 清空匹配状态与 flags，并把 `endoff` 重置为
  `TM_NOLIMIT`。它不碰 pattern。
- `ltm_src` 对 `retain == 0` 不 retain Buffer；string / buffer 切换时清空
  另一侧字段，避免悬垂指针。
- `gfind` 初始化内部 State 时先 `rawgeti(src->srcref)` 再 `ltm_attach`，
  让迭代器自己 retain 一份。若原 Buffer userdata 已被 `delete()`，`gfind`
  会报 `invalid Buffer`，这是可接受行为。

### pattern 绑定

- 单次 `find` / `match` 直接用 `luaL_checklstring` 返回的指针构造
  `tm_Slice` 传给 `tm_find` / `tm_match`；调用期间 Lua 栈保证指针有效。
- `gfind` / `gmatch` 用 `ltm_attachpattern` 把 pattern 放入 registry 并保存
  `patref`，同时把稳定指针存入 `it->pat`；迭代器直接用 `it->pat` 调用
  `tm_find`。

### 迭代器

- `gfind` 复制当前 state 的 source 与 flags 到新 userdata，不复制 pattern；
  调用方传入的 pattern 单独绑定到迭代器。
- `gfind` / `gmatch` 返回 4 个值：迭代器函数、`nil`、`nil`、closeable
  迭代器 State。第 4 个值可用于 `<close>` 保证迭代器提前退出时清理。
- 每次迭代用 `it->pat` 调用 `tm_find(&S, pat, endoff)`，成功 yield 结果后
  按现有空匹配推进规则移动 offset。

## 五、测试

- `lua/tests/textmatch_test.lua` 覆盖：
  - 兼容层 string / Buffer 的 find / match / gmatch；
  - 对象层构造绑定、option getter/setter、reset 清 flags、find range、
    match、capture、gfind；
  - delete 后 `find` / `match` / `gfind` 抛 `invalid State`、
    reset 复活；
  - 错误返回 `nil, errmsg` 与 gfind 抛错；
  - 非法 rebind 保持旧状态、`nil` 可选参数、`endoff` 不持久化；
  - Buffer 生命周期、delete 后 gfind。
- 运行：`just lua/tm`、`just lua/tm-cov`。
- C 侧 `tests/textmatch_test.c` 保持 `textmatch.h` 100% 行/函数/分支覆盖。

## 六、已知限制

- `tm_find` 对 `S->off >= endoff` 直接返回 `TM_OK`，因此 `^` 在空输入或
  范围终点不匹配；这是已接受行为，作为低优先级特性记录在 `TODO.md`。
- 匹配可能越过 `endoff`（suffix verification 无界），这是 textmatch.h
  当前接受的语义。
