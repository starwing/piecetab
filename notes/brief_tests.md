# 测试编写指南

## 测试纪律（最高优先）

- **没有用户确认，永远不允许删除测试**
- **单测出现任何问题，放弃一切计划，先修单测，再继续原任务**
- **遇 bug 必须先写单测重现、确认失败，再改业务代码**；禁止在重现之前修改业务代码
- 测试是隐性契约的回归网（见 brief_refine.md §26）：重构前列行为清单，逐项映射既有测试

## assert 约定（编码时）

- **用断言保不变式**，非运行时检查
- **static helper 不设防御性参数校验**，用 `assert` 校验关心的前置条件
- 测试尽量用 `asserttree` 匹配树形，避免判定过松不暴露错误（度量正确不代表结构正确）

## 运行方式

`just <xx> [args...]` — `xx` 为库前缀：

| 命令 | 库 |
|---|---|
| `just lc` / `just lc8` | linecache（FANOUT=4 / 8） |
| `just pt` | piecetab |
| `just ut` | undotree |
| `just cg` | cellgrid |
| `just tf` | termfeed |

- 无参数：运行全部；`just lc splice`：前缀匹配所有 `splice*`
- `just lc @splice_trailing`：`@` 仅运行首个匹配
- 覆盖率：`just <xx>-cov`（生成 lcov.info）、`just <xx>-lines`（未覆盖行源码）、`just <xx>-unbranched`（未覆盖分支）、`just cov`（全量）、`just clean`

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
- **宏幻影分支**：`utV_len`（`?:`）、`utV_free`（`&&`）、`utOK` 等 C89 宏展开的伪分支，gcov 可见但逻辑不可达，允许不覆盖但须列入报告
- **reserve 守卫分支**：`utV_push` 在 `utV_reserve` 成功后理论永不失败，push 错误路径为防御代码，允许不覆盖但须列入报告
- **可覆盖的 OOM 路径必须覆盖**：`drainpool` + `oom_alloc` 精准触发，不留未覆盖 OOM 分支
- **可覆盖的逻辑分支必须覆盖**：确无法覆盖须写 **brute test（穷举）** 证明各输入组合正确

## 编写要点

- 每个测试：调用 API 后校验树不变量 + 游标，结束断言无泄漏（如 `S->nodes.live_objs == 0`）
- 边界/内部路径首选 `cacheV` 精确树，大量 break 用 `lc_rscanV`
- C89：局部变量声明在函数开头（`-Wdeclaration-after-statement` 报错）
- `lc_rscanner` 原地修改数组，重复使用须重建
- 测试注释描述功能，不写行号（行号随编辑变化，注定过时）
