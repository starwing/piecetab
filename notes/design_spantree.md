# spantree 设计（讨论决议）

> 源自 marktree 调研讨论，背景分析见 `notes/research_marktree.md`
> （尤其第六节定位方式对比、第七节 extmark 模型）。
> 状态：**arbiter 单层模型已定案**（2026-08-12：方案 E 取代层向量
> 方案 A，论证见 §六）；开放问题已决议（§十）；attr 不透明 id 与
> sc 分工见 §6.5；**ns 标记与按 ns 操作定案见 §九**（2026-08-15 初
> 案：第三度量通道 + 剪枝下降；2026-08-15 三轮审核修订：arb 出参
> mask + sp_addns/sp_delns 封装，fill 签名不变——定案 v3；
> 2026-08-15 四轮审核修订：spM_up 统一 remask（ptM_up 同构）、
> 维护清单补全（makechain/stitchnode/merge 祖先链）、树内 ns 域
> [1..SP_MASK_BITS] + op 载荷 ns 无限（§8.2 同步修订）、
> sp_clear 批量剪枝清除（§9.4a，ptC_freeze 稀疏同构）、
> filtered next inclusive 语义——定案 v4）；
> 2026-08-16 五轮审核修订：filtered next 落点由"匹配段尾"改为
> "下一段头"（变体 B——落段尾破坏 peek 配对与导航层不变式，
> 论证与实测见 §9.2、notes/reports/audit_pieceend_pt.md；
> 同轮：pt_checkcursor 补禁树中段尾校验（lc 同款移植）、
> ptD_rmleaf 裂段光标逃逸修复）——定案 v5。
> 2026-08-16 六轮审核修订：变体 B 回滚——piecetab 侧实测使用方
> 未简化（pt_read/ptZ_append 反而更复杂），审计结论"exclusive
> 才是使用方使用最简的形式"；sp_next 定案 exclusive（pt_next
> 逐行同构），消费循环 = style 预查 + next 剪枝步进；树内严禁
> 段尾态纪律恢复（sp_checkcursor 禁则 + 编辑函数出口 forwardoff(0)
> 收尾）——
> 定案 v6。
> 2026-08-17 七轮审核修订：**规范形放宽定案（用户）**——全局
> "相邻同 id 段必须合并"改为"**节点内必须合并，节点边界允许不
> 合并**"（论证与代价见 §6.8）。跨容器合并义务整体废弃：remove
> 的 spD_mergeleft/dropleftchain/foldbelow 删除（空容器直接挂回
> rt[0]）、fill/insert 的 spI_mergeleft/mergeright/foldchain 删除
> （同容器合并保留）。fill 密铺论证不受影响（append merge 是节点
> 内操作，论证 1 精化后仍成立）。§6.7、落地细节 8 转存档。
> 实施计划见 plans/plan_spantree_ns.md。
> 2026-08-18 八轮实施落地：**叶容器内合并机制统一为 piecetab 同构**
> ——新增 spK_seamleaf（叶内相邻同 id 合并，cursor helper）与
> spD_seambound（foldnode 融合决策前预合并接缝 + 父指标 SEAM 同步），
> spD_foldnode 按 piecetab 逻辑重写；spD_stitch 重写；spI_fillrt
> 内合并 rt（动态 need）+ splitins/filterleaf/
> cutpiece 尾部 seamleaf；删除 spD_foldleft/foldright/foldbelow/
> dropleftchain/mergeleft 与 spI_foldchain/neighbor/absorbleft/
> remaskleft/mergeleft/absorbright/remaskright/mergeright/merge、
> spC_peekright。测试：serialtree/mcompare 合并跨容器待合并段后与
> 模型比对（用户语义流）；fuzz 1M ops 全绿（seed 1/2/3/5/7 + ASan）。
>
> 设计方法论：**API 语义先行，数据结构反推**。边界语义单方向决定结构
> 可行域——字节缝语义（区间可为 0）排除 B+ 计量树；字节覆盖语义
> （L>0）排除 B 树点标记。本文档所有结构决策皆由 API 语义倒推。
>
> Lua 绑定与 editor 接入定案见 `notes/design_spantree_lua.md`
> （2026-08-14：compositor C 化 + arbiter 语义 + API 摩擦检查结论
> spantree.h 零改动）。

## 一、定位

spantree 是**渲染染色最终结果的存储**（帧缓冲性质）：

- 标记不在字节缝，而是**覆盖字节，作为字节属性存在**（run/span 模型，
  先例：Emacs intervals、Quill Delta、Word run）
- `[AAABBBAAA]` 存为**三个段**（A 段、B 段、A 段），不是"一段 A 上叠
  一个 B"
- 可重建——大不了全量重染，正确性兜底简单
- 对 vim-like（计划扩展 sam 语义）编辑器，"字节属性"是自然抽象；
  "字节缝"模型（extmark 式）反而别扭

与 neovim extmark 的根本分工差异：extmark 存原始标记点集、渲染端逐列
合成；spantree 存合成后结果、渲染端零合成，合成上移到染色写入方。

## 二、模型

全覆盖 partition：文本被划分为连续段，每段 `(len > 0, attr)`。

**与 piecetab 同构（不计 COW）**——"piece 加属性、不持内容"：

- piecetab piece = `(len, src)`（字节块引用内容）；spantree 段 =
  `(len, sp_Id)`（字节块引用 attr，arbiter 单层，见第六节）——形态
  一一对应，唯内容引用换属性 id
- **分裂**：`ptI_splitins`（字节点裂 piece）↔ spantree 段分裂
  （sp_fill 边界 / splice 插入）
- **合并**：`ptD_mergeleaf` + stitch（相邻 piece 合并）↔ spantree
  节点内合并（单 id 比较，§6.8；跨容器不合并——规范形放宽定案）
- **字节度量**：两者都不管行，只管字节块；编辑平移 = 度量树前缀和
  天然免费（对比 marktree splice 每层改 O(2T) 个 key）
- 继承：B+ 骨架、池分配（pt_Pool）、splice、mergeleaf/stitch、scan
  批量加载
- **剥离**（piecetab 为 undo 快照而设，spantree 不需要）：
  COW（version/max_version/ptK_cow）、hole/mask 空洞、arena 数据块、
  内容引用——spantree 段无内容，纯度量+属性
- 文本删除 = 段缩短/消失，**无批量删问题**；稀疏染色 = 大 id 0 段
  天然表达（§四）

## 三、无 gravity：操作决定重力

gravity 在点模型中存在，是因为插入方不知道缝里有什么标记，粘附决策只能
预存在标记上。span 模型把**决策从数据搬到操作**：

| 操作 | 属性语义 |
|---|---|
| 段内插入 | 自动继承所在段属性（天然；不继承反而难做） |
| `append`（边界） | 继承**左**段属性 |
| `insert`（边界） | 继承**右**段属性 |
| `remove` | 只缩段不扩段，**无左右歧义** |

- append/insert 与 vim `a`/`i`、sam 语义同构——抽象与产品语义一致
- **undo 无歧义**：append/insert 的逆操作是 remove，remove 不存在扩段
  选择，undo 链**无需存方向信息**
- 突变（如关键字中插字破坏语法）：上层介入重染整段，spantree 只做机械
  继承。注意这不是 span 模型的额外成本——extmark 做语法高亮同样要上层
  维护（static 中插 x，"s 前 c 后"两个点照样要上层处理）

## 四、部分结果存储

- 稀疏染色**不需要树级偏移机制**（2026-08-12 修正）：未染色 = 大 id 0
  段，天然表达——4GB 文件仅染 1 字节 = `[len 4GB id0 段][len 1 染色
  段]` 两段即可，无需特殊机制（曾拟"树级整体偏移"，用户否决：纯
  冗余）
- "未染色"作为属性值（id 0），在全覆盖模型内自然表达

## 五、明确不做（有意取舍的表达力边界）

| 不做 | 理由 / 归属 |
|---|---|
| 标记身份（按 id 删改查） | 段是染色结果非对象；归身份层 |
| id→node 反查（id2node） | 前提是 node 有 parent 指针（反查后须上行重建路径）；linecache 骨架无 parent，加哈希表也无用 |
| 0 长度标记（书签/锚点） | L>0 是模型公理；归身份层 |
| 同写者覆盖可逆 | 同写者写入即覆盖（分支替换），有损是固有语义；**跨写者**恢复由 arbiter 无损合成天然支持（§6.3） |
| 层/混合器内建于树 | 写者/仲裁语义全部外置（arbiter 回调 + id 空间），树零格式知识（§6.5） |
| 树内 parent 指针 | 当前无需求方（区间外包已否决、intern 外包位置无关）。逃生通道：改 `lcN_parent` + 结构操作全面维护即可加，但侵入面广（split/balance/fold/stitch 处处修正，参照 marktree），无 parent 跑通则有 parent 只会更简单 |

## 六、模型定案：arbiter 单层（2026-08-12 推翻层向量方案 A）

### 6.1 为什么不是层：两个独立论证

**论证一（层合并困难，用户动机）**：多层模型要"精确地每层相等才能
合并"（规范形）——k 槽整向量比较、覆写只改一层的合并陷阱，段分裂
要复制 k 槽。层数越多，段操作越贵。

**论证二（插件痕迹需求，第一性原理）**：插件写 span 的核心诉求——
"写是稳定的，不会被轻易覆盖；至少 sc 层不会被完全淹没；即使所有
attribute 都被其他写者覆盖，也该有【我写过】的痕迹"。而记忆型插件
（一次性写入）无重算能力、无地方存 offset——**痕迹必须由 spantree
保管**。曾据此推断"层必需"，后经 arbiter 机制推翻（§6.2 方案 E）：
痕迹可编码进合成值链，无需独立槽。

### 6.2 方案空间探索（记录，防重议）

| 方案 | 概述 | 结论 |
|---|---|---|
| 多树 | 每层一棵薄 spantree + 合成函数 + 最终树缓存 | 位置空间只有一份，k 棵树冗余存 k 份位置结构、splice k+1 次。降级为插件便利库候选（保底） |
| ~~A. 单树层向量~~ | 段属性 = `attr[SPAN_LAYERS]`，读端 fold | **被 E 取代（2026-08-12）**：层合并困难（§6.1）+ arbiter 无损合成（方案 E）表达力 ≥ 层 |
| B. intern 精化 | 层向量 hash-cons 成小整数 id | 已被 E 吸收（id 空间即 intern） |
| C. 拉模式 | 写者注册回调，重合成时向源拉取 | 思想吸收进 E 的 operator：清除 = 挥发 id 元操作，恢复 = 源重染 |
| D. 区间外包 | 树内存 id，树外按区间存层数据 | **否决**：外部数据须索引回位置，offset 会漂 |
| **E. arbiter 单层** | 树段 = `(len, sp_Id)`；写入经 arbiter 回调 `(ud, old, in) → merged`；id 空间 = attr 值 ∪ 操作符 | **定案（2026-08-12）**。层数/写者语义完全外置（sc），spantree 零格式知识；数学完备性见 §6.3 |
| piece 携带属性 | 染色存进 piece table | **否决**：piece 边界由编辑历史决定，染色边界由语法决定，两种边界不同源，强耦合互相污染 |

### 6.3 arbiter 模型与数学完备性

**机制**：
- 段 = `(len, sp_Id)`——单 id，无层槽
- 写入唯一原语 `sp_fill(C, id, len)`：对区间内每段调
  `arbiter(ud, id, 段当前 id, &mask)`，写返回 id 与 mask（0 = 清段，
  arbiter 可自行调 id finalizer；**0 也进 arbiter**，spantree 不特判）
- **2026-08-16 修订（id 生命周期，§9.7 定案）**：arb 回调升级为
  **三态契约**——`arb(0, old)` = 死亡事件（合并/删除/freetree 由
  树补发；真清写同形）、`arb(in, 0)` = 新生事件（写空段/复制/
  继承，**必须原样返回 in**）、`arb(in, old)` = 合并决策（出口
  双向计数：ret != old → old-- / ret != 0 → ret++；filterleaf
  以保护 +1 使出口 −1 永不提前归零，见 §9.7）。refcnt 依据 =
  report_sp_idref.md 事件矩阵审计。
- id 空间 = attr 编码 ∪ **operator 编码**（写者/操作/ns 可编码进
  id：如负数或大 id 区间；挥发 id 用后清空 intern 映射）——
  **2026-08-15 修订**：ns 不进 id，走叶 mask 并排数组（第三度量
  通道），arb 经 sp_addns/sp_delns 报告结果 ns 集（§九 v3）
- 合成 = 无损结构：`merged = {parent = {w: old_contrib, ...}}`（写者
  标签 + 历史树，存 sc 的 intern 表，**树完全无感**）

**数学论证（覆盖/撤销/恢复全部可表达）**：
- 无损 parent 链 = 表达能力 ≥ 任意层模型——层只是"标签放树段里"的
  特例；arbiter 把标签放 id 结构里（sc 侧），树保持单槽
- **覆盖** = 同写者分支替换（`{P:X, E:E1}` + P 写 X' → `{P:X', E:E1}`）
- **撤销** = 元操作：清 E → `fill(op_clearE)` → arbiter 从 parent 链
  移除 E 分支 → 写回（op 为挥发 id，事后释放，树中无悬挂）
- **全清** = `fill(0)` → arbiter 自行决定（可调 finalizer）→ 返回 0
  → 写 0
- **交错撤销**（P/E/Q 任意序）全可行——parent 树支持任意分支操作
- 编辑漂移 = 段平移（位置+值一起走）；编辑删除 = 认（段消失）
- 恢复 = 源重染（§七 request）或元操作——两者皆备

**痕迹保证（§6.1 论证二）**：old 参与每次合成——写者未覆盖的字段
透传（痕迹在合成值链里）；arbiter 可自定义编码（ud 上下文）——"被
覆盖但未被淹没"。

### 6.4 快层与身份层

- **快层（光标/选区/光标行）**：不进树，渲染端每帧直叠（同前）
- **身份层**：书签/诊断对象等独立生命周期结构，职责 = 对象生命周期
  管理 + 向 spantree 输出染色（经 arbiter）——不变

### 6.5 不透明 id 与 sc 分工（2026-08-12 定案）

**树段存不透明 sp_Id（上层编码产物），非字段化 attr**——cellgrid
style id 同款抽象：cellgrid 是最终消费者但不管 attr 格式，spantree
同构。

- **sc 分工**：值域持久（intern 表：id → attr/operator，id 永不回收
  除显式释放）+ 仲裁（arbiter 实现：合成/分支替换/移除/finalizer）
- **spantree 分工**：位置域持久（offset + 单 id，编辑平移/删除）——
  "替写者管理每次写的 offset，不挥发"
- id 0 = 未染色（对齐 sc style 0 = 空 attr 预置）
- 节点内合并 = 单 id 整数比较（**层模型的整向量比较陷阱消失**；
  跨容器不合并，§6.8）
- attr/operator 格式演进零波及 spantree

### 6.6 区域运算：sp_fill 算法定案（v8/v9 历史 + v10 精炼实现）

**独有操作**：编辑同步 splice（度量变化，piecetab edit/remove 对位）；
染色写入 sp_fill（度量不变、单 id 覆写 + arbiter）是其他 B+ 树
（linecache/piecetab/marktree）都没有的——linecache 行不可分裂
覆写，piecetab 无属性概念，marktree 是点标记非区间覆盖。

#### 迭代史（每 v 一句话）

- **v1**（2026-08-12）：定位→边界分裂→覆写→3 检查点——逐段遍历
  低效、反复 seek、缩根、OOM，弃
- **v2**（2026-08-12）：cutrange 变体摘区间 → mt + mergek 洋葱合并
  ——批量摘取与批量合并机制多边界度量失配 + mt 槽位冲突（b4 bug 7），
  弃
- **v3**（2026-08-12）：现场剥洋葱——L 右/中间/R 左分阶段摘一层剥
  一层，单 rt 数组无 mt。**硬伤**（2026-08-13 发现）：边界裂段在剥
  回时 +1/+2 段，满树时剥回 append 溢出 → makeroom 造链挂 fl 层尾
  （R 槽后）→ 越过 R 错序；裂段前置修复有效但三段操作（splitseg
  memmove + memcpy 进 rt + merge 回来）不优雅；标准分裂兜底需跨
  函数识别最坏情况（makeroom 填满 vs 二分混用）
- **v4**（2026-08-13）：**边界裂后置**——摘挂只处理整段们（边界
  两半不摘不裂），严格段数守恒 → 剥回永不溢出；裂推迟到 stitch
  后安全区执行。**表述缺陷**（2026-08-13 用户纠正）：① 摘挂切
  fl 层必须 **[iL+1..cc] 全切**（mid + R + R 右一起进 rt）——只切
  mid 的提法使"R 让位"论证缺位（链在 fl 层需要 R 让出的槽位）；
  ② "三阶段摘挂"把 mid 与 R 左分成两阶段，实际是同构的**前缀
  剥填**递归
- **v5 前缀剥填**（2026-08-13，定案）：fl 层 [iL+1..cc] 全切进
  rt[levels-fl] + **前缀剥填**递归（统一 mid 与 R 左：每层剥
  `[前缀][R][后缀]` 的前缀 → R 前移 → R 的孩子下放下一层 rt →
  递归至叶）+ stitch 挂回 R 右森林。密铺论证（lcB_append）保证
  摘挂阶段 fl 层不溢出
- **v6**（2026-08-13，审核修订）：删 R 路径空壳（挂回 = 逆操作，
  stitch 无溢出）；append merge 含节点内同 id 合并（§6.8，密铺前提）；光标 =
  L 链 seek 到 off+len（R 废弃）；树尾 fill 的 0 长半段不保留
- **v7**（2026-08-13，审核修订）：阶段 1 仅当 fl < levels 执行
  （fl=levels 时 L 叶容器即 parent(fl)，切+剥会把 R 与 R 右整段
  重染）；后置裂 = 复用两次 filterleaf（绝对位置区域重染，兼容
  [L+链首]/[链尾+R] 合并段）；stitch 以链尾光标调用（lcB_append
  光标语义：assert i>=cc-1、结束后 paths 指向最后填的段）——
  mergeleaf 合并链尾与 R 段，字节序正确；findroom 放宽为"第一个
  cc<FANOUT 的已切层"（挂载层右侧可能为链容器）
- **v8**（2026-08-14，用户纠正）：恢复虚拟 fill pad 语义（v7 误删
  "L/L+len 不越树尾"拒绝——与 #if 0 的 fill_virtual 历史测试冲突；
  教训：多次迭代会覆盖早期定案语义，重写文档须对照 #if 0 测试）：
  C 虚拟（sp_offset > bytes）→ pad [bytes, C) 为 id0（fill 区间外
  物化，不通知 arbiter）；R = seek(off0+len)（**不可用 advance**：
  空树 advance no-op）——R 虚拟（> bytes）→ pad [bytes, off0+len)
  （末字节 off0+len-1，pad 不含 R 位置）+ 调一次 arb(0,0) 作 pad
  notice（返回值弃用）；R == bytes 不 pad（仅 locend 修正 poff）；
  pad R 动树后 seek C 回 off0；spI_pad 属 fill 入口前置，豁免
  filterrange 禁 spI_ 铁律
- **v9**（2026-08-20，用户定案）：**彻底去掉 R pad 与 fill 内
  locate**。fill 只允许补 L（filtervirtual：若 C 在树尾/虚拟，先
  pad L，再插入 id+len 段返回）；R 虚拟不再物化，而是把 R 所在末叶
  自然延长到 R->poff（同步祖先 bytes 与 tree->bytes），使 R 成为
  实树尾；filterrange 只接受 **L 已是整叶开头（L->poff == 0）**，
  内部用“剥填 L 右 → 前缀剥填 → stitch R 右”三步，结束时 L 落在
  原 R 段起点；最后对 R 段做一次 filterleaf(pr)。因此 filterrange
  自身负责最终光标位置，不再需要外部 locate。
- **v10**（2026-08-22，用户精炼实现）：把 v9 落地为 spantree.h 当前
  实现，并删去中间层 `spF_fillto`/`spF_fillrange`/`spF_splitleaf`。
  算法仍是三步（剥填 L 右 / 前缀剥填 / 缝回 R 右），只是第二步的
  代码已合并进 `spF_filterrange` 本体，不再以独立 `spF_fillrange`
  伪码呈现。`sp_fill` 只做薄分流：虚拟/树尾 → `spF_appendvirt`；
  当前段内 → `spF_filterleaf`；否则 `sp_seek` 构造 R 后直接
  `spF_filterrange`。`spF_filterrange` 内部实现为 clear rt →
  `spF_cutleaf` →（fl<levels 时）`spF_cutright` → 逐层 `spF_peel`
  （k=0..levels-fl-1）→ `spF_peeldown` → `spF_peelleaf` →
  `spD_stitch`。`spF_peel` 带 `keep` 参数，`spF_peelleaf` 用
  `keep=pr<n` 避免反复 makespace；R 段落在虚拟区时以 zero-id 虚拟段
  剥填，保证 arbiter 看到 old=0。

#### 限制条件（铁律）

1. **rt 单数组**：只用 `S->rt`（`sp_Node *rt = S->rt`），禁本地 rt
   数组、禁 mt+rt 双数组
2. **filterrange 内禁 spI_ 系**：剥洋葱/摘挂/stitch 不得调
   spI_splitins/insertrt/splitchild/splitroot/fillrt（延后实现，
   不在 filterrange 范围）；spI_pad（fill 入口前置）与 filterleaf
   （单段处理）豁免
3. **批量**：摘挂层循环批量；禁止逐段 seek
4. **一次扫描**：区间内容经剥洋葱线性处理，无反复上下跳

#### 坐标系与术语（2026-08-13 用户定案，全算法统一用语）

- **层号**：`T->levels` = l 意味着树有 **l+2 层**——root = -1 层
  （嵌入 Tree，只能经 parent(0) 索引到）；层 0 节点 = root 的
  children；层 levels-1 = 叶容器层；**层 levels = 叶层**
  （`C->paths[levels]` 指向的槽实际存储 sp_Id，即叶）。层号变量
  用 **l**（与左游标 L 区分，同代码约定）
- **不变式**：`C->paths[level] == &parent(level)->children[i]`
  （level = 0..levels）——parent(l) = 层 l-1 节点（l > 0 时 =
  `*paths[l-1]`；l = 0 时 = `&T->root`）
- **叶**：parent(levels)->children 里被 cast 成 `sp_Node *` 的
  sp_Id；其长度 = parent(levels)->bytes[i]
- **叶容器**：parent(levels)（层 levels-1 节点）——其每个孩子都是
  一个叶，故名
- **中间节点**：层 l（l < levels-1）的节点（children 是更低层节点
  指针）
- **根**：嵌入 Tree 的 -1 层；其 children 里的节点是 0 层节点
- **分歧层 fl**：splitpaths 返回的最小 l，使 `L->paths[l] !=
  R->paths[l]`（paths[0..fl-1] 相同）——L、R 位于 parent(fl)
  （层 fl-1 节点）的不同槽
- **fl 层**：parent(fl) 的 children 序列 =
  `[L左...][L][mid...][R][R右...]`——L 左 = 范围外（不碰）；L =
  L 路径 fl 节点；mid = (L,R) 内内容（覆盖）；R = R 路径 fl 节点；
  R 右 = 范围外（不碰）
- **rt[k] 层语义**：rt[k] 装**层 levels-k** 的内容——rt[0] = 叶们
  （层 levels），rt[1] = 叶容器们（层 levels-1），rt[levels-fl] =
  fl 层节点们。摘取/驻留必须按层落槽，禁止跨层混放
- 不变式（既有）：**除非 Cursor 指向整树结尾，否则 Cursor 不指叶尾**
  （findleaf 段尾前进到下一段；locend 树尾态 poff=末段长）。
  **2026-08-16 v6 复核强化（exclusive 定案）**：导航层
  （seek/locate/advance/next/prev）全部落段头/段内/树尾，零产出
  树中段尾态——next 改 exclusive（跳过当前段落下一匹配段头）保
  住此不变式；编辑层逃逸（插入段尾/合并段尾落点）由各编辑函数
  出口 forwardoff(0) 收尾（树尾豁免；fill/append 逃逸实测，remove
  路径不逃逸）；sp_checkcursor 补禁则校验
  （pt/lc 同款：offset >= bytes 豁免，否则 poff < 段长）

#### 光标契约（2026-08-14 二轮审核定案）

**光标三态**：

| 态 | poff | off | paths |
|---|---|---|---|
| 正常态 | [0, 段长) | 段前字节 | 段槽 |
| 树尾态 | 末段长 | bytes - 末段长 | 末段槽 |
| 虚拟态 | ≥ 末段长 | bytes - 末段长 | 末段槽（树尾路径） |

- **虚拟态 = 树尾态 + 超出量入 poff**（2026-08-16 定案）：off 恒
  < bytes（i 恒有效），超出树尾的部分全进 poff（poff ≥ 段长）；
  linecache `lnu == breaks[i]` 同款。段内判定 = `poff < 段长` 单
  条件——off 项只剩空树守卫语义（bytes == 0 时 off == 0 == bytes）
- 虚拟态由 seek/locate/advance 越尾分支与 spI_pad 产生；**虚拟态回退
  入树须真实重定位**——树外字节无树结构可减，不能走 backwardoff
- 树尾态（poff == 末段长）与虚拟态（poff > 末段长）区分：前者是
  末段内位置，后者是树尾之外

**seek/locate/advance 调用清单（例外论证，禁其余调用点）**：

| 调用点 | 原语 | 理由 |
|---|---|---|
| sp_fill | sp_seek 构造 R | 构造区间右端；空树/树尾由 seek 落虚拟/树尾态 |
| spD_remove | sp_advance(&R, len) | R 为 C 的栈副本，构造区间右端的自然原语 |
| sp_clear | sp_seek(&C, T, 0) | 初始游标构造，非路径信息丢失 |

内部 Cursor helper 已全部改为相对维护：sp_prev 树头回退用
spK_lochead；spI_splitins 满容器分裂后光标本就在新段，不再 locate；
sp_insert 用 spK_backwardoff 回插入点；sp_clear 不重定位 Cursor——
从当前匹配段起批量处理所在容器，压缩后由 spC_clearnode 把 Cursor
放到容器末段，外层再用 sp_next(ns) 前进，全程无 locate/forwardoff/
locend。

**sp_advance 实现（2026-08-16 二轮修订：虚拟态统一，advance =
相对 seek）**：不再调 sp_locate（公共 API 互调 + 从头重扫）。
**空树不再 no-op**——no-op 是 piecetab 无虚拟态时代的结论（越尾夹树
尾，空树树尾 = 0）；spantree 越尾进虚拟态，advance(5) 空树 = 虚拟
offset 5，与 seek(5) 一致（pt 式 d==0 早退同删：d==0 全路径幂等）。
负向两分支 + 回绕比较：`-d > off` → backwardoff(off)（自然夹 0）；
否则 backwardoff(-d)——**虚拟起点经 backwardoff 天然完备**（poff 部
分覆盖"留虚拟/退入末段"，超出 poff 时目标 ≤ 末段首，爬层自 idx−1
起步正确），无需 findleaf 特判（初版三分支冗余）。向前越尾 →
locend + poff += (off+d − bytes)（超出量入 poff，虚拟态不夹）。

**内部函数光标契约**：

| 函数 | 前置 | 后置 |
|---|---|---|
| spK_findleaf(C, l, &poff) | paths[0..l-1] 有效、off = 前缀路程 | 完整 paths、poff 段内、off 段前 |
| spK_locend(C) | 树非空 | 树尾态 |
| spK_forwardoff / backwardoff | 正常态、目标在树内 | 正常态、前进/后退 d |
| spD_remove(C, len) | 任意 | 光标于删除点（offset 不变，段尾态已收尾） |
| spI_insert(C, ins, useleft) | 光标于插入点（正常/树尾/虚拟三态；树中段尾态 assert 拒绝——inherit 只处理段头/段内/树尾） | 插入段尾（poff=ins，恒；useleft 只定继承方向；append 出口 forwardoff(0) 收尾，insert 经 advance 回插入点） |
| spI_splitins(C, len, id) | 光标于插入点 | 插入段尾（poff = len） |
| spK_seamleaf(C, right) | 任意 | 叶内相邻同 id 段已合并（i = 光标槽 + right；合并后 cursor 在合并段内正确位置） |
| spI_pad(C) | 任意 | 树尾态（offset 不变） |
| spF_filterleaf(C, len, in, right) | 区间头 | 区间尾（单出口；非树尾时已收尾 poff < 段长） |
| spF_filterrange(C, R, fl, in) | L 可含半叶（内部 cutleaf 切整）、R 实光标、fl=spD_diverlevel(L,R) | L 落在 fill 尾（R 右半已由 peelleaf 处理） |
| spF_appendrt(C, rt) | C->poff == 0、rt 为待落段 | 段已落回、C 在插入内容尾 |
| spF_appendvirt(C, id, len) | 树尾/虚拟态 | fill 段尾（off = 段起点、poff = len） |
| spF_appendspan(C, id, len) | 链尾段（assert i ≥ cc-1） | 链尾（随 append 推进，落地细节 4） |
| spD_stitch(L, rt) | 链尾段（mergeleaf 前提，落地细节 2） | L 位置保持 |

#### 机制一：剥洋葱原语

对 rt 数组的循环处理（"rt[0] 有数据就 arb+append merge，没数据就
往上找有数据的层，将 children 剥到 k-1 层，一次一次剥到叶，循环
直到 rt 数组为空"）：

```
peel(rt):
  loop:
    if rt[0] 有数据（叶们）:
      逐段 arb -> append merge 回 L 链（arb 只对叶调用）
      （叶们剥完 rt[0] 清空）
    else:
      自底向上找第一个有节点的层 k（k > 0）
      将 rt[k] 的一个节点的 children 下放到 rt[k-1]
      下放后 free 该节点
      继续（rt[k-1] 有数据则再下放它的一个节点到 rt[k-2]，直到
      rt[0] 有数据）
    until 剥完目标内容（阶段 1：rt 数组空；前缀剥填：前缀剥完）
```

**容量保证**：rt[k] 每次只下放一个节点的 children（≤ FANOUT 个）
到空的 rt[k-1] → 每槽恒 ≤ FANOUT，剥完即清空。

#### 机制二：append merge（lcB_append 语义，密铺）

- append merge = **"先尽量填叶子，如果叶子满了，垂单链树，然后
  继续填叶子"**——从开始 append 的地方起没有任何空隙，会被填成
  满树（lcB_append 是 linecache 既有机制名，spantree 移植同语义
  的 spF_ 版）；**光标随 append 推至链尾**（lcB_append 语义：调用
  前提 = 光标在叶容器末段 assert i>=cc-1，结束后 paths 指向最后
  填的段）—— stitch 的调用位置（落地细节 2）
- "垂单链树" = makeroom/findroom 机制：从 L 叶容器向上找第一个
  "右侧有空间"的**已切层** → 该层起每层建一个新节点（单链）→
  新叶容器 → 继续填
- 满树情况下 append merge 是**摘 L 右的严格逆操作**（内容不变时
  节点数不变、L 恢复满树）
- findroom 断言的 fill 场景处理见"落地细节 1"

#### 机制三：前缀剥填（核心递归，统一 mid 与 R 左）

**前置条件**：rt[levels-l] 的内容 = `[前缀][R][后缀]`——
前缀 = 该层 R 左侧待剥内容（覆盖区间内）；R = R 路径节点（保留至
下放，其后空壳删除，见步骤 3）；后缀 = R 右侧内容（R 右森林
一部分，保留不剥）

```
peeldown(l, rt):                 # 层 l 的内容已在 rt[levels-l]  1. [剥前缀] 对前缀所有节点完整剥洋葱回填（arb + append merge 回 L 链）
              # 剥洋葱带 R 边界：每层只下放 R 路径左侧的节点
  2. [R 前移] memmove 让 R 到 children[0]（前缀剥完，槽压缩；后缀跟着前移）
  3. [下放]   R->children 全部放进 rt[levels-l-1]
              （R->paths[l+1] 同步更新为 &rt[levels-l-1]->children[i]）
              # 步骤 1 不碰 R 的孩子们（在 R 节点 children 内完好），
              # 下放无悬垂；下放后 R 空壳（cc=0）free——R 路径剥左后
              # 无内容，不进 R 右森林（挂回 = 逆操作无溢出，见论证 8）
  4. [新前缀] 通过 R->paths 得到新 rt 层的前缀：
              R->paths[l+1] 指向的槽之前的孩子们 = 新前缀（R 路径孩子的左侧）
  5. [递归]   对层 l+1、rt、新前缀递归调用 peeldown——直到叶层
  6. [叶层]   l = levels-1（R 叶容器下放后）或 fl = levels（R 直接是段）：
              剥 R->paths[levels] 前的叶们（arb + append merge 回 L 链）；
              R 段 + R 右段们（R 残留）留在 rt[0]
              （fl < levels 时经 R 叶容器 children 下放，叶容器空壳 free）
  终止：rt 中只剩后缀们（各层 R 的右兄弟节点们）+ R 残留段们（rt[0]）
```

#### sp_fill 完整流程（v8 前缀剥填 + 虚拟 pad）

```
sp_fill(C, id, len):
  1. [参数检查] C 有效；len == 0 -> SP_OK
  2. [pad C] C 虚拟（sp_offset > bytes）-> spI_pad(C) 物化
     [bytes, C) 为 id0 段（fill 区间外物化，不通知 arbiter）；
     C 落树尾，offset 不变（记 off0 = sp_offset(C)）
  3. [pad R] R = seek(off0 + len)（seek 构造 R：advance 亦可——空树
      no-op 理由已随 2026-08-16 advance 修订失效，seek 为 v8 流程
      保留）；R 虚拟（> bytes）-> spI_pad(R) 物化
     [bytes, off0+len)（= fill 区间内虚拟部分，末字节 off0+len-1，
     pad 不含 R 位置），随后调一次 arb(0,0) 作 pad notice（返回值
     弃用），seek C 回 off0（树结构已变）；
     R == bytes（树尾）不 pad，仅 locend 修正 poff
     # 空树 fill 走同路径：seek 到 off0+len 即虚拟态 -> pad R
     # onepiece 物化 -> R 落树尾，叶内 filterleaf
  4. [分流] fl = splitpaths(C, R)；fl > levels -> filterleaf，返回
  5. [边界记录] pl/bl（L 段）、pr/br（R 段）
     # nidL/nidR 不在此预算——阶段 5 的 filterleaf 内自算
     # （arb 每段恰好一次的契约不变，论证 5）
  6. [reserve] 4*levels+5（覆盖后置裂链）
  7. [阶段 1：L 右（>fl 层），全切再剥；仅当 fl < levels 执行]
     # fl = levels（L、R 同叶容器）时整体跳过——L 叶容器即
     # parent(fl)，阶段 1a 会连 [mid][R][R右] 一起切出并被阶段 1c
     # 整段剥掉重染（R 与 R 右不在覆盖区间内）
     a. 切 L 叶容器 children[i+1..cc)（L 段右侧叶们）-> rt[0]；cc = i+1
     b. 对 kl = levels-1 downto fl+1：
        切 parent(kl)（层 kl-1 节点）children[i+1..cc)
        -> rt[levels-kl]；cc = i+1
        # L 路径每层右兄弟全部切出（全切），槽位保留
     c. 剥洋葱原语（arb + append merge 回 L 链）直到 rt 空
        # 链挂载 = L 路径各层摘出槽（密铺：链层 kl <= 摘出层 kl，
        # 不突破 fl 层）
  8. [阶段 2+3：fl 层全切 + 前缀剥填]
     a. 切 parent(fl) children[iL+1..cc)
        （mid + R + R右 全切）-> rt[levels-fl]；cc = iL+1
        # 树上 fl 层 = [L...]；rt[levels-fl] = [mid...][R][R右...]
        # = [前缀][R][后缀] —— 前置条件满足
     b. 前缀剥填（层 l = fl，前缀 = mid）——递归到叶子
        （arb + append merge 回 L 链）
        # 链继续挂 fl 层 = mid/R 让出的槽位（密铺：链层 fl <=
        # mid + R 原槽位）
  9. [阶段 4：stitch]
     rt 中只剩后缀们（各层 R 的右兄弟节点们）+ R 残留段们（rt[0]）
     -> stitch 挂回 fl 层 L 链之后
     # 挂回 = 摘出的逆操作（论证 8）：各层空槽恰好够，无溢出无扩根；
     # stitch 是成熟机制（piecetab/linecache 广泛使用），保 Cursor 位置；
     # 以链尾光标调用（spF_append 已推至链尾）：mergeleaf 合并链尾
     # 段与 R 段（R 紧随链尾），字节序正确（落地细节 2）
  10. [阶段 5：后置裂 = 两次 filterleaf 复用]
     a. seek(off + pl) + filterleaf(bl - pl)   # 重染 [off+pl, off+bl) → nidL
     b. seek(off + len - pr) + filterleaf(pr)  # 重染 [off+len-pr, off+len) → nidR
     # filterleaf 按绝对位置定位（裂左边界 poff>0、裂右边界
     # poff<段长）：区间端落在段内则裂，段界/树尾处天然退化；
     # 即使边界段被同 id 合并（[L+链首] / [链尾+R]），绝对位置
     # 定位与重染区域仍正确（合并段 id 未变，nid = arb(sid, in)
     # 在 filterleaf 内对当前段自算）；树尾 fill 时右边界
     # poff == 段长不裂，0 长半段不产生
   11. 返回——光标定位到 fill 末尾 = 步骤 10b 的 filterleaf 自然结果
       （其内部两次 seamleaf 按"光标=插入段尾"不变量收尾，append
       语义；R Cursor 在前缀剥填后废弃，见落地细节 3）
```

#### v9/v10 流程（v9 定案，v10 当前实现：无 R pad + filterrange 整叶/半叶开头）

本版取代 v8 的“pad R + seek C 回 off0 + 后置 locate”流程。fill
结束时 Cursor 必须落在 fill 尾；这个职责由 filterrange 自身保证，
外部不再用 `sp_locate` / `sp_seek` 重建路径。v10 把 v9 落地为当前
spantree.h 实现：删去 `spF_fillto` / `spF_fillrange` / `spF_splitleaf`，
并把“切 L”并入 `spF_filterrange` 本体（`spF_cutleaf` 直接处理
`C->poff > 0`）。

```
sp_fill(C, id, len):
  1. [参数检查] C 有效；len == 0 -> SP_OK
  2. [reserve] spP_reserve(6 * levels + 7)
  3. [filtervirtual] 若 sp_offset(C) >= spK_bytes(C)：
       spF_appendvirt(C, id, len)；返回
  4. [当前段内] i = 当前叶槽；若 len < p->bytes[i] - C->poff：
       spF_filterleaf(C, len, id, 1)；返回
  5. [跨段] r = sp_seek(&R, T, sp_offset(C) + len)；assert ok
       fl = spD_diverlevel(C, &R)；若 fl > levels 则 fl = levels
  6. [filterrange] spF_filterrange(C, R, fl, id)；返回
     —— C 已在 fill 尾
```

##### filterrange 算法（三步；v10 实现映射）

算法仍是 v9 的三步（剥填 L 右 / 前缀剥填 / 缝回 R 右），只是第二步
已合并进 `spF_filterrange` 本体，不再以独立 `spF_fillrange` 伪码
呈现。前置条件：

- R 是实光标（跨段 fill 由 `sp_seek` 构造；虚拟尾由 `sp_fill` 提前走
  `spF_appendvirt`）；
- `fl = spD_diverlevel(L, R)`，且 `fl <= levels`；
- L 可处于叶内任意位置（`C->poff` 可 > 0）；`spF_cutleaf` 负责把 L
  右半段切进 rt，因此调用方不再需要先切 L。

```
spF_filterrange(L, R, fl, in):
  1. 剥填 L 右
     a. 清空 rt（所有层 cc = 0）
     b. spF_cutleaf：若 L->poff > 0，把当前叶的右半段（原 id/mask）
        放入 rt[0]，L 叶只留左半段，并前进到整叶开头
     c. 若 fl < levels：spF_cutright 把 L 路径 fl 层到叶容器层（含 fl）
        各层右兄弟全切进对应 rt 层，并 spM_up 记账
     d. 对 k = 0..levels-fl-1 中非空 rt[k] 整层调 spF_peel(C, in, cc, k, 0)
        剥回 L 链（arb + append merge；节点内同 id 合并）
     结束时 L 的 fl 层 child 成为已处理左前缀。

  2. 前缀剥填（核心；v10 已合并进本体）
     fl 层当前形态：
       [L 左...][L][mid...][R][R右...]
     a. 步骤 1c 已把 [mid...][R][R右...] 放入 rt[levels-fl]
     b. spF_peeldown(C, R, in, fl)：从 fl 层起把 R 左前缀逐层剥填回
        L；每层把 R 的 children 下放到下一层 rt、删除 R 空壳，
        R->paths 同步更新；循环到叶层（levels）后停，在叶层先剥
        R 左叶子
     c. spF_peelleaf(C, R, in)：处理 R 段本身（pr = R->poff）：
        若 pr < n 先剥真实段再恢复后缀（keep=pr<n 避免反复
        makespace）；若 pr > n 表示 R 落在虚拟区，以 zero-id 虚拟段
        剥填，保证 arbiter 看到 old=0
     结束时 rt 中只剩 R 右。

  3. 缝回 R 右
     调用 spD_stitch(C, rt) 把 rt 中剩余 R 右缝回 L。
     结束后 L 已落在 fill 尾（原 R 段起点，R 右半已由 peelleaf 处理）。
```

##### 收尾

```
filterrange 后：
  L 已落在 fill 尾，不需要外部 locate / 二次 filterleaf；
  R->poff（pr）已在步骤 2c 由 spF_peelleaf 消费；
  虚拟尾段由 spF_appendvirt 在入口处理，filterrange 内不 pad R。
```

##### 关键不变式

1. `filterrange` 的输入不再要求 `L->poff == 0`；`spF_cutleaf` 负责
   把 L 右半切进 rt，调用方（sp_fill）直接进入跨段分流。
2. `filterrange` 返回时 L 落在原 R 段起点 / fill 尾，这是
   `spF_peelleaf` + `spD_stitch` 的自然结果；若现有 stitch 不满足，
   应修 stitch/peel 的光标推进，而不是在外部 locate。
3. 全程无 R pad：虚拟尾段由 `spF_appendvirt` 作为独立新段插入，
   不延长末叶，也不触发树结构分裂影响 L 的 paths。
4. 剥填阶段节点数净减，因此不需要 fl 层以下的 makechain/扩根。

##### refcount 写回规则

- `spF_filterleaf` 裂叶分支：若 `left > 0` 先 `spA_born(oid)` 复制左半
  引用；右半与 nid 段经 `spI_fillrt` / `spF_appendrt` 落回。
- `spF_cutleaf` / `spF_peelleaf` 的裂段同样先 `spA_born(oid)` 补一份
  引用（旧段一分为二，引用数 +1）。
- 合并两个同 id 段时（`spI_fillrt` 左邻合并 / `spF_appendspan` /
  `spF_appendvirt` 的 merge 分支）必须 `spA_died` 一次：两个引用变一个。
- 新段插入（`spF_appendvirt` / `spF_appendspan` 的新增分支）不额外调
  born/died；新 id 的引用由 `spA_born` / `spA_arb` 负责。
- `spF_append` 剥叶时逐段 `spA_arb(C, in, old, &m, 0)`，`!nid` 时
  `m = 0`；`spF_peelleaf` 虚拟分支用 zero-id 段使 arb 看到 old=0。

##### helper 抽象（实现映射）

- `spF_appendvirt(C, id, len)`：树尾/虚拟起点的 fill 入口（sp_fill
  步骤 3 直接调用；pad 内联处理，不单独 spI_pad）。
  内部：
  1. 构造 `rt`：若有 pad（start > old），先放 `(id=0, len=pad)`；
     再放 `(nid=spA_born(id), len=len)`；
  2. 经 `spI_fillrt` 放入 rt（左邻/rt 尾同 id 会合并并 `spA_died`）；
  3. 交给 `spF_appendrt`（单出口，满时 makeroom 再插）；
  4. 插入后 Cursor 指向 fill 段：`off = 段起点`，`poff = len`。
- `spF_filterleaf(C, len, in, right)`：单段染写。`right` 控制是否与右
  邻 `spK_seamleaf(C, 0)` 合并；裂叶分支构造 rt 后统一
  `spF_appendrt`。
- `spI_fillrt(C, id, len, m)`：向 `S->rt` 追加/合并一段。返回 0 表示
  合并或复用，1 表示新增槽。左邻合并必须 `C->off += len`。
- `spF_appendrt(C, rt)`：`assert(C->poff == 0)`；把 rt 内容落回 L 链
  （满时 `spI_makeroom` 再插），并推进 C 到插入内容尾。
- `spF_append(C, in, n, keep)` / `spF_flushrt(rt, n)`：剥叶回填；
  `keep` 为真时不 flush rt[0]，供 `spF_peelleaf` 保留后缀。
- `spF_peel(C, in, n, k, keep)`：剥洋葱。`k == 0` 时 `spF_append` 回填
  叶子；`k > 0` 时把 `rt[k]` 的 n 个节点 children 下放到 `rt[k-1]` 并
  free 空壳。
- `spF_peeldown(C, R, in, l)`：前缀剥填的下钻循环（fl 层到叶层）。
- `spF_peelleaf(C, R, in)`：处理 R 段本身，含虚拟区 zero-id 段。
- `spF_cutleaf(C, rt)` / `spF_cutright(C, rt, fl)`：切 L 右半 / 切 L
  路径各层右兄弟进 rt，并 spM_up 记账。
- `spF_filterrange(C, R, fl, in)`：三步总入口，内部完成上述 1/2/3。
- `spD_stitch`：负责缝回 R 右并保证 L 落在 fill 尾；若遇到 L
  underfill（如 L 正好 cc=1 且 R 为空），参考 linecache `lcD_stitch`
  在 stitch 后做 foldnode + rebalance，确保合法。

#### 核心论证（密铺，勿再违背）

1. **密铺（第一性原理）**：append merge（spF_ 版，lcB_append 语义）
   是密铺——从开始 append 的地方起没有任何空隙，会被填成满树。
   **严格化前提**：arb 只改 id 不改变段长（内容不变）+ append merge
   含**节点内同 id 合并**（§6.8 放宽后仍为义务，段数单调不增）→
   剥回段数 ≤ 摘出段数（合并只减不增，跨容器不合并不影响此式）
   → 叶容器数 ≤ 摘出叶容器数 → 链层 k 节点数 = ceil(叶容器数 /
   F^(levels-1-k))（密铺最小）≤ 摘出前层 k 节点数（合法树节点数 ≥
   密铺最小）→ **摘挂阶段每层节点数净减少（至少不增）**——剥洋葱
   会密铺 L 后 R 左所有节点，节点一定更紧凑
2. **fl 层不溢出**：链层 k ≤ 摘出层 k（每层，论证 1）→ 垂链
   findroom 总能找到已切层（链层 k < 摘出槽数）→ 摘挂阶段不可能
   要求 <fl 层的 makeroom——
   - 阶段 1：L 右剥回 = 摘的严格逆操作（内容不变时 L 恢复满树，
     节点数不变）
   - 阶段 2+3：**同一递归同一条链**（前缀剥填统一处理 mid 与 R 左，
     append 连续）——整体密铺：**链层 fl ≤ mid 原槽数 + R 原槽数**
     （R 左 ≤ R 子树内容 → 密铺 fl 层 ≤ 1 节点）；链层 k（fl<k<
     levels）超摘出槽时沿垂链机制**向上借槽到 fl 层**（fl 层切出槽
     是链的最终挂载空间）
3. **stitch 兜底**：fill 不依赖 stitch 的正确性承诺——stitch 有问
   题专项解决，不阻塞本算法；即使挂回触发 makeroom/扩根（R 残留挂
   叶容器层时链尾叶容器满），在 stitch 自身语义下结果正确，可容忍。
   论证 8 已证本算法输出 = 逆操作场景，无溢出
4. **严格段数守恒**（沿自 v4；§6.8 放宽后改为节点内守恒）：摘挂
   内容 = 整段们（边界两半不摘不裂）→ 剥回段数 ≤ 摘出段数（节点内
   合并保证，跨容器段原样摘原样回，段数不变）→ append 需求 ≤ 空槽
   → 剥回不溢出
5. **arb 契约**：合并决策对每段恰一次（剥洋葱 + 后置裂两次
   filterleaf 各算一次，nidL/nidR 在 filterleaf 内对当前段自
   算）；合并/死亡经 `arb(0, id)`、复制新生经 `arb(id, 0)` 事件
   调用（§9.7 三态契约，2026-08-16 修订）；pad notice（R 虚拟
   时 arb(0,0,&m) 恰一次，返回值与 m 弃用）是物化通知非染色写
   入，不计入段染色——fill_virtual 断言 arb_notifypad == 2 =
   pad notice + 物化段染色。§九 v3 起 arb 调用带
   `sp_Mask *mask` in/out（in = 段当前 mask；out = 新 id 的精
   确 ns 集，经 sp_addns/sp_delns 操作，树写回），id == 0 时树
   强制 mask = 0
6. **rt 容量**：剥洋葱逐节点下放（每次 ≤ FANOUT 到空槽）→ 每槽
   恒 ≤ FANOUT；摘挂阶段每层槽 ≤ FANOUT（兄弟数上限）
7. **复杂度**：O(定位 + 段数 × arbiter + 摘挂层循环)；一次扫描
   （剥洋葱线性）
8. **R 残留挂回 = 逆操作**（删空壳推论）：R 路径空壳全删（机制三
   步骤 3）→ 挂回内容 = 后缀们（各层，未动过）+ R 残留段们
   （rt[0]）——**R 边界裂不产生 +1 节点**：R 叶容器拆两半（Rseg 左
   侧段们合入剥回内容整体密铺，不单独成叶容器；Rseg + 右段们 =
   R 残留原样保留）→ 总叶容器数 ≤ 摘出叶容器数 → 挂回内容 = 摘出
   内容 - 剥回内容 → 各层挂回节点数 = 摘出各层 - 链各层（内容守
   恒）→ **各层空槽恰好够，无溢出无扩根**；stitch 以链尾光标调用
   时 mergeleaf 把链尾段并入 R 段（段数不增，核算不变，落地细节
   2）；R 残留挂叶容器层至多触发一次垂链（新建叶容器 ≤ 空槽 ≥ R
   残留 1 个，findroom 放宽为"第一个 cc<FANOUT 的已切层"，见落地
   细节 1）

#### 落地细节

1. **findroom（fill 版，定案）**：现有 findroom 断言"挂载层右侧全
   空"（remove 语义；linecache 版还会把右侧兄弟切进 rt[k] 腾空）——
   fill 挂回时挂载层右侧可能是**链容器们**（树上活内容），断言必炸
   且不可照搬 linecache 的切兄弟逻辑（链容器不能动）。fill 版 =
   自叶容器向上找第一个 **cc < FANOUT** 的已切层（上溯上限
   parent(fl)，论证 1/8 保证存在），makechain 挂到 cc 位置 = 链容
   器之后，字节序正确
2. **stitch 复用**：直接复用现有 spD_stitch（成熟机制）；挂回 =
   逆操作（论证 8）无特殊处理。**调用前提：光标必须位于链尾段**
   （lcD_stitch 的 assert i >= cc-1 同源）——spF_append 把光标推至
   链尾（落地细节 4），剥洋葱结束后直接以该光标 stitch；此时
   mergeleaf 合并的 = L 路径叶容器最后一段 = 链尾段 与 rt[0] 首段
   = R 段（R 紧随链尾），字节序正确
3. **光标维护**：前缀剥填期间 R->paths[l+1..]
   指向 rt 槽（随前移/下放同步更新：R 前移后 R->paths[l] 更新为
   &rt[levels-l]->children[0]；下放后 R->paths[l+1] 更新为
   &rt[levels-l-1]->children[i]）——递归中必须（步骤 4 定位新前缀）；
   **阶段 2a 切出时 R->paths[fl] 初值指向 rt[levels-fl] 内 R 槽**；
   **递归完成后 R Cursor 废弃**。L 光标由 spF_append 推至链尾
   （落地细节 4），作为 stitch 调用位置；最终光标 = 步骤 9b 的
   filterleaf 自然结果（off + len，append 语义）
4. **append merge 原语**：spF_ 版（lcB_append 框架，密铺），fill
   场景专用（挂载层 = 已切层）；**必须含节点内同 id 合并（§6.8——
   lcB_append 无合并概念（行无 id），spantree 版必须有，否则段数
   不守恒、论证 1 前提失效**；跨容器同 id 不合并，原样落位）；**光标
   语义照 lcB_append：调用前提 = 光标在叶容器末段（assert i >= cc-1），
   填完叶容器后 paths 指向最后填的段——剥洋葱全程把光标推至链尾**
   （stitch 前提，落地细节 2）；填段时与左邻同 id 则并入（段数单调
   不增）
5. **filterleaf 的裂叶路径**：filterleaf 内部构造 rt（左半段 + nid
   段）后统一经 spF_appendrt 落回（单出口，满时 makeroom 再插；空
   rt 也由 appendrt 收尾）
6. **度量记账（切出减/剥回加）**：摘挂切出每层执行 spM_up 减度量
   （spD_cutrange 同款）——切 parent(kl)（层 kl-1 节点）的
   [i+1..cc) 后 spM_up(L, kl-1, -db)，db = 被切子节点字节和（阶段
   1a/1b 统一 kl ∈ [fl+1, levels]，阶段 2a 为 kl = fl）；剥回
   append 与 stitch 各自加回；arb 只改 id 不改变段长 → 全程净 0，
   tree->bytes 恒不变（fill 是度量保持操作）
7. **reserve 顺序与预算（2026-08-14 定案；2026-08-14 修订：splice
   改为 remove + edit 组合）**：reserve 必须在 pad 之前——pad 的
   insertrt 满叶分裂会消耗 freelist（levels+1 个节点），裸奔即
   ralloc 断言。fill 预算 = 6*levels+7（两个 pad 的 insertrt +
   阶段预算 4*levels+5）；insert/append 预算 = 3*levels+4（pad 的
   insertrt + splitins）；remove 预算 = 4*levels+5（cutrange + stitch
   的节点需求）。splice 无独立预算——remove 与 edit 各自 reserve
   （reserve 只保 freelist 水位，重复调用无害；虚拟光标下 remove
   早退、pad 后光标总在树尾，先删后插与原先插后删语义等价）。
   非虚拟 fill 不消耗 pad 预算，多预留无害（reserve 只保 freelist
   水位）
8. **跨容器合并的树结构维护（2026-08-17 废弃，存档）**：规范形
   放宽（§6.8）后节点边界允许不合并，spI_merge 不再跨容器合并，
   mergeleft/mergeright/foldchain 删除。以下为历史定案存档
   （2026-08-14 fill_brute 逼出；2026-08-14 修订：fanout8 逼出
   foldchain 阈值分层；2026-08-14 修订：foldchain 复用 foldnode，
   mergeleft/mergeright 共用 spI_neighbor 走邻容器；
   2026-08-14 修订：merge 光标不变量——零 seek 收尾）：
   spI_merge 跨容器合并（mergeleft/mergeright）吸收邻容器段后，邻容器
   可能掏空（1→0）或欠满（2→1）——**不调 spD_remove**（太重的递归
   删除），本地化解决：spN_remove 单节点内删段 + `spI_foldchain` 沿侧链
   （fork 层以下恒挂首槽）逐级删空壳/fold 欠满链接，fork 层（槽
   ip-1 / ip+1）空壳删槽后交 `spD_rebalance(C, d0)` 的 foldnode 收尾。
   链上欠满链接必有兄弟可 fold（单孩子链在沿途已塌缩，assert 保）。
   插入点容器的容器内合并欠满由 spI_merge 末尾 rebalance 兜底（cc ≥
   FANOUT/2 时 O(1) 早退）。foldnode 合并分支必须 free 被并入的空壳
   （B+ 骨架移植补全，与 cutrange 三处 free 同族——L 右兄弟子树、R 前缀
   子树、fl 层 mid+R 空壳，cutrange 现经 spN_purge 批量释放）；
   foldchain 非空链接的 fold/balance 复用 spD_foldnode（mergeleft 先
   还原 paths 到 sav 链后调用），空壳直删+爬升保留专用分支。
   **merge 光标不变量（零 seek 收尾）**：splitins 结束光标恒在
   **插入段尾**（poff=段长，sp_offset=endoff，m<need 满容器分裂后以
   `sp_locate(C, C->off)` 重建 paths——off 为字节计数不受分裂影响）；
   merge 前置 assert 该态。左邻合并循环维护 off/poff（段头左移），
   右邻合并零维护（段向右长）。mergeleft 简单路径光标随 paths 落左叶
   末段尾（off -= bl, poff += bl）；mergeright 简单路径光标不动
   （paths 还原 sav，插入段尾仍在合并段内）。foldchain 破坏 paths 后
   以 `sp_locate(C, C->off)` + `poff = off0/offn - off` 重建段尾态。
   merge 全程零 seek/advance，收尾光标 sp_offset == endoff 恒成立。
   **foldchain 阈值分层**：折叠条件原为硬编码 cc < 2（fanout4 下
   FANOUT/2 == 2 恰好一致）；fanout8 下中间层欠满阈值 = FANOUT/2 = 4，
   mergeleft/mergeright 掏空容器使中间层 cc 4→3（<4 但 ≥2）时旧逻辑
   不折叠 → 欠满残留、checktree 失败。修复：叶容器层（x == levels-1）
   阈值 2（叶容器无最小 cc），中间层（x < levels-1）阈值 FANOUT/2；
   折叠后 par 检查 `x == 0 || cc(par) >= FANOUT/2` 同理分层。
   fanout4 行为不变。

#### 边界情况

- **空前缀**（R 在 children[0]）：剥洋葱无内容，直接 R 前移 +
  下放 + 递归
- **空后缀**（R 在 children 末尾）：后缀们 = 空；R 右森林 = R 残留
  段们（rt[0]）
- **空 mid**（L、R 相邻，iL+1 == iR）：前缀 = 空，直接进入 R 处理
- **叶层**：剥 R->paths[levels] 前的叶们（R 段左侧）；R 段 + R 右段
  们（R 残留）留下（段们留在 rt[0]，叶容器空壳 free）
- **满树验证例子**：fanout=4，fl 层 = [L,N1,R,N2] 全满，fl=0（根）
  ——① L 右剥回 → L 满树不变（逆操作）；② 切根 [N1,R,N2] → 剥 N1
  → append 密铺成 N1' → 根 = [L,N1']；③ 剥 R 左（R 子树 4 叶容器
  满 → R 左 ≈ 3.75 内容）→ 密铺 3 叶容器 → R'（1 节点）→ 根 =
  [L,N1',R']；④ stitch 挂 N2（根第 4 槽）+ R 残留（1 叶容器，挂
  R' 空槽）→ 根 cc = 4，全程无溢出无扩根
- **树尾 fill**：R = 树尾（locend 态，pr = bl 段长）→ 阶段 5b 的
  filterleaf 中 `poff < 段长` 为假 → R 段右半 0 长**不产生**（0 长
  段禁）；L 段 `pl == 0` 时 `poff > 0` 为假 → 左半同理
- **虚拟 fill**（R > bytes，含空树）：pad C（若 C 虚拟）/pad R
  物化 id0 段后走常规路径；pad notice arb(0,0) 恰一次（仅 R 虚拟
  时）；空树 fill 由 seek 构造 R 走虚拟态（advance 空树 no-op 不可
  用）——fill_virtual 测试三场景：全虚拟 pad+color、树尾起步虚拟
  扩展、实前缀+虚拟尾（跨叶 filterrange）
- **R == bytes（树尾）**：不 pad，仅 locend 修正 poff（advance 越过
  尾会置 poff=0，locend 恢复末段长）

### 6.7 删除 stitch 的左邻吸收（spD_mergeleft——2026-08-17 废弃）

> **废弃（2026-08-17，§6.8 规范形放宽）**：节点边界允许不合并，
> remove 的 L 容器被掏空（cc==0）时不再拉取左邻尾段，rt[0] 直接
> 挂回空容器；spD_mergeleft/dropleftchain/foldbelow/foldleft/
> foldright 从代码删除。以下为历史定案存档（2026-08-15）。

> 动机（存档）：spD_stitch 的左叶容器被 cutrange 掏空（cc==0）且 rt[0] 非空时，
> 走 mergeleft 而非 mergeleaf。初版把 rt[0] 首段"推入"左邻容器——合并段
> 跨过删除空隙盖住光标位置，后续 splitins 在失效光标上崩溃（mergeleft
> 光标脱节 bug，report_sp_mergeleft_handoff.md）。重写后合并方向反转。
> piecetab/linecache 无此函数（无 id 合并概念），独有逻辑。

**合并方向（定案）**：左邻容器尾段**拉入** rt[0] 首段——合并段落入
cut 侧的空容器，光标留在 cut 链上只左移 bj（join 段长），不走失效
光标。

- **叉点（fork）**：自叶层向上首个 L 路径 `idx > 0` 的层——L 路径
  贴最左沿止于此，左邻子树挂在叉点槽 i-1 上；全路径最左（无叉点）
  = 无左邻，mergeleft 早退。fork 是"左邻链"与"共享祖先"的分界：
  链内（fork 至叶）逐层记账，fork 以上共享 cut 侧祖先、一次
  `spM_up(fork-1, -bc)`；dropleftchain 以 fork 为链根（chain[fork]
  = 叉点左兄弟子树），叉点层的删/折同样在此层同步 paths[fork]。
- **pull-more**：join 后若左邻容器欠满（cc < FANOUT/2），继续把尾部
  段拉入 rt[0] 前部（spN_makespace+spN_copy 逐段），直到容器健康
  或空。保证"健康或空"、绝不半空——后续折叠永不携带欠满孩子；
  rt[0] 装满即停，幸存容器经 dropleftchain 折叠。拉取的槽是移动
  不是死亡，零 arb 事件（与 join 不同）。
- **spD_dropleftchain**：叉点左兄弟链向下至叶容器——链上自下而上
  （spD_foldbelow）删空节点 + 欠满者折入左兄弟（spD_foldleft），
  首个健康节点即停；叉点层的删/折同步 `paths[fork] -= 1`；
  链头无左兄弟（i==0）走 spD_foldright 折入光标子节点（合并：光标
  槽右移 cN；平衡：子节点移入幸存者——两路都需调用方修正 paths）。
- **foldleft/foldright 返回契约**：1 = 并入（调用方 free 节点 +
  删槽）；0 = 平衡（两节点保留）。
- **光标落点**：合并段位于 rt[0] 第 e 槽（e = 追加拉取的段数），
  光标指其内部空隙边界：`off -= bj, poff = bj, paths[l] += e`。
- **记账**：左邻链每层 `bytes[i] -= bc` + `mask[i] = OR(children[i])`
  （自 fork 到叶），fork 以上经 `spM_up(L, fork-1, -bc)`。

### 6.8 规范形放宽：节点内必须合并，节点边界允许不合并（2026-08-17 用户定案）

**定案**：放弃全局规范形（"相邻同 id 段必须合并"），改为：

- **节点内义务**：同一叶容器内相邻同 id 段必须合并（append merge、
  spI_merge 同容器部分、spD_mergeleaf）——分裂/合并产生的同 id 段
  一旦落入同一容器即合并
- **节点边界允许不合并**：跨容器相邻同 id 段不强求合并，原样保留

**代价论证（用户）**：非规范形最坏情况下每个叶容器至多 1 个额外
段负担（左/右边界与邻容器同 id，均摊 1）——按 FANOUT 62 计，
浪费容量 1/63 ≈ 1.58%，可接受。换取：跨容器合并义务整体删除
（remove 的 mergeleft/dropleftchain/foldbelow + fill/insert 的
spI_mergeleft/mergeright/foldchain），维护成本大幅下降、bug 面
收窄（本会话三个 mask bug 全部出自跨容器合并路径）。

**为什么节点内合并必须保留（密铺论证）**：fill 的剥回段数 ≤ 摘出
段数依赖"append merge 合并同 id"（论证 1）——这是**节点内**操作
（spF_appendspan 检查当前容器末段）。跨容器合并不参与此式（跨容器
段摘出时原样、剥回时原样，段数不变）。故放宽后密铺论证仍成立。

**保留义务清单（2026-08-18 落地，机制统一为 piecetab 同构）**：
- 叶内合并原语：`spK_seamleaf(C, right)` 合并叶容器内相邻同 id 段
  （i = cursor 槽 + right），同步 paths/off/poff 与 mask OR + died；
  `spD_seambound` 在 foldnode 合并/平衡决策前预合并接缝（同 id 进
  同一容器 = 叶内义务），并同步 p->bytes[i]/[i+1] 父指标（SEAM A/B，
  修复了平衡只扣移动段、留下被并段字节的指标漂移）
- fill/insert/append：fillrt 内合并 rt（born 后同 id 即并，动态
  need）+ splitins 尾部两次 seamleaf（左/右向）+ filterleaf 尾部
  两次 seamleaf；appendspan 容器末段同 id 合并（密铺论证依赖）
- remove：cutpiece 删段后 seamleaf；spD_mergeleaf（rt[0] 首段与
  L 容器尾段——挂回后同容器）；幽灵槽清理；L 容器掏空时直接挂回
  rt[0]，不再拉取左邻。**无 d 回退**：spantree 只有段合并（无 hole
  容量），可合并即整段并入 rt[0]，光标恒落 rt[0]（linecache 行可
  被切开、piecetab hole 部分合并才需回退）
- foldnode：seambound 预合并后 cL+cR 决策（合并可能使 >FANOUT 变
  ==FANOUT，促成原先无法的融合），paths 修正用合并后 cL 值
- 后续编辑会自然消化跨容器残留（同 id 段进入同一容器时合并），
  残留段数 = O(容器边界) 有界，不随操作无界膨胀

## 七、高亮引擎接口抽象（支持后期换框架）

调研（notes/research_highlighter.md）表明四类引擎输出归一为
"(区间, 类型) 流"，增量归一为"最小重染区间"，公共分母接口很小：

```
notify_edit(start, old_len, new_len)   /* TSInputEdit 是最全形态，取之 */
request(range)                          /* 请求染色（异步） */
→ 回调 yield_spans(range', spans[])    /* range' 允许大于请求区间 */
```

可替换性四要点：

1. **重染区间由引擎动态扩展**：收敛式引擎（TextMate 型）染完才知边界，
   输出区间必须允许大于请求区间
2. **天然异步**：LSP 强制异步；spantree 已定"染色滞后 + 旧段平移撑
   空窗"模式，异步是既有前提
3. **类型统一在适配层**：scope 栈 / capture 名 / legend 索引 → 适配器
   内映射为统一 style id，引擎差异不泄漏进 spantree
4. **分块 = 引擎实例边界**：引擎按块实例化，输入为块内容读取接口；
   换框架甚至可逐块混用（小文件块 ts 整树、巨文件块行收敛 lexer）

先例：Scintilla ILexer、emacs font-lock 挂入 treesit、neovim legacy
syntax 与 treesitter 并存同一 decoration provider。

## 八、Lua 绑定与 extmark 兼容

**全项目方向**：linecache/piecetab/spantree 最终都上 Lua 绑定，Demo 以
Lua 库形式交付——**接口设计以 Lua 暴露形态为准绳**。

### 8.1 nvim extmark Lua API 面（参考规格）

核心 6 函数：`nvim_create_namespace` / `nvim_buf_set_extmark(buf, ns,
row, col, opts)` / `nvim_buf_get_extmarks`（支持 overlap 查询）/
`nvim_buf_del_extmark` / `nvim_buf_clear_namespace`（区间批量删）/
`nvim_set_decoration_provider`（ephemeral 回调路径）。复杂度集中在
set_extmark 的 ~35 个 opts 键（keysets_defs.h:29-67），分三类：

| 类别 | 键 | 兼容判定 |
|---|---|---|
| 染色 | hl_group, priority, hl_mode, end_row/end_col, hl_eol, conceal, spell, url | **可兼容**（身份层实现） |
| 位置/生命周期 | right_gravity, end_right_gravity, invalidate, undo_restore, strict, ephemeral | gravity 可模拟；undo_restore/strict 等行为长尾**不承诺** |
| 渲染注入 | virt_text*, virt_lines*, sign_*, number/line/cursorline_hl_group, conceal_lines | **数据层不可兼容**——是向渲染流插内容，需渲染器对齐 nvim，最多 payload 透传 |

### 8.2 兼容决议

- 目标定为**染色子集方言**：set/get/del/clear + 染色类 opts + gravity
  语义，不承诺渲染注入类与行为长尾
- 实现路径：身份层实现 extmark 对象模型（id/区间/ns 分组），染色输出
  经 arbiter 写入。**gravity 在身份层模拟**（漂移收敛规则见
  research_marktree.md 3.2 节：left→删除点、right→新文本后），
  spantree 本体不引入 gravity
- **ns 动态无限由身份层承载（2026-08-15 v4 修订）**：extmark 动态
  ns 语义由身份层维护——映射到树内 ns 热槽（1..SP_MASK_BITS，mask
  通道，§九；复用策略后续定）；冷 ns 查询 = `sp_next(0)` 全扫 +
  intern 解码谓词（身份层持 intern，可判任意 merged id 的 ns 成员），
  零树改动。树内 ns 域有限（§9.2），无限语义在消费端兑现
- `ephemeral` + decoration provider 与本设计"快层不进树 + arbiter
  合成"同构，理念兼容零成本
- **hint/vtext 调和条款（2026-08-12）**：vtext 作**注入节点**挂树
  ——带宽度参与坐标（树遍历 = 拼接流，消解 design_editor.md 六b
  拼接循环摩擦），payload 透传（"最多 payload 透传"的中间形态）。
  渲染管线须能处理注入节点（P5 定节点形态）。hint 不进 sc intern
  （design_sc §五不变）
- 即使最终不逐字兼容，extmark API 也是身份层 API 的**免费需求规格**
  （经考验、文档完善），按其语义骨架设计避免闭门造车

## 九、ns 标记与按 ns 操作（2026-08-15 定案 v4，四轮审核修订）

> 动机链（第一性推演）：染色模型（存合成结果、丢身份）→ 清理/查询
> 须扫描或消费端跟踪区间 = 语义摩擦 → 需"按 ns"操作（del/find）
> → 高效 = 剪枝下降，不能全扫（GB 文件下扫描则建树无意义）→ 剪枝
> 靠子树聚合（度量骨架的第三通道）。方案推演与否决记录见
> notes/reports/research_spantree_usage.md。
>
> 抽象演进记录（三轮教训）：
> - v1：fill 带 ns 参数 + 操作式 mask 维护——推演依赖 arb 行为与
>   动词一致，arb 自由返回时无法甄别（Parse not validate 违背）
> - v2：arbiter 出参精确 mask / ns 通道投影——mask 布局泄漏进
>   arb 接口（用户：**用户不应知道 ns 用 mask 存储**）
> - **v3 定案**：fill 签名不变（ns 语义全进 id，op 编码，§6.3
>   原设计）；arb 加 `sp_Mask *mask` in/out 出参，**经 sp_addns/
>   sp_delns 操作，不直接写位**（位布局封装）；mask 定位 =
>   附加优化（next/prev 剪枝），fill 全范围遍历（fill 本来就不
>   管 ns——写操作必须触达 range 内所有段）。
> - arb 契约：out mask = 返回 id 的**精确 ns 集**（arb 自解码自答；
>   树零校验）。id == 0 时树强制 mask = 0（公理收敛，不信 arb）。

### 9.1 方案空间与定案

| 方案 | 否决/采纳 | 理由 |
|---|---|---|
| 扫描 + 谓词（fill(clear(w), 全域)） | 保底路径 | O(段数)；del_ns 全树写本就 O(段数)，谓词版仅省 arb 调用 |
| id2node + refkey（marktree 式 O(1)） | 否决 | 前提 parent 指针（本骨架无）；搬 key 处处维护 = 最贵不变式（research §1.1/建议 2） |
| bloom filter 节点摘要 | **否决（用户）** | 位不可清空——ns 状态可删除，bloom 无删除 |
| mask + 溢出位（冷热分离） | **否决（用户）** | 只是指定某 ns 当 "others"，后续开发债 |
| ownerf 回调（用户管 id→owner 映射） | 否决 | merged 值属多 ns——ownerf 必须返 mask 而非单 owner |
| id 高 6 位编码 ns | 否决 | merged 值多 ns 编码不下；id 全宽留给身份 |
| fill 带 ns 参数 + 操作式 mask 维护 | **被 v3 取代** | 推演与 arb 结果脱钩，无法甄别（Parse not validate 违背） |
| arb 出参精确 mask（v2） | **被 v3 吸收** | 方向对；mask 布局封装进 sp_addns/sp_delns 后成 v3 |
| ns 通道投影（splitf/joinf） | 否决 | 每段多两次回调 + 接口三重奏，抽象过度；v3 更简 |
| **arb 出参 mask + addns/delns 封装 + fill 不变（v3）** | **定案** | arb 报告结果 ns 集（与写回 id 同源，构造级同步）；位布局封装；fill 零迁移 |

**ns 定案**：命名 ns（namespace，与 marktree 一致）；**0 = 无归属**；
1..上限有效（上限 = sp_Mask 位宽：64 位平台 64、32 位平台 32——
C89 无 long long，sp_Mask = size_t）。位布局（bit(ns-1)）对用户
隐藏，全部经 sp_addns/sp_delns。

### 9.2 API 定案（v4）

```c
typedef size_t sp_Mask;   /* ns 位集；位布局对用户隐藏 */
#define SP_MASK_BITS (sizeof(sp_Mask) * CHAR_BIT)

/* mask 操作：唯一合法入口。ns == 0 / 越界 -> SP_ERRPARAM。 */
SP_API int sp_addns(sp_Mask *mask, int ns);
SP_API int sp_delns(sp_Mask *mask, int ns);

/* Arbiter v4：mask 是 in/out。in = old 的 ns 集（树传入，arb 免解
 * 码可得）；out = 返回 id 的精确 ns 集——**经 sp_addns/sp_delns 操
 * 作，不直接写位**（库文档明示）。id == 0（清）时返回 0。
 * 参数序（ud, id, old, mask）：新值在前，old + mask 成组殿后
 * （旧-新-旧 序无信息可读；返回类型 unsigned -> sp_Id 同步）。 */
typedef sp_Id sp_Arbiterf(void *ud, sp_Id id, sp_Id old, sp_Mask *mask);

/* fill 签名不变：ns 语义全进 id（op 载荷，§6.3）；fill 不管 ns，
 * range 内每段 arb（写操作必须触达所有段）。 */
SP_API int sp_fill(sp_Cursor *C, sp_Id id, size_t len);

/* ns 过滤迭代（mask 聚合剪枝下降），返回值改为 sp_Id 值（const
 * sp_Id* 是层模型时代残留——指针暴露树内槽位，段尾/NULL 语义别
 * 扭）；ns == 0 = 无过滤（= 旧语义，含全部段）；无匹配/段尾返回
 * SP_NONE（id 0 是合法未染色值，不能兼作哨兵）。sp_style 同步改值返回并吸收
 * sp_stylemask 的 mask 出参（2026-08-16 修订：pmask != NULL 时
 * 返回段精确 ns 集，NULL 则跳过）。
 * ns 域 = [0, SP_MASK_BITS]；超界 ns 视作查询空域：返回 SP_NONE、
 * plen = 0、光标不动（树内 ns 域有限，冷 ns 查询归身份层全扫谓词，
 * §8.2）。 */
SP_API sp_Id sp_next(sp_Cursor *C, int ns, size_t *plen);
SP_API sp_Id sp_prev(sp_Cursor *C, int ns, size_t *plen);
SP_API sp_Id sp_style(sp_Cursor *C, size_t *plen, sp_Mask *pmask);

/* 剪枝清除：对 mask 含 ns 位的每叶恰调一次 arb(ud, id, old, &m)
 * 并写回（写回规则同 fill：id 0 → mask 0）；非匹配叶不通知 arb
 * （与 fill 每段必调不同——剪枝是 sp_clear 存在的理由）。算法 =
 * 批量剪枝下降 + 容器内批量 arb（piecetab ptC_nexthole/ptC_freeze
 * 稀疏同构），见 §9.4a。ns 越界 -> SP_ERRPARAM；空树无匹配叶，
 * 早退 SP_OK。 */
SP_API int sp_clear(sp_Tree *T, int ns, sp_Id id);
```

- **SP_NONE**：`~(sp_Id)0`，sp_style/sp_next/sp_prev 的段尾/越尾/无效
  哨兵；id 0 是合法未染色值，不能兼作哨兵。fill/clear 拒绝 SP_NONE
  入树；arb 返回 SP_NONE 在 debug 下 assert、release 下收敛为 0。
- **树写回规则**：`new = arb(ud, in, old, &m)`（m 初值 = 段当前
  mask）→ 段 id = new，mask = (new == 0) ? 0 : m——id 0 公理：
  mask 恒 0（arb 报矛盾值也收敛）。
- **filtered 迭代精确语义（2026-08-16 v6 定案：exclusive。v5 变体 B
  ——返回当前段剩余落下一段头——经 piecetab 侧实测后回滚：使用方
  并未简化，pt_read/ptZ_append 反而更复杂；审计结论"exclusive 才
  是使用方使用最简的形式"。ns==0 与 ns≠0 完全同构，无不对称）**：
  - next(ns) = **exclusive**（pt_next 同构）：跳过当前段，剪枝
    下降（mask 聚合）找下一匹配段，返回整段 plen = 段长、**落匹配
    段头**（poff = 0）；树尾/虚拟/空树（poff >= 段长）返 SP_NONE。匹配
    判定 = `(段 mask & bit(ns-1)) != 0`。
  - 消费循环（pt_piece + pt_next 的 ns 版，写进绑定层与 sp_clear）：
    `style 预查当前段（mask 判定）→ 处理 → next 步进`——peek 恒可
    用（next 落段头），无死循环（落段头 + 跳过已消费段 = 结构保
    证），非匹配子树经 next 剪枝跳过。
  - prev(ns≠0) 不变：poff > 0 时当前段匹配 → 返回当前段（plen =
    poff，落段头，同旧）；不匹配或段头 → 向前单步循环；树头返 SP_NONE。
    prev(0) = 同款。
  - **树内严禁段尾态**（光标纪律，与 piecetab/linecache 共享不变
    式恢复）：API 返回时光标不得处于树中段尾（poff == 段长且
    offset < bytes）——sp_checkcursor 禁则（pt/lc 同款）；编辑 API
    （append/splice/fill）的插入段尾落点由各函数出口 forwardoff(0)
    收尾（offset 不变），树尾态豁免。sp_prev 落段头天然合规。
- **"ns 已完全消失"通知 = 存在性检查**：消费循环形式
  （`seek(0) + style 预查 + sp_next(ns) 循环`）首轮零匹配即消失
  ——无需 pcount 出参。
- **arb 契约**（库文档明示）：out mask = 返回 id 的精确 ns 集（域
  [1..SP_MASK_BITS]；op 载荷 ns 不限，属 arb 私有解码，不进 mask）。
  建议基于 in mask 增量操作（加/删动作 addns/delns），全替换路径
  由 arb 自行重建。mask 报错 = 剪枝假阴性 = arb 实现 bug（树对
  mask 零校验，checktree 只验聚合一致性）。
- **兼容迁移**：fill 零迁移；arb 实现补 (sp_Mask *mask) 参 + 参数
  序 (in, old) + 返回类型 sp_Id；sp_next/sp_prev/sp_style 调用点
  改值接收 + next/prev 补 ns 参。

### 9.3 聚合与维护（第三度量通道）

- sp_Node 增 `sp_Mask mask[SP_FANOUT]` 并排数组：叶槽 = 叶精确
  ns 集；内槽 = OR(子槽)。piecetab 单字段技巧（1 bit/child）不适
  用——双维（槽 × ns 位）；并排数组使 spN_copy/move memcpy 自动
  覆盖批量搬位（漏点 = 剪枝假阴性 = 正确性 bug 的主要防线）。
- **spM_up 统一 remask（2026-08-15 v4 定案，ptM_up 逐行同构，
  无 set/clear 分路；2026-08-15 实施修订：删 `!changed && !db`
  早停——merge 系内联更新抢先改槽，changed 探测失真 → 上层聚合
  假阴性（ns_differ 逼出：mergeleft 内联重算右链后，调用方
  spM_up 首层 b==a 误停）→ 每层无条件重算 OR + 爬到顶。db≠0
  调用本就无条件爬满（字节必传），删除仅影响 db==0 路径，性能
  无回退）**：每爬升层 bytes += db 后调
  `spM_remask(p, i)`（l < levels 时重算 OR(子槽)，l == levels 叶
  槽不动）；**去 db==0 早退**——fill 度量保持但改 mask，纯
  mask 传播 = `spM_up(C, levels-1, 0)` 调用。remask = O(cc ≤
  FANOUT) 扫描，无唯一子槽特判。
- **spM_up 只爬光标路径**——路径外的槽 mask 更新全部手工：
  bytes 求和点（splitroot/splitchild/foldnode）同点重算 OR；
  makechain/stitchnode/merge 系见下。
- **手工维护清单（并排数组外仅此，逐点核对 piecetab 先例）**：
  - spI_onepiece：mask[0] = 0
  - spI_fillrt：slot0 = 新段 mask（调用方传）；slot1 = 原段 mask
    （p->mask[i]）
  - spI_splitins / spI_inherit：新段 id + mask 双继承（pad = 0）
  - spI_splitroot：r->mask[0] = OR(pp)、r->mask[1] = OR(nw)
  - spI_splitchild：p->mask[i]/[i+1] = OR(nd)/OR(nw)
  - spF_cutleaf：左半留在 p（mask 不变），右半 rt[0] 复制原 mask；
    `spN_copy` 后续整段原样
  - spF_filterleaf：rt[0] 左半 mask = om；rt[1] nid 段 mask = m；
    setid 后写回 mask = m + **新增传播调用点 spM_up(C, levels-1, 0)**
    （db=0，依赖去早退）
  - spF_appendspan：新槽 mask = 段 mask（签名加参）；同 id 合并
    分支 mask OR
  - spK_seamleaf 叶内合并：mask[i-1] |= mask[i]（spN_remove 前）
  - spD_seambound 接缝合并：同 seamleaf（mask OR + died）
  - spD_mergeleaf：rt[0].mask[0] |= p->mask[cc-1]（spN_setcc 前）
  - spI_mergeleft / spI_mergeright：**祖先链槽 mask 同步**——内联
    字节更新循环里左侧链槽 |= M（吸收叶 mask）、右侧链槽重算 OR
    （失去叶；自底向上序，先子后父，pf 层最后）；foldchain 路径
    由 foldnode 兜底【2026-08-17 废弃：跨容器合并删除，§6.8】
  - spD_mergeleft（stitch）：join 分支 `rt[0].mask[0] |= M`；链循环
    每层 `mask[i] = OR(children[i])`；`spM_up(L, fork-1, -bc)` 覆盖
    fork 以上【2026-08-17 废弃：§6.8】
  - **spD_makechain：新槽 bytes 与 mask 均置 0**（piecetab
    ptD_makechain 先例：`nn->mask = 0` + 父槽位清零——ralloc 内存
    非零，漏置 = 假阴性）
  - **spD_stitchnode：每轮顶部 `p->bytes[i] += db` 后
    spM_remask(p, i)**（kl < levels 时）——覆盖上一轮 findroom
    链拷贝挂入的深链节点父槽（piecetab `ptM_remask(p, i, k)` 同
    款）
  - spD_foldnode：合并分支 p->mask[i] = OR(ns[0])；平衡分支
    p->mask[i]/[i+1] = OR(ns[0])/OR(ns[1])；**两分支末尾均
    spM_up(C, l-1, 0)**——fold 改变 p 自身聚合（删槽/搬位），p 的
    父槽必须上推（2026-08-15 实施逼出：fold 链停止时上层聚合漏
    传）
  - spI_foldchain：cc==0 删槽分支同样 spM_up(C, x-1, 0)
    【2026-08-17 废弃：§6.8】
  - spD_balancenode：槽经 spN_copy/move 自动，无父级操作
  - spD_cutrange：**末尾补 spM_up(L, levels-1, 0)**——中间层删右
    兄弟子树改变 L 路径槽聚合，字节内联更新覆盖不到 mask；fl-1
    以上的 -db 爬升不受影响
  - spC_clearnode：arb 可能仅改 mask 不动 id——spM_up(C, levels-1, 0)
    无条件刷新（§9.4a 步骤 7；漏刷 = 聚合假阳性，sp_next 下降
    assert，clear_maskrefresh 测试逼出）
  - spD_rebalance 根塌缩：经 struct 赋值自动
- 编辑继承：splice/append/insert 新段 mask = 继承段 mask 全量
  （随 id 同源）；remove 槽随 spN_move 搬移。
- 语义 = **精确集合**（arb 报告同源）：mask 无 ns 位 ⇒ 子树必无该
  ns 段；有 ⇒ 下降细查。不是 bloom：删除减位由 arb 报告驱动，
  无位积累。
- 编辑漂移：splice 平移段（mask 随段走），聚合沿 splice 路径重算。
- fill 不剪枝（写必须触达全部段）；next/prev 剪枝靠聚合。

### 9.4 消费端语义

- **del_ns(ns)** = `sp_clear(T, ns, op_clear_ns(ns))`（热 ns，剪枝
  O(匹配叶)）；**op 载荷 ns 无限**（冷 ns 走 `fill(C, op_clear_ns,
  bytes)` 全扫——不限 SP_MASK_BITS，del 不依赖 mask）
- **del_object(id)** = `fill(C, op_del_object, 区间)`（op 载荷带对
  象 id；arb 段内解码精确匹配删分支）——删最后贡献后 out mask 自
  然无该 ns 位，假阳性不存在。**绑定实现偏差（2026-08-16 审计，
  Ltree_unmark）**：全扫收集命中 ns 集 + 逐 ns sp_clear(CLEAR(ns))
  ——CLEAR(ns) 清整个 ns 槽而非仅该对象分支（design_spantree_lua
  定案语义，非本库约束）
- **find/查询** = sp_next/sp_prev(ns) 迭代（mask 剪枝，消费端零
  区间跟踪）——热 ns（≤ SP_MASK_BITS）；**冷 ns 查询** =
  `sp_next(0)` 全扫 + 消费端 intern 解码谓词（身份层职责，§8.2）
- 消费端词汇 = ns + id；mask 只经 sp_addns/sp_delns 出现在 arb
  实现里，消费端永不出现

### 9.4a sp_clear 算法定案（2026-08-15 v4，piecetab commit 稀疏同构）

> 先例：piecetab.h `ptC_nexthole`（剪枝下降找下个含洞叶容器）+
> `ptC_freeze`（容器内批量物化 + 压缩 + `ptM_up(C, l-1, 0)` 纯
> mask 传播 + 欠满 rebalance）。sp_clear 拒绝逐段 filterleaf/fill
> 方案（每段 O(levels) 定位重扫 = 违背批量铁律；GB 稀疏场景退化
> 为 O(段数×levels)）。

```
sp_clear(T, ns, id):
  1. [参数] ns ∈ [1, SP_MASK_BITS]，T 非空；空树早退
  2. [游标] C = seek(T, 0)
  3. [peek] style 查当前段 mask：无 ns 位 → sp_next(ns) 剪枝下降
     跳至下一匹配段（内层槽 mask 无 ns 位即跳——ptC_nexthole 同构，
     但按槽 mask & bit 判且**向前索引步进**（非 ptC_ 的从头重扫：
     arb 可能返回仍含 ns 位的 id，重扫会重访已处理叶））；无 → 返回
     有 → 处理该段所在容器（v6 exclusive：next 跳过已 peek 段，
     落匹配段头——"skip 前先 peek"消费模式）
  4. [批量 arb] 容器内所有匹配叶恰一次（先全部 arb 完再动结构）：
      nid = arb(ud, id, old, &m)；nid == old → 不写；否则
      setid + mask = (nid == 0) ? 0 : m
   5. [容器内合并] 同 id 相邻叶合并扫描（ptC_freeze 压缩同构）——
       此刻容器内匹配叶均已 arb，吸收无欠账
   6. [跨容器边界不合并（§6.8，2026-08-17）] 容器边界同 id 段原样
       保留；历史定案（2026-08-16）为 6a 左邻 mergeleft + 6b 右邻
       吸收，随跨容器合并义务一并废弃
   7. [传播与折叠] spM_up(C, levels-1, 0) 纯 mask 传播（db=0，去早
       退依赖；容器槽路径恒有效，无需前置 relocate）；
       容器欠满走 rebalance（2026-08-17 起无 foldchain，rebalance
       内含 foldnode）；spC_clearnode 把 Cursor 放到容器末段，
       回步骤 3 前由外层 sp_next(ns) 前进（exclusive，跳过当前容器，
       直接到下一匹配段）
```

- **Cursor 定位**：全程不调 backwardoff/forwardoff/locend；处理容器
  时 Cursor 停在当前匹配段头，压缩后由 spC_clearnode 移到容器末段，
  外层 sp_next(ns) exclusive 前进到下一匹配段
- **复杂度**：O(匹配叶容器数 × (FANOUT + 匹配叶 × arb) + 边界合并
  + 折叠)；非匹配子树零下降零 arb。**全程零分配**（setid/合并/折
  叠只释放节点，无 split 无 ralloc——无需 reserve）
- **欠账零不变式（正确性核心）**：每匹配叶恰一次 arb；任何合并不
  得吸收"未 arb 的匹配叶"。容器内批量先 arb 后合并 + 边界 6a/6b
  规则保证
- **与 fill 的语义差**：fill 对区间内每段必调 arb；sp_clear 只调
  匹配叶——arb 不得依赖非匹配段被调用（如计数语义的 arb 须接受
  调用次数 = 匹配叶数）
- **arb 返回仍匹配 id**：写回后该叶 mask 仍含 ns 位——向前索引步
  进保证不重访（每叶仍恰一次），节点内合并不受影响

### 9.5 测试要点

- mask 聚合差分：随机 fill/splice/append/insert/remove 后
  sp_next(ns) 遍历与全段扫描对照（fanout4/8 两档）
- arb 精确报告：多 ns 段（a 画 + b 画 + 删 a 后仅剩 b）、
  del_object 删最后贡献后 sp_next(ns) 零匹配（无假阳性）
- id == 0 公理：arb 报矛盾 mask（返回 0 + 非零 mask）→ 树强制 0
- 剪枝：mask 无 ns 位时 sp_next(ns) 零匹配、零下降
- sp_addns/sp_delns：ns 0/越界/负数 → SP_ERRPARAM；往返一致
- 编辑漂移/继承：splice、append、insert 后 mask 正确
- 兼容：fill 签名不变；旧 arb（mask 原样返回）行为不变
- filtered 迭代语义（v6 exclusive）：next 跳过当前段剪枝至下一
  匹配段落段头（plen = 段长）；消费循环 style 预查 + next 步进
  无死循环；段中起步跳过当前段；prev 中段态（plen=poff 落段头）；
  越界 ns 返 SP_NONE 光标不动；树内段尾态禁则（编辑 API 出口
  forwardoff(0) 收尾）
- sp_clear（§9.4a）：arb 调用数 == 匹配叶数（arb_counting 先例）；
  与 fill(op_clear_ns, bytes) 差分 id/mask 流一致（arb 对非匹配
  段 no-op 时）；吸收合并欠账零（容器内合并仅吸收已 arb 叶）；arb
  返回仍匹配 id 不重访；节点内合并 checktree 通过；ns 越界/
  空树/arb NULL；GB 级性能冒烟（合成段稀疏场景剪枝有效）

### 9.6 eph（ephemeral）层定案（2026-08-16，绑定层实现）

> 动机：调研（reports/research_sp_highlight_ns.md + 姊妹篇
> research_extmark_ephemeral.md）判定——hl = 视口级派生可重建数据
> （每帧重建），lsp = 快照持久（漂移有价值）。eph 层 = 给"每帧
> 重建"数据一个 C 端平铺存储 + 读时与树合成，免 Lua 表构造与
> 帧上 overlay；neovim 的 ephemeral 调研结论 = 不引入其"每帧全清
> scratchpad"机制（spantree 无 gravity/undo 负担，快层已是库内
> 等价物），本节 eph 是其**有持久语境的变体**：数据留在 C 存储、
> 树编辑自动失效。

**分工定案：eph 完全在绑定层（lua/spantree.c）实现。**

- eph = 每 ns 一个 sv_ 平铺 segment 数组（naive spantree：memmove
  维护节点内合并），不进 B+ 树、不占 mask 位、不参与 arb 写时折叠、
  不随编辑漂移（编辑动词 = 全清）。
- 无参读 = 树流 + 各 eph 层边界切分 merge；每子区间 **attr 黑盒
  覆盖**（comp:attr(树 id) + 各 eph attr 按层序覆盖 → comp:intern
  幂等）——零构成拆解、零 mapof、零 mask 读依赖（2026-08-16 v4
  修订：v3 的 sp_stylemask 方案退场）。
- **C 库无新增读 API**（v4 修订）；新增依赖 = **arb 三态契约与
  全量调用点**（API 无修改，§9.7 定案）：新生段调 arb(id, 0)、
  死亡（合并/删除/trim 归零/freetree）调 arb(0, id)、合并调
  arb(id, old) 出口双向计数（filterleaf 以保护 +1 防瞬态
  ref==0 误回收）。绑定层三态分派（design_spantree_lua.md v4
  §3.2）：新生/合并出口 ref++、死亡与合并出口 old-- ref--。

**eph 不变量**（绑定层）：
1. 数组恒有序、不交叠、相邻异 id；fill 覆盖切分 + 规范化合并。
2. 树编辑动词（splice/append/insert/remove，树级 + 游标级）= 先
   全清 eph；普通 ns 的 fill/clear/reprio 不清（树段未位移）。
3. eph fill/clear = 零树操作、零 epoch（读路径逐读重算零缓存）。
4. 合成层序：p<0 上、p>=0 下（0 层 = 表层）；eph 间 (prio,
   regseq) 升序，后层覆盖前层。

测试要点（绑定侧 TestEph；C 侧无新增 API——v4）：覆盖切分/
规范化/幂等覆盖、区间清/全清、合成流（p<0 上、p>=0 下、多层
优先级、单层直返）、滚动残留两窗口正确、8 个编辑动词清空、
eph fill 零 epoch。C 侧新增 = id 生命周期事件（§9.7）的
fanout4/8 计数差分用例。

### 9.7 id 生命周期契约（arb 三态，2026-08-16 定案）

> 背景与论证：report_sp_idref.md——refcnt = 树段引用计数；旧 arb
> 出口规则只覆盖部分事件（分裂/合并/删除静默、部分覆写欠计数、
> ret==old 幻影）。全 API 事件矩阵审计后定案：**单 arb 回调三态
> 分派**，树按段槽事件补发调用，无第二回调。

**三态契约**（arb 文档注释同步）：

```
arb(ud, in, old, mask):
    ret = in && old ? merge(in, old) : (in ? in : 0)
    if (ret != 0) refcnt[ret] += 1   /* +1 先于 −1；ret==old 两笔抵消 */
    if (old != 0) refcnt[old] -= 1
    return ret
```

| 形态 | 语义 | 契约 |
|---|---|---|
| `arb(0, old)` | 死亡：一个持有 old 的段槽消失（合并/删除/trim 归零/freetree）；真清写 fill(0) 同形 | ret=0 → 仅 old--；返回 0（纯死亡弃用；清写写回） |
| `arb(in, 0)` | 新生：一个持有 in 的段槽出现（写空段/分裂碎片/继承） | ret=in → 仅 in++；**必须原样返回 in**（空旧值无物可合成，转换自由本不存在） |
| `arb(in, old)` | 合并决策 | ret=merge(in,old)；出口统一规则 +ret/−old（ret==old 两笔自然抵消，net 0）；**分裂碎片计数由 filterleaf 的保护/补片承担**（下） |

**树侧事件清单**（全部经 arb；id 0 不计不发；`T->arb == NULL`
时全部静默）：

| 站点 | 调用 |
|---|---|
| spN_purge（加树参；k==0 分支逐叶） | `arb(0, id)`——统一覆盖 cutrange ×3 与 freetree |
| 合并吸收（spK_seamleaf / spD_seambound / spD_mergeleaf / spF_appendspan 并入 / spC_clearnode 并入 / spI_fillrt rt 内合并） | `arb(0, id)`/次 |
| spD_cutpiece 整槽删除、spD_trimright/trimleft 减至 0 | `arb(0, id)` |
| spI_fillrt 新段与右半槽 | `arb(id, 0)`，返回值写入槽；rt 内合并（born 后同 id）补 `arb(0, id)` 抵消 |
| spF_filterleaf | 见下 |
| spF_cutleaf / spF_peelleaf | `arb(id, 0)` 补片（裂段复制引用），见下 |
| spF_cutright / spF_peeldown / spD_stitch | 静默（逻辑移动零事件） |

**filterleaf 保护/补片/cancel（出口双向计数的时序前提）**：

```
k = (poff > 0) + (poff + len < n)         /* 存活片段数 */
if (k >= 1 && sid != 0) arb(sid, 0)       /* 保护：出口 −1 永不归零 */
nid = arb(in, sid)                        /* 出口双向计数 */
if (nid == sid && mask 未变):
    if (k >= 1 && sid) arb(0, sid)        /* cancel，净 0 */
    return                                /* 早退保留：零结构 churn */
if (k == 2 && sid) arb(sid, 0)            /* 补第二片 */
分裂（静默）→ 写回 nid → 合并（arb(0, id)）
```

- **早退保留**：arb 前置顺序不变（v8 流程），no-change（ret==sid
  且 mask 同，构成复用热路径）零结构操作，仅多 2 次回调。
- **refcnt 中途可暂时超前树状态 1**（保护 +1 至分裂落地或 cancel
  才兑现）——穿越安全由"保护 +1 恒先于可归零的 −1"保证；k=0 时
  出口 −1 归零 = 整段死亡，合法（arb 保留 old 入链时建链的结构
  ++ 已先防住，见 report_sp_idref §六）。
- **计数精确性**：k=0/1/2 部分覆写净 −1/0/+1；ret==old 净 0；
  复用净 +1；append/insert 继承 +2/−2 成对净 0；remove/freetree
  全 −1。差分测试 vs 全树扫描恒等。

**测试要点**：三态分派各形态；计数差分 fuzz（随机编辑序列，
fanout4/8）；部分覆写 k=0/1/2 与 ret==old/mask 变；保护/cancel
对称；purge 承载的 remove/freetree 全量死亡；arb 调用计数测试
改按合并态（in != 0 && old != 0）统计。

## 十、开放问题

1. ~~属性表达：合成值 vs 按层分离值~~ **已决议**：**arbiter 单层**
   （方案 E，2026-08-12）——合成值经 arbiter 无损结构表达，层向量
   方案 A 被取代，见 §六。
2. ~~默认属性~~ **已决议（2026-08-12）**：id 0 = 未染色（对齐 sc
   style 0 = 空 attr 预置）。空文档、文档两端 append、全删后初始态
   = 未染色。渲染读端对 id 0 透传 → 默认样式。
3. ~~规范形维护~~ **已决议（2026-08-17 放宽，§6.8）**：节点内单
   id 整数比较合并；节点边界允许不合并（跨容器合并义务删除）。
   原决议（2026-08-12）为全局相邻同属性段 merge（照 piecetab
   piece merge + checktree 经验），2026-08-17 用户定案放宽。
4. ~~混合器仲裁细则~~ **被 arbiter 取代（2026-08-12）**：仲裁完全外置
   ——arbiter 回调（ud 上下文）+ operator id 空间（§6.3），无独立
   混合器组件；priority 语义由 sc 的 arbiter 实现决定。
5. ~~层槽具体分配~~ **取消（2026-08-12）**：单层无槽；写者/操作语义
   进 id 空间（attr ∪ operator）。
6. **cdir 暂缓（2026-08-12 决策记录）**：undo 回放继承方向 best-effort
   统一默认，窗口期染错由 `hl:reset` 整树重染兜底（editor.lua:903
   既有机制）。hunk 分拆方案否决——坐标漂移未解
   （spantree_cdir_analysis.md:60 自认 hunk2 pa 在 X 坐标、B 方向在
   Y 坐标）。需求明确（如窗口期染色精确）再回炉。

## 十一、风险与测试策略

- **方向语义贯穿调用链**：粘贴、redo、sam 命令、脚本编辑都须正确选
  append/insert。用错不崩溃只染错（软错误）→ **差分测试**：随机编辑
  序列后全量重染对比 spantree 状态。
- **写放大**：突变重染真重写段（extmark 只挤压位置）；换取读端零合成。
  渲染读多写少（见 research_marktree.md 第六节负载分析），交换划算；
  配合 viewport 懒染色控制重染范围。
