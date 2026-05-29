# piecetab 代码库研究结果

> 生成日期：2026-05-29
> 探索目标：了解 piecetab/linecache 项目整体结构、源文件、关键数据结构与函数

---

## 一、所有源文件列表

### 1.1 主要头文件（含实现）

| 文件 | 绝对路径 | 行数 | 说明 |
|------|----------|------|------|
| `linecache.h` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/linecache.h` | 1291 | 单头文件 C89 库，B+ 计量树实现，行号缓存 |
| `piecetab.h` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/piecetab.h` | 1270 | piece table 文本缓冲区，前缀 `pt_`，与 linecache 相关联 |

### 1.2 测试文件

| 文件 | 绝对路径 | 说明 |
|------|----------|------|
| `lc_tests.h` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/tests/lc_tests.h` | 共享测试辅助（宏、构造器、扫描器、校验器） |
| `lc_test4.c` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/tests/lc_test4.c` | 单元测试 (LC_FANOUT=4)，包含 `lc_insert` 等 84+ 测试用例 |
| `lc_test8.c` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/tests/lc_test8.c` | 单元测试 (LC_FANOUT=8)，6 测试用例 |

### 1.3 设计文档与笔记 (.md)

| 文件 | 绝对路径 | 说明 |
|------|----------|------|
| `README.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/README.md` | 项目说明 |
| `TODO.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/TODO.md` | 待办事项 |
| `brief_linecache.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/brief_linecache.md` | linecache 项目总览（数据结构、API、内部函数体系） |
| `brief_tests.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/brief_tests.md` | 测试编写指南 |
| `design_splice.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/design_splice.md` | splice 区间删除设计 |
| `design_bulk_loading.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/design_bulk_loading.md` | lc_scan Bulk Loading 实现设计 |
| `design_marktree.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/design_marktree.md` | Mark Tree 设计 |
| `research_bulk_loading.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/research_bulk_loading.md` | Bulk Loading 算法理论 |
| `research_piecetab_insert.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/research_piecetab_insert.md` | piecetab insert 算法分析 |
| `research_piecetab_splitpiece.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/research_piecetab_splitpiece.md` | piecetab splitpiece 源码分析 |
| `history_range_delete.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/history_range_delete.md` | 区间删除算法五代演进史 |
| `lessons_trimnode_mergenode.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/notes/lessons_trimnode_mergenode.md` | trimnode/mergenode 重构教训 |
| `feat_lc_insert.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/.opencode/plans/feat_lc_insert.md` | lc_insert 实现计划 |
| `refactor_lcB_bulk.md` | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/.opencode/plans/refactor_lcB_bulk.md` | lcB_Ctx 重构计划 |
| 其他计划文档 | `/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/.opencode/plans/` | refactor_scan_unify_v1/v2, feat_splice_v6, feat_bottomup_insert 等 |

### 1.4 其他

| 文件 | 说明 |
|------|------|
| `.claude/CLAUDE.md` | Claude 编码规范（C89、函数行数限制等） |
| `.github/copilot-instructions.md` | GitHub Copilot 指令 |

### 1.5 注意

- 项目无 `.c` 文件（除测试外）。`linecache.h` 与 `piecetab.h` 均为单头文件库（single-header library），声明与实现在同一文件内，由 `#ifdef LC_IMPLEMENTATION` / `#ifdef PT_IMPLEMENTATION` 控制。

---

## 二、lcB_Ctx 的定义位置和完整代码

### 2.1 定义位置

**文件**：`/Users/sw/Library/CloudStorage/SynologyDrive-Home/Work/Code/piecetab/linecache.h`
**行号**：第 1007–1015 行

### 2.2 完整代码

```c
typedef struct lcB_Ctx {
    lc_Cursor c;          /* tree-end cursor: tree/paths/metrics inside */
    lc_Node   pend[LC_MAX_LEVEL];
    lc_Node  *pend_root;  /* lazy-allocated spare node for root split */
    lc_Leaf  *rt_leaf;    /* right split leaf from splitleafat */
    size_t    rt_bytes;   /* byte width of right split leaf */
    unsigned  rt_breaks;  /* line count of right split leaf */
    int       at_end;     /* 1=append (lc_scan), 0=insert (lc_insert) */
} lcB_Ctx;
```

### 2.3 各字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| `c` | `lc_Cursor` | 游标，记录树位置 (tree/paths/off/idx/lidx/col) |
| `pend[LC_MAX_LEVEL]` | `lc_Node[16]` | 待定节点数组，每层级的暂存缓冲区 |
| `pend_root` | `lc_Node*` | 惰性分配的根分裂备用节点 |
| `rt_leaf` | `lc_Leaf*` | 由 `lcB_splitleafat` 产生的右半裂叶 |
| `rt_bytes` | `size_t` | 右裂叶的字节宽度 |
| `rt_breaks` | `unsigned` | 右裂叶的行断点数 |
| `at_end` | `int` | **1=末尾追加**（`lc_scan` 走），**0=中间插入**（`lc_insert` 走） |

### 2.4 lcB_Ctx 的所有引用位置

| 行号 | 函数 | 说明 |
|------|------|------|
| 1007 | (typedef) | 定义 |
| 1017 | `lcB_checkpendroot` | 参数类型 `lcB_Ctx *x` |
| 1023 | `lcB_init` | 参数类型 + memset 初始化 `sizeof(lcB_Ctx)` |
| 1033 | `lcB_merge` | 参数类型 |
| 1060 | `lcB_rootpush` | 参数类型 |
| 1080 | `lcB_fill` | 参数类型 |
| 1103 | `lcB_flush` | 参数类型 |
| 1127 | `lcB_fillflush` | 参数类型 |
| 1142 | `lc_scan` | 局部变量 `lcB_Ctx x;` |
| 1153 | `lcB_initat` | 参数类型 + memset |
| 1159 | `lcB_splitleafat` | 参数类型 |
| 1186 | `lcB_pushrt` | 参数类型 |
| 1194 | `lcB_packleafs` | 参数类型 |
| 1223 | `lcB_applyfirst` | 参数类型 |
| 1259 | `lc_insert` | 局部变量 `lcB_Ctx x;` |

---

## 三、at_end 的所有使用位置和上下文

### 3.1 定义

**文件**：`linecache.h`，第 1014 行

```c
int       at_end;    /* 1=append (lc_scan), 0=insert (lc_insert) */
```

### 3.2 赋值

**位置 1**：`lcB_init` 函数，第 1025 行

```c
static int lcB_init(lcB_Ctx *x, lc_Cache *c) {
    memset(x, 0, sizeof(lcB_Ctx));
    x->c.tree = c, x->at_end = 1;     // <-- 此处设为 1（末尾追加模式）
    if (c->root.child_count > 0)
        lcK_locend(&x->c);
    else
        x->c.paths[0] = &c->root.children[0];
    return LC_OK;
}
```

**位置 2**：`lcB_initat` 函数，第 1153–1157 行（注意：此处**未显式设置** at_end）

```c
static int lcB_initat(lcB_Ctx *x, lc_Cursor *C) {
    memset(x, 0, sizeof(lcB_Ctx));    // <-- memset 零化，故 at_end 默认 = 0
    x->c = *C;
    return LC_OK;
}
```

### 3.3 读取使用

**位置**：`lcB_flush` 函数，第 1109–1110 行

```c
at = x->at_end ? (int)parent->child_count
               : (int)(x->c.paths[l] - parent->children) + 1;
```

**语义**：
- 若 `at_end == 1`（末尾追加）：插入位置 = 父节点末位（`child_count`），即追加到尾
- 若 `at_end == 0`（中间插入）：插入位置 = 游标当前位置 + 1（paths[l] 在 children 中的下标 + 1），即插入到游标右侧

### 3.4 调用链

```
lc_scan → lcB_init → at_end=1 → lcB_fillflush → lcB_flush (at=child_count)
lc_insert → lcB_initat → at_end=0 → lcB_fillflush → lcB_flush (at=paths+1)
           (末尾退化) → lcB_init → at_end=1  (same as lc_scan)
```

---

## 四、lc_insert 的完整实现代码

### 4.1 声明

**文件**：`linecache.h`，第 114 行

```c
LC_API int lc_insert(lc_Cursor *C, int e, lc_Scanner *scanner, void *ud);
```

### 4.2 完整实现

**文件**：`linecache.h`，第 1258–1287 行

```c
LC_API int lc_insert(lc_Cursor *C, int e, lc_Scanner *scanner, void *ud) {
    lcB_Ctx   x;
    lc_Cache *c;
    unsigned  br;
    size_t    old_off, old_bytes;
    int       r = 0, lv, trailing, i;
    if (C == NULL || (c = C->tree) == NULL || scanner == NULL)
        return LC_ERRPARAM;
    old_off = lc_offset(C), old_bytes = c->bytes;
    trailing = (old_off >= old_bytes || c->root.child_count == 0);
    if (!trailing && !(br = scanner(ud, c->bytes))) return lcB_skipinsert(C, e);
    if ((r = trailing ? lcB_init(&x, c) : lcB_initat(&x, C)) != LC_OK) return r;
    if (!trailing && (r = lcB_applyfirst(&x, br, e)) < 0) return r;
    for (lv = (int)c->levels, r = lcB_fill(&x, lv, scanner, ud); r > 0;
         lv = (int)c->levels, r = lcB_fill(&x, lv, scanner, ud)) {
        if ((r = lcB_checkpendroot(&x)) != LC_OK) break;
        if ((r = lcB_flush(&x, lv)) != LC_OK) break;
    }
    if (r >= 0)
        r = x.rt_leaf ? (lcB_pushrt(&x, lv),
                         lcB_packleafs(&x, lv) ? LC_OK : lcB_flush(&x, lv))
                      : lcB_flush(&x, lv);
    if (x.pend_root) lc_poolfree(&c->S->nodes, x.pend_root);
    if (r != LC_OK) {
        for (i = 0; i <= lv; ++i) lcN_freechildren(c->S, &x.pend[i], lv - i);
        return r;
    }
    lc_seek(C, c, old_off + (c->bytes - old_bytes));
    return (trailing ? (C->col += (unsigned)e) : 0), LC_OK;
}
```

### 4.3 流程详解

```
lc_insert(C, e, scanner, ud)
│
├─ 参数校验
│   C != NULL && C->tree != NULL && scanner != NULL
│
├─ 判断 trailing（末尾退化）
│   trailing = old_off >= old_bytes || root.child_count == 0
│
├─ 扫描第一个 break
│   if (!trailing && scanner()==0)
│     → lcB_skipinsert(C, e)  // 无新换行，仅调整当前行长
│
├─ 初始化 lcB_Ctx
│   trailing ? lcB_init(&x, c)    // 末尾模式 (at_end=1)
│            : lcB_initat(&x, C)  // 中间模式 (at_end=0)
│
├─ 应用第一个 break
│   if (!trailing) lcB_applyfirst(&x, br, e)
│     ├─ lcB_splitleafat(x)   → 游标处裂叶
│     ├─ col>0 → 当前行增长 br 字节
│     └─ col==0 → 分配新叶，存第一个 break
│
├─ fill-flush 循环（B+ 树 bulk loading）
│   while (lcB_fill(...) > 0)
│     lcB_checkpendroot(x)
│     lcB_flush(x, lv)
│
├─ 最终 flush
│   有 rt_leaf → lcB_pushrt + lcB_packleafs/lcB_flush
│   无 rt_leaf → lcB_flush
│
├─ 清理
│   释放 pend_root
│   失败时释放 pend[] 中已分配节点
│
└─ 回位
    lc_seek(C, c, old_off + delta_bytes)
    末尾模式下 C->col += e
```

### 4.4 辅助函数 lcB_skipinsert（第 1249–1256 行）

```c
static int lcB_skipinsert(lc_Cursor *C, int e) {
    lc_Node *p = lcK_parent(C, lcK_levels(C));
    if (e > 0 && C->lidx < (int)p->breaks[lcK_idx(C, p, lcK_levels(C))])
        lcK_leaf(C)->bytes[C->lidx] += (unsigned)e,
                lcM_up(C, lcK_levels(C), (lc_Diff)e, 0);
    C->col += (unsigned)e;
    return LC_OK;
}
```

当 scanner 返回 0（无新换行符）时，仅需将 e 字节加到当前行长度，无需插入任何行断点。

---

## 五、其他相关函数实现概要

### 5.1 lc_scan — 批量扫描构建行缓存

**位置**：`linecache.h` 第 1141–1149 行

```c
LC_API int lc_scan(lc_Cache *c, lc_Scanner *scanner, void *ud) {
    lcB_Ctx x;
    int     r;
    if (c == NULL || scanner == NULL) return LC_ERRPARAM;
    if ((r = lcB_init(&x, c)) != LC_OK) return r;
    r = lcB_fillflush(&x, scanner, ud, (int)c->levels);
    if (x.pend_root) lc_poolfree(&c->S->nodes, x.pend_root);
    return r;
}
```

**概要**：空树时 bulk load 全部行断点到树末尾，非空时追加扫描。调用链：`lcB_init` → `lcB_fillflush`。

### 5.2 lcB_init — 初始化 lcB_Ctx（末尾模式）

**位置**：第 1023–1031 行

```c
static int lcB_init(lcB_Ctx *x, lc_Cache *c) {
    memset(x, 0, sizeof(lcB_Ctx));
    x->c.tree = c, x->at_end = 1;
    if (c->root.child_count > 0)
        lcK_locend(&x->c);
    else
        x->c.paths[0] = &c->root.children[0];
    return LC_OK;
}
```

**要点**：`at_end=1`，游标定位到树末；空树则 paths[0] 指向 root 首个子位。

### 5.3 lcB_initat — 初始化 lcB_Ctx（插入模式）

**位置**：第 1153–1157 行

```c
static int lcB_initat(lcB_Ctx *x, lc_Cursor *C) {
    memset(x, 0, sizeof(lcB_Ctx));
    x->c = *C;
    return LC_OK;
}
```

**要点**：`at_end=0`（由 memset 零化），拷贝游标路径。游标位置即插入点。

### 5.4 lcB_fill — 填充一层 pend 节点

**位置**：第 1080–1101 行

```c
static int lcB_fill(lcB_Ctx *x, int l, lc_Scanner *sc, void *ud) {
    lc_Node *pend = &x->pend[l];
    size_t   sum, bytes = x->c.tree->bytes;
    while ((int)pend->child_count < LC_FANOUT) {
        lc_Leaf *lf;
        unsigned br, i;
        lf = (lc_Leaf *)lc_poolalloc(x->c.tree->S, &x->c.tree->S->leaves);
        if (!lf) return LC_ERRMEM;
        memset(lf, 0, sizeof(lc_Leaf));
        for (sum = i = 0; i < LC_LEAF_FANOUT; ++i) {
            if ((br = sc(ud, bytes)) == 0) break;
            lf->bytes[i] = br, sum += br, bytes += br;
        }
        if (i == 0) return (lc_poolfree(&x->c.tree->S->leaves, lf), 0);
        pend->children[pend->child_count] = (lc_Node *)lf;
        pend->bytes[pend->child_count] = sum;
        pend->breaks[pend->child_count] = i;
        pend->child_count++;
        if (i < LC_LEAF_FANOUT) return 0;
    }
    return 1;
}
```

**概要**：扫描生成满叶（LC_LEAF_FANOUT 条记录），存入 pend[l] 待定节点，直到满或扫描器返回 0。返回值：>0 表示 pend 满仍需继续，0 表示扫描完毕，<0 表示 OOM。

### 5.5 lcB_flush — 将 pend 节点合并入树

**位置**：第 1103–1125 行

```c
static int lcB_flush(lcB_Ctx *x, int l) {
    lc_State *S = x->c.tree->S;
    for (; l >= 0; --l) {
        lc_Node *parent = lcK_parent(&x->c, l), *pend = &x->pend[l], *n;
        int      at, pi;
        if (pend->child_count == 0) continue;
        at = x->at_end ? (int)parent->child_count
                       : (int)(x->c.paths[l] - parent->children) + 1;
        if (parent->child_count + pend->child_count <= LC_FANOUT) {
            lcB_merge(x, l, at, NULL);
            return LC_OK;
        }
        if (!(n = (lc_Node *)lc_poolalloc(S, &S->nodes))) return LC_ERRMEM;
        memset(n, 0, sizeof(lc_Node));
        lcB_merge(x, l, at, n);
        if (l == 0) return lcB_rootpush(x, n);
        pi = x->pend[l - 1].child_count++;
        x->pend[l - 1].children[pi] = n;
        x->pend[l - 1].bytes[pi] = lcN_sumbytes(n, 0, (int)n->child_count);
        x->pend[l - 1].breaks[pi] = lcN_sumbreaks(n, 0, (int)n->child_count);
    }
    return LC_OK;
}
```

**概要**：自底向上将 pend 层数据合并入树。若父节点容得下则直接合并；否则溢出→分配新节点→合并部分→向上传播。

### 5.6 lcB_fillflush — fill+flush 循环

**位置**：第 1127–1139 行

```c
static int lcB_fillflush(lcB_Ctx *x, lc_Scanner *sc, void *ud, int lv) {
    lc_Cache *c = x->c.tree;
    int       r = lcB_fill(x, lv, sc, ud), i;
    while (r > 0) {
        if ((r = lcB_checkpendroot(x)) != LC_OK) break;
        if ((r = lcB_flush(x, lv)) != LC_OK) break;
        lv = (int)c->levels, r = lcB_fill(x, lv, sc, ud);
    }
    if (r >= 0) r = lcB_flush(x, lv);
    if (r != LC_OK)
        for (i = 0; i <= lv; ++i) lcN_freechildren(c->S, &x->pend[i], lv - i);
    return r;
}
```

**概要**：fill→flush 循环。每当 pend 层满就 flush 到树，flush 可能导致 root push（树深+1），然后继续 fill。

### 5.7 lcB_splitleafat — 在游标处裂叶

**位置**：第 1159–1184 行

```c
static int lcB_splitleafat(lcB_Ctx *x) {
    lc_Cursor *C = &x->c;
    int        lv = lcK_levels(C);
    lc_Node   *p = lcK_parent(C, lv);
    int        li = lcK_idx(C, p, lv), count = (int)p->breaks[li];
    int        n = count - C->lidx, i, dl;
    lc_Leaf   *lf = (lc_Leaf *)p->children[li], *rt;
    lc_Diff    db;
    if (n <= 0) return 0;
    if (!(rt = lc_poolalloc(C->tree->S, &C->tree->S->leaves))) return -1;
    if (C->col) {
        unsigned orig = lf->bytes[C->lidx];
        rt->bytes[0] = orig - C->col;
        for (i = 1; i < n; ++i) rt->bytes[i] = lf->bytes[C->lidx + i];
        lf->bytes[C->lidx] = C->col, p->breaks[li] = C->lidx + 1;
        db = (lc_Diff)(orig - C->col + lcL_sumbytes(lf, C->lidx + 1, count));
        dl = n - 1;
    } else {
        for (i = 0; i < n; ++i) rt->bytes[i] = lf->bytes[C->lidx + i];
        p->breaks[li] = (size_t)C->lidx;
        db = (lc_Diff)lcL_sumbytes(lf, C->lidx, count), dl = n;
    }
    x->rt_leaf = rt, x->rt_bytes = (size_t)db, x->rt_breaks = (unsigned)n;
    p->bytes[li] -= (size_t)db;
    return lcM_up(C, lv - 1, -db, -(lc_Diff)dl), n;
}
```

**概要**：在游标位置将叶一分为二。col>0（行内）则将一行拆为两段；col==0（行首）则从该行起全移至右叶。结果存入 `x->rt_leaf`、`x->rt_bytes`、`x->rt_breaks`。

### 5.8 lcB_applyfirst — 应用第一个 break

**位置**：第 1223–1247 行

```c
static int lcB_applyfirst(lcB_Ctx *x, unsigned br, int e) {
    lc_Cursor *C = &x->c;
    int        lv = lcK_levels(C), n = lcB_splitleafat(x);
    lc_Node   *p = lcK_parent(C, lv);
    int        li = lcK_idx(C, p, lv);
    lc_Leaf   *lf = (lc_Leaf *)p->children[li], *nr;

    assert(n > 0);
    if (n < 0) return n;
    if (C->col)
        lf->bytes[C->lidx] += br, p->bytes[li] += br,
                lcM_up(C, lv - 1, (lc_Diff)br, 0);
    else {
        lc_Node *pend = &x->pend[lv];
        if (!(nr = lc_poolalloc(C->tree->S, &C->tree->S->leaves)))
            return LC_ERRMEM;
        nr->bytes[0] = br;
        pend->children[0] = (lc_Node *)nr;
        pend->bytes[0] = br, pend->breaks[0] = 1;
        pend->child_count = 1;
    }
    x->rt_leaf->bytes[x->rt_breaks - 1] += (unsigned)e,
            x->rt_bytes += (size_t)e;
    return LC_OK;
}
```

**概要**：插入模式下，先裂叶再处理首个 break。若 col>0 在当前行中追加；若 col==0 则分配新叶放入 pend。

### 5.9 lcB_merge — 合并 pend 到父节点

**位置**：第 1033–1058 行

```c
static void lcB_merge(lcB_Ctx *x, int l, int at, lc_Node *next) {
    lc_Node *parent = lcK_parent(&x->c, l);
    lc_Node *pend = &x->pend[l];
    int      space = LC_FANOUT - (int)parent->child_count;
    int      n = lc_min(space, (int)pend->child_count);
    int      pcc = (int)parent->child_count;
    lc_Diff  db, dl;
    if (at < pcc) lcN_makespace(parent, (unsigned)at, (unsigned)n);
    lcN_copy(parent, at < pcc ? (unsigned)at : (unsigned)pcc, pend, 0, (unsigned)n);
    parent->child_count = (unsigned short)(pcc + n);
    x->c.paths[l] = &parent->children[(at < pcc ? at : pcc) + n - 1];
    db = (lc_Diff)lcN_sumbytes(pend, 0, n);
    dl = (lc_Diff)lcN_sumbreaks(pend, 0, n);
    if (l > 0)
        lcM_up(&x->c, l - 1, db, dl);
    else
        x->c.tree->bytes += (size_t)db, x->c.tree->breaks += (size_t)dl;
    if (next && n < (int)pend->child_count) {
        int rest = (int)pend->child_count - n;
        lcN_copy(next, 0, pend, n, (unsigned)rest);
        next->child_count = (unsigned short)rest;
    }
    pend->child_count = 0;
}
```

**概要**：将 pend[l] 中的子节点（最多 space 个）复制到父节点 at 位置。若 pend 有剩余则写入 next 节点（溢出）。更新度量。

### 5.10 lcB_rootpush — 新增根层

**位置**：第 1060–1078 行

```c
static int lcB_rootpush(lcB_Ctx *x, lc_Node *nr) {
    lc_Cache *c = x->c.tree;
    int       l = (int)c->levels, i;
    lc_Node  *nl = (assert(x->pend_root != NULL), x->pend_root);
    if (l + 1 >= LC_MAX_LEVEL) return LC_ERRPARAM;
    i = (int)(x->c.paths[0] - c->root.children);
    *nl = c->root;
    c->root.children[0] = nl, c->root.children[1] = nr;
    c->root.child_count = 2;
    c->root.bytes[0] = c->bytes, c->root.breaks[0] = c->breaks;
    c->root.bytes[1] = lcN_sumbytes(nr, 0, (int)nr->child_count);
    c->root.breaks[1] = lcN_sumbreaks(nr, 0, (int)nr->child_count);
    c->bytes += c->root.bytes[1], c->breaks += c->root.breaks[1];
    memmove(x->c.paths + 1, x->c.paths, (size_t)(l + 1) * sizeof(lc_Node **));
    x->c.paths[0] = &c->root.children[i >= (int)nl->child_count];
    x->c.paths[1] = &nl->children[i];
    c->levels++, x->pend_root = NULL;
    return (x->pend[c->levels].child_count = 0), LC_OK;
}
```

**概要**：树深+1。旧 root 下移为左子，新节点 nr 为右子，创建新根。在 flush 溢出到根时调用。

### 5.11 lcB_packleafs — 合并右裂叶到父叶

**位置**：第 1194–1221 行

```c
static int lcB_packleafs(lcB_Ctx *x, int lv) {
    lc_Node *parent = lcK_parent(&x->c, lv);
    lc_Node *pend = &x->pend[lv];
    int      li, total, start;
    unsigned k, pi;
    lc_Leaf *lf;
    size_t   sum;
    if (pend->child_count == 0) return 1;
    if (parent->child_count == 0) return 0;
    li = lcK_idx(&x->c, parent, lv), lf = (lc_Leaf *)parent->children[li];
    start = (int)parent->breaks[li];
    for (total = start, pi = 0; pi < pend->child_count; ++pi)
        total += (int)pend->breaks[pi];
    if (total > LC_LEAF_FANOUT) return 0;
    /* 合并到当前叶 */
    for (pi = 0; pi < pend->child_count; ++pi) {
        lc_Leaf *src = (lc_Leaf *)pend->children[pi];
        for (k = 0; k < pend->breaks[pi]; ++k)
            lf->bytes[start++] = src->bytes[k];
        lc_poolfree(&x->c.tree->S->leaves, pend->children[pi]);
    }
    pend->child_count = 0;
    for (sum = 0, k = 0; k < (unsigned)total; ++k) sum += lf->bytes[k];
    x->c.tree->bytes += sum - parent->bytes[li];
    parent->bytes[li] = sum;
    x->c.tree->breaks += (size_t)(total - (int)parent->breaks[li]);
    parent->breaks[li] = (size_t)total;
    return 1;
}
```

**概要**：尝试将 pend 中的叶（包括右裂叶 rt_leaf）合并入游标所在的左叶。仅当总条目数 <= LC_LEAF_FANOUT 时可合并，否则走正常 flush 路径。

### 5.12 lcB_checkpendroot — 惰性分配 pend_root

**位置**：第 1017–1021 行

```c
static int lcB_checkpendroot(lcB_Ctx *x) {
    if (x->pend_root) return LC_OK;
    x->pend_root = (lc_Node *)lc_poolalloc(x->c.tree->S, &x->c.tree->S->nodes);
    return x->pend_root ? LC_OK : LC_ERRMEM;
}
```

### 5.13 lcB_pushrt — 将右裂叶推入 pend

**位置**：第 1186–1192 行

```c
static void lcB_pushrt(lcB_Ctx *x, int lv) {
    lc_Node *pend = &x->pend[lv];
    int      pi = (int)pend->child_count++;
    pend->children[pi] = (lc_Node *)x->rt_leaf;
    pend->bytes[pi] = x->rt_bytes, pend->breaks[pi] = x->rt_breaks;
    x->rt_leaf = NULL;
}
```

---

## 六、lc_initpool 函数（与 lc_init 相似）

用户提及 "lc_init"，本项目中最相近者为 `lc_initpool`（无单独的 `lc_init` 函数，亦无 `lc_line_start`）。

**位置**：`linecache.h` 第 192–196 行

```c
static void lc_initpool(lc_Pool *pool, size_t obj_size) {
    memset(pool, 0, sizeof(lc_Pool));
    pool->obj_size = obj_size;
    assert(obj_size > sizeof(void *) && obj_size < LC_PAGE_SIZE / 4);
}
```

**调用**：
- `lc_open` → 初始化 `S->nodes` 和 `S->leaves` 两池（第 240–241 行）
- `lc_freepool` → 释放页面链后重新初始化池（第 210 行）

---

## 七、内部函数命名体系总览

| 前缀 | 职责 | 主要函数 |
|------|------|----------|
| `lcK_` | 游标导航 | `findleaf`, `findline`, `findinleaf`, `locend`, `forwardoff`, `backwardoff`, `forwardline`, `backwardline` |
| `lcB_` | 行断批量插入 | `init`, `initat`, `fill`, `flush`, `fillflush`, `merge`, `rootpush`, `checkpendroot`, `splitleafat`, `pushrt`, `packleafs`, `applyfirst`, `skipinsert`, `initempty`, `splitroot`, `splitchild`, `splitleaf`, `makeroom`, `putbreak` |
| `lcD_` | 区间删除 | `spliceleaf`, `splicerange`, `trimleaf`, `trimnode`, `shiftleaf`, `shiftnode`, `foldleaf`, `foldnode`, `rebalance`, `prune`, `balanceleaf`, `balancenode`, `emptytree` |
| `lcM_` | 度量更新 | `up` (自底向上), `tx` (跨父度量转移) |
| `lcN_` | 节点操作 | `makespace`, `copy`, `move`, `sumbytes`, `sumbreaks`, `freechildren`, `freerange` |
| `lcL_` | 叶操作 | `sumbytes` |

---

## 八、公共 API 一览

| 函数 | 签名 | 功能 |
|------|------|------|
| `lc_open` | `lc_State *lc_open(lc_Alloc *allocf, void *ud)` | 创建状态 |
| `lc_close` | `void lc_close(lc_State *S)` | 销毁状态 |
| `lc_reset` | `void lc_reset(lc_State *S)` | 重置状态 |
| `lc_newtree` | `lc_Cache *lc_newtree(lc_State *S)` | 建新树 |
| `lc_deltree` | `void lc_deltree(lc_State *S, lc_Cache *c)` | 删树 |
| `lc_breaks` | `size_t lc_breaks(const lc_Cache *c)` | 总行断点数 |
| `lc_bytes` | `size_t lc_bytes(const lc_Cache *c)` | 总字节数 |
| `lc_scan` | `int lc_scan(lc_Cache *c, lc_Scanner *scanner, void *ud)` | 批量扫至树尾 |
| `lc_seek` | `int lc_seek(lc_Cursor *C, lc_Cache *c, size_t pos)` | 按字节偏移定位 |
| `lc_seekline` | `int lc_seekline(lc_Cursor *C, lc_Cache *c, size_t line)` | 按行号定位 |
| `lc_advance` | `int lc_advance(lc_Cursor *C, lc_Diff delta)` | 字节偏移移动 |
| `lc_advline` | `int lc_advline(lc_Cursor *C, lc_Diff delta)` | 行偏移移动 |
| `lc_offset` | `size_t lc_offset(const lc_Cursor *C)` | 游标字节偏移 |
| `lc_line` | `size_t lc_line(const lc_Cursor *C)` | 游标行号 |
| `lc_linelen` | `unsigned lc_linelen(const lc_Cursor *C)` | 当前行长 |
| `lc_col` | `unsigned lc_col(const lc_Cursor *C)` | 当前列 |
| `lc_markbreak` | `int lc_markbreak(lc_Cursor *C, unsigned br)` | 单点插行断 |
| `lc_clearbreaks` | `int lc_clearbreaks(lc_Cursor *C, size_t len)` | 区间清行断 |
| `lc_insert` | `int lc_insert(lc_Cursor *C, int e, lc_Scanner *scanner, void *ud)` | **中间插入文本** |
| `lc_splice` | `void lc_splice(lc_Cursor *C, size_t del, size_t ins)` | 区间删/插字节 |

---

## 九、lc_insert 与 lc_scan 的比较

| 维度 | `lc_scan` | `lc_insert` |
|------|-----------|-------------|
| 入口参数 | `lc_Cache *c, scanner, ud` | `lc_Cursor *C, int e, scanner, ud`  |
| 初始化 | `lcB_init` (at_end=1, locend) | `lcB_initat` (at_end=0) 或尾部退化->`lcB_init` |
| 首个 break | 无关（已到末尾） | `lcB_applyfirst`（裂叶+处理） |
| fill-flush | `lcB_fillflush` | 同，但 flush 用 `at_end` 控制插入位置 |
| 扫描器无 break | N/A | `lcB_skipinsert`（仅行长调整） |
| 完事后 | 无回位 | `lc_seek` 回位 + col 调整 |
| 右裂叶处理 | N/A | `lcB_packleafs` 尝试合并，否则 flush |
