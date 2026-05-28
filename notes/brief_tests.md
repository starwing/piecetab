# lc_test4.c 测试结构笔记

> **注**：本文原基于旧版单文件 `linecache_test.c` 撰写。代码已重构为 `tests/lc_tests.h` (共享辅助) + `tests/lc_test4.c` (LC_FANOUT=4) + `tests/lc_test8.c` (LC_FANOUT=8)。下述行号已废，但测试逻辑、模式、范例仍适用。

## 一、TESTS 宏定义与格式

```c
#define TESTS(X)                        \
    X(lifecycle)                        \
    X(scan_seek)                        \
    X(scan_no_input)                    \
    X(scan_one_leaf)                    \
    X(scan_bulk_many)                   \
    X(scan_append_many)                 \
    X(scan_oom_items)                   \
    X(scan_oom_flush)                   \
    X(scan_oom_build)                   \
    X(seekline)                         \
    X(advance_single)                   \
    X(advance_cross)                    \
    X(node_split)                       \
    X(seek_past_leaf)                   \
    X(advance_backward_cross)           \
    X(advline_cross)                    \
    X(advline_backward_within)          \
    X(cov_backwardline_cross)           \
    X(cov_forwardline_crossnode)        \
    X(advance_skip_siblings)            \
    X(node_split_cursor_right)          \
    X(splitleaf_left)                   \
    X(rootsplit_left)                   \
    X(findlines_skip)                   \
    X(rootsplit_left_deep)              \
    X(nodesplit_left)                   \
    X(findlines_skip_deep)              \
    X(backwardoff_skip)                 \
    X(cov_remaining)                    \
    X(clearbreaks)                      \
    X(clearbreaks_edge)                 \
    X(clearbreaks_slot)                 \
    X(splice)                           \
    X(splice_tmp)                       \
    X(splice_l2)                        \
    X(splice_trailing)                  \
    X(splice_cross_breaks)              \
    X(splice_cross_breaks_slot)         \
    X(splice_cross_breaks_mid)          \
    X(spliceleaf_1a)                    \
    X(spliceleaf_1b)                    \
    X(spliceleaf_1c)                    \
    X(spliceleaf_foldright)             \
    X(spliceleaf_foldleft)              \
    X(prune_leaf)                       \
    X(prune_node)                       \
    X(rebalance_shrink)                 \
    X(rebalance_double)                 \
    X(rebalance_node_merge_right)       \
    X(rebalance_node_merge_left)        \
    X(rebalance_node_merge_right_large) \
    X(rebalance_cascade)                \
    X(trimleaf_trimleft)                \
    X(trimleaf_trimright)               \
    X(trimleaf_mergeleaf)               \
    X(trimleaf_right_end)               \
    X(trimnode_trimright)               \
    X(trimnode_trimleft)                \
    X(trimnode_mergenode)               \
    X(mergeleaf_combine)                \
    X(mergeleaf_sr_nonzero)             \
    X(mergeleaf_cross_subtree)          \
    X(mergenode_combine)                \
    X(mergenode_sr_nonzero)             \
    X(mergenode_cross_subtree)          \
    X(splicerange_2leaf)                \
    X(splicerange_prune)                \
    X(splicerange_all)                  \
    X(splicerange_merge_rebalance)      \
    X(splicerange_foldnode_upper)       \
    X(splice_uf_last)                   \
    X(splice_mergeleaf_sr)              \
    X(splice_mergeleaf_dpos)            \
    X(splice_mergenode_fold)            \
    X(splice_mergenode_dpos)            \
    X(splice_mergenode_last)            \
    X(splice_removed2)                  \
    X(empty_tree_reset)                 \
    X(shiftleaf_neg_d)                  \
    X(foldnode_redistribute)            \
    X(trimleaf_left_full)               \
    X(foldleaf_redistribute_left)       \
    X(foldnode_redistribute_left)       \
    X(foldnode_redistribute_right)      \
    X(foldnode_redistribute_shift)      \
    X(boundary_cmp)                     \
    X(markbreak)                        \
    X(markbreaks)                       \
    X(markbreak_split)                  \
    X(markbreak_empty)                  \
    X(markbreak_noop)                   \
    X(markbreak_crossline)              \
    X(markbreak_crossline_end)          \
    X(markbreak_trailing)               \
    X(markbreak_brzero)                 \
    X(markbreak_node_split)             \
    X(markbreak_root_split)             \
    X(markbreak_split_right)            \
    X(markbreak_root_split_on_add)      \
    X(markbreak_crossleaf)
```

**注册机制**：main() 将 TESTS 宏展开为函数指针表 (2758-2768 行)：

```c
int main(int argc, char *argv[]) {
    typedef struct entry {
        const char *name;
        void (*fn)(void);
    } entry_t;
    const entry_t entries[] = {
#define X(name) {#name, test_##name}, /* clang-format off */
        TESTS(X)
#undef  X
        {NULL, NULL}, /* clang-format on */
    };
```

**运行方式**：
- `./test` (无参数) → 依次运行全部测试
- `./test splice` → 模糊匹配运行测试名（前缀匹配）
- `./test splice,markbreak` → 运行多个匹配的测试
- `./test @splice` → 仅运行首个精确匹配（`@` 前缀）

---

## 二、现有测试函数签名模式

**基本模式**（所有测试均无参数、无返回值）：
```c
static void test_<name>(void) { ... }
```

**典型测试结构**（使用 `lc_scan` 构建树）：
```c
static void test_SOMETHING(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[N] = { ... , 0}, *pbrs = brs;
    int       r;

    r = lc_scan(c, scanner, &pbrs);
    assert(r == LC_OK && lc_breaks(c) == M);

    // ... 操作 ...

    lc_deltree(S, c);
    lc_close(S);
}
```

**典型测试结构**（使用 `cacheV` + 树构造助手直接构建）：
```c
static void test_SOMETHING(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor cur;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10)));

    // ... 操作 ...

    lc_deltree(S, c);
    lc_close(S);
}
```

注意：`cacheV` 模式不需要 `lc_newtree` 单独调用，因为 `cacheV` 内部会调用 `lc_newtree`。

---

## 三、扫描器设置模式 (`scanner` / `test_alloc`)

### 3.1 分配器
```c
static void *test_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nsize);
}
```

### 3.2 扫描器
```c
static unsigned scanner(void *ud, size_t prev) {
    unsigned **brs = (unsigned **)ud;
    (void)prev;
    return brs ? *(*brs)++ : 0;
}
```

此 scanner 取 `ud` 为 `unsigned *` 的指针的指针（即 `unsigned **`）。每次被调用时，从数组中取下一个 `unsigned` 值并递增指针。返回 0 表示扫描结束。

### 3.3 OOM 分配器
```c
static int   oom_cnt;
static void *oom_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud, (void)osize;
    if (nsize == 0) { free(ptr); return NULL; }
    if (--oom_cnt == 0) return NULL;
    return realloc(ptr, nsize);
}
```

用于 `lc_open(&oom_alloc, NULL)` 的 OOM 测试。

### 3.4 在测试中启动 lc_State 的模式
```c
lc_State *S = lc_open(&test_alloc, NULL);   // 正常分配器
lc_State *S = lc_open(&oom_alloc, NULL);    // OOM 测试
```

---

## 四、构建多行 cache 的 scan 方式

### 4.1 基本 scan（单叶以内，<=4 段）
```c
unsigned brs[5] = {10, 15, 15, 15, 0}, *pbrs = brs;
r = lc_scan(c, scanner, &pbrs);
// 产生 levels=0, 1 leaf, breaks=4, bytes=55
```

### 4.2 中等 scan（多叶，单层内部节点）
```c
unsigned brs[18] = {10, 10, 10, 10, 10, 10, 10, 10, 10,
                    10, 10, 10, 10, 10, 10, 10, 10, 0};
// 17 breaks, 170 bytes → 5 leaves → 产生 root 下有多个 children (levels=1)
```

### 4.3 大量 scan（多层级树，>=120 行）
```c
// 120 行 → levels >= 2 (LC_FANOUT=4, LC_LEAF_FANOUT=4)
unsigned brs[121];
for (i = 0; i < 120; ++i) brs[i] = 1;
brs[120] = 0;
r = lc_scan(c, scanner, &pbrs);
// assert(c->levels >= 2)
```

### 4.4 更深层级 (>=65 行)
```c
// 65 行 → scan 构建 multi-level tree（约 levels=2~3）
unsigned brs[66];
for (i = 0; i < 65; ++i) brs[i] = 10;
brs[65] = 0;
```

### 4.5 极深层级 (80~200 行)
```c
// 80 breaks × 5 bytes → levels 通常为 3+
unsigned brs[81];
for (i = 0; i < 80; ++i) brs[i] = 5;
brs[80] = 0;
{
    unsigned *pbrs = brs;
    r = lc_scan(c, scanner, &pbrs);
}
```

### 4.6 追加扫描（非空树再次 scan）
```c
unsigned brs_a[5] = {10, 10, 10, 10, 0}, *pa = brs_a;
unsigned brs_b[6] = {20, 20, 20, 20, 20, 0}, *pb = brs_b;
r = lc_scan(c, scanner, &pa);  // 第一次: 4 breaks, 40 bytes
r = lc_scan(c, scanner, &pb);  // 第二次: 增加到 9 breaks, 140 bytes
```

**层级推演** (LC_LEAF_FANOUT=4, LC_FANOUT=4):
- levels=0: 1 leaf (≤4 segments)
- levels=1: root 下 ≤4 leaves (≤16 segments)
- levels=2: root 下 ≤4 internals, 每个 ≤4 leaves (≤64 segments)
- levels=3: root 下 ≤4 internals, 每个 ≤4 子internals, 每个 ≤4 leaves (≤256 segments)

120 breaks 每条 1 byte → 约 30 leaves → levels 必然 ≥2。
200 breaks 每条 5 bytes → 约 50 leaves → levels 必然 ≥3。

---

## 五、splice 测试中设置两个 marks 然后 splice 的模式

### 5.1 单 cursor + lc_splice（基本 splice 模式）
```c
lc_State *S = lc_open(&test_alloc, NULL);
lc_Cache *c = lc_newtree(S);
lc_Cursor cur;
// scan 构建树...
lc_seek(&cur, c, start_offset);
lc_splice(&cur, del_bytes, ins_bytes);
// lc_splice(cursor, del, ins)
//   del: 删除字节数
//   ins: 插入的虚拟字节数（在 cursor.col 中记录）
```

这是最常用的 splice 模式。只需要一个 cursor —— `lc_splice` 内部会根据 `del` 计算出第二个位置（R）。

### 5.2 两个 cursor + lcD_splicerange（双 cursor splice 模式）

用于需要精确控制 L 和 R 两个端点的场景：

```c
// splicerange_2leaf: levels=0, C1 in leaf0, C2 in leaf1
static void test_splicerange_2leaf(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10, 10), leafV(10, 10, 10)));
    check_tree(c);
    lc_seek(&C1, c, 20);  // 左光标
    lc_seek(&C2, c, 40);  // 右光标
    lcD_splicerange(&C1, &C2);
    assert_tree(c, 0, botV(leafV(10, 10, 10, 10)));
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}
```

**splicerange 完整范例** — `splicerange_prune`（跨越多叶删除中间部分）：
```c
static void test_splicerange_prune(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cursor C1, C2;
    lc_Cache *c = cacheV(S, 0,
        botV(leafV(10, 10), leafV(10, 10), leafV(10, 10), leafV(10, 10)));
    check_tree(c);
    lc_seek(&C1, c, 10);  // L 在 leaf0
    lc_seek(&C2, c, 70);  // R 在 leaf3
    lcD_splicerange(&C1, &C2);
    assert_tree(c, 0, botV(leafV(10, 10)));  // 保留 leaf0 首尾合并
    check_tree(c);
    lc_deltree(S, c);
    lc_close(S);
}
```

**splicerange_merge_rebalance** — 多层树两个 cursor 在不同子树：
```c
static void test_splicerange_merge_rebalance(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Node  *left = innerV(
            botV(leafV(2, 0)), botV(leafV(2, 0)), botV(leafV(2, 0)));
    lc_Node  *right = innerV(botV(leafV(2, 0)));
    lc_Node  *root = innerV(left, right);
    lc_Cache *c = cacheV(S, 2, root);
    lc_Cursor C1, C2;

    // L 在 left->botV[0], R 在 left->botV[2]
    lc_seek(&C1, c, 0);
    lc_seek(&C2, c, 4);
    assert(C1.paths[0] == C2.paths[0]);  // 同一 root child
    assert(C1.paths[1] != C2.paths[1]);  // 不同 inner child

    lcD_splicerange(&C1, &C2);
    check_tree(c);

    lc_deltree(S, c);
    lc_close(S);
}
```

---

## 六、三个特定测试的完整代码

### 6.1 `test_rootsplit_left_deep`

```
T31: root split left path via markbreak on deep tree
```

```c
static void test_rootsplit_left_deep(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[101];
    int       i, r;

    for (i = 0; i < 100; ++i) brs[i] = 5;
    brs[100] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK);

    /* seek to first leaf, add break to trigger cascade from left side */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 2);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}
```

**scan 参数**：100 个 breaks，每个 5 bytes → 100 breaks, 500 bytes → 约 25 leaves → levels 可能为 2~3。

**说明**：先 scan 构建一棵 deep tree（>=2 levels），然后在第一个叶中 seek(2) 并 markbreak(2)，触发从左侧往上的级联节点分裂，最终可能导致 root split。

---

### 6.2 `test_findlines_skip_deep`

```
T33: findlines skip-child in deep multi-level tree
```

```c
static void test_findlines_skip_deep(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[81];
    int       i, r;

    for (i = 0; i < 80; ++i) brs[i] = 5;
    brs[80] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK);

    /* seek to near-end → advline back by many lines → backwardline traverses
     * deep
     */
    lc_seekline(&cur, c, 0);
    r = lc_advline(&cur, 75);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}
```

**scan 参数**：80 个 breaks，每个 5 bytes → 80 breaks, 400 bytes → 约 20 leaves → levels 可能 >=2。

**说明**：seekline 到第 0 行（文件末尾），然后 advline 前进 75 行。触发 `lcC_forwardline` 的 skip-child 路径（跳过整个 child subtree），覆盖 deep tree 中的行跳转逻辑。

---

### 6.3 `test_markbreak_root_split`

```
T19: markbreak triggers root split on deep tree
```

```c
static void test_markbreak_root_split(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[71];
    int       i, r;

    for (i = 0; i < 70; ++i) brs[i] = 10;
    brs[70] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK && lc_breaks(c) == 70);

    /* seek to first gap in first leaf, add break to trigger cascade */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 3);
    assert(r == LC_OK && lc_breaks(c) == 71);

    lc_deltree(S, c);
    lc_close(S);
}
```

**scan 参数**：70 个 breaks，每个 10 bytes → 70 breaks, 700 bytes → 约 18 leaves → levels 必然 >=2（根节点满后会触发 root split）。

**说明**：构建至少 levels=2 的 deep tree 后，seek 到第一叶第一间隙（offset=2），markbreak 添加 3 bytes。此操作触发叶分裂 → 内部节点分裂 → 最终导致 root split（根节点 child_count 超过 LC_FANOUT=4）。

**关联测试** `markbreak_root_split_on_add` (1029-1051 行) 使用 150 个 breaks 构建更深的树后执行相同操作：
```c
static void test_markbreak_root_split_on_add(void) {
    lc_State *S = lc_open(&test_alloc, NULL);
    lc_Cache *c = lc_newtree(S);
    lc_Cursor cur;
    unsigned  brs[151];
    int       i, r;

    for (i = 0; i < 150; ++i) brs[i] = 5;
    brs[150] = 0;
    {
        unsigned *pbrs = brs;
        r = lc_scan(c, scanner, &pbrs);
    }
    assert(r == LC_OK);

    /* markbreak in first gap to trigger cascading splits */
    lc_seek(&cur, c, 2);
    r = lc_markbreak(&cur, 2);
    assert(r == LC_OK);

    lc_deltree(S, c);
    lc_close(S);
}
```

---

## 七、cacheV 命令用法

### 7.1 定义

```c
/* cacheV(S, levels, root) — wrap pre-built root node into a cache. */
static lc_Cache *cacheV(lc_State *S, unsigned levels, lc_Node *root) {
    lc_Cache *c = lc_newtree(S);
    unsigned  i;
    assert(c != NULL && root->child_count <= LC_FANOUT);
    c->levels = levels;
    c->root = *root;
    lc_poolfree(&S->nodes, root); /* root copy moved to cache, free template */
    c->bytes = 0;
    c->breaks = 0;
    for (i = 0; i < c->root.child_count; i++)
        c->bytes += c->root.bytes[i], c->breaks += c->root.breaks[i];
    check_tree_allow_empty(c, 1);
    return c;
}
```

**签名**：`cacheV(S, levels, rootExpr)`
- `S`: `lc_State *` 指针
- `levels`: 树的层级深度（0=仅叶层, 1=root+叶层, 2=root+inner+叶层, ...）
- `rootExpr`: 根节点表达式，通常由 `botV(...)` 或 `innerV(...)` 构建

### 7.2 配合使用的助手宏/函数

```c
#define leafV(...)  leafV_(S, __VA_ARGS__, 0)   // 构造叶节点（段大小列表，0 结尾）
#define botV(...)   botV_(S, __VA_ARGS__, NULL)  // 构造底层内部节点（children 是 leafV）
#define innerV(...) innerV_(S, __VA_ARGS__, NULL) // 构造中层内部节点（children 是 botV/innerV）
```

### 7.3 使用范例

**levels=0 单叶**：
```c
lc_Cache *c = cacheV(S, 0, botV(leafV(10, 10)));
// → 1 leaf, 2 segments: [10, 10], 1 root child
```

**levels=0 多叶**：
```c
lc_Cache *c = cacheV(S, 0, botV(leafV(10), leafV(20)));
// → 2 leaves in root, levels=0 (botV constructor 将其放在 root 直下)
```

**levels=1 两层树**：
```c
lc_Cache *c = cacheV(S, 1, innerV(botV(leafV(10)), botV(leafV(10, 10))));
// → root → 2 internal nodes → leaves
```

**levels=2 三层树**：
```c
lc_Node *left = innerV(botV(leafV(2, 0)), botV(leafV(2, 0)), botV(leafV(2, 0)));
lc_Node *right = innerV(botV(leafV(2, 0)));
lc_Node *root = innerV(left, right);
lc_Cache *c = cacheV(S, 2, root);
// → root → 2 inner → 3+1 botV → leaves
```

**levels=3 四层树**：
```c
lc_Cache *c = cacheV(S, 3, innerV(innerV(innerV(botV(leafV(10))))));
// → root → inner → inner → botV → leaf
```

**使用 cacheV 不需要 lc_newtree**，cacheV 内部已调用。但使用后仍需 `lc_deltree(S, c)` 清理。

**levels 字段必须正确**，因为寻找 leaf 的逻辑根据 levels 判断何时到达叶子层。错误的 levels 值会导致 assertion 失败或段错误。

---

## 八、main 函数

```c
int main(int argc, char *argv[]) {
    typedef struct entry {
        const char *name;
        void (*fn)(void);
    } entry_t;
    const entry_t entries[] = {
#define X(name) {#name, test_##name}, /* clang-format off */
        TESTS(X)
#undef  X
        {NULL, NULL}, /* clang-format on */
    };
    int i, j;
    (void)&innerV_; /* used by upcoming merge tests */
    printf("=== linecache tests ===\n");
    if (argc == 1) {
        /* run all tests */
        const entry_t *e = entries;
        while (e->name) {
            printf("- %s\n", e->name);
            e->fn();
            printf("  %s OK\n", e->name);
            ++e;
        }
        printf("\nAll tests passed!\n");
        return 0;
    }
    /* run specified tests */
    for (i = 1; i < argc; ++i) {
        const char *name = argv[i];
        size_t      len = strlen(name);
        int         found = 0, only = *name == '@';
        if (only) name++, len--;
        for (j = 0; entries[j].name; ++j) {
            if (strlen(entries[j].name) >= len
                && strncmp(name, entries[j].name, len) == 0) {
                printf("- %s\n", entries[j].name);
                entries[j].fn();
                printf("  %s OK\n", entries[j].name);
                found = 1;
                if (only) break;
            }
        }
        if (!found) {
            fprintf(stderr, "Unknown test: %s\n", name);
            return 1;
        }
    }
    return 0;
}
```

**关键点**：
- 旧版 `(void)&innerV_;` 用于抑制 compiler 警告。新版中 `innerV_` 定义于 `tests/lc_tests.h`，由 `LC_TEST_MAIN` 宏集中处理。
- 无参数运行全部测试 （按 TESTS 宏中声明顺序）。
- 前缀匹配逻辑在 `for (j = 0; entries[j].name; ++j)` 循环中。
- `@` 前缀使匹配在第一个命中后停止（`only` 模式）。
- 测试单次运行，无重复框架（非 xUnit 模式）。

> **新版变更**：旧版 main() 已替换为 `LC_TEST_MAIN(banner)` 宏（定义于 `tests/lc_tests.h`）。宏自动生成条目表、运行循环、`@` 前缀精确匹配，省略上述样板代码。

---

## 附录：测试中的关键 API 速查

| API | 用途 |
|-----|------|
| `lc_open(alloc, ud)` | 创建 lc_State |
| `lc_close(S)` | 销毁 lc_State |
| `lc_newtree(S)` | 创建空 lc_Cache |
| `lc_deltree(S, c)` | 销毁 lc_Cache |
| `lc_scan(c, scanner, ud)` | 批量扫描构建行缓存 |
| `lc_seek(&cur, c, offset)` | 定位 cursor 到指定字节偏移 |
| `lc_seekline(&cur, c, line)` | 定位 cursor 到指定行 |
| `lc_advance(&cur, d)` | 光标前进/后退 d 字节 |
| `lc_advline(&cur, d)` | 光标前进/后退 d 行 |
| `lc_markbreak(&cur, br)` | 在光标处插入换行符（br=新行长度） |
| `lc_markbreaks(&cur, scanner, ud)` | 从光标处开始批量插入换行 |
| `lc_clearbreaks(&cur, len)` | 清除光标后的换行符 |
| `lc_splice(&cur, del, ins)` | 从光标处删除 del 字节，插入 ins 虚拟字节 |
| `lcD_splicerange(&C1, &C2)` | 删除 C1→C2 之间的全部内容（两个 cursor 之间） |
| `lc_bytes(c)` | 树的字节总数 |
| `lc_breaks(c)` | 树的换行符总数 |
| `lc_offset(&cur)` | 光标的字节偏移 |
| `lc_line(&cur)` | 光标所在行号 |
| `lc_col(&cur)` | 光标在当前行中的列偏移 |
| `lc_linelen(&cur)` | 光标所在行的长度 |
| `lc_reset(s)` | 重置 lc_State 的内存池 |
