# linecache.h 项目总览

> 供 Agent 快速了解项目全貌。可 grep 查询的信息不载此文，提供查询命令。

## 一、项目概要

单头文件 C89 库，前缀 `lc_`。以 B+ 计量树 (Metric B+ Tree) 维护字节偏移→行号之映射。

**功用**: 文本编辑器中维护行号缓存。支持行断点插入/删除 (`lc_markbreak`/`lc_clearbreaks`)、区间删除/插入 (`lc_splice`)、中部插入 (`lc_insert`)、批量加载 (`lc_scan`)。

关联: `piecetab.h` (piece table, 前缀 `pt_`)，linecache 为其先行实验。

```bash
# 查行数
wc -l linecache.h

# 查测试用例数
grep -c 'X(' tests/lc_test4.c
grep -c 'X(' tests/lc_test8.c
```

编译参数（测试用）: `-DLC_LEAF_FANOUT=4 -DLC_FANOUT=4` 以极小扇出迫树分裂。
`lc_test8` 用默认扇出 62。

## 二、数据结构

### lc_Leaf — 叶节点

```c
typedef struct lc_Leaf {
    unsigned bytes[LC_LEAF_FANOUT];
} lc_Leaf;
```

无 `child_count` — 有效条目数由父 `breaks[i]` 记录。

### lc_Node — 内节点（兼叶层父节点）

```c
struct lc_Node {
    size_t   bytes[LC_FANOUT];    /* 各子树累计字节 */
    size_t   breaks[LC_FANOUT];   /* 各子树累计行数 */
    lc_Node *children[LC_FANOUT]; /* Leaf* 于叶层, Node* 于内层 */
    unsigned short child_count;
};
```

叶层 `children[i]` 实为 `lc_Leaf*`，须 cast。无 parent 指针。

### lc_Cache — B+ 树

```c
struct lc_Cache {
    lc_State      *S;
    lc_Node        root;     /* 嵌入（非指针），永不 OOM */
    size_t         breaks;   /* 总行数 */
    size_t         bytes;    /* 总字节数 */
    unsigned short levels;   /* 0 = root->children 即叶层 */
};
```

### lc_Cursor — 游标

```c
struct lc_Cursor {
    lc_Node **paths[LC_MAX_LEVEL]; /* 根→叶路径槽位指针 */
    lc_Cache  *tree;
    unsigned   col;   /* 行内列偏移 */
    int        lnu;   /* 叶内行索引 */
    size_t     loff;  /* 叶内字节偏移 */
    size_t     nu;    /* 叶起始行号 */
    size_t     off;   /* 叶起始字节偏移 */
};
```

- `paths[0] == &c->root.children[...]`，`paths[levels]` 指向叶槽（内为 `lc_Leaf*` 需 cast）
- 绝对偏移 = off + loff + col; 绝对行号 = nu + lnu
- 非持久 — 由 `lc_seek`/`lc_seekline` 初始化

### lc_Pool / lc_State

池分配器：每页 `LC_PAGE_SIZE`(65536)，freelist 回收。`nodes` 池 (`sizeof(lc_Node)`) 与 `leaves` 池 (`sizeof(lc_Leaf)`) 独立。
`lcP_reserve(n)` 保证池中 ≥ n 个可用对象，用于事务（stitch 入口预分配防 OOM）。

## 三、关键常量

| 符号 | 默认 | lc_test4 值 | 含义 |
|------|------|------------|------|
| `LC_FANOUT` | 62 | 4 | 内节点最大子数 |
| `LC_LEAF_FANOUT` | 62 | 4 | 叶最大行数 |
| `LC_MAX_LEVEL` | 16 | 16 | 最大树深 |
| `LC_PAGE_SIZE` | 65536 | 512 | 池分配器页大小 |

半满阈值 = FANOUT/2 (lc_test4=2, 默认=31)。小扇出极易触发分裂/合并。

## 四、核心游标宏

```c
#define lcK_levels(C) ((C)->tree->levels)
#define lcK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define lcK_idx(C, p, l) ((C)->paths[(l)] - (p)->children)
#define lcK_leaf(C)      (*(lc_Leaf **)(C)->paths[lcK_levels(C)])
```

## 五、公共 API

```bash
# 查完整 API 声明
grep '^LC_API' linecache.h
```

关键 API：

| 类别 | 函数 | 功用 |
|------|------|------|
| 生命周期 | `lc_open`, `lc_close`, `lc_reset` | 状态管理 |
| 树 | `lc_newtree`, `lc_deltree` | 树生命周期 |
| 批量 | `lc_scan` | 批量加载行断点到树尾（可叠加） |
| 查询 | `lc_breaks`, `lc_bytes` | 树级汇总 |
| 定位 | `lc_seek`, `lc_seekline` | 按偏移/行号定位游标 |
| 移动 | `lc_advance`, `lc_advline` | 字节/行偏移移动（越界 clamp） |
| 查询 | `lc_offset`, `lc_line`, `lc_linelen`, `lc_col` | 游标状态查询 |
| 断点 | `lc_markbreak`, `lc_clearbreaks` | 单点插入/区间清除行断 |
| 编辑 | `lc_splice`, `lc_insert` | 区间删插字节 / 中部插入文本 |

## 六、内部函数命名体系

```bash
# 列出所有 static 函数名
grep '^static' linecache.h
```

| 前缀 | 职责 | 代表函数 |
|------|------|----------|
| `lcK_` | 游标导航 | `findleaf`, `findline`, `findinleaf`, `locend`, `forwardoff`, `backwardoff`, `forwardline`, `backwardline` |
| `lcB_` | 行断/插入 | `oneline`, `makeroom`, `splitroot`, `splitchild`, `splitleaf`, `putbreak`, `append`, `cutleaf`, `fixsource`, `rollback` |
| `lcD_` | 删除/平衡 | `trimleft`, `trimright`, `balanceleaf`, `balancenode`, `foldleaf`, `foldnode`, `rebalance`, `spliceleaf`, `splicerange`, `makechain`, `findroom`, `mergeleaf`, `backwardnode`, `stitch`, `stitchnode`, `checkstitch`, `reset` |
| `lcM_` | 度量 | `up` (自底向上传播 bytes/breaks 至根) |
| `lcN_` | 节点操作 | `sumbytes`, `sumbreaks`, `makespace`, `copy`, `move`, `erase`, `freechildren` |
| `lcL_` | 叶操作 | `sumbytes` (宏 `lcL_new`, `lcL_idx`) |
| `lcP_` | 池 | `init`, `destroy`, `alloc`, `free`, `reserve` |

## 七、lc_scan 流程概要

`lc_scan` 从树尾定位游标，循环调用 `lcB_append` 逐叶填充 scanner 输出（一叶填满后跨兄弟叶继续，非逐行插入）。

当 append 填满父节点且当前叶也满时返回 >0，触发扩容：
1. 自底向上寻首个非满层
2. `lcD_makechain` 建空节点链（reserve 预分配，OOM 安全返回 `LC_ERRMEM`）
3. 下轮 append 填入新链

扫完后自顶向下 foldnode→foldleaf→rebalance 平衡树。`i == cc` 状态由 foldleaf 内部消化。

允许多次 `lc_scan` 叠加——后续扫描在已有数据之后追加。

## 八、lcB_makeroom 流程概要

`lcB_makeroom` 在 markbreak 叶满时调用，确保插入行断前有空间：

1. 自底向上遍历，计数需裂层数 `c`
2. `lcP_reserve(S, nodes, c)` 一次预分配全部所需节点
3. 自底向上寻首个非满祖先层。若全满 `l<0` → `lcB_splitroot` 加深树 → `l=1`
4. 自上向下逐层 `lcB_splitchild`，用预分配节点
5. `lcB_splitleaf` 裂叶

`lcP_alloc(NULL, pool)` 的 NULL 参数因 reserve 已保 freelist 充足，永不为 NULL（有 assert 校验）。splitroot / splitchild / splitleaf 均根据游标位置修正 paths[]。

## 九、关键设计决策

1. **嵌入 root**: `lc_Cache.root` 是值非指针 — 省一次分配，免 OOM 于建树
2. **叶无 child_count**: 行数由父 `breaks[i]` 决定，冗余由上层约束保证
3. **度量双计**: bytes + breaks 双数组允许 O(log n) 双向导航
4. **lc_splice void 返回**: 设计保证永不失败 — 删除路径 reserve 预分配，插入路径仅改叶内字节
5. **stitch 事务性**: `lcD_checkstitch` 入口 reserve(levels+2 节点)，stitch 全程无 OOM
6. **balanceleaf rounding**: `d = l - ((l + r + 1) >> 1)` 向上取整，与 foldleaf 游标修正断言强耦合（`assert((dl<0) != (*ls!=o))`）— 修改均分公式需同步改调用方

## 十、测试

```bash
just dbg              # 编译运行 lc_test4 (FANOUT=4, ASAN+UBSAN)
just dbg <prefix>     # 运行名称以 prefix 开头的测试
just dbg8             # 编译运行 lc_test8 (FANOUT=62)
just cov              # 全量覆盖率（编译、运行、lcov 报告、未覆盖列表）
```

测试用例以 `TESTS(X)` 宏列举，scanner 从 `unsigned*` 数组顺序取行长度。`lc_test4` 以极小扇出逼树分裂/合并/旋转变换。

```bash
# 列出全部测试名
grep '^    X(' tests/lc_test4.c
```

## 十一、相关文档

| 文档 | 内容 |
|------|------|
| `notes/design_insert_delete_v2.md` | 当前基于stitch的批量插入/删除设计 |
| `notes/design_bulk_loading.md` | lc_scan Bulk Loading 设计 |
| `notes/design_splice.md` | Splice 区间删除三段法设计 |
| `notes/history_range_delete.md` | 区间删除算法演进史 |
| `notes/brief_tests.md` | 测试结构笔记 |
| `notes/lessons_trimnode_mergenode.md` | trimnode/mergenode 重构教训 |
| `notes/uncovered_branches.md` | 未覆盖分支详细映射 |
| `linecache.md` | 面向用户的 API 参考手册 |

## 十二、编码铁律 (AGENTS.md)

- C89 唯。函数 25/30 行限。签名 ≤79 字。
- static helper 不校验参数 — assert 保不变式。
- 内部函数前缀 `lcX_` (如 `lcK_findleaf`)，X 为单字母分类码。
- **禁止删测试**。先修测试后改代码。
- 修改后必过 `clang-format`。
