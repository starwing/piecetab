# lc_insert 重构设计 (v2)

## 0. 要略

当前 `lc_insert` 有三病：**O(N^2) 性能**（`makespace(1)` 逐级 memmove）、**OOM 不安全**（裂叶不可回滚）、**代码晦涩**（`at_end`/`rt_leaf`/`applyfirst` 策略拼凑）。

新设计以四条原则重构：

1. **右半放树里，左半进 pend** — 统一裂叶与裂内节点之对称操作
2. **at[] 数组记录插入位置** — 取消 `at_end`
3. **flush 不调用 makespace** — 以"一次性劈开 → 后续追加"替代"逐次移位"
4. **每次修改树后即时维护 cursor paths** — 数据结构始终自然正确，拒绝 save/restore

`lc_scan` 自然退化为纯追加路径之特例。

### 0.1 坐标系约定

```
l=0:  根层     lcK_parent(C,0) = &c->root
l=1:  根的子层  lcK_parent(C,1) = *paths[0]
...
l=levels: 叶层  lcK_parent(C,levels) = *paths[levels-1]  注：此为叶父（内节点）

paths[l] 指向 parent(l)->children[] 中某个槽位。
at[l]   为 pend[l] 数据应插入 parent(l)->children 的索引，值域 [0, child_count]。
```

## 1. 裂分之统一原则

### 1.1 叶级裂分（insert 步骤 3）

游标在叶 Leaf[li] 之 lidx 行、col 列。新数据应出现在游标位置：col 之前的内容静止，col 字节之后的内容（含 col 所在行的剩余部分）与新插入文本合并重组。

```
原始叶 Leaf[li]:  [ bytes[0] | ... | bytes[lidx]=7 | ... ]
                   游标→  "abcdef\n"
                           col=3: "abc"残行(3字节) | "def\n"(4字节，归属右半)
```

**操作**：
1. 保存原始度量：`old_bytes_li = p->bytes[li]`，`old_breaks_li = p->breaks[li]`（供 OOM 回滚用）
2. 预分配右半新叶 `rt = lc_poolalloc(...)`（若失败树未动，直接返回 `LC_ERRMEM`）
3. 截断原叶为左半：`breaks = lidx + 1`（col>0 时 `bytes[lidx] = col`，此为残行，合并到插入首行）；若 col==0 且 lidx==0，`breaks = 0`（空叶）
4. 原叶入 pend[levels]：`pend[levels].children[0] = (lc_Node*)lf`，`pend[levels].bytes[0] = lcL_sumbytes(lf, 0, 左半breaks)`，`pend[levels].breaks[0] = 左半breaks`，`pend[levels].child_count = 1`
5. 右半数据拷入 rt：`rt->bytes[0] = orig - col`（col>0 时的残行剩余），`rt->bytes[1..] = orig[lidx+1..end]`
6. 右半替换树中位置：`p->children[li] = (lc_Node*)rt`
7. 更新父节点度量：`p->bytes[li] -= (rt 的字节合计)`，`p->breaks[li] = 左半breaks`，`lcM_up(C, levels-1, -db, -dl)`
8. 设置插入位置：`x->at[levels] = li`（pend 插入到右半叶子之前）
9. **paths 同步**：`paths[levels]` 仍指向 `p->children[li]`（同一槽位），内容从原叶变为右半叶。裂叶后游标的"树中视图"切换为右半叶——此为刻意设计，因为 pend 中的左半残行 + 插入文本将在此处插入。

**OOM 安全**：步骤 2 的预分配确保裂叶操作本身不产生 OOM（rt 已分配才修改树）。

### 1.2 内层裂分（flush 步骤 2，仅 l < levels-1）

> 叶层裂分已由 Section 1.1 完成，flush 步骤 2 仅负责内部节点裂分。n 必须为真实的 `lc_Node`（非叶子），故限制 `l < levels-1`。

```
parent(l) 的 children:  [ ... | children[at[l]-1]=n | children[at[l]] | ... ]
                              ← n 的左半 →|← n 的右半 →→
                                         ↑ at[l+1] 处分界
```

**操作**：
1. `n = parent(l)->children[at[l] - 1]`（l+1 层之内部节点）
2. 预分配新节点 `np = lc_poolalloc(...)`（若失败树未动，返回 `LC_ERRMEM`）
3. 保存各层 paths 偏移（供步骤 10 同步用）：`k[i] = paths[i] - n->children`（对 i = l+1 .. levels 中 paths[i] 落在 n->children 者）
4. `lcN_copy(np, 0, n, at[l+1], n->child_count - at[l+1])` — n 的右半 children 拷入 np
5. `n->child_count -= (np->child_count)`，同步截断 n->bytes/breaks（左半度量在有效范围内自然缩减）
6. `parent(l)->children[at[l] - 1] = np` — 右半 np 留树中
7. 更新父节点度量：`parent(l)->bytes[at[l]-1] = lcN_sumbytes(np, 0, np->child_count)`，同理 `parent(l)->breaks`；差额通过 `lcM_up(C, l-1, -db, -dl)` 向上传播
8. `at[l]--` — 插入位置回退
9. `pend[l].children[pend[l].child_count++] = n` — 左半 n 入 pend（n 自身度量从其 child_count 推定）
10. **paths 同步**：对 i = l+1 .. levels，若 paths[i] 原先在 n->children 内且 k[i] >= at[l+1]（右半，已拷入 np），重定向至 `&np->children[k[i] - at[l+1]]`。若 k[i] < at[l+1]（左半，仍在 n 中），paths[i] 保持不变（n 暂时在 pend 中，待步骤 3 合并回树后自然恢复）。

### 1.3 对称性

| 层 | 裂分时机 | 左半去向 | 右半去处 | paths 维护 |
|----|---------|---------|---------|-----------|
| 叶 (levels) | insert 步骤 3 | pend[levels] | p->children[li]（新 rt） | 自然正确（同槽位换对象） |
| 内 (l<levels-1) | flush 步骤 2 | pend[l]（n） | p->children[at[l]-1]（新 np） | paths[l+1..levels] 按左右半重定向 |

此对称结构与 splice 算法同源：先 trimleaf/shiftleaf（叶层），再 trimnode/shiftnode（内层），prune 后自上而下 foldnode/mergeleaf。

## 2. `at[]` 数组与 at_end 消除

### 2.1 方案

在 `lcB_Ctx` 中新增 `unsigned short at[LC_MAX_LEVEL]`，存储每层 pend 数据插入 parent->children 的索引。

```c
typedef struct lcB_Ctx {
    lc_Cursor       c;
    lc_Node         pend[LC_MAX_LEVEL];
    lc_Node        *pend_root;
    unsigned short  at[LC_MAX_LEVEL];
} lcB_Ctx;
```

**at 值语义**：

| 模式 | at[l] 初值 | 说明 |
|------|-----------|------|
| append（空树/locend） | parent(l)->child_count | pend 追加到 children 末尾 |
| insert | lcK_idx(C, parent(l), l) + 1 | +1 使插入点在游标所在孩子之右侧 |
| 裂叶后 | li（由 li+1 缩回） | 左半移入 pend 后，插入点缩至右半之前 |

### 2.2 at_end 消除

旧代码：
```c
at = x->at_end ? (int)parent->child_count : (int)(x->c.paths[l] - parent->children) + 1;
```
新代码：
```c
at = x->at[l];
```

### 2.3 类型选择

择 `unsigned short`（2B×16=32B），覆盖 FANOUT 任意合理取值。

### 2.4 at[] 随 flush 进度推进

flush 步骤 3 将 pend[l]（n 个孩子）合并至 parent(l) 的 at[l] 处。合并后 children 从 at[l] 处多了 n 个槽位，原位置之后的孩子后移。下一轮 fill-flush 的插入点应跟随其后：

```
合并后: at[l] += n  // 指向上次合并数据末尾，即"还在右半之前"
```

## 3. flush 算法

### 3.1 核心接口

```c
static int lcB_flush(lcB_Ctx *x, int l, int *has_more_out);
```

**前置**：pend[l].child_count > 0
**保证**：返回成功时 pend[l].child_count == 0；`has_more_out` 指示上层是否仍有 pend 数据待处理（供外层循环续调）
**语义**：flush 处理单层（l 层），将该层的 pend 数据写入 parent(l) 或打包至上层 pend，返回后由外层循环决定是否继续处理更低层。

### 3.2 外层调用框架

参考现有 `lcB_fillflush` 模式：

```c
// 在 lc_insert 的步骤 5 中
int lv = levels;
int r = lcB_fill(x, lv, scanner, ud);
while (r > 0) {
    if ((r = lcB_checkpendroot(x)) != LC_OK) break;
    if ((r = lcB_flush(x, lv, &has_more)) != LC_OK) break;
    lv = (int)c->levels;
    r = lcB_fill(x, lv, scanner, ud);
}
if (r >= 0) r = lcB_flushfinal(x, lv);  // 最终驱尽所有 pend 层
```

`lcB_flushfinal` 自 lv 向下迭代至 0，逐层调用 flush 逻辑直至所有 pend 清空。

### 3.3 算法步骤

`flush_one` 为**单层**处理函数：仅处理入参 l 层之 pend 数据。若需打包至上层则返回 `LC_AGAIN`（表示此行未完，上层仍有 pend 待驱），外层循环据此续调。选定单层语义因其与现有 `lcB_fillflush` 的"一层一次 flush"模式一致。

```
flush_one(x, l):
  若 pend[l].child_count == 0: return LC_OK

  parent = lcK_parent(&x->c, l)

  // 步骤 2: 内层裂分（仅 l < levels-1 且非 append）
  // 裂分约束：若 at[l+1] 或 n->child_count - at[l+1] < FANOUT/2，跳过裂分
  if l < (int)c->levels - 1 && x->at[l] != (int)parent->child_count:
    n = parent->children[x->at[l] - 1]
    if x->at[l+1] >= LC_FANOUT/2 && (int)n->child_count - x->at[l+1] >= LC_FANOUT/2:
      保存路径偏移 k[i] = paths[i] - n->children（对 i=l+1..levels）
      np = alloc_node()
      if (!np) return LC_ERRMEM
      unsigned short nc = n->child_count - x->at[l+1]
      lcN_copy(np, 0, n, x->at[l+1], nc)
      np->child_count = nc
      n->child_count -= nc
      // db = lcN_sumbytes(n, 0, at[l+1])  (左半 bytes 合计，即移出量)
      // dl = lcN_sumbreaks(n, 0, at[l+1])
      lc_Diff db = (lc_Diff)lcN_sumbytes(n, 0, x->at[l+1])
      lc_Diff dl = (lc_Diff)lcN_sumbreaks(n, 0, x->at[l+1])
      parent->bytes[x->at[l]-1] = lcN_sumbytes(np, 0, (int)np->child_count)
      parent->breaks[x->at[l]-1] = lcN_sumbreaks(np, 0, (int)np->child_count)
      lcM_up(&x->c, l-1, -db, -dl)
      parent->children[x->at[l] - 1] = np
      x->at[l]--
      pend[l].children[pend[l].child_count++] = n
      // paths 同步：右半重定向至 np
      for i = l+1 .. levels:
        若 paths[i] 在 n->children 内且 k[i] >= x->at[l+1]:
          paths[i] = &np->children[k[i] - x->at[l+1]]

  // 步骤 3: 直接合并到 parent(l)
  if parent->child_count + pend[l].child_count <= LC_FANOUT:
    size_t old_total = lcN_sumbytes(parent, 0, parent->child_count)
    lcN_makespace(parent, x->at[l], pend[l].child_count)
    lcN_copy(parent, x->at[l], pend[l], 0, pend[l].child_count)
    size_t new_total = lcN_sumbytes(parent, 0, parent->child_count)
    lcM_up(&x->c, l-1, (lc_Diff)(new_total - old_total),
           (lc_Diff)(lcN_sumbreaks(parent, 0, parent->child_count) -
                     lcN_sumbreaks(parent, 0, parent->child_count - pend[l].child_count)))
    paths[l] = &parent->children[x->at[l] + pend[l].child_count - 1]
    x->at[l] += pend[l].child_count
    pend[l].child_count = 0
    return LC_OK

  // parent(l) 满了 → 打包 pend[l] 进 pend[l-1]
  if l == 0:
    lcB_rootpush(x, pend[0], ...)  // 见 3.3.2
    return LC_OK

  // 步骤 5-7: 打包（pend 内操作，不涉树，无需 lcM_up）
  if pend[l-1].child_count == 0:
    newNode = alloc_node()
    if (!newNode) return LC_ERRMEM
    newNode->child_count = 0
    pend[l-1].children[0] = newNode
    pend[l-1].bytes[0] = 0
    pend[l-1].breaks[0] = 0
    pend[l-1].child_count = 1
  lastNode = pend[l-1].children[pend[l-1].child_count - 1]
  for each child at index i in pend[l]:
    if lastNode->child_count == LC_FANOUT:
      newNode = alloc_node()
      if (!newNode) return LC_ERRMEM
      newNode->child_count = 0
      pend[l-1].children[pend[l-1].child_count++] = newNode
      // pend[l-1].bytes/breaks 扩展一项（初值 0，后续追加时更新）
      lastNode = newNode
    lastNode->children[lastNode->child_count] = pend[l].children[i]
    lastNode->bytes[lastNode->child_count] = pend[l].bytes[i]
    lastNode->breaks[lastNode->child_count] = pend[l].breaks[i]
    lastNode->child_count++
    // 更新 pend[l-1] 最后一项度量 = lcN_sumbytes(lastNode, ...)
    int li = pend[l-1].child_count - 1
    pend[l-1].bytes[li] = lcN_sumbytes(lastNode, 0, lastNode->child_count)
    pend[l-1].breaks[li] = lcN_sumbreaks(lastNode, 0, lastNode->child_count)
  pend[l].child_count = 0
  return LC_AGAIN  // 上层（l-1）仍有 pend 待驱
```

### 3.3.1 外层迭代逻辑

```c
static int lcB_flush_range(lcB_Ctx *x, int lv) {
    int l, r;
    for (l = lv; l >= 0; ) {
        if (x->pend[l].child_count == 0) { l--; continue; }
        r = flush_one(x, l);
        if (r != LC_OK && r != LC_AGAIN) return r;
        if (r == LC_OK) l--;    // 此层已驱尽，下移
        // LC_AGAIN: l 不变（打包后 pend[l-1] 有新数据，下一轮处理 l-1）
    }
    return LC_OK;
}
```

`flush_range` 即为 Section 5 中 `lcB_flushfinal` 的实现。`flush_one` 为单层原子操作——成功清空 l 层或将其打包至 l-1——循环确保所有层最终驱尽。

### 3.3.1 步骤 2 之 `x->at[l+1]` 下标推导

`x->at[l+1]` 为 l+1 层的插入位置索引。裂分 n 时，右半 = n->children[at[l+1]..end)。

- 叶层（l+1 = levels）：裂叶后 `at[levels] = li`（右半叶子索引）。n 为叶父。lcN_copy 从 at[levels] 开始拷右半 children。✓
- 内层（l+1 < levels）：初始 `at[l+1] = 原 idx + 1`。n 为 l+1 层父节点。lcN_copy 从 at[l+1] 开始拷 `children[原idx+1..end)`。✓

**无需 -1**：at 始终是"插入位置"，恰好也是"右半起始索引"。lcN_copy 源起始 = at[l+1]，统一含括所有层。

### 3.4 步骤 2 裂分约束（简化步骤 6）

步骤 2 裂分 n 时，左半（n 自身）大小 = at[l+1]，右半（np）大小 = n->old_child_count - at[l+1]。为避免后续重平衡，裂分时保证两边均 ≥ FANOUT/2（若游标位置导致一侧不足，则此层不裂分——改为将整 n 留树，pend 数据以 makeplace 方式挤入）。**此约束下步骤 6 退化为空操作**。

### 3.5 性能分析

**旧算法**：每轮每层 makespace → O(N^2)。
**新算法**：每层至多一次 makespace。溢出打包后该层后续 flush 纯追加（步骤 3 合并），性能 O(N)。

## 4. fill 算法

### 4.1 接口

```c
static int lcB_fill(lcB_Ctx *x, lc_Leaf *leaf, lc_Scanner *sc, void *ud, unsigned *col_inout);
```

**前置**：leaf 为已分配叶子，可能已有部分数据（左半裂叶，末段为残行）
**行为**：从 leaf 的当前 breaks 末尾开始填充，直至 leaf 满或 scanner 返回 0

`col_inout`：输入为残行字节数（裂叶时 col>0 的余留），输出为下一叶子的残行。首 scanner 行与残行合并为一行。
`x`：仅用于读取 `x->c.tree->bytes`（scanner 偏移基准）。

**返回**：0 = scanner 耗尽，1 = leaf 已满

### 4.2 实现

```c
static int lcB_fill(lcB_Ctx *x, lc_Leaf *lf, lc_Scanner *sc, void *ud, unsigned *col_inout) {
    size_t   bytes = x->c.tree->bytes;
    int      start = /* leaf 当前 breaks */;
    unsigned br;
    if (start == 0 || *col_inout) {
        br = sc(ud, bytes);
        if (!br) {
            if (*col_inout) {
                lf->bytes[start] = *col_inout;  /* 残行单独成段 */
                *col_inout = 0;
                return 0;
            }
            return 0;
        }
        lf->bytes[start] = *col_inout + br;     /* 残行 + 首 scanner 行合并 */
        *col_inout = 0;
        if (++start >= LC_LEAF_FANOUT) return 1;
        bytes += br;
    }
    for (int i = start; i < LC_LEAF_FANOUT; ++i) {
        br = sc(ud, bytes);
        if (!br) return 0;
        lf->bytes[i] = br;
        bytes += br;
    }
    return 1;
}
```

**残行处理示例**：原始 "abcdef\n"（7 字节），col=3，插入 "xyz\n"。col_inout=3（"abc"）。首 scanner 返回 4（"xyz\n" 长）。bytes[0]=3+4=7（"abcxyz\n"）。✓

### 4.3 外层调用框架与度量维护

```c
lc_insert(...) {
    ...
    // 步骤 3: 裂叶，左半入 pend[levels]
    lc_Leaf *first = (lc_Leaf *)pend[levels]->children[0];
    unsigned col_inout = (unsigned)C->col;

    // 步骤 4: fill 循环
    lc_Leaf *cur = first;
    for (;;) {
        int r = lcB_fill(&x, cur, scanner, ud, &col_inout);
        if (r == 0) break;
        lc_Leaf *next = lc_poolalloc(...);
        if (!next) goto fail;
        pend[levels]->children[pend[levels]->child_count++] = (lc_Node *)next;
        cur = next;
    }
    // fill 后统一重算 pend[levels] 之度量（bytes/breaks）
    for (unsigned pi = 0; pi < pend[levels].child_count; ++pi) {
        lc_Leaf *lf = (lc_Leaf *)pend[levels].children[pi];
        int bi = (int)pend[levels].breaks[pi];  // 裂叶时设的初值，fill 后未变
        // 若 fill 追加了行：从 bi 起统计新 bytes
        pend[levels].bytes[pi] = lcL_sumbytes(lf, 0, bi);
        // breaks 在 fill 中已自然增长（新行写入 bytes 数组，计数由外层追踪）
    }
    ...
}
```

**关键**：裂叶时 `pend[levels].breaks[0]` 已设为左半段数。fill 追加新段后，pend[levels].breaks[0] 需重设为 `lf->breaks`（左半 breaks + 新增 breaks 之和）。简化为 `pend[levels].breaks[pi] = lc_Leaf_breaks(lf)`——需追踪 fill 填入了多少段。替代方案：fill 返回时顺便更新 `pend[lv].breaks[pi]`（传 `unsigned short *breaks_out` 参数）。

**前置**：leaf 为已分配叶子，可能已有部分数据（左半裂叶，末段为残行）
**行为**：从 leaf 的当前 breaks 末尾开始填充，直至 leaf 满或 scanner 返回 0

`col_inout`：输入为残行字节数（裂叶时 col>0 的余留），输出为下一叶子的残行。首 scanner 行与残行合并为一行。

**返回**：0 = scanner 耗尽，1 = leaf 已满

### 4.2 实现

```c
static int lcB_fill(lc_Leaf *lf, lc_Scanner *sc, void *ud, unsigned *col_inout) {
    size_t   bytes = x->c.tree->bytes;
    int      start = /* leaf 当前 breaks */;
    unsigned br;
    if (start == 0 || *col_inout) {
        br = sc(ud, bytes);
        if (!br) {
            if (*col_inout) {
                lf->bytes[start] = *col_inout;  /* 残行单独成段 */
                *col_inout = 0;
                return 0;
            }
            return 0;
        }
        lf->bytes[start] = *col_inout + br;     /* 残行 + 首 scanner 行合并 */
        *col_inout = 0;
        if (++start >= LC_LEAF_FANOUT) return 1;
        bytes += br;
    }
    for (int i = start; i < LC_LEAF_FANOUT; ++i) {
        br = sc(ud, bytes);
        if (!br) return 0;
        lf->bytes[i] = br;
        bytes += br;
    }
    return 1;
}
```

## 5. lc_insert 总流程

```
lc_insert(C, e, scanner, ud):
  ┌─ 1. 参数校验
  │
  ├─ 2. 初始化 lcB_Ctx x
  │    memset(&x, 0, sizeof(x)); x.c = *C
  │    判 trailing = (old_off >= c->bytes || c->root.child_count == 0)
  │    trailing → for l in 0..levels: x.at[l] = parent(l)->child_count
  │          非 → for l in 0..levels: x.at[l] = lcK_idx(&x.c, parent(l), l) + 1
  │
  ├─ 3. 裂叶（若非 trailing）
  │    【快速短路】若 col==0 && lidx==0:
  │      br = scanner(ud, c->bytes)  // 预读首个 break
  │      if (!br): 无新行 — 类 lcB_skipinsert 原地调整 col+e，直接返回 OK
  │    预分配右半新叶 rt（OOM → 直接返回错误）
  │    保存 p->bytes[li], p->breaks[li] 旧值（供回滚）
  │    原叶截为左半、右半拷入 rt、入 pend、更新度量、lcM_up
  │    x.at[levels] = li
  │    col_inout = C->col
  │
  │    【若 trailing：分配空叶入 pend[levels]，col_inout = 0】
  │
  ├─ 4. fill 循环（lv = levels）
  │    cur = pend[lv].children[pend[lv].child_count - 1]
  │    for (;;):
  │      r = lcB_fill(cur, scanner, ud, &col_inout)
  │      if r == 0: break
  │      next = lc_poolalloc(...); if (!next) goto fail
  │      pend[lv].children[pend[lv].child_count++] = (lc_Node *)next
  │      cur = next
  │
  ├─ 5. flush 循环（类 lcB_fillflush 模式）
  │    lv = c->levels
  │    r = lcB_fill(x, lv, scanner, ud)
  │    while (r > 0):
  │      lcB_checkpendroot(x)
  │      lcB_flush_range(x, lv)   // 自顶向下驱尽所有 pend 层
  │      lv = c->levels            // flush 可能触发 rootpush 增 levels
  │      r = lcB_fill(x, lv, scanner, ud)
  │    if r >= 0: lcB_flush_range(x, lv)  // 驱尽最后一轮残余 pend
  │
  ├─ 6. e 值处理
  │    树中右半叶子的末 bytes 追加 e 字节
  │
  └─ 7. 释放 pend_root；用 x.c 覆盖 C；返回 LC_OK
```

### 5.1 OOM 回滚（步骤 4 fail 标记）

```
fail:
  释放 pend[levels].children[1..child_count-1] 所有已分配叶子（lc_poolfree）
  lcB_unsplitleafat(&x, old_bytes_li, old_breaks_li, old_col)
  // 将 pend[levels].children[0]（左半原叶）放回树中
  // 释放右半叶子 rt；恢复度量；lcM_up 反向
  释放 pend_root（若有）
  return LC_ERRMEM
```

### 5.2 lc_scan 退化

```c
LC_API int lc_scan(lc_Cache *c, lc_Scanner *scanner, void *ud) {
    lcB_Ctx x;
    lcB_initend(&x, c);  // locend + at[l] = child_count for all l
    return lcB_fillflush(&x, scanner, ud, (int)c->levels);
}
```

## 6. OOM 安全

三级防线：

1. **全预分配**：裂叶所需 rt、flush 步骤 2 所需 np——在修改树前分配。失败则树零修改，直接返回。
2. **裂叶逆操作**：若裂叶后、fill 中 OOM，逆行裂叶（释放 pend 中后填叶子，左半原叶放回树，释放 rt，恢复度量）。
3. **splice 回滚**（后备）：flush 已将部分 pend 挂入树后 OOM，用 splice 删除已写入部分。

## 7. 与当前算法逐项对比

| 维度 | 当前 | 新 |
|------|------|-----|
| 插入位置 | `at_end` 布尔 | `unsigned short at[16]` |
| 初始化 | `lcB_init`/`lcB_initat` 二择 | 单一 + at[l] 赋值 |
| 首个 br | `lcB_applyfirst` 特判 | fill 统一接管 |
| 裂叶 | `rt_leaf` 延迟暂存 | 原叶入 pend + 新右半留树 |
| fill | 自分配叶 | 纯填充，分配由外层 |
| flush | 每轮每层 makespace → O(N^2) | 每层至多一次 → O(N) |
| paths | old_off + lc_seek（restore） | 即时同步，始终自然正确 |
| OOM | ❌ | ✓（预分配 + 逆操作 + splice） |
| lc_scan | 独立实现 | 纯追加特例 |

## 8. 注意事项

1. **at[l] 可等于 child_count**：非数组越界，makespace(child_count) = 追加到末尾。
2. **内层裂分度量传播**：替换 parent(l)->children 后，需计算新旧度量差并通过 lcM_up 向上传播。
3. **打包不用 lcM_up**：pend 不在树中度量链内，打包仅更新 pend 自身度量。
4. **lcB_Ctx 大小**（64 位）：移除 `rt_leaf(8)+rt_bytes(8)+rt_breaks(4)+at_end(4)=24B`，新增 `at[16]=32B`，净增 **+8 字节**。
5. **fill 的 bytes 基值**：`x->c.tree->bytes`，裂叶后的减量已通过 lcM_up 反映。
6. **lcB_skipinsert 消除**：scanner 返回 0 且无残行时，fill 自然返回 0——无需树修改。
7. **步骤 2 裂分约束**：若 at[l+1] 导致一侧 < FANOUT/2，跳过此层裂分，整 n 留树，pend 以 makeplace 挤入。如此步骤 6（重平衡）退化为空操作。

## 9. 改造范围

| 模块 | 操作 | 说明 |
|------|------|------|
| `lcB_Ctx` | 移除 4 字段 + 加 `at[16]` | 净 +8B |
| `lcB_init`/`lcB_initat` | 合并 | at[l] 赋值 |
| `lcB_fill` | 改签名 | +col_inout，不分配 |
| `lcB_flush` → `lcB_flush_one` | 重写 | ~55 行 |
| `lcB_fillflush` | 保留+适配 | 外部循环调用 flush_one |
| `lcB_merge` | 保留 | 适配 at[] |
| `lcB_rootpush` | 修改 | 适配 at[] |
| `lcB_splitleafat` → `lcB_splitleaf_in_pend` | 重写 | 左半入 pend + 预分配 |
| `lcB_unsplitleafat` | 新增 | ~25 行逆操作 |
| `lcB_applyfirst`/`lcB_pushrt`/`lcB_packleafs`/`lcB_skipinsert` | 删除 | 逻辑由 fill/flush 吸收 |
| `lc_insert` | 重写 | ~50 行 |
| `lc_scan` | 重写为 wrapper | ~10 行 |

净增 ~50 行。
