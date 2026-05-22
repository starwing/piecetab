# 计量 B+ 树区间删除——第六代终极算法

> 本文为正式实施蓝本。承 `range_delete_evolution.md` 历代演进之总结，针对其中第二代未尽之憾，找出关窍重新铺陈。
> 与 `splice_repair_plan.md`（第三代三段法）相比，将"shift 二态化"，从而消除 top-down 对 R 路径的依赖。

---

## 一、第二代未尽之原委

第二代（双游标拉链法）当时被搁置，理由为"合并必致 R 失效"。然回首细察，此非死结，因第五代亦在合并时令 R 失效却仍能续行。第二代真正之症结，乃是：

> **`shiftleaf`/`shiftnode` 仅有"合并"一态。若 `cl + cr > FANOUT`（不能合并），则 L、R 两节点各自残缺，bottom-up 阶段束手无策。后续亦无可恃手段补救。**

若将 `shift` 操作赋予"合并或均分"二态：
- `cl + cr <= FANOUT`：合并，R 节点归零，待父层 `trimnode` 一并回收。
- `cl + cr > FANOUT`：均分（redistribute），二者皆变合法（`>= FANOUT/2 + 1`）。

则 bottom-up 阶段结束之后，R 路径上每一节点 **要么不存在（已合并归零），要么合法（已均分）**。从而 `R->paths` 失去使用价值——而这恰好不是问题，因为 R 已无需修复。

此乃第六代算法立论之根基。

---

## 二、算法总览

```
function splicerange(L, R):
    1. 求分歧层 l
    2. Bottom-Up Cleanup（自叶向上至 l+1 层）
       - trimleaf(L, right), trimleaf(R, left), shiftleaf(L, R)
       - for dl in [levels-1 .. l+1]:
            trimnode(L, dl, right)
            trimnode(R, dl, left)
            shiftnode(L, R, dl-1)
    3. prune(L, R, l)：删除 l 层 L 与 R 之间的中间节点
    4. Top-Down Repair（仅用 L 路径，丢弃 R）
       - 若 l==0 且 root 仅一子且 levels>0，缩根（实际由 rebalance 收尾）
       - 若 L_l underfull，foldnode(L, l) 修复
         （若合并发生，rebalance(L, l-1) 处理上传）
       - for dl in [l+1 .. levels-1]:
            foldnode(L, dl)
       - foldleaf(L)
```

---

## 三、术语与不变式

### 路径不变式

- `paths[l]` 类型为 `lc_Node**`（除 `paths[levels]` 外），值为 `&parent->children[i]`。
- 不变式：`paths[l+1] == &(*paths[l])->children[i]`。
- `paths[0]` 指向 `c->root.children` 中某槽。`-1` 层即 root（无可寻址父）。
- `paths[levels]` 指向叶子父之 children 数组的某槽（解引用得 `lc_Leaf*`）。

### 各原语作用层

| 原语 | 参数 `l` 含义 | 操作对象 |
|---|---|---|
| `trimleaf(C, left)` | 隐含 `levels` | 叶子内部段裁剪 |
| `trimnode(C, l, left)` | 节点自身层 | 节点 `paths[l]` 同父之兄弟裁剪 |
| `shiftleaf(L, R)` | 隐含 `levels` | 叶子之间合并/均分 |
| `shiftnode(L, R, l)` | **父层** | `paths[l+1]` 即 dl 层节点合并/均分 |
| `foldleaf(C)` | 隐含 `levels` | 叶子向同父兄弟合并/均分 |
| `foldnode(C, l)` | **节点自身层** | `paths[l]` 向同父兄弟合并/均分 |
| `rebalance(C, l)` | 节点自身层 | 自 l 层向上修补 |

注：`shiftnode` 之 `l` 为父层（与现行代码一致），与 `foldnode` 之 `l` 为节点自身层不同，命名不对称，须留意。

### 不变量

**I1（L 路径稳定）**：bottom-up 阶段中，L 路径自首至尾不变。所有 shift 操作皆将 R 数据搬至 L 尾部，L 节点之地址、其在父中之槽位均不变。

**I2（R 节点最终性）**：bottom-up 阶段结束后，对任意 dl ∈ [l+1, levels]，R 在 dl 层要么"已合并"（其原槽空，将被父层 trimnode 回收），要么"合法"（child_count >= FANOUT/2 + 1）。

**I3（修补归纳）**：top-down 阶段中，若 `L_dl->child_count > FANOUT/2`（严格大于），则可调用 `foldnode(L, dl)` 修补 `L_{dl+1}`；修补后 `L_{dl+1}->child_count >= FANOUT/2`。若 fold 实为合并，`L_dl->child_count` 减 1 仍 `>= FANOUT/2`。归纳成立。

---

## 四、Bottom-Up 阶段详解

### 4.1 trim 之"惰性"语义

`trimnode` **不执行 `memmove`**，仅释放被删除子节点并调整父节点 `child_count`。物理上：
- `trimnode(C, l, right)`：释放 `[i+1, cc)`，`cc := i+1`。
- `trimnode(C, l, left)`：释放 `[0, i)`，`cc := cc - i`。注意此处 `children[0..i)` 现已为悬垂指针；剩余有效内容仍位于 `[i, old_cc)`，逻辑上以 "从 `paths[l]` 处开始算 `cc` 个有效项" 看待。

此惰性设计依赖 `R->paths[l]` 始终为绝对槽位地址，后续 `shiftnode` 通过 `paths[l]` 直接定位有效区。

### 4.2 shiftleaf/shiftnode 二态语义

#### shiftleaf(L, R)
- 设 `cl = pl->breaks[il]`, `cr = pr->breaks[ir]`，叶子段数。
- **若 `cl + cr <= LEAF_FANOUT`**（合并）：
  - 将 R 叶 `[R->lidx, cr)` 范围拷至 L 叶 `[cl, ...)` 末。
  - `lcM_tx` 将该数据度量自 R 链转 L 链。
  - **关键**：`R->paths[levels-1] += 1`——令 R 父之逻辑视图向后移一槽。下一轮 `trimnode(R, levels-1, left)` 将自然回收原 R 叶（因为它现在被划在"左侧待删"之内）。
  - 返回 1（已合并）。
- **若 `cl + cr > LEAF_FANOUT`**（均分）：
  - 总数 `total = cl + cr`，新左 `new_cl = (total + 1) / 2`。
  - 将数据搬移使 L 叶有 `new_cl` 段，R 叶（自其 `R->lidx` 起）有 `total - new_cl` 段。
  - 度量按差量 `lcM_tx` 转移。
  - 返回 0（未合并，R 仍存在且合法）。

> 注意：均分后 R 叶变合法，且其物理位置不变。R->paths 在该层依然有效，但后续不会用——top-down 不依赖 R。

#### shiftnode(L, R, l)
（参数 l 为父层；操作 dl=l+1 层节点）
- 设 `nl = lcK_parent(L, l+1)`，`nr = lcK_parent(R, l+1)`，皆为 dl 层节点。
- `cl = nl->child_count`，`cr = nr->child_count`。
- **若 `cl + cr <= FANOUT`**（合并）：
  - `lcN_copy(nl, il+1, nr, ir, cr)` 将 nr 之 `[ir, ir+cr)` 拷至 nl 末。
  - 度量经 `lcM_tx` 转移（自 nr 父之 ir 处转 nl 父之 il 处）。
  - `nl->child_count += cr`，`nr->child_count -= cr`。
  - **关键**：`R->paths[l] += 1`——令 R 之 l 层逻辑视图后移一槽，下一轮 `trimnode(R, l, left)` 回收原 R 节点。
  - 返回 1。
- **若 `cl + cr > FANOUT`**（均分）：
  - `mid = (cl + cr + 1) / 2`，`d = mid - cl`。
  - 自 nr 之 ir 处搬 d 个 child 至 nl 末，nr 内压实剩余。
  - 度量差量转移。
  - 返回 0。

> 由于 shiftnode 改变 dl 层节点结构，其 `paths[l+1]` 失效。但下一轮迭代会用 `R->paths[dl-1]` 即 `paths[l]`，与本层无关。L 之路径未变（数据流入 L 尾部，L 之 il 槽位仍然指向同一节点）。

### 4.3 trim 与 shift 的协作

shiftleaf 合并后令 `R->paths[levels-1] += 1`。下一轮 `trimnode(R, levels-1, left)`：
- `i = R->paths[levels-1] - p->children` 已 +1。
- `cc = cc - i`：实际上把 `[0, i)` 即包含原 R 叶子在内的左侧诸项归为"待删"。
- 释放 `[0, i)` 包括原 R 空叶。
- R 父之逻辑视图自原槽 +1 起。

这种"惰性+槽位漂移"策略，使空节点回收无需独立 step。

### 4.4 边界处理

#### 当 R 为父之最后一子
shiftleaf 合并时 `R->paths[levels-1] += 1` 可能越界（指向 `&parent->children[cc]`，越过末尾）。此时下一轮 `trimnode(R, levels-1, left)` 中 `i = cc`：
- `cc := cc - cc = 0`。
- 释放 `[0, cc)` 即整个父节点之子全释放。
- R 父变空——此父节点本身也将作为"垃圾"在再上一层被处理。

但这会引发 R 路径继续向上失效。需保证：R 父空时，R 之祖父在 dl-1 层 trimnode 时也将其释放。事实上，dl 层 R 路径 += 1 之后再 dl-1 层操作时，`R->paths[dl-2]` 决定 dl-1 层位置，而 dl-1 层节点本身仍存在，`paths[dl-1] += 1` 之后整体后移即可。

需仔细处理的边界：当 R 在父中是最后一子，且 shift 合并成功，则原 R 节点空之后还需让其父也"前移一槽"或被一并回收。具体处置详见实施时之单元测试。

#### 当 L 与 R 同父（l = dl - 1，即下一轮 dl=l+1 时）
此时迭代终止条件 `dl > l` 已退出循环，shiftnode(L, R, l) 即针对共父的两个子。但此时 L 与 R 之间还有中间节点未删（trim 不删中间），shiftnode 会越过中间节点直接拷贝 R 数据至 L 末——这是错误的！

**解法**：循环范围应当为 `dl ∈ [levels, l+1]`，即终止于 `dl = l+1`，shiftnode(L, R, l) 操作 l+1 层节点。这一层 L 与 R 仍不共父（共父在 l 层但 L 与 R 在 l 层还是不同节点），此时 shift 是合理的。

而 dl=l 那一步——即 L 与 R 在 l 层共父——**不属于 bottom-up 范围**，由 prune 处理。

故循环为：

```c
for (dl = levels - 1; dl >= l + 1; --dl) {
    trimnode(L, dl, 0);
    trimnode(R, dl, 1);
    shiftnode(L, R, dl - 1);
}
```

而 leaf 层（dl = levels）由 trimleaf + shiftleaf 处理（在循环外）。

---

## 五、Prune 阶段

`prune(L, R, l)`：在 l 层共父中，删除 `[il+1, ir)` 范围之中间节点。这些节点是干净的（trim 未触及），整子树释放即可。释放后，父节点压实剩余子。度量按差量上传。

prune 之后：
- L_l 在 il 位置（不变）。
- R_l 原本在 ir 位置，prune 后位于 il+1（紧邻 L_l）。
- 共父之 child_count 减少 `ir - il - 1`。

**特殊情形**：
- 若 shiftleaf/shiftnode 在某层成功合并，对应 R 节点已空，被 trimnode 回收。但 ir 计算时仍可使用原始位置。需在调用 prune 时统一传入 R 在 l 层的逻辑位置（即 `R->paths[l]` 之 idx）。
- 若 R_l 整体已被合并归零（即在 dl=l+1 层 shiftnode 时合并），其在 l 层之槽位为空，但 ir 不变；prune 范围 `[il+1, ir)` 仍正确。但此时还要回收这个空 R_l——可以让 prune 范围扩展为 `[il+1, ir+1)`，或者通过 `R->paths[l-1] += 1` 触发上层 trim。
- 简化处理：在 dl=l+1 层 shiftnode 合并成功时，让 `R->paths[l] += 1`，则 prune 时取 `ir = R->paths[l] 之 idx`（已 +1），范围自然涵盖原 R_l 空槽。

---

## 六、Top-Down Repair 阶段

bottom-up 之后丢弃 R。按 L 路径自上而下修补。

### 6.1 缩根

若 `l == 0` 且 prune 后 root.cc == 1 且 levels > 0：root 唯一子升为新 root，levels 减一。L 路径同步收缩。

实际上由 `rebalance` 末段处理。

### 6.2 修复 L_l

l 层共父之子数已被 prune 削减，L_l 自身可能 underfull（child_count < FANOUT/2）。

```c
if (L_l->child_count < FANOUT / 2) {
    if (foldnode(L, l) merged) {
        rebalance(L, l - 1);   /* 父减一子，可能向上传递 */
    }
    /* foldnode 若均分则直接合法，无须传递 */
}
```

注意：`l == 0` 时 L_l 是 root 之子，其"兄弟"是 root 的其他子。若 root 已只剩 L_l 一子，foldnode 无兄弟可寻——此时为"l==0 且 root.cc==1"特殊情形，由缩根处理。

### 6.3 沿 L 向下递降修补

```c
for (dl = l + 1; dl < levels; ++dl) {
    foldnode(L, dl);   /* 修复 L_dl underfull */
}
foldleaf(L);           /* 修复叶子 */
```

每层 fold 之归纳保证（不变量 I3）：
- `L_dl >= FANOUT/2`（步骤 6.2 保证 dl=l 时严格大于半满）。
- foldnode(L, dl) 操作 L_dl 同父之 L_dl 与兄弟（兄弟存在因为 L_dl 父严格大于半满）。
- 合并：L_dl 父减 1 子，但仍 `>= FANOUT/2`。
- 均分：L_dl 父不变，L_{dl+1} （即被操作节点）变 `>= FANOUT/2 + 1`。

注意 `foldnode(L, dl)` 修复的是 `L_dl` 节点自身的 child_count（即 dl+1 层数量）；它并不修复 `L_dl` 自身在 dl-1 层之 child_count 不足。

实际上，bottom-up 阶段 `trim+shift` 已让 dl 层 child_count 在 R 那边趋于合法，但 L 那边的 dl 层节点 child_count 因 trimnode(L, dl, right) 削减而可能 underfull。foldnode(L, dl-1) 操作的是 L 在 dl-1 层节点（看 dl 层兄弟数）……此处命名混乱再次需要谨慎。

**澄清**：`foldnode(C, l)`——参数 l 为节点自身层。它检查 `paths[l]` 节点（即 l 层之 nl），尝试把 nl 与同父兄弟合并/均分。换言之，它修复的是"nl 之父（l-1 层）失去一子带来的影响"——不，它修复的是 nl 自身 child_count 不足。

让我们以现有代码 `lcD_foldnode(C, l)` 为准（节点自身层）：
- `p = lcK_parent(C, l) = *paths[l-1]`，即 l-1 层节点（nl 之父）。
- `i = paths[l] - p->children`，即 nl 在父中的索引。
- nl = `p->children[i]`。
- 检查 nl->child_count 不足时，与 `p->children[i+1]` 或 `p->children[i-1]` 合并/均分。

所以 `foldnode(L, dl)` 修复 L_dl 节点的 child_count（即 L_dl 含有的 dl+1 层子数量）。

### 6.4 完整 top-down 流程

```c
/* 步骤 6.2：修复 L_l */
if (l > 0) {
    /* L_l 同父有兄弟（共父非 root，或 root 经 prune 仍 cc>1） */
    if (L_l underfull) {
        if (foldnode(L, l) merged)
            rebalance(L, l - 1);
    }
} else {
    /* l == 0：L_l 即 root 之子 */
    if (root.cc > 1 && L_l underfull)
        foldnode(L, 0);   /* 与 R 之残骸合并/均分 */
    /* root.cc == 1 时无须此处 fold，由后续 rebalance 缩根 */
}

/* 步骤 6.3：递降修补 dl ∈ (l, levels) */
for (dl = l + 1; dl < levels; ++dl) {
    if ((*paths[dl])->child_count < FANOUT/2)
        foldnode(L, dl);
}

/* 步骤 6.4：修补叶子 */
if (lcK_leaf(L)->breaks 不足)
    foldleaf(L);

/* 步骤 6.5：缩根（rebalance 末段） */
while (levels > 0 && root.cc == 1)
    缩根;
```

---

## 七、与现有代码的差异表

| 函数 | 现状 | 第六代要求 |
|---|---|---|
| `lcD_shiftleaf` | 仅合并；`cl+cr>FANOUT` 返回 0 | **二态**：合并或均分 |
| `lcD_shiftnode` | 仅合并；`cl+cr>FANOUT` 返回 0 | **二态**：合并或均分 |
| `lcD_foldleaf` | 仅合并；`cl+cr>FANOUT` 返回 0 | **二态**：合并或均分 |
| `lcD_foldnode` | 仅合并；`cl+cr>FANOUT` 返回 0 | **二态**：合并或均分 |
| `lcD_mergeleaf` | 均分（含 fold 回退） | 可保留作为 spliceleaf 用 |
| `lcD_mergenode` | 均分（含 fold 回退） | 可保留 |
| `lcD_rebalance` | 现有 | 可保留（用于 fold 合并后向上传） |
| `lcD_splicerange` | 三段法（依赖 R->paths phase 2） | **重写**为第六代流程 |

### shiftleaf / shiftnode 改造方法

二态融合最自然的做法：让 `shiftleaf` 实质上等同 `mergeleaf`（合并或均分），但
- merge 后须 `R->paths[levels-1] += 1`（旧 mergeleaf 不做此）。
- 不需要 `sr` 参数处理（因 R 已 trim，其 lidx 已对齐到 0）。

**实际：** `mergeleaf` 现有签名 `lcD_mergeleaf(C, sr)` 有 sr 参数，是为处理 R 未 trim 时之偏移。在第六代流程中，bottom-up shift 在 R 已 trim 之后调用，sr 始终为 0；可直接复用 `mergeleaf` 但需在合并分支补上 `R->paths[levels-1] += 1` 之逻辑。

类似地 shiftnode。

### foldleaf / foldnode 改造方法

现有 foldleaf/foldnode 仅合并，不均分。第六代要求均分作为兜底。

最简实现：在现有 foldleaf/foldnode 函数内，`cl+cr > FANOUT` 分支调用 mergeleaf/mergenode 之均分逻辑。或者直接将"均分"逻辑内联进 foldleaf/foldnode。

但 mergeleaf/mergenode 需要 sr 参数；对 fold 而言 sr 应始终为 0（fold 是修复 L underfull，不涉及 R）。

### 当前 splicerange 之多余复杂度

现有 `lcD_splicerange` 之 phase 2 循环 `for (dl = l+1; dl < levels; ++dl) lcD_mergenode(L, ridx, dl);` 和末尾 `lcD_mergeleaf(L, R->lidx)` 仍依赖 R 路径之 ridx 与 lidx。第六代去除这一依赖，改为 `foldnode(L, dl)` 沿 L 路径递降。

---

## 八、正确性证明

### 8.1 引理 L1：bottom-up 后 R 之总状态

**命题**：对任意 dl ∈ [l+1, levels]，bottom-up 阶段结束后，要么 R_dl 不存在（合并归零并被 trim 回收），要么 R_dl->child_count >= FANOUT/2 + 1。

**证明**：依赖 shift 二态。
- 若 dl=levels：shiftleaf 之 cl + cr ≤ LEAF_FANOUT 则合并，R 叶归零被 levels-1 层 trim 回收；否则均分，新 cr = (total) / 2 ≥ (LEAF_FANOUT + 1)/2 > LEAF_FANOUT/2。
- 若 dl ∈ [l+1, levels-1]：shiftnode 同理。

### 8.2 引理 L2：prune 后 L_l 之兄弟皆合法

**命题**：prune(L, R, l) 之后，L_l 的右兄弟（即原 R_l 经 shift 处理后留下的节点，若存在）child_count ≥ FANOUT/2 + 1（合法）。

**证明**：bottom-up 之 shiftnode 在 dl=l+1 层（即父层 l）操作 L_{l+1} 与 R_{l+1}。若该层合并成功，R_{l+1} 归零，R_l 父中其槽位空，需在 prune 时一并回收（通过 `R->paths[l] += 1` 之机制）。若均分，R_{l+1} 合法。

R_l 自身（l 层节点）child_count 取决于其下层 dl=l+1 处理后剩余子数：
- 若 shiftnode(l-1) 合并：R_l 之 children 全数转入 L_l，R_l 归零（→ 不存在）。
- 若 shiftnode(l-1) 均分：R_l 之 child_count ≥ FANOUT/2 + 1。

故 prune 后 L_l 之右兄弟（若存在）合法。L_l 之左兄弟在区间外，未被本次操作触及，自然合法。

### 8.3 引理 L3：top-down 修补不向上级联

**命题**：top-down 阶段 `foldnode(L, dl)` 不引发跨层级联合并（即 dl-1 层不会因此 underfull）。

**证明**：归纳 dl。
- 基础情形 dl=l：步骤 6.2 修复 L_l，可能合并致 l-1 层减一子。但 l-1 层之共父在 prune 阶段未被影响（l-1 层是 L、R 共父之父，区间删除从未触及）。若 l-1 层减一子后仍合法，止于此层；若 underfull，由 rebalance(L, l-1) 处理（标准向上修补）。
- 归纳步：假设 L_dl->child_count > FANOUT/2（严格）。foldnode(L, dl+1) 操作 L_{dl+1}：
  - 合并：L_dl child_count -= 1，仍 ≥ FANOUT/2（因为严格大于）。
  - 均分：L_dl child_count 不变，L_{dl+1} child_count ≥ FANOUT/2 + 1（严格）。
- 故 L_{dl+1} 修复后仍满足"父严格大于半满"或"自身严格大于半满"，归纳成立。

### 8.4 度量一致性

每 trim/shift/prune 操作均维护 `bytes`、`breaks` 之差量上传（`lcM_up` 或 `lcM_tx`）。foldleaf/foldnode 同理。整个流程末，`c->bytes` 和 `c->breaks` 减少 `del` 和对应的 break 数。

---

## 九、实施时之关键细节

### 9.1 shiftleaf 合并分支之 R 路径漂移

```c
/* 合并 */
memcpy(&ll->bytes[cl], &lr->bytes[R->lidx], cr * sizeof(unsigned));
lcM_tx(L, R, levels, pr->bytes[ir], cr);
R->paths[levels - 1] += 1;  /* 关键：让 R 之父视图后移一槽 */
return 1;
```

注意：若 `levels == 0`（L、R 共同在 root 之 children 中，分歧层 l=0），则 `levels-1 = -1` 越界，需特殊处理。但 `splicerange` 之分歧层 l < levels 才进入；`levels-1 < l` 不可能（因为 dl 循环为 levels-1 downto l+1，dl >= l+1 即 levels-1 >= l+1 即 levels >= l+2）。故 levels >= 1 + l + 1 ≥ 1，levels-1 ≥ 0 安全。

但有个例外：l=0 且 levels=0 即整树仅一叶时——此情形不进 splicerange（同叶走 spliceleaf）。所以安全。

实则 `levels-1` 处之 `R->paths[levels-1]` 在 shiftleaf 合并时 +1，保证下一轮 dl=levels-1 之 trimnode(R, levels-1, left) 用更新后之值。

### 9.2 shiftnode 合并分支之 R 路径漂移

```c
/* l 是父层 */
lcN_copy(nl, il+1, nr, ir, cr);
lcM_tx(L, R, l, p->bytes[i], p->breaks[i]);
nl->child_count += cr;
nr->child_count -= cr;
R->paths[l - 1] += 1;  /* R 之祖父视图后移一槽 */
return 1;
```

当 `l == 0` 时（L、R 共父为 root），`l-1 = -1` 越界。此时 dl = l + 1 = 1，是 dl 循环的最后一步（因为循环至 dl=l+1 终止）。这个 shiftnode 合并的是 L_1 与 R_1 在 root 之下的两节点。R_1 归零之后，需要在 prune 阶段回收——但循环已结束。**解法**：在 dl = l+1 层 shiftnode 合并时不漂移 R->paths[l-1]，而是漂移 R->paths[l]，使 prune 时 R 在 l 层之 idx 已 +1，prune 范围扩展。

具体：
```c
if (l - 1 < 0) {
    /* shiftnode 操作的是共父之子，下一步是 prune */
    R->paths[l] += 1;
} else {
    R->paths[l - 1] += 1;
}
```

或者统一为：shiftnode 合并后总是 `R->paths[l] += 1`（让 R 在父层之 idx 后移），下一轮 trimnode(R, l, left) 之 i 自然包含原 R 槽。

实际上分析：shiftnode(L, R, l) 操作 dl=l+1 层节点。合并后 R_{l+1} 归零。R_{l+1} 之父在 l 层（即 nr，paths[l]）。需要让 nr 中那个空槽也被回收。

下一轮迭代 dl = l（若 l+1 > l+1 = false，循环退出）——等等，循环条件是 `dl >= l+1`，所以 dl=l+1 是循环最后一次。下一步是 prune(L, R, l)。prune 用 ir = lcK_idx(R, p, l)，取自 `R->paths[l]`。

关键：合并后 R 在 l+1 层之槽空了，但其在 l 层之槽（即 R->paths[l]）尚未漂移。如果 prune 用 R->paths[l] 当前之 idx（未漂移）作为 ir，则范围 `[il+1, ir)` 不包括 R_l 自身。R_l 自身被认为是"已合法之 R 残部"——但事实上其 child_count 已减了 cr，可能空（若全部合并）！

**精确分析**：shiftnode(L, R, l) 合并时 nr->child_count -= cr。若 nr->child_count 因此为 0（即 R 在 l 层只有一个子，被合并），nr 应被视为"空 R 节点"，需在 prune 中回收。

简化策略：**shiftnode 合并时不修改 R->paths**（因为 nr 本身可能未空，仅 child_count 减少），而是让外部检测——若 nr->child_count == 0，prune 范围扩展至 `[il+1, ir+1)`。

或者更稳妥：每次 shiftnode 合并后，检查 nr->child_count 是否归零，若是，则 `R->paths[l] += 1`（让 R 在父层视图后移）。这样 prune 自然涵盖 R_l 空壳。

再者，shiftnode 合并通常 cr 是 nr->child_count（全转移），故 nr->child_count -= cr 后为 0。所以这个判断几乎总是成立。

**最终方案**：shiftnode 合并恒令 `R->paths[l] += 1`（R 在父层视图后移），dl 层之操作结束。但仍要让 dl-1 层 trimnode 知晓 R 之 paths[l] 已 +1。事实上下一轮 dl=l+1 是循环最后一次（已分析），不再有 trimnode；直接进 prune，prune 用更新后的 ir。

但更前面的迭代如 dl=l+2, l+3 时 shiftnode(l+1) 合并，R->paths[l+1] += 1，下一轮 dl=l+1 之 trimnode(R, l+1, left) 之 i 已 +1，自然回收原 R 空节点。

故统一规则：**shiftnode(L, R, l) 合并时 `R->paths[l] += 1`**。`l` 是父层。覆盖所有迭代。

注意 shiftleaf 类似：合并时 `R->paths[levels-1] += 1`（叶子之父层）。

### 9.3 prune 中 ir 之取值

```c
ir = lcK_idx(R, p, l);  /* p = lcK_parent(R, l) = *R->paths[l-1] = 共父 */
```

R->paths[l] 经过 bottom-up 阶段中各 shiftnode 之漂移（最末一次是 dl = l+1 之 shiftnode(L, R, l)，若合并则 R->paths[l] += 1）。

故 ir 已自动包含必要漂移。prune 范围 `[il+1, ir)` 自然涵盖原 R_l（若已空）。

### 9.4 foldleaf / foldnode 之均分实现

现有：
```c
static int lcD_foldleaf(C, l) {
    /* 仅当 cl+cr <= LEAF_FANOUT 才操作 */
    if (cl + cr > LC_LEAF_FANOUT) return 0;
    /* 合并并 prune */
}
```

第六代要求"合并或均分"二态。可改为：
```c
static int lcD_foldleaf(C, l) {
    if (cl + cr <= LC_LEAF_FANOUT) {
        /* 原有合并 */
        return MERGED;
    } else {
        /* 均分（沿用 mergeleaf 之 balanceleaf 逻辑） */
        balanceleaf(...);
        return REDISTRIBUTED;
    }
}
```

返回值：约定 `MERGED=1`（调用方需 rebalance 父），`REDISTRIBUTED=2`（无须 rebalance），`NOOP=0`。或简化为合并返回 1（同现行），均分返回 0。但需要外层 rebalance 之触发逻辑。

更好的方式：让 foldnode/foldleaf 始终返回 1 表示"已修复"，由调用方根据情况判断是否 rebalance。但 rebalance 之触发需要知道是合并还是均分（因为均分不会让父减一子）。

折衷方案：
```c
/* 返回值：0 未修复，1 已合并（父减一子），2 已均分（父不变） */
static int lcD_foldnode(C, l) { ... }
```

或者让 foldnode 内部完成所有事（包括 prune 和必要的上传 rebalance），返回值仅表"成功"。这与现有 rebalance 风格一致。

具体到 splicerange 中：
- 步骤 6.2 修复 L_l：foldnode 合并则需 rebalance(l-1)，均分则止。
- 步骤 6.3 递降：L_dl 修复时合并致 L_{dl-1} 减一子，但归纳保证 L_{dl-1} 仍合法（严格 > FANOUT/2 → ≥ FANOUT/2）。无须额外 rebalance。

故步骤 6.3 之 foldnode 调用，可以忽略其返回值（只要它正确处理 prune 即可，父减一子已被归纳吸收）。

步骤 6.2 需要根据返回值决定是否 rebalance。

### 9.5 trimleaf 之 L->col 处理

现有 splicerange 在 trimleaf 之后有：
```c
if (L->col) {
    lc_Node *pr = lcK_parent(R, lcK_levels(R));
    int      ir = lcK_idx(R, pr, lcK_levels(R));
    if (pr->breaks[ir]) {
        lc_Leaf *lr = (lc_Leaf *)pr->children[ir];
        lr->bytes[(int)R->lidx] += L->col;
        lcM_up(R, lcK_levels(R), (lc_Diff)L->col, 0);
    }
}
```

L->col 表示 L 在当前段之偏移。trimleaf(L, right) 删除 lidx 之后段；但 L 当前段（lidx）剩余的 (len - col) 部分也要删除——trimleaf 已处理。但 L 当前段保留的是 [0, col) 字节。如果 R 有数据要保留，则 R 当前段（[lidx, ...)）跨过 col 字节后才是真正保留区。这部分由现有 trimleaf 处理。

但 L 段之 col 部分需要"移植"到 R 段，因为合并/均分时 L 段末尾要接续 R 段开头。具体地，shiftleaf 合并时把 R 段拷至 L 段末，但要确保两段衔接处字节对齐。

此处细节繁琐，沿用现有逻辑即可。

### 9.6 整体 splicerange 骨架

```c
static void lcD_splicerange(lc_Cursor *L, lc_Cursor *R) {
    int l, dl, levels = lcK_levels(L);
    
    /* 1. 求分歧层 */
    for (l = 0; l <= levels; ++l)
        if (L->paths[l] != R->paths[l]) break;
    assert(l <= levels);
    
    /* 2. Bottom-Up Cleanup */
    lcD_trimleaf(L, 0);
    lcD_trimleaf(R, 1);
    /* L->col 之处理（如现行代码） */
    if (L->col) { ... }
    lcD_shiftleaf(L, R);   /* 合并或均分；合并时 R->paths[levels-1] += 1 */
    
    for (dl = levels - 1; dl >= l + 1; --dl) {
        lcD_trimnode(L, dl, 0);
        lcD_trimnode(R, dl, 1);
        lcD_shiftnode(L, R, dl - 1);  /* 合并或均分；合并时 R->paths[dl-1] += 1 */
    }
    
    /* 3. Prune l 层 */
    {
        lc_Node *p = lcK_parent(L, l);
        int      ir = lcK_idx(R, p, l);   /* 已含漂移 */
        lcD_prune(L, ir, l);
    }
    
    /* 4. Top-Down Repair（仅用 L 路径） */
    
    /* 4a. 修复 L_l underfull */
    {
        lc_Node *p = lcK_parent(L, l);
        int      i = lcK_idx(L, p, l);
        lc_Node *Ll = p->children[i];
        if (l > 0 && Ll->child_count < LC_FANOUT / 2) {
            int merged = lcD_foldnode(L, l);  /* 二态：1 合并、2 均分 */
            if (merged == 1)
                lcD_rebalance(L, l - 1);
        } else if (l == 0 && p->child_count > 1 && Ll->child_count < LC_FANOUT / 2) {
            lcD_foldnode(L, 0);
        }
    }
    
    /* 4b. 沿 L 递降修补 */
    for (dl = l + 1; dl < levels; ++dl) {
        lc_Node *p = lcK_parent(L, dl);
        int      i = lcK_idx(L, p, dl);
        if (p->children[i]->child_count < LC_FANOUT / 2)
            lcD_foldnode(L, dl);  /* 修复，归纳保证父合法 */
    }
    
    /* 4c. 修补叶子 */
    {
        lc_Node *p = lcK_parent(L, levels);
        int      i = lcK_idx(L, p, levels);
        if (p->breaks[i] < LC_LEAF_FANOUT / 2)
            lcD_foldleaf(L, levels);
    }
    
    /* 4d. 缩根（如需） */
    while (lcK_levels(L) > 0 && L->tree->root.child_count == 1) {
        /* 缩根逻辑（同现行 rebalance 末段） */
    }
}
```

### 9.7 缩根之复杂性

缩根改 `paths` 数组（`memmove(C->paths + 1, C->paths + 2, ...)`）。在 splicerange 中可能多次缩根（深空树），需循环。现行 `lcD_rebalance` 已包含此逻辑，可调用 `rebalance(L, l)` 收尾。

实际上步骤 4a 之"l==0 且 root.cc==1"情形，可以让 rebalance(L, 0) 处理（rebalance 末段缩根）。但 rebalance 主体逻辑会先 fold 后缩根，对 l>0 情形也合适。

**统一处理**：步骤 4a 改为
```c
if (l > 0)
    rebalance(L, l);   /* 修复 L_l 自身，必要时向上传 */
else
    rebalance(L, 0);   /* l=0 直接 rebalance 自顶处理缩根 */
```

但 rebalance 当前只 fold-合并不均分。需先确保 foldnode 二态化后，rebalance 之循环兼容。

---

## 十、测试用例预想

### T1：splice_removed2（当前失败）
levels=3 树，L 在 root.children[0]，R 在 root.children[2]，l=0。删除中间。预期结果：合法树，无空叶。

### T2：深谷悬切
从极左删到极右，仅留首尾。验证 bottom-up 各层合并到底，top-down 缩根至最少层数。

### T3：均分一次
设置 cl + cr 恰超 FANOUT，验证 shiftnode 进入均分分支。

### T4：合并一次
设置 cl + cr ≤ FANOUT，验证 shiftnode 合并并 R->paths 漂移。

### T5：foldnode 均分
设置 L_dl underfull，兄弟丰满，cl+cr > FANOUT，验证 foldnode 进入均分分支。

### T6：连续合并
设置 cl, cr 皆小，多层连续合并，验证缩根多次。

### T7：trimnode 边界
R 在父之最后一子，shift 合并后 paths 漂移越界，验证 trim 在下一轮正确处理。

---

## 十一、与第三代之等价性

第三代 `lcD_splicerange` 在 phase 2 调用 `mergenode` / `mergeleaf`，其内部已有"合并或均分"逻辑（不过是均分时用 sr 参数处理 R 偏移）。第六代实质上是把这一逻辑提前到 bottom-up 阶段（shift），并废弃 phase 2 中对 R 路径的依赖，改用 fold 沿 L 递降。

故第六代不是新增能力，而是**结构重整**：将"合并或均分"之统一原语前置至 bottom-up，简化 top-down 至单纯 fold 序列。

代码量预计减少 30-40%（去除 phase 2 之 R 路径维护、mergenode 之 sr 参数处理）。
