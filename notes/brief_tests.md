# 测试编写指南

## 测试纪律（最高优先）

- **没有用户确认，永远不允许删除测试**
- **单测出现任何问题，放弃一切计划，先修单测，再继续原任务**
- **遇 bug 必须先写单测重现、确认失败，再改业务代码**；禁止在重现之前修改业务代码
- 测试是隐性契约的回归网（见 brief_refine.md §28）：重构前列行为清单，逐项映射既有测试

## assert 约定（编码时）

- **用断言保不变式**，非运行时检查
- **static helper 不设防御性参数校验**，用 `assert` 校验关心的前置条件
- 测试尽量用 `asserttree` 匹配树形，避免判定过松不暴露错误（度量正确不代表结构正确）

## 运行方式

`just <xx> [args...]` — `xx` 为库前缀：

| 命令                    | 库                             |
| ----------------------- | ------------------------------ |
| `just lc`               | linecache（FANOUT=4 + 8 全跑） |
| `just lc4` / `just lc8` | linecache（FANOUT=4 / 8）      |
| `just pt`               | piecetab                       |
| `just sp`               | spantree（FANOUT=4 + 8 全跑）  |
| `just sp4` / `just sp8` | spantree（FANOUT=4 / 8）       |
| `just ut`               | undotree                       |
| `just cg`               | cellgrid                       |
| `just tf`               | termfeed                       |

- 无参数：运行全部
- 前缀匹配：`just lc4 splice` 匹配所有 `splice*`
- `@` 前缀：仅运行首个匹配，如 `just lc4 @splice_trailing`
- `?` 前缀：找不到测试不报错、静默退出（exit 0），如 `just lc4 ?no_such_test`
- `just lc` / `just sp` 会自动给两个 runner 都加 `?` 前缀，所以某个测试只存在于 fanout4 时，fanout8 侧不会因 Unknown test 报错
- 覆盖率：`just <xx>-cov`（生成 lcov.info）、`just <xx>-lines`（未覆盖行源码）、`just <xx>-unbranched`（未覆盖分支）、`just cov`（全量）、`just clean`
- **Lua 绑定测试在 `lua/justfile`**，命名与根 justfile 对齐：`just lua/pt`、`just lua/ts`、`just lua/ts-cov`、`just lua/ts-lines`、`just lua/clean`（`just lua/<recipe>` 自动执行该目录下 justfile）。根 justfile 不设其他位置 justfile 的入口

## Fuzz 测试

- 源文件 `fuzz/<lib>_fuzz.c`（pt/sp/lc，fanout 4），共享脚手架 `fuzz/fz.h`（种子 RNG、op 日志 io、op 表宏）
- 运行：`just fuzz/<lib> [seed]`（如 `just fuzz/pt 1`）；崩溃重放：`just fuzz/replay <lib> <path>`；消毒版 `just fuzz/<lib>-dbg [seed]`（ASan/UBSan）
- **op 表统一为 X-macro 权重表**：每 fuzz 定义 `FZ_KIND(X)`（每 op 一行 `X(NAME, weight)`）后展开 `FZ_TABLE()`，得到 enum、名字表、权重表和 `fz_opidx`/`fz_opname`；`o->op = rnd()%100` 按累计权重映射到 op——**权重和必须保持 100**（超 100 的尾部行永不命中），调权重只改数字不重排；加新 op = 表加一行 + `runop` 加 case
- 崩溃契约：每 op 先写 `/tmp/<lib>_oplog.txt` 再执行，崩溃即用 `just fuzz/replay <lib> /tmp/<lib>_oplog.txt` 重放定位
- 查树频率：游标检查每 op（O(1)），树检查每 `FZ_CHECK` op（O(树大小)，默认 256）；`-DFZ_CHECK=1` 最强扫描（build.just 改 CFLAGS 或直接 gcc -D）
- **禁止在 fuzz 里手写弱化 check 副本**：直接用 tests 标准 `pt_checktree`/`lc_checktree`/`sp_checktree`。教训：标准 checktree 曾漏叶容器内合并检查（pt 的 ADJACENT、sp 的 SEGMERGE 相邻段检查），已补进 tests 标准版（tests/pt_tests.h、tests/sp_tests.h）——fuzz 与单测共用一份，两边同步受益

## Lua 测试（luaunit）

- 文件 `lua/tests/<name>_test.lua`，harness 用 luaunit（vendored 于
  `lua/tests/luaunit.lua`）；`just lua/<t>` 跑 PUC + LuaJIT 双运行时
- 过滤：`lua tests/<name>_test.lua -p Pattern`（匹配测试类/方法名）
- **os.exit 铁律**：`os.exit(lu.LuaUnit.run(), true)` 必须在文件**末尾**
  —— run() 执行时遍历 _G 收集测试，定义在其后的类**永不执行**且无任何
  报错（测试数不增 = 中招）。已两次踩坑（TestSc、TestDoc 追加后静默
  丢失），追加测试后必核对测试计数
- **grid cell 断言优先**：`e.grid:cell(r, c)` → (cp, style) 直接读屏幕
  矩阵，比渲染字节流 `assertStrContains`（CSI 序列）稳定——多层高亮后
  CSI 嵌颜色码更脆弱；字节流断言只在测 diff 输出本身时用
- **布局先算再断言**：
  - grid 列 = 内容列 + lnum_width + 1（默认 lnum_width=3，内容 col 0 →
    grid col 4）
  - pt_edit 插入会分裂出 hole piece（`[0,off)` + hole + 剩余），piece
    数 = 1 + 编辑次数；断言 piece 布局前先用 `doc:piece("len"/"next")`
    遍历 dump 实际结构（或先跑调试脚本验证）
  - 多层合成断言用 `e.comp:intern({fg=.., bg=..})` 取 handle 对比，不写
    死 handle 数值（intern 顺序依赖渲染时机）
- **调试**：独立脚本（/tmp/*.lua）+ fake term + `io.write` 复现渲染，
    对应 C 侧 `test_log`（AGENTS.md 调试节）；渲染断言用 cell() 逐格
    打印；「单跑对、测试环境错」先查 os.exit 位置与全局状态污染
- **调试时不要往库头文件（比如spantree.h）加 `#include <stdio.h>`**：
  测试文件（比如`spantree_test_fanout*.c`） 里的 `stdio.h` 都是在头文件之前引入的
  （如果某个测试文件没有，修测试文件而不是改头文件）。
  需要在 spantree.h 内临时打印时，依赖测试已先包含 stdio；
  若 standalone 编译也要在包含头文件前先 `#include <stdio.h>`。
- 断言 API：`assertEquals` / `assertIsTrue` / `assertNotEquals` /
  `assertIsNil` / `assertStrContains`（luaunit 内建）

## 测试文件与命名

- 文件：`<name>_test.c`（如 `undotree_test.c`）；带配置 tag 时 `<name>_test_<tag>.c`，tag 自描述（`linecache_test_fanout4.c` 用 `LC_FANOUT=4`）。无配置的库不写 tag
- 测试命名 `<category>_<detail>`，category 在前：`scan_bulk`、`diff_oom_compose`、`foldnode_cursor_left_cacheV`
- **oom 不作为 category**：`diff_oom_compose` 正 vs ~~`oom_diff_compose`~~ 错
- 同类 null/边界测试**合入同一个** `<category>_params` 函数，不单开（`hunks_params`、`put_params`、`diff_params`）

## 入口与共享工具（tests/tests.h）

- `TEST(name)` 定义测试函数；`misc/gen_entries.lua` 生成 `*.gen.inc`（构建产物，勿手改，`just` 编译时自动再生）
- 断言：`asserteq(a,b)` / `assertok(e)` / `assertstreq(a,b)`；`check(e, ...)` 用于校验器 helper（失败打日志返回 0）
- 分配器：`test_alloc` 正常、`oom_alloc` OOM 模拟（计数）、`count_alloc` / `oomcount_alloc` 计数（断言无泄漏用）
- 日志：`test_log(...)`（stderr）；测试文件用 `TEST_STATIC` 定义本地 helper
- **OOM 测试技巧**：`drainpool` 清空 freelist 强制走 page 分配路径，再用 `oom_alloc` 计数控制 OOM 触发点

## linecache 专属工具（tests/lc_tests.h）

- **构建树**：
  - `lc_scanV(c, 10, 15)` — 扫描构建（真实负载/大量 break）
  - `lc_rscanV(c, 512, 1, 256, 1)` — [次数, 值] RLE（大量重复值）
  - `cacheV(S, levels, botV(leafV(10), leafV(15)))` — 手工精确构造树（边界/内部路径），已含 `lc_newcache`，用后仍须清理
- **校验**（每个测试必须）：
  - `lc_checktree(c)` / `lc_checktree_allow_empty(c, n)` — 树不变量
  - `lc_checkcursor(&C, off)` — 游标位置与 paths 一致
  - `checkleavesV(c, count, val, ..., 0)` — 叶段序列 RLE 比对
- **`lc_asserttree(c, lvls, ...)`** — 构造期望树全树精确比对，失败自动 dump 期望/实际树。**可预知终态的测试优先用**，仅断 `lc_breaks/bytes` 会假阴性（children 排列错、段合并错仍通过）
- `lc_dumptree(c, tag)` — 调试打印

## 覆盖率铁律

- **严禁 LCOV_EXCL** — 不得用 `LCOV_EXCL_BR_LINE` / `LCOV_EXCL_START` 排除标记，这是作弊
- **行覆盖率必须 100%**；**分支覆盖率基准线 95%**，到不了须写报告说明每一条未覆盖分支原因
- **覆盖构建已带 `-DNDEBUG`**（build.just 的 c-cov-run / lua-cov-run），assert 分支不纳入统计；95% 针对的是实际可执行分支
- **宏幻影分支**：`utV_len`（`?:`）、`utV_free`（`&&`）、`utOK` 等 C89 宏展开的伪分支，gcov 可见但逻辑不可达，允许不覆盖但须列入报告
- **reserve 守卫分支**：`utV_push` 在 `utV_reserve` 成功后理论永不失败，push 错误路径为防御代码，允许不覆盖但须列入报告
- **可覆盖的 OOM 路径必须覆盖**：`drainpool` + `oom_alloc` 精准触发，不留未覆盖 OOM 分支
- **可覆盖的逻辑分支必须覆盖**：确无法覆盖须写 **brute test（穷举）** 证明各输入组合正确

## 覆盖率纠偏（2026-08-14 定案，spantree 绑定教训）

- **95% 分支门槛含豁免余量**：确认不可达/幻影的分支逐条报告豁免即可，
  **禁止以覆盖率为名改写代码逻辑或删惯用法守卫**——`if
  (luaL_newmetatable(...))` 幂等注册、`__gc` 的 NULL 幂等守卫、OOM
  检查均属惯用法，删之有害。brief_refine §33 的"删防御检查"只适用于
  API 契约已保证的冗余分支，不适用于守卫惯用法。
- 豁免报告要求：每条附证据——结构保证论证（枚举恒界/契约）、gcov
  幻影证据（DA 已执行行上的 BRDA 边）、不可注入路径（如 lua_Alloc
  无 OOM 注入手段）。
- **luals 零诊断**：无法干净消解的诊断直接注释豁免
  （`---@diagnostic disable-next-line: xxx`），**不写绕过代码**（如
  ANY 变量凑类型）。
- **shell 工具走 justfile**：subagent 权限白名单只放行 `just`；需要
  awk/sed/python 等 shell 工具时，**先写进 justfile recipe 再
  `just` 调用**（build.just 已有 cov-lines/cov-unbranched 范式）。
  Coder 应意识到：只要封装进 just 即可使用任何工具。

## 编写要点

- 每个测试：调用 API 后校验树不变量 + 游标，结束断言无泄漏（如 `S->nodes.live_objs == 0`）
- **编辑循环每步必挂 `sp_checkcursor`**（2026-08-15 教训：differ/fill_brute
  曾只验树不验游标，mergeleft 光标脱节 bug 潜伏到 fuzz 才炸——树校验
  对光标状态失明；`checktree` 绿 ≠ 光标对。pt/lc 测试早已挂
  checkcursor，spantree 补齐）。游标期望位置按操作语义给：append/
  splice/fill = pos+len，insert/remove = pos
- 边界/内部路径首选 `cacheV` 精确树，大量 break 用 `lc_rscanV`
- C89：局部变量声明在函数开头（`-Wdeclaration-after-statement` 报错）
- `lc_rscanner` 原地修改数组，重复使用须重建
- 测试注释描述功能，不写行号（行号随编辑变化，注定过时）
