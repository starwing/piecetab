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

| 前缀           | 被测 API                              | 说明                                            |
| -------------- | ------------------------------------- | ----------------------------------------------- |
| `lifecycle`    | `lc_open/close/reset/newtree/deltree` | 创建、销毁、重置                                |
| `scan`         | `lc_scan`                             | 批量扫描构建行缓存                              |
| `seek`         | `lc_seek`, `lc_seekline`              | 光标定位                                        |
| `advance`      | `lc_advance`                          | 光标字节级移动                                  |
| `advline`      | `lc_advline`                          | 光标行级移动                                    |
| `backwardline` | `lcK_backwardline`                    | 内部后退行遍历（跨叶/跨节点路径）               |
| `forwardline`  | `lcK_forwardline`                     | 内部前进跨行遍历                                |
| `backwardoff`  | `lcK_backwardoff`                     | 内部后退字节偏移遍历                            |
| `findline`     | `lcK_findline`                        | 内部查找行（skip-child 路径）                   |
| `clearbreaks`  | `lc_clearbreaks`                      | 清除换行符                                      |
| `splice`       | `lc_splice`, `lcD_splicerange`        | 拼接/删除（含 Fold 触发）                       |
| `markbreak`    | `lc_markbreak`, `lc_markbreaks`       | 标记换行                                        |
| `insert`       | `lc_insert`                           | 插入文本                                        |
| `split`        | 叶/节点/根分裂                        | 分裂级联（通常由 `lc_markbreak` 触发）          |
| `foldleaf`     | `lcD_foldleaf`                        | 叶折叠/平衡，光标调整                           |
| `foldnode`     | `lcD_foldnode`                        | 节点折叠/平衡，光标调整                         |
| `fold`         | Fold 系列综合                         | 通过 splice 触发 foldleaf+foldnode 的综合性测试 |
| `rebalance`    | `lcD_rebalance`                       | 欠满节点后的祖先链重平衡                        |
| `shiftnode`    | `lcD_shiftnode`                       | 相邻兄弟节点重分配                              |
| `balancenode`  | `lcD_balancenode`                     | 两兄弟节点间数据再平衡                          |
| `boundary`     | 边界条件                              | 极值、空树、一致性                              |

### 命名示例

| 测试名                    | 解读                                        |
| ------------------------- | ------------------------------------------- |
| `scan_bulk_many`          | lc_scan 大量 break                          |
| `scan_oom_items`          | lc_scan 时 OOM 在 fill 阶段                 |
| `advance_skip_siblings`   | lc_advance 跨叶时跳过中间兄弟节点           |
| `advline_backward_within` | lc_advline 负向同叶内移动                   |
| `backwardline_cross`      | lcK_backwardline 跨层级外层 for 循环        |
| `forwardline_crossnode`   | lcK_forwardline 跨节点 skip-child           |
| `findline_skip`           | lcK_findline 跳过整个子树                   |
| `findline_skip_deep`      | lcK_findline 在深层树中跳过子树             |
| `clearbreaks_edge`        | lc_clearbreaks 光标在边界                   |
| `splice_trailing`         | splice 在树末尾操作                         |
| `splice_cross_breaks`     | splice 跨多个换行符                         |
| `splice_trimleaf_locend`  | splice 删至树尾触发 trimleaf 的 locend 路径 |
| `splice_mergeleaf_sr`     | splice 后 mergeleaf 的 sr 非零路径          |
| `markbreak_split`         | markbreak 时叶满触发叶分裂                  |
| `markbreak_root_split`    | markbreak 级联触发根分裂                    |
| `markbreak_crossleaf`     | markbreak 跨叶边界                          |
| `insert_single_leaf`      | 插入到单叶树                                |
| `insert_col_mid`          | 在行中（col>0）插入                         |
| `insert_oom_col0`         | 在行首（col==0）插入时 OOM                  |
| `split_cascade`           | 反复 markbreak 触发级联分裂                 |
| `foldleaf_switch`         | foldleaf 平衡后光标从左侧叶切到右侧         |
| `foldnode_cursor_left`    | foldnode 平衡后光标从右子节点切至左子       |
| `balancenode_dneg`        | 两内节点平衡，diff 为负                     |
| `rebalance_merge`         | splice 使内节点欠满 → 祖先链折叠合并        |
| `shiftnode_balance`       | 两相邻内节点 cl+cr > LC_FANOUT 触发重分配   |
| `foldleaf_inmarkbreak`    | markbreak 场景中 foldleaf 的右→左光标移动   |

---

## 三、lc_tests.h 工具参考

### 3.1 分配器

```c
void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize);  // 正常 realloc
int    oom_cnt;                                                       // oom_alloc 倒数器
void  *oom_alloc(void *ud, void *ptr, size_t osize, size_t nsize);    // OOM 模拟
```

用法：
```c
lc_State *S = lc_open(&test_alloc, NULL);   // 正常
lc_State *S = lc_open(&oom_alloc, NULL);    // OOM（需预设 oom_cnt）
```

### 3.2 树构造助手

```c
lc_Node *leafV(unsigned seg1, unsigned seg2, ..., 0);    // 构造叶节点（段大小列表）
lc_Node *botV(lc_Node *leaf1, lc_Node *leaf2, ..., NULL); // 构造底层内部节点
lc_Node *innerV(lc_Node *child1, lc_Node *child2, ..., NULL); // 构造中层内部节点
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

### 3.4 树校验

```c
void lc_checktree(const lc_Cache *c);              // 常规：不允许空子节点
void lc_checktree_allow_empty(const lc_Cache *c, int allow_empty); // 允许空节点
```

`lc_checktree` 递归校验整棵树：
- 每节点 `child_count <= LC_FANOUT`
- 每叶 `breaks[i] <= LC_LEAF_FANOUT`
- 每子树的 `bytes[i]` 与递归计算值一致
- 树总 `bytes`/`breaks` 与逐子节点求和一致

### 3.5 assert_tree — 精确断言树结构（推荐）

```c
assert_tree(c, levels, botV(leafV(10, 10), leafV(15)));
```

构造一棵期望树，与 `c` 进行完全比较（level、子节点数、度量、叶段数组均逐项比较）。失败时打印期望树与实际树的 dump。

**强烈建议**：凡是可预知树最终形态的测试，均用 `assert_tree` 校验。仅依赖 `assert(lc_breaks(c) == N)` 会产生**假阴性**——度量正确但结构错误仍能通过（如 children 排列不对、叶段合并异常等）。

### 3.6 树 dump

```c
void lc_dumptree(const lc_Cache *c, const char *tag);  // 打印树结构到 stderr
```

调试用。`assert_tree` 失败时会自动输出。

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
unsigned brs[] = {10, 10, 10, 10, 0}, *p = brs;
r = lc_scan(c, lc_scanner, &p);
// 产生 4 个 break、40 字节、1 叶的树（LC_LEAF_FANOUT=4）
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

/* levels=2: root → 2 inner → 各自的 botV → 叶 */
lc_Node *left  = innerV(botV(leafV(2, 0)), botV(leafV(2, 0)), botV(leafV(2, 0)));
lc_Node *right = innerV(botV(leafV(2, 0)));
lc_Node *root  = innerV(left, right);
lc_Cache *c = cacheV(S, 2, root);
```

> **注意**：`cacheV` 内部已调用 `lc_newtree`。使用后仍需 `lc_deltree(S, c)` 清理。

---

## 五、完整测试示例

### 示例 1：基本 splice

```c
static void test_splice(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;

    lc_scanV(c, 10, 15, 15);
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 40);

    lc_seek(&cur, c, 10);
    lc_splice(&cur, 5, 0);

    assert_tree(c, 0, botV(leafV(10, 5, 15, 0)));
    assert(lc_breaks(c) == 3 && lc_bytes(c) == 35);

    lc_deltree(S, c);
    lc_close(S);
}
```

### 示例 2：cacheV 构造 + splice + assert_tree（无 lc_scan）

```c
static void test_splicerange_2leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10, 10), leafV(10, 10, 10)));
    assert(c); /* 避免 clang-tidy 警告 c 可能为 NULL */

    lc_seek(&C1, c, 20);
    lc_seek(&C2, c, 40);
    lcD_splicerange(&C1, &C2);

    assert_tree(c, 0, botV(leafV(10, 10, 10, 10)));
    lc_checktree(c);

    lc_deltree(S, c);
    lc_close(S);
}
```

### 示例 3：markbreak 触发级联分裂

```c
static void test_split_cascade(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    int       r, k;

    lc_rscanV(c, 200, 5);
    assert(lc_breaks(c) == 200);
    lc_checktree(c);

    /* 反复在最左叶 markbreak，填满左子树触发级联分裂 */
    for (k = 0; k < 24; ++k) {
        lc_seek(&cur, c, 2);
        r = lc_markbreak(&cur, 2);
        assert(r == LC_OK);
    }
    lc_checktree(c);

    lc_deltree(S, c);
    lc_close(S);
}
```

### 示例 4：OOM 白盒测试（操控池空闲链表）

```c
static void test_insert_oom_col0(void) {
    lc_State *S;
    lc_Cache *c;
    lc_Cursor C;
    unsigned  brs_b[] = {3, 3, 0}, *pb;
    int       count;
    void     *head;

    S = lc_open(&test_alloc, NULL);
    assert(S);
    c = cacheV(S, 0, botV(leafV(5, 5)));
    lc_checktree(c);

    /* 将叶池空闲链截断为仅剩 1 对象 */
    head = S->leaves.freed;
    count = 0;
    while (head) { count++; head = *(void **)head; }
    if (count > 1) {
        int i;
        head = S->leaves.freed;
        for (i = 0; i < count - 1 && *(void **)head; i++)
            head = *(void **)head;
        S->leaves.freed = head;
        *(void **)head = NULL;
    }

    lc_seek(&C, c, 0);

    /* 换 OOM 分配器：splitleafat 取走最后空闲叶，applyfirst 触 OOM */
    S->allocf = oom_alloc;
    oom_cnt = 1;
    pb = brs_b;
    assert(lc_insert(&C, 0, lc_scanner, &pb) == LC_ERRMEM);

    lc_deltree(S, c);
    lc_close(S);
}
```

---

## 六、最佳实践

1. **尽量使用 `assert_tree`** 而不仅 `assert(lc_breaks/bytes)`。度量正确不代表树结构正确（children 排列、叶段分布等）。
2. **每个测试末尾调用 `lc_checktree(c)`** 确保树不变量。
3. **函数名/注释描述功能**，不写行号。
4. **有局部变量的场景声明在函数开头**（C89 风格，`-Wdeclaration-after-statement` 会报 warning→error）。
5. **首选 `cacheV` 构造精确树** 用于边界/内部路径测试，`lc_scan` 用于真实负载/大量 break 场景。
6. **brs 数组过长时用 for 循环**，不用大括号初始化。若 brs 声明超出一行（如 20+ 元素），应用 for 循环初始化，避免冗长且难维护的单行/多行大括号列表。例如：`for (i = 0; i < 200; ++i) brs[i] = 5; brs[200] = 0;`。
7. **大量重复 break 用 `lc_rscanner`**：格式 `{count, val, ..., 0}`。例如 `unsigned brs[] = {768, 1, 0}, *p = brs;` 即 768 个值为 1 的 break。
8. **OOM 白盒技巧**：池分配器仅在空闲链空时调用底层 allocf。若目标 OOM 点前的池分配被空闲链屏蔽，可操控 `S->leaves.freed` 或 `S->nodes.freed` 消耗空闲链后再测试。
9. **状态隔离**：每个测试独立 `lc_open` → `lc_deltree` → `lc_close`，确保无干扰。
