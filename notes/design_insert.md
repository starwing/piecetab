# lc_append 与 lc_scan 设计

## 1. 总纲

树中插入文本之分两步：先在插入点裂开（将游标右侧数据搬入 `rt[]` 临时数组），再以 append 消化 scanner 输出，findroom 扩容，最后将 rt 中数据缝合回树。

- **lcB_cutleaf**：裂叶——右半入 `rt[0]`，右侧叶兄弟同入 `rt[0]`
- **lcB_append**：从 scanner 读取行，填叶。返回时 `paths[levels]` 指向空叶位。返回 1=叶父满、0=scanner 枯、<0=OOM
- **lcB_findroom**：叶父满时调用，从 `l` 层向上寻首个非末位层（`idx < FANOUT-1`），搬右兄弟入 `rt`，垂空链至 `l` 层
- **lcB_makechain**：在 from 至 to-1 层建空节点链，设 `C->paths`。findroom 与 lc_scan 共用。`from < 0` 时先 rootpush
- **lcB_stitch**：洋葱序将 `rt[]` 中数据缝合回树。返回时 `rt[]` 清零，`paths` 已修偏移。自叶向根逐层缝合，边缝合边修复（含整树 balancerange）
- **lcB_balancerange**：修复 `[s, e)` 层级范围内的 underfill 问题。并修复 paths 指向
- **lcB_fixremain**：插入完成后修复裂点叶残字节 `C->col`，对齐 sC 路径以补写
- **lcB_rollback**：OOM 回滚——降根至裂叶时高度、合并裂点叶、释孤缝合 rt

高层组合：

- **lc_scan = lcB_append ⊛ lcB_makechain** + 内联 fold/rebalance（树尾轮流追叶-扩容，末修填缩根）
- **lc_append = lcB_cutleaf ⊛ lcB_append ⊛ lcB_findroom + lcB_stitch + lcB_fixremain**（裂开插入点后轮流追叶-搬右，stitch 缝合回树并整树，fixremain 补裂点残字节）
- **OOM 路径 = lcB_rollback**（降根 + 并叶 + 释孤 + 缝合 rt）

`⊛` 表示轮替：先调右（append），若返 1（叶父满）则调左（扩容），如此反复至 scanner 枯。

二者共用 append。区别在于：
- **lc_scan** 在树尾追加，用 makechain 建空链（含 rootpush，无右侧数据需搬）。fold/rebalance 内联于 lc_scan 结尾
- **lc_append** 在中间插入，用 cutleaf 裂开插入点，findroom 搬右兄弟入 `rt[]`，stitch 缝合回树并整树（含 balancerange fold），fixremain 补裂点残字节

`rt[]` 数组为 findroom 与 stitch 之通信媒介：findroom 将父节点中右侧兄弟搬入 `rt[levels-fl]`，stitch 将 `rt[k]` 逐层搬回树 `C->paths` 所指向之右侧。stitch 后 `rt` 中各层清零，然 `rt[0].children[0]` 仍指原裂点叶——fixremain 据此判定叶层是否被 fold 合并（详见 §12）。

---

## 2. 行模型

- 每行以 `\n` 结尾。`bytes[k]` 为该行之字节数（含 `\n`）
- 文件末若无 `\n`，称 **trailing area**——不存叶中，仅以 `C->col` 表示
- 每叶最多 `LC_LEAF_FANOUT` 行，`breaks` 即行数
- scanner 返回行字节数（含 `\n`），返回 0 耗尽。scanner **不可生成不带 `\n` 之行**

### col 和 e

`lc_append(C, e, scanner, ud)` 中：
- **C->col**：插入算法会在 C 位置直接裂叶，但C可能指向行中，这意味着行内 `[0, C->col)` 范围字节变成“不成行的残字节”，这些字节会被从 cutleaf 的右侧抹去，直到插入完成后再插入到 `sC` （即插入前 C 位置）所在的行
- **e**：额外尾缀字节（不成行），插入文本之末若不以 `\n` 终则以此计

`C->col` 与 e 皆须在插入全流程（S2-S4）完成后于 S5 最后更新，**不可提前**：若提前写入而后 append/findroom/stitch 失败，rollback 无从撤销已写入树之字节，树度量将永久错误。

---

## 3. 坐标系

- `node->children`：`l == levels` 时存 `lc_Leaf*`，`l < levels` 时存 `lc_Node*`
- `paths[]`：游标路径节点指针数组，层级 0..levels。每层 parent 节点中某槽位指向下一层 parent（或叶）
  - 不变式：
    - `l == 0`: `paths[0] == &tree->root.children[i]`
    - `l > 0`: `paths[l] == &(*paths[l-1])->children[i]`
- `parent(l)`：第 l 层 parent 节点，`l=0` 根层，`l=levels` 叶层
  - 不变式：`paths[l] == &parent(l)->children[i]`
  - `l == 0`：根层 `parent(0) = &tree->root`
  - `l > 0`：内层/叶层 `parent(l) = *paths[l-1]`
- `lcK_idx(C, p, l)` = `C->paths[l] - p->children`（`paths[l]` 在父节点中的子索引）
- `lcK_levels(C)` = `C->tree->levels`

 光标字段更新约束：
- `off`/`idx`：仅 `lcB_append` 增量更新（因其增改内容）；`lcB_cutleaf` 不改 off/idx——游标裂后仍指向左半叶末，off/idx 度量正确无需更动
- `loff`/`lidx`/`col`：`lcB_append` 每轮迭代归零；`lcB_cutleaf` 重设为左半叶末
- `paths`：各函数各司其职，其余函数仅动 `paths`

---

## 4. 宏与工具

### lcB_newleaf / lcB_newnode

- `lcB_newleaf(S, lf)`：从 `S->leaves` 池分配叶 `lf`，失败则 `return LC_ERRMEM`
- `lcB_newnode(S, n)`：从 `S->nodes` 池分配节点 `n`，失败则 `return LC_ERRMEM`

分配即检查。`lcB_newleaf`/`lcB_newnode` 失败时直接 `return LC_ERRMEM`，由调用方 `lc_append` 捕获后走 `lcB_rollback` 回滚。

### lcM_up

`lcM_up(C, l, db, dl)` 自 `l` 层向上至根，沿 `C->paths` 逐层将 `db`/`dl` 加至对应 `bytes[i]`/`breaks[i]`，并更新 `tree->bytes`/`tree->breaks`。内置零增量守卫：`db == 0 && dl == 0` 时直接返回，调用方免守卫。

### lcN_copy / lcN_sumbytes / lcN_sumbreaks

- `lcN_copy(dst, dst_ofs, src, src_ofs, cnt)`：自 src 之 `src_ofs` 起复制 cnt 个子节点（含 bytes/breaks/children）至 dst 之 `dst_ofs` 起
- `lcN_sumbytes(n, start, end)`：`n->bytes[start..end)` 之和（end 不含）
- `lcN_sumbreaks(n, start, end)`：同上但取 breaks

---

## 5. lcB_cutleaf — 裂叶

`static int lcB_cutleaf(lc_Cursor *C, lc_Node *rt)`

裂 C 所在叶。被裂行整体入新叶，右半及右侧叶兄弟入 `rt[0]`。游标移回左半叶末。若无右侧行可裂（`C->lidx == p->breaks[i]` 且右无兄弟），返回 `LC_OK`（无操作）。

**无操作时**：`rt[0].child_count == 0`。下游 append 从 C 当前位置（叶末）继续填叶；stitch 遇 `rkcc == 0` 跳过缝合，因叶未裂、无数据需搬回。fixremain 遇 `rt[0].child_count == 0` 则跳过叶内偏移修复。此路径与正常裂叶路径在数据流上一致——惟 rt 始终为空。

- 记 `p = parent(levels)`、`i = idx(C, p, levels)`
- 若 `p->child_count == 0` 或 `cr = p->breaks[i] - C->lidx == 0`：返回 `LC_OK`（无右侧行可裂）
- `lcB_newleaf(S, lr)` 分配右半叶
- `memcpy(lr->bytes, leaf(C)->bytes + C->lidx, cr * sizeof(unsigned))` 复制被裂行及后续行
- 设 `rt[0].children[0] = lr`，`rt[0].bytes[0] = lcL_sumbytes(lr, 0, cr)`，`rt[0].breaks[0] = cr`
- `cc = p->child_count - i`（裂点起子节点数，含左半叶下一槽位）
- `lcN_copy(&rt[0], 1, p, i + 1, cc - 1)` 搬叶兄弟入 `rt[0]`，`rt[0].child_count = cc`
- `p->breaks[i] = C->lidx`（左半叶段数），`p->bytes[i] -= rt[0].bytes[0]`（减去右半字节量）
- 计 `db = lcN_sumbytes(&rt[0], 0, cc)`、`dl = lcN_sumbreaks(&rt[0], 0, cc)`
- `lcM_up(C, levels - 1, -(lc_Diff)db, -(lc_Diff)dl)` 祖先扣减
- `rt[0].bytes[0] -= C->col`、`lr->bytes[0] -= C->col`（剔除 `rm` 字节），`C->col = 0`
- `C->lidx = p->breaks[i]`、`C->loff = p->bytes[i]`（游标移回左半叶末）
- `C->paths[levels] = &p->children[i]`
- `p->child_count = i + 1`（截断），返回 `LC_OK`

**不变式**：
- cutleaf 后 `rt[0].child_count >= 1`（恒有右半叶，除非无内容可裂——此时 `rt[0]` 为空，下游各函数据此跳过相关处理）
- 左半叶旧数据 `bytes[C->lidx..]` 仍存，然 `p->breaks[i] = C->lidx` 限界，永不访问
- `p->child_count = i + 1`——仅左半及之前的叶留于父节点

**设计要点**：
- `rt[0].bytes[0]` 先设为全部右半叶字节和，再减 `C->col`（剔除 `rm`）——此顺序使 `p->bytes[i] -= rt[0].bytes[0]` 正确扣除完整右半
- `lcM_up` 在 `rt[0].child_count` 完整（含右半叶 + 右侧兄弟）后调用，一次更新全部祖先度量
- `cc = p->child_count - i`（裂点起子节点总数，含裂出之右半叶槽位）

---

## 7. lcB_makechain — 垂空链（含 rootpush）

`static int lcB_makechain(lc_Cursor *C, int from, int to)`

在层 from 至 to-1 逐层建空节点，设 `C->paths`。findroom 与 lc_scan 共用。若 `from < 0` 则先 rootpush，然后自 0 层起垂链至 to 层。

- 若 `from < 0`（全满需扩根）：
  - `lcB_newnode(S, nn)`，`*nn = *p`（旧根入新节点）
  - 新根：`bytes[0] = tree->bytes`、`breaks[0] = tree->breaks`、`children[0] = nn`、`child_count = 1`
  - `tree->levels++`，`from = 0`，`to++`
- 自 `l = from` 至 `l < to`，逐层：
  - `lcB_newnode(S, nn)` 分配空节点，`nn->child_count = 0`
  - `p = parent(l)`，将 `nn` 挂为 `p` 之末子：`p->children[p->child_count] = nn`，`p->child_count++`
  - 设对应度量槽位为 0：`p->bytes[p->child_count - 1] = 0`、`p->breaks[...] = 0`
  - `C->paths[l] = &p->children[p->child_count - 1]`
- 末层：`C->paths[l] = &nn->children[0]`（链末空节点之首子位，供上层写入）
- 返回 `LC_OK`

**rootpush 注意**：rootpush 后 `C->paths[0]` 未更新为新根 children 偏移——调用方须在返回后自行修复。空节点不 memset——仅需 `child_count = 0`，bytes/breaks 首次使用时赋值。

---

## 8. lcB_findroom — 搬右 + 垂空链

`static int lcB_findroom(lc_Cursor *C, lc_Node *rt, int l)`

`l` 层（调用时该层父节点已满）向上寻首个可划分层 `fl`，搬右兄弟入 `rt[levels - fl]`，垂空链至 `l` 层。stitch 与 lc_append 共用。

- **找 fl**（自 `fl = l` 向上至 `0`）：
  - 取 `p = parent(fl)`、`i = idx(C, p, fl)`
  - 若 `i < LC_FANOUT - 1`（非末位，右侧有兄弟可搬）→ break
  - 判 `idx < FANOUT-1` 而非 `child_count < FANOUT`：满层中只要 C 不在最末位，则右侧仍有兄弟可划分——无需上溯 rootpush。findroom 调用时 `l` 层父节点恒满（`child_count == FANOUT`），且 C 在末位（`idx >= child_count - 1`），故 `fl == l` 时条件恒假，循环至少下移一层。
- **划分 fl 层**：
  - 若 `fl >= 0` 且 `c = p->child_count - i - 1 > 0`：
    - 计 `db = lcN_sumbytes(p, i + 1, p->child_count)`、`dl` 同理（移出子树度量）
    - `lcM_up(C, fl - 1, -db, -dl)` 祖先扣减
    - `p->child_count = i + 1`（截断父节点）
    - `k = lcK_levels(C) - fl`，`assert(rt[k].child_count == 0)`
    - `lcN_copy(&rt[k], 0, p, i + 1, c)` 搬右兄弟入 rt
    - `rt[k].child_count = c`
- **垂链补齐**：
  - `assert(fl < l)`（参见上段选层条件及末行约束）
  - 返回 `lcB_makechain(C, fl, l)`（`fl < 0` 时 makechain 内自动 rootpush）

**约束**：
- rootpush 全委 `lcB_makechain`
- 祖先更新在 `p->child_count` 截断前调用（`db`/`dl` 基于完整 children 范围求和）
- rt[k] 写入前断言其为空（`child_count == 0`），直接赋值而非累加
- fl 永小于 l：findroom 调用时 `l` 层父满且 C 在末位，故 `idx < FANOUT-1` 在 `fl == l` 时必假，循环下移至少一层。若所有层 C 皆在末位，`fl` 终为 -1，由 makechain 内 rootpush 处理

---

## 9. lcB_append — 末尾追叶

`static int lcB_append(lc_Cursor *C, lc_Scanner sc, void *ud)`

双重循环：外循环逐叶，内循环逐行。先补现存末叶，再分配新叶填满叶父。叶父满返 1（外层调 findroom 或 makechain 扩容），scanner 枯返 0，OOM 返 < 0。

- 设 `pos = lc_offset(C)`（当前绝对字节偏移，含 off+loff+col）
- 记 `p = parent(levels)`、`i = idx(C, p, levels)`（i 指向末叶或末叶后）
- **前置**：`assert(i >= p->child_count - 1)`（C 在末位或末位后）
- 设 `li = LC_LEAF_FANOUT`（确保首轮进入外循环）、`db = 0`、`dl = 0`
- **外循环**（`; i < LC_FANOUT && li == LC_LEAF_FANOUT; ++i`）：
  - `lf = (lc_Leaf *)p->children[i]`
  - `li = p->breaks[i]`（现存行数）
  - 若 `i >= p->child_count`（需新叶）：`lcB_newleaf(S, lf)` 分配，`li = p->bytes[i] = p->breaks[i] = 0`，`p->children[i] = (lc_Node *)lf`
  - **内循环**（`cb = 0, cl = 0; li < LC_LEAF_FANOUT && (br = sc(ud, pos)); ++li`）：
    - `lf->bytes[li] = br`，`pos += br`，`cb += br`，`cl += 1`
  - `db += cb`，`dl += cl`，`p->bytes[i] += cb`，`p->breaks[i] += cl`
  - 若 `i >= p->child_count && li == 0`（本轮分配了新叶但未填入任何行）：
    - `i -= 1`，`lc_poolfree(leaves, lf)` 回收枯叶
  - `C->idx += cl`，`C->off += C->loff + cb`，`C->loff = 0`，`C->lidx = 0`
- `lcM_up(C, levels - 1, (lc_Diff)db, (lc_Diff)dl)` 祖先度量更新
- `p->child_count = i`（更新父节点子计数）
- `C->paths[levels] = &p->children[i]`（指向**下一个待用空位**，i 为循环退出后值）
- 返回 `i == LC_FANOUT && li == LC_LEAF_FANOUT`（父满且末叶满方返 1；仅父满而末叶未满则 scanner 枯于叶间，返 0）

**循环条件**：`li == LC_LEAF_FANOUT`——仅在前一轮叶被填满时才进下一叶。首轮 li 初始化为 `LC_LEAF_FANOUT` 确保必入。

**paths 语义**：返回时 `paths[levels]` 指向 `p->children[i]`——即下一个待分配叶之槽位。`i` 可能等于 `child_count`（在末叶后）或指向新叶槽位（叶满退出时）。此语义使下游 lc_scan 或 lc_append 可直接从该位置继续操作。

**lc_scan 之后撤**：lc_scan 在 append 返回后若 `C.off != 0`（有数据写入），则 `C.paths[levels] -= 1` 回退至末个有数据之叶——因 fix 阶段需要 C 指向有效叶而非空位。

**光标增量**：append 每轮迭代皆统一执行 `C->off += C->loff + cb; C->loff = 0; C->lidx = 0`，无新旧叶之分。轮次间 loff/lidx 恒为零——新叶与旧叶之字节差已于迭代末累入 off。

---

## 10. lcB_stitch — 缝合 rt 回树（边缝合边修复）

`static int lcB_stitch(lc_Cursor *C, lc_Node *rt)`

洋葱序（k=0..levels）将 `rt[k]` 搬回树。**stitch 自身包含整树修复**——以 `lcB_balancerange` 统一进行 fold + rebalance，stitch 结束后 `rt[]` 被清零； C 指向插入内容end位，即指向 `rt[0].children[0]` 被防入树时所在位置。

- 预分配 `level + 1` 个节点保 findroom 不 OOM；若不足 → return `LC_ERRMEM`
- 设 `l = lcK_levels(C)`（追踪需末修之最低层号）、`d = 0`（缝合至某层时向左搬运计数）
- `for (k = 0; k <= lcK_levels(C); ++k)`：
  - 设置 `rkcc = rt[k].child_count`，`rt[k].child_count = 0`
  - 若 `rkcc == 0` 没有要搬的 `rt[k]`，continue
  - `kl = lcK_levels(C) - k`（rt[k] 对应树中层级），`p = parent(C, kl)`，`cc = p->child_count`
  - **叶复用**（`k == 0 && cc && p->bytes[cc - 1] == 0`）如果当前叶是空叶：
    - 末叶槽为空（前次缝合未填满），将 `rt[0].children[0]` 内容直接 memcpy 覆写入该槽
    - 释放 `rt[0].children[0]`，`rt[0].children[0]` 指回旧末叶指针
    - `cc -= 1`，`C->paths[kl] -= 1`（回退至该空槽，变为填入而非追加）
    - 不能直接释放当前叶，因为 `sC` 可能指向该叶（未被 cutleaf 裂出）
  - **修下层**：`m = lc_min(rkcc, LC_FANOUT - cc)`；若 `m < rkcc`（首复制后会溢出）：
    - 若 `k > 0`（即不是叶层）：调用 `lcB_balancerange(C, kl, l, d)` 修复 `[kl, l)` 层级范围内的 underfill 问题
    - `l = kl`、`d = m`（更新下一段末修起点及首复制量）
  - **首复制**：`lcN_copy(p, cc, &rt[k], 0, m)` 搬入，`p->child_count += m`
    - 计 `db`/`dl` 为搬入子树度量之和，`lcM_up(C, kl - 1, db, dl)` 祖先加回
    - 若 `m == rkcc` → continue（全部搬完，无剩余需 findroom）
  - **次复制**（`m < rkcc`，首复制未搬尽）：
    - `lcB_findroom(C, rt, kl)` 扩容（assert 成功，因预分配保 OOM 不现）；findroom 后 `lcK_levels(C)` 可能 +1
    - 重算 `kl = lcK_levels(C) - k`，`p = parent(C, kl)`
    - `lcN_copy(p, p->child_count, &rt[k], m, rkcc - m)` 搬剩余
    - 重算 `db`/`dl`，`lcM_up(C, kl - 1, db, dl)` 祖先加回
    - `p->child_count += rkcc - m`
- 返回 `lcB_balancerange(C, 0, l, d)`（修 0..l-1 层 node + 修 paths + foldleaf + rebalance，完成整树修复）

**洋葱序**：k=0 叶层，k=1 叶父层，...，k=levels 根层。自叶向根逐层缝合。因 findroom 可搬高层右兄弟入 rt，后层（较大 k）在缝合时已有数据。

**防溢出机制**：先以 `m = min(rkcc, FANOUT - cc)` 填满当前父节点，这是为了修复的时候 C 的路径自然是 **待修复** 的路径（即可能 underfill 的），兄弟路径被尽量填满，就可以无须修复。

**paths 更新**：findroom 依赖 `C->paths[sl]` 决定右侧兄弟数。首复制填满父节点后须将 paths 移至末位（`idx = child_count - 1`），则 findroom 不搬数据，走 makechain 建空链。

**游标**：stitch 结束后 C 之 off/loff/lidx 保持 S3 末态不变，仅 paths 更新至各层末位。`lc_append` 的 `lcB_fixremain` 负责修复游标位置以补 rm。

## 11. fold/rebalance — 内联于扫描与缝合

插入流程中需 fold/rebalance 之处有二：

- **lc_scan** 结尾：`for (l = 0; l < lcK_levels(&C); ++l) lcD_foldnode(&C, l); lcD_foldleaf(&C); lcD_rebalance(&C, 0)`
- **lcB_stitch** 尾部委托 `lcB_balancerange(C, 0, l, d)` 统一进行（内含 foldnode + foldleaf + rebalance + paths 修偏移）

fold 约束：仅动 paths，不设 off/idx/loff/lidx。调用者若需完整游标须自行复原位置字段——此由 fixremain 承担。

---

## 12. lcB_fixremain — 补裂点残字节

`static int lcB_fixremain(lc_Cursor *sC, int l, unsigned rm)`

S3 结束后调用，通过 sC（cutleaf 后快照）定位裂点叶，补写 `rm`（裂点行前半残字节）并修复 sC 路径以对齐可能的 rootpush。

- 若 `lcK_parent(sC, l)` 尚有子节点（`idx < child_count`），裂点叶仍存：
  - `k = lcK_levels(sC) - l`（stitch 中 rootpush 所致层级差）
  - `memmove(sp + k + 1, sp + 1, l * sizeof(lc_Node **))`：sC 旧路径 `paths[1..l]` 下移 k 位
  - `for (l = 0; l < k; ++l) sp[l] = &lcK_parent(sC, l)->children[0]`：新层级填全量左侧路径（`idx = 0`）
  - `sp[k] = &lcK_parent(sC, k)->children[i0]`：旧根层 `paths[0]` 的偏移恢复
  - `lcK_leaf(sC)->bytes[sC->lidx] += rm`，`lcM_up(sC, k, rm, 0)`：裂点行前半字节加回，`col→0`
  - 返回 1（rm 已补写）
- 若裂点叶为空（后续 append 未读取到行）（`idx >= child_count`）：返回 0

**返回值**：1 = rm 已补写，需调用方修 `C->loff` 或 `C->off`；0 = 裂点叶不存在，`rm` 本身就是 `C->col` 值，无须特别操作。

**调用方后续**：
- 如果 `lcB_fixremain` 返回 1，即已经将 `C->col` 归入插入前位置：
  - 如果这个位置和结束位置在同一个叶，`C->loff += C->col`（行内偏移加回）
  - 否则，`C->off += C->col`（行外偏移加回）
- `C->col = 0`

若 sC 缝合后路径指向的叶与 C 当前叶相同（未 fold 合并），`C->col`（即 `rm`）加至 `C->loff` 完成行内偏移；若不同（C 已移到他叶），加至 `C->off`。

---

## 13. lcB_balancerange — 折叠层段并修 paths

`static void lcB_balancerange(lc_Cursor *C, int s, int e, int d)`

修复 `[s, e)` 层 node/leaf 可能的 underfill，修 paths 偏移以应对因 fold 所致的子节点位移，末层若为叶则 foldleaf 并重算 `C->loff`。stitch 末调用以整树。

- `for (l = s; l < e; ++l) lcD_foldnode(C, l)` 修复 s..e-1 层 node
- **修 paths[e]**：foldnode 可能收缩合并子节点，C 在 e 层之索引 *i* 须依 `d`（首复制量）调整：
  - 若 `d <= i`（首复制量小，未越界）：`C->paths[e] -= d`
  - 若 `d > i`（首复制量使索引翻入前槽）：`C->paths[e - 1] -= 1`，`C->paths[e] = &p->children[child_count - (d - i + 1)]`
- **修叶**：若 `e == lcK_levels(C)`（末层为叶）：
  - `lcD_foldleaf(C)` 修复叶层 underfill
  - `C->loff = lcL_sumbytes(lcK_leaf(C), 0, C->lidx)` 重算 loff（foldleaf 可能移动叶内行偏移）

**d 之含义**：`d` 为 stitch 缝合到该层时已复制但尚未 fold 的子计数。`balancerange` 通过 `d` 与 `idx(e)` 的关系判断 fold 后应如何调整 paths 避免指向无效槽位。

**调用场景**：
- stitch 内预防溢出时：`lcB_balancerange(C, kl, l, d)` 折叠高层腾空间，`d` 为首复制已搬计数，`l` 为当前最低待修层
- stitch 末尾：`lcB_balancerange(C, 0, l, d)` 折叠 0..l-1 层并整树，使 stitch 后树达到平衡

---

## 14. lcB_rollback — OOM 回滚

`static int lcB_rollback(lc_Cursor *C, lc_Node *rt, lc_Cursor *sC, int sl)`

**裂叶后快照**：`sC = *C` 保存裂叶后完整游标副本，`sl = lcK_levels(C)` 另存层级（因 `lcK_levels` 读 `tree->levels`，`sC` 与 `C` 同指针，根推后玷污不可复读）。

回滚目标：将 C->tree 恢复至裂叶后状态（S2），即 `sC` 所指状态。

- **降根**（`for (l = lcK_levels(C); l > sl; --l)`）：
  - 释放 root.children[1..child_count] 所有子树（`lcN_freerange`）
  - `tree->bytes = root.bytes[0]`、`tree->breaks = root.breaks[0]`（退度量至裂时水平——root.bytes[0] 乃 makechain rootpush 时所设之全树度量，详见 §7）
  - `p = root.children[0]`，`root = *p`（旧根内容拷回 root），`lc_poolfree(nodes, p)`（释包裹节点）
- **复原游标与层级**：`*C = *sC`，`C->tree->levels = sl`
- **合并裂点叶**（若 `rt[0].child_count > 0`）：
  - `memcpy(&lcK_leaf(C)->bytes[C->lidx], rt[0].children[0], rt[0].breaks[0] * sizeof(unsigned))`（右半叶行拷回左半叶）
  - `lcM_up(C, sl, rt[0].bytes[0], rt[0].breaks[0])` 祖先加回
  - `*C->paths[sl] = rt[0].children[0]`（右半叶指针放回树中），`rt[0].children[0] = (lc_Node *)lf`（左半叶移入 rt 待缝合）
- **缝合 rt**（`for (l = 0; l <= sl; ++l)`，k = sl - l 洋葱序）：
  - `p = parent(C, l)`、`i = idx(C, p, l) + (k > 0)`（非叶层索引右移一位）
  - `db/dl = rt[k] 之和 - p.children[i..child_count] 之和`（净差量）
  - `lcN_freerange(S, p, k, i, p->child_count)` 释旧子，`lcN_copy(p, i, &rt[k], 0, rkcc)` 拷入
  - `lcM_up(C, l - 1, db, dl)`，`p->child_count = i + rt[k].child_count`
- 返回 `LC_ERRMEM`（OOM 标识）

**释孤子树**：降根中 `lcN_freerange` 释所有 scanner 填充之新增子树（非裂叶时已有数据）。缝合 rt 中 `lcN_freerange` 释旧位置之子（可能含 scanner 填充数据），以 rt 原内容替之。

---

## 15. lc_scan — 批量扫描建树

`LC_API int lc_scan(lc_Cache *c, lc_Scanner *scanner, void *ud)`

纯树尾追加。append 填叶，makechain 扩容，无独立 fix——fold/rebalance 内联于结尾。

- 参数校验，建临游标 `C`
- `lc_seek(&C, c, c->bytes)` 定位树尾（空树时 off=0、paths[0]=root.children[0]）
- 循环：
  - `r = lcB_append(&C, *scanner, ud)`
  - 若 `r < 0` → 返回 `r`（OOM）
  - 若 `r == 0` → break（scanner 枯）
  - 沿路径向上找首个 `child_count < LC_FANOUT` 层 `l`（自 `levels` 向下至 `0`）
  - 若 `lcB_makechain(&C, l, lcK_levels(&C)) != LC_OK` → 返回 `r`（OOM）
- **paths 回退**：若 `C.off != 0`（append 有写入），`C.paths[lcK_levels(&C)] -= 1` 回退至末个有数据之叶——因 append 返回时 paths 指向空位，而后续 fold 需 C 指向有效叶
- **内联 fold**：`for (l = 0; l < lcK_levels(&C); ++l) lcD_foldnode(&C, l)` 自顶向下 fold node
- `lcD_foldleaf(&C)` fold leaf
- `lcD_rebalance(&C, 0)` 缩根
- 返回 `LC_OK`

> OOM 注意：失败时树中可能有部分 scanner 数据。调用者若需原子性须自行回滚。`lc_scan` 自身不做回滚（无法得知回滚界）。

**与 lc_append 之关系**：lc_append 固定走 cutleaf → append+findroom → stitch + fixremain，与 lc_scan 独立。lc_scan 纯追加至树尾，不涉裂叶与缝合。

---

## 16. lc_append — 主流程

`LC_API int lc_append(lc_Cursor *C, unsigned e, lc_Scanner *scanner, void *ud)`

**S1 — 参数校验与 rt 初始化**：
- 若 `C == NULL || C->tree == NULL || scanner == NULL` → 返回 `LC_ERRPARAM`
- `for (i = 0; i < LC_MAX_LEVEL; ++i) rt[i].child_count = 0`（清空 rt）

**S2 — 裂叶**：
- `lcB_cutleaf(C, rt)`（右半 + 侧叶入 `rt[0]`）；若 OOM → 返回
- `rm = C->col`（存列偏移，因 cutleaf 清零 col；`rm` 非完整行，不可入树）、`sC = *C`、`sl = (int)lcK_levels(C)`（裂后快照，备 OOM 回滚与 rm 补写）

**S3 — 追搬轮替** 循环：
  - `r = lcB_append(C, *scanner, ud)`
  - 若 `r < 0` → `return lcB_rollback(C, rt, &sC, sl)`
  - 若 `r == 0` → break
  - `C->off += C->loff, C->loff = 0, C->lidx = 0`（append 内部已做此操作，此处为二重保障，实为无操作）
  - 若 `r = lcB_findroom(C, rt, (int)lcK_levels(C)) < 0` → `return lcB_rollback(C, rt, &sC, sl)`

**S4 — 缝合**：
- `lcB_stitch(C, rt)`；若 OOM → `return lcB_rollback(C, rt, &sC, sl)`

**S5 — 补 rm/e**：
- `lcB_fixremain(&sC, sl, C->col)` 补裂点叶残字节；若返 1（rm 已补写）：
  - 若 sC 缝合后指向之叶 == C 当前叶：`C->loff += C->col, C->col = 0`（行内偏移修复）
  - 否则：`C->off += C->col, C->col = 0`（层间偏移修复）
- `p = lcK_parent(C, lcK_levels(C))`，`i = lcK_idx(C, p, lcK_levels(C))`
- 若 `i < p->child_count`（游标在有效叶内）：`lcK_leaf(C)->bytes[C->lidx] += e`，`lcM_up(C, lcK_levels(C), (lc_Diff)e, 0)`
- 否则（游标在末叶之外，即 trailing area）：回退至末叶末行，`lcK_leaf(C)->bytes[...] += e` 无意义——`C->col += e` 即存入 trailing area
- 返回 `(C->col += e), LC_OK`

### sC 快照时机

`sC` 于 cutleaf 后存（非前），因 cutleaf 可能 OOM 故先执行之。若 cutleaf 成功而后续失败，回滚用 `sC` 及 `sl`（裂后层级）定位裂点。S5 中 `lcB_fixremain` 用 `sC` 定位裂点叶以补 `rm`。

### rm 与 e

- **`rm`（= C->col）**：S2 中赋值（取裂前列偏移），S5 中通过 `lcB_fixremain(&sC, sl, C->col)` 补写。因 `rm` 非完整行，仅存为 `lc_append` 局部变量，不可写入树中；必待全流程成功后方可补写——若提前入树而后续失败，rollback 无从撤销
- **e**：尾缀字节，S5 中加至当前游标行之 `bytes`。同理须在全流程完成后最后更新。若游标超出末叶（`i >= child_count`），e 直接加入 `C->col`（trailing area）
- `rm` 仅在裂点叶尚存时补写（`lcB_fixremain` 返回 1）——若裂点叶被 fold 合入邻叶，`rm` 已随数据归入，`fixremain` 返 0，无需额外操作

### OOM 回滚

cutleaf 后保存 `sC = *C, sl = lcK_levels(C)`。若 S3 中 append 或 findroom 失败，或 S4 中 stitch 失败，`lcB_rollback(C, rt, &sC, sl)` 降根至裂叶时高度、清除 scanner 填充、合并裂点叶、释右侧孤子树、缝合 rt。树完全恢复至 cutleaf 前状态。

---

## 17. 关键不变式

跨各函数之核心约定：

- **append 后 paths[levels] 指向空叶位**：返回时 `paths[levels]` 指向下一待分配叶槽位（非最后有效叶）。lc_scan 据此若 `C.off != 0` 则后撤一位指回有效叶；lc_append 的 findroom 从此空位起建空链扩容。append 是全局 off/idx/loff/lidx 唯一增量更新者。

- **stitch 中，C->paths 始终在可能underfill的节点上**：所以如果遇到需要 `findroom` 扩充单链树的情况，必定先填满前节点，以保证修复的时候 C->paths 所在的单链树节点是唯一可能 underfill 的节点

- **rm 与 e 须最后更新**：`rm`（裂点行前半残字节）与 e（尾缀字节）皆非完整行，仅存为 `lc_append` 局部变量，不可入树。必待全流程（S2-S4）成功后于 S5 补写——若提前入树而 append/findroom/stitch 后续失败，rollback 无从撤销已写入树之字节，度量将永久错乱。

- **sC 快照定位裂点**：`sC = *C, sl = lcK_levels(C)` 于 cutleaf 后存（非前），用途有二：OOM 回滚时 `lcB_rollback(C, rt, &sC, sl)` 恢复树至裂叶后状态；S5 中通过 sC 定位裂点叶以补 `rm` 字节。

- **rt[0].children[0] 恒为原裂点叶**：cutleaf 将右半叶指针写入此处，stitch 清零 `rt[k].child_count` 但不动 children 数组。fixremain 通过 sC（cutleaf 后快照）而非 rt 判定裂点叶状态——若 `idx < child_count` 则裂点叶仍存，可通过 sC 补 `rm`；若 `idx >= child_count` 则裂点叶已被 fold 合并，`rm` 自然归入。

- **fixremain 以 cutleaf 后快照定位裂点**：`sC = *C` 在 cutleaf 后存，S5 中 `lcB_fixremain(&sC, sl, C->col)` 用 sC 计算层级差 `k = levels - sl`，对齐 paths 后补写 `rm`。返回 1 时调用方据此修 `C->loff` 或 `C->off`。

- **cutleaf 后左半叶旧数据破弃**：裂叶后左半叶 `bytes[C->lidx..]` 仍存于内存，然 `p->breaks[i] = C->lidx` 限界，永不访问。

## 18. 函数清单

| 函数             | 用途                                                     | 约行 |
| ---------------- | -------------------------------------------------------- | ---- |
| lcB_newleaf      | 叶分配宏（OOM 返回）                                     | 4    |
| lcB_newnode      | 节点分配宏（OOM 返回）                                   | 4    |
| lcM_up           | 自 l 层向上更新祖先度量直至根（含零增量守卫）            | 9    |
| lcB_cutleaf      | 裂叶 + 搬右半及右侧叶入 rt[0]                            | 19   |
| lcB_makechain    | 垂空链建空节点（findroom/lc_scan 共用，含 rootpush）     | 20   |
| lcB_findroom     | 从 l 层向上找非末位层，搬右兄弟入 rt，垂空链             | 17   |
| lcB_append       | 末尾追叶（双重循环），返回时 paths 指向空位              | 24   |
| lcB_stitch       | 缝合 rt 回树（洋葱序，边缝合边修复，含 balancerange）    | 33   |
| lcB_balancerange | 折叠 s..e-1 层 node 并修 paths 偏移（内含 foldleaf）     | 22   |
| lcB_fixremain    | 补裂点叶残字节 rm，对齐 sC 路径                          | 12   |
| lcB_rollback     | OOM 回滚（降根+并叶+释孤+缝合 rt）                       | 28   |
| lc_scan          | 批量扫描建树（append + makechain，内联 fold）            | 15   |
| lc_append        | 主流程（cutleaf ⊛ append+findroom + stitch + fixremain） | 28   |
| **总计**         |                                                          | ~240 |

> 行数基于 `linecache.h` 当前版本，含 blank 行。`lcB_stitch`（33 行）、`lcB_rollback`（28 行）接近 30 行硬限。stitch 合缝合+防溢出+balancerange 三项操作，rollback 合降根+并叶+释孤+缝合——拆分违反"不为凑行数而拆"原则。
