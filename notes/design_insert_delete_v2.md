# lc_append 与 lc_splice 算法设计 v2

## 1. 概述

本文档描述 linecache 的中部插入 (`lc_append`) 与区间删除 (`lc_splice`) 算法。二者共用一套 B+ 树操作原语（裂叶、缝合、折叠、均分、缩根），但因语义不同（插入是"裂开→追加→缝合"，删除是"修剪→挖掘→缝合→修复"），流程各异。

**lc_append** 在游标位置裂开树，将右侧数据搬入临时数组 `rt[]`，以 append 消化 scanner 输出并通过 findroom 扩容，最后将 rt 缝合回树并补写裂点残字节。

**lc_splice** 以双游标 L/R 界定删除区间，修剪边缘后将 R 右侧子树移入 `rt[]`、删除 L-R 中段，通过 stitchnode 将 rt 逐层缝合回树并沿路径修复 underfill。rt 惰性使用——仅在 R 实际有右侧数据的层存入。

## 2. 术语表

| 术语      | 含义                                                                                         |
| --------- | -------------------------------------------------------------------------------------------- |
| `rt[]`    | 临时节点数组，mediates findroom 与 stitch 之间的数据搬运。`rt[k]` 对应树中第 `levels - k` 层 |
| `paths[]` | 游标的路径指针数组，`paths[0]` 指向根中槽位，`paths[levels]` 指向叶槽位                      |
| `levels`  | 树深度。`levels=0` 表示根直连叶子（无内部节点）。`levels>0` 表示根→...→叶                    |
| fold      | 两兄弟合并（充分小时）或均分（过大时），不改变当前节点的 child_count                         |
| rebalance | 自 `l` 层向上逐层折叠/均分，最后缩根（root 仅一子时降层）                                    |
| underfill | 节点子数 < `FANOUT/2`。B+ 树要求叶半满，内节点放宽（允许偶尔不足）                           |
| findroom  | 向上寻首个非末位层，搬右兄弟入 `rt`，垂空链至目标层。`addroom` 的别名                        |
| 洋葱序    | 自叶向根逐层缝合（k=0..levels），stitchnode 的核心遍历顺序                                   |

## 3. 坐标系

同 [design_insert.md](design_insert.md) §3，不赘述。补充：

- `C->col`：行内列偏移。裂 point 若在行中，`col` 为裂前左侧不成行的残字节数，由 fixremain 在最终阶段补写
- `C->lnu`：叶内行索引（0-based）。`C->lnu == p->breaks[i]` 表示游标在叶末尾（trailing area）
- `C->loff`：叶内字节偏移（`lcL_sumbytes(leaf, 0, C->lnu)`）
- `C->off`：所有前驱叶及祖先的累计字节偏移（不含当前叶内偏移）

---

<!-- 以下各节逐一填充 -->

## 4. 函数清单

| 函数              | 分类   | 功能                                           |
| ----------------- | ------ | ---------------------------------------------- |
| `lcB_cutleaf`     | 裂叶   | 右半入 `rt[0]`，右侧叶兄弟同入                 |
| `lcB_append`      | 追填   | scanner 输出直填叶，叶父满返回 1               |
| `lcD_makechain`   | 扩容   | 建空节点链（含 rootpush）                      |
| `lcD_findroom`    | 扩容   | 向上寻非末位层 + 搬右兄弟 + 垂空链（findroom） |
| `lcD_mergeleaf`   | 合并   | 相邻叶数据迁移（完全合并或部分拷贝）           |
| `lcD_foldleaf`    | 折叠   | 叶层平衡：两叶合并/均分，修游标                |
| `lcD_foldnode`    | 折叠   | 内层平衡：两节点合并/均分，修游标              |
| `lcD_rebalance`   | 修复   | 自 `l` 层向上逐层 foldnode，最后缩根           |
| `lcD_stitchnode`  | 缝合   | 洋葱序将 `rt[]` 搬回树，边缝合边修复           |
| `lcD_stitch`      | 缝合   | mergeleaf + stitchnode + foldleaf + 游标修正   |
| `lcB_fixremain`   | 补写   | 将裂点残字节写入正确行                         |
| `lcB_rollback`    | 回滚   | OOM 时恢复树至裂叶前状态                       |
| `lc_append`       | 入口   | 裂叶→append+findroom→stitch→fixremain          |
| `lc_splice`       | 入口   | 独叶删走 spliceleaf，跨叶走 splicerange        |
| `lcD_splicerange` | 区间删 | 三段法：修剪→挖掘→缝合→修复                    |

## 5. lcB_cutleaf — 裂叶

`static int lcB_cutleaf(lc_Cursor *C, lc_Node *rt)`

于 C 所在叶中延行索引 `C->lnu` 裂开。裂点行及右侧行入新叶，右侧叶兄弟同入 `rt[0]`。游标移回左半叶末。

### 前置条件

- `C` 已定位至合法位置（`paths` 有效、`lnu` 未超界）
- `rt[0]` 已清零（`child_count == 0`）
- `C->col` 已设为裂点行内残字节数（`rm`），供后续 `fixremain` 补写

### 行为

- 记 `p = parent(levels)`、`i = idx(C, p, levels)`
- 若 `p->child_count == 0` 或 `cr = p->breaks[i] - C->lnu == 0`（无右侧行可裂）：直接返回 `LC_OK`，`rt[0]` 保持为空
- 分配新叶 `lr`，`memcpy(lr->bytes, leaf(C)->bytes + C->lnu, cr * sizeof(unsigned))` 复制裂点行及后续行
- 设 `rt[0].children[0] = lr`，`rt[0].bytes[0] = lcL_sumbytes(lr, 0, cr)`，`rt[0].breaks[0] = cr`
- `cc = p->child_count - i`（自裂点起的子节点总数，含裂出之右叶槽位）
- `lcN_copy(&rt[0], 1, p, i + 1, cc - 1)`：将右侧叶兄弟一并搬入 `rt[0]`。`rt[0].child_count = cc`
- `p->breaks[i] = C->lnu`（左半叶截断）、`p->bytes[i] -= rt[0].bytes[0]`（扣除右半度量）
- `db = lcN_sumbytes(&rt[0], 0, cc)`、`dl = lcN_sumbreaks(&rt[0], 0, cc)`
- `lcM_up(C, levels - 1, -(lc_Diff)db, -(lc_Diff)dl)`：祖先度量扣减
- `rt[0].bytes[0] -= C->col`、`lr->bytes[0] -= C->col`：剔除裂点行前半残字节（此部分字节保持于左叶最后一行，不计入 rt 度量）。此 `C->col` 字节即 `rm`——非完整行，由 fixremain 在全流程结束补写
- `C->lnu = p->breaks[i]`、`C->loff = p->bytes[i]`：游标移回左半叶末
- `C->paths[levels] = &p->children[i]`：游标指回左叶槽
- `p->child_count = i + 1`：父节点截断，返回 `LC_OK`

### 后置条件

- `p->child_count == i + 1`：仅左叶及之前的子节点留于父节点中
- `C` 指向左半叶末尾（`C->lnu == p->breaks[i]`、`C->loff == p->bytes[i]`）
- 若 `cr > 0`：`rt[0].child_count >= 1`，`rt[0].children[0]` 为裂出之右半叶
- 若 `cr == 0`：`rt[0].child_count == 0`，树未变动，下游各函数据此跳过
- `C->col` 不变（`rm` 保留，由 fixremain 最终处理）
- 左半叶旧数据 `bytes[C->lnu..]` 仍存于内存，然 `p->breaks[i] = C->lnu` 限界，永不访问

### 不变式

- `rt[0].bytes[0]` 先设为全部右半叶字节和，`p->bytes[i] -= rt[0].bytes[0]` 再扣除——此顺序确保 `p->bytes[i]` 扣减量为完整右半（含 `rm`）
- `lcM_up` 在 `rt[0].child_count` 完整（含右半叶 + 右侧兄弟）后调用，一次更新全部祖先度量
- `cc = p->child_count - i` 包含裂出之右半叶——`rt[0]` 拥有 `cc` 个子节点（children[0]=右半叶, children[1..]=右侧兄弟）

## 6. lcB_append — 追填叶

`static int lcB_append(lc_Cursor *C, lc_Scanner sc, void *ud)`

scanner 输出直填叶：先补完当前末叶，再分配新叶填至满或 scanner 耗尽。返回时 `paths[levels]` 指向下一空叶槽位。

### 前置条件

- `C` 在末叶或末叶后（`assert(i >= p->child_count - 1)`）
- `C` 可能在 cutleaf 后指向左半叶末（`i == p->child_count - 1`）
- scanner 正常，返回行长度（含 `\n`），返回 0 表示耗尽

### 行为

- 记 `pos = lc_offset(C)`（当前绝对字节偏移）、`p = parent(levels)`、`i = idx(C, p, levels)`
- 设 `li = LC_LEAF_FANOUT`（保首轮进入内循环）、`db = dl = 0`
- **外循环**（`; i < LC_FANOUT && li == LC_LEAF_FANOUT; ++i`）：
  - `lf = (lc_Leaf *)p->children[i]`
  - `li = p->breaks[i]`（现存行数）
  - 若 `i >= p->child_count`（需新叶）：`lcL_new` 分配，`li = p->bytes[i] = p->breaks[i] = 0`，`p->children[i] = (lc_Node *)lf`
  - **内循环**（`cb = cl = 0; li < LC_LEAF_FANOUT && (br = sc(ud, pos)) != 0; ++li`）：`lf->bytes[li] = br`，`pos += br`，`cb += br`，`cl += 1`
  - `db += cb`，`dl += cl`，`p->bytes[i] += cb`，`p->breaks[i] += cl`
  - 若 `i >= p->child_count && li == 0`（分配新叶但未填入任何行）：`i -= 1`，`lcP_free(leaves, lf)` 回收枯叶
  - `C->nu += cl`，`C->off += C->loff + cb`，`C->loff = 0`，`C->lnu = 0`
- `lcM_up(C, levels - 1, (lc_Diff)db, (lc_Diff)dl)`：批量更新祖先度量
- `C->paths[levels] = &p->children[i]`：指向下一待用空槽
- 返回 `(p->child_count = i) == LC_FANOUT && li == LC_LEAF_FANOUT`

### 后置条件

- `paths[levels]` 指向 `p->children[i]`——下一空槽。`i` 可能等于 `child_count`（末叶后）或指向未满叶的位置（scanner 耗尽于叶间）
- `C->lnu == 0`（游标在叶首列）、`C->loff == 0`
- `C->off` 已累加写入的所有字节（叶内偏移归并入 off）
- 返回值 = 1 表示父满且末叶满（需 findroom 扩容）；= 0 表示 scanner 耗尽；< 0 表示 OOM

### 不变式

- **外循环仅在前轮叶满时继续**：`li == LC_LEAF_FANOUT` 保证这轮叶不能再填，需要下一叶
- **首轮 `li = LC_LEAF_FANOUT` 必入**：若 C 不在末叶（如 cutleaf 后 `i < child_count`），末叶有未满空间，首轮即填
- **空叶回收**：`i >= p->child_count && li == 0` 表示本轮新分配的叶未使用，回退 i 并释放。防止空叶留于父节点中
- **度量批量更新**：`db`/`dl` 在外循环内逐个叶累加，外循环后一次性 `lcM_up` 更新祖先。避免每行调用 `lcM_up` 的 O(h) 开销
- **paths 指向空位而非有效叶**：调用方（lc_scan 或 lc_append）自行决定是否需要回退

## 7. lcD_makechain + lcD_findroom — 扩容

`static int lcD_makechain(lc_Cursor *C, int from, int to, int nofail)`

自 `from` 层至 `to - 1` 层逐层建空节点，设 `C->paths`。`from < 0` 时先 rootpush。返回 1 表示发生了 rootpush（调用方据此修正层级）。

### 前置条件

- `C->paths[0..from-1]` 已指向有效节点（rootpush 需此路径完整）
- `C->tree->S` 有足够节点（`nofail` 为假时已预分配）
- `from < to`（否则无层需建）

### 行为

- 若 `nofail` 为假且 `lcP_reserve` 失败：返回 `LC_ERRMEM`
- 若 `from < 0`（全满需扩根）：
  - `lcP_alloc` 分配空节点 `nn`，`*nn = *p`（旧根内容拷入新节点）
  - 新根：`bytes[0] = tree->bytes`、`breaks[0] = tree->breaks`、`children[0] = nn`、`child_count = 1`
  - `tree->levels++`，`from = 0`，`to++`，`r = 1`
- 自 `l = from` 至 `l < to` 逐层：
  - `lcP_alloc` 分配空节点 `nn`，`nn->child_count = 0`
  - `p = parent(l)`，将 `nn` 挂为末子，度量槽位初始化为 0
  - `C->paths[l] = &p->children[p->child_count - 1]`
- 若 `from < to`：`C->paths[l] = &nn->children[0]`（最底层空节点之首子位，供上层写入）
- 返回 `r`（rootpush 为 1，否则为 0）

### 后置条件

- `paths[from..l]` 指向各层新分配之空节点槽位
- 若 rootpush：`tree->levels` 增 1，`C->paths[0..]` 需调用方偏移修正

---

`static int lcD_findroom(lc_Cursor *C, lc_Node *rt, int nofail, int l)`

向上寻首个非末位层 `fl`，搬右兄弟入 `rt`，垂空链至 `l` 层。stitchnode 与 lc_append 共用。

### 前置条件

- `l` 层父节点已满，C 在末位或末位后（否则不需要找 room）
- `rt[levels - fl]` 对寻到的 `fl` 层为空（`child_count == 0`）

### 行为

- **找 fl**（自 `fl = l - 1` 向上至 `0`）：
  - `p = parent(fl)`，`i = idx(C, p, fl)`
  - 若 `i < LC_FANOUT - 1`（非末位，右侧有兄弟可搬）→ break
  - 为何不直接用 `child_count`？因 C 在末位、但父节点未必满——仅需右侧有槽位即可停止上溯。findroom 调用时 `l` 层父必满且 C 在末位，故 `fl == l` 时条件必假，循环至少下移一层
- **搬右兄弟**：若 `fl >= 0` 且 `c = p->child_count - i - 1 > 0`：
  - `k = levels - fl`，计 `db`/`dl` 为搬出子树度量之和
  - `lcM_up(C, fl - 1, -db, -dl)`：祖先扣减搬出部分
  - `lcN_copy(&rt[k], 0, p, i + 1, c)`：搬右兄弟入 rt
  - `rt[k].child_count = c`，`p->child_count = i + 1`：父节点截断
- **垂空链**：`assert(fl < l)`（参见"找 fl"段约束），返回 `lcD_makechain(C, fl, l, nofail)`

### 后置条件

- 若 `fl >= 0`：原 `l` 层父满状态已解除（父截断、右兄弟入 rt）
- `paths[fl+1..l]` 指向新空节点链（由 makechain 设置）
- 返回 `makechain` 的结果（rootpush 标识 或 `LC_ERRMEM`）

## 8. lcD_mergeleaf — 叶合并

`static int lcD_mergeleaf(lc_Cursor *C, lc_Node *rt)`

将 `rt[0].children[0]`（被裂出的右半叶或 splice 挖掘的右叶）与当前左邻叶合并。返回位置修正量 `d`，供 stitch末尾 在 foldleaf 后计算正确行位置。

### 前置条件

- `p->child_count >= 1`（左叶存在），`rt[0].child_count > 0 && rt[0].breaks[0] > 0`（有右侧数据可合并）
- `C` 在末叶后（`idx(C, p, l) >= cc - 1`）或末叶中
- 若 `bc == LC_LEAF_FANOUT`（左叶已满），任何数据都不可并入，返回 `d=0`

### 行为

- 记 `ll = p->children[cc-1]`（左叶）、`lr = rt[0].children[0]`（右侧叶）
- `bc = p->breaks[cc-1]`（左叶行数）、`rtbc = rt[0].breaks[0]`（右侧叶行数）
- `dl = min(rtbc, LC_LEAF_FANOUT - bc)`——最多将左叶填满
- `memcpy(ll->bytes + bc, lr->bytes, dl)`：将右侧 `dl` 行拷入左叶末尾
- **完全合并**（`bc + rtbc <= LC_LEAF_FANOUT`）：
  - `lcP_free`：右侧叶已全并入左叶，释放右叶
  - 若 `idx == cc`（C 在末叶后）：`C->lnu = bc`，`C->off -= p->bytes[cc-1]`（C 回退入左叶末）
  - `cc -= 1`：父失一子。`db = -p->bytes[cc]`、`dl = -p->breaks[cc]`（从祖先扣减该子度量）
  - `rt[0].children[0] = p->children[cc]`（旧末叶指针移入 rt[0] 首子位——fixremain/rollback 需通过此指针定位裂点叶）
  - `p->child_count = cc`
  - 返回 `d=0`——完全合并后 C 已精确定位，无需 stitch末尾 修正
- **部分合并**（`bc + rtbc > LC_LEAF_FANOUT`）：
  - `memmove(lr->bytes, lr->bytes + dl, (rtbc - dl))`：rt 叶剩余行前移
  - `db = rt[0].bytes[0] - lcL_sumbytes(lr, 0, rtbc - dl)`：被迁出部分之字节数
  - 若 `idx == cc`：`C->lnu = bc`、`C->off += db`（C 移入左叶合并后位置）
  - 若 `idx < cc`：`C->off += p->bytes[cc-1]`
  - `p->bytes[cc-1] += db`、`p->breaks[cc-1] += dl`：左叶度量加回
  - `d = LC_LEAF_FANOUT - C->lnu`（合并后左叶为满叶 LC_LEAF_FANOUT 行，d 为合并引入的"在 C 之后的行数"，供 foldleaf 后位置修正）
  - `C->lnu = 0`（清除，让 foldleaf 在此值上做修正）
- `rt[0].bytes[0] -= db`、`rt[0].breaks[0] -= dl`：扣除已迁出部分
- `lcM_up(C, l - 1, db, dl)`：祖先度量加回迁入部分（`l-1 < 0` 时仅更新 tree 度量）
- `C->paths[l] = &p->children[cc]`：C 指向末叶（完全合并）或末叶后（部分合并）
- 返回 `d`

### 后置条件

- 左叶行数 = `min(原 bc + rtbc, LC_LEAF_FANOUT)`
- `rt[0]` 的行数减少 `dl`
- `d == 0` 表示完全合并（stitch末尾 跳过位置修正），`d > 0` 表示部分合并（`d = LC_LEAF_FANOUT - bc`——即合并后 C 之后的行数，stitch末尾 据此做位置修正）
- `C->paths[l]` 指向末叶槽位（`p->children[cc]`）

### `d` 的语义

```
部分合并后左叶为满叶（LC_LEAF_FANOUT 行）。C 在 bc 处（原始左叶末行之后）。
d = LC_LEAF_FANOUT - bc = "在 C 之后新引入的行数"。

foldleaf 可能重新分配数据（如从满叶移走 dl 行到右叶）。此时 C 的位置会变化。
stitch末尾 用 d 做双重修正：
  if d > C->lnu: 需要 backwardnode + d -= C->lnu + 重设 lnu
  C->lnu -= d: 最终定位
d 的语义同 stitchnode 中的 d——"真正期待的行在当前行之前多少行"。
```

## 9. lcD_foldleaf — 叶层折叠

`static int lcD_foldleaf(lc_Cursor *C)`

对 C 当前叶及其相邻叶执行平衡：若两叶行数之和 ≤ `LC_LEAF_FANOUT` 则合并（右并入左），否则均分。修 C 的 `paths`、`off`、`lnu` 以应对数据迁移。返回 1 表示发生了合并（父失一子，调用方须 rebalance），返回 0 表示无需 rebalance。

### 前置条件

- `p->child_count > 1`（至少两叶，才有相邻叶可操作）
- C 在叶内或叶末（`lnu <= p->breaks[i]`）

### 行为

- `i = idx(C, p, levels)`，`ls = (lc_Leaf **)&p->children[i]`，`o = *ls`（初始时 C 所在的叶）
- **提前退出**：若 `p->breaks[i] > LC_LEAF_FANOUT / 2`（C 所在叶行数已过半满），直接返回 0。均分方向数学约束保证该叶无需折叠
- 若 `i == p->child_count - 1`（C 在末叶）：`ls -= 1`，`i -= 1`——将待操作对改为 `(children[i], children[i+1])`，其中 `ls[1]` 为 C 原在的末叶，`o` 保存其指针
- `cl = p->breaks[i]`、`cr = p->breaks[i+1]`、`bc = p->bytes[i]`
- **合并路径**（`cl + cr <= LC_LEAF_FANOUT`）：
  - `memcpy(ls[0]->bytes + cl, ls[1]->bytes, cr)`：右叶行拷入左叶
  - `p->breaks[i] += cr`、`p->bytes[i] += p->bytes[i+1]`
  - 若 `*ls != o`（C 原在右叶，已并入左叶）：`*ps -= 1`（C 移入左叶槽），`C->off -= bc`、`C->loff += bc`、`C->lnu += cl`（左叶原有行数）
  - `lcN_remove`：释放右叶并抹除父节点中对应槽位。父失一子，返回 1
- **均分路径**（`cl + cr > LC_LEAF_FANOUT`）：
  - `db = lcD_balanceleaf(ls, cl, cr, 0)`：执行字节级均分。若 `db == 0`（两叶完全均衡无须搬移），直接返回 0
  - `dl = cl - (cl + cr + 1) / 2`（行数差量，仅 db≠0 时计算，故 dl ≠ 0）
  - 断言 `dl != 0 && (dl < 0) != (*ls != o)`：编码不变式——`dl < 0`（右→左搬移）当且仅当 C 原在左叶（`*ls == o`），`dl > 0` 当且仅当 C 原在右叶（`*ls != o`）
  - `p->bytes[i] -= db`、`p->bytes[i+1] += db`
  - `p->breaks[i] -= dl`、`p->breaks[i+1] += dl`
  - **游标修正**（单分支）：`if (*ls != o) C->off -= db, C->lnu += dl`
  - 返回 0

### 游标修正的不变式

均分方向 `dl` 的符号与 `*ls == o` 严格绑定：
- **`*ls == o`**（C 在原左叶）：右叶段数更多（`cr > cl`）导致 `dl < 0`，数据从右叶搬入左叶末段。C 在左叶中位置不变，无需修正
- **`*ls != o`**（C 在原右叶）：左叶段数更多（`cl > cr`）导致 `dl > 0`，数据从左叶搬入右叶前段。C 在右叶中被推后 dl 个段

`dl == 0` 时（`cl == cr+1`）`db == 0` 早退，故到达 assert 时 `dl ≠ 0` 且 `(dl < 0) != (*ls != o)` 恒真。

### 后置条件

- **合并**：父节点 `child_count` 减 1，右叶已释放。C 的 `paths`/`lnu`/`off`/`loff` 均已更新至合并后叶
- **均分**：两叶行数平衡。`db == 0` 时 C 不变；否则 C 的 `off` 和 `lnu` 已按数据迁移量修正
- **不变**：节点 `p->bytes[]` 和 `p->breaks[]` 总和不变

## 10. lcD_foldnode — 内层折叠

`static int lcD_foldnode(lc_Cursor *C, int left, int l)`

内层节点平衡。`left` 参数为 1 时优先探左侧兄弟（stitchnode 的 `fl` 循环使用此标记以正确修复 C 所在路径）。与 foldleaf 结构对称。

### 前置条件

- `l < levels`（操作的层次 < 叶层，即操作的是内部节点）
- `p->child_count > 1`
- `left` 为 1 且 `i > 0` 时向左探兄弟（非叶层 C 不恒在末位）

### 行为

- `ns = (lc_Node **)&p->children[i]`，`o = *ns`（C 所在的原节点）
- **提前退出**：若 `ns[0]->child_count > LC_FANOUT / 2`（C 所在节点子数已过半满），直接返回 0。与 foldleaf 同原理——均分方向数学约束保证该节点无需折叠
- 若 `(i && left) || i == cc - 1`：`ns -= 1`，`i -= 1`——优先左探（`left=1` 时）或末位回退
- `cl = ns[0]->child_count`、`cr = ns[1]->child_count`
- **合并**（`cl + cr <= LC_FANOUT`）：
  - `lcN_copy(ns[0], cl, ns[1], 0, cr)`、`ns[0]->child_count += cr`
  - 父度量更新：`p->bytes[i] += p->bytes[i+1]`、`p->breaks[i] += p->breaks[i+1]`
  - 若 `*ns != o`（C 原在右节点）：`lcN_tx(cp[l+1], ns[0], ns[1], cl)` 修正更深层路径，`cp[l] -= 1` 修正本层路径
  - `lcN_remove` 释放右节点 → 父失一子 → 返回 1
- **均分**（`cl + cr > LC_FANOUT`）：
  - `lcD_balancenode(ns, 0, (*ns == o), ds)`：内部已含 `d == 0` 早退。若返回 0（无搬移），foldnode 直接返回 0
  - `dn = cl - (cl + cr + (*ns == o)) / 2`（子节点数差量，正=左→右。仅 balancenode 成功后计算，故 `dn ≠ 0`）
  - 断言 `dn != 0 && (dn < 0) != (*ns != o)`：与 foldleaf 同不变式——`dn < 0` iff `*ns == o`（C 原在左节点），`dn > 0` iff `*ns != o`（C 原在右节点）
  - `p->bytes[i] -= ds[0]`、`p->bytes[i+1] += ds[0]`
  - `p->breaks[i] -= ds[1]`、`p->breaks[i+1] += ds[1]`
  - **路径修正**（单分支）：`if (*ns != o) cp[l+1] += dn`
  - 返回 0

### 游标修正的不变式

与 foldleaf 完全对称。`dn` 的符号与 `*ns == o` 严格绑定：
- **`*ns == o`**（C 在原左节点）：`dn < 0`，右节点子数更多，数据从右搬入左节点尾部。C 在左节点末不受影响，无需修正
- **`*ns != o`**（C 在原右节点）：`dn > 0`，左节点子数更多，数据从左搬入右节点头部。C 被推后 dn 位（`cp[l+1] += dn`）

`dn == 0`（`cl == cr+(*ns==o)`）时 balancenode 返回 0 早退，故到达 assert 时 `dn ≠ 0` 且 `(dn < 0) != (*ns != o)` 恒真。

游标跨节点跳转（原三分支的前两条 `lcN_tx` 跳对侧）不再需要——L659 提前退出保证游标所在节点仅在不足半满时才进入折叠，此时 `cl+cr < LC_FANOUT`（总会触发合并路径而非均分），或均分路径中游标恒在对侧不动。

`dn` 的均分中点公式含 `(*ns == o)` 项：当 C 在原节点时，给原节点多留一个子节点（避免 C 被挤入对侧）。

### 与 foldleaf 差异

- foldnode 需处理 `cp[l+1]`（更深层路径偏移），因合并/均分后子节点位置变化
- `left` 参数：stitchnode 在缝合右单链时，用 `left=1` 保证 foldnode 优先选择左侧方向，避免将 C 路径推入已修复的右单链区域
- 合并路径中用 `lcN_tx` 修正深层路径（而 foldleaf 仅需 `*ps -= 1`），因内层节点搬迁涉及子节点指针而非段数组

## 11. lcD_rebalance — 缩根

`static void lcD_rebalance(lc_Cursor *C, int l)`

自 `l` 层向上逐层 foldnode 修复 underfill，最后缩根（root 仅一子时降层）。不修 paths 后的 off/lnu/loff——游标位置由调用方后续复原。

### 前置条件

- `l == 0` 或 `l < levels`（l == levels 是叶层，应调用 foldleaf 而非 rebalance）
- C 的 paths 在 `l..levels` 有效

### 行为

- **自 l 向上至 1**：
  - `p = parent(l)`，若 `p->children[idx(C, p, l)]->child_count >= LC_FANOUT / 2`：直接返回（该节点已满足 B+ 树半满约束）
  - 否则 `assert(p->child_count > 1)`（至少两子），调 `lcD_foldnode(C, 0, l)` 均分或合并该层节点
  - foldnode 返回 0（平衡后无需进一步处理）则返回；返回 1（合并发生，父失一子）则继续 `--l` 处理上层
- **缩根**（`while levels && root.child_count == 1`）：
  - `only = parent(1)`（root 的唯一子节点）
  - `C->tree->root = *only`（唯一子节点内容拷入 root）
  - `lcP_free` 释放旧子节点
  - `tree->levels--`
  - `C->paths[0] += i`（paths[0] 原指向 root 中的旧子节点槽位，现需偏移至新 root 中对应位置）
  - `memmove(C->paths + 1, C->paths + 2, levels * sizeof(...))`：paths 数组上移一层（因 levels 减 1）

### 后置条件

- 树上满足 B+ 树半满约束（各内节点子数 ≥ FANOUT/2，如有能力满足）
- root.child_count ≥ 2（或 levels=0 时 root.child_count 任意）
- C->paths[0..levels] 已修偏移（缩根时 paths 数组已压缩）

## 12. lcD_stitchnode — 洋葱序缝合

`static void lcD_stitchnode(lc_Cursor *C, lc_Node *rt)`

stitch 的主引擎。洋葱序（自叶向根，k=0..levels）将 `rt[k]` 中数据搬回树。边缝合边修复 underfill：先填满当前父节点（防溢出机制），再逐层 foldnode、最后 findroom 为剩余数据建新节点链。

### 前置条件

- `rt[0..levels]` 存有待缝合数据（来自 cutleaf/findroom/splicerange）
- `C->paths[0..levels]` 有效（每层在末位或末位后）
- `findroom` 不会 OOM（stitch 入口已 `lcP_reserve` 预分配节点）

### 循环结构

洋葱序 `for (k = 0; k <= lcK_levels(C); ++k)`。注意循环条件 `k <= lcK_levels(C)` 是**动态**的——若 kl==0 时 rebalance 缩根（levels 变化），循环上界随之调整。

### 变量

- `k`：洋葱层（k=0=叶层，k=levels=根层）
- `kl = lcK_levels(C) - k`：rt[k] 在树中对应的层级
- `d`：路径回退计数。"真正的路径"与"当前 C->paths 指向的位置"之间的子节点数差量。在本次迭代结束时设值，下一轮迭代中 `backwardnode(C, d, l)` 实际回退
- `l`：待修复层级范围的下界（当前需 fold/rebalance 的最低层号）

### 每轮行为

- `rtcc = rt[k].child_count`，`rt[k].child_count = 0`（取数据并清零——stitch 结束时 rt[] 全为零）
- **首复制**：`m = min(rtcc, LC_FANOUT - p->child_count)`
  - `lcN_copy(p, p->child_count, &rt[k], 0, m)` 搬 m 个子节点到父节点尾部
  - `p->child_count += m`
  - `lcM_up(C, kl - 1, db, dl)` 祖先度量加回
- **早退判断**：`if (!(m < rtcc || kl == 0)) continue`
  - 若全部搬完（`m == rtcc`）且 `kl > 0`（非根层），跳过本轮修复。`kl == 0` 恒进入修复——此为保底修复
  - 为何 kl==0 不早退？见下文"保底修复"
- **保底修复**（kl == 0 且 root.child_count == 1）：
  - `lcD_rebalance(C, 0)` 缩根（若根仅一子）。缩根使 `lcK_levels(C)` 减 1
  - `l -= (k - lcK_levels(C))`：因缩根减层，`l` 需下移适配。此调整使后续 foldnode 循环在正确的层级范围执行
  - 为何"保底"？stitchnode 缝合过程中 l 逐次下移、逐步缩小修复范围；但根层（kl==0）是最后一道防线——不论此前 l 停留在何层，kl==0 时强制执行 foldnode + 若根仅一子则缩根，确保整树平衡
- **foldnode 循环**：`for (fl = kl; fl < l; ++fl) lcD_foldnode(C, (fl == kl), fl)`
  - 自 kl 层向上至 l-1 层逐层 foldnode
  - `left = (fl == kl)`：首层调用 foldnode 时 C 在末位（stitch 保证 paths 在可能 underfill 的节点上），优先左探
- **路径回退**：`if (k) lcD_backwardnode(C, d, l)`
  - d 由上一轮迭代设置——见下文 d 的由来
  - k==0（叶层）时 skip——叶层不在此修路径，由 stitch末尾 foldleaf + d 修正承担
- **全搬完早退**：`if (!(m < rtcc)) continue`（虽进入修复块但 m == rtcc，无需 findroom）
- **次复制**（`m < rtcc`，首复制未搬尽）：
  - `l = kl`：待修复层级下界降至当前层
  - `d = k ? LC_FANOUT - lcK_idx(C, lcK_parent(C, l), l) : m`
    - 叶层（k==0）：`d = m`——当前父节点中新搬入的子节点数
    - 内层（k>0）：`d = LC_FANOUT - idx(C, parent(l), l)`——当前父节点中 C 右侧的槽位数（findroom 前父节点必满，此值为右侧搬出子节点数）
  - `lcD_findroom(C, rt, 1, l)`：向上寻非末位层，搬右兄弟入 rt，垂空链。`l += r`（r 为 rootpush 补偿）
  - `lcN_copy(p, 0, &rt[k], m, rtcc - m)`：剩余子节点搬入新链的根 `p`
  - `lcM_up(C, l - 1, db, dl)` 度量加回

### d 的双重身份与延迟生效

`d` 在每轮迭代的末尾设置，在**下一轮**迭代的 foldnode 之后通过 `backwardnode(C, d, l)` 实际生效。

为何延迟？findroom 搬右兄弟入 rt 时，C 所在路径的**左侧**节点必然已满（若不满，findroom 的上溯循环会停在更低的层）。而 C 路径上唯一可能 underfill 的节点在 findroom 新垂链的**右侧**单链树上。因此需先在前轮 foldnode 修复右侧单链树的 underfill，再通过次轮 backwardnode 修正 C 的 paths 指向。

`d` 值为"修复过程中合并/消除的右侧子节点数"，backwardnode 据此将 C 回退正确位置。

### kl==0 保底修复与缩根处理

`kl == 0`（k == levels，当前操作根层）时恒进入修复块，即使 m == rtcc。这是最后一层——无论之上的 l 停留在何处、有多少层 foldnode 未及处理，kl==0 处执行：
1. 若 root 仅一子 → rebalance 缩根（levels 减 1）
2. foldnode 自 kl 向上修复
3. backwardnode(C, d, l)：用前轮设的 d 修正 C 的 paths

若缩根发生，循环条件 `k <= lcK_levels(C)` 重新评估（levels 已降）——此轮结束后循环可能提前退出，但此前轮次设的 d 和 l 已在本轮 backwardnode 和 foldnode 中正确消耗。

### 后置条件

- `rt[0..levels]` 全部清零（`child_count == 0`）
- 树中所有节点满足 B+ 树半满约束
- C 的 paths 已更新至各层末位（stitch末尾 做最终 leaf-level 修正）
- `d` 已清空（所有轮次的 backwardnode 已执行）

## 13. lcD_stitch — 缝合入口

`static int lcD_stitch(lc_Cursor *C, lc_Node *rt)`

lc_append 之缝合入口。调度 mergeleaf → stitchnode → foldleaf → 位置修正。

### 事务性保证

stitch 为**事务操作**：要么完全成功，要么完全失败（返回 `LC_ERRMEM`）。保证来自入口处的 `lcP_reserve(S, &S->nodes, l + 2)`——预分配 `levels + 2` 个节点供 stitchnode 内全部 `makechain` 调用使用。

**节点数上界证明**：stitchnode 洋葱序（k=0..levels）中，每轮仅在 `m < rtcc`（当前父节点装不下 rt[k] 全部数据）时调用 `findroom → makechain`。此时 `l = kl = levels - k`。`makechain(from=fl, to=kl)` 消耗 `max(0, kl - fl) + rootpush` 个节点。

关键：rootpush 后 `to += 1`，chain 覆盖 `[0, to)` 范围比原 `kl` 多层，rootpush 本身也消耗 1 个节点。最坏情况（k=0 全满扩根）：`rootpush(1) + chain(l=0..levels)(levels+1) = levels + 2`。各轮链互不重叠（`kl` 逐轮递减），总消耗 ≤ `levels + 2`。

stitch 内部其他子调用均不分配节点：`mergeleaf` 仅释放叶子，`lcN_copy/lcN_move` 仅搬移已有指针，`lcD_foldleaf/lcD_foldnode/lcD_rebalance` 仅释放节点（合并路径）或原地修改。因此一旦 reserve 成功，stitch 必定返回 `LC_OK`。

### 前置条件

- `lcP_reserve` 已预分配 `l + 2` 个节点（保 stitchnode 不 OOM）
- `C` 在末叶或末叶后（`assert(idx >= cc - 1)`）
- `rt[0..levels]` 存有待缝合数据

### 行为

- 预分配节点保 findroom 不 OOM
- `d = (cc && rtcc) ? lcD_mergeleaf(C, rt) : 0`：将裂点叶与 `rt[0].children[0]` 合并。`d` 的语义见 §8——为 0 则表示完全合并（无需 stitch末尾 修正）；> 0 则表示部分合并（`d = LC_LEAF_FANOUT - bc` = 合并后 C 之后的行数）
- `lcD_stitchnode(C, rt)`：洋葱序缝合（§12），事务性保证永不失败
- 修正 `paths[l]`：若 `i >= cc`（C 在越界位置），回退至末叶：`i = cc - 1`，`C->paths[l] = &p->children[i]`，`C->lnu = p->breaks[i]`，`C->off -= p->bytes[i]`
- `lcD_foldleaf(C)`：叶层折叠。折叠后可能 `lcD_rebalance(C, l-1)` 缩根
- **d 修正**（§8 `d` 的语义之应用）：
  - 若 `d > C->lnu`：foldleaf 没有把 C 移到足够远。d 中有一部分行跨到了前一个叶子中。执行 backwardnode 回到前一个叶子：`d -= C->lnu`、`C->lnu = p->breaks[i]`（用当前叶行数做基底）
  - `C->lnu -= d`：最终定位。无论是否 backwardnode，此行必然执行——将 C 在叶内的行索引修正
- 返回 `C->loff = lcL_sumbytes(leaf, 0, C->lnu)` 重算 loff

### 后置条件

- C 位于正确叶中，`C->lnu` 准确，`C->loff` 已重算
- 树完全平衡（stitchnode + foldleaf + rebalance 已覆盖所有层）

## 14. lcB_fixremain — 补裂点残字节

`static int lcB_fixremain(lc_Cursor *sC, int l, unsigned rm)`

S3 结束后调用，通过 sC（cutleaf 后快照）定位裂点叶，补写 `rm`（裂点行前半残字节 `C->col`）。返回 1 表示 rm 已补写入树（需调用方修 `C->loff` 或 `C->off`），返回 0 表示裂点叶已被 fold 合并（rm 已随数据自然归入）。

### 前置条件

- `sC` 为 cutleaf 后的游标快照（裂点左叶末）
- `l = sl`（cutleaf 时的 levels）
- `rm = C->col`（cutleaf 时的 col 值，调用方保存的裂点残字节）

### 行为

- `p = parent(sC, l)`，`i = idx(sC, p, l)`
- 若 `i < p->child_count`（裂点叶仍存，stitch 中未被 fold 合并）：
  - `k = lcK_levels(sC) - l`（stitch 中 rootpush 所致的层级差）
  - `memmove(sp + k + 1, sp + 1, l * sizeof(...))`：sC 旧路径 paths[1..l] 下移 k 位
  - `for (l = 0; l < k; ++l) sp[l] = &parent(sC, l)->children[0]`：新增层级填全量左侧路径
  - `sp[k] = &parent(sC, k)->children[i0]`：旧根层 paths[0] 偏移恢复
  - `lcK_leaf(sC)->bytes[sC->lnu] += rm`：裂点行前半字节加回，`lcM_up(sC, k, rm, 0)`
  - 返回 1
- 否则（裂点叶不存在）返回 0

### 调用方后续

- fixremain 返回 1（rm 已补写，`C->col` 非零）：若 sC 与 C 在同叶 → `C->loff += C->col`；否则 → `C->off += C->col`。`C->col = 0`
- fixremain 返回 0：`C->col` 本身就是 rm 的全部值，无需额外操作

## 15. lcB_rollback — OOM 回滚

`static int lcB_rollback(lc_Cursor *C, lc_Node *rt, lc_Cursor *sC, int sl)`

OOM 时恢复树至 cutleaf 后状态。降根至裂叶时高度（清除 append/findroom 新增的 scanner 数据）、合并裂点叶、释右侧孤子树、缝合 rt。返回 `LC_ERRMEM`。

### 前置条件

- `sC` 为 cutleaf 后的游标快照，`sl = lcK_levels(sC)`（裂叶时树高度）
- 树已部分修改（append/findroom 可能已写入部分数据、可能已 rootpush 增层）
- **严格依赖 stitch 事务性**：stitch 第一步 `mergeleaf` 会修改树（合并左右叶）和 rt[0]（`rt[0].children[0]` 被换为旧左叶指针），stitchnode 全程修改 rt 各层数据。若 stitch 中途失败，树和 rt 均处于不一致状态，rollback 无法正确恢复。stitch 的事务性由 §13 的 reserve 保证，故当前实现安全。

### 行为

- **降根**（`for (k = levels; k > sl; --k)`）：清除 findroom rootpush 增加的层
  - `lcN_freechildren`：释放 root.children[1..child_count] 所有子树
  - `tree->bytes = root.bytes[0]`、`tree->breaks = root.breaks[0]`（退度量至裂时水平）
  - `p = root.children[0]`，`root = *p`（旧根内容拷回 root），`lcP_free(nodes, p)`（释包裹节点）
- **复原游标**：`*C = *sC`，`C->tree->levels = sl`
- **合并裂点叶**（若 `rt[0].child_count > 0`，即有数据需要复原）：
  - `memcpy(&lcK_leaf(C)->bytes[C->lnu], rt[0].children[0], rtbc * sizeof(unsigned))`：右半叶行拷回左半叶
  - `lcM_up(C, sl, rt[0].bytes[0], rt[0].breaks[0])` 度量加回
  - `*C->paths[sl] = rt[0].children[0]`（右半叶指针放回树），`rt[0].children[0] = (lc_Node *)lf`（左半叶移入 rt 待缝合）
- **缝合 rt**（洋葱序 `for (l = 0; l <= sl; ++l)`，`k = sl - l`）：
  - `i = idx(C, p, l) + (k > 0)`（非叶层索引右移一位——因 mergeleaf 搬右兄弟到 rt 时已改 child_count）
  - `db/dl = rt[k] 和 - p.children[i..cc] 和`（净差量）
  - `lcN_freechildren` 释旧子，`lcN_copy` 拷入 rt 内容
  - `lcM_up(C, l-1, db, dl)`，`p->child_count = i + rt[k].child_count`
- 返回 `LC_ERRMEM`

### 后置条件

- 树完全恢复至 cutleaf 后状态（scanner 填充数据全部清除）
- C 指向裂点左叶末（与 sC 相同）

## 16. lc_append — 插入主流程

`LC_API int lc_append(lc_Cursor *C, unsigned e, lc_Scanner *sc, void *ud)`

五段流程：

**S1 — 参数校验**：若 `C == NULL || C->tree == NULL || sc == NULL` → `LC_ERRPARAM`。`rt[]` 清空。

**S2 — 裂叶**：`lcB_cutleaf(C, rt)` 将 C 右侧数据搬入 `rt[0]`。失败 → `return LC_ERRMEM`。保存快照 `sC = *C`、`sl = lcK_levels(C)`（备 OOM 回滚与 rm 补写）。

**S3 — 追搬轮替**：while 循环：
- `r = lcB_append(C, sc, ud)`：scanner 输出填叶。`r < 0` → rollback；`r == 0` → break（scanner 耗尽）
- `C->off += C->loff, C->loff = 0, C->lnu = 0`（append 已做此操作，此为防御性）
- `r = lcD_findroom(C, rt, 0, lcK_levels(C))`：叶父满则扩容搬右兄弟。失败 → rollback

**S4 — 缝合**：`lcD_stitch(C, rt)`。失败 → rollback。

**S5 — 补 e/rm**：
- `lcB_fixremain(&sC, sl, C->col)`：补裂点残字节 `rm`。若返 1：sC 与 C 同叶 → `C->loff += C->col`；否则 → `C->off += C->col`。`C->col = 0`
- `p = parent(levels)`，`i = idx(C, p, levels)`
- 若 `C->lnu < p->breaks[i]`（游标在有效行内）：`lcK_leaf(C)->bytes[C->lnu] += e`，`lcM_up(C, levels, e, 0)`
- `C->col += e`，返回 `LC_OK`

### 不变式

- **e 最后写入**：`e` 非完整行，不可在 S2-S4 中提前写入树——若后续失败则 rollback 无法撤销
- **rm 补写时机**：`rm` 在 fixremain 中定位裂点叶后补写。若裂点叶被 fold 合并（fixremain 返 0），rm 已随数据自然归入
- **sC 快照**：在 cutleaf 之后保存（非之前），因 cutleaf 是第一个可能失败的操作

## 17. lcD_splicerange + lc_splice — 区间删除

### lcD_splicerange

`static void lcD_splicerange(lc_Cursor *L, lc_Cursor *R)`

L 和 R 界定删除区间（L 在区间左端、R 在区间右端）。操作分为：求分岔层 → 修边 → 自底向上剪除 L 右 + R 右入 rt → 分岔层处理 → stitch 缝合回树。rt 是惰性使用的——仅在有数据可搬的层才存入 `rt[k]`，而非每层必填充。

**第一段 — 求分岔 + 修边**：
- `l`：首个 `L->paths[l] != R->paths[l]` 的层——L 与 R 分岔层
- `lcD_trimright(L)`：将 L 当前叶中 `lnu` 右侧的行全部删除（仅更新度量，不移动数据）
- `lcD_trimleft(R)`：将 R 当前叶中 `lnu` 左侧的行全部删除（仅更新度量，不移动数据）

**第二段 — 自底向上剪除**（`for kl = levels down to l+1`）：
- **L 侧**：`i = idx(L, p, kl)`。L 右侧的兄弟子树全部删除：汇总度量 → `lcM_up` 扣祖先 → `lcN_remove` 释放并压实父节点
- **R 侧**：`k = levels(R) - kl`（rt 槽位）。`i += !(k==0 && breaks[i]!=0)`（叶层时若当前位置为非空叶则右跳一位，避免取 L 已修剪的半残叶）。将 `children[i..cc)` 搬入 `rt[k]`，释放旧子，父节点 child_count 归零
- rt 在此过程中只被惰性填充——仅在 R 的某层实际有右侧兄弟时，该层的 `rt[k]` 才被写入；其他层保持 `child_count == 0`。stitch 遇空 rt[k] 直接跳过（洋葱序中 `rtcc == 0 → continue`）

**第三段 — 分岔层处理**：
- `k = levels(L) - l`
- R 侧：`i += !(k==0 && breaks[i]!=0)`（叶层跳半残叶）。`lcN_copy(&rt[k], 0, p, i, cc-i)` 将 R 右侧搬入 rt。`p->child_count = i`（截断父节点）
- L 侧：`i = idx(L, p, l)`。汇总 L 与 R 之间（i+1..cc）的度量 → `lcM_up` 扣祖先 → `lcN_remove` 释放中段子树并压实。中段子树直接删除，不存于 rt

**第四段 — 缝合**：
- `lcD_stitch(L, rt)`：将 rt 中 R 右侧数据缝合回树（mergeleaf → stitchnode → foldleaf → 位置修正）
- 最后将 `R->col`（trimleft 时从 R 叶抹去的残字节）加回 L 当前行

### lc_splice

`LC_API void lc_splice(lc_Cursor *C, size_t del, size_t ins)`

区间删除/插入的公开 API：

1. 参数校验（空指针、无操作、空树、越界 → col 直接加减）
2. `del = min(del, tree->bytes - offset(C))`（clamp 到树内）
3. 删空树 → `lcD_reset(C, ins)`（清空树，col = ins）
4. `del > 0` 时：
   - 同叶：`lcD_spliceleaf(C, del)`（在叶内逐行消费删除字节，可能触发 foldleaf + rebalance）
   - 跨叶：`lcD_splicerange(C, &R)`（先 `lc_advance(&R, del)` 定位 R，再三段法删除区间）
5. `C->loff = lcL_sumbytes(leaf, 0, C->lnu)`（重算 loff）
6. 若树已删空：`lcD_reset(C, ins)`（重置）
7. `ins > 0` 时：将 `ins` 字节加到当前行末（或 trailing area）

### rt 惰性使用设计

与传统的 split/merge 算法不同，splicerange 的 rt 数组是**随用随放**的：仅在有右侧数据可搬的层才写入 `rt[k]`（`child_count > 0`），其余层保持为空（`child_count == 0`）。stitchnode 的洋葱序中检查 `rtcc = rt[k].child_count`，遇 0 直接跳过本层缝合。这种设计避免了在未涉及的层做无意义的拷入拷出。

## 18. 关键不变式

1. **append 后 paths[levels] 指向空位**：返回时 `paths[levels]` 指向下一待分配叶槽位（非最后有效叶）。lc_scan 若 `C.off != 0` 则后撤一位；lc_append 的 findroom 从此空位起建空链扩容。append 是 `off/nu/loff/lnu` 唯一增量更新者。

2. **stitchnode 中 C 总在可能 underfill 的节点上**：findroom 的垂链策略保证 C->paths 路径上的节点左侧必为满节点（若不满，findroom 上溯会停在更低层），唯一可能 underfill 者就是 C->paths 所在的右侧单链。故 foldnode 时 `left=1` 优先左探，自动处理 underfill。

3. **d 的延迟生效**：stitchnode 的 `d` 在后一轮迭代中经 backwardnode 生效。findroom 建新链时右侧单链节点未及修复，此轮 d 记录的是"待修复的右侧子节点数"。前轮 foldnode 修复右链 underfill，后轮 backwardnode 用 d 修正 C->paths。

4. **kl==0 为保底修复**：stitchnode 循环中不论之前各层是否已修复，根层（kl==0）必进入修复块执行 foldnode + 若根仅一子则缩根。循环条件 `k <= lcK_levels(C)` 动态适应缩根。

5. **rm 与 e 须最后更新**：裂点残字节 `rm`（`C->col`）与尾缀字节 `e` 皆非完整行，须在 S2-S4 全部成功后于 S5 补写。若提前入树而后失败，rollback 无法撤销。

6. **sC 快照定位裂点**：`sC = *C, sl = lcK_levels(C)` 于 cutleaf 后保存——若 cutleaf 成功而后续失败，用 sC+sl 回滚至裂叶后状态。若全部成功，用 sC+sl 在 fixremain 中定位裂点叶补 `rm`。

7. **rt[0].children[0] 恒为原裂点叶**：cutleaf 将右半叶指针写入此处。stitch 清零 `rt[k].child_count` 但不动 children 数组，fixremain 通过 sC（而非 rt）判定裂点叶状态。

8. **cutleaf 后左半叶旧数据破弃**：裂叶后左半叶 `bytes[C->lnu..]` 仍存于内存，然 `p->breaks[i] = C->lnu` 限界，永不访问。

9. **mergeleaf d 的语义**：`d = LC_LEAF_FANOUT - bc`（合并后满叶中，C 之后的行数）。foldleaf 可能重新分配数据后，stitch末尾 用 `d > C->lnu` 判定是否需要 backwardnode，用 `C->lnu -= d` 完成最终定位。与 stitchnode 之 d 语义一致——"真正期待的行在当前行之前多少行"。

10. **stitchnode 循环上界动态**：`for (k = 0; k <= lcK_levels(C); ++k)` 中 `lcK_levels(C)` 在读。若 kl==0 时 `lcD_rebalance` 缩根（levels 减 1），循环提前终止——此前轮次的 d 已在对应 backwardnode 中消耗，未处理的上层数据在缩根时自然归位。
