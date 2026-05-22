# Linecache Splice 区间删除终极重构计划

## 1. 危机与理论来龙去脉

在实现并测试经典的度量 B+ 树（Metric B+ Tree）进行大规模 Range Delete (`splice` / `clearbreaks`) 时，我们遭遇了深层悬垂下无法治愈的 Underfull 结构。原有的算法及通常粗糙的修改都惨遭失败。

### 为什么一次或两次通过（传统 Bottom-Up 或 直白合并）必然失败？

**反例展现：深层两端保留 `[1..1000] 删除内含至 [1, 1000]`**
- 假设这是一棵4层的树，我们分歧层（Divergence）在第0层（根），并需要保留极左的叶子1和极右的叶子1000。中间的子树全部被直接挖空。
- **纯 Bottom-Up 合并困境**：当你在 Leaf 层（第3层）将 `叶子1` 和 `叶子1000` 两端“相邻”的心智模型合并时，它们合成了一个只有 2 个子项（严重低于 FANOUT/2）的不足叶子。但在传统的 B+ 树自底向上重新平衡算法中，补齐必须要找同父兄弟。由于它们的父亲（第2层的残存节点）因为中间子树全被删除，此刻也只剩下这唯一的单传残脉。既无同父兄弟，如何同侧借位？
- **传统解法死锁**：若放任其 Underfull 留存给上层合并解决，上层（第2层）合并甚至去向上一层借位后，第2层获取了健康完整的子节点，但原本留在第3层的那个只有 2 个项的严重不足子树**已被巡查探过，不再受上层福泽反哺**！这种依靠单次 Bottom-up 的方法注定留下“永远填不平的底坑”。
  
**“底求顶放”的本质与前瞻倒流的破局：**
- 正如研究表明的那样，真正能够解决跨层借位的算法流往往是一个隐形的嵌套：即底层发现无兄弟可依，于是抛出合并需求，递归上层（向父亲要）；若父亲亦不足，父亲向上求爷爷；待上峰求得横向亲戚并把新资源揽入麾下后，再把借来的健康资源灌输分发给下层。这就是**“底层探测求要 → 顶层兼并 → 自上顺流补血”**的本质。
- 然而，我们既已明晰在区间删除中，**从分歧层 `l` 斜切向下的那条边缘线（即左边缘及右边缘）必然全部遭到重创（全部都处于空壳或偏瘦状态）**，那么我们何苦还要任由它们层层往上发起栈式“打报告”呢？我们完全可以采取前瞻性降维打击（Proactive Top-Down）：即从必定富足（或者有途径富足）的分歧父本 `l` 开始，自带着资源往下层“开仓放粮”，这样所有空洞都能在一遍顺下的 `for` 循环中即时获得新兄弟并填满。

## 2. 终极三段法重构框架

我们将原本无序修补的 `lcD_splicerange` 完全收敛到最为清晰的等效理论模型：“底切顶收、上顺分粮、下归落定”。

### 第一段：断支残脉（Prune & Trim）
**目标：截断无用组织，确立空洞左右崖壁与结构骨架**
1. **Divergence (`l`)**：探测游标 C1 和 C2 首次分道的层级。
2. **TrimLeaf**：严格执行末端叶子段的数据消费计算并清空余量（必须要有完善的左剪右并移裁测试）。
3. **TrimNode（关键免转移优化）**：无需发生任何 `memmove` 去挤压对齐！只需简单利用 C1 和 C2 的 paths 和 idx 得知哪些分支属于留存骨架。让移除退化成指针界限的偏移标记，将彻底归拢合并推迟到顺流之时。
4. **ShiftNode（关键免转移优化）**：在 Bottom-Up 阶段尝试真合并（即销毁节点），但绝对禁止平分！如果不满足合并条件，直接放任其残缺状态，允许它们成为悬崖边缘的枯树桩。这样就完全规避了 Cursor Jump 和路径拷贝导致的崩溃问题。
5. **Prune (`l`层剪中段)**：对中间被跨越包含的无辜巨根尽数摧除，这使得 C1 和 C2 变成悬崖边缘，此时它们已成为 `l` 下相邻的同门脉系。

### 第二段：顶流输血（Proactive Top-Down Repair）
**目标：以简单循环规避深递归栈，将资源向下沿崖壁降级补给（只借补，不开花扩高）**
从 `d = l+1` 顺延至 `levels-1` 递降：
- 此时 `C1` 及 `C2` 边缘节点在 `d-1` 层已经与健康兄弟并立（由上级事前调停完毕，绝无单传空巢）。
- `C1` 节点调用专门的辅助方法（如 `lcD_mergenode`），由于其必定能在 `d-1` 内部找到强壮兄弟（向本家的右侧健康方借树，甚至直接吞并），从而使 `C1` 及其底下的路径骤得充足资源。
- `C2` 节点同理（向左侧堂亲借）。
- 这种修补从上逐层进行，当修到 `d+1` 时，`d+1` 会惊异地发现自己原本空空如也，却由于上面的调转而在左右得到了无数饱满堂族。如此推及叶底层，一切边缘枯叶皆受荫借，瞬间肥瘦均衡、合归 B+ 算法正统。
- 到达 `levels` 层后，以 `MergeLeaf` 进行最后的叶内合并（如果满足条件），彻底消除残缺，完成第一轮修复。

### 第三段：归本扶正（Bottom-Up Rebalance & Root Shrink）
**目标：修复宗主（分歧点及以上）的可能欠缺与调整树总高**
- 二段下放必然从宗支（`l` 层左右及 `d-1` 层借出端）抽散了节点数。需要处理这种抽身带来的父节点欠缺问题，如果非常幸运l层只在之前的prune中只删除了一个节点，则直接调用 `rebalance(l)`；如果l层缺少多于一个节点，则需要向上层借位或合并、平分。如果向`l-1` 层借位，那么就满足了 `rebalance(l-1)` 的前提条件（`l-1` 层最多只删一个节点）。如果l层本身满足B+树条件，则直接返回即可。
- 因此最后如果需要，只需以标准的、单点专用的自底向上 `Rebalance`，从 `l` 层向根部检查（向同父堂亲借合并）。
- 如果根节点经过抽身或缩并变得唯余其一，连斩降根直至健康多子，这亦是树高因大区间拔除而合理降阶的必由途径。

## 3. 具体函数行为（依当前代码实况）

### 辅助度量函数

- **`lcK_upmetrics(C, l, db, dl)`**：自 l 层向上至根，逐级累加 `bytes` 与 `breaks` 之增量。
- **`lcK_txmetrics(L, R, lt, rt, db, dl)`**：自 lt 层向上至根，于 L 之各级父加度量；自 rt 层向上至根，于 R 之各级父减度量。专供 shift 用之——数据自 R 迁 L，故增 L 链而减 R 链。
- **`lcN_sumbytes(p, s, e)` / `lcN_sumbreaks(p, s, e)`**：求和父节点 p 中 children[s..e) 之 bytes/breaks。
- **`lcL_sumbytes(leaf, s, e)`**：求和叶中 bytes[s..e)。
- **`lcN_copy(dst, di, src, si, n)`**：自 src 之 children[si..si+n) 拷入 dst 之 children[di..di+n)，并逐子更其父指针。
- **`lcN_move(p, di, si, n)`**：于父 p 内移 children[si..si+n) 至 children[di..di+n)，并逐子更其父指针。

### 3.1 `lcD_prune(lc_Cursor *L, int sr, int l)`

剪除 l 层父子下 L 之 children[i+1..sr) 中间子节点，压实数组，更度量。

- 若 `i+1 >= sr` 则无中段，直接返回。
- 否则求和 bytes/breaks，调 `lcD_freerange` 释中段诸子，`lcN_move` 压实，`child_count -= (sr - i) - 1`。
- 以 `lcK_upmetrics(L, l-1, -db, -dl)` 向上更度量。
- `sr` 为右界索引（不含），由调用者传入。调用者负责维护右游标路径同步。

### 3.2 `lcD_trimleaf(lc_Cursor *C, int left)`

修剪叶级边缘段。不涉物理 memmove，仅更度量。

- **left=0（trimright）**：保留 C 左侧，删除 lidx 右侧诸段。C 在当前叶中，其 lidx 之左为活数据，右侧为待弃。
- **left=1（trimleft）**：保留 C 右侧，删除 lidx 左侧诸段。C 在当前叶中，其 lidx 之右为活数据（含 C.col 之部分消耗），左侧为待弃。
- 以 `lcK_upmetrics(C, levels, -db, -dl)` 向上更新。

### 3.3 `lcD_trimnode(lc_Cursor *C, int l, int left)`

修剪 l 层边缘子节点。更 child_count 界标，释死节点，更度量。

- **left=0**：C 在节点 p 之 children[i]，保留 children[0..i]，释 children[i+1..child_count)。`p->child_count = i+1`。
- **left=1**：保留 children[i..child_count)，释 children[0..i-1]。`p->child_count = child_count - i`。
- 调 `lcD_freerange` 释死节点，`lcK_upmetrics(C, l-1, -db, -dl)` 更祖父度量。
- 不压实数组——死节点仅由 child_count 逻辑屏蔽，待后续 prune 或上层操作自然压实。

### 3.4 `lcD_shiftleaf(lc_Cursor *L, lc_Cursor *R, int l)`

纯 shift：若 L 叶段数 + R 叶段数 ≤ LEAF_FANOUT，将 R 叶数据拷入 L 叶，转移度量，R 游标前移。

- 前置：`cl + cr <= LC_LEAF_FANOUT`，否则直接返回0，不作任何操作。
- `memcpy(&ll->bytes[cl], &lr->bytes[R->lidx], cr * sizeof(unsigned))`：R 叶自其 lidx 起拷入 L 叶末。
- `lcK_txmetrics(L, R, l, l, pr->bytes[ir], cr)`：度量自 R 链转 L 链。
- `R->paths[l] += 1`：R 游标前移，指向前兄弟（即原 R 节点之左邻），返回1。
- **不释死叶，不压实父数组**——此二者由上层 trimnode 或 prune 负责。

### 3.5 `lcD_foldleaf(lc_Cursor *C, int l)`

自寻合并目标之叶级 shift：若 C 之叶不足半，先探右兄弟，`cl+cr <= LEAF_FANOUT` 则将右叶数据拷入 C 叶，调 `lcD_prune` 清死叶。右兄弟不可行则探左兄弟，同法。左右皆不成（cl+cr > LEAF_FANOUT）则返回 0。

- 专用于 `spliceleaf`：删后叶不足，foldleaf 尝试将残叶并入左右兄弟。
- 成功则父失一子，需调 `lcD_rebalance` 修父层。
- 不成则说明 cl+cr > LEAF_FANOUT，应走 `lcD_mergeleaf` 均分。

### 3.6 `lcD_mergeleaf(lc_Cursor *C, int sr)`

均分叶数据。若 C 为末叶且已足半，直接返回。否则与右兄弟（由 sr 定其数据偏移）均分：

- **回退**：若 `cl+cr <= LEAF_FANOUT`，调 `lcD_foldleaf` 代之（foldleaf 自寻左或右兄弟 shift）。foldleaf 成功则返回，不成功则说明 cl+cr > LEAF_FANOUT 之条件实不成立（不会发生）。
- 若 cl==mid，仅 `memmove` 右叶数据；否则调 `lcD_balanceleaf` 双向迁移。
- 更父度量 `breaks[i]`、`bytes[i]` 及 `breaks[i+1]`、`bytes[i+1]`。
- 若数据迁出致 C 之叶变，修正 `C->lidx` 或 `C->paths[levels]`。
- **不改父 child_count**——纯数据迁移。

### 3.7 `lcD_shiftnode(lc_Cursor *L, lc_Cursor *R, int l)`

纯 shift：若 L 之 l+1 层子节点数 + R 之 l+1 层子节点数 ≤ FANOUT，将 R 之子拷入 L 之子，转移度量，R 游标前移。

- 前置：`cl + cr <= LC_FANOUT`，否则直接返回0，不作任何操作。
- `lcN_copy(nl, il+1, nr, ir, cr)`：R 之 children 拷入 L 之 children 末。
- `lcK_txmetrics(L, R, l, l, p->bytes[i], p->breaks[i])`：度量转移。
- `nl->child_count += cr, nr->child_count -= cr, R->paths[l] += 1`，返回1。
- **不释死节点，不压实父数组**。

### 3.8 `lcD_foldnode(lc_Cursor *C, int l)`

自寻合并目标之内节点级 shift：若 C 在 l 层之子不足半，先探右兄弟（同父下），`cl+cr <= FANOUT` 则将右兄弟之子拷入，调 `lcD_prune` 清死节点。右不成则探左兄弟，同法。左右皆不成（cl+cr > FANOUT）则返回 0。

- 专用于 `lcD_rebalance` 及 `mergenode` 回退。
- 成功则父失一子，rebalance 继续向上修。
- 不成则说明 cl+cr > FANOUT，应走 `lcD_mergenode` 均分。

### 3.9 `lcD_mergenode(lc_Cursor *C, int sr, int l)`

均分子节点。若 C 为末子且「充足」，直接返回。否则与右兄弟（由 sr 定其子偏移）均分：

- **回退**：若 `cl+cr <= FANOUT`，调 `lcD_foldnode` 代之（foldnode 自寻左或右兄弟 shift）。foldnode 成功则返回，不成功则说明 cl+cr > FANOUT 之条件实不成立（不会发生）。
- 若 cl==mid，仅 `lcN_move` 迁移；否则调 `lcD_balancenode` 双向迁移。
- 更父度量 `bytes`、`breaks`。
- 若数据迁出致 C 之节点变，修正 `C->paths[l+1]` 或 `C->paths[l]`。

### 3.10 `lcD_rebalance(lc_Cursor *C, int l)`

新 rebalance 仅理内节点，自 l 层向上循环——
- 足半则退，不足则 fold 兄弟（若 cl+cr≤FANOUT）
- fold 后调用 prune 清死节点致父失一子续修
- 循环毕缩根

### 3.11 `lcD_spliceleaf(lc_Cursor *C, size_t del)`

单叶内删除 del 字节。若删除跨越多段，memmove 压实叶内 bytes 数组，更度量。

- 若 `del < 当前段剩余字节`：仅减当前段 bytes，更度量，返回。
- 否则跨段删除：逐段消耗 del，memmove 压实剩余段，`lcK_upmetrics` 更度量。
- 删毕，若叶不足半（`breaks[li] < LEAF_FANOUT/2`）：
  - 先调 `lcD_foldleaf` 尝试 shift 左右兄弟。
  - foldleaf 若成功（合并入兄弟），调 `lcD_rebalance` 修父层。
  - foldleaf 若不成（说明 cl+cr > LEAF_FANOUT 不可 shift），调 `lcD_mergeleaf` 均分。

### 3.12 `lcD_splicerange(lc_Cursor *L, lc_Cursor *R)`

三段法区间删除之主调度器。

**第一段**：求分歧层 l。修剪叶缘（trimleaf L 右、R 左）→ shift 叶缘。自 levels 向下至 l+1：修剪节点缘（trimnode L 右、R 左）→ shift 节点缘。prune 删 l 层中段诸子。**若 l==0 且 prune 后根仅一子（非叶），以该子为新根，释旧根，树高降一。**

**第二段**：若 R 仍在父内（prune 后 L、R 于 l 层为相邻兄弟），自 l+1 向下至 levels-1：
- 调 `lcD_mergenode` 均分节点（mergenode 内部若 cl+cr<=FANOUT 则回退 foldnode）。
- 调 `lcD_mergeleaf` 均分叶数据（mergeleaf 内部若 cl+cr<=LEAF_FANOUT 则回退 foldleaf）。

**第三段**：据 prune 删去之子数分情形修父层：
- 删 1 子：`lcD_rebalance(L, l)`（若 l==levels 改 l-1）。
- 删 0 子或 l<=1：`lcD_rebalance(L, 0)`。
- 删多子：先 `lcD_foldnode(L, l-1)`；若不成，`lcD_mergenode(L, 0, l-1)`。后续由 rebalance 向上修。

### 3.13 `lc_splice(lc_Cursor *C, size_t del, size_t ins)`

公开 API。参数校验 → 删除（同叶走 spliceleaf，跨叶走 splicerange）→ 空树重置 → 插入。

- **Bug**：空树重置（step 2）在删除操作之后，应先判删空以短路。

## 4. 验证策略

以下场景须有测试覆盖，入 `linecache_test.c`，以 `just debug` 运行：

1. **深谷悬切** `[1..1000] -> [1, 1000]`：4 层树，极左极右各留 1 项。验 Phase 2 自 l 层逐级下放资源，终叶满足最少填充，树结构合法。
2. **叶 shift-many-to-one**：左叶 2 项 + 右叶 3 项 ≤ LEAF_FANOUT。验 foldleaf（于 mergeleaf 回退）正确 shift 合并。若合并后仍不足半，验 rebalance 向上修。
3. **叶均分**：左叶 4 项 + 右叶 7 项 > LEAF_FANOUT。验 mergeleaf 正确均分 5/6。
4. **内节点 fold**：边缘父 2 子 + 兄弟 3 子 ≤ FANOUT。验 foldnode（于 mergenode 回退）正确 shift。
5. **内节点均分**：边缘父 4 子 + 兄弟 7 子 > FANOUT。验 mergenode 正确均分。
6. **l=0 缩根**：根层分歧，prune 后根仅一子。验 Phase 1 末正确缩根。
7. **删至全空**：删所有数据。验树正确归空（root 清零，levels=0）。
8. **多层跨越**：4 层树删中间大部，验 Phase 2 于 l+1、l+2 层 fold/merge 后全树合法（各层节点数、叶数据数均在 [FANOUT/2, FANOUT] 范围内）。