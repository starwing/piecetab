# design_stitch_r2 — linecache / spantree / piecetab remove / stitch 精确同步事实（r2）

> 本文件只描述当前实现事实，按“共同精确文本骨架 → 各库领域差异 → 为什么不同”组织。
> 函数名不带库前缀；三库同名函数视为同一算法节点，库差异用 `[L]` / `[S]` / `[P]` 标出。
> 本文件描述当前 remove / stitch 的精确文本同步事实。

---

## 0. 研究基准与阅读约定

### 0.1 代码基准

- 基准：本文件所在提交下的三份头文件——`linecache.h` 1193 行、`spantree.h` 1243 行、`piecetab.h` 1478 行。
- 本文所有行号以该状态为准；行号漂移时先按函数名定位，再用本文给出的函数体语义核对。
- 三库的公共 API 实现集中在文件末尾的 `/* APIs */` 段，内部原语按 `/* utils */`、`/* navigation */`、`/* remove */`、`/* insertion */`、`/* scan */` 等段排列；本文的章节顺序与之一致。

### 0.2 参考文档与数据模型

本文件同时参考：

- `notes/design_linecache.md`（linecache 语义总览）
- `notes/design_piecetab_v2.md`（piecetab 设计意图）
- `notes/design_spantree.md`（spantree 当前设计）

当前 `piecetab.h:209-221` 的数据模型：`pt_Node` 使用单字 `pt_Mask mask`、`pt_Ver version`；`pt_Hole` 使用 `char data[PT_MAX_HOLESIZE]`，宏 `PT_MAX_HOLESIZE=64`。`notes/design_piecetab_v2.md` 同步描述这些字段。

### 0.3 标记与分类

- `[L]` = `linecache.h`
- `[S]` = `spantree.h`
- `[P]` = `piecetab.h`

每个函数 / 分支按下列类别标注：

1. **exact-sync**：把库前缀、类型名、宏名统一后，语句顺序、表达式、控制流完全相同，可直接中文算法描述。
2. **skeleton**：控制流同形，但字段数、度量通道、回调、结构原语不同；可提取骨架并按领域通道补全。
3. **domain**：由数据模型、层次结构或功能承诺决定，无法与另一库共享同一文本逻辑；必须写清领域语义与根本原因。
4. **optional**：只在某个库 / 某功能路径存在；需说明 remove 路径是否可达，以及同步时应保留还是删除。
5. **interface-divergence**：纯接口形状、参数名、参数个数、返回值、helper 拆分；用于区分接口差异与算法差异。

---

## 1. 数据结构：共同骨架与各库差异

### 1.1 三库共同骨架

三库都是“嵌入式根 + 计量 B+ 树 + 无 parent 指针 + Cursor 保存 root-to-leaf 路径槽指针”的结构。共同 field 只列一次：

- `Node` 共同 field：`children[FANOUT]`、`bytes[FANOUT]`、`child_count`。
- `Tree` / `Cache` 共同 field：嵌入 `root`、`levels`、`bytes`。
- `Cursor` 共同 field：`paths[MAX_LEVEL]`、`tree`、`poff`、`off`、`epoch`。
- `Tree` / `Cache` 共同 field 追加 `epoch`：每次成功的原地修改 API 递增一次；`valid(C)` = `C && C->tree && C->epoch == C->tree->epoch`，解引用 `paths` 的入口先过 `valid`，过期游标返回 `ERRPARAM`（`linelen` / `next` / `prev` / `style` 一类返回 `0` / `NONE`）。
- `State` 共同 field：`nodes` pool、`Node rt[MAX_LEVEL]` scratch。
- 叶层槽的共同语义：`[L]` 是 `lc_Leaf *`，`[S]` 是 `sp_Id`，`[P]` 是 literal / `pt_Hole *`；槽位都通过 `paths[levels]` 访问。
- 共同声明位置：`[L]` `linecache.h:108-122`、`157-175`；`[S]` `spantree.h:115-121`、`172-187`；`[P]` `piecetab.h:130-137`、`208-242`。

共同宏与语义：

- `K_levels(C)` = `C->tree->levels`。
- `K_parent(C, l)` = `l > 0 ? *C->paths[l - 1] : &C->tree->root`。
- `K_idx(C, p, l)` = `C->paths[l] - p->children`。
- `N_cc(p)` = `p->child_count`；`N_setcc(p, n)` 写回 `child_count`。
- `paths[l] == &parent(l)->children[K_idx(C, parent(l), l)]` 是全程不变量。
- `levels == 0` 时 `root.children[i]` 直接是叶槽；`levels > 0` 时 `paths[levels]` 是叶槽。
- `bytes[i]` 聚合：叶层是叶数据长度，内部层是子树所有叶数据长度之和；根槽之和必须等于 `tree->bytes`。
- 树中间叶槽长度 > 0；切点处的 0 长槽由 `cutrange` 的 `i += (k || 0长)` 随切点删去。
- mutation 必须在函数内部维护 `paths[]`、`child_count`、祖先度量、树总度量；定位原语只用于入口 / 导航。
- `rt[]` 是“被切下的各层 children”的 scratch：`rt[k]` 装第 `levels-k` 层内容，`rt[0]` 是叶层；`stitchnode` 消费后逐个 `setcc(rt[k], 0)`。

### 1.2 各库不同的数据结构

#### [L] linecache.h

以下列各库额外 field；共同 field 见 §1.1。声明位置：`linecache.h:157-175`、`108-122`。

- `lc_Leaf`：独立叶类型，唯一 field 是 `unsigned bytes[LC_LEAF_FANOUT]`；有效行数由父槽 `breaks[i]` 记录。
- `lc_Node` 额外 field：`breaks[LC_FANOUT]`（在共同 `children`、`bytes`、`child_count` 之外）。
- `lc_Cache` 额外 field：`breaks`（总行数）；共同 `root`、`levels`、`bytes` 不重复。
- `lc_Cursor` 额外 field：`col`、`lnu`、`loff`、`nu`；共同 `paths`、`tree`、`off`、`epoch` 不重复。

- `LC_FANOUT=16`、`LC_LEAF_FANOUT=34`、`LC_MAX_LEVEL=21`（`linecache.h:139-145`、`104-106`）。
- 叶是独立类型 `lc_Leaf`；有效行数由父节点同槽的 `breaks[i]` 记录。
- 绝对行号 `nu + lnu`；绝对字节 `off + loff + col`（`linecache.h:86-89`）。
- 叶槽 `paths[levels]` 里存 `lc_Leaf *`，读叶必须 `lcK_leaf(C)` 解引用（`linecache.h:257`）。
- `lcM_up(C, l, db, dl)` 同时维护 `bytes` 与 `breaks` 两条度量（`linecache.h:279-287`）。
- 虚拟尾：`lnu == breaks[i]` 时当前行耗尽，`col` 表示尾后残段长度；`lc_linelen` 在该态直接返回 `col`（`linecache.h:1085-1091`）。

**为什么 remove 要为 [L] 额外维护行语义**：linecache 的承诺是“按行/列导航”。叶内是行长数组，父 `breaks[i]` 是行数，cursor 是“叶号 + 叶内行号 + 行内列偏移”的行索引型游标。任何 remove 都必须同时更新 `bytes` 和 `breaks`；跨行删除还要维护“行内前缀”和“尾后虚拟区”。

#### [S] spantree.h

以下列各库额外 field；共同 field 见 §1.1。声明位置：`spantree.h:172-187`、`115-121`。

- `sp_Node` 额外 field：`mask[SP_FANOUT]`（叶 ns 集或子树 OR）；共同 `children`、`bytes`、`child_count` 不重复。
- `sp_Tree` 额外 field：`arb`、`aud`；共同 `root`、`levels`、`bytes` 不重复。
- `sp_Cursor` 使用共同 field；叶槽存 `sp_Id`，通过 `spL_id` / `spL_setid` 与指针槽互转。
- 类型别名：`sp_Id = size_t`、`sp_Mask = size_t`。

- `SP_FANOUT=34`、`SP_MAX_LEVEL=16`（`spantree.h:142-143`、`111-113`）。
- 叶层与内部层使用同一 `sp_Node` 类型；叶槽 `children[i]` 存的是 `sp_Id`，通过 `spL_id` / `spL_setid` 与指针互转（`spantree.h:257-258`）。
- `bytes[i]` 叶层是 span 长度，内部层是子树累计；`mask[i]` 叶层是该 span 的 ns 集合，内部层是子树的 OR（`spM_sumns`、`spM_remask`，`spantree.h:272-276`）。
- cursor 只有 `poff/off`，绝对偏移 `off + poff`（`spantree.h:82`）。
- 虚拟尾：`poff` 可以大于等于当前 span 长；`sp_seek` / `sp_locate` / `sp_advance` 会保留越尾量（`spantree.h:353-435`）。remove 入口在 `sp_remove` 判 no-op 并 reserve（`spantree.h:1223-1229`），`spD_remove` 把 `len` clamp 到树内（`spantree.h:684-695`）。

**为什么 remove 要为 [S] 维护 mask 与 arbiter**：spantree 存不透明的结果 id，id 代表最终渲染结果或混合列表句柄；树必须通知外部 id 的 born/died/merge/touch 事件。叶内 ns 集合并为 mask，内部 mask 是子树 OR，导航 `sp_next/sp_prev` 靠它剪枝。因此 remove 需要同时维护字节与 mask，并调用 `spA_*` 回调。

#### [P] piecetab.h

以下列各库额外 field；共同 field 见 §1.1。声明位置：`piecetab.h:209-269`、`131-141`。

- `pt_Hole`：`char data[PT_MAX_HOLESIZE]`；长度以父槽 `bytes[i]` 为准。
- `pt_Node` 额外 field：`mask`（单字位图：叶 hole 位 / 内部含 hole 位）、`version`（COW 版本）；共同 `children`、`bytes`、`child_count` 不重复。
- `pt_Tree` 额外 field：`from`、`arena`、`refc`；共同 `root`、`levels`、`bytes` 不重复。
- `pt_Cursor` 额外 field：`dirty`；共同 `paths`、`tree`、`poff`、`off` 不重复。
- 类型别名：`pt_Mask`（`PT_FANOUT < 32` 时 `unsigned`，否则 `unsigned long long`）、`pt_Ver = unsigned`。

- `PT_FANOUT=31`、`PT_MAX_LEVEL=17`、`PT_MAX_HOLESIZE=64`（`piecetab.h:165-177`、`128-129`）。
- 叶层与内部层使用同一 `pt_Node` 类型；叶槽 `children[i]` 是 literal 指针或 `pt_Hole *`，由 `mask` 第 i 位区分（`ptM_ishole`，`piecetab.h:386-387`）。
- `mask` 是单字位图（`PT_FANOUT <= PT_MASK_BITS`，`piecetab.h:204-206`）；内部层第 i 位表示子树含 hole。收缩 `child_count` 后必须 `ptM_clamp`（`piecetab.h:393`、`455-458`）。
- `version` 与 `ptK_cow`：修改共享节点前必须 path-copy。入口（`pt_remove` / `pt_edit` / `pt_splice` / `pt_append`）先 `ptK_reserve` 预留下界，再由段机器里的 `ptK_markdirty` fork transient tree、沿共享路径 `ptK_cow`（`/* navigation */` 段 `ptK_*`、`/* remove */` 段 `ptD_remove`）。
- 越尾处理：`pt_seek` / `pt_locate` / `pt_advance` 直接 `ptK_locend`，超出量归零；删段入口 `ptD_remove` 把 `len` clamp 到 `bytes - offset`。

**为什么 remove 要为 [P] 维护 COW / hole / literal**：piecetab 是带编辑态 hole 的字节 piece 表；literal 不可变且可能来自外部 buffer，hole 是可原地改写的数据。remove 需要：对共享节点 path-copy；对 hole 用 `memmove` 缩数据；对 literal 则切指针/长度，必要时分裂 piece；合并叶内相邻且物理连续的 literal；并且 mask 位必须与 hole 状态同步。

### 1.3 数据结构对照表

| 维度 | [L] linecache | [S] spantree | [P] piecetab |
|---|---|---|---|
| 叶类型 | 独立 `lc_Leaf` | 无，统一 `sp_Node` | 无，统一 `pt_Node` |
| 叶槽内容 | `unsigned` 行长数组 | `sp_Id`（指针槽 cast） | literal 指针 / `pt_Hole *` |
| 叶有效计数 | 父 `breaks[i]` | 叶自己的 `child_count` | 叶自己的 `child_count` |
| 行 / breaks | `breaks[]` + `tree->breaks` | — | 无 |
| 附加通道 | — | `mask[]` + arbiter | `mask` 位图 + version/COW/arena |
| cursor 偏移 | `off + loff + col` | `off + poff` | `off + poff` |
| cursor 行号 | `nu + lnu` | — | 无 |
| `paths[levels]` 槽里 | `lc_Leaf *` | `sp_Id`（cast） | literal / `pt_Hole *` |
| 扇出 | `LC_FANOUT=16`；叶 `LC_LEAF_FANOUT=34` | `SP_FANOUT=34` | `PT_FANOUT=31` |
| `levels==0` 含义 | root children 就是叶 | 同 | 同 |
| 总度量 | `bytes` + `breaks` | `bytes` + mask 聚合 | `bytes` + mask 聚合 |
| 虚拟尾 | 支持；`col` 存残段 | 支持；`poff` 可越尾 | 越尾直接 `locend` |
| remove 入口 | `remove(C,len)` 长度式 | `remove(C,len)` 长度式 | `remove(C,len)` 长度式 |
| cursor 失效 | `epoch` + `valid(C)` | 同 | 同 |

---

## 2. remove / stitch 总流程（中文步骤 + 库差异）

### 2.1 入口与分派

1. 参数与范围校验
   - 三库同为 `remove(C, len)`：先过 `valid(C)`，过期游标返回 `ERRPARAM`；`len == 0` 或 `offset(C) >= bytes` 直接 OK（no-op，不 reserve、不递增 epoch）。
   - `[L]` / `[S]` 在 `remove` 路径内部把 `len` clamp 到 `bytes - offset(C)`（`[L]` `lcD_remove`、`[S]` `spD_remove`）；`[P]` 在 `pt_remove` 入口 clamp。
2. 构造右端
   - `[L]` `R = *C; lc_advance(&R, len)`；`R` 越尾时保留虚拟尾，表示删到树尾且 `C` 所在当前行整体删除。
   - `[S]` `R = *C; sp_advance(&R, len)`；若 `R` 越尾，用 `spK_locend(&R)` 落到树尾态。
   - `[P]` `R = *C; pt_advance(&R, len)`；越尾会直接 `ptK_locend`（无虚拟尾）。
3. reserve / 事务
   - `[L]` `remove` 不预留：删段原地修改，不分配 node。
   - `[S]` `sp_remove` 不预留：`spD_remove` 的唯一分配源是跨段 `makechain`，remove 路径由 `cutrange` 保证不进入扩根分支。
   - `[P]` `pt_remove` 在入口预留 `2 * levels + 2` 个 node；失败返回 `PT_ERRMEM`，树和 cursor 保持调用前状态。预留覆盖删段里的 path-copy（`ptK_cow`）与 `foldnode` 的邻居 COW。
4. 同容器判定
   - `[L]` / `[S]` 比较 `paths[]` 求最小差异层 `l`（`[L]` 在 `remove` 路径内联、`[S]` 用 `spD_diverlevel`）；`l <= levels` 走跨容器 `range`，`l > levels` 走同容器 `leaf`。
   - `[P]` `ptD_remove` 内先 `ptK_markdirty` + 沿共享路径 COW，再比较 `paths[]` 得到 `l`；`l <= levels` 走跨 piece `range`，否则同 piece `leaf`。

**为什么入口不同**：三库都是游标 + 长度式 `remove(C, len)`；`[P]` 还要在结构修改前 fork transient tree，所以 COW / markdirty 属于入口层差异。

### 2.2 同容器路径

共同形状：删除范围完全落在同一个叶 / span / piece 内。

- `[L]` `leaf(C, del)`：区分行内删除、跨行删除、删到叶尾；行内只缩短 `leaf->bytes[lnu]`；跨行要 `memmove` 行数组、把前缀 `col` 与残余行拼成一行；删到叶尾时前缀留在 `C->col` 虚拟区；出口按需 `foldleaf` + `rebalance`，并重算 `loff`。
- `[S]` `leaf(C, del)`：部分删用 `spA_touch(C,p,i,-del,0)` 缩短当前 span；整段删用 `spN_remove` 摘槽，再用 `spK_seamleaf` 合并相邻同 id span；出口修正 cursor，必要时 `spD_rebalance`。
- `[P]` `leaf(C, del)`：hole 直接 `ptH_remove`；literal 整段删或首尾切；中间删要先把 literal 分裂成左右两个 literal；为分裂新槽会向上找一层空位，必要时 `ptI_splitroot`/`ptI_splitchild`；出口 `ptD_rebalance`，并用 `ptK_forwardoff(C,0)` 把停在 piece 尾的 cursor 推到下一 piece（树尾除外）。

### 2.3 跨容器路径

1. 保存删除起点前缀
   - `[L]` `rm = L->col`，最后要 `L->col = 0; addbytes(L, rm)` 恢复。
   - 行内前缀只属于 `[L]`。
2. `trimright(L)`：把 L 所在叶 / span / piece 逻辑上截到删除起点。
   - `[L]` 按行粒度截到当前行之前，扣 `db` bytes 与 `dl` lines，不改 leaf 数组。
   - `[S]` 把当前 span 缩短到 `L->poff`，走 `spA_touch` 通知 arbiter 并传播 bytes。
   - `[P]` 只把当前 piece 度量扣到 `L->poff`（`ptM_up`）；hole/literal 数据先不动。
3. `trimleft(R)`：把 R 所在叶 / span / piece 截到删除终点之后。
   - `[L]` 物理移动 R 叶行数组，丢掉当前行之前的部分，并把当前行首 `R->col` 字节扣掉（拼到最终首行时用）。
   - `[S]` 把 R 当前 span 缩短到 `R->poff`，`R->poff = 0`。
   - `[P]` hole 用 `ptH_remove`；literal 把数据指针前移 `R->poff` 并缩短 `bytes[i]`；最后 `ptM_up` 扣度量并 `R->poff = 0`。
4. `cutrange(L,R,fl)`：经 `S->rt` 取 scratch root，从叶层向上逐层“切下 R 右侧、删除 L 右侧”：
   - 对 `kl = levels .. fl+1`：
     1. L 侧：当前 L child 槽先扣掉下层已删的 `db`（`[L]` 还有 `dl`）；把 L 右侧 sibling 的 bytes（`[L]` 还有 breaks）累加进 `db`；`N_remove` 摘掉 `[Lindex+1, cc)`。
     2. R 侧：取 R 所在槽；`i += (k || 该槽为 0 长)`；把 `[i, cc)` 复制进 `rt[k]`；把 `[0, i)` purge 并 `setcc(p,0)`。
   - 在 `fl` 层：复制 R 右侧 tail 到 `rt[k]`；共同父 `setcc(p, R起点)`；再扣 L child、累加 L 右侧、`M_up(L, fl-1, -db)`，最后 `N_remove` 摘除 `[Lindex+1, cc)`。
   - `k = levels - kl`，所以 `rt[0]` 是叶层，`rt[k]` 是第 `levels-k` 层。
5. `stitch(L)`：`mergeleaf` 尝试把 `rt[0]` 并进当前最后叶；`stitchnode` 逐层把 `rt[k]` graft 回树；必要时 `findroom` 建链 / 扩根；`foldnode` / `rebalance` 修 underfill；最后修正 cursor。
6. 恢复起点前缀
   - `[L]` `L->col = 0; addbytes(L, rm)`：把删除起点行内前缀重新加到 stitched 第一行。
   - `[S]` / `[P]` 无此步。

### 2.4 `stitch` 子流程

1. 入口 assert cursor 位于容器末段（`idx >= cc - 1`，三库同形）；节点预算由各入口的 reserve 保证。`[S]`/`[P]` 再先删末尾 0 长槽，`[L]` 的末尾 0 长叶由 `mergeleaf` 的 fill 逻辑覆盖。
2. `mergeleaf`：把 `rt[0]` 与当前最后叶 / span / piece 尝试合并；返回 / 不返回 `d` 取决于叶模型。
3. `stitchnode`：从 `k = 0` 到 `levels`，每层把 `rt[k]` 能放下的 children 追加到当前父；放不下时 `foldnode` + `findroom` + 重新挂剩余 children；同时维护 cursor `paths` 和 `d`。
4. `rebalance` / `foldleaf`：修复 remove 后 underfull 的节点 / 叶；`[L]` 因叶独立，需要先 `foldleaf` 再 `rebalance`；`[S]`/`[P]` 叶与内部节点同型，`rebalance` 从叶容器层起。三库的 `rebalance` 同形（内部从 `levels - 1` 起），缩根都抽成 `xxD_shrinkroot`，由 `rebalance` 末尾与 `stitchnode` 的两处时机调用（§3.11）。
5. cursor 收尾：
   - `[L]` 用 mergeleaf 返回的 `d` 修正 `lnu`，必要时 `backwardnode` 退一个叶，再由行数组重算 `loff`。
   - `[S]` 若 cursor 落在 `children[cc]`（one-past），把它左移到最后一个 child，并同步 `off`/`poff`；最后 `M_up(levels,0)` 纯 mask 传播。
   - `[P]` 同 `[S]` 的 `i == cc` 修正；若 mergeleaf 返回 `d > poff`，先 `backwardnode` 退一个 piece，再修正 `poff`/`off`；最后 `ptM_up(levels,0)` 纯 mask 传播。

### 2.5 reserve / 事务时机

`L` = 入口处的 `levels`。当前各站点的节点预留量：

| 站点 | `[L]` linecache | `[S]` spantree | `[P]` piecetab |
|---|---|---|---|
| `remove` | 0 | 0 | `2*L + 2` |
| `append` / `insert` | `L + 2`（`lc_append` 在 `stitch` 前预留一次） | `L + 2` | `2*L + 2` |
| `splice` | 0（无节点分配） | `L + 2`（仅 `ins > 0`） | `3*L + 3` |
| `edit` | — | — | `3*L + 3` |
| `commit` / 每轮 freeze | — | — | `1` |
| `makechain` / `checkchain`（`nofail == 0`） | `to - from + (from < 0)` | 纯分配 | `to - from + (from < 0)` |

- `[L]` / `[S]` 的 `remove` 与 `[L]` 的 `splice` 不预留：删段的**峰值缺口是 0** —— `cutrange` / `N_remove` 先把切掉的 children 回池，`stitchnode` 的 `makechain` 才分配，回收物即预算；`cutrange` 的截断同时保证 `findroom` 停在 `fl >= 0`，不进入扩根分支。见证是「两池抽干 + 页分配关死 + 零预算」跑通（`[L]` `remove_need` / `splice_need`，`[S]` `remove_need` / `splice_need`）。
- `[P]` 的 `remove` 需要预算：入口的 path-copy（`ptK_cow`）与 `foldnode` 的邻居 COW 都要新节点，而删段自身的 `makechain` 只经 `ptD_findroom`（`assert(fl >= 0)`）调用，不扩根。
- `[P]` 的 holes 需求由各入口单独预留：`append` 用内联 `inhole`（游标严格落在 hole 片内部 = 1），`edit` 用 `len > 0 ? 2 : 0`，`splice` 用 `len ? 1 : 0`。
- `[P]` 的入口独占整段预算：`ptD_remove` / `ptI_append` 自身不预留，因此入口的预留量就是一次调用的上界；`ptK_beginedit`（markdirty + 整条路径 COW）在预留之后才执行，保证失败时树与 cursor 不变。
- `[L]` 的批量 append 无法在入口一次算出总量，改由 `lcD_checkchain` 逐段预留（`lcD_findroom` 内调用）；`[S]` / `[P]` 的 `makechain` 同理：`[S]` 用入口 reserve 契约（纯分配、无 `nofail`），`[P]` 用 `nofail` 参数区分「调用方已预留」与「自己预留」。

---

## 3. 逐函数算法流程与库差异

> 约定：函数名不带库前缀。写“共同流程”时不重复库名；差异在下面用 `[L]` / `[S]` / `[P]` 标出。
> `M_up` 表示度量向上传播；`N_*` 表示节点原语；`K_*` 表示 cursor 宏。

### 3.1 `remove`

**共同流程**

1. 校验参数与同树。
2. 规范化删除范围：clamp 到树尾；决定是否 no-op。
3. reserve：失败必须在任何结构修改前返回。
4. 构造 / 接收右端。
5. 求第一差异层 `fl` 或同容器标记。
6. `fl` 在树内层 → 跨容器 `range`；否则 → 同容器 `leaf`。

**差异**

- `[L]` 游标 + 长度；入口条件 `len == 0 || offset(C) >= bytes` 为 no-op；不 reserve；`R` 由 `lc_advance` 构造且可落在虚拟尾（删到树尾，`C` 所在当前行整体删除）；差异层在 `remove` 路径内联求出。
- `[S]` 游标 + 长度；入口条件同 `[L]`，不 reserve；真正逻辑在 `spD_remove(C,len)`，它把 `len` clamp 到 `bytes - offset(C)`，再 `sp_advance(&R,len)`，越尾用 `spK_locend`。
- `[P]` 长度式；入口 `pt_valid` + no-op 判定后 reserve `2L+2`，主体在 `ptD_remove`：clamp、`ptK_markdirty` fork、为 `R` 设置 transient tree / paths 并沿共享路径 COW。COW 是 [P] 的领域语义，也是它需要节点预算的原因。

### 3.2 `range`

**共同流程**

1. 取 `rt` scratch。
2. 清空 `rt[0..MAX_LEVEL)` 的 `child_count`。
3. `trimright(L)`，`trimleft(R)`。
4. `cutrange(L,R,rt,fl)`。
5. `stitch(L)`。

**差异**

- `[L]` 接收 `fl`（与 `[S]` / `[P]` 同形），由 `remove` 路径求出；额外保存 `rm = L->col`，最后 `L->col = 0; addbytes(L, rm)`。
- `[S]` 接收 `fl`，由 `spD_remove` 用 `spD_diverlevel` 求出；清空 `rt` 后直接 trim / cut / stitch。
- `[P]` 接收 `fl`，由 `pt_remove` 在 COW 后求出；清空 `rt` 时还要设置 `rt[k].version = L->tree->root.version`，供 `ptN_purge` 判断哪些共享节点属于本次修改版本。
- **为什么 [L] 多 `rm` + `addbytes`**：删除起点可能在行内；`trimright` 按行粒度截断，必须把行内前缀暂存，最后由 `addbytes` 重新加到 stitched 第一行。`[S]`/`[P]` 是纯字节模型；跨行前缀逻辑属于 `[L]`。

### 3.3 `trimright`

**共同流程**

1. 取当前叶 / span / piece 的父 `p` 和槽号 `i`。
2. 计算“当前容器从 cursor 位置到末尾”的待删长度。
3. 从当前层向上扣掉该长度的 bytes（`[L]` 还要扣 lines）。

**差异**

- `[L]` `lc = p->breaks[i]`（叶行数），`db = sum(bytes[lnu..lc-1])`，`dl = lc - lnu`，`M_up(C, levels, -db, -dl)`；**只改度量，不改 leaf 行数组**。原因：`mergeleaf` 稍后会把 R 行覆盖到 L 叶的 `lnu` 位置，`addbytes` 再把 `L->col` 前缀加回。
- `[S]` `spA_touch(L, p, i, -(p->bytes[i] - L->poff), 1)`：把 span 缩到 `L->poff`，通知 arbiter，并传播 bytes。span 模型按字节计，删除量就是字节差。
- `[P]` `ptM_up(L, l, -(p->bytes[i] - L->poff))`：只改度量；hole 数据 / literal 指针暂不物理截断，留给 `mergeleaf` / `cutpiece` 处理。

### 3.4 `trimleft`

**共同流程**

1. 取 R 当前叶 / span / piece 的父 `p` 和槽号 `i`。
2. 计算 R 容器内要从起点删掉的前缀长度。
3. 扣度量，并把 R 的内容调整到“从 R 的 cursor 位置开始”。

**差异**

- `[L]` 若 `R->lnu >= lc`（R 已在叶尾 / 虚拟态）：整叶删除，`db = p->bytes[i]`，`dl = lc`。否则 `db = sum(bytes[0..lnu-1])`，`dl = R->lnu`，再 `memmove` 把当前行及后续行移到行数组开头，`db += R->col`，`lf->bytes[0] -= R->col`。最后 `M_up(-db,-dl)`。**物理移动**原因：R 叶的第一行将成为拼接后的后半，不会被 `mergeleaf` 覆盖，必须就地扣掉 `R->col`。
- `[S]` `spA_touch(R, p, i, -(R->poff), 1); R->poff = 0;`；前缀就是 span 内偏移。
- `[P]` hole：`ptH_remove(p, i, 0, R->poff)`；literal：`children[i] = literal + R->poff`，`bytes[i] -= R->poff`；然后 `ptM_up(R, l-1, -R->poff); R->poff = 0;`。hole 需要物理 `memmove`，literal 只需改指针长度。

### 3.5 `cutrange`

**共同骨架**

**共同骨架（中文步骤）**

1. 取 `State` / `Tree` 引用和 `levels`；`db = 0`（`[L]` 还有 `dl = 0`）。
2. 对 `kl = levels` 向下到 `fl + 1`：
   - 取 L 侧当前父 `p`、槽号 `i`、原 child_count `cc`。
   - `p->bytes[i] -= db`；把 L 右侧 sibling 的 bytes 累加进 `db`。
   - `k = levels - kl`；`N_remove` 摘掉 `[i+1, cc)`。
   - 取 R 侧当前父、槽号 `i`、cc；`i += (k || 当前槽长度为 0)`。
   - 把 R 的 `[i, cc)` 复制进 `rt[k]`；把 `[0, i)` purge 并 `setcc(p, 0)`。
3. 在 `fl` 层：复制 R 右侧 tail 到 `rt[k]`；共同父 `setcc(p, i)`；再扣 L child、累加 L 右侧、`M_up(L, fl-1, -db)`，最后 `N_remove` 摘除 `[Lindex+1, cc(p))`。
4. `k = levels - kl`，所以 `rt[0]` 是叶层，`rt[k]` 是第 `levels-k` 层；`rt` 由 `stitchnode` 逐层挂回。

**差异**

- `[L]` 的“当前槽长度”是 `p->breaks[i]`；还维护 `breaks[i] -= dl`、`dl += sumbreaks(...)`、`M_up(..., -db, -dl)`。`N_purge` 在 `k == 0` 时释放 `lc_Leaf`，`k > 0` 时递归释放 `lc_Node`。
- `[S]` 的“当前槽长度”是 `p->bytes[i]`；只有 bytes 通道。`spN_remove` 接收 `sp_Tree *`，在 `k == 0` 叶槽处逐槽调用 `spA_died(T, id)`，叶槽只存 id，释放动作交由池。`spN_copy` 同时复制 `children`、`bytes`、`mask` 三个数组。
- `[P]` 的“当前槽长度”是 `p->bytes[i]`；`ptN_remove(S,p,k,s,e)` 内部用 `p->version` 调 `ptN_purge`：只 purge 版本匹配的共享节点 / 释放 hole；`ptN_copy` 同时处理单字 `mask` 的位段；`ptN_remove` 末尾 `ptM_clamp(p)`，若 `cc == 0` 还清 `children[0]` / `bytes[0]`。
- `[P]` 在 `range` 中把 `rt[k].version` 设为当前 root version，`cutrange` 里 `ptN_purge(..., rt->version)` 才能正确区分“本次版本新切下的节点”和“共享旧节点”。
- **共同 assert**：三库都在函数内 `assert(s <= e && e <= cc(p))`，先取 `cc = cc(p)`，再 purge、前移、`setcc`。

### 3.6 `mergeleaf`

**共同意图**：`cutrange` 之后，`rt[0]` 保存 R 侧尾部叶；当当前最后一个叶槽与 `rt[0]` 满足合并条件时直接合并，减少一次 stitch 挂回；否则把 cursor 放到 `children[cc]`，由 `stitchnode` 把 `rt[0]` 挂到那里。

**差异**

- `[L]` 返回 `int d`（行修正量）。
  1. `p = parent(C, levels)`，`cc = cc(p)`，`rtlc = rt[0].breaks[0]`；`bs`/`ls` 指向当前最后叶的 bytes/breaks 槽（即 L 前缀度量）。
  2. `dl = min(rtlc, LC_LEAF_FANOUT - *ls)`：本次能搬的行数。
  3. 若 `*ls == LC_LEAF_FANOUT`，最后叶已满，返回 0，让 `stitchnode` 把 `rt[0]` 挂到新槽。
  4. 把 R 叶前 `dl` 行复制到 L 叶末尾。
  5. 若 `*ls + rtlc <= LC_LEAF_FANOUT`：可以整叶并入。释放 `rt[0]` 的 leaf，父少一个 child；若 cursor 原本 one-past，先 `lnu = *ls, nu -= *ls, off -= *bs`；把合并后的 L 叶搬回 `rt[0]`，`setcc(p, cc-1)`，`db = -*bs, dl = -*ls`。
  6. 否则只能部分搬移：`memmove` 掉 R 叶已搬走的行；算出搬走的 bytes `db`；若 cursor 原本 one-past 或就在最后叶，调整 `lnu/nu/off` 到“合并后叶尾”；`*bs += db, *ls += dl`；`d = LC_LEAF_FANOUT - C->lnu, C->lnu = 0`。由于 `trimright` 已把 `p->breaks[cc-1]` 截到原 `C->lnu`，所以这里 `C->lnu == 原 *ls`，`d` 正好等于 `dl`（被搬进 L 叶的 R 行数）。
  7. `rt[0].bytes[0] -= db, rt[0].breaks[0] -= dl`；`M_up(C, l-1, db, dl)`；`C->paths[l] = &p->children[cc]`；返回 `d`。
  - **为什么 [L] 需要 `d`**：linecache 叶有独立占用率，`foldleaf` 只修 cursor 所在的叶；partial 合并后 cursor 可能被推到合并叶尾或后续 tail 叶起点，需要 `d` 在 `stitch` 末尾退回真正的删除终点。`trimright` 的“先扣度量”保证了 `C->lnu` 与 `*ls` 一致。
- `[S]` 返回 void。
  1. `p = parent(C, levels)`，`cc = cc(p)`，`bc = p->bytes[cc-1]`。
  2. 若最后 span 的 id `!= rt[0]` 首 span 的 id：不合并，只把 cursor 推到 `children[cc]`：`off += poff; poff = 0; paths[l] = &p->children[cc]`。
  3. 若 id 相同：把 L 最后 span 合并进 `rt[0]`：`rt[0].bytes[0] += bc`，`rt[0].children[0] = p->children[cc-1]`，`rt[0].mask[0] |= p->mask[cc-1]`，`spA_died(T, old_id)`；父少一个 child；`M_up(l-1, -bc)`；若 cursor 原本 one-past，调整 `off -= bc, poff = bc`；最后 `paths[l] = &p->children[cc-1]`。
  - `d` 只在 `[L]` / `[P]` 使用：spantree 叶同型且 cursor 的绝对位置在 mergeleaf 内就地维护，`stitchnode` 挂回后 cursor 就是删除终点。
- `[P]` 返回 `int d`（字节修正量），原因见 `design_piecetab_v2.md` §2.4 与当前代码：
  1. `cc = cc(p)`，`bc = p->bytes[cc-1]`（L 前缀长度），`hL = mask bit cc-1`，`hR = mask bit 0`。
  2. `lit + lit` 且物理连续（`literal + bc == rt literal`）：`merged = 1`。
  3. `hole + hole`：`d = min(rt->bytes[0], PT_MAX_HOLESIZE - bc)`；把 `rt` hole 前 `d` 字节 `ptH_append` 到 L hole；若 `d < rt->bytes[0]`，`ptH_remove(rt,0,0,d)` 并 `ptM_up(C, l-1, d)`；否则释放 `rt` hole，`merged = 1, d = 0`。
  4. 若 merged 为假：`C->off += C->poff + d; C->poff = 0; C->paths[l] = &p->children[cc]`。这时 cursor 落到 survivor hole / tail piece 的槽尾，`d` 记录已从 `rt` 搬走多少字节，供 `stitch` 末尾回退。
  5. 若 merged：把 L 最后 child 搬到 `rt[0]`，`ptM_up(C, l-1, -bc)`；cursor 原本 one-past 则 `off -= bc, poff = bc`；`paths[l] = &p->children[cc-1]`，`setcc(p, cc-1)`，`ptM_clamp(p)`。
  - **为什么 [P] 需要 `d`**：叶异质且 hole 容量有限；`hole + hole` 常常只能部分追加，`rt` 槽存活为缩短的 hole。mergeleaf 先把 cursor 放到 survivor 槽尾，再由 `stitch` 用 `d` 回退到删除终点。

### 3.7 `stitch`

**共同流程**

1. 入口只 assert cursor 位于容器末段（`idx >= cc - 1`，三库同形）；节点预算按调用路径支付（§2.5）：append / insert 类入口各自预留（`[L]` `lc_append` 在 `stitch` 前、`[S]` 三个入口、`[P]` 四个入口）；删段路径（`[L]` / `[S]` `remove`、`[L]` `splice`）不预留——切段先回池、`makechain` 后分配，峰值缺口为 0。
2. 处理当前最后叶槽的 0 长残留（`[S]`/`[P]` 有；`[L]` 由 mergeleaf 的 fill 逻辑覆盖）。
3. `mergeleaf` 尝试把 `rt[0]` 并进最后叶，得到 `d`（`[S]` 无 d）。
4. `stitchnode` 逐层 graft `rt[]`，必要时扩根 / 建链。
5. `fold` / `rebalance` 修 underfull；修复覆盖的层范围规则见 §3.8「修复范围」。
6. 修正 cursor 到删除终点；传播最后的 mask 变化（`[S]`/`[P]` 用 `M_up(levels,0)`）。

**差异**

- `[L]`：`d = cc(p) && cc(rt[0]) ? mergeleaf(C) : 0;` 然后 `stitchnode(C)`；重新取 `p/l`；若 `cc(p) && foldleaf(C)` 则 `rebalance(C)`；若 `d > C->lnu`，先 `backwardnode(C,1,l)` 退一个叶，`d -= C->lnu`，把 `lnu` 放到该叶末尾（`lnu = p->breaks[i]`，`nu`/`off` 相应减掉该叶），最后 `C->lnu -= d`，`C->loff = sumbytes(leaf,0,lnu)`。`[L]` 不在 `stitch` 里做 mask 传播。
- `[S]`：先删当前最后槽的 0 长 span（`if (cc && p->bytes[cc-1] == 0) N_remove(C->tree, p, 0, cc-1, cc);`）；`if (cc && cc(rt[0])) mergeleaf(C);`；`stitchnode(C)`（根收缩的两次时机都在 `stitchnode` 内，见 §3.8「修复范围」第 4 条）；重新取 `l/i/cc`；若 `i == cc`，`--C->paths[l]; C->poff += p->bytes[cc-1]; C->off -= p->bytes[cc-1];`；最后 `M_up(C, levels, 0)` 纯 mask 传播。
- `[P]`：先删当前最后槽的 0 长 piece；`d = cc && cc(rt[0]) ? mergeleaf(C) : 0;`；`stitchnode(C)`（缩根的两处时机都在 `stitchnode` 内）；`i == cc` 时 `--C->paths[l]` 并同步 `poff`/`off`；`(size_t)d > C->poff` 时 `backwardnode(C,1,l)` 退一个 piece，`d -= C->poff`，`C->poff = p->bytes[i]`，`C->off -= p->bytes[i]`；最后 `C->poff -= d`，`ptM_up(C, levels, 0)`。
- **为什么 [L] 多一步 `foldleaf`**：linecache 叶是独立类型且有独立扇出 / 占用率，`foldleaf` 先修叶层；`rebalance` 三库同形，内部从 `levels - 1` 起逐层 fold，叶层由 `foldnode` 的叶容器分支处理。
- **为什么 [S] 的 cursor 在 `mergeleaf` 内完成定位**：spantree 的 cursor 在 `mergeleaf` 内已经落到删除终点 / one-past，`stitchnode` 挂回后位置正确。`[L]` 因行索引型 cursor 和 partial merge；`[P]` 因异质叶和 partial hole 追加，都使用 `d` 回退。

### 3.8 `stitchnode`

**算法思想：分段修复**

`cutrange` 把 L 路径上 `kl >= dfl` 各层节点右侧的子树切进 `rt[]`，`mergeleaf` 处理叶容器层，`stitchnode` 负责把这些子树接回树。主循环按“叶 → 根”逐层推进（`k = 0..levels(C)`，`kl = levels - k`）：每层先尽量把 `rt[k]` 追加到该层节点末尾；放得下就继续上一层。

放不下时进入一次**分段修复**：

1. `findroom`（§3.9）从 `kl` 层向上找第一个“cursor child 之后还有空位”的祖先层 `fl`：把该槽位之后的尾部 children 整体移入 `rt[levels-fl]`，并把祖先截到 cursor child 后一槽。该层比当前 `k` 更靠 root、尚未被主循环处理，`findroom` 以 `assert(cc(rt[levels-fl]) == 0)` 记录“此时该槽位必为空”这一前提。
2. `makechain`（§3.10）从 `fl` 层垂一条单链到 `kl` 层，`paths` 指向链底新节点的首槽。
3. 把 `rt[k]` 剩下的 children 放进这个新节点，记下当前层号 `l` 与这一段要回退的槽数 `d`。
4. 回到主循环继续向上。下一次再遇到放不下时，先对 `[kl, l-1]` 逐层 `foldnode` 修这一段留下的 underfill，再用 `backwardnode(C, d, l)` 把 `paths` 指回上一次放进树的节点。

于是整个过程是“接一段、修一段、再往上接”：每一段的 underfill 都在下一段开始前修完，直到 `kl == 0`（root 层）做最后一次修复与收尾 —— foldnode 之前先缩根一次、循环结束再缩根一次（§3.7、§3.11）。这样安排的原因是 graft 同时改变占用与度量，而 `foldnode` 只在 `paths` 上修 underfill：只有先把这一段接好、把位置记下来，等下一次修复时树形已稳定，才能回退到正确的槽位。

**共同骨架（中文步骤）**

1. `k, i, d = 0, levels(C)`；`db = 0`（`[L]` 还有 `dl = 0`）。
2. 对 `k = 0..levels(C)`：
   - `kl = levels - k`；`rtcc = cc(rt[k])`；先 `setcc(rt[k], 0)`，再取 `i = idx(C, parent(C,kl), kl)`。
   - 若 `i < cc(p)`，把下层累计的 `db` 加到当前父槽（`[L]` 还有 `dl`）。
   - `m = min(rtcc, FANOUT - cc(p))`；若 `m > 0`，复制 `rt[k]` 前 `m` 个 child 到父末尾，累加 `db`（`[L]` 还有 `dl`）。
   - 若 `m == rtcc && kl != 0`，本层已放完，继续下一层。
   - `M_up(C, kl-1, db)`（`[L]` 还有 `dl`）；清空累计。
   - 若 `kl == 0`，先缩根（三库都直接调 `shrinkroot(C)`），并按新 `levels` 修正 `l`。
   - 从 `kl` 到 `l-1` 逐层 `foldnode(C, fl == kl, fl)`；若 `k > 0`，`backwardnode(C, d, l)`。
   - 若 `m == rtcc`，继续下一层。
   - 否则 `p = parent(C, l = kl)`；`d = k ? cc(p) - idx(C,p,l) : m`；`l += findroom(C, l[, nofail])`（`[P]` 的 `findroom` 返回 void，`l` 不变）；把 `rt[k]` 剩余 child 挂到新父；累加 `db`（`[L]` 还有 `dl`）。
3. 循环结束：再缩一次根，收掉 sweep 的 `foldnode(0)` 可能造成的单 child root（三库都在 `stitchnode` 末尾调 `shrinkroot(C)`）；`rt[]` 已全部消费并清空，新挂节点的度量已上推。

**实现技巧与原因（三库共同）**

1. **`d` 与 `backwardnode`**：`findroom` 垂下来的单链一定是 underfill 的，而它左侧的节点一定是满的 —— 不满就不会溢出，也就不会走到这一分支；所以这一段最终必须靠 `foldnode` 修单链上的 underfill。而 `foldnode` 只能修 `paths` 上的节点，被搬走的尾部又正是待修的位置、且可能被下一段的 `foldnode` 合并 / 平均掉，因此 `paths` 不在这一段就地指回它：先记下 `d`，等下一次修复完成、树形稳定之后，再由 `backwardnode(C, d, l)`（§3.15）按 `d` 把 `paths[l]` 回退到修复后的正确位置。`d` 的取值：`k == 0`（叶容器层，没有“移入 `rt` 的尾部”）取放进容器的槽数 `m`；否则取 `cc(parent(C,kl)) - idx(C,·,kl)`，即该层 cursor 槽及其右侧的 child 数。
2. **`db` 分段上推**：graft 进树的节点度量不逐层立刻向上加，而是攒在 `db`（`[L]` 还有 `dl`）里，等进入下一段修复前用一次 `M_up(C, kl-1, db)` 统一更新从当前层到 root 的槽度量。必须一段一段更新、不能只留到最后一次：随后的 `foldnode` 会改变树结构（合并 / 搬移 child），届时刚 graft 的槽位已不在原位，度量就加不到正确的槽上。
3. **每层“先追加、后判断”**：`m = min(rtcc, FANOUT - cc(p))` 先尽量放；只有 `m < rtcc`（溢出）或 `kl == 0`（root 层）才进入修复 / 收尾，`m == rtcc && kl != 0` 直接继续上一层。修复只在真的溢出或需要收尾时发生。
4. **root 收缩的两个必需时机**（root 自身不参与 fold，`foldnode` 修的是 `kl = l+1` 层；root 层的收尾是“cursor child 仍 underfill 时 `foldnode(0)`，`cc(root) == 1` 时缩根”，缩根循环见 §3.11）：
   - **foldnode 之前**：`foldnode(0)` 的前提是 `cc(root) > 1`（§3.12）。`cutrange` 截断 `kl = dfl` 层、或上一段合并，都可能让 root 只剩 1 个 child，此时根层 fold 无法进行；所以进入 sweep 之前必须先缩根。三库同为收尾块里的 `if (kl == 0) shrinkroot(C), l -= (k - levels(C));`。缩根会让 `levels` 变小，收尾用的 `l` 按删掉的层数同步下调。
   - **foldnode 之后**：sweep 的 `foldnode(0)` 自己可能走 merge 分支（`cL + cR <= FANOUT`），并以 `N_remove` 删掉 root 一个 child，使 `cc(root)` 由 2 变 1；这次下降发生在“foldnode 之前”的判定之后，所以 sweep 结束后还要再缩一次。三库都在 `stitchnode` 循环之后调 `shrinkroot(C)`，`stitch` 不再额外调 `rebalance`。
   - 两次落在同一段 `while (levels && cc(root) == 1)`：前一次让根可 fold，后一次收掉 fold 自身造成的单 child root。settled 树的不变式是“`levels > 0` ⇒ `cc(root) >= 2`”；只有 `levels == 0`（root 的孩子就是叶槽）才允许 root 只有一个 child。

**修复范围（三库共同）**

记分歧层为 `dfl`（`cutrange` 的入参，即 L / R 两条 cursor 路径最先分叉的层号）。需要修复的层由脏层决定，推导如下。

1. 脏层的两个来源：
   - `cutrange` 在每一层 `kl ∈ [dfl, levels]` 对 L 路径节点做 `N_remove(p, k, Lindex+1, cc(p))`，最坏把该层节点的 `cc` 截到 1；其中 `kl == levels` 是叶容器层，不受 `FANOUT/2` 约束，不需要 fold。
   - `findroom` / `makechain` 改动或新建的层：搬走尾部的祖先层（`cc` 减 1）、扩根产生的新 root 与链节点（新节点里未写过的槽使 `cc` 为 0 或 1）。
2. 起点推导：`foldnode(l)` 修的是 `kl = l+1` 层的节点（`ns` 取 `parent(C,l)` 的 cursor child 与邻居），前提是 `cc(parent(C,l)) > 1`。最浅的内部脏层是 `kl = dfl`，所以修它的 fold 在 `dfl-1` 层 —— sweep 起点 = `dfl-1`（`dfl == 0` 时取 0，root 自身由缩根修复，`foldnode(0)` 修它的 children）。中途扩根 g 次后脏层整体下移 g 层，起点随之取收尾层的 `kl-1`。
3. 级联推导（修复为什么必须要一路修到 root）：`foldnode` 的合并分支以 `N_remove(p, i+1, i+2)` 收尾，删掉 `p`（`kl = l` 层节点）一个 child。若合并前 `cc(p) == FANOUT/2`，合并后为 `FANOUT/2 - 1 < FANOUT/2`，`p` 成为新的违规节点；而修它的 fold 在 `l-1` 层、已被向深的 sweep 走过，所以修复必须继续向上。对祖先链归纳：每有一层在合并前恰好 `FANOUT/2`，级联就再上升一层；sweep 范围内最深的 fold 也可能合并，所以向上走的最坏高度是到 root，与 `dfl` 无关（`dfl` 只决定 sweep 从哪一层开始）。balance 分支不改变 `cc(p)`，级联终止于 child 已健康或只 balance 的层（`rebalance` 的 `for (; l > 0; --l)` 就是这条向上级联：child 已健康即返回、只 balance 即返回）。
4. 最小占用形状的例子（每层路径节点只有 `FANOUT/2` 个 child，级联被迫到 root）：取 `FANOUT = 4`、`levels = 10`，树里每个内部节点恰好 2 个 child（= `FANOUT/2`），叶容器同样 2 个叶槽（层 10 的容器与 root 不受 `FANOUT/2` 下限约束），在 `dfl = 8` 处删除第 8 层节点 P 的 child 0（一个第 9 层节点，子树 = 2 × 2 = 4 个叶槽）的整棵子树。推导：
   - `cutrange` 把 P 截到 `cc = 1`（P 是最浅的内部脏层，`rl = 2`），修它的是 `foldnode(7)`。
   - 第 7 层路径节点恰好 2 个 child，且两个 child 之和 `cc(P) + cc(P 的兄弟) = 1 + 2 = 3 <= FANOUT`，只能走 merge ⇒ 第 7 层路径节点被删掉一个 child，`cc` 从 2 变 1，成为新的违规节点；修它的是 `foldnode(6)`。
   - 同理逐层上升，经过 `7, 6, 5, 4, 3, 2, 1, 0` 共 8 层 fold，最后 root 的 `cc` 从 2 变 1，触发缩根（`levels` 10 → 9）。
   - 必要性：若修复止步于 `dfl-1 = 7` 或 `dfl-2 = 6`，则在第 7 层或第 6 层会留下 `cc = 1 < FANOUT/2` 的内部节点，违反内部节点下限。即“路径上每层都只剩 `FANOUT/2` 个 child”的形状把修复一路逼到 root；只有每层都刚好最小占用时级联最长，`FANOUT/2` 以上的层在合并后仍 `>= FANOUT/2`，级联随之提前终止。
5. 结论：graft 只触及 `rt[k]` 非空的层，而修复范围由脏层（第 1 条）与级联（第 3、4 条）决定，必须能覆盖到 root；root 的收尾（foldnode 前后各一次缩根）见「实现技巧与原因」第 4 条 —— 修到哪一层由“路径上还有多少层 `cc == FANOUT/2`”决定，无法由 `dfl` 或任何固定偏移推出。

**差异**

- 三库参数名统一为 `C`。
- `[L]` 双度量 `bytes/breaks`；`[S]`/`[P]` 只有 bytes，且每层用 `M_remask(p,i,k)` 重算 mask / hole 位。
- `[L]` `findroom(C, l, 1, NULL)` 返回 int，`l +=` 表示可能扩根；`[S]` `findroom(C, l)` 同样返回 int 且 `l +=`；`[P]` `findroom(C, l)` 返回 void，remove 路径的 `findroom` assert `fl >= 0 && cc(p) - i - 1 == 0`，扩根分支由 compact 使用。
- `[L]` 在 `m > 0` 时用 `lcN_copy` 只复制 children/bytes/breaks；`[S]`/`[P]` 的 `N_copy` 还会复制 mask / 位段。
- `[S]` 与 `[P]` 的循环骨架非常接近；主要差异是 `findroom` 签名和 `ptD_foldnode` 的 COW / mask 位处理。

### 3.9 `findroom`

**共同流程**

1. 从 `l-1` 层向上找第一个 cursor child 之后有空位的祖先 `fl`：`idx(C, parent(C,fl), fl) < FANOUT-1`。
2. 如果该祖先 cursor child 之后还有尾部 children，把它们整体搬进 `rt[levels-fl]`，并把祖先截到 cursor child 后一槽；同时从 `fl-1` 向上扣掉这些 children 的度量。
3. 调 `makechain(C, fl, l, nofail)`：在 `fl..l` 之间插入空节点链；若 `fl < 0`，扩根。

**差异**

- `[L]` `findroom(C, l, nofail, touched)`：搬尾部 children 时同时维护 `db`（bytes）和 `dl`（breaks），用 `M_up(C, fl-1, -db, -dl)`；预留由本函数入口的 `checkchain(C, fl, l)` 完成，随后 `makechain(C, fl, l)`。
  - **`touched` 出参**：回填"本次调用在树里实际动过的最深层"——`fl >= 0` 时为 `l - fl`，`fl < 0`（只扩根：链挂在 `root.children[1]` 上，回滚的 deroot 会整条清掉）时为 0。
  - **为什么 `[L]` 需要它**：`lc_append` 是批量的，无法在入口一次预留，失败时靠 `lcB_rollback` 回滚；回滚要知道"该在哪些层 purge 并把 `rt[k]` 填回去"。`findroom` 的搬尾发生在 `fl` 层、`makechain` 的链节点铺在 `fl..l` 上，所以被动过的层恒为前缀 `[0, l-fl]`，`touched = l - fl` 正是该前缀右端；它是单标量（不是每层数据），由 `lc_append` 取最大值累计成 `water`。`[S]`/`[P]` 的 remove 路径不需要回滚水位，故无此出参。
- `[S]` `findroom(C, l)`：签名不含 `nofail`；只维护 bytes，用 `M_up(C, fl-1, -db)`；然后 `makechain(C, fl, l)`。三库的祖先查找循环同为 `if ((i = K_idx(C, p = K_parent(C, fl), fl)) < FANOUT - 1) break;`。
- `[P]` `findroom(C, l)`：采用 `assert(fl >= 0 && cc(p) - i - 1 == 0)` 契约，然后 `makechain(C, fl, l, 1)` 并丢弃返回值。
  - **契约证明（`cc(p)-i-1 == 0`）**：
    - 若 `kl == fl`：`cutrange` 在 `fl` 层把共同父截到 `Lindex + 1`；`rtcc = cc_original - iR_adj`，其中 `iR_adj >= iR` 且 `iR > Lindex`（L、R 在 `fl` 层不同槽），所以 `rtcc <= FANOUT - (Lindex + 1)`，当前父一定放得下，`findroom` 在该层完成空间判断。
    - 若 `kl > fl`：`cutrange` 已处理 `kl..fl+1` 每一层 L 路径，并把 `fl` 层共同父截到 `Lindex + 1`；因此从 `kl` 向上到 `fl` 的每个祖先，cursor child 之后为空。`findroom` 从 `kl-1` 向上找，最迟在 `fl` 层因 `idx = Lindex < FANOUT-1` 停下；找到的祖先满足 `cc(p)-i-1 == 0`。
    - 因此该 assert 是 remove 路径可证明的契约，为可证明契约；`[L]`/`[S]` 的 `findroom` 还被插入 / fill 等其他调用者使用，所以保留搬移分支。

### 3.10 `makechain`

**共同流程**

1. 若 `nofail == 0`，先 reserve `to - from + 1` 个节点；失败返回 ERRMEM。
2. 若 `from < 0`：分配一个新节点 `nn`，把旧 root 内容复制到 `nn`；旧 root 清成“只有 child 0 = nn”的新 root；把 `paths[to]` 之后的路径槽右移一位；`levels += 1`；更新 `from/to/cp/r` 标记扩根。
3. 从 `from` 到 `to-1`，每层分配一个空节点，挂到当前父的 `children[cc(p)]`，paths 指向该新槽，`child_count += 1`。
4. 把 `cp` 指向最后一个新节点的 `children[0]`，返回是否扩根。

**差异**

- `[L]` `makechain(C, from, to)`：纯分配，签名不含 `nofail`；预留由调用方 `lcD_checkchain` 完成，公式与 `[P]` 一致：`to - from + (from < 0)`；扩根时旧 root 的 `bytes[0]` / `breaks[0]` 设为全树总量；新空节点用 `lcN_setcc(nn,0)`；叶数组无 mask。
- `[S]` `makechain(C, from, to)`：签名不含 `nofail` / reserve；扩根时旧 root 的 `bytes[0] = bytes(C)`，`mask[0] = spM_sumns(nn)`；新空节点 `memset(nn,0,sizeof *nn)`；叶槽只存 id，对象分配由调用方入口 reserve 覆盖。
- `[P]` `makechain(C, from, to, nofail)`：有 reserve；扩根分支还 `assert(to == levels(C))`，并计算旧 root 是否含 hole：`h = (p->mask & ptM_mask(cc)) != 0`，新 root 的 child 0 位用 `ptM_sethole(p,0,h)` 设置；新空节点要设 `version = root.version`、`mask = 0`；最后 `ptM_sethole(p, cc-1, 0)`。
- **`nofail` 与批量插入复用**：`[P]` 的 `ptD_makechain` 有 `nofail`，同时服务批量插入 / compact，那条路径允许失败并返回 `ERRMEM`；remove 路径传 `nofail == 1`，表示入口已 reserve。`[L]` 与 `[S]` 的 `makechain` 签名都不含 `nofail`，为纯分配：`[L]` 由调用方先调 `lcD_checkchain` 预留（append 路径在 `lcD_findroom` 内、scan 路径在 `lc_scan` 内），`[S]` 使用调用方入口 reserve 的契约。这属于代码复用领域差异。
- **扩根分支也是代码复用差异**：`[L]`/`[S]` 的 `from < 0` 分支来自批量插入 / scan 的树长高；remove 只是复用 `makechain` 时可能走到该分支。`[P]` 的 `from < 0` 分支只被 compact 路径使用，remove 路径被 `findroom` 的 `assert(fl >= 0)` 挡住；`[P]` remove 需要增长树时走 `leaf` 里的 `ptI_splitroot` / `ptI_splitchild`。`[S]` 的 remove 路径不触发搬尾与扩根这两条分支（`cutrange` 在 `fl` 层已保证余位），它们由 `sp_fill` / `sp_clear` 经 `spF_range` → `stitch` 到达；`[L]` 的 `from < 0` 还由 `lc_append` / `lc_scan` 的树长高到达。这是与 `nofail` 同类的代码复用领域差异，其存在源于批量插入 / compact / fill 复用。

### 3.11 `rebalance` / `shrinkroot`

**exact-sync 部分**

三库除前缀、节点类型、`N_cc` 外逐语句相同（`[L]` / `[S]` 行号见 §4 表；`[P]` 为 `/* remove */` 段）：

1. 取 `p`；三库都在函数内取 `l = levels - 1`。
2. `for (; l > 0; --l)`：若 cursor child 的 child_count 已 `>= FANOUT/2` 立即返回；否则 `assert(cc(p) > 1)` 后调 `foldnode(C, 0, l)`；`foldnode` 返回 0（只 balance 不合并）立即返回。
3. 尾块：root 的 cursor child `< FANOUT/2` 且 root 至少 2 个 child 时，调 `foldnode(C, 0, 0)`。
4. 调 `shrinkroot(C)`。

**`shrinkroot`**

`while (levels && cc(root) == 1)`：取 root 唯一 child 的槽号 `i`，把该 child 内容赋值给 root，释放旧 child 节点，`--levels`，`paths[0] += i`，`memmove` 上移 `paths[1..]`。三库同名同形（`lcD_shrinkroot` / `spD_shrinkroot` / `ptD_shrinkroot`），调用点一一对应：

| 调用点 | 作用 |
|---|---|
| `rebalance` 末尾 | 收掉级联 fold 造成的单 child root |
| `stitchnode` 的 `kl == 0` 分支 | fold 之前先让根可 fold |
| `stitchnode` 循环之后 | 收掉 sweep 的 `foldnode(0)` 合并造成的单 child root |
| 批量路径收尾（`[P]` `ptZ_build` / `[L]` `lc_scan`） | 批量重建后收尾；`[S]` 无对应批量路径 |

**差异**

- **签名**：三库都是 `rebalance(C)`，函数内取 `l = levels - 1`；`ptD_leaf` 与 `ptC_freeze` 调用它时不带层号（`ptD_rebalance(C)`），返回后重新取 `levels`。
- **尾块条件**：三库都是 `if (levels > 0)`（循环只在 `l == 0` 处离开，否则提前 `return`，所以不必再判 `l`）。
- 缩根分支三库分别用 `lcP_free` / `spP_free` / `ptP_free`，结构相同。
- **根的下界不变式**：settled 树满足 `levels > 0` ⇒ `cc(root) >= 2`。所有能改动 `cc(root)` 的路径（`foldnode` 的 merge 分支、`stitchnode` 的 sweep）都以 `shrinkroot` 收尾，因此 rebalance 尾块的 `cc(root) >= 2` 守卫在 fold 之前恒成立：merge 造成的单 child root 由随后的 `shrinkroot`（本函数末尾或 `stitchnode` 末尾）收掉，不会流到下一次 `rebalance`。

### 3.12 `foldnode`

**共同骨架**

1. `p = parent(C,l)`，`i = idx`，`ns = &p->children[i]`。
2. 若当前 child 的 `child_count > FANOUT/2`，不用 fold，返回 0。
3. 选择邻居：若 cursor 在最后一个 child，或 `i > 0 && lfirst`，则改用左邻居 + 当前 child，并置 `r = 1`。
4. 若左右 child 的 child_count 之和 `<= FANOUT`：把右 child 内容复制到左 child 末尾，左 child 接管，右 child 置空；合并父槽 bytes/metrics；修正 cursor 路径（若 cursor 在右 child）；`N_remove` 删除右 child；返回 1。
5. 否则 `balancenode(ns, !r, &ds)`：左 / 右搬 `d` 个 child；更新父槽 bytes/metrics；修正 cursor（若在右 child）；返回 0。
6. 层语义：`foldnode(l)` 修的是 `kl = l+1` 层的节点；调用方必须保证 `cc(parent(C,l)) > 1`（`rebalance` 与 `stitchnode` 的调用点都以此为前提，见 §3.8「修复范围」）。
7. 返回值即级联信号：1 = 本层发生了合并，`cc(parent(C,l))` 因此减 1，可能低于 `FANOUT/2`，调用方需要继续修 `l-1` 层；0 = 当前 child 已健康或只做了 balance（`cc` 不变），可以停止。`rebalance` 的 `for (; l > 0; --l)` 就是这条向上级联，它可以一直走到 root。

**差异**

- `[L]` 直接取 `(cL = cc(ns[0])) + (cR = cc(ns[1]))`；`[S]` / `[P]` 在合并决策前调用 `seambound`。`[L]` 父槽同时维护 `bytes` 和 `breaks`；`ds[2]` 两个通道；`N_remove` 释放叶对象。
- `[S]` 在决策前先 `spD_seambound(C,l,ns,&cL,&cR)` 预合并接缝；合并后父槽 `bytes[i] += bytes[i+1]`，并用 `p->mask[i] = spM_sumns(ns[0])` 重算 mask；balance 分支还要重算 `mask[i]` / `mask[i+1]`；`N_remove` 在叶层通知 `spA_died`。
- `[P]` 先 `assert(ns[0]->version == root.version)`，再 `ns[!r] = ptK_cow(C,l, r ? -1 : 1)` 对邻居做 COW；`ptD_seambound` 预合并物理连续 literal；合并 / balance 后用 `ptM_sethole(p,i,...)` 维护单字 mask；`N_remove` 需要 `S` 且内部按 `p->version` purge；balance 后 `ptM_clamp` 左右节点。
- **为什么 [P] 使用 COW**：fold 会修改邻居 child；若邻居是共享旧版本的节点，必须先 path-copy 以保持快照。`[L]`/`[S]` 的节点原地修改。
- **为什么 [S]/[P] 使用 `seambound`**：span / literal 有“相邻同 id / 物理连续”合并语义；在 fold 合并 / balance 之前先合并接缝，可保持 `cL + cR` 判断和节点内规范形。`[L]` 以行长为叶属性。

### 3.13 `seambound`

**共同流程**

1. 仅在 `l == levels - 1`（叶容器层）生效；若任一 child 为空则返回。
2. 检查左右 child 的边界是否可合并：`[S]` 比 id；`[P]` 两边都不是 hole 且 literal 物理连续。
3. 若 cursor 路径在左 child `ns[0]`：把右 child 首槽并入左 child 末槽；更新父 bytes；删除右 child 首槽。
4. 否则：把左 child 末槽移到右 child 首槽；更新父 bytes；若 cursor 正指向右 child 首槽，调整 `off/poff`；左 child `child_count--`。
5. 返回更新后的 `cL/cR`，供 `foldnode` 决策。

**差异**

- `[S]`：合并同 id span；左并右时 `mask[*cL-1] |= ns[1]->mask[0]`；右并左 / 左搬到右时调用 `spA_died(T, spL_id(ns[1],0))` 通知旧 id 死亡；`spN_remove(T, ns[1],0,0,1)` 删除槽。
- `[P]`：只处理两个 literal（`ptM_ishole` 为假且 `ptN_lit` 物理连续）；id 回调属于 `[S]`；左并右时直接 `bytes` 相加；左搬到右时把 `children[0] = children[cL-1]`，调整父 bytes 与 cursor，`*cL -= 1`，`ptN_setcc(ns[0], *cL)`，最后 `ptM_clamp(ns[0])`。
- **领域差异**：`[S]` 的边界属性是 opaque id 与 mask，`[P]` 的边界属性是“是否 hole + 物理地址连续”。两者的可合并判定和合并后的语义完全不同。

### 3.14 `balancenode`

**共同流程**

1. `d` 由调用方给定（`foldnode` 算出后传入），`l = cc(ns[0])`，`r = cc(ns[1])`。
2. `d < 0`：从右往左搬 `-d` 个 child；把右节点前 `-d` 个 child 复制到左节点末尾；右节点剩余部分前移；bytes delta 为左节点新增字节和（负号表示左节点变化）。
3. `d > 0`：从左往右搬 `d` 个 child；右节点后移 `d` 槽；把左节点末尾 `d` 个 child 复制到右节点开头；bytes delta 为正。
4. 更新左右 child_count；`d` 由调用方持有。

**差异**

- `[L]`：`ds[2]`，`ds[0]` 是 bytes，`ds[1]` 是 breaks；调用方还要更新父 `breaks[i]` / `breaks[i+1]`。
- `[S]`：`*ds` 单通道 bytes；无 mask 更改（`spN_copy/move` 会复制 mask 数组）。
- `[P]`：`*ds` 单通道 bytes；`ptN_setcc` 后必须 `ptM_clamp(ns[0])` 与 `ptM_clamp(ns[1])`，因为 child_count 缩小后高位 mask 位可能残留。
- `[L]` 多出的 breaks 通道是 linecache 行号承诺所必需；`[S]`/`[P]` 使用单 bytes 通道。除这个第二通道外，表达式与控制流同形。
- **`d` 入参化**：四库统一为 `调用方算 d → helper 搬移并回报 ds`，helper 不再返回 `d`。`d` 的公式 `l - ((l + r + (left != 0)) >> 1)` 由调用方在 `seambound` / `cow` 之后、平衡决策原点算出（`[L]` 直接用当前 `cL/cR`），`left` 取 foldnode 的 `!r`（`r` = 选中对侧邻居的标记）。helper 内的 `assert(d != 0)` 哨兵保留。

### 3.15 `backwardnode`

**exact-sync**：三库除前缀、节点类型、`N_cc` 外逐语句完全相同。算法流程：

1. `i = idx(C, parent(C,l), l)`。
2. 若 `d > i`：`d -= i + 1`；从 `l-1` 层向上找第一个 `idx != 0` 的祖先层 `dl`；`paths[dl] -= 1`；再从 `dl+1` 到 `l`，把每层 `paths` 设为该层父的最后一个 child。
3. 最后 `paths[l] -= d`。
4. 只改 `paths[]`，不改 `nu/off/lnu/loff/col`；调用方自己负责绝对位置字段。

**差异**：无。`[L]` `linecache.h:590-601`、`[S]` `spantree.h:564-575`、`[P]` `piecetab.h:842-853`。

### 3.16 `leaf`

**共同骨架**：删除范围在同一叶 / span / piece 内；分为“部分删除当前叶槽”和“整段删除当前叶槽”；出口修正 cursor，必要时触发 fold / rebalance；整段删除可能减少 child_count，需要修父。

**差异**

- `[L]` `leaf(C,del)`：
  1. `end = C->lnu`，`lc = p->breaks[i]`。
  2. 若 `del < leaf->bytes[end] - C->col`：只缩短当前行 `leaf->bytes[end] -= del`，`M_up(C,l,-del,0)`，返回 0。
  3. 否则 `del += C->col`，`removed = C->col`；从 `end` 开始消费完整行，逐行扣 `del` 和 `removed`。
  4. `memmove` 把第一个部分消费的行（或叶尾之后）移到 `C->lnu`。
  5. 若 `end < lc`，把残余行与 `C->col - del` 合成一行。
  6. 若 `end == lc`，剩余 `del` 是虚拟超出量，不写入内容；`removed` 按实际删除量修正。
  7. `M_up(C,l,removed,-(end - C->lnu))`；若 `foldleaf` 合并了叶，则 `rebalance(C)`；最后 `loff = sumbytes(leaf,0,lnu)`，返回 1。
- `[S]` `leaf(C,del)`：
  1. `assert(C->poff + del <= p->bytes[i])`。
  2. 若 `C->poff > 0 || C->poff + del < p->bytes[i]`：`spA_touch(C,p,i,-del,0)` 只缩短 span。
  3. 否则整段删：`spN_remove(T,p,0,i,i+1)`，`spK_seamleaf(C)` 合并相邻同 id，`cc--`。
  4. `M_up(C,l-1,-del)`。
  5. 若 `cc == 0`，cursor 落到 `children[0]`；否则若 `i == cc`，cursor 左移到最后一个 child 并同步 `poff/off`。
  6. 若 `cc < oc`，`spD_rebalance(C)`。
- `[P]` `leaf(C,del)`：
  1. `endpoff = C->poff + del`，`doff = C->off + C->poff`；`assert(endpoff <= p->bytes[i])`。
  2. 若当前是 literal 且 `C->poff > 0 && endpoff < p->bytes[i]`：要先把 literal 中间删变成“左 literal + 右 literal”，需要叶容器多一个槽。从当前层向上找第一个 `cc + 1 <= PT_FANOUT` 的层；若全满，`ptI_splitroot(C)`，`l = 1`；然后逐层 `ptI_splitchild(C,l)`，最后重新取 `p/i`。这是 [P] 独有的“增长树以容纳 piece 分裂”路径。
  3. `ptD_cutpiece(C, C->poff, endpoff)` 做真正的切 / 删；`ptM_up(C, l-1, -del)`。
  4. 若 `cc(p) == 0`，cursor 落到 `children[0]`；否则若 `idx == cc`，cursor 左移到最后一个 piece 并同步 `poff/off`。
  5. 若 `cc(p) < oc && l > 0`，调 `ptD_rebalance(C)`，再重取 `l`。
  6. 若 `C->poff == p->bytes[i] && doff < bytes(C)`，`ptK_forwardoff(C,0)`：把停在 piece 尾的 cursor 推到下一 piece，避免“树中段尾态”；树尾除外。
- **为什么三者完全领域特定**：`[L]` 处理行长数组、跨行拼接和虚拟尾；`[S]` 处理 span 缩短、整段删、同 id 合并；`[P]` 处理 hole/literal 异质、literal 分裂、COW 和段尾逃逸。

### 3.17 linecache 独有 helper

- `checkstitch(C)`：`return reserve(S, &S->nodes, levels + 2);`。只被 `lc_append` 调用，为随后的 `stitch` 预留节点；`[S]`/`[P]` 的批量插入路径不分配中间节点链，因此无此 helper。
- `balanceleaf(ls,l,r,&db)`：
  1. `bsL = ls[0]->bytes`，`bsR = ls[1]->bytes`。
  2. `d = l - ((l + r + 1) >> 1)`。
  3. `d < 0`：右叶前 `-d` 行复制到左叶末尾；右叶剩余前移；`*db = -sumbytes(ls[0], l, l-d)`。
  4. `d > 0`：右叶后移 `d` 行；左叶末尾 `d` 行复制到右叶开头；`*db = sumbytes(ls[1], 0, d)`。
  5. 返回 `d`。
- `foldleaf(C)`：
  1. `p = parent(C, levels)`，`ps = &paths[levels]`，`i = idx`，`ls = (Leaf **)&p->children[i]`，`o = *ls`。
  2. 若 cursor one-past（`cc(p) && i == cc(p)`）：把 cursor 规范化到最后一个叶，更新 `lnu/loff/nu/off`。
  3. 若 `cc(p) <= 1` 或当前叶 `breaks[i] > LC_LEAF_FANOUT/2`，返回 0。
  4. 若 cursor 在最后一叶，选择左邻居。
  5. 若两叶行数之和 `<= LC_LEAF_FANOUT`：整叶合并；更新父 breaks/bytes；若 cursor 原在右叶，修正 `paths/lnu/loff/nu/off`；删除右叶，返回 1。
  6. 否则 `balanceleaf`；若返回 `dl == 0`，返回 0；用断言保证 balance 方向与 cursor 所在叶一致；更新父 breaks/bytes；若 cursor 在右叶，修正 `lnu/nu/off`；返回 0。
- `addbytes(C, ins)`：
  1. 若 `ins == 0` 返回。
  2. 取当前叶槽号。
  3. 若 `C->lnu < p->breaks[i]`（cursor 在真实行内）：`leaf->bytes[lnu] += ins`，`M_up(C,l,ins,0)`。
  4. 否则（cursor 在叶尾虚拟区）：不写 leaf，只 `C->col += ins`。
  - **为什么 [L] 需要两个分支**：linecache 支持尾后虚拟区；piecetab 的越尾直接 `locend`；spantree 以 span 长度为度量。`addbytes` 只属于 linecache。

### 3.18 piecetab 独有 helper

- `H_remove(n,i,d,len)`：对 hole 数据做 `memmove(h->data + d, h->data + d + len, end - (d+len))`，`n->bytes[i] = end - len`。hole 可变，所以可以直接物理删除。
- `cutpiece(C,lo,hi)`：
  1. 取当前 piece 槽 `p/i`。
  2. 若 `ptM_ishole(p,i)`：`H_remove(p,i,lo,hi-lo)`；若 `bytes[i] == 0`，`ptN_remove` 删槽并 `ptK_seamleaf`。
  3. literal 且 `lo == 0 && hi == bytes[i]`：整段删，`ptN_remove`，`ptK_seamleaf`。
  4. literal 且 `lo == 0`：`children[i] = literal + hi`，`bytes[i] -= hi`，`ptK_seamleaf`。
  5. literal 且 `hi == bytes[i]`：`bytes[i] = lo`，`ptK_seamleaf`。
  6. literal 中间删：`ptN_makespace(p, i+1, 1)`；`children[i+1] = literal + hi`，`bytes[i+1] = bytes[i] - hi`，`bytes[i] = lo`，`ptM_sethole(p,i+1,0)`。这样中间删除变成“保留左 literal + 新增右 literal”，树中间叶槽长度保持 > 0。
- `K_seamleaf(C,right)`：叶容器内相邻 literal 物理连续时合并；若 cursor 槽正好在合并后槽，同步 `off/poff/paths`；`ptN_remove` 删除后槽。
- `K_reserve(C, nn, nl)`：nodes + holes 两池一次预留；入口层唯一预留点（§2.5）。
- `K_markdirty(C)` / `K_cow(C, l, d)`：fork transient tree 与 path-copy；`K_cow` 的版本判定为 `l == levels || n->version == root.version` 时直接返回该子节点。
- `K_beginedit(C)`：`markdirty` + 整条路径 COW；只做 fork，不含预留。
- `D_remove(C, len)`：删段机器；含 clamp、`markdirty`、为右端构造 transient tree / paths、逐层 COW、求差异层并分派 `range` / `leaf`。
- `I_append(C, s, len)`：插段机器；先 `K_beginedit`，再沿「左邻 literal 接尾 / 本槽 literal 接尾 / 前缀重合 literal」三条合并快路径，否则 `I_putpiece`。
- `I_putpiece(C, s, len, h)`：填 `rt`（`h` = 新数据做 hole），`N_makespace` + `N_copy` 放得下就直接放，放不下则 `I_insertrt`；出口修正 cursor 的 `off/poff` 并 `K_seamleaf`。

### 3.19 remove / stitch 直接依赖的原语

- `M_up`
  - `[L]` `M_up(C,l,db,dl)`：`if (db == 0 && dl == 0) return;`；从 `l` 到 0 逐层 `p->bytes[i] += db; p->breaks[i] += dl;`；最后 `tree->bytes += db; tree->breaks += dl;`。
  - `[S]` `M_up(C,l,db)`：从 `l` 到 0 逐层 `p->bytes[i] += db;`，若 `l < levels` 则 `p->mask[i] = spM_sumns(p->children[i]);`；最后 `if (db) tree->bytes += db;`。**`db == 0` 也要走路径**，因为 mask 需要重算。
  - `[P]` `M_up(C,l,db)`：从 `l` 到 0 逐层 `p->bytes[i] += db;`；`ptM_remask` 报告 mask 是否变化；mask 保持原值且 `db == 0` 时提前返回；最后 `if (db) tree->bytes += db;`。
- `N_copy` / `N_move`
  - `[L]` 复制 / 搬移 `children`、`bytes`、`breaks`。
  - `[S]` 复制 / 搬移 `children`、`bytes`、`mask`（并排数组，memcpy 自动覆盖掩码）。
  - `[P]` 复制 / 搬移 `children`、`bytes`，并用位运算处理单字 `mask` 的位段；`N_copy` 会清掉目标对应位段再写入源位段。
- `N_makespace`
  - 三库都在 index `i` 处腾出 `n` 个槽，先 `N_move` 后 `setcc`；`[P]` 还要维护 mask 位段。
- `N_purge`
  - `[L]` `k == 0` 释放 `lc_Leaf`；`k > 0` 递归 purge 子树后释放 `lc_Node`。
  - `[S]` `k == 0` 对每个叶槽调 `spA_died(T, id)`；叶槽只存 id，释放动作由池承担。`k > 0` 递归 purge 后释放 `sp_Node`。
  - `[P]` `k == 0` 只释放 hole（通过 `ptM_iterhole` 遍历 mask 位）；literal 槽的数据由 arena / 用户内存管理。`k > 0` 递归 / 释放 `version == p->version` 的共享节点；旧版本共享节点由 refc 生命周期管理。
- `N_remove`
  - 三库都先 purge `[s,e)`，再把 `[e,cc)` 前移，最后 `setcc(cc - (e-s))`。
  - 三库都在函数内 `assert(s <= e && e <= cc(p))`；首参按精确传参：`[L]`/`[P]` 接收 `*_State *`（要回池释放），`[S]` 接收 `sp_Tree *`（叶槽只存 id，死亡通知需要树，见 §5.1 D12）。
  - `[P]` 末尾 `ptM_clamp(p)`；若 `cc == 0` 还清 `children[0]` / `bytes[0]`。
- `seamleaf`
  - `[S]` `spK_seamleaf`：叶槽相邻同 id 时合并后槽到前槽；合并 `bytes` 与 `mask`；同步 cursor；`spN_remove` 删后槽。
  - `[P]` `ptK_seamleaf(C,right)`：只合并相邻 literal（都非 hole 且物理连续）；同步 cursor；`ptN_remove` 删后槽。
- `sumbytes` / `sumbreaks`
  - `[L]` 有 `sumbytes`（leaf 内区间）、`sumbytes`（node 内区间）、`sumbreaks`（node 内区间）。
  - `[S]` / `[P]` 只有 node 内区间 `sumbytes`。
- `P_reserve` / `P_ralloc` / `P_free`
  - 三库同形：pool 用 freelist + page；`reserve(S,p,n)` 保证 freelist 至少 `n` 个对象，不够时分配新页并立刻 free 回链；`ralloc` 只能从已有 freelist 取，内部 `assert(obj)`。
  - 三库 `P_init` 的上界都是 `obj_size < PAGE_SIZE / 2`。
- `K_parent` / `K_idx` / `K_leaf`
  - 三库 `K_parent` / `K_idx` 公式相同。
  - `[L]` 有 `K_leaf(C)` 解引用叶槽为 `lc_Leaf *`；`[S]` 用 `spL_id` / `spL_setid`；`[P]` 用 `ptN_lit` / `ptN_hole` / `ptM_ishole`。

### 3.20 reserve 充分性

本节的「最坏分配上界」按各库当前预留量的推导给出；`[P]` 的逐站点峰值推导见 `notes/reports/piecetab_reserve_exactness.md`。

**共同事实**

- `stitchnode` 洋葱序 `k = 0..levels` 中，`findroom -> makechain` 是唯一会分配 `nodes` 节点的位置；`mergeleaf`、`N_copy`、`N_move`、`foldleaf`、`foldnode`、`rebalance`、`shrinkroot` 只搬移 / 释放 / 原地修改已有节点。
- `makechain(from, to)` 分配 `max(0, to - from) + rootpush` 个节点，`rootpush ∈ {0,1}`；rootpush 时 `to += 1`，链覆盖多一层，rootpush 本身也消耗 1 个节点。
- `stitchnode` 第 `k` 轮只在 `m < rtcc`（当前父装不下 `rt[k]`）时调用 `findroom`；此时 `to = kl = levels - k`。
- 跨段 `makechain` 的覆盖层区间随 `kl` 逐轮递减、互不重叠，所以 `stitch` 的节点分配总量上界是单次最坏值：`rootpush(1) + chain(levels + 1) = levels + 2`。

**各库当前预算**

| 库 | 入口 | 预留量 | 与上界的关系 |
|---|---|---|---|
| `[L]` | `lc_append`（`stitch` 前一次） | `levels + 2` | 恰好等于上界 |
| `[L]` | `lcD_checkchain`（批量 append / scan 逐段） | `to - from + (from < 0)` | 逐段覆盖 `makechain` 自身分配 |
| `[L]` | `lc_remove` | 0 | 峰值缺口 0：切段先回池、`makechain` 后分配；`remove_need` 零预算见证 |
| `[S]` | `sp_append` / `sp_insert` / `sp_splice` | `levels + 2` | 恰好等于上界 |
| `[S]` | `sp_remove` | 0 | 峰值缺口 0：切段先回池、`makechain` 后分配；`remove_need` 零预算见证 |
| `[P]` | `pt_append` / `pt_insert` | `2 * levels + 2`（nodes）+ `inhole`（holes） | `levels + 2`（`makechain` 上界）+ `levels`（入口 path-copy） |
| `[P]` | `pt_remove` | `2 * levels + 2` | 同上，holes 为 0 |
| `[P]` | `pt_edit` / `pt_splice` | `3 * levels + 3` | 删段两端 COW `2L` + 匀位 COW `1` + 插段分裂 `L + 2` |
| `[P]` | `ptC_freeze`（每轮） | `1` | 冻结一轮的 `foldnode` 邻居 COW |
| `[P]` | `ptD_makechain`（`nofail == 0`） | `to - from + (from < 0)` | 与 `[L]` 的 `lcD_checkchain` 同式 |

- `[P]` 的预算来源是 COW：入口要复制整条路径（`levels` 个节点），删段 / 插段还要复制邻居（`foldnode` 的 `ptK_cow`、插段分裂的 `splitroot` / `splitchild`）。这是 `[P]` 与 `[L]`/`[S]` 的唯一预算级差异。
- `[P]` 的入口独占整段预算：`ptD_remove` / `ptI_append` 内不再预留，因此入口值即一次调用的上界；`ptK_beginedit` 在预留之后执行，失败时树与 cursor 不变。
- `[P]` 的峰值可用性由单测钉住：`append_need` / `insert_need` / `remove_need` / `edit_need` / `splice_need` 在恰好预算处成功且 `nodes.freed_obj == 0`，少一个节点即 `PT_ERRMEM` 或 `ptP_ralloc` 断言。


### 3.21 插入路径的分工与命名（三库对照）

| 层 | `[L]` linecache | `[S]` spantree | `[P]` piecetab |
|---|---|---|---|
| 入口（公共 API） | `lc_append` / `lc_insert` / `lc_splice` | `sp_append` / `sp_insert` / `sp_splice` | `pt_append` / `pt_insert` / `pt_splice` / `pt_edit` |
| 入口职责 | `valid` / no-op 判定 / 预留 / 调段机器 | 同 | `valid` / no-op 判定 / `ptK_reserve` / `S_ok` |
| 插段段机器 | `lcB_append`（`/* insertion */` 段，B = build） | 由 `spI_*` 原语在入口内直接组合 | `ptI_append`（内部先 `ptK_beginedit` fork，再走合并快路径或 `ptI_putpiece`） |
| 删段段机器 | `lc_remove` 主体（入口内联） | `spD_remove` | `ptD_remove` |
| 底层「放进一片」原语 | 叶数组填充（`lcB_cutleaf` / `lcB_fixsource`） | `spI_pad` / `spI_grow` | `ptI_putpiece`（填 `rt` → `N_makespace` / `N_copy` → 必要时 `ptI_insertrt`） |

- 命名规则：公共 API `xx_name`，内部函数 `xxX_name`（X 为分类码，全名最多一个下划线）。`[P]` 的删段 / 插段机器分别叫 `ptD_remove` / `ptI_append`，与 `spD_remove` / `lcD_remove` 同层；「放进一片」的底层原语让出 `ptI_append` 这个名字后叫 `ptI_putpiece`。
- `[P]` 的入口独占整段预算（`ptD_remove` / `ptI_append` 不再预留），因此 `pt_edit` / `pt_splice` 能把删段与插段的峰值合并成一个上界（§2.5）；`[L]` 的批量 append 无法预取总量，改由 `lcD_checkchain` 逐段预留。
- `[S]` 没有独立的插段段机器函数，`sp_append` / `sp_insert` 直接在入口内组合 `spI_grow` / `spI_pad` 等原语；这是三库在插入侧唯一的骨架级差异。

---

## 4. 逐函数分类对照表

分类列：`exact` 严格精确同步；`skeleton` 控制流同形但字段 / 回调 / 原语不同；`domain` 领域特定；`optional` 只在某库 / 某路径存在；`interface` 纯接口形状不同。

| 函数（不带库前缀） | 分类 | 关键差异 / 证据 |
|---|---|---|
| `remove` | domain / interface | 三库同为 `(C, len)`；`[L]`/`[S]` 不 reserve、原地修改；`[P]` 入口 reserve `2L+2`，主体 `ptD_remove` 做 `markdirty` + 路径 COW。 |
| `range` | skeleton / domain | 三库都接收 `fl`；[L] 保存 `rm=col`、末尾 `addbytes`；[P] 额外设置 `rt[k].version`。 |
| `trimright` | domain | [L] 行粒度扣 `db/dl`；[S] `spA_touch` 缩 span；[P] `ptM_up` 只扣度量。 |
| `trimleft` | domain | [L] 物理移动行数组并扣 `col`；[S] `spA_touch`；[P] hole `H_remove` / literal 改指针长度。 |
| `cutrange` | skeleton | 共同循环形状；[L] 双度量 + 释放叶；[S] `spA_died` + mask 数组；[P] `rt->version` + hole mask。 |
| `mergeleaf` | domain | [L] 行 item 填充并返回行 `d`；[S] 按同 id 整段合并、无返回值；[P] 按 literal/hole 异质合并、返回字节 `d`。 |
| `stitch` | domain | [L] mergeleaf `d` + `foldleaf`；[S] 先删末尾 0 长 span、无 `d`；[P] mergeleaf 字节 `d` + `d > poff` 回退。 |
| `stitchnode` | skeleton | 外层 `k/m/findroom/fold/backward` 同形；[L] 双度量 + `findroom` 返回 int；[S] mask remask + int；[P] mask remask + void findroom。 |
| `findroom` | skeleton / optional | [L]/[S] 搬尾部 children 到 `rt`；[P] assert 无尾部 children。 |
| `makechain` | skeleton / domain | [L]/[P] 含 `nofail`；[S] 签名使用调用方入口 reserve。扩根分支 [L] 由批量插入 / scan 到达、[S] 由 fill / clear 到达，[P] 由 compact 使用。 |
| `rebalance` | **exact** | 三库逐语句相同，只差前缀、节点类型、`N_cc`；`[P]` 多一个 `l` 入参（按层调用），尾块条件因此多一个 `l == 0` 操作数。 |
| `shrinkroot` | **exact** | 三库同名同形，四处调用点一一对应（`rebalance` 末尾、`stitchnode` 的 `kl == 0`、`stitchnode` 循环后、批量路径收尾）。 |
| `foldnode` | domain | [L] 无 seambound、双度量；[S] seambound + mask remask；[P] COW + hole mask + 版本断言。三库均在本函数内算出 `d` 后传入 `balancenode`。 |
| `seambound` | domain | [S] 同 id 合并 + `spA_died`；[P] literal 物理连续合并 + `ptM_clamp`。 |
| `balancenode` | skeleton | 搬移公式与方向完全相同；`d` 为调用方入参（`foldnode` 在平衡决策原点算出）；[L] `ds[2]`；[S] `*ds`；[P] `*ds` + `ptM_clamp`。 |
| `backwardnode` | **exact** | 三库逐语句相同，只差前缀、节点类型、`N_cc`。 |
| `leaf` | domain | [L] 行内 / 跨行 / 虚拟尾；[S] span 部分删 / 整段删 + seamleaf；[P] hole / literal split + forwardoff。 |
| `checkstitch` | optional | 只有 [L] 有，服务 `lc_append` 的预 reserve；[S]/[P] 批量插入不分配中间节点链。 |
| `balanceleaf` | domain / optional | [L] 独立叶 / 行长数组 / 父 breaks 占用率专有。 |
| `foldleaf` | domain / optional | 同上；[S]/[P] 无独立叶，fold 在 `foldnode` 叶层处理。 |
| `addbytes` | domain / optional | [L] 行长度 + 虚拟尾专有；[S]/[P] 无行长度 / 虚拟尾。 |
| `cutpiece` | domain / optional | [P] literal/hole 异质叶专有。 |
| `H_remove` | domain / optional | [P] hole 可变数据专有。 |
| `M_up` | skeleton | [L] 双通道 `bytes/breaks`；[S] 单 bytes + mask OR；[P] 单 bytes + hole remask，含 `db==0` 纯 mask 传播。 |
| `N_copy` / `N_move` | skeleton | [L] children/bytes/breaks；[S] children/bytes/mask 数组；[P] children/bytes + mask 位段。 |
| `N_makespace` | skeleton | 形状相同；[P] mask 位段随 move 维护。 |
| `N_purge` / `N_remove` | skeleton / domain | [L] 释放叶 / 节点；[S] 叶槽 `spA_died`；[P] 只释放 hole 和 `version` 匹配节点 + `ptM_clamp`。 |
| `seamleaf` | domain | [S] 同 id；[P] 相邻物理连续 literal。 |
| `diverlevel` | interface / optional | [S] 独立 helper（`spD_remove`、`sp_fill` 两处调用者）；[L]/[P] 各只有一处调用者，内联。 |
| `sumbytes` / `sumbreaks` | exact（前缀差异） | 区间求和；[L] 多 leaf / breaks 两版；[S]/[P] 只 node bytes。 |
| `P_reserve` / `P_alloc` / `P_free` | exact（前缀差异） | pool freelist 形状相同；三库 `P_init` 都断言 `obj_size < PAGE_SIZE / 2`。 |
| `K_parent` / `K_idx` / `K_leaf` | domain | `parent/idx` 公式相同；[L] 有 `K_leaf`；[S] 用 `spL_id`；[P] 用 `ptN_lit` / `ptN_hole`。 |

**严格 exact-sync 清单**

1. `backwardnode`：三库逐语句相同。
2. `rebalance`：三库逐语句相同（签名 `rebalance(C)`、内部 `l = levels - 1`、尾块 `if (levels > 0)`、末尾 `shrinkroot(C)`）。
3. `shrinkroot`：三库同名同形，调用点表见 §3.11。

**骨架级同步（公式 / 控制流可复制，领域通道按各库保留）**

- `cutrange` 的循环形状。
- `balancenode` 的搬移公式与 `d` 取整。
- `stitchnode` 的外层 `k/m/findroom/fold/backward` 形状。
- `M_up`、`N_copy/move/makespace/remove` 的区间搬移形状。
- `findroom` 的 [L]/[S] 尾部 children 搬移形状。

---

## 5. 当前文本层差异

> 本节列出统一库前缀 / 类型名后仍然存在的文本层差异。已并入 §2 / §3 / §4 描述的项不再单列。

### 5.1 决定保留的差异（领域 / 精确传参）

| 编号 | 项 | 当前差异 | 决定理由 |
|---|---|---|---|
| D3 | `makechain` 的 `nofail` | `[L]`/`[P]` 含 `nofail` / reserve；`[S]` 签名使用调用方入口 reserve。 | 复用批量插入 / compact 路径；`[S]` 的那条路径由入口 reserve 覆盖。 |
| D4 | `findroom` 签名 | `[L]` `(C, l, nofail) -> int`；`[S]` `(C, l) -> int`；`[P]` `(C, l) -> void`。 | `[S]` 的唯一调用者 `stitchnode` 已由入口 reserve 覆盖；`[P]` 的单调用者契约由 assert 证明。 |
| D6 | 第一差异层 helper | `[S]` 有独立 `diverlevel`；`[L]` / `[P]` 在 `remove` 路径内联（`[L]` `lcD_remove`、`[P]` `pt_remove`）。 | `[S]` 有两处调用者（`spD_remove`、`sp_fill`），抽 helper 消除重复；`[L]` / `[P]` 各只有一处。 |
| D9 | `balancenode` 的 delta 形状 | `[L]` `Delta ds[2]`（bytes + breaks）；`[S]` / `[P]` `Delta *ds`。 | `[L]` 的第二通道由行语义使用；签名差异保留。 |
| D10 | `M_up` 早退 | `[L]` 两通道皆 0 时返回；`[S]` 每层重算 mask；`[P]` mask 保持原值且 `db == 0` 时返回。 | 三库 mask 语义不同，早退条件随之不同。 |
| D12 | `N_remove` / `N_purge` 首参 | `[L]`/`[P]` 传 `*_State *`；`[S]` 传 `sp_Tree *`。 | **决定项（精确传参）**：函数只接收它真正需要的上下文——`[L]`/`[P]` 要回池释放节点 / hole，需要 `State`；`[S]` 的叶槽只存 `sp_Id`，死亡通知必须经树。不做统一。 |
| D13 | `mergeleaf` 返回类型 | `[L]` 返回行 `d`；`[S]` 返回 void；`[P]` 返回字节 `d`。 | 各自叶模型的定位语义不同。 |

## 6. 研究范围

- 覆盖三库的 `/* remove */` 段（`[L]` `linecache.h:446`、`[S]` `spantree.h:433`、`[P]` `piecetab.h:719`）到文件末尾 `/* APIs */` 段的 remove / stitch 及直接依赖原语；插入侧只取入口与段机器的分工（§3.21）。
- 行号基准：§0.1 的基准文件；行号漂移时按函数名定位。
