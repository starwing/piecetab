# spantree 设计（讨论决议）

> 源自 marktree 调研讨论，背景分析见 `notes/research_marktree.md`
> （尤其第六节定位方式对比、第七节 extmark 模型）。
> 状态：**arbiter 单层模型已定案**（2026-08-12：方案 E 取代层向量
> 方案 A，论证见 §六）；开放问题已决议（§九）；attr 不透明 id 与
> sc 分工见 §6.5；实施计划见 plans/plan_spantree.md。
>
> 设计方法论：**API 语义先行，数据结构反推**。边界语义单方向决定结构
> 可行域——字节缝语义（区间可为 0）排除 B+ 计量树；字节覆盖语义
> （L>0）排除 B 树点标记。本文档所有结构决策皆由 API 语义倒推。

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
  规范形合并（单 id 比较，§6.6 最终算法 merge 步）
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
  `arbiter(ud, 段当前 id, 传入 id)`，**写返回值**（0 = 清段，arbiter
  可自行调 id finalizer；**0 也进 arbiter**，spantree 不特判）
- id 空间 = attr 编码 ∪ **operator 编码**（写者/操作可编码进 id：如
  负数或大 id 区间；挥发 id 用后清空 intern 映射）
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
- 规范形/stitch 合并 = 单 id 整数比较（**层模型的整向量比较陷阱消失**）
- attr/operator 格式演进零波及 spantree

### 6.6 区域运算：sp_fill 算法定案（2026-08-14 v8 前缀剥填 + 虚拟 pad 定案）

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
  stitch 无溢出）；append merge 含同 id 合并（规范形前提）；光标 =
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
  （findseg 段尾前进到下一段；locend 树尾态 poff=末段长）

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
prefixpeel(l, rt):               # 层 l 的内容已在 rt[levels-l]
  1. [剥前缀] 对前缀所有节点完整剥洋葱回填（arb + append merge 回 L 链）
              # 剥洋葱带 R 边界：每层只下放 R 路径左侧的节点
  2. [R 前移] memmove 让 R 到 children[0]（前缀剥完，槽压缩；后缀跟着前移）
  3. [下放]   R->children 全部放进 rt[levels-l-1]
              （R->paths[l+1] 同步更新为 &rt[levels-l-1]->children[i]）
              # 步骤 1 不碰 R 的孩子们（在 R 节点 children 内完好），
              # 下放无悬垂；下放后 R 空壳（cc=0）free——R 路径剥左后
              # 无内容，不进 R 右森林（挂回 = 逆操作无溢出，见论证 8）
  4. [新前缀] 通过 R->paths 得到新 rt 层的前缀：
              R->paths[l+1] 指向的槽之前的孩子们 = 新前缀（R 路径孩子的左侧）
  5. [递归]   对层 l+1、rt、新前缀递归调用 prefixpeel——直到叶层
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
  3. [pad R] R = seek(off0 + len)（seek 构造 R：advance 空树 no-op
     不可用）；R 虚拟（> bytes）-> spI_pad(R) 物化
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
      （其内部 spI_merge seek 到区间末 off + len，append 语义；
      R Cursor 在前缀剥填后废弃，见落地细节 3）
```

#### 核心论证（密铺，勿再违背）

1. **密铺（第一性原理）**：append merge（spF_ 版，lcB_append 语义）
   是密铺——从开始 append 的地方起没有任何空隙，会被填成满树。
   **严格化前提**：arb 只改 id 不改变段长（内容不变）+ append merge
   含**同 id 合并**（规范形，段数单调不增）→ 剥回段数 ≤ 摘出段数
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
4. **严格段数守恒**（沿自 v4）：摘挂内容 = 整段们（边界两半不摘
   不裂）→ 剥回段数 ≤ 摘出段数 → append 需求 ≤ 空槽 → 剥回不溢出
5. **arb 契约**：剥洋葱每段恰好一次 + 后置裂两次 filterleaf 各算
   一次（nidL/nidR 在 filterleaf 内对当前段自算）；合并（规范形）
   不调 arbiter；pad notice（R 虚拟时 arb(0,0) 恰一次，返回值弃用）
   是物化通知非染色写入，不计入段染色——fill_virtual 断言
   arb_notifypad == 2 = pad notice + 物化段染色
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
   场景专用（挂载层 = 已切层）；**必须含同 id 合并（规范形）——
   lcB_append 无合并概念（行无 id），spantree 版必须有，否则段数
   不守恒、论证 1 前提失效**；**光标语义照 lcB_append：调用前提 =
   光标在叶容器末段（assert i >= cc-1），填完叶容器后 paths 指向
   最后填的段——剥洋葱全程把光标推至链尾**（stitch 前提，落地细
   节 2）；填段时与左邻同 id 则并入（段数单调不增）
5. **filterleaf 的 splitseg 叶满路径**：后置裂复用 filterleaf 后
   （阶段 5），其内部 spF_splitseg 的叶满路径仍借 spI_insertrt；
   filterrange 内禁 spI_ → spF_ 版标准分裂延后实现（当前测试样例
   避开叶满裂）
6. **度量记账（切出减/剥回加）**：摘挂切出每层执行 spM_up 减度量
   （spD_cutrange 同款）——切 parent(kl)（层 kl-1 节点）的
   [i+1..cc) 后 spM_up(L, kl-1, -db)，db = 被切子节点字节和（阶段
   1a/1b 统一 kl ∈ [fl+1, levels]，阶段 2a 为 kl = fl）；剥回
   append 与 stitch 各自加回；arb 只改 id 不改变段长 → 全程净 0，
   tree->bytes 恒不变（fill 是度量保持操作）
7. **reserve 瞬时峰值（TODO）**：剥洋葱下放 free 与 append 垂链新
   建的瞬时差——第一轮（L 右段们填满 L 叶容器 → 垂链新建 ≤ levels
   个节点，此刻无 free）可能瞬时负；累计平衡（密铺，论证 1）。
   4*levels+5 粗略够（第一轮垂链 + 后置裂 2(levels+1) + 潜在扩根）；
   实现后用 SP_POOL_STATS 断言实测，不够再加

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
- **ns 映射到 id 空间分组**：ns 动态无限——身份层为每个 ns 维护
  写者标识，经 arbiter 的 operator 机制（§6.3）撤销/更新，树无感
- `ephemeral` + decoration provider 与本设计"快层不进树 + arbiter
  合成"同构，理念兼容零成本
- **hint/vtext 调和条款（2026-08-12）**：vtext 作**注入节点**挂树
  ——带宽度参与坐标（树遍历 = 拼接流，消解 design_editor.md 六b
  拼接循环摩擦），payload 透传（"最多 payload 透传"的中间形态）。
  渲染管线须能处理注入节点（P5 定节点形态）。hint 不进 sc intern
  （design_sc §五不变）
- 即使最终不逐字兼容，extmark API 也是身份层 API 的**免费需求规格**
  （经考验、文档完善），按其语义骨架设计避免闭门造车

## 九、开放问题

1. ~~属性表达：合成值 vs 按层分离值~~ **已决议**：**arbiter 单层**
   （方案 E，2026-08-12）——合成值经 arbiter 无损结构表达，层向量
   方案 A 被取代，见 §六。
2. ~~默认属性~~ **已决议（2026-08-12）**：id 0 = 未染色（对齐 sc
   style 0 = 空 attr 预置）。空文档、文档两端 append、全删后初始态
   = 未染色。渲染读端对 id 0 透传 → 默认样式。
3. ~~规范形维护~~ **已决议**：stitch 缝合点**单 id 整数比较**，相邻
   同属性段 merge（照 piecetab piece merge + checktree 经验）。
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

## 十、风险与测试策略

- **方向语义贯穿调用链**：粘贴、redo、sam 命令、脚本编辑都须正确选
  append/insert。用错不崩溃只染错（软错误）→ **差分测试**：随机编辑
  序列后全量重染对比 spantree 状态。
- **写放大**：突变重染真重写段（extmark 只挤压位置）；换取读端零合成。
  渲染读多写少（见 research_marktree.md 第六节负载分析），交换划算；
  配合 viewport 懒染色控制重染范围。
