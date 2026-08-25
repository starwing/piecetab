# 测试框架统一（设计决议）

> 背景：5 个测试文件各带 runner/断言/allocator，3 套命名，基建分散
> （pt_tests.h / lc_tests.h / ut_tests.h + cellgrid/termfeed 内联）。
> 决议日期见 git log。目标写法：`TEST(name) {...}` + 免 TESTS 列表。

## 规则

### 1. 公共基建 → `tests/tests.h`（stb 风格单头，C89）

- `TEST(name)` 宏 → `static void test_name(void)`
- 断言（失败即 abort，打印 file:line）：
  - `asserteq(a, b)` — long 比较
  - `assertstreq(a, b)` — strcmp
  - 布尔断言直接用标准 `assert(e)`
- `check(e, ...)` — 不变式 checker 用（失败打印并 return 0）
- `test_alloc` / `oom_alloc` — 分配器（OOM 测试铁律依赖）
- `test_run(banner, entries, argc, argv)` — runner：无参数全跑；
  前缀匹配；`@` 开头只跑第一个匹配；未知测试报错
- `test_log` / `test_lu` — 打印工具（吸收各库 `xx_log`/`xx_lu`）

### 2. 测试源布局

```c
#include "tests.h"
TEST(foo) { ... }
#include "foo.gen.inc"   /* 脚本生成，勿手改 */
```

- 函数命名保持 `<category>_<detail>`（如 `TEST(hunks_params)`）
- 测试文件末尾**不写** TESTS(X) 列表和 main —— 由生成物提供

### 3. 免 TESTS 列表 — `tests/gen_entries.lua` 脚本生成

- 扫描测试源 `^TEST(name)` 行，生成 `<src>.gen.inc`：
  entries 表（`{"name", test_name}`）+ main
- banner = 源文件 basename 去 `.c`
- 生成物 gitignore（`tests/*.gen.inc`）；`just` 编译前自动重新生成
  （c-dbg-run / c-cov-run 前置一步）

### 4. 库特有基建位置

- **单 FANOUT 库**（pt/ut/cg/tf）：基建进测试 `.c`，删 `*_tests.h`
  - 未来 piecetab 增加 fanout=8 时，把 dump/checktree 抽回共享头
- **多 FANOUT 库**（lc，4/8 双测试）：基建留共享头 `lc_tests.h`
  （瘦身：仅 dump/checktree/scanV/asserttree 等线缓存特有）
- 特有基建统一用公共 `test_log`/`test_lu`/`check`，删各库别名

### 5. 文件命名与 banner

- 测试文件：`<name>_test.c`；带配置 tag 时 `<name>_test_<tag>.c`
  （tag 自描述：`fanout4`；多配置 `_tag1_tag2` 拼接）
- banner：生成器从文件名提取库名与 tag → `[piecetab]`、
  `[linecache fanout4]`；结尾 `[<库>] All tests passed!` 便于多库
  串跑时分辨

### 6. 迁移清单

| 文件 | 动作 |
|---|---|
| tests/tests.h | 新建（公共：TEST/asserteq/assertstreq/check/test_run） |
| tests/gen_entries.lua | 新建（生成 .gen.inc：entries + main + banner） |
| tests/piecetab_test_fanout4.c | 基建并入 .c；删 pt_tests.h |
| tests/lc_tests.h | 瘦身（删公共部分） |
| tests/linecache_test_fanout4.c / fanout8.c | 双 FANOUT 共享 lc_tests.h |
| tests/undotree_test.c | drainpool 并入；删 ut_tests.h |
| tests/cellgrid_test.c / termfeed_test.c | 删内联 runner |
| build.just | c-dbg-run/c-cov-run 前置 gen_entries |
| .gitignore | `tests/*.gen.inc` |
