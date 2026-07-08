# linecache.h API 参考与实现笔记

> 单头文件 C89 库，以 B+ 计量树 (Metric B+ Tree) 维护字节偏移→行号之映射。
> 前缀 `lc_`。测试时以极小扇出 (`LC_FANOUT=4`, `LC_LEAF_FANOUT=4`) 逼树分裂。

---

## 一、数据类型

### 错误码

| 宏            | 值  | 含义             |
| ------------- | --- | ---------------- |
| `LC_OK`       | 0   | 成功             |
| `LC_ERRPARAM` | -1  | 空指针或参数越界 |
| `LC_ERRMEM`   | -2  | 内存分配失败     |

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
#define lc_offset(C)     ((C)->off + (C)->loff + (C)->col)
#define lc_line(C)       ((C)->nu + (C)->lnu)
#define lc_col(C)        ((C)->col)
#define lc_lineoffset(C) ((C)->off + (C)->loff)
unsigned lc_linelen(const lc_Cursor *C);
```

- `lc_offset`: 游标绝对字节偏移（宏，直接解引用，调用方须保证 C 非 NULL）。
- `lc_line`: 游标所在行号（0-based，宏）。
- `lc_linelen`: 游标当前行之字节长度。若 `C` 在 trailing 区域（`lnu == breaks[leaf]`），返回 `C->col`（虚拟行长度）。若 `C==NULL` 返回 0。
- `lc_col`: 游标在当前行内列偏移（0-based，宏）。
- `lc_lineoffset`: 游标当前行起始字节偏移（宏，= offset - col）。

### 标记/清除行断点

```c
int lc_markbreak(lc_Cursor *C, unsigned len);
#define lc_clearbreaks(C, len) lc_splice((C), (len), (len))
```

- `lc_markbreak`: 在游标位置插入一个行断点。`len` 为新行长度（含 `\n`），须 ≥ 1。
  - 若 `len == lc_linelen(C) - C->col`: no-op（断点已在末尾）
  - 若 `len > 行剩余长度`: 先将 `len` 字节插入至行尾，再在 `len` 处断（见 `lc_markbreak` 的 splice 回退逻辑）
  - 空树时：直接以 `len` 建立单行树
  - 返回后 `C` 定位断后新行之首（`col=0`, `lnu+=1`, `loff`/`off/nu` 已更新）
- `lc_clearbreaks`: 删除从游标位置起 `len` 字节内的所有行断点（各段连接为一行）。等价于 `lc_splice(C, len, len)` 的宏。游标位于合并后行之 col 位置。
  - `lc_splice` 的宏包装。参数校验委托给 `lc_splice`。

**返回值**: 同 `lc_splice` — `LC_OK`, `LC_ERRPARAM`。

### lc_erase — 擦除游标间区间

```c
int lc_erase(lc_Cursor *L, lc_Cursor *R);
```

**行为**: 删除游标 L 到 R 之间所有字节（含行断），右方内容前移。L、R 须属同一树且 `lc_offset(L) < lc_offset(R)`。

**参数校验**:
- `L==NULL` 或 `R==NULL` 或 `!L->tree` 或 `L->tree != R->tree`: 返回 `LC_ERRPARAM`
- 空区间、逆序、L 越界: 返回 `LC_OK`，no-op

**返回值**: `LC_OK`（成功或 no-op），`LC_ERRPARAM`（参数非法）。操作后 R 失效（树结构已变），L 指向删除点。

**实现**: 同叶调用 `lcD_eraseleaf`，跨叶调用 `lcD_eraserange`（三段法：trim→cut→stitch）。内部 `lcP_reserve` 预分配保证不 OOM。

### lc_splice — 区间删除/插入字节

```c
int lc_splice(lc_Cursor *C, size_t del, unsigned ins);
```

**参数校验**: `C==NULL` 或 `C->tree==NULL` 返回 `LC_ERRPARAM`。`del` 自动 clamp 至 `bytes - offset`。`ins` 为 0 时跳过插入。

**行为**:
- 游标在 trailing 区域: `C->col += ins` 直接返回
- 删+补字节: 内部 `lc_advance` + `lc_erase` 处理删除，再 `lcD_addbytes` 加回插入字节。删除后若树已空，重置树。
- 插入字节: 若游标在有效行内，`leaf->bytes[C->lnu] += ins` 加长当前行；`C->col += ins` 移动游标。

**返回值**: `LC_OK`（成功），`LC_ERRPARAM`（参数非法）。删除路径由 `lc_erase` 的预分配保证不 OOM。

**注意**: 删除区间可跨任意叶边界。`lc_splice` 内部委托 `lc_erase` 处理删除——`splice(C, del, ins)` 等价于 `erase + addbytes`。`lc_clearbreaks` 是 `splice(len, len)` 的便捷宏。

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
5. **行长度 ≥ 1** — 树内 `bytes` 数组的每个值至少为 1（换行符自身占 1 字节），不支持零长行。

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

### 虚拟行（trailing region）

游标可位于所有叶数据之后（树尾或空树），此状态下 `C->lnu == p->breaks[li]`（叶末行数），称 **trailing 区域**。此区域无对应叶内数据，`C->col` 存储从叶末起算的虚拟字节偏移。

| 操作                      | trailing 区域行为                                          |
| ------------------------- | ---------------------------------------------------------- |
| `lc_linelen()`            | 返回 `C->col`（虚拟行长度）                                |
| `lc_seek(C, c, n)`        | 若 `n ≥ lc_bytes(c)`，`C->col = n - lc_bytes(c)`           |
| `lc_seekline(C, c, n)`    | `n == lc_breaks(c)` 定位末行之尾                           |
| `lc_splice(C, del, ins)`  | 仅 `C->col += ins`，不改树结构                             |
| `lc_markbreak(C, len)`    | 空树时直接建单行树；非空树若 `lnu==breaks[i]` 则于叶尾加行 |
| `lc_insert(C, e, sc, ud)` | 空树时等价于首次填充（同 `lc_scan`）                       |

虚拟行设计允许游标在文本之外操作，避免了大量"边界之前"与"边界之后"分支。`lc_seek` 于空树下设 `C->col=n`，`lc_insert`/`lc_markbreak` 随后自该位置建立树数据。

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

### 2. 三段法区间删除 (lcD_eraserange)

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

`dl==0`/`dn==0` 时 `db==0`/balancenode 返回 0 已提前退出，故到达 assert 时 `dl≠0` 且 `(dl<0) != (*ls!=o)` 恒真。此不变式由均分公式的 **rounding 方向** 保证:
- `balanceleaf` — `d = l - ((l + r + 1) >> 1)`，固定向上取整
- `balancenode` — `d = l - ((l + r + (left != 0)) >> 1)`，`left` = `(*ns == o)`

C 在原左侧时 `mid` 多留一个段给左侧，反之亦然。保证游标指向的叶/节点永不为
右侧的搬迁目的地。

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

## 五、配置宏

| 宏               | 默认  | 含义                                 |
| ---------------- | ----- | ------------------------------------ |
| `LC_FANOUT`      | 62    | 内节点最大子数                       |
| `LC_LEAF_FANOUT` | 62    | 叶最大行数                           |
| `LC_MAX_LEVEL`   | 16    | 最大树深（paths 数组大小）           |
| `LC_PAGE_SIZE`   | 65536 | 池分配器页大小                       |
| `LC_STATIC_API`  | —     | 定义后所有函数为 static (单文件嵌入) |
| `LC_POOL_STATS`  | —     | 定义后统计池中 live objects          |
