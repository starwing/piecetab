# linecache 测试编写指南

## 一、运行方式

```sh
./lc_test4              # 运行全部测试
./lc_test4 splice       # 前缀匹配：所有 splice* 测试
./lc_test4 splice,mark  # 多组匹配
./lc_test4 @splice      # @ 前缀：仅首个精确匹配
```

匹配为前缀匹配：`splice` 可匹配 `splice`、`splice_trailing`、`splice_cross_breaks` 等。

---

## 二、命名规范

> **铁律**：函数名、注释、文档均以功能描述，**不写行号**（行号随编辑变化，注定过时）。

命名格式：`<操作域>_<场景细节>`

### 操作域前缀一览

| 前缀          | 被测 API                              | 说明                      |
| ------------- | ------------------------------------- | ------------------------- |
| `lifecycle`   | `lc_open/close/reset/newtree/deltree` | 创建、销毁、重置          |
| `boundary`    | 边界条件                              | 极值、空树、一致性        |
| `scan`        | `lc_scan`                             | 批量扫描构建行缓存        |
| `seek`        | `lc_seek`, `lc_seekline`              | 光标定位                  |
| `adv`         | `lc_advance`/`lc_advline`             | 光标字节级移动            |
| `clearbreaks` | `lc_clearbreaks`                      | 清除换行符                |
| `splice`      | `lc_splice`, `lcD_splicerange`        | 拼接/删除（含 Fold 触发） |
| `markbreak`   | `lc_markbreak`, `lc_markbreaks`       | 标记换行                  |
| `insert`      | `lc_insert`                           | 插入文本                  |

### 命名示例

| 测试名                   | 解读                                        |
| ------------------------ | ------------------------------------------- |
| `scan_bulk_many`         | lc_scan 大量 break                          |
| `scan_oom_items`         | lc_scan 时 OOM 在 fill 阶段                 |
| `splice_trailing`        | splice 在树末尾操作                         |
| `splice_cross_breaks`    | splice 跨多个换行符                         |
| `splice_trimleaf_locend` | splice 删至树尾触发 trimleaf 的 locend 路径 |
| `splice_mergeleaf_sr`    | splice 后 mergeleaf 的 sr 非零路径          |
| `insert_single_leaf`     | 插入到单叶树                                |
| `insert_col_mid`         | 在行中（col>0）插入                         |
| `insert_oom_col0`        | 在行首（col==0）插入时 OOM                  |
| `insert_brute`           | 遍历树中所有插入位置，确认插入操作正确      |

---

## 三、lc_tests.h 工具参考

### 3.1 分配器

```c
void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize);  // 正常 realloc
void  *oom_alloc(void *ud, void *ptr, size_t osize, size_t nsize);    // OOM 模拟，ud 是 int *oom_cnt
```

用法：
```c
lc_State *S = lc_open(&test_alloc, NULL);   // 正常
lc_State *S = lc_open(&oom_alloc, &oom_cnt);    // OOM（需预设 oom_cnt）
```

### 3.2 树构造助手

```c
lc_Node *leafV(unsigned segs, ...);    // 构造叶节点（段大小列表）
lc_Node *botV(lc_Node *leafs, ...);    // 构造底层内部节点
lc_Node *innerV(lc_Node *childs, ...); // 构造中层内部节点
lc_Cache *cacheV(lc_State *S, unsigned levels, lc_Node *root); // 将预构建树装入 Cache
```

用法见"构建树的两种方式"节。

### 3.3 扫描器与便捷宏

**日常测试推荐用宏**，零样板代码，数组自动管理，无重用隐患：

```c
/* lc_scanV: 每个参数是一个 break 的字节数（值不同时用） */
lc_scanV(c, 10, 15, 15);           // 3 个 break: 10, 15, 15 字节

/* lc_rscanV: [次数, 值] 成对出现（大量重复值时用） */
lc_rscanV(c, 17, 10);              // 17 个 break，每个 10 字节
lc_rscanV(c, 512, 1, 256, 1);     // 512 个 1 字节 + 256 个 1 字节
lc_rscanV(c, 20, 1);              // 20 个 1 字节
```

`lc_scanV` 内部调用 `lc_scan(c, lc_scanner, &p)` 并 `assert(r == LC_OK)`。`lc_rscanV` 同理用 `lc_rscanner`。每调用创建独立的局部数组，**无修改重用问题**。

**底层 scanner**（仅测试 `lc_scan` 本身时直接用）：

```c
/* lc_scanner: 逐值读取，遇 0 停止。 */
unsigned lc_scanner(void *ud, size_t prev);

/* lc_rscanner: [次数, 值] 重复对，遇 0 次停止。 */
unsigned lc_rscanner(void *ud, size_t prev);
```

格式：`lc_scanner` 用 `{val1, val2, ..., 0}`；`lc_rscanner` 用 `{count1, val1, count2, val2, ..., 0}`。

> `lc_rscanner` 会**原地修改**数组。仅在不使用宏时需注意。

### 3.4 树/游标校验

```c
void lc_checktree(const lc_Cache *c);              // 常规：不允许空子节点
void lc_checktree_allow_empty(const lc_Cache *c, int allow_empty); // 允许空节点
void lc_checkcursor(const lc_Cursor *C, size_t expected_offset); // 校验游标位置
```

`lc_checktree` 递归校验整棵树：
- 每节点 `child_count <= LC_FANOUT`
- 每叶 `breaks[i] <= LC_LEAF_FANOUT`
- 每子树的 `bytes[i]` 与递归计算值一致
- 树总 `bytes`/`breaks` 与逐子节点求和一致

`lc_checkcursor` 校验游标位置与预期 offset 是否匹配。
- 游标的 `paths` 是否合法
- `off` 是否和 `paths` 匹配
- `loff` 是否和叶内位置 `lidx` 匹配

所有测试都 **必须** 在待测API调用后检查游标和树的一致性。

### 3.5 lc_asserttree — 精确断言树结构（推荐）

```c
lc_asserttree(c, levels, botV(leafV(10, 10), leafV(15)));
```

构造一棵期望树，与 `c` 进行完全比较（level、子节点数、度量、叶段数组均逐项比较）。失败时打印期望树与实际树的 dump。

**强烈建议**：凡是可预知树最终形态的测试，均用 `    lc_asserttree` 校验。仅依赖 `assert(lc_breaks(c) == N)` 会产生**假阴性**——度量正确但结构错误仍能通过（如 children 排列不对、叶段合并异常等）。

### 3.6 树 dump

```c
void lc_dumptree(const lc_Cache *c, const char *tag);  // 打印树结构到 stderr
```

调试用。`lc_asserttree` 失败时会自动输出。

### 3.7 测试入口

```c
#define TESTS(X)          \
    X(test_one)           \
    X(test_two)

LC_TEST_MAIN("my tests")
```

`LC_TEST_MAIN` 自动生成 `main()`，包含条目表、运行循环、`@` 前缀精确匹配。每个 `X(name)` 对应 `static void test_##name(void)` 函数。

---

## 四、构建树的两种方式

### 方式一：lc_scan（扫描构建）

适合需要真实扫描行为、大量 break、或触发内部 fill/flush 路径的场景。

```c
lc_scanV(c, 10, 10, 10, 10); // 产生 4 个 break、40 字节、1 叶的树（LC_LEAF_FANOUT=4）
// 或者
lc_rscanV(c, 4, 10); // 同上，4 个 break，每个 10 字节
```

**层级推演**（LC_LEAF_FANOUT=4, LC_FANOUT=4）：
- levels=0: 1 叶 (≤4 段)
- levels=1: root 下 ≤4 叶 (≤16 段)
- levels=2: root → inner → ≤16 叶 (≤64 段)

### 方式二：cacheV（手工构造）

适合精确控制树结构、深层层级、边界条件的场景。

```c
/* levels=0: root 直连 2 叶 */
lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10), leafV(15)));

/* levels=1: root → 2 inner → 叶 */
lc_Cache *c = cacheV(S, 1, innerV(
    botV(leafV(10)), botV(leafV(10), leafV(10))));

> **注意**：`cacheV` 内部已调用 `lc_newtree`。使用后仍需 `lc_deltree(S, c)` 清理。

---

## 五、完整测试示例

### 示例 1：基本 splice

```c
static void test_splice(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor C;
    assert(c); /* 避免 clang-tidy 警告 c 可能为 NULL */

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);

    /* 调用待测 API */
    lc_seek(&C, c, 10);
    lc_splice(&C, 5, 0);

    /* 必须包含的断言：树结构与度量正确，游标位置正确 */
    lc_asserttree(c, 0, botV(leafV(10, 10, 15)));
    lc_checktree(c);
    lc_checkcursor(&C, 10);

    /* 其他测试 */
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 35);

    /* 释放资源并检测内存泄露 */
    lc_deltree(S, c);
    assert(S->nodes.live_objs == 0 && S->leaves.live_objs == 0);
    lc_close(S);
}
```

### 示例 2：cacheV 构造 + splice + lc_asserttree（无 lc_scan）

```c
static void test_backwardline_cross(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = cacheV(
            S, 1,
            innerV(botV(leafV(10), leafV(10), leafV(10)), botV(leafV(10))));
    lc_Cursor C;
    assert(c); /* 避免 clang-tidy 警告 c 可能为 NULL */

    /* 调用 API */
    lc_seekline(&C, c, 3);
    assert(lc_advline(&C, -1) == LC_OK);

    /* 断言树结构与度量正确，游标位置正确 */
    assert(r == LC_OK);
    lc_checkcursor(&C, 20);
    lc_asserttree(c, 1,
              innerV(botV(leafV(10), leafV(10), leafV(10)), botV(leafV(10))));

    /* 释放资源，检测内存泄露 */
    lc_deltree(S, c);
    assert(S->nodes.live_objs == 0 && S->leaves.live_objs == 0);
    lc_close(S);
}
```

---

## 五、最佳实践

1. **尽量使用 `lc_asserttree`** 而不仅 `assert(lc_breaks/bytes)`。度量正确不代表树结构正确（children 排列、叶段分布等）。
2. **每个测试必须使用 `lc_checktree`/`lc_checktree_allow_empty` 和 `lc_checkcursor`** 确保树不变量。
3. **函数名/注释描述功能**，不写行号。
4. **有局部变量的场景声明在函数开头**（C89 风格，`-Wdeclaration-after-statement` 会报 warning→error）。
5. **首选 `cacheV` 构造精确树** 用于边界/内部路径测试，`lc_scan` 用于真实负载/大量 break 场景。
6. **大量重复 break 用 `lc_rscanner`**：格式 `{count, val, ..., 0}`。例如 `unsigned brs[] = {768, 1, 0}, *p = brs;` 即 768 个值为 1 的 break。
7. **测试结束必须检测内存泄露** 确保 `S->nodes.live_objs` 和 `S->leaves.live_objs` 均为 0，无内存泄露。
