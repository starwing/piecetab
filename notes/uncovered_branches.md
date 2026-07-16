# piecetab 分支覆盖指南

> 源：`piecetab.h` lcov BRDA 数据
> 测试参数：`PT_FANOUT=4 PT_PAGE_SIZE=512 PT_MAX_HOLESIZE=16`
> 当前：行 100% / 函数 100% / 分支 90.9% (718/790)
> （cov 系列测试已覆盖大部分 ERROR 分支与部分 LOGIC 分支：
> `test_error_cov_params` / `test_reserve_cov_oom` / `test_fork_cov_oom` /
> `test_edit_cov_holeoom` / `test_remove_cov_midsplit` / `test_edit_cov_prevfull`，
> 下文各表为 87.6% 时期的快照，部分条目已过时）

---

## 分类

| 类别 | 含义 | 优先级 |
|------|------|--------|
| **ASSERT** | `assert()` 内的分支 — release 下不可达，无需测 | 忽略 |
| **DEAD** | 在当前常量/调用约定下数学不可达 | 忽略 |
| **ERROR** | NULL检查/OOM 错误返回路径 — 防御性代码 | 低 |
| **LOGIC** | 有意义的未覆盖逻辑路径 — 值得加测 | 中-高 |

---

## 当前未覆盖分支概况

`just pt-unbranched` 输出约100行，分类如下：

| 类别 | 数量 | 说明 |
|------|------|------|
| ASSERT | ~40 | `assert(X && Y)` 中 X/Y 的 false 方向 |
| DEAD (for循环跳过) | ~10 | `for(l=levels; l>=0; --l)` 首次 `l>=0` 为 false 不可达 |
| ERROR (NULL/OOM) | ~35 | `if(!C)`、`ptP_alloc` 返回 NULL 等防御路径 |
| LOGIC | ~15 | 算法边角分支 |

**有效覆盖率**（排除ASSERT+DEAD）：~93%

---

## 各类别详细说明

### ASSERT — 不覆盖

所有 `assert()` 内的布尔表达式，GCOV 将其拆为子条件，每个子条件有 T/F 两个分支。F 方向（断言失败）永不触发。

典型：
```
L327  assert(i >= 0 && i < PT_FANOUT)
L378  assert(s <= e && e <= ptN_cc(p))
L771  assert(h && len <= PT_MAX_HOLESIZE)
```

### DEAD — 不覆盖

for循环首条件永为 false 的"跳过循环体"方向：

```
L503  for (i = 0; i < ptN_cc(p) && ...)    ← ptN_cc(p) 永不为0
L541  for (l = ptK_levels(C); l >= 0; --l) ← levels 恒 ≥0
L556  同上（另一个函数）
L1024 for (fl = l - 1; fl >= 0; --fl)      ← 同模式
L1219 同上
```

这些循环只在空树/非法状态下会跳过，当前调用约定保证不会发生。

### ERROR — 低优先级

NULL 参数检查、OOM 分配失败路径。大致分两类：

**极易覆盖**（加一行测试即可）：
- `pt_*` API 的 `C == NULL` 检查 → `pt_xxx(NULL, ...)` 单行
- `pt_*` API 的 `C->tree == NULL` 检查 → 设 `c.tree = NULL` 再调用

已通过 `test_error_paths` 覆盖大部分。剩余可通过扩展该测试覆盖。

**需 OOM 注入**（需 `oom_alloc` + 精确计数）：
- `ptP_alloc` / `ptA_alloc` 返回 NULL
- `ptK_markdirty` 分配失败
- `ptH_reserve` 失败

OOM 测试脆弱：pool 页大小 512 字节，节点数量依赖树结构，难以精确计数到目标失败点。已通过 `test_insert_oom_*` / `test_edit_oom` / `test_remove_oom` 覆盖了主路径。

### LOGIC — 值得覆盖

以下条目有实际算法价值：

#### 导航/读

| 行 | 函数 | 分支 | 如何触发 |
|----|------|------|----------|
| L596 | pt_next | `poff == bytes[i]` 为 true | 游标恰好在某 piece 末尾时调 pt_next |
| L600 | pt_next | `l < 0` 返回 NULL | 从最后 piece 调 pt_next（已覆盖，GCOV 假阳） |
| L619 | pt_prev | `plen == NULL` | pt_prev 从 piece 边界返回且 plen=NULL（已覆盖） |

#### 编辑

| 行 | 函数 | 分支 | 如何触发 |
|----|------|------|----------|
| L918 | pt_edit | 前邻 hole 合并 | 在 hole 后面插入、前一 piece 是 hole 且有空间 |
| L909 | pt_edit | ptH_reserve 失败 | OOM，需要 hole 池耗尽 |
| L910 | pt_edit | 内部 pt_remove 失败 | OOM，remove 途中分配失败 |

#### 删除

| 行 | 函数 | 分支 | 如何触发 |
|----|------|------|----------|
| L1164 | ptD_stitch | 零字节尾 piece 清理 | 删除后产生空 piece（整 piece 被删且清空洞） |
| L1175 | ptD_stitch | stitch 后游标边界修正 | 深层 stitch 后游标在最后 piece 之后 |
| L1037 | ptD_backwardnode | `d > i` 跨节点回溯 | stitch 时需跳过超过当前节点容量的 piece |
| L1040 | ptD_backwardnode | 下降循环体 | 同上，回溯后定位到父节点末尾 |
| L1126-1127 | ptD_stitchnode | findroom 链构建 | 深层 stitch 中节点全满，需 makeroom |
| L1220-1223 | ptD_rmleaf | mid literal 裂树 | 全满树中删除字面量中间部分（非边界） |
| L1240 | ptD_cowpaths | COW 共享路径内循环 | 深层 committed 树中同子树跨叶删除 |

#### 事务

| 行 | 函数 | 分支 | 如何触发 |
|----|------|------|----------|
| L685 | pt_commit | arena 预留不足或 literal 不连续 | OOM 或 reserve 被中途打断 |

---

## 提升到 90% 的策略

当前 87.6%，需约 18 个分支。排除 ASSERT+DEAD 后仅剩约 15 个 LOGIC 分支和约 35 个 ERROR 分支。

### 推荐路线（按投入产出排序）

**一、补 LOGIC 分支（~10个，中型投入）**

1. **L1164** — `ptD_stitch` 零字节清理：构造删除后产生空 piece 的场景
2. **L1220-1223** — `ptD_rmleaf` mid literal 裂树：用 treeV 构造全满多层树，从中间位置删1字节
3. **L1037-1040** — `ptD_backwardnode` 跨节点：需要深层 stitch 触发 backtrack
4. **L1126-1127** — `ptD_stitchnode` findroom：深层 stitch 节点全满场景
5. **L1240** — `ptD_cowpaths` 共享路径 COW：深层 committed 树同子树内删除

这些都需要精心构造树形，建议用 treeV DSL 搭特定树 + pt_remove 触发。参考已有测试 `test_remove_findroom`、`test_remove_fold_balance` 等。

**二、补 ERROR 分支（~15个，小投入）**

扩展 `test_error_paths`，补充所有 public API 的 NULL/非法参数调用。每个约 1-2 行。

```c
/* 示例：在 error_paths 末尾补 */
pt_advance(NULL, 0);                     // C==NULL
c.tree = NULL; pt_advance(&c, 1);       // tree==NULL
pt_read(NULL, buf, 1);                  // C==NULL
pt_read(&c, NULL, 1);                   // buf==NULL
pt_piece(NULL, NULL);                   // C==NULL
pt_rollback(NULL);                      // C==NULL
```

**三、补 OOM ERROR 分支（~10个，高投入）**

需要 `oom_alloc` + 精确计数。建议先用 `pt_localfill` 填满池的空闲列表，再让特定分配的 page alloc 失败。参考 `test_insert_oom_reserve` 的模式。

---

## 测试编写技巧

### 覆盖率命令

```bash
just pt-cov             # 编译运行 + 显示覆盖率
just pt-unbranched      # 列出未覆盖分支（排除assert行）
just pt-lines           # 列出未覆盖行源码
just clean-gcda         # 清除 .gcda/.gcno
```

### 树构造 DSL

```c
// 单层叶
treeV(0, leafV(litV("ab"), holeV("cd"), litV("ef")))

// 两层（根→内节点→叶）
treeV(1, innerV(leafV(litV("a"), litV("b")),
                leafV(litV("c"), litV("d"))))

// 三层
treeV(2, innerV(innerV(leafV(litV("a")), leafV(litV("b"))),
                innerV(leafV(litV("c")), leafV(litV("d")))))

// 直接搭树 + 置 dirty
editV(&c, offset, levels, root_expr)
```

### 断言

```c
pt_asserttree(blob, levels, root_expr)  // 精确树比对（优先用）
pt_checktree(blob)                      // 不变式校验
pt_checkcursor(&c, expected_offset)     // 游标不变式
```

### 调试

```c
pt_dumptree(blob, "tag")    // 打印树结构
pt_log("fmt", ...)          // fprintf(stderr, ...)
```

### OOM 注入

```c
int cnt = 1000;
pt_State *S = pt_open(&oom_alloc, &cnt);
pt_seek(&c, pt_empty(S), 0);
cnt = 0;  // 下一次 allocf 调用返回 NULL
assert(pt_insert(&c, "x", 1) == PT_ERRMEM);
```

### localsfill 精确控制池

```c
char buf[512];
pt_localfill(&S->nodes, (void **)&save, buf, count);
// 现在 nodes 池有 count 个空闲对象
```

---

## 备注

- 当前 87.6% = 693/790。若排除 40 个 ASSRET + 10 个 DEAD = 50 个假分支，有效覆盖率 = 693/(790-50) = 93.6%
- 不建议为了强行达到 90% 而写大量 OOM 测试（脆弱、维护成本高）
- GCOV 的 `--rc branch_coverage=1` 模式将 `assert(cond)` 中的 cond 子表达式分别计数，导致大量假阳性
- `test_error_paths` 是集中覆盖 NULL/非法参数的测试，新增这类覆盖直接在此测试中追加即可
