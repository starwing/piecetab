# linecache.h API 参考与实现笔记

> 单头文件 C89 库，以 B+ 计量树 (Metric B+ Tree) 维护字节偏移→行号之映射。
> 前缀 `lc_`。测试时以极小扇出 (`LC_FANOUT=4`, `LC_LEAF_FANOUT=4`) 逼树分裂。

---

## 一、数据类型

### 错误码

| 宏 | 值 | 含义 |
|----|------|------|
| `LC_OK` | 0 | 成功 |
| `LC_ERRPARAM` | -1 | 空指针或参数越界 |
| `LC_ERRMEM` | -2 | 内存分配失败 |
| `LC_ERREMPTY` | -3 | (预留) 树已空 |

### lc_State — 内存上下文

```c
typedef struct lc_State lc_State;
```

拥有两个对象池 (`lc_Pool`): `nodes` (``lc_Node``) 与 `leaves` (`lc_Leaf`)。通过 `lc_Alloc` 回调获取物理内存（页粒度 `LC_PAGE_SIZE=65536`）。多树共享同一 `lc_State`。

### lc_Cache — B+ 树

```c
typedef struct lc_Cache lc_Cache;
```

一棵 B+ 树。`root` 内嵌（非指针），故 `lc_newtree` 永不为 OOM（仅 root 嵌入于 cache 结构中）。多树间互不影响。

### lc_Cursor — 游标

```c
typedef struct lc_Cursor lc_Cursor;
```

非持久导航器，由 `lc_seek`/`lc_seekline` 初始化。`paths[0]` 指向根中槽位 (`&root.children[...]`), `paths[levels]` 指向叶槽位 (`&parent->children[...]`)。`off/lnu/loff/col` 四字段联合编码游标的绝对（字节/行号）与叶内相对位置。

`lc_Diff` 为 `ptrdiff_t`，用于移动 API 的符号偏移。

---

## 二、公共 API

### 生命周期

```c
lc_State *lc_open(lc_Alloc *allocf, void *ud);
void      lc_close(lc_State *S);
void      lc_reset(lc_State *S);
```

- `lc_open`: 创建状态对象。`allocf` 为自定义分配器 (realloc 语义)，`ud` 为透传用户数据。失败返回 `NULL`。
- `lc_close`: 释放状态及其内所有树、池。允许传入 `NULL`。
- `lc_reset`: 清空状态内所有池（含所有树之节点/叶）。状态对象本身保留。允许传入 `NULL`。

### 树生命周期

```c
lc_Cache *lc_newtree(lc_State *S);
void      lc_deltree(lc_State *S, lc_Cache *c);
```

- `lc_newtree`: 分配一棵空树（root 嵌入，`levels=0`, `child_count=0`）。失败返回 `NULL`。
- `lc_deltree`: 释放树中全部节点与叶，再释放 cache 结构。

**约束**: 不允许多树间共享节点/叶——每树独立。`lc_reset` 后所有树失效。

### 简单查询

```c
size_t lc_breaks(const lc_Cache *c);
size_t lc_bytes(const lc_Cache *c);
```

- `lc_breaks`: 返回树中总行断点数（= 总行数）。
- `lc_bytes`: 返回树中总字节数（各行长度之和）。

### lc_scan — 批量加载行断点

```c
int lc_scan(lc_Cache *c, lc_Scanner *sc, void *ud);
```

**行为**: 反复调用 `scanner(ud, 当前字节偏移)` 获取行长度（含 `\n`），返回值 0 表示扫描终止。各行自左向右追加至树尾。**允许多次 `lc_scan` 叠加**——后续扫描在已有数据之后追加。

**返回值**: `LC_OK` (成功), `LC_ERRPARAM` (空指针), `LC_ERRMEM` (OOM)。

**约束**: scanner 必须返回 ≥1 的行长度（含换行符）。同一树多次扫描之间可穿插游标操作。

**注意**: scanner 可能被调用额外次数（首轮探测返回 0 后即停止追加）；`pos` 参数为当前绝对字节偏移，可用于一致性校验。

### 游标定位

```c
int lc_seek(lc_Cursor *C, lc_Cache *c, size_t offset);
int lc_seekline(lc_Cursor *C, lc_Cache *c, size_t line);
```

- `lc_seek`: 将游标定位于字节偏移 `offset` 处。若 `offset >= lc_bytes(c)`，游标置树尾且 `C->col = offset - lc_bytes(c)`（虚拟 trailing 区域）。
- `lc_seekline`: 将游标定位于第 `line` 行首列。`line` 可等于 `lc_breaks(c)`（末行之尾，即 trailing 位置）。`line > lc_breaks(c)` 时为 `LC_ERRPARAM`。空树 (`child_count==0`) 下 `line=0` 合法。

**返回值**: `LC_OK`, `LC_ERRPARAM`。

**后置**: `C->tree = c`, `C->paths[]` 已初始化指向根→叶路径。`C->col` 为 0（seekline）或行内偏移（seek）。

### 游标移动

```c
int lc_advance(lc_Cursor *C, lc_Diff delta);
int lc_advline(lc_Cursor *C, lc_Diff delta);
```

- `lc_advance`: 按字节偏移移动游标。`delta > 0` forward, `< 0` backward。越界自动 clamp（`≤0 → offset=0`, `≥bytes → locend + col`）。空树 (bytes=0) 下 delta=0 是合法 no-op，delta≠0 报错。
- `lc_advline`: 按行偏移移动游标。空树 (bytes=0) 下始终返回 `LC_OK`（no-op）。越界同法 clamp（`≤0 → line=0`, `≥breaks → 末行尾`）。move 0 行为 == 当前行重定位（col 归 0），no-op。

**返回值**: `LC_OK`, `LC_ERRPARAM`。

### 游标查询

```c
size_t   lc_offset(const lc_Cursor *C);
size_t   lc_line(const lc_Cursor *C);
unsigned lc_linelen(const lc_Cursor *C);
unsigned lc_col(const lc_Cursor *C);
```

- `lc_offset`: 游标绝对字节偏移。若 `C==NULL` 返回 0。
- `lc_line`: 游标所在行号（0-based）。若 `C==NULL` 返回 0。
- `lc_linelen`: 游标当前行之字节长度。若 `C` 在 trailing 区域（`lnu == breaks[leaf]`），返回 `C->col`（虚拟行长度）。若 `C==NULL` 返回 0。
- `lc_col`: 游标在当前行内列偏移（0-based）。若 `C==NULL` 返回 0。

### 标记/清除行断点

```c
int lc_markbreak(lc_Cursor *C, unsigned br);
int lc_clearbreaks(lc_Cursor *C, size_t len);
```

- `lc_markbreak`: 在游标位置插入一个行断点。`br` 为新行长度（含 `\n`）。
  - 若 `br == 0`: 原地插入零长度断点（当前行剩余数据转入下一行）
  - 若 `br == lc_linelen(C) - C->col`: no-op（断点已在末尾）
  - 若 `br > 行剩余长度`: 先将 `br` 字节插入至行尾，再在 `br` 处断（见 `lc_markbreak` 的 splice 回退逻辑）
  - 空树时：直接以 `br` 建立单行树
  - 返回后 `C` 定位断后新行之首（`col=0`, `lnu+=1`, `loff`/`off/nu` 已更新）
- `lc_clearbreaks`: 删除从游标位置起 `len` 字节内的所有行断点（各段连接为一行）。内部调用 `lc_splice(C, len, len)`。游标位于合并后行之 col 位置。

**返回值**: `LC_OK`, `LC_ERRPARAM`, `LC_ERRMEM`。

### lc_splice — 区间删除/插入字节

```c
void lc_splice(lc_Cursor *C, size_t del, size_t ins);
```

**参数校验**: 空指针、`del==0 && ins==0`、空树 (`levels==0 && root.child_count==0`) — 皆 no-op 返回。`del` 自动 clamp 至 `bytes - offset`。

**行为**:
- 删空树 (`offset==0 && del >= bytes`): 重置树为初始态，`C->col = ins`（虚拟 trailing 字节）
- 删+补字节: 先 `lc_advance(&R, del)` 定位右侧，调用 `lcD_spliceleaf`（同叶）或 `lcD_splicerange`（跨叶）删除。删除后若树已空，重置树。
- 插入字节: 若游标在有效行内，`leaf->bytes[C->lnu] += ins` 加长当前行；`C->col += ins` 移动游标。

**返回值**: void（永不失败——设计确保）。操作前后游标 100% 合法。

**注意**: 删除区间可跨任意叶边界。`lc_splice` 是 `lc_clearbreaks` 的基础原语，也是 `lc_markbreak` 内部 br>rem 时的回退机制。

### lc_insert — 在中部插入文本/行

```c
int lc_insert(lc_Cursor *C, unsigned e, lc_Scanner *sc, void *ud);
```

**行为**: 在游标位置裂开当前行，scanner 输出追加于裂点之后，再将右侧数据缝合回树。`e` 为不完整行尾缀字节（缝合完毕后才加回），scanner 返回 0 表示结束。

**两个独立概念**:
- `scanner` 输出: 完整行（含 `\n`）。在裂点后逐行填充。
- `e`: 不成行之残字节。总是在全部 scanner 输出 + 缝合 + `rm` 补写完毕后才加回裂点行末尾。`C->col` 加 `e`。

**流程**: cutleaf 裂树 → [append scanner 行, fillrt 扩容] 循环 → stitch 缝合 → fixsource 补 `rm` → 加回 `e`

**返回值**: `LC_OK`, `LC_ERRPARAM`, `LC_ERRMEM`。OOM 时通过 `lcB_rollback` 完整恢复树至 cutleaf 前状态。

**约束**: 树无数据 (`root.child_count == 0` 且 `levels == 0`) 时 `lc_insert` 等价于首次填充——scanner 行为与 `lc_scan` 等同，`e` 仍需最后加回。

---

## 三、数据结构要点

### B+ 计量树

```
root (深度 0..15)
├── children[0]: Node* (clusters[0..62])
│   ├── bytes[0..62]: 各子树累计字节
│   ├── breaks[0..62]: 各子树累计行数
│   └── children[0..62]: Node* / Leaf*
├── children[1]: Node*
│   ...
└── children[n-1]: Leaf* (叶层: 存 raw line lengths)
    └── bytes[0..62]: 各行字节长度
```

**核心设计决策**:
1. **叶无 `child_count`** — 有效行数由父 `breaks[i]` 决定。叶仅存储各行字节长度，约束来自 LC_LEAF_FANOUT 上限。
2. **度量双计 (bytes + breaks)** — 每内部节点存储两个累积数组，允许 O(log n) 字节偏移↔行号双向导航。
3. **嵌入 root** — `lc_Cache.root` 是值而非指针，省一次分配且不允许 root 被单独释放。
4. **无 parent 指针** — 游标经 `paths[]` 维护祖先路径。所有向上传播 (`lcM_up`) 皆依赖此数组。

### 游标字段编码

```
C->off    : 所有前驱叶（不含当前叶）的累计字节
C->nu     : 所有前驱叶的累计行数
C->loff   : 当前叶内，C->lnu 行之前各行之和（叶内字节偏移）
C->lnu    : 当前叶内行索引 (0-based)
C->col    : 当前行内列偏移

offset = off + loff + col
line   = nu + lnu
```

设计意图: `off`+`nu` 锚定叶位置，`loff`+`lnu` 为叶内精确定位，`col` 为行内微调。三组累加成全局偏移，无需扫描。

### 对象池 (lc_Pool)

每页 `LC_PAGE_SIZE` 字节，分配 `sizeof(lc_Node)` 或 `sizeof(lc_Leaf)` 的对象。空闲链表 (`freed`) 回收已释放对象。`S->nodes` 与 `S->leaves` 独立管理。

`lcP_reserve(n)` 保证池中至少有 `n` 个可用对象（含 freelist），用于事务——stitch 入口的预分配保证全程不 OOM。

---

## 四、核心算法要点

### 1. 洋葱序缝合 (lcD_stitchnode)

stitchnode 是 linecache 最精巧的算法——洋葱序 `for (k=0; k≤levels; ++k)` 中 `k` 为洋葱层 (k=0=叶层, k=levels=根层)，`kl = levels - k` 为 rt[k] 对应树中层级。

**不动点**: 每轮首复制 (`m = min(rtcc, FANOUT-pcc)`) 填当前父节点，`lcM_up` 更新祖先度量。若全部搬完 (`m == rtcc`) 且非根层 → 跳过本层修复。否则进入修复块执行 foldnode + fillrt 为剩余数据建新链。

**kl==0 保底**: 根层永远进入修复——无论之前各层是否已处理，kl==0 处强制 foldnode → root仅一子则缩根。循环条件 `k <= lcK_levels(C)` 动态适应缩根。

**d 的延迟生效**: d 用于记录"待修复右侧子节点数"，当前轮末尾设值，**下一轮** backwardnode 才消耗。因 fillrt 新链建在右侧、其 underfill 需本轮的 foldnode 修复后才能将 C 回退至此位置。

### 2. 三段法区间删除 (lcD_splicerange)

L/R 双游标界定删除区间，操作分三段:
1. **求分岔+修边**: 找 `L->paths[l] != R->paths[l]` 的首层。`trimright(L)` 删 L 叶右侧、`trimleft(R)` 删 R 叶左侧。
2. **底切+掘空**: 自 leaf 向上至 `l+1` 层——L 右侧兄弟子树全部删除，R 右侧兄弟子树惰性搬入 `rt[]`。分岔层 `l` 处删除 L 与 R 之间的中段子树。
3. **缝合+修复**: `stitch(L, rt)` 将 R 右侧子树缝回，再 `foldleaf(L)` + 可能 `rebalance` 平衡树。

`rt[]` 惰性使用: 仅在有右侧数据可搬的层才写入 `rt[k].child_count > 0`；stitchnode 遇 `rtcc == 0` 直接跳过本层。

### 3. lc_insert 五段流程

1. **参数校验**: 空指针 → `LC_ERRPARAM`; `rt[]` 清零。
2. **裂叶 (cutleaf)**: 将 C 右侧行搬入 `rt[0]`，`rm = C->col`（裂点残字节）在 `rt[0].bytes[0]` 扣除。保存 sC 快照。
3. **追搬轮替**: `append(scanner)` 填叶 → 旁满则 `findroom` 扩容搬右兄入 rt。OOM → rollback。
4. **缝合 (stitch)**: mergeleaf(合并裂点叶与 rt[0]) → stitchnode(洋葱序) → foldleaf → 位置修正。
5. **补 e/rm**: fixsource 用 sC 将 `rm` 补回裂点叶。`e` 加回当前行末尾。

### 4. foldleaf/foldnode 游标修正不变式

均分方向 `dl`/`dn` 的符号与 `*ls == o`/`*ns == o` 严格绑定:
- **C 原在左 (`*ls==o`)**: 右兄弟段数更多 → `dl<0`，数据从右搬入左叶尾部。C 在左叶中位置不变，无需修正。
- **C 原在右 (`*ls!=o`)**: 左兄弟段数更多 → `dl>0`，数据从左搬入右叶前部。C 在右叶中被推后 `dl` 位: `C->lnu += dl`。

`dl==0`/`dn==0` 时 `db==0`/balancenode 返回 0 已提前退出，故到达 assert 时 `dl≠0` 且 `(dl<0) != (*ls!=o)` 恒真。此不变式由 `balanceleaf`/`balancenode` 的均分公式的 **rounding 方向** 保证:

```
mid = (cl + cr + (*ls==o != 0)) / 2
```

C 在原左叶时 `mid` 多留一个段给左叶，反之亦然。保证游标指向的叶/节点永不为右侧的搬迁目的地。

### 5. lcB_rollback — OOM 回滚

`lc_insert` 在任何阶段 OOM 时通过 `lcB_rollback` 完整恢复树至 `lcB_cutleaf` 之后状态。步骤:
1. **降根**: 循环 `for(k=levels; k>sl; --k)` 清除 fillrt rootpush 增加的层，将 root 降回 `sC` 的 levels
2. **合并裂点叶**: `rt[0].children[0]` 拷贝回左半叶 `bytes[C->lnu]`
3. **缝合 rt**: 洋葱序将 `rt[]` 中右侧兄弟子树全部缝回。net 度量差量以 `lcM_up` 传播

依赖 `stitch` 的事务性: 因为 `stitch` 内部 OOM 在入口 `lcP_reserve` 已被阻止，rollback 在 stitch 之后不会执行（若此前 append/findroom 已失败则直接 rollback; 若 stitch 之后失败... 理论上不可能失败）。

### 6. fixsource — rm 定位与补写

- `sC` 为 cutleaf 后的游标快照，保存裂点叶信息
- `lcK_levels(S) - sl` 为 stitch 中 rootpush 增加的层级差，sC 旧路径需下移 `k` 位
- 新增层级填全量左侧路径 (`sp[l] = &parent(sC, l)->children[0]`)，旧根层 `paths[0]` 偏移恢复
- `lcK_leaf(sC)->bytes[sC->lnu] += rm; lcM_up(sC, sl, rm, 0)` 补回度量

### 7. lc_scan 批量加载中的满叶父 fill 逻辑

`lcB_append` 返回 1 当 `pcc == LC_FANOUT && li == LC_LEAF_FANOUT`（父满且末叶满）。`lc_scan` 循环响应此信号:
- 先自底向上寻首个非满层 `l`
- `lcD_makechain(C, l, levels, 0)` 建空节点链
- 继续下一轮 append，将 scanner 输出填入新链

`lcP_reserve` 在 makechain 前预分配节点，OOM 时安全返回 `LC_ERRMEM`。

---

## 五、内部函数分类

| 前缀 | 职责 | 关键函数 |
|------|------|----------|
| `lcK_` | 游标导航 (Kursor) | `findleaf` (按 offset 自上向下定位叶), `findline` (按 line 定位叶), `findinleaf` (叶内按 offset 定位行), `locend` (定位树尾), `forwardoff/backwardoff` (跨叶字节移动), `forwardline/backwardline` (跨叶行移动) |
| `lcB_` | 行断插入/insert (Break) | `oneline` (空树首行), `splitroot/splitchild/splitleaf` (被动裂), `putbreak` (叶内插入行断), `makeroom` (自底向上预裂所有满层), `append` (追填叶), `cutleaf` (裂出右半叶), `fixsource` (补 rm), `rollback` (OOM 回滚) |
| `lcD_` | 删除/平衡 (Delete) | `trimleft/trimright` (删除叶缘度量), `spliceleaf` (叶内跨行删除), `splicerange` (跨叶区间删除), `stitch` (缝合 rt 回树), `stitchnode` (洋葱序缝合), `mergeleaf` (相邻叶合并), `backwardnode` (stitchnode 褶皱修复), `foldleaf/foldnode` (兄弟折叠), `rebalance` (缩根+内层平衡), `balanceleaf/balancenode` (均分), `makechain` (建空节点链), `findroom` (寻非末位层+搬右兄弟+垂空链) |
| `lcM_` | 度量 (Metrics) | `up` (自底向上传播度量至根) |
| `lcN_` | 节点操作 (Node) | `sumbytes/sumbreaks` (求和), `makespace` (开槽), `copy/move` (搬移), `erase` (释子+压实), `freechildren` (递归释子树) |
| `lcL_` | 叶操作 (Leaf) | `sumbytes` (求和叶内 range), `new` (分配) |
| `lcP_` | 池 (Pool) | `init/destroy/alloc/free/reserve` |

---

## 六、配置宏

| 宏 | 默认 | 含义 |
|----|------|------|
| `LC_FANOUT` | 62 | 内节点最大子数 |
| `LC_LEAF_FANOUT` | 62 | 叶最大行数 |
| `LC_MAX_LEVEL` | 16 | 最大树深（paths 数组大小） |
| `LC_PAGE_SIZE` | 65536 | 池分配器页大小 |
| `LC_STATIC_API` | — | 定义后所有函数为 static (单文件嵌入) |
| `LC_POOL_STATS` | — | 定义后统计池中 live objects |

---

## 七、代码审核

`linecache.h` 当前状态: **稳定可用**。

线覆盖率 100%，分支覆盖率 87.5%。未覆盖分支中绝大多数为 assert 栅、NULL 检查路径、及 `LC_LEAF_FANOUT=4` 下不可达的数学平衡点（如 `lcD_balanceleaf` 之 `d==0` 分支——需 `l == (l+r+1)/2` 整数解，当 `l≤2, r≤4` 时无解）。工程中 stitchnode 的 foldnode 洋葱循环、rebalance 的缩根续行、backwardnode 的跨兄弟跳转，均已由 brute 测试及 cacheV 构造覆盖。

### 已知未覆盖分支（含分析）

| 位置 | 条件 | 可达性（FANOUT=4） | 说明 |
|------|------|---------------------|------|
| `lcD_balanceleaf` d==0 | 两叶完美均分 | 不可达 | 需 `cl==(cl+cr+1)/2` 整数解，`cl≤2, cr≤4` 无解。大扇出可触发 |
| `lcD_foldleaf` db==0 | balanceleaf 无数据搬移 | 同上 | 由 d==0 直升，同上不可达 |
| `lcD_rebalance` foldnode-return-0 | 平衡但未合并 | 已落地 | 见 `test_rebalance_earlyexit` |
| `lcD_spliceleaf` end==bc | 游标在叶末，无可删 | 不可达 | `lc_splice` 在同叶内调用 spliceleaf 时，游标必在有效行内（否则跨叶走 splicerange） |
| `lcD_trimleft` C->lnu>=bc | 游标在 locend 处 | 间接覆盖 | 仅 splicerange 调用，C 为 L 游标（已跨叶），lnu 永不 >= bc |

### 可改进之处

1. `lcD_foldnode` 之 `useleft` 参数在 stitchnode 中为 `(fl==kl)`，rebalance 中恒为 0。语义隐晦——建议加注"非 0 时优先与左兄弟合并"以明类型。
2. `lc_clearbreaks` 以 `lc_splice(C, len, len)` 做等价实现，为正确但非最优——可在未来实现真正的按 break 跨越删除（参见 `design_splice.md` 的历史方案），当前规模下不必要。
3. `lc_markbreak` 之 `br>rm` 回退路径通过 `lc_splice` 清除跨行断，调用方不可见——期待未来提供直接 API。
