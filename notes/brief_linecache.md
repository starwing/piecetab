# linecache.h 项目总览

> 供 Agent 快速了解项目全貌。可 grep 查询的信息不载此文，提供查询命令。

## 一、项目概要

单头文件 C89 库，前缀 `lc_`。以 B+ 计量树 (Metric B+ Tree) 维护字节偏移→行号之映射。

**功用**: 文本编辑器中维护行号缓存。支持行断点插入/删除 (`lc_markbreak`/`lc_clearbreaks`)、区间删除 (`lc_remove`)、区间删插字节 (`lc_splice`)、中部插入 (`lc_append`/`lc_insert`)、批量加载 (`lc_scan`)。

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

| 符号             | 默认  | lc_test4 值 | 含义           |
| ---------------- | ----- | ----------- | -------------- |
| `LC_FANOUT`      | 62    | 4           | 内节点最大子数 |
| `LC_LEAF_FANOUT` | 62    | 4           | 叶最大行数     |
| `LC_MAX_LEVEL`   | 16    | 16          | 最大树深       |
| `LC_PAGE_SIZE`   | 65536 | 512         | 池分配器页大小 |

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

| 类别     | 函数                                                            | 功用                                                                           |
| -------- | --------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| 生命周期 | `lc_open`, `lc_close`, `lc_reset`                               | 状态管理                                                                       |
| 树       | `lc_newcache`, `lc_delcache`                                      | 树生命周期                                                                     |
| 批量     | `lc_scan`                                                       | 批量加载行断点到树尾（可叠加）                                                 |
| 查询     | `lc_breaks`, `lc_bytes`                                         | 树级汇总                                                                       |
| 定位     | `lc_seek`, `lc_seekline`                                        | 按偏移/行号定位游标                                                            |
| 移动     | `lc_advance`, `lc_advline`                                      | 字节/行偏移移动（越界 clamp）                                                  |
| 查询     | `lc_offset`, `lc_line`, `lc_linelen`, `lc_col`, `lc_lineoffset` | 游标状态查询（宏）                                                             |
| 断点     | `lc_markbreak`, `lc_clearbreaks`                                | 单点插入 / 区间清除行断（宏）                                                  |
| 编辑     | `lc_remove`, `lc_splice`, `lc_append`, `lc_insert`              | 区间删除 / 区间删插字节 / 中部插入或纯字节追加（sc=NULL） / 插入游标处（不动） |

## 六、内部函数命名体系

```bash
# 列出所有 static 函数名
grep '^static' linecache.h
```

| 前缀   | 职责      | 代表函数                                                                                                                                                                                                     |
| ------ | --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `lcK_` | 游标导航  | `findleaf`, `findline`, `findinleaf`, `locend`, `forwardoff`, `backwardoff`, `forwardline`, `backwardline`                                                                                                   |
| `lcB_` | 行断/插入 | `oneline`, `makeroom`, `splitroot`, `splitchild`, `splitleaf`, `putbreak`, `append`, `cutleaf`, `fixsource`, `rollback`                                                                                      |
| `lcD_` | 删除/平衡 | `trimleft`, `trimright`, `balanceleaf`, `balancenode`, `foldleaf`, `foldnode`, `rebalance`, `rmleaf`, `rmrange`, `makechain`, `findroom`, `mergeleaf`, `backwardnode`, `stitch`, `stitchnode`, `checkstitch` |
| `lcM_` | 度量      | `up` (自底向上传播 bytes/breaks 至根)                                                                                                                                                                        |
| `lcN_` | 节点操作  | `sumbytes`, `sumbreaks`, `makespace`, `copy`, `move`, `remove`, `freechildren`                                                                                                                               |
| `lcL_` | 叶操作    | `sumbytes` (宏 `lcL_new`, `lcN_leaf`)                                                                                                                                                                          |
| `lcP_` | 池        | `init`, `destroy`, `alloc`, `ralloc`, `free`, `reserve`                                                                                                                                                      |

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

`lcP_ralloc(pool)` 从预留 freelist 取对象，内部 `assert(freed)` 保证不缺。splitroot / splitchild / splitleaf 均根据游标位置修正 paths[]。

## 九、关键设计决策

1. **嵌入 root**: `lc_Cache.root` 是值非指针 — 省一次分配，免 OOM 于建树
2. **叶无 child_count**: 行数由父 `breaks[i]` 决定，冗余由上层约束保证
3. **度量双计**: bytes + breaks 双数组允许 O(log n) 双向导航
4. **lc_splice 事务性**: 删除路径委托 `lc_remove`，其内部 reserve 预分配保不 OOM；插入路径仅改叶内字节
5. **stitch 事务性**: `lcD_checkstitch` 入口 reserve(levels+2 节点)，stitch 全程无 OOM
6. **balanceleaf rounding**: `d = l - ((l + r + 1) >> 1)` 向上取整，与 foldleaf 游标修正断言强耦合（`assert((dl<0) != (*ls!=o))`）— 修改均分公式需同步改调用方

## 十、测试

```bash
just lc               # 编译运行 lc_test4 (FANOUT=4, ASAN+UBSAN)
just lc <prefix>      # 运行名称以 prefix 开头的测试
just lc8              # 编译运行 lc_test8 (FANOUT=8)
just cov              # 全量覆盖率（编译、运行、lcov 报告、未覆盖列表）
```

测试用例以 `TESTS(X)` 宏列举，scanner 从 `unsigned*` 数组顺序取行长度。`lc_test4` 以极小扇出逼树分裂/合并/旋转变换。

```bash
# 列出全部测试名
grep '^    X(' tests/lc_test4.c
```

## 十一、相关文档

| 文档                                  | 内容                              |
| ------------------------------------- | --------------------------------- |
| `notes/design_insert_delete_v2.md`    | 当前基于stitch的批量插入/删除设计 |
| `notes/design_bulk_loading.md`        | lc_scan Bulk Loading 设计         |
| `notes/design_splice.md`              | Splice 区间删除三段法设计         |
| `notes/history_range_delete.md`       | 区间删除算法演进史                |
| `notes/brief_tests.md`                | 测试结构笔记                      |
| `notes/brief_refine.md`               | 代码精炼经验教训                  |
| `notes/uncovered_branches.md`         | 未覆盖分支详细映射                |
| `notes/research_marktree.md`          | neovim marktree 调研（对比参考）  |
| `notes/design_spantree.md`            | spantree 设计决议（字节属性段树） |
| `notes/research_highlighter.md`       | 语法高亮器输出与增量更新调研      |
| `docs/linecache.zh.md`                | 面向用户的 API 参考手册           |

## 十二、编码铁律 (AGENTS.md)

- C89 唯。函数 25/30 行限。签名 ≤79 字。
- static helper 不校验参数 — assert 保不变式。
- 内部函数前缀 `lcX_` (如 `lcK_findleaf`)，X 为单字母分类码。
- **禁止删测试**。先修测试后改代码。
- 修改后必过 `clang-format`。

## 十三、变量命名规范

> **铁律**：新增函数**必须**从下表取变量名，不得自造。
> 如有新概念需新名，先更新此表后写代码。

### 13.1 层级变量（尾缀 `l` = level）

| 变量 | 含义 | 上下文 |
|------|------|--------|
| `l`  | 当前层级（通用循环变量） | 函数参数、循环计数器 |
| `sl` | saved levels（快照树深） | `sC` 保存时的 `lcK_levels(C)` |
| `fl` | found level（findroom 寻得层） | `lcD_findroom` |
| `kl` | 计算层号（`levels - k`） | `lcD_stitchnode`, `lcD_cutrange` |
| `rl` | remaining levels（距叶层数） | `lc_checknode` |

### 13.2 计数与度量

| 变量   | 含义 | |
|--------|------|---|
| `cc`   | child_count of current node | `lcN_cc(p)` |
| `lc`   | line count（break count of leaf/child） | `p->breaks[i]` |
| `rtlc` | rt[0].breaks[0]（右半叶行数） | rollback leaf merge |
| `rtcc` | rt[k].child_count | rollback node loop |
| `n`    | 通用计数 | loops, memcpy |
| `db`   | delta bytes（度量差，向上传播） | `lcM_up` 累积量 |
| `dl`   | delta lines（行数差） | `lcM_up` 累积量 |
| `cb`   | current bytes（本轮追加） | `lcB_append` 内循环 |
| `cl`   | current lines（本轮追加行数） | `lcB_append` 内循环 |
| `d`    | delta / distance（lcK_* 导航剩余距离） | `lcK_forwardoff` 等 |

### 13.3 游标位置

| 变量 | 含义 |
|------|------|
| `pos` | 绝对字节偏移（scanner 用） |
| `off` | 叶起始字节偏移（全局累积） |
| `loff`| 叶内字节偏移 |
| `col` | 行内列偏移 |
| `nu`  | 叶起始行号（全局累积） |
| `lnu` | 叶内行索引 |

### 13.4 节点/叶指针

| 变量 | 含义 | 何处赋值 |
|------|------|----------|
| `p`  | parent（当前父节点） | `lcK_parent(C, l)` |
| `d`  | destination（lcN_* 操作目标节点） | `lcN_copy(d, ...)` |
| `r`  | rt[k]（右半临时节点） | `&rt[k]` |
| `n`  | 通用 node / new node | `lcN_new(S)` |
| `lf` | leaf pointer | `lcN_leaf(p, i)` |
| `lr` | right leaf（rt[0] 右半叶） | `rt[0].children[0]` |
| `nl` | new leaf | `lcL_new(S)` |
| `nn` | new node | `lcP_ralloc(...)` |

### 13.5 循环索引

| 变量 | 含义 | 铁律 |
|------|------|------|
| `k`  | 洋葱层（0=叶, sl=根） | **所有 `k` 必须满足 `k = levels - l`** |
| `i`  | parent 中子节点下标（通用） | 几乎所有函数 |
| `j`  | 辅助下标 | 极少使用 |

### 13.6 状态快照与参数

| 变量 | 含义 |
|------|------|
| `sC`  | saved cursor（快照） |
| `sbc` | saved break count（快照时 lnu） |
| `S`   | lc_State |
| `c`   | lc_Cache |
| `root`| `&C->tree->root` |
| `rt`  | `C->tree->S->rt`（临时节点数组） |
| `w`   | water mark（findroom 调用过的最高层级） |
| `e`   | extra bytes（行末追加字节） |
| `rm`  | remainder（裂点残字节, = sC.col） |

### 13.7 左右区分

> 变量名末位 `L`/`R`（大写）表示左/右。**多字母变量必须用大写后缀**，
> 以免和单字母 `l`（lines）或 `l`（levels）冲突。

| 变量 | 含义 | 上下文 |
|------|------|--------|
| `cL`/`cR` | count-Left / count-Right | `lcD_foldleaf`, `lcD_foldnode`, `lcD_balanceleaf` |
| `bL`/`bR` | bytes-Left / bytes-Right | `lcD_balanceleaf` |
| `offL`/`offR` | offset-Left / offset-Right | cursor 左右偏移量 |

### 13.8 单字母多义（领域不同、无需改）

| 字母 | 领域 A | 领域 B |
|------|--------|--------|
| `d`  | destination（`lcN_*`） | distance / delta（`lcK_*`, `lcD_stitchnode`） |
| `l`  | levels（`lcD_rebalance` 等） | left（`lcD_balanceleaf` 参数）— 单字母不冲突 |
| `r`  | rt[k]（rollback/stitch） | right（balanceleaf）— 同上 |

### 13.9 函数前缀

| 前缀 | 全称 | 职责 |
|------|------|------|
| `lcK_` | Kurser | 游标导航 |
| `lcB_` | Break | 行断、追加、裂叶、回滚 |
| `lcD_` | Delete | 删除、折叠、平衡、缝合 |
| `lcM_` | Metric | 度量向上传播 |
| `lcN_` | Node | 节点运作（copy/move/purge/sum/makespace） |
| `lcL_` | Leaf | 叶操作（sumbytes, new） |
| `lcP_` | Pool | 池分配器 |

### 13.10 快速检索

```bash
# 查某变量已用语义
rg '\b<name>\b' linecache.h | head

# 查某前缀函数清单
grep '^static.*lcX_' linecache.h
```

## 十四、边界行为速查表（供绑定层胶水参考）

### 14.1 越界 / 零参数 / 空树

| 函数 | 越界 | 零参数 | 空树 | OOM |
|------|------|--------|------|-----|
| `lc_scan` | — | c==NULL→ERRPARAM | 从尾部开始追加 | 返回ERRMEM |
| `lc_append` | — | C==NULL→ERRPARAM | 正常追加 | 回滚sC→ERRMEM |
| `lc_insert` | — | 同 append | 同 append | 同 append |
| `lc_splice` | off≥bytes→仅col+=ins | del=0,ins=0→完全无操作 | col+=ins | 委托 remove |
| `lc_remove` | L≥R→无操作 | NULL/非同树→ERRPARAM | 无操作 | assert 预留 |
| `lc_seek` | **软 clamp**, col=excess | C/c==NULL→ERRPARAM | 跳 locend, col=n | 无分配 |
| `lc_seekline` | **硬 ERR_PARAM** | C/c==NULL→ERRPARAM | paths[0]=root.children | 无分配 |
| `lc_advance` | clamp 到端 | C/tree==NULL→ERRPARAM | — | 无分配 |
| `lc_advline` | clamp 到最后行末 | C/tree==NULL→ERRPARAM | bytes==0→无操作 | 无分配 |

### 14.2 关键语义

- **lc_seek 越界软 clamp** vs **lc_seekline 越界硬 ERR_PARAM**（不一致，已知摩擦）
- **lc_seek 到 off > lc_bytes 时 col = off - lc_bytes**（尾后区域，excess）。后续
  `lc_col(C)` 返回该 excess 值，`lc_linelen(C)` 也返回该值。
- **尾后区域的语义**：`lc_linelen` 在 lnu >= breaks[i] 时返回 `C->col`。
  残段不存树（设计目的），通过 lc_seek 定位到尾后 + lc_col 获取残段长度。
- **lc_splice(C, 0, 0) → 完全无操作**。绑定层不需要包 `if (del > 0)` 守卫。
- **lc_append(C, 0, NULL, NULL)** → 仅 `lcD_addbytes(C, 0)`，全无操作（`!e` 提前 return）。
- **Scanner 回调返回 0 = eof**（不是空行）。linecache 不支持零长行。
- **`lc_remove` 用双游标定区间**，非长度参数。两游标同位置 → 无操作。
