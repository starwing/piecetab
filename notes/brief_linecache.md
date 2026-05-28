# linecache.h 项目总览

> 供后续 agent 快速了解项目全貌，无需遍览源码。

## 一、项目概要

**linecache.h** 者，单头文件 C89 库也。以 B+ 计量树为骨，行号缓存为肉。前缀 `lc_`。

**功用**: 于文本编辑器中，维护字节偏移→行号之映射。支持插入/删除行断点 (line breaks)，支持区间字节删除/插入 (`lc_splice`)。

**关联项目**: `piecetab.h` (piece table 文本缓冲区，前缀 `pt_`)，linecache 为其子项目/先行实验。

## 二、文件结构

| 文件 | 行数 | 用途 |
|------|------|------|
| `linecache.h` | 1045 | 单头文件，含全部声明与实现 |
| `tests/lc_test4.c` | -- | 单元测试 (LC_FANOUT=4)，84 测试用例 |
| `tests/lc_test8.c` | -- | 单元测试 (LC_FANOUT=8)，6 测试用例 |
| `tests/lc_tests.h` | -- | 共享测试辅助 (定义宏、构造器、扫描器) |
| `justfile` | -- | 构建命令: `just dbg`, `just dbg_lc8`, `just lc-cov` |

编译参数（测试用）: `-DLC_LEAF_FANOUT=4 -DLC_FANOUT=4` 以极小扇出迫树分裂。

## 三、数据结构

### lc_Leaf (叶子节点)
```c
typedef struct lc_Leaf {
    unsigned bytes[LC_LEAF_FANOUT]; /* 每行之字节长度 */
} lc_Leaf;
```
- 无 `child_count` 字段，有效条目数由父节点 `breaks[i]` 记录
- 测试中 `LC_LEAF_FANOUT=4`

### lc_Node (内部节点 & 叶层父母)
```c
struct lc_Node {
    size_t   bytes[LC_FANOUT];    /* 每子之累计字节数 */
    size_t   breaks[LC_FANOUT];   /* 每子之累计行断点数 */
    lc_Node *children[LC_FANOUT]; /* Leaf* 于叶层, Node* 于内层 */
    unsigned short child_count;   /* 有效子数 (≤ LC_FANOUT) */
};
```
- 叶层时 `children[i]` 实际为 `lc_Leaf*`，须 cast
- 无 parent 指针（与 piecetab 路径法一致）


- 测试中 `LC_FANOUT=4`

### lc_Cache (B+树整体)
```c
struct lc_Cache {
    lc_State      *S;
    lc_Node        root;     /* 嵌入之根节点 */
    size_t         breaks;   /* 总行断点数 */
    size_t         bytes;    /* 总字节数 */
    unsigned short levels;   /* 树深: 0 = root->children 为叶 */
};
```
- `root` 嵌入而非指针 — 无分配则不会 OOM
- `levels=0` 表示 root 之子即叶层

### lc_Cursor (游标)
```c
struct lc_Cursor {
    lc_Node **paths[LC_MAX_LEVEL]; /* 自根至叶之路径槽位指针 */
    lc_Cache  *tree;
    size_t     off;     /* 当前叶起始字节偏移 */
    size_t     idx;     /* 当前行之行号索引 */
    size_t     loff;    /* 当前叶内字节偏移 */
    unsigned   col;     /* 当前行内列偏移 */
    unsigned short lidx; /* 当前叶内行索引 */
};
```
- `paths[0] == &c->root.children[...]`, `paths[levels]` 指向叶槽
- 非持久 — 由 `lc_seek`/`lc_seekline` 初始化

### lc_Pool / lc_State
```c
struct lc_Pool { size_t obj_size; void *freed; void *pages; };
struct lc_State { void *alloc_ud; lc_Alloc *allocf; lc_Pool nodes; lc_Pool leaves; };
```
- 池分配器：每页 `LC_PAGE_SIZE=65536`，空闲链表回收
- `nodes` 池分配 `sizeof(lc_Node)`，`leaves` 池分配 `sizeof(lc_Leaf)`

## 四、关键常量 (测试模式)

| 符号 | 默认 | 测试值 | 含义 |
|------|------|--------|------|
| `LC_FANOUT` | 62 | **4** | 内节点最大子数 |
| `LC_LEAF_FANOUT` | 62 | **4** | 叶最大行数 |
| `LC_MAX_LEVEL` | 16 | 16 | 最大树深 |

半满阈值 = FANOUT/2 = 2，极易触发分裂/合并。

## 五、核心游标宏

```c
#define lcK_levels(C) ((C)->tree->levels)
#define lcK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define lcK_idx(C, p, l) ((C)->paths[(l)] - (p)->children)
#define lcK_leaf(C)      (*(lc_Leaf **)(C)->paths[lcK_levels(C)])
```

## 六、公共 API

| 函数 | 签名 | 功能 |
|------|------|------|
| `lc_open` | `lc_State *lc_open(lc_Alloc *allocf, void *ud)` | 创建状态 |
| `lc_close` | `void lc_close(lc_State *S)` | 销毁状态 |
| `lc_newtree` | `lc_Cache *lc_newtree(lc_State *S)` | 建新树 |
| `lc_deltree` | `void lc_deltree(lc_State *S, lc_Cache *c)` | 删树 |
| `lc_breaks` | `size_t lc_breaks(const lc_Cache *c)` | 总行断点数 |
| `lc_bytes` | `size_t lc_bytes(const lc_Cache *c)` | 总字节数 |
| `lc_scan` | `int lc_scan(lc_Cache *c, lc_Scanner *scanner, void *ud)` | 批量加载行断点到树尾 |
| `lc_seek` | `int lc_seek(lc_Cursor *C, lc_Cache *c, size_t pos)` | 按字节偏移定位 |
| `lc_seekline` | `int lc_seekline(lc_Cursor *C, lc_Cache *c, size_t line)` | 按行号定位 |
| `lc_advance` | `int lc_advance(lc_Cursor *C, lc_Diff delta)` | 字节偏移移动 |
| `lc_advline` | `int lc_advline(lc_Cursor *C, lc_Diff delta)` | 行偏移移动 |
| `lc_offset` | `size_t lc_offset(const lc_Cursor *C)` | 游标字节偏移 |
| `lc_line` | `size_t lc_line(const lc_Cursor *C)` | 游标行号 |
| `lc_linelen` | `unsigned lc_linelen(const lc_Cursor *C)` | 当前行长 |
| `lc_col` | `unsigned lc_col(const lc_Cursor *C)` | 当前列 |
| `lc_markbreak` | `int lc_markbreak(lc_Cursor *C, unsigned br)` | 单点插行断 |
| `lc_markbreaks` | `int lc_markbreaks(lc_Cursor *C, lc_Scanner *scanner, void *ud)` | 批量插行断 (逐调 markbreak) |
| `lc_clearbreaks` | `int lc_clearbreaks(lc_Cursor *C, size_t len)` | 区间清行断 |
| `lc_splice` | `void lc_splice(lc_Cursor *C, size_t del, size_t ins)` | 区间删/插字节 |

## 七、内部函数命名体系

| 前缀 | 职责 | 主要函数 |
|------|------|----------|
| `lcK_` | 游标导航 | `findleaf`, `findline`, `findinleaf`, `locend`, `forwardoff`, `backwardoff`, `forwardline`, `backwardline` |
| `lcB_` | 行断插入 | `initempty`, `makeroom`, `splitroot`, `splitchild`, `splitleaf`, `fitleaf`, `putbreak` |
| `lcD_` | 区间删除 | `spliceleaf`, `splicerange`, `trimleaf`, `trimnode`, `shiftleaf`, `shiftnode`, `foldleaf`, `foldnode`, `rebalance`, `prune`, `balanceleaf`, `balancenode`, `emptytree`, `freerange` |
| `lcM_` | 度量更新 | `up` (自底向上更新度量), `tx` (跨父度量转移) |
| `lcN_` | 节点操作 | `makespace`, `copy`, `move`, `sumbytes`, `sumbreaks`, `freechildren` |
| `lcL_` | 叶操作 | `sumbytes` |

## 八、`lc_scan` 当前实现 (行1020-1041)

```c
LC_API int lc_scan(lc_Cache *c, lc_Scanner *scanner, void *ud) {
    lc_Cursor C;
    unsigned  br;
    int       r;
    if (c == NULL || scanner == NULL) return LC_ERRPARAM;
    C.tree = c;
    if (c->root.child_count == 0) {
        if ((br = scanner(ud, c->bytes)) == 0) return LC_OK;
        if ((r = lcB_initempty(&C, br)) < 0) return r;
    }
    lcK_locend(&C);
    while ((br = scanner(ud, c->bytes)) > 0) {
        lc_Leaf *wr_leaf;
        unsigned wr_lidx;
        if ((r = lcB_fitleaf(&C)) < 0) return r;
        wr_leaf = lcK_leaf(&C), wr_lidx = C.lidx;
        wr_leaf->bytes[wr_lidx] = br;
        C.loff += br, C.lidx++, C.idx++, lcM_up(&C, lcK_levels(&C), br, 1);
    }
    C.col = 0;
    return LC_OK;
}
```

**问题**: 逐条插入，叶满时 `lcB_fitleaf` → `lcB_makeroom` → 裂叶取中点 (50%/50%)，左半永不动，填充率仅 ~50%。

## 九、`lcB_makeroom` (自底向上被动分裂, 行937-959)

六相算法: 计数需裂层数 → 预分配全部节点 → 寻首个非满祖先 → 自上向下逐层裂 → 裂叶 → 释未用节点。

## 十、测试框架

```bash
just dbg            # 编译 + 运行 lc_test4 (gcc -O0 -fsanitize=address,undefined)
just dbg <test>     # 运行特定测试
just dbg_lc8        # 编译 + 运行 lc_test8 (gcc -O0 -fsanitize=address,undefined)
just dbg_lc8 <test> # 运行特定测试
just lc-cov         # 覆盖率 (目标 100%)
```

测试以 `TESTS(X)` 宏列举，`scanner(void *ud, size_t prev)` 回调从 `unsigned*` 数组顺序取换行位 (忽略 prev)。

## 十一、相关设计文档

| 文档 | 内容 |
|------|------|
| [design_bulk_loading.md](design_bulk_loading.md) | lc_scan Bulk Loading 实现设计 |
| [research_bulk_loading.md](research_bulk_loading.md) | B+ 树 Bulk Loading 算法理论 (Wikipedia) |
| [design_marktree.md](design_marktree.md) | Mark Tree 设计 (B+树 + gap 编码 + 哈希映射) |
| [design_splice.md](design_splice.md) | Splice 区间删除三段法设计 (当前实现) |
| [history_range_delete.md](history_range_delete.md) | 区间删除算法五代演进史 |
| [research_piecetab_insert.md](research_piecetab_insert.md) | piecetab insert 算法分析 |
| [research_piecetab_splitpiece.md](research_piecetab_splitpiece.md) | piecetab splitpiece 源码分析 |
| [brief_tests.md](brief_tests.md) | 测试结构笔记 |
| [lessons_trimnode_mergenode.md](lessons_trimnode_mergenode.md) | trimnode/mergenode 重构教训 |

## 十二、编码规范 (CLAUDE.md)

- **C89 唯** — 禁 C99/C11 特性
- **函数 25 行软限 / 30 行硬限**
- **函数签名不逾 79 字**
- **static helper 不设防御性参数校验** — 用断言保不变式
- **禁删测试**
- **先修测试后改代码** — 单测出现任何问题，放弃计划修复后再继续
- **修改后必过 clang-format**
