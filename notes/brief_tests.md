# brief_tests.md — 测试纪律与工具

> 写测试前读本文件；调试见 `notes/brief_debug.md`；框架定案见 `notes/design_test_framework.md`。
> 测试 / fuzz 源文件不受库头文件的 25/30 行限制（意图清晰、可回归优先）。

## 1. 铁律

- **没有用户确认，永远不允许删除测试**
- **单测出现任何问题，放弃一切计划，先修单测，再继续原任务**
- **遇 bug 必须先写单测重现、确认失败，再改业务代码**；禁止在重现之前修改业务代码
- 测试是隐性契约的回归网（列行为清单、逐项映射既有测试，见 `brief_refine.md` 7.2）

## 2. 断言与不变式

- **用断言保不变式**，非运行时检查；static helper 不设防御性参数校验，用 `assert` 校验前置条件
- 结构断言用精确匹配（如 `asserttree` 全树比对），避免判定过松：度量正确不代表结构正确
- 可预知终态的测试优先全树精确比对，仅断计数 / 字节数会假阴性
- 每个测试：调用 API 后校验结构不变量 + 游标位置，结束断言无泄漏

## 3. 命令面（just）

`just <别名> [测试名]`；根不提供其它 `*-cov` 别名，其余 recipe 用子模块调用 `just t <recipe>`。

| 命令 | 内容 |
|------|------|
| `just lc` / `lc4` / `lc8` | linecache（双 / 单 FANOUT） |
| `just pt` | piecetab |
| `just sp` / `sp4` / `sp8` | spantree |
| `just at` / `at4` / `at9` / `at32` | anchortree |
| `just ut` / `cg` / `tf` / `tm` / `comp` | undotree / cellgrid / termfeed / textmatch / compositor |
| `just cov` | C 全量覆盖率（lc/pt/ut/cg/tf/sp/tm；at、comp 用各自 `-cov`） |
| `just at-cov` / `at-lines` / `at-unbranched`、`just comp-cov` | anchortree / compositor 覆盖率 |
| `just t <lib>-cov` / `<lib>-lines` / `<lib>-unbranched` | 其余库覆盖率（lc/pt/ut/cg/tf/tm/sp） |
| `just lua/<recipe>` | Lua 绑定测试与其他 lua recipe |
| `just fuzz/<lib> [seed]` | fuzz（pt/sp/lc/at） |

- 无参数全跑；前缀匹配（`just lc4 splice` 匹配所有 `splice*`）；`@` 前缀只跑首个匹配；`?` 前缀未匹配静默退出（exit 0）
- `just lc` / `sp` / `at` 对多 runner 自动加 `?`：测试只存在于某个 FANOUT 时另一侧不报 Unknown test
- `just tm` 先跑 `misc/check_textmatch.lua` 检查
- **不要直接调 gcc 或测试二进制**；编译器 / 运行命令集中在 `build.just`，子 justfile 只调用其 recipe

## 4. 覆盖率

- **严禁 LCOV_EXCL**（排除标记 = 作弊）；**行覆盖率必须 100%**；**分支覆盖率基准线 95%**，到不了须逐条报告
- 覆盖构建带 `-DNDEBUG`（assert 分支不纳入统计）；95% 针对实际可执行分支
- **豁免只留**：结构保证、平台分支、依赖外部配置、宏幻影分支（`?:`、`&&` 展开的伪分支）、契约已保证的冗余守卫；每条豁免附证据（结构保证论证 / gcov 幻影证据 / 不可注入路径）
- **可覆盖的 OOM 路径必须覆盖**（`drainpool` 清 freelist + `oom_alloc` 计数触发）；可覆盖的逻辑分支无法覆盖时用穷举测试（brute test）证明
- **禁止以覆盖率为名改写代码逻辑或删惯用法守卫**（幂等注册、`__gc` NULL 守卫、OOM 检查都属惯用法）
- 覆盖率缺口先问“能消除吗”（改写代码优先），豁免是最后手段；手段顺序见 `brief_refine.md` 8.2
- luals 零诊断；无法干净消解的诊断用 `---@diagnostic disable-next-line` 注释豁免，**不写绕过代码**
- 需要 awk/sed/python 等工具时先写进 justfile recipe 再 `just` 调用（权限白名单只放行 `just`）

## 5. 文件、命名与基建

- 文件 `<name>_test.c`；带配置 tag 时 `<name>_test_<tag>.c`，tag 自描述（如 `fanout4`）；无配置不写 tag
- 测试命名 `<category>_<detail>`，category 在前；**oom 不作为 category**（`diff_oom_compose` 对，`oom_diff_compose` 错）
- 同类 null / 边界测试**合入同一个** `<category>_params` 函数，不单开
- 基础设施位置：多配置库放共享头（如 `tests/lc_tests.h` / `tests/at_tests.h`），单配置库放测试 `.c` 内；出现第二个配置时抽共享头
- 测试源只写 body，`main` 与 entries 表由 `misc/gen_entries.lua` 生成到 gitignore 的 `.gen.inc`（勿手改，`just` 编译前自动再生）：
  ```c
  #include "tests.h"
  TEST(hunks_params) { ... }
  #include "linecache_test_fanout4.gen.inc"
  ```
- `tests/tests.h` 提供：`asserteq` / `assertok` / `assertstreq`、`check(e, ...)`（失败打日志返回 0）、`test_alloc` / `oom_alloc` / 计数分配器、`test_log(...)`（stderr）；测试本地 helper 用 `TEST_STATIC`
- 库专属工具（树构造 / 精确校验 helper）见对应库 brief（如 `lc_asserttree` / `lc_dumptree`，见 `notes/brief_linecache.md` §十）

## 6. fuzz

- 源文件 `fuzz/<lib>_fuzz.c`（pt/sp/lc/at），共享脚手架 `fuzz/fz.h`（种子 RNG、op 日志 io、op 表宏）
- 运行 `just fuzz/<lib> [seed]`；崩溃重放 `just fuzz/replay <lib> <path>`；消毒版 `just fuzz/<lib>-dbg [seed]`（ASan/UBSan）；anchortree 另有 `at-odd` / `at-odd-dbg` / `at-odd-replay fanout path`
- **op 表统一 X-macro 权重表**：每 fuzz 定义 `FZ_KIND(X)`（每 op 一行 `X(NAME, weight)`）后展开 `FZ_TABLE()`，得到 enum、名字表、权重表和 `fz_opidx` / `fz_opname`；`o->op = rnd()%100` 按累计权重映射——**权重和必须保持 100**（超 100 的尾部行永不命中），调权重只改数字不重排；加 op = 表加一行 + `runop` 加 case
- **崩溃契约**：每 op 先写 `/tmp/<lib>_oplog.txt` 再执行，崩溃即 `just fuzz/replay <lib> /tmp/<lib>_oplog.txt` 重放定位
- 查树频率：游标检查每 op（O(1)），树检查每 `FZ_CHECK` op（默认 256）；`-DFZ_CHECK=1` 最强扫描
- **禁止在 fuzz 里手写弱化 check 副本**：直接用 tests 的标准 `checktree`（单测与 fuzz 共用一份；标准版曾漏叶容器内合并检查，已补）

## 7. Lua 测试

- 文件 `lua/tests/<name>_test.lua`，harness 用 vendored luaunit；`just lua/<recipe>` 跑 PUC + LuaJIT 双运行时；过滤用 `-p Pattern`
- **`os.exit(lu.LuaUnit.run(), true)` 必须在文件末尾**：run() 遍历 `_G` 收集测试，定义在其后的类永不执行且无报错（追加后必核对测试计数；已两次踩坑）
- 断言优先屏幕矩阵（`cell(r, c)` → (cp, style)）或精确句柄（`intern` 取 handle 后比对），不写死数值；字节流断言只在测 diff 输出本身时用
- **布局先算再断言**：先 dump 实际结构（piece 链 / 网格映射）验证，再写期望；不要凭想象写死布局数值
- 调试：独立脚本（`/tmp/*.lua`）+ fake term（`write/flush/move/size`）+ `io.write` 复现；「单跑对、测试环境错」先查 `os.exit` 位置与全局状态污染
- **不要为临时打印往库头文件加 `#include <stdio.h>`**：测试文件已在头文件之前包含；缺就修测试文件
- 断言 API：`assertEquals` / `assertIsTrue` / `assertNotEquals` / `assertIsNil` / `assertStrContains`

## 8. 编写要点

- 游标必须逐层检查：**编辑循环每步挂 checkcursor**——树校验对光标状态失明（树绿 ≠ 游标对）；游标期望位置按操作语义给
- 边界 / 内部路径首选精确树构造 helper；大量重复值用 RLE scan helper
- Clean C：局部变量声明在函数开头（`-Wdeclaration-after-statement` 报错）
- 注释描述功能，不写行号（行号随编辑变化，注定过时）
