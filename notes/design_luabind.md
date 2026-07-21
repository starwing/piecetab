# linecache + piecetab Lua 绑定设计

> TODO 优先级 #1 的设计定案。产出物：**绑定惯例**（供 spantree 第一天沿用）
> + 接口摩擦清单。原则：接口设计以 Lua 暴露形态为准绳。
> 本文档为定案版（v2）：doc 内核按"位点模型 + 惰性追赶"全部重新设计，
> 旧实现（changeset 4 元组 + nsync 追赶）判定为不可修复，doc 区代码全部重写。

## 〇、绑定开发的最高铁律

**绑定的首要目的是寻找 C API 的摩擦点，而非在绑定层开发功能。**

1. 绑定的职责是**忠实暴露 C API 语义**，不做魔术、不替用户做决定
2. 如果绑定需要大量代码才能完成某个操作，**这是 C API 的缺口**，
   应当在 C API 层补齐，而不是在绑定层用胶水代码填坑
3. 遇到摩擦时：**诚实描述当前状态和需要的 C API 能力**，而不是在
   绑定层自行实现业务逻辑
4. 除非用户明确要求，否则不在绑定层开发功能——给 C API 层解决问题
   的机会

### 0.1 胶水层的定义

**绑定层 = 胶水层。唯一的职责是组合各库 API 暴露到 Lua，在组合过程中
发现并报告 API 摩擦。** 绑定层不实现功能、不做防御性检查、不替用户
做决定——这些是各库 API 自己的责任。

- 如果 API 自身对零参数已是 no-op，绑定层不再包 `if (del > 0)` 守卫
- 如果 API 有构造器（如 pt_seek），绑定层用构造器；有导航器（如
  pt_locate），用导航器——不混用
- 如果 API 之间组合需要大量代码，说明 API 有缺口，应报告而非在绑定层
  用胶水填坑

### 0.2 绑定代码铁律：充分利用回调/遍历 API 的已有参数

**不要自己追踪框架已经提供的信息。** 典型案例：

- `lc_scan`/`lc_append` 的回调 `unsigned scanner(void *ud, size_t pos)` —
  `pos` 参数**就是当前已加载的字节偏移**。不要自己在上下文中维护
  `consumed`/`limit` 字段并在回调中比较——`pos` 已经是消费进度，
  `pos + plen` 就是对侧偏移。
- pt_Cursor 在 `pt_piece`/`pt_next`/`pt_advance` 下顺序前进时，
  `pt_offset(C)` **就是当前到位位置**。不需要计算 `off + consumed`。

**检查清单**：
1. 回调函数中：是否有自定义进度变量，而回调签名已经给了等效参数？
2. 遍历中：是否有手动计算位置，而 `pt_offset(C)` 已经给出了？
3. 初始化后不再 `pt_locate` — 光标已经在上次操作的正确位置，直接用
   `pt_piece`/`pt_advance` 顺序推进

## 一、定位

- 单模块 `require "piecetab"`，三对象：`buffer`（不可变）、`cursor`
  （编辑会话）、`doc`（可变文档）。
- 分期：先 buffer + cursor（忠实映射 C，摩擦清单主产地），
  后 doc（组合层，第一个"Lua 形态倒逼"验证品）。
- linecache **不单独暴露**——lc 作为 doc 内部件，scanner 回调等
  C 形态不过 Lua 边界。
- **项目主目标：检验 pt_Buffer COW 的可用性**（结论见 §八）。

## 二、兼容基座

- 目标：**Lua 5.1–5.5 + LuaJIT**，`LUA_VERSION_NUM` 条件编译，
  垫层宏集中 `piecetab.c` 文件头（单文件自足，见 §九）。
- 编译：C89 但**不加 -pedantic**（Lua 头 5.3+ 用 long long；Lua
  自身亦不用 pedantic）。justfile `lua-build`/`lua-test` 以
  PUC 5.5 + LuaJIT 两端点覆盖兼容区间。测试框架 luaunit
  （vendored 于 `tests/lua/luaunit.lua`）。
- 5.4+ 启用 to-be-closed（`__close`），低版本静默降级为仅 `__gc`。
- 迭代器返回按 generic-for 四值协议（iterator, state, control,
  closing），5.1–5.3 忽略第四值，天然向下兼容。
- `read` 格式串同收 `"l"` 与 `"*l"`（io 库 5.1/5.2+ 两代写法）。

## 三、通用绑定惯例（spantree 沿用之法度）

| 议题         | 惯例                                                                                                                                                                                        | 理由                                                                                                                       |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| 索引基准     | **全 0-based**（偏移与行号），显示 +1 归 UI 边界                                                                                                                                            | 偏移是间隙语义（io.seek 先例）；对齐 extmark/LSP/treesitter 方言；零 ±1 转换层。nvim 混合基准是 vimscript 遗产伤疤，不重演 |
| 参数校验     | `luaL_argcheck` / `luaL_check*` 家族                                                                                                                                                        | 标准 "bad argument #n" 格式                                                                                                |
| 状态错误     | `luaL_error` 抛（detached cursor 再用、OOM、失效 vid）                                                                                                                                      | C 层事务性保证抛后对象仍合法可重试；nvim API 同风格                                                                        |
| 越界移动     | 沿 C 层 clamp 语义，不报错                                                                                                                                                                  | 与 lc_advance/pt_advance 一致                                                                                              |
| 字符串所有权 | **动词定路径**：edit 走 hole（pt_edit 内部 memcpy，Lua string 不过 toliteral）；insert/append/splice 走 literal（`pt_reserve`+memcpy+`pt_literal` 全拷入树自有 arena）。Lua string 用完即弃 | literal 引用语义与 GC string 不可调和（from 链共享使锚定归属不可判）；arena 本为此设                                       |
| 内存         | `lua_Alloc` 直接接 `pt_Alloc`/`lc_Alloc`（同为 Lua 风格 realloc 签名）                                                                                                                      | 内存统一归 Lua 记账，`collectgarbage"count"` 可见                                                                          |
| State        | 模块级单例 `pt_State`/`lc_State`（luaopen 时建），池 freelist 跨对象复用                                                                                                                    | 多 Lua state 各自独立互不干扰                                                                                              |
| 生命周期     | retain/release 完全藏进 `__gc`；`__close` 提供确定性释放                                                                                                                                    | 用户不见引用计数                                                                                                           |
| 库习惯对齐   | 能对齐 Lua 标准库惯用法处尽量对齐，但**编辑器语义优先于 io 兼容**（如 read("l") 含换行，见 §七）                                                                                            | 降低学习成本；我们是做编辑器的                                                                                             |

## 四、buffer + cursor（忠实层）

```lua
local pt = require "piecetab"
local b = pt.from("hello\nworld")     -- pt.empty() 亦可
#b                                    -- 字节数（__len）
b:read(off, len) → string             -- 越界截断
b:pieces()                            -- 只读遍历: for off, len, s in b:pieces()
b:compact() → buffer                  -- 紧凑新 blob（compact 类接口归 buffer 层）

local c = b:cursor(off)               -- 编辑会话
c:offset() → off
c:locate(off) / c:advance(d)          -- 命名从 C
c:read(len) → string
c:edit(del, s)                        -- hole 路径（打字动词）；len ≤
                                      --   pt.MAX_HOLESIZE，超长 argcheck
c:insert(s) / c:append(s)             -- literal 路径任意长
c:splice(del, s)                      -- literal 路径删+插
c:remove(len)
c:commit() → buffer                   -- detach；再用 → error（忠实 C 语义）
c:rollback() → buffer
```

- **光标落点忠实 pt 文档**（docs/piecetab.md §3.7/3.8）：
  - `insert`：光标**前**插，光标**不动**
  - `append`：光标**处**插，光标**推进到插入尾**（非"追尾插"——
    旧文档此处表述有误，以 pt 文档为准）
  - `edit`/`splice`：删 del 后插入，光标**推进到插入尾**
  - `remove`：光标停删除点
- **动词按路径分，不按长度分流**：edit=hole、insert/append/splice=
  literal（arena 拷贝）。绑定层不做智能分流魔法。`pt.MAX_HOLESIZE`
  暴露为常量。
- cursor `__close`/`__gc`：未 commit 即弃 → 自动 rollback，
  transient 树不泄漏；5.4+ 可 `local c <close> = b:cursor(0)`。
- cursor 持有 buffer 引用（uservalue）防 GC。

## 五、doc（组合层）API 定案

doc = pt_Cursor（长期）+ undo-tree（内含 lc_Cache）。
**编辑动词直通 pt_ 底层语义，不替用户做决定**。doc 相对忠实层的
唯一附加职责：编辑记 journal 喂 undo-tree，保证行查询正确。

```lua
local d = pt.doc(src)                 -- src: buffer | string | nil
-- 定位（io file 兼容 + 行扩展）
d:seek() → pos                        -- 查询当前位置
d:seek("set", off) → pos
d:seek("cur", delta) → pos
d:seek("end" [, -n]) → pos
d:seek("line", lnum) → pos            -- 行首定位；lnum ∈ [0, breaks]
                                      --   （=breaks 落残行首）
d:offset() → off
-- 读（io file 形态；位置推进；EOF 返 nil）
d:read() → s | nil                     -- 无参 = read("l")，读一行不含 \n
d:read(N) → s | nil                   -- N 字节，尾部截断；不触发 lc
d:read("l") → s | nil                   -- 当前行内容（**不含** \n），io 兼容
d:read("L") → s | nil                   -- 完整行（**含** \n），编辑器语义；"*L" 同义；
                                       --   手扫不触发 lc（见 §七偏离注记）
d:read("a") → s                       -- 当前位置至文档尾
-- 行查询（触发 lc 追赶，target 见 §6.4）
d:line() → lnum                       -- 当前位置行号
d:column() → col                      -- 当前位置列（行内字节偏移）
d:linelen([lnum]) → n                 -- 行长含 \n；残段/虚拟尾行 =
                                       --   pt_bytes - lc_bytes
d:breaks() → n                        -- 断数直通 lc_breaks；行数恒 = breaks+1
d:lines([fmt, ...])                   -- 迭代器，反复调 read(fmt, ...) 直到 nil；
                                       --   无参 = lines() 逐行不含 \n（io.lines 语义）
d:piece("len"|"next"|"prev") → n       -- 当前/下一/前一 piece 长度（调试用）；
                                       --   "next"/"prev" 会移动光标
#d                                    -- 总字节
-- 编辑（直通 pt 语义；光标落点同 §四；全部记 journal，不触发 lc）
d:edit(del, s)                        -- pt_edit；hole 拷贝；超长 argcheck
d:insert(s)                           -- pt_insert；光标不动
d:append(s)                           -- pt_append；光标推进
d:write(s)                            -- append 纯别名（io 用户顺手，
                                      --   同一 C 函数双名注册）
d:splice(del, s)                      -- pt_splice；光标推进
d:remove(n)                           -- pt_remove；光标停删除点
-- 历史（接口形态先定；内核结构待讨论，见 §十一）
d:commit() → vid                      -- 冻结入 undo-tree，单调递增版本
d:undo([vid]) → vid                   -- 【丢弃 fresh 草稿】；无参回父版本；
                                      --   带 vid 回祖先（校验策略待 §十一）
d:redo() → vid                        -- 如果有 fresh 草稿，无操作；
                                      --   否则快进当前链的下一版本
                                      --   分支选择策略待 §十一
d:buffer([vid]) → buffer              -- 快照导出（diff/异步保存/喂 pt.doc()）
d:dump() → string
```

- **fresh 草稿世界观**：commit 才入史；undo/redo 是版本跳转，
  跳转前未 commit 编辑一律**丢弃**（数据库事务直觉：rollback 丢弃
  未提交）。丢弃 = `pt_release(pt_rollback(&d->C))` + journal 清 +
  lc 位点处置：逆放已消费前缀（§11.2）。
- `goto` 是 5.2+ 保留字——统一入 `undo(vid)`（vim `:undo {N}` 先例）。
- vid 单调递增不复用（无 ABA）；53bit double 精度足够（5.1）。
- v1 不做修剪（maxdepth 无上限），整条链靠 GC 自然回收。

## 六、doc 内核：位点模型与惰性追赶

> 定案范围：编辑 journal、单版本内追赶（§6.2–6.4）、错误路径
> 基本盘（§6.6）。undo-tree 内核定案见 §十一。

### 6.1 世界观

- pt 层 undo = **版本切换**：COW 快照链免费，零拷贝零树编辑。
- lc 是**快取**，无 COW 之理（cache **不可 delcache+newcache**；
  数据可从 payload 重 scan 覆盖；COW 化已否决）。lc 只跟随活动
  查询需求，靠 undotree diff + journal 追赶增量更新。
- **编辑零负担**：编辑路径只 journal（24B/条 O(1) append），
  不碰 lc、不触发追赶。lc 允许任意落后，追赶时逐条 apply 的坐标
  基准逐条自然成立（无需 lc 与 pt 同步的前置条件——此为位点模型
  相对旧设计"编辑必先修复行表"的关键差异）。

### 6.2 数据结构（定案，2026-07 勘误）

> **勘误**：原设计将 lc_Cache 归于 lpt_UndoTree（§11.2），但 undotree.h
> 升格为独立库后不含 lc。lc 归 doc 直接持有，doc 借助 undotree 的 diff
> 服务增量更新 lc。

```
lpt_Doc (userdata)
├── pt_Cursor C      (fresh 期 dirty；文本唯一入口)
├── ut_Tree  *ut     (undotree 版本树)
├── lc_Cache *lc     (linecache，doc 持有；不可 delcache+newcache 重建)
├── ut_Vid    lcvid  (lc 对齐的 commit 版本节点，NULL=空)
└── int       lck    (journal 条目已消费进 lc 的前缀条数)
```

**doc = cursor 装文本 + undotree 管历史 + linecache 查行**。
journal 由 undotree 管理（`ut_Tree.journal`），doc 通过
`ut_freshcount(ut)` 获取未消费条目数。

**lc 不可 delcache+newcache 重建**——借助 undotree diff 做增量追赶：
- 版本切换（undo/redo 后 lcvid ≠ current）→ `ut_diff(lcvid, current)`
  → apply hunk 到 lc → lcvid = current
- fresh 追赶（lck < freshcount）→ `ut_freshdiff(lck, freshcount)`
  → apply hunk 到 lc → lck = freshcount
- 编辑后 lc 自然落后，下次行查询自动追赶
- lcvid==NULL（首次）→ lc_scan 装载基线 → lcvid = current

### 6.3 编辑路径

```
Ldoc_edit/insert/append/splice/remove（write=append 别名）:
  1. ut_record(d->ut, off, del, ins)     -- journal 记 undo 信息
     （del=0且ins=0 时 ut_record 内部 no-op，绑定层不预先判断）
  2. pt_xxx(&d->C, ...)                   -- 直通
     失败 → ut_unrecord(d->ut, 1) 回退后抛
  3. lcok = 0（编辑后 lc 过期）
```

编辑不触发 lc 追赶（§6.1）。off 取编辑前 `pt_offset(&d->C)`；
append 的 off 同样取当前光标（pt_append 在光标处插入，非文档尾）。

### 6.4 追赶算法（行查询入口）

> 算法定案；doc 自行管理 lc 同步，借助 undotree diff 服务。

触发集与 target（双限 tbytes/tlines，`(size_t)-1` = 不限）：

| 接口                             | target              |
| -------------------------------- | ------------------- |
| `seek("line", n)`                | tlines = n          |
| `line()` / `column()`            | tbytes = offset + 1 |
| `linelen(n)`                     | tlines = n + 1      |
| `breaks()`                       | 全量                |
| `lines()` / read 族 / seek 字节类 / 编辑 | **不触发**          |

```
lpt_docsync(L, d, tbytes, tlines):
  A. 版本追赶：lcvid != ut_current(ut) →
     ut_diff(ut, lcvid, cur) → hunk apply（读源=ut_payload(cur)）
     → lcvid = cur；仅版本变时 reset lck=0
  B. fresh 追赶：lck < ut_freshcount(ut) →
     ut_freshdiff(ut, lck, ut_freshcount(ut))
     → hunk apply（读源=pt_buffer(&d->C)）→ lck = freshcount
  C. 扩展装载：未达 target 且 lc_bytes < pt_bytes 时，
     lc_scan(lc, scanner, ud) 限量扫描
  D. 兜底：hunkapply 部分失败 → lc_remove 从最早修改点截断
     lc 尾部不一致数据（见 §6.5 恢复方案），下次查询重 diff 追赶

行查询映射（追赶后）：

- `line()`/`column()`：`lc_seek(lcC, d->lc, pt_offset(&d->C))` 定位。
  offset 可能 > lc_bytes（在残段区），此时 lc_seek 软 clamp 并将 col
  设为 excess。lc_line 给出行号，lc_col 给出列（残段区为残段内偏移）。
- `linelen(n)`：n < breaks → `lc_seekline` + `lc_linelen`；n == breaks
  → 残段行长 = `pt_bytes - lc_bytes`（从 lc_seek 到 lc_bytes 处取得
  col=excess，即残段长度）；n > breaks → argcheck。
- `seek("line", n)`：n ≤ breaks → `lc_seekline` + `lc_lineoffset` 返回行首；
  n > breaks → argcheck。
- `lines([fmt, ...])`：io.lines 语义——返回闭包迭代器，每次迭代调
  `lpt_read(d, fmt, ...)` 直到 nil。无参=逐行读不含 \n。不触发 lc，
  从当前光标位置开始读。迭代器持 doc 引用防 GC。
- `piece("len")`：调用 `pt_piece` 返回当前 piece 长度，不移动光标。
  `piece("next")`/`piece("prev")` 调用 `pt_next`/`pt_prev`（会移动光标）。

### 6.5 undo/redo 路径

- **commit**（§5 Ldoc_commit）：
  1. 如有 fresh journal：pt_commit(&d->C) → pt_Buffer b
  2. ut_commit(d->ut, (ut_Payload*)b) → 新节点 n
  3. lpt_setvid(d, pt_version(b), n)
  4. pt_seek(&d->C, b, pt_offset 旧位置)
  5. lcvid = n, lck = 0（版本变了，lc 数据需追赶至新节点）
- **undo**（§5 Ldoc_undo）：
  1. 保存 pos = pt_offset(&d->C)
  2. 如有 fresh journal → ut_freshdiff + 光标坐标映射 → 丢弃
  3. ut_diff(src, dst) → 光标坐标映射 + lc 数据同步
  4. ut_switch(d->ut, dst); pt_seek(&d->C, payload(dst), pos)
  5. lcvid = dst, lck = 0（lc 已同步至目标版本）
- **redo**（§5 Ldoc_redo）：
  1. 如有 fresh journal → 无操作（§5 定案）
  2. ut_diff(cur, target) → 光标坐标映射 + lc 数据同步
  3. ut_switch(d->ut, target); pt_seek(&d->C, payload(target), pos)
  4. lcvid = target, lck = 0

> **光标坐标映射**：`ut_diff` 产生的 hunk 描述了 src→dst 的变换。
> 利用 `ut_Hunk {pa, ca, pdel, cins}` 将 src 侧偏移 pos 映射到
> dst 侧——纯区间代数，binding 层自行实现（~12 行），无需 undotree
> 提供新 API。
>
> **只调一次 ut_diff**：undo/redo 中同时用 hunk 做光标映射和 lc
> 同步，避免下次行查询重 diff。
>
> **lc 同步失败恢复方案**：hunkapply 前保存 `trimpos = hs[0].pa`
> （最早修改点，hunk 按 pa 升序排列）。若 hunkapply 失败：
> 1. `lc_remove` 删除 lc 中 [trimpos, lc_bytes) 区间——此区域
>    的 lc 数据已部分修改/被 splice 移位，不可信；trimpos 之前的
>    数据未受任何 hunk 影响，与原版本一致，保留。
> 2. lcvid 不更新→下次 docsync 重 diff 后完整重新追赶被截区域。
> 3. lc 结构本身不重建，仅截断数据。

### 6.6 错误路径与生命周期

- **pt 编辑 ERRMEM = 事务语义，失败不改 buffer**。绑定层处置：
  `ut_unrecord(d->ut, 1)` 回退 journal 后抛，状态干净。
- **doc __gc**：dirty 先 `pt_release(pt_rollback(&d->C))`；
  `ut_deltree(S->US, d->ut)`；`lc_delcache(S->LS, d->lc)`。
  `__close` 同 `__gc`，幂等（释放后置 NULL 守卫）。
- `pt_empty` 哨兵 release 安全（pt 文档 §3.2/3.3）。

## 七、口径定案 + 摩擦清单

### 7.1 行口径（编辑器语义，全 0-based）

**残段不进树**：scanner 只返回含 \n 完整行；尾部无 \n 字节留在
树外（lc_bytes ≤ pt_bytes），由 trailing 虚拟行机制寻址。
**行数恒 = breaks + 1**（无脑 +1：尾要么有残段行，要么 \n 后有
虚拟空行，要么空文档一个空行）。

| 文档     | breaks | 行数 | read("l") 连续     | lines() 迭代 |
| -------- | ------ | ---- | ------------------- | ------------ |
| `""`     | 0      | 1    | —                   | （无迭代）   |
| `"a"`    | 0      | 1    | `"a"`, nil          | `"a"`        |
| `"a\n"`  | 1      | 2    | `"a"`, nil          | `"a"`        |
| `"a\nb"` | 1      | 2    | `"a"`, `"b"`, nil   | `"a"`, `"b"` |

- **lines() = io.lines 语义**：无参时反复调 `read()`（行不含 \n）直到 nil。
  接受可选格式参数：`d:lines("*L")` 含换行，`d:lines(80)` 按字节。
  迭代从**当前光标位置**开始，会推进光标。
- **read("l") 不含 \n**（io 兼容），**read("L") 含 \n**（编辑器语义）。
  `"l"`/`"*l"` 同义，"L"/"*L" 同义。两代写法兼收。
  **read() 无参 = read("l")**。
- read("l")/read("L") 手扫换行推进位置，**不触发 lc**。
- 换行探测集中于单 helper（现 memchr `'\n'`）。**开放项**：
  Unicode 多换行方式（CR/CRLF/NEL/LS/PS），将来只换探测函数，
  lc 无涉（lc 只认段长）。

### 7.2 暂缓与开放项

1. 跨分支跳转（非祖先非后代）：暂只支持 undo(祖先)/redo(单步)。
2. maxdepth 修剪：v1 无上限；修剪留给上层策略（buffer() 导出 →
   compact() → 重建 doc）。
3. OOM 注入测试（allocf 可注）：注入路径待补。
4. `buffer:sub`/`doc:sub`：1-based/0-based 摩擦，**HOLD**。
5. Unicode 换行探测（见 §7.1）。
6. journal 相邻条目合并优化（连续打字每键一条）：v1 不做，
   正确性优先。

### 7.3 摩擦清单（spantree 设计输入）

| #   | 摩擦                                                                       | 处置                                                                                       |
| --- | -------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| 1   | literal 引用语义 vs Lua GC string                                          | 全拷贝入 arena 惯例（`lpt_toliteral`）                                                     |
| 2   | `pt_edit` 超长返 ERRPARAM                                                | **动词按路径分**；spantree API 宜从设计起动词即路径                                        |
| 3   | 判 detach 依赖内部字段                                                     | 已解决：`pt_valid`/`lc_valid` 宏                                                           |
| 4   | "读 n 停 n"半步问题                                                        | `pt_read` 分块直读；read 类 API 是绑定读端标配                                             |
| 5   | C 资源先于 userdata → error 窗口泄漏                                       | **userdata-first** 惯例（§9.2）                                                            |
| 6   | 栈上裸 cursor 编辑无兜底                                                   | 编辑必经 cursor userdata；构造器内手动闭环是唯一例外                                       |
| 7   | pt 编辑 ERRMEM 为**事务语义**（失败不改 buffer），绑定层 journal 可安全回退 | 正面样本：事务性上游让绑定层错误路径趋零成本                                               |
| 8   | **pt_seek 对 dirty cursor 直接 memset** → transient 泄漏陷阱              | 版本切换前必 `pt_release(pt_rollback())`。改用 pt_locate 导航 dirty cursor                 |
| 9   | lc trailing col 语义恰为残段长度                                           | 正面样本：lc_seek 到尾后 col=excess 即残段长                                               |
| 10  | undotree.h 不含 lc_Cache，doc 需自行管理 lc 位点                          | 绑定层自己维护 lck/lcok，借 ut_freshdiff+ut_hunks 做增量追赶                              |
| 11  | lc_seek 越界软 clamp vs lc_seekline 越界硬 ERR_PARAM                      | 不一致，需注意。seekline 前必须校验 lnum ≤ breaks                                         |
| 12  | ut_switch 在 journal 非空时拒绝（ERR_PARAM）                                | undo 丢弃草稿前必须先 ut_discard                                                           |
| 13  | hunkapply 跨多 hunk 无原子性：lc_append 自身回滚，但部分 hunk 成功、部分失败后 lc 不一致                               | **已解决**：hunkapply 前保存 `hs[0].pa`（最早修改点）；失败时 `lc_remove` 截断 lc 中 [pa, lc_bytes) 范围（保留 pa 前不受影响的正确数据），lcvid 不更新让下次 docsync 重 diff 补回被截区域。不新增 C API。 |

### 7.4 绑定层代码审计（undotree.h 集成后，2026-07 勘误）

以下统计 piecetab.c 中 doc 胶水代码的问题（扣除 buffer/cursor 忠实层 ~300 行）：

| 类别 | 问题 | 处数 | 根因 |
|------|------|------|------|
| 防御性守卫 | `if (del > 0)` 等先判断再调 API | ~8 | API 自身对 0 已是 no-op，绑定层不应越俎代庖 |
| pt_seek 误用 | dirty cursor 上直接 pt_seek | undo路径 | 应先用 pt_locate（导航）或先 rollback 再 seek（构造） |
| 残段长度 | `pt_bytes - lc_bytes` 手工计算 | 4 | lc_seek 到尾后 lc_col 即残段长度，无需手算 |
| scanner 桥 | pt_Cursor→lc_scan 回调适配 | ~37 | lc 消费者共享需求，应下沉到 linecache |

**结论**：当前代码已重写 read/lines 为 io.lines 语义，函数均合规。剩余摩擦点待 C API 层解决。

## 八、C 侧改动预期 + pt COW 可用性结论

- **pt_Buffer COW 可用性**：确认——undotree.h 骑在 COW 之上提供差分服务。
- **lc 缺少 `lc_applyhunks`**（2026-07 审计确认）——hunk apply 需要在绑定层
  自己实现（looping lc_seek + lc_splice + lc_append + scanner）。这是
  已知摩擦，暂不下沉（胶水层的合理实现量）。
- hunkapply OOM 恢复：利用现有 `lc_remove` 截断不一致尾部数据（摩擦 #13），
  不需要新 C API。
- lc COW 化已否决：cache 无 COW 之理。

## 九、绑定代码风格（参照 musubi.c，starwing 惯用法）

> 范本：<https://github.com/starwing/musubi/blob/master/musubi.c>。
> 单绑定文件 `piecetab.c`，`#define LUA_LIB` + `PT_STATIC_API`/
> `LC_STATIC_API` 嵌入单头文件库。

### 9.1 命名体系

| 类别                    | 形式                              | 实码例                                             |
| ----------------------- | --------------------------------- | -------------------------------------------------- |
| Lua C 函数（方法）      | `L<obj>_<name>`                   | `Lbuf_len`, `Lcur_offset`, `Lstate_gc`             |
| 库级/构造函数           | `Lpt_<name>`                      | `Lpt_from`, `Lpt_empty`                            |
| metatable 名宏          | `LPT_<OBJ>_TYPE` = "piecetab.Xxx" | `LPT_BUFFER_TYPE`, `LPT_CURSOR_TYPE`               |
| State key 魔数          | `LPT_STATE_KEY`                   | `((void*)0x91ECE7AB)`                              |
| 内部 helper（小写前缀） | `lpt_<name>`                      | `lpt_checkerror`, `lpt_toliteral`, `lpt_newbuffer` |

**userdata 直接映射 C 指针**，无包装 struct：buffer = `pt_Buffer` 指针胶囊、cursor = `pt_Cursor` 直嵌、doc = `lpt_Doc` 直嵌。校验 helper 直接返 C 指针。

### 9.2 惯用法清单

- **兼容垫层集中文件头**：`#if LUA_VERSION_NUM < 502` 宏块
  （setuservalue/setfenv、luaL_setfuncs、luaL_setmetatable；
  LuaJIT 检测 `LUA_GCISRUNNING` 以 `luaL_newlib` 补丁）；
  带返回值 shim 冠 `lua53_`（`lua53_rawgetp` 式）。
- **userdata 直嵌**：无效态仲裁 `luaL_argcheck`（detached cursor
  用 `pt_valid(C)`，invalid buffer 用 `*b != NULL`，invalid doc
  用 `d->root != NULL`）。
- **`__gc` 幂等**：释放后置 NULL，NULL 守卫防双重释放；buffer/doc
  亦挂 `__close`。
- **uservalue 锚定**：cursor 创建时 uservalue 表 `{buffer}` 防 GC。
  buffer 不锚 State——registry 强引用保证 State 活到 lua_close。
- **资源优先（userdata-first）**：先 `lua_newuserdata` +
  `setmetatable`（自此 `__gc` 兜底）→ 后取 C 资源填入。
  可失败的纯内存分配一律前置于不可回退的 C 资源获取。
- **OOM 守卫宏**：`lpt_checkmem(L, p)` —— `(p) || luaL_error(L,...)`。
- **literal 入库 helper**：`lpt_toliteral(L, i, l, C)` 合并
  checklstring + reserve + memcpy + literal，len==0 返 NULL。
- **read 共享 helper**：`lpt_readstring(L, C, len)` 用 luaL_Buffer
  分块循环 + `pt_read` 零中转。
- **注册**：X-macro + 内联 `static const luaL_Reg` 表；
  `if (luaL_newmetatable(...))` 内 `luaL_setfuncs` + 自 `__index`。
- **State 单例**：registry 存 `lpt_State`，key `LPT_STATE_KEY`；
  `setmetatable` 推迟到成功路径。
- **返 self 链式**：`return lua_settop(L, 1), 1;`；编辑类方法皆返
  self 支持链式。
- **错误集中**：`lpt_checkerror(L, err)` 单点 switch，消息冠
  `"piecetab: "` 前缀。
- **逗号表达式流**：声明 + 赋值 + 条件同 clause 一次成句。

## 十、实现计划

### Phase 1（确定部分，可先行实施）

旧 doc 区代码（lpt_UNode 4 元组 journal、nsync、needline、
Ldoc_* 全体）**整体删除重写**。undo-tree 相关方法
（commit/undo/redo/buffer(vid)）**待 §十一 定案后实施**——
先行版 doc 为单版本文档（无历史），接口面其余部分全量落地；
lpt_UndoTree 以单节点退化骨架先立（架构归属 §十一 已定，
doc 行查询即经其取）。

工序（依赖序）：

1. **结构体与位点**：lpt_Doc = C + ut；lpt_UndoTree 退化骨架
   （lc/journal/lck/lcok，§6.2 机制簿记）；journal push；释放路径。
2. **编辑五动词 + write 别名**：直通 pt + journal（§6.3）；
   ERRMEM 处置（§6.6）。
3. **UndoTree sync（暂名 lpt_docsync）**：A 基线检查（lcok=0 →
   清空限量装载）；B 两遍法（结构 + 脏区重扫）；C 扩展装载
   （双限 scanner，只收完整行——修正旧 scanline 尾段误入树）；
   D 兜底阈值。
4. **行查询四接口 + lines 迭代器**：§6.4 映射；残段行特判；
   迭代器完整行含 \n。
5. **read/seek 族**：read("l"/"L") 含 \n；seek("line") 边界
   [0, breaks]；seek whence 用 luaL_checkoption。
6. **测试**（tests/lua/test_pt.lua 增补）：
   - 口径表 4 例（§7.1 表逐行断言 breaks/行数/lines 内容）
   - read("l") 含 \n、"*l" 兼收、EOF nil
   - 编辑后行查询（journal 追赶正确性：插入含/不含 \n、删除跨行、
     残段区编辑、trailing 定格）
   - write 别名、光标落点断言（insert 不动/append 推进/remove 停点）
7. **docs 回填**：linecache.md markbreak 节补"断点 = offset+len、
   trailing col 定格入树"表述 + brief_linecache.md 同步（单测
   自证所得回填文档惯例）。

验收：`just lua-test` 双运行时全绿；`just test` C 侧不回归。

### Phase 2（待 §十一 undo-tree 定案后）

- commit/undo/redo/buffer(vid) 全套 + 丢弃草稿路径 + 深链
  非递归释放（千级 commit GC 不爆栈测试）。
- 版本切换后 lc 对齐（增量 apply/restore，§11.4）。
- D 兜底阈值 K 调优 + journal 合并优化评估（§7.2.6）。

## 十一、lpt_UndoTree → undotree.h（独立库）

> **2026-07 勘误**：本节原始设计（lpt_UndoTree 内含 lc_Cache + journal）
> 已废弃。undotree.h 升格为独立库后不含 lc。下文 §11.1–11.5 保留
> 历史参考，**以 `notes/brief_undotree.md` + 代码为准**。
> doc 自己持有 lc（§6.2 勘误），借助 undotree.h 的 diff 服务做增量追赶。

### 11.1 架构定位（已定）

- **lpt_UndoTree 内含一个 lc_Cache**——行快取归 UndoTree 所有，
  且**全局唯一**（不随版本复制）。
- **lpt_Doc 包含一个 lpt_UndoTree**；**所有行信息从 UndoTree 取**。
- doc 编辑时向 UndoTree 提供信息支持（journal 三元组，§6.3）。
- **doc = buffer（pt_Cursor 承载文本）+ 从 lpt_UndoTree 来的
  行信息** 的结合。doc 不接触 lc 内部，changeset/追赶细节
  对 doc 不可见。

### 11.2 内核结构（已定）

```
lpt_UndoTree
├── lc_Cache *lc               唯一行快取
├── lc 位点标记：lc 当前贴着谁——fresh 中？某 UNode 上？
│     （node 指针 + fresh 标志 + 消费条数 lck）
├── journal                    fresh changeset，独立数组，在树外
├── UNode *root / *current
└── nextvid

lpt_UNode
├── vid
├── pt_Buffer b                提交后的参考 buffer（retained）
├── parent / children[]
└── changeset                  commit 时从 journal 整体挪入
```

- **fresh 状态 = 编辑后不在树中的游离状态**（悬于 current 之上）；
  fresh changeset（journal）在树外，**commit 时挪入新 UNode**。
- 每次 commit 产生新 UNode：pt_commit 所得参考 buffer + 挪入的
  changeset，挂 current 之下，current 移至新节点。
- **fresh 时 undo()**：丢弃草稿（§11.3），lc 已消费的 journal
  前缀 [0..lck) 通过**逆向 changeset** 恢复（非全量重建）。
- **undo/redo 后行查询**：记录 lc 所在 vid 与当前 vid，找到两
  vid 对应 node → 求树上路径 → 顺路径 apply（向下）/
  restore（向上）changeset 回到现场。
- UNode 可用内存池维护——后续优化，现在不管。

### 11.3 已裁决边界（不再翻案）

- undo 前 fresh 草稿一律**丢弃**（非隐式 commit）；丢弃时
  dirty cursor 必须 `pt_release(pt_rollback(&d->C))` 先清
  transient 再 seek（pt_seek 对 dirty cursor 直接 memset →
  必泄漏，摩擦 8）。redo 遇 fresh 草稿：**无操作**（§五）。
- vid 单调递增不复用；v1 无修剪。
- 接口形态见 §五（undo/redo/buffer/commit 签名已定）。
- lc 归 UndoTree 且唯一（§11.1）；lc 无 COW（§6.1/§八）。
- OOM 处置：hunkapply 每 hunk 的 lc_append 自身事务回滚
  （lcB_rollback），但跨 hunk 无原子性——部分 hunk 成功、部分
  回滚后 lc 处于不一致状态（详见 §7.3 摩擦 #13）。解法：在
  undo/redo 中必须一次 ut_diff 同时做光标映射和 lc 同步，不允许
  lc 处于半同步状态遗漏给下次行查询处理。
- **lc 不可 delcache+newcache 重建**（§6.2）；doc __gc 时 `lc_delcache` 除外。

### 11.4 apply/restore 定案：hunk 双向迁移

changeset 定形为 **hunk 四元组** `(pa, ca, pdel, cins)`，有序、
不相交，挂 child UNode（描述 parent M → child N 的变换）：
"M 的 [pa, pa+pdel) 被替换为 N 的 [ca, ca+cins)"。
**p 字段全是 M 侧量、c 字段全是 N 侧量**；ca ≡ pa +
Σ前缀(cins−pdel)，四元组冗余存 ca 换 O(1)（校验时 assert
前缀和一致）。

**单遍直插消费**（右到左逐 hunk：删旧区 + scanner 带断直插）：

| 方向 | lc 起点态 | 删旧区 | 直插区（scanner 读源） |
|---|---|---|---|
| forward M→N | M | `seek(pa); splice(pdel, 0)` | `[ca, ca+cins)` ← **N.b** |
| backward N→M | N | `seek(ca); splice(cins, 0)` | `[pa, pa+pdel)` ← **M.b** |

（backward 行为语义定义；实现统一走"取逆视图 + forward"，
见下文 compose 段）

```
for i = H-1 .. 0:                      -- 右到左，本侧坐标免调整
  if 本侧起点 ≥ lc_bytes: continue     -- 未加载区整跳
  lc_seek(本侧起点); lc_splice(本侧旧长, 0)   -- 删旧，免 OOM，吞断真实
  pt_seek(&pc, 终点buffer, 对侧起点)          -- 栈上只读 cursor
  lc_append(0, scanner, {pc, 限量 对侧长})    -- 带断直插完整行
  lc_splice(0, e)                             -- e = 限量−consumed，补残段
```

- **无两遍**：hunk 的读源（迁移终点 buffer）恒存在——树上每个
  UNode 都持 retained b——直插不依赖结构放进度；被中间条目删掉
  的插入字节在规范化时已从 hunk 消失，"读中间态"需求不存在。
- **右到左免坐标调整**：处理 hunk i 时左侧未动，本侧坐标恒有效
  （右侧虽已换成对侧内容，不影响左侧偏移）。
- `lc_append` 原生完成"分割点插完整行 + 残段与右侧内容拼行"
  （两遍法的逐断 markbreak 循环整套消失）；批量装行快于逐断。
- **e 处理定案（免预扫）**：`lc_append(0, scanner)` 插完整行
  （append 语义 cursor 落插入尾，正是拼接行处），残段
  e = 限量 − ctx.consumed，就地 `lc_splice(0, e)` 补入——
  文本只读一遍，零 advance。
- OOM：删旧（lc_splice）免 OOM；单 hunk 直插由 lc_append 事务
  回滚（lcB_rollback）。跨 hunk 混合态恢复：lc_remove 截断最早
  修改点之后数据（见 §6.5 恢复方案、§7.3 摩擦 #13）。
- 部分加载：本侧起点 ≥ lc_bytes 的 hunk 整跳；del 跨 lc_bytes
  自动 clamp。trailing 定格兜残段转换。
- 正确性归纳不变式：右到左进行时，每个 hunk 的本侧坐标基准
  逐个自然成立（时序条目版同理：apply 正序/restore 倒序 +
  del/ins 互换，坐标零变换）。
- scanner 与 C 步扩展装载共用同款（挂 pt_Cursor 的限量
  scanline，只返回含 \n 完整行）。

**规范化**（时序 journal → hunk list，唯一新增算法）：
逐条 (off, del, ins) 合入有序 hunk 数组——删除区与既有 hunk
相交则吞并（被吞原文量入 pdel、幸存 cins 残端并入新 cins），
不相交则插槽；内部删除+插入天然连片，**调整无分裂分支**。
hunk 数 = 编辑位置分散度（打字场景恒 1；全文替换收敛为 1 个
巨 hunk——重扫全文是真实需求，零冗余）。O(k·H)，commit 低频。
**必须 brute-force 对拍**：随机编辑序列，直接模拟 vs hunk
重放，文本+断双一致。

**hunk 折叠（compose）与消费路径统一**：

- **取逆视图**：hunk 取逆 = `(pa,ca,pdel,cins) → (ca,pa,cins,pdel)`
  字段互换，零成本。backward ≡ 取逆后 forward（逐项相等）——
  **消费原语唯一化：forward only**。
- **compose**：`A: X→Y` 与 `B: Y→Z` 合成 `X→Z`——A 的 c 侧与
  B 的 p 侧同在 Y 坐标系，双指针按 Y 坐标归并：不相交各自
  映射通过（ca 经 B 前缀 delta 映射到 Z；pa 经 A 逆映射到 X），
  相交合并成大 hunk。与规范化同族的区间代数，同需 brute-force
  对拍。空 hunk（pdel==0 && cins==0，来回改抵消）过滤。
- **vid1→vid2 行查询流程**：求路径（LCA = 节点带 depth 双指针
  上提）→ 上行段取逆 → 链式 compose 出单一 H* → H* 空则
  零操作 → 兜底判断（Σcins(H*) × K > 目标装载量时，仍走
  一次 forward 消费；folded 才是真实工作量，逐段估计会高估）→
  一次 forward 消费，**读源恒 = vid2 侧 buffer**（目标节点 b）。
  中间版本 buffer 与中间态装载浪费全部消灭；同区域多版编辑
  折叠后只迁移一次。
- commit：journal 规范化 → hunk list 挂新 UNode，**journal
  释放**（全文替换场景内存从 k×24B 缩到少数 hunk）。
- fresh 追赶：journal[lck..jn) 现场规范化成临时 hunk →
  forward 消费（读源 = d->C 当前树），用完即弃，lck = jn。
- fresh undo 丢草稿：journal[0..lck) 现场规范化 → 取逆 →
  forward 消费（读源 = current->b），零特例。
- lc 位点在 fresh 而目标为别版本时：先丢草稿段（取逆），
  再走 vid 路径 compose，可一并 compose 后单次消费。

### 11.5 次要开放项

- redo 分支选择（children[0]?）、undo(vid) 祖先校验细节。
- 是否升格独立 C 库 undotree.h（§八 保底阶梯最终态，仅当
  pt COW 被判不可用时才走到）。
- UNode 内存池化（后续优化）。
