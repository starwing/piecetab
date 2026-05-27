# piecetab `ptI_splitpiece` 源码分析

> 摘自 `piecetab.h`，分析 `ptI_splitpiece`、`ptI_takegapright` 及 `ptI_init` 中与 splitpiece 相关之代码段。
> 日期：2026-05-28

---

## 一、核心数据结构速查

### pt_Cursor（行 148-165）

```c
struct pt_Cursor {
    struct pt_Node **paths[PT_MAX_LEVEL]; // 每层指向 parent->children[] 中的某个槽位
    struct pt_Tree  *snap;                // 当前操作之快照
    struct pt_Tree  *oldsnap;             // 变更前之快照，供回滚
    short            dirty;               // 游标是否有未提交变更
    unsigned short   pbr;                 // 当前 piece 内 poff 之前的换行数
    unsigned short   poff;                // 当前 piece 内的字节偏移
    pt_Offset        off;                 // 整棵树中的绝对字节偏移
    pt_Offset        remain;              // 到下一个换行符之剩余字节数（取自 gap 缓存）
    pt_LineCol       linecol;             // 行号与列号；列可为 -1（未知）
};
```

- **`poff`**：欲插入位置在当前 piece 内的字节偏移。0 表 piece 开头，=size 表 piece 末尾。
- **`pbr`**：`poff` 之前在此 piece 中的换行总数。
- **`paths[l]`**：指向第 `l` 层 parent 的 `children[]` 中"gap 所在槽位"之指针。leaf 层 `paths[levels]` 指向当前 piece 槽。

### pt_Piece（行 197-201）

```c
typedef struct pt_Piece {
    pt_PieceSize ends[PT_PIECE_MAXLINES]; // 换行位置+1 之偏移数组
    unsigned     version;
    const char  *data;                    // 数据指针
} pt_Piece;
```

- `ends[i]` 存的是 `data` 中第 `i` 个换行符后下一字节的偏移量（即 `linebreak+1`）。
- 若 piece 无换行，则 `breaks=0`，`ends` 无有效项。

### pt_Node（行 205-212）

```c
struct pt_Node {
    pt_Size        bytes[PT_MAX_FANOUT];  // 各 child 子树之总字节数
    pt_Size        chars[PT_MAX_FANOUT];  // 各 child 子树之总字符数
    pt_Size        breaks[PT_MAX_FANOUT]; // 各 child 子树之总换行数
    unsigned       version;
    unsigned short child_count;
    pt_Node       *children[PT_MAX_FANOUT]; // leaf 层存 pt_Piece*
};
```

### pt_Insert（行 766-774）

```c
typedef struct pt_Insert {
    pt_Cursor  *c;
    const char *s;
    size_t      len;
    pt_Node *parents[PT_MAX_LEVEL]; // 记录 c->paths 各层之原始父节点（COW 前快照）
    pt_Node  pend[PT_MAX_LEVEL];    // 待安装的节点/piece（pending）
    pt_Node *pend_root;             // splitroot 时预分配的空节点
} pt_Insert;
```

### 关键宏

```c
#define ptC_levels(c) ((c)->snap->levels)
#define ptC_parent(c, l) ((l) ? *(c)->paths[(l) - 1] : &c->snap->root)
#define ptC_idx(c, p, l) ((c)->paths[(l)] - (p)->children)
```

- `ptC_levels(c)`：树高。leaf 编号 = levels，root 编号 = 0。
- `ptC_parent(c, l)`：第 `l` 层的父节点。
- `ptC_idx(c, p, l)`：`paths[l]` 在 parent 的 `children[]` 中之索引。

---

## 二、`ptI_splitpiece` — 切开当前 piece（行 831-855）

### 完整源码

```c
static int ptI_splitpiece(pt_Insert *x) {
    pt_State *S = x->c->snap->S;
    pt_Node  *pend = &x->pend[ptC_levels(x->c)];
    pt_Node  *parent = ptC_parent(x->c, ptC_levels(x->c));
    int       pi, i = ptC_idx(x->c, parent, ptC_levels(x->c));
    pt_Piece *r, *l = (pt_Piece *)parent->children[i];
    pt_Size   breaks = parent->breaks[i], size = parent->bytes[i];
    if (x->c->poff == 0 || x->c->poff == size) return PT_OK;
    if (!(r = (pt_Piece *)pt_poolalloc(S, &S->pieces))) return PT_ERRMEM;
    if (breaks > 0) {
        for (pi = x->c->pbr; pi < breaks; ++pi)
            r->ends[pi - x->c->pbr] = l->ends[pi] - x->c->poff;
        parent->breaks[i] = x->c->pbr, pend->breaks[0] = (breaks -= x->c->pbr);
    }
    if (breaks == 0 || x->c->pbr == 0) {
        pt_Size chars = parent->chars[i], remain = size - x->c->poff;
        parent->chars[i] = ptC_measure(x->c, 0, x->c->poff);
        pend->chars[0] = chars ? chars - parent->chars[i]
                               : ptC_measure(x->c, x->c->poff, remain);
    }
    parent->bytes[i] = x->c->poff, pend->bytes[0] = size - x->c->poff;
    r->version = x->c->snap->version, r->data = l->data + x->c->poff;
    pend->children[0] = (pt_Node *)r, pend->child_count = 1;
    return PT_OK;
}
```

### 逐行注解

#### 第一段：定位当前 piece 与上下文

```c
pt_State *S = x->c->snap->S;
pt_Node  *pend = &x->pend[ptC_levels(x->c)];                             // (a)
pt_Node  *parent = ptC_parent(x->c, ptC_levels(x->c));                   // (b)
int       pi, i = ptC_idx(x->c, parent, ptC_levels(x->c));               // (c)
pt_Piece *r, *l = (pt_Piece *)parent->children[i];                       // (d)
pt_Size   breaks = parent->breaks[i], size = parent->bytes[i];            // (e)
```

- **(a)** `pend` 取 leaf 层（`levels`）之 `pend` 槽——右半 piece 将暂存于此。
- **(b)** `parent` 取 leaf 层的父节点。
- **(c)** `i` 为当前 piece 在 parent 的 `children[]` 中的索引。
- **(d)** `l` 是当前 piece 的指针（即被切之 piece）。leaf 层的 `children[i]` 存的是 `pt_Piece*`，故需强制转换。
- **(e)** 快照 `breaks` 和 `size`——当前 piece 的换行数和字节数。

#### 第二段：边界条件

```c
if (x->c->poff == 0 || x->c->poff == size) return PT_OK;
```

若 cursor 恰在 piece 边界（开头或末尾），则无需切开。这是"无操作"快速路径。

#### 第三段：分配右半 piece

```c
if (!(r = (pt_Piece *)pt_poolalloc(S, &S->pieces))) return PT_ERRMEM;
```

从 piece 池中分配一个新 piece `r`，作为切开后的右半。

#### 第四段：换行信息分裂（含换行者）

```c
if (breaks > 0) {
    for (pi = x->c->pbr; pi < breaks; ++pi)
        r->ends[pi - x->c->pbr] = l->ends[pi] - x->c->poff;
    parent->breaks[i] = x->c->pbr, pend->breaks[0] = (breaks -= x->c->pbr);
}
```

此段仅当原 piece 有换行时执行：

- **复制右半换行偏移**：`l->ends[pbr..breaks)` 是原 piece 中 `poff` 之后的换行偏移（皆为绝对偏移）。右半 piece `r` 的偏移应从 0 重新计算，故减去 `poff`。
- **更新度量**：
  - `parent->breaks[i]` = `pbr`，即左半（保留在原 slot 的 piece）的换行数为 `pbr`。注意此处**修改了原 parent 的 breaks 槽**——左半的 piece 本身没有被新分配，而是原地修改了原 piece（`l`）在 parent 中的度量记录。原 piece 对象 `l` 未变，但其在树中的字节/换行/字符度量被 parent 槽缩减。
  - `pend->breaks[0]` = `breaks - pbr`，即右半的换行数。
  - 同时 `breaks` 就地更新为右半换行数，供下文判断用。

#### 第五段：字符数分裂（`chars`）

```c
if (breaks == 0 || x->c->pbr == 0) {
    pt_Size chars = parent->chars[i], remain = size - x->c->poff;
    parent->chars[i] = ptC_measure(x->c, 0, x->c->poff);
    pend->chars[0] = chars ? chars - parent->chars[i]
                           : ptC_measure(x->c, x->c->poff, remain);
}
```

字符数分裂有两条路径：

- **有换行且 pbr>0**：此时 `ptC_measure` 的结果可能不准（因为 measure 基于当前 piece 的原始数据指针），故跳过此步。左半 chars 后续由上层通过 `ptC_charsfromdelta` 之类的机制推算。
- **无换行（breaks==0）或 pbr==0（左半无换行）**：
  - 若原 chars 已计算过（`chars > 0`），则左半 chars = 重新测量 `[0, poff)`，右半 = 原 chars - 左半 chars。
  - 若原 chars 为 0（未知），则分别测量左右两半。

`ptC_measure(c, off, len)` 通过调用 `charsf` 回调来计算指定字节区间中的字符数（返回实际消耗的字节数，chars 通过间接参数 `col` 传出）。

**关键点**：此处的条件 `breaks == 0 || x->c->pbr == 0` 意在规避换行存在时 `ptC_measure` 可能不准确的情况。当 piece 有换行时，`ptC_measure` 测量的是**当前 cursor 指向的原 piece**，但原 piece 的 `data` 指针和 `ends` 数组仍对应完整的原 piece 数据——所以测量 `[0, poff)` 在数据层面是正确的。条件"pbr==0"时跳过 chars 计算，可能是因为 pbr>0 时需特殊处理 chars-runs（多字节字符跨越换行边界等复杂情况）。

#### 第六段：字节数分裂

```c
parent->bytes[i] = x->c->poff, pend->bytes[0] = size - x->c->poff;
```

- `parent->bytes[i]`（原 slot）更新为 `poff`（左半字节数）。
- `pend->bytes[0]` 设为 `size - poff`（右半字节数）。

#### 第七段：构造右半 piece，置入 pend

```c
r->version = x->c->snap->version, r->data = l->data + x->c->poff;
pend->children[0] = (pt_Node *)r, pend->child_count = 1;
return PT_OK;
```

- 右半 piece `r` 的 `data` 指针 = 原 piece `l` 的 data + `poff`（共享同一块底层 buffer，零拷贝语义）。
- 右半 piece 放入 `pend[levels]` 的 `children[0]`。
- `pend[levels].child_count = 1` 表示有一个待安装 piece。

---

## 三、splitpiece 之后置条件

调用 `ptI_splitpiece` 后，树状态如下：

```
原状态（插入前）：
    parent->children[i] = l（完整 piece，字节数 = size）

后置状态（splitpiece 后）：
    parent->bytes[i]  = poff（左半字节数）
    parent->breaks[i] = pbr（左半换行数）
    parent->chars[i]  = 左半字符数（若已计算）
    parent->children[i] 仍指向原 piece l（但其有效范围为 [0, poff)）

    pend[levels].children[0] = r（新分配的右半 piece）
    pend[levels].bytes[0]    = size - poff
    pend[levels].breaks[0]   = breaks - pbr
    pend[levels].chars[0]    = 右半字符数
    pend[levels].child_count = 1
```

**关键**：splitpiece 之后，cursor 的 `poff` / `pbr` **并未被重置**。左半留在原 slot 不变；右半驻留在 `pend[levels]` 中，等待后续 `ptI_flush` 将其"安装"到 gap 之后。

---

## 四、`ptI_takegapright` — 取出 gap 右侧后缀（行 933-941）

```c
static void ptI_takegapright(pt_Insert *x, int l, pt_Node *suffix) {
    pt_Node *parent = x->parents[l];
    int      i = ptC_idx(x->c, parent, l);
    suffix->child_count = 0;
    if (i + 1 == parent->child_count) return; /* 右侧无 gap */
    ptI_move(suffix, 0, parent, i + 1, parent->child_count - i - 1);
    ptI_subdelta(x, l, ptI_sumrange(suffix, 0, suffix->child_count));
    parent->child_count = i + 1;
}
```

### 语义

- `suffix` 初始化为空。
- 若 `i+1 == parent->child_count`（gap 已在最右），则无右侧子节点，直接返回。
- 否则，将 `parent` 中 `[i+1..child_count)` 的所有子节点移至 `suffix`。
- 同时从 `x->parents[l]` 向上逐层减去这些子节点的度量（`ptI_subdelta`）。
- 最后将 `parent->child_count` 截断为 `i+1`。

### 用途

`takegapright` 是 `ptI_flush` 和 `ptI_splitinsert` 中的第一步：先将当前 gap 右侧的"旧树后缀"摘出到一个临时 `suffix` 节点中。后续 flush 会在 gap 后填入新的 pending child，并尽可能将 `suffix` 中的旧节点回填到 parent 剩余空间。

在 splitpiece 场景中，`ptI_flush(x, levels, 0)` 会先 `takegapright` 取出旧右侧，然后将 `pend[levels]` 中的右半 piece 安装到原 piece 之右。

---

## 五、`ptI_init` — 初始化插入上下文（行 857-880）

### 完整源码

```c
static int ptI_init(pt_Insert *x, pt_Cursor *c, const char *s, size_t len) {
    pt_Tree  *snap = c->snap;
    pt_State *S = snap->S;
    int       l, i, tc, r;
    x->c = c, x->s = s, x->len = len, x->pend_root = NULL;
    if (!c->dirty) { /* 创建新树（COW） */
        if (!(snap = (pt_Tree *)pt_poolalloc(S, &S->trees))) return PT_ERRMEM;
        *snap = *c->snap, c->oldsnap = c->snap, c->snap = snap, c->dirty = 1;
        snap->version = ++snap->S->max_version;
        snap->root.version = snap->version;
    }
    if (ptC_bytes(c) == 0 && (r = ptI_fillroot(x)) != PT_OK) return r;
    for (l = 0; l <= ptC_levels(c); ++l) {
        pt_Node *n, *parent = ptC_parent(c, l), **cs = parent->children;
        i = ptC_idx(c, parent, l), tc = parent->child_count;
        x->parents[l] = parent, x->pend[l].child_count = 0;
        if (l < ptC_levels(c) && i < tc && cs[i]->version != snap->version) {
            if (!(n = (pt_Node *)pt_poolalloc(S, &S->nodes))) return PT_ERRMEM;
            c->paths[l + 1] = &n->children[c->paths[l + 1] - cs[i]->children];
            *n = *cs[i], cs[i] = n, n->version = snap->version;
        }
    }
    return ptI_splitpiece(x);
}
```

### 逐段注解

#### 第一段：COW（Copy-On-Write）快照

```c
if (!c->dirty) {
    if (!(snap = (pt_Tree *)pt_poolalloc(S, &S->trees))) return PT_ERRMEM;
    *snap = *c->snap, c->oldsnap = c->snap, c->snap = snap, c->dirty = 1;
    snap->version = ++snap->S->max_version;
    snap->root.version = snap->version;
}
```

若 cursor 尚未标记为 dirty（此前无变更），则从树池中分配一个新 `pt_Tree`，将旧树整体浅拷贝过去。新树获得新版本号，root 版本号亦更新。

**关键**：此处的 `*snap = *c->snap` 是**浅拷贝**（struct 赋值），整棵树的所有 node 和 piece 指针均为共享。后续 COW 只复制路径上的节点。

#### 第二段：空树快速路径

```c
if (ptC_bytes(c) == 0 && (r = ptI_fillroot(x)) != PT_OK) return r;
```

若树为空（总字节数为 0），则直接调用 `ptI_fillroot` 在 root 层生成 piece（root 即 leaf）。此时无需 splitpiece，因为无旧 piece 可切。

#### 第三段：COW 路径克隆

```c
for (l = 0; l <= ptC_levels(c); ++l) {
    pt_Node *n, *parent = ptC_parent(c, l), **cs = parent->children;
    i = ptC_idx(c, parent, l), tc = parent->child_count;
    x->parents[l] = parent, x->pend[l].child_count = 0;
    if (l < ptC_levels(c) && i < tc && cs[i]->version != snap->version) {
        if (!(n = (pt_Node *)pt_poolalloc(S, &S->nodes))) return PT_ERRMEM;
        c->paths[l + 1] = &n->children[c->paths[l + 1] - cs[i]->children];
        *n = *cs[i], cs[i] = n, n->version = snap->version;
    }
}
```

此循环遍历树的每一层：

- **记录 parents**：`x->parents[l] = parent` 保存各层父节点指针（在 COW 之前）。
- **初始化 pend**：`x->pend[l].child_count = 0` 清空所有层的待安装列表。
- **COW 克隆**：对每层 `l < levels`（非 leaf 层），若 `paths[l]` 指向的节点版本号不等于新 snapshot 版本号，则：
  1. 分配一个同类型新 node `n`
  2. 浅拷贝旧节点内容至 `n`
  3. 更新版本号
  4. 用 `n` 替换 parent 中的旧指针
  5. 修正下一层的 `paths[l+1]`，使其指向克隆节点内的对应子槽位

**注意**：leaf 层的 piece 不需要 COW，因为 splitpiece 会用新 piece 替换当前 slot。

#### 第四段：切分当前 piece

```c
return ptI_splitpiece(x);
```

初始化最后一步：调用 `ptI_splitpiece`。如果 cursor 恰在 piece 边界，则该函数直接返回 `PT_OK`；否则将当前 piece 沿 `poff` 切开，左半留在原 slot，右半入 `pend[levels]`。

---

## 六、`pt_insert` — 顶层插入流程（行 1046-1063）

```c
PT_API int pt_insert(pt_Cursor *c, const char *s, size_t len) {
    pt_Insert x;
    int       r, l, i;
    if (len == 0) return PT_OK;
    if (c == NULL || c->snap == NULL || s == NULL) return PT_ERRPARAM;
    if ((r = ptI_init(&x, c, s, len)) != PT_OK) return r;
    l = ptC_levels(c), assert(l < PT_MAX_LEVEL);
    while (x.len > 0) {
        if ((r = ptI_checkpendroot(&x)) != PT_OK) break;
        if ((r = ptI_fill(&x, &x.pend[l])) != PT_OK) break;
        if ((r = ptI_flush(&x, l, 1)) != PT_OK) break;
        l = ptC_levels(c);
    }
    if (r != PT_OK)
        for (l = 0; l <= ptC_levels(c); ++l) ptI_disposepend(&x, l);
    if (x.pend_root) pt_poolfree(&c->snap->S->nodes, x.pend_root);
    return r;
}
```

### 流程

1. **`ptI_init`**：COW 快照 + 路径克隆 + splitpiece。
2. **循环**（`x.len > 0`）：
   - `ptI_checkpendroot`：确保 splitroot 所需的备用节点已就绪。
   - `ptI_fill`：将 `x.s` 中的插入数据打包成 piece，放入 `pend[levels]`。
   - `ptI_flush(x, l, 1)`：将 `pend[levels]` 中的新 piece 安装到树中（`movegap=1` 表示推进 gap）。
   - 更新 `l`（树可能因 splitroot 而增高）。
3. **错误回滚**：若失败，释放所有 pend 中的内容。

---

## 七、`splitpiece` 在非空树中间插入时的完整示意

```
假设树中有 n 个 piece：[P0, P1, P2, ..., Pn]
cursor 指向 P1 内部的偏移 poff（0 < poff < P1.size）

Step 1: ptI_init
  - COW 快照整个树
  - 克隆路径上的内部节点
  - 调用 ptI_splitpiece

Step 2: ptI_splitpiece
  输入: cursor->poff 在 P1 内部

  P1 被切开:
    左半 P1_left:  data = P1.data,             size = poff
    右半 P1_right: data = P1.data + poff,      size = P1.size - poff
                  P1_left 留在 parent->children[i]（原 slot）
                  P1_right 放入 pend[levels].children[0]

  后置:
    树顺序: [P0, P1_left, (gap), P2, ..., Pn]
    pend[levels] = [P1_right]  ← 待安装

Step 3: ptI_fill
  将插入数据 s 打包成新 piece: newP0, newP1, ...
  pend[levels] = [P1_right, newP0, newP1, ...]（追加在右半之后）
  （注：实际 fill 填入的是 pend[levels]，追加在已有右半之后）

Step 4: ptI_flush(x, levels, 1)
  将 pend[levels] 中的 piece 依次"推入" gap 右侧:
  - takegapright: 将 [P2, ..., Pn] 摘出为 suffix
  - 将 P1_right, newP0, newP1, ... 依次装入 parent 的 gap 后
  - 若 parent 装不下，剩余打包成右兄弟上推
  - gap 推进（movegap=1）

  最终:
    树顺序: [P0, P1_left, P1_right, newP0, newP1, ..., P2, P3, ..., Pn]
```

---

## 八、关键设计要点

### 1. 左半原地不动，右半入 pend

`ptI_splitpiece` 不创建新的左半 piece。左半复用原 piece 对象 `l`，仅修改 parent 中该 slot 的度量（`bytes[i] = poff`）。这是一种"零分配"优化——左半无需新内存。

### 2. 右半共享数据

右半 piece `r->data = l->data + poff` 直接指向原 piece 数据缓冲区的偏移位置，无需拷贝数据。

### 3. chars 分裂的条件路径

`chars` 的分裂逻辑有两个路径：有换行时跳过，无换行或 pbr==0 时重新测量。此设计可能是因为 `ptC_measure` 在多段换行时的行为与 chars runs 不完全一致，故留给上层后续处理。

### 4. splitpiece 不与 flush 耦合

splitpiece 仅负责切开并放入 `pend[levels]`。真正的"安装"——将右半 piece 写入树中的正确位置——由 `ptI_flush` 负责。这种分离使得 splitpiece 职责单一。

### 5. BOF 插入的特殊性

若 `poff == 0`（在 piece 开头插入），`ptI_splitpiece` 直接返回 `PT_OK`（无操作）。这意味着 BOF 插入时不存在"切开"动作——整个当前 piece 成为 gap 右侧的旧内容，新数据将插入其前。此路径由 `ptI_flush` 和 `pt_insert` 中的局部逻辑处理（当前代码中仍在设计中）。

---

## 九、相关辅助函数

### `ptI_subdelta`（行 926-931）

```c
static void ptI_subdelta(pt_Insert *x, int l, pt_Metrics m) {
    while (--l >= 0) {
        pt_Node *parent = x->parents[l];
        ptN_submetrics(parent, ptC_idx(x->c, parent, l), m);
    }
}
```

从当前层向上，逐层减去度量 `m`。用于 `takegapright` 摘除右侧 suffix 后更新上层祖先节点的度量。

### `ptI_sumrange`（行 810-819）

```c
static pt_Metrics ptI_sumrange(const pt_Node *n, int start, int count) {
    pt_Metrics m = {0, 0, 0};
    int        i;
    for (i = 0; i < count; ++i) {
        m.bytes += n->bytes[start + i];
        m.breaks += n->breaks[start + i];
        m.chars += n->chars[start + i];
    }
    return m;
}
```

对节点 `n` 中从 `start` 开始的 `count` 个子节点，求和其度量。

### `ptC_measure`（行 723-728）

```c
static pt_Size ptC_measure(pt_Cursor *c, pt_PieceSize off, pt_PieceSize len) {
    pt_PieceSize col = len;
    pt_Slice     s = pt_slice(ptC_leaf(c)->data + off, len);
    c->snap->S->charsf(c->snap->S->chars_ud, s, &col);
    return len - col;
}
```

测量指定区间的字符数。返回消耗的字节数（`len - col`，其中 `col` 是剩余的"未消耗字节数"）。此函数修改了传入的 `col`，但最终返回的是实际消费字节数。

### `ptI_fill`（行 790-808）

```c
static int ptI_fill(pt_Insert *ctx, pt_Node *n) {
    pt_State *S = ctx->c->snap->S;
    int       i;
    for (i = n->child_count; i < PT_MAX_FANOUT && ctx->len > 0; ++i) {
        pt_LinesInfo li = {0, 0, NULL};
        pt_Slice     sl = pt_slice(ctx->s, pt_min(ctx->len, PT_PIECE_MAXSIZE));
        short        cur = n->child_count;
        pt_Piece    *p;
        if (!(p = (pt_Piece *)pt_poolalloc(S, &S->pieces))) return PT_ERRMEM;
        li.ends = &p->ends, n->bytes[cur] = S->linesf(S->lines_ud, sl, &li);
        assert(n->bytes[cur] > 0 && n->bytes[cur] <= sl.len);
        assert(li.chars <= n->bytes[cur] && li.breaks <= PT_PIECE_MAXLINES);
        n->breaks[cur] = li.breaks, n->chars[cur] = li.chars;
        p->version = ctx->c->snap->root.version, p->data = sl.s;
        n->children[n->child_count++] = (pt_Node *)p;
        ctx->s += n->bytes[cur], ctx->len -= n->bytes[cur];
    }
    return PT_OK;
}
```

将插入数据 `ctx->s` 打包为最多 `PT_MAX_FANOUT - n->child_count` 个 piece，每个 piece 不超过 `PT_PIECE_MAXSIZE` 字节。通过 `linesf` 回调检测换行位置。生成的新 piece 直接填入 `n`（可能是 `pend[levels]` 或 root）。

---

## 十、对 linecache 中 Leaf 的对应关系

在 linecache 语境中：
- piece 对应 Leaf（行数据节点）。
- `ptI_splitpiece` 对应在 Leaf 内部的某行偏移处"切开"该 Leaf——将 Leaf 沿行边界分为左半（留在原位）和右半（暂存待安装）。
- `ends[]` 数组对应 Leaf 中各行结束位置的偏移量。
- `pbr` 对应 Leaf 内切开位置之前的行数。

---

*本文档由对 `piecetab.h` 源码之直接研读生成，所有行号以当前代码版本为准。*
