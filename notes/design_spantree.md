# spantree 设计（当前实现）

> 本文描述 `spantree.h` 的当前设计与实现。背景分析见
> `notes/research_marktree.md`；Lua 绑定与 editor 接入见
> `notes/design_spantree_lua.md`。
> 设计方法论：**API 语义先行，数据结构反推**。字节缝语义（区间可为 0）
> 排除 B+ 计量树；字节覆盖语义（L>0）排除 B 树点标记。
> 历史讨论、方案更迭与决议存档统一收在文末“历史与决议记录”。

## 一、定位

spantree 是**结果 id 的存储**（帧缓冲性质）：

- 每个字节段存一个不透明的 `sp_Id`；这个 id 代表什么由用户定义：
  - 可以是**最终渲染结果**：读取后可直接渲染；
  - 也可以是**混合列表的句柄**：读取后可还原这段属性中所有的 id 再混合。
- 标记不挂在字节缝，而是**覆盖字节，作为字节属性存在**（run/span
  模型，先例：Emacs intervals、Quill Delta、Word run）
- `[AAABBBAAA]` 存为**三个段**（A 段、B 段、A 段），不是“一段 A 上叠
  一个 B”
- 可重建——大不了全量重染，正确性兜底简单
- 对 vim-like（计划扩展 sam 语义）编辑器，“字节属性”是自然抽象；
  “字节缝”模型（extmark 式）反而别扭

与 neovim extmark 的根本分工差异：extmark 存原始标记点集、渲染端逐列
合成；spantree 存的是“结果 id”——它可能是最终渲染结果，也可能是混合
列表的句柄，具体由用户定义。若存最终渲染结果，读取后即可渲染；若存
混合列表的句柄，读取后可还原这段属性中所有的 id 再混合。合成决策上移
到染色写入方（arbiter），树本身不解释 id。

## 二、数据模型

全覆盖 partition：文本被划分为连续段，每段 `(len > 0, attr)`。在
spantree 中 attr 是**不透明 `sp_Id`**。

**与 piecetab 同构（不计 COW）**——“piece 加属性、不持内容”：

- piecetab piece = `(len, src)`；spantree 段 = `(len, sp_Id)`——
  形态一一对应，唯内容引用换属性 id
- **分裂**：`ptI_splitins` ↔ spantree 段分裂（sp_fill 边界 / splice）
- **合并**：`ptD_mergeleaf` + stitch ↔ spantree 节点内合并（单 id
  比较；跨容器不合并，见 §2.1）
- **字节度量**：两者都不管行，只管字节块；编辑平移 = 度量树前缀和
  天然免费
- 继承：B+ 骨架、池分配（pt_Pool）、splice、mergeleaf/stitch、scan
  批量加载
- **剥离**（piecetab 为 undo 快照而设，spantree 不需要）：COW、
  hole/mask 空洞、arena 数据块、内容引用——spantree 段无内容，纯
  度量 + 属性
- 文本删除 = 段缩短/消失，**无批量删问题**；稀疏染色 = 大 id 0 段
  天然表达（§四）

### 2.1 规范形：节点内必须合并，节点边界允许不合并

- **节点内义务**：同一叶容器内相邻同 id 段必须合并（append merge、
  spI_merge 同容器部分、spD_mergeleaf）
- **节点边界允许不合并**：跨容器相邻同 id 段不强求合并，原样保留

代价：最坏情况下每个叶容器至多 1 个额外段负担（约 1.58% 容量，FANOUT
62），换取跨容器合并义务整体不存在、维护成本与 bug 面收窄。

**为什么节点内合并必须保留（密铺论证）**：fill 的剥回段数 ≤ 摘出段数
依赖“append merge 合并同 id”——这是**节点内**操作
（spF_appendspan 检查当前容器末段）。跨容器合并不参与此式。

保留的叶内合并机制：

- `spK_seamleaf(C, right)`：合并叶容器内相邻同 id 段，同步
  paths/off/poff 与 mask OR + died
- `spD_seambound`：foldnode 合并/平衡决策前预合并接缝，同步
  `p->bytes[i]/[i+1]` 父指标（SEAM A/B）
- fill/insert/append：fillrt 内合并 rt（born 后同 id 即并）+ splitins
  尾部两次 seamleaf + filterleaf 尾部两次 seamleaf；appendspan 容器末
  段同 id 合并
- remove：cutpiece 删段后 seamleaf；spD_mergeleaf（挂回后同容器）；
  L 容器掏空时直接挂回 rt[0]
- foldnode：seambound 预合并后 cL+cR 决策

后续编辑会自然消化跨容器残留（同 id 段进入同一容器时合并），残留段数
= O(容器边界) 有界。

### 2.2 树形与术语

- **层号**：`T->levels` = l 意味着树有 **l+2 层**——root = -1 层
  （嵌入 Tree）；层 0 节点 = root 的 children；层 levels-1 = 叶容器
  层；**层 levels = 叶层**（`C->paths[levels]` 指向的槽实际存储
  sp_Id）。层号变量用 **l**（与左游标 L 区分）
- **不变式**：`C->paths[level] == &parent(level)->children[i]`
  （level = 0..levels）
- **叶**：parent(levels)->children 里被 cast 成 `sp_Node *` 的 sp_Id；
  其长度 = parent(levels)->bytes[i]
- **叶容器**：parent(levels)（层 levels-1 节点）
- **中间节点**：层 l（l < levels-1）的节点
- **根**：嵌入 Tree 的 -1 层
- **分歧层 fl**：splitpaths 返回的最小 l，使 `L->paths[l] !=
  R->paths[l]`；L、R 位于 parent(fl) 的不同槽
- **fl 层**：parent(fl) 的 children 序列 = `[L左...][L][mid...][R]
  [R右...]`
- **rt[k] 层语义**：rt[k] 装**层 levels-k** 的内容——rt[0] = 叶们，
  rt[1] = 叶容器们，rt[levels-fl] = fl 层节点们

## 三、无 gravity：操作定义继承

gravity 在点模型中存在，是因为插入方不知道缝里有什么标记，粘附决策
只能预存在标记上。span 模型把**决策从数据搬到操作**：

| 操作 | 属性语义 |
|---|---|
| 段内插入 | 自动继承所在段属性 |
| `append`（边界） | 继承**左**段属性 |
| `insert`（边界） | 继承**右**段属性 |
| `remove` | 只缩段不扩段，**无左右歧义** |

- append/insert 与 vim `a`/`i`、sam 语义同构
- **undo 无歧义**：append/insert 的逆操作是 remove，remove 不存在扩
  段选择，undo 链**无需存方向信息**
- 突变（如关键字中插字破坏语法）：上层介入重染整段，spantree 只做机
  械继承

## 四、部分结果存储

- 稀疏染色**不需要树级偏移机制**：未染色 = 大 id 0 段，天然表达——
  4GB 文件仅染 1 字节 = `[len 4GB id0 段][len 1 染色段]` 两段即可
- “未染色”作为属性值（id 0），在全覆盖模型内自然表达

## 五、明确不做（有意的表达力边界）

| 不做 | 理由 / 归属 |
|---|---|
| 标记身份（按 id 删改查） | 段是染色结果非对象；归身份层 |
| id→node 反查（id2node） | 前提是 node 有 parent 指针；linecache 骨架无 parent，加哈希表也无用 |
| 0 长度标记（书签/锚点） | L>0 是模型公理；归身份层 |
| 同写者覆盖可逆 | 同写者写入即覆盖，有损是固有语义；**跨写者**恢复由 arbiter 无损合成天然支持（§6.2） |
| 层/混合器内建于树 | 写者/仲裁语义全部外置（arbiter 回调 + id 空间），树零格式知识（§6.4） |
| 树内 parent 指针 | 当前无需求方；逃生通道：改 `lcN_parent` + 结构操作全面维护即可加，但侵入面广 |

## 六、arbiter 单层模型

### 6.1 当前模型

树段 = `(len, sp_Id)`——单 id，无层槽。写入唯一原语
`sp_fill(C, id, len)`：对区间内每段调
`arbiter(ud, id, 段当前 id, &mask)`，写返回 id 与 mask（0 = 清段，
arbiter 可自行调 id finalizer；**0 也进 arbiter**，spantree 不特判）。

id 空间 = attr 编码 ∪ **operator 编码**（写者/操作/ns 可编码进 id：
如负数或大 id 区间；挥发 id 用后清空 intern 映射）。ns 不进 id，走叶
mask 并排数组（第三度量通道），arb 经 sp_addns/sp_delns 报告结果 ns
集（§九）。

合成 = 无损结构：`merged = {parent = {w: old_contrib, ...}}`（写者标
签 + 历史树，存 sc 的 intern 表，**树完全无感**）。

**为什么不是层**：

- 多层模型要“精确地每层相等才能合并”——k 槽整向量比较、覆写只改一
  层的合并陷阱，段分裂要复制 k 槽。层数越多，段操作越贵
- 插件写 span 的核心诉求是“写是稳定的，不会被轻易覆盖；至少 sc 层
  不会被完全淹没；即使所有 attribute 都被其他写者覆盖，也该有【我
  写过】的痕迹”。记忆型插件无重算能力、无地方存 offset——痕迹必须
  由 spantree 保管。当前模型把痕迹编码进合成值链，无需独立层槽

### 6.2 数学完备性

- 无损 parent 链 = 表达能力 ≥ 任意层模型；arbiter 把标签放 id 结构
  里（sc 侧），树保持单槽
- **覆盖** = 同写者分支替换（`{P:X, E:E1}` + P 写 X' →
  `{P:X', E:E1}`）
- **撤销** = 元操作：清 E → `fill(op_clearE)` → arbiter 从 parent 链
  移除 E 分支 → 写回（op 为挥发 id，事后释放）
- **全清** = `fill(0)` → arbiter 自行决定 → 返回 0 → 写 0
- **交错撤销**（P/E/Q 任意序）全可行——parent 树支持任意分支操作
- 编辑漂移 = 段平移（位置 + 值一起走）；编辑删除 = 认
- 恢复 = 源重染（§十一 request）或元操作——两者皆备

**痕迹保证**：old 参与每次合成——写者未覆盖的字段透传（痕迹在合成值
链里）；arbiter 可自定义编码（ud 上下文）——“被覆盖但未被淹没”。

### 6.3 快层与身份层

- **快层（光标/选区/光标行）**：不进树，渲染端每帧直叠
- **身份层**：书签/诊断对象等独立生命周期结构，职责 = 对象生命周期
  管理 + 向 spantree 输出染色（经 arbiter）

### 6.4 不透明 id 与 sc 分工

**树段存不透明 sp_Id（上层编码产物），非字段化 attr**——cellgrid
style id 同款抽象。

- **结果 id 语义由用户定义**：id 对 spantree 不透明——它可以是最终
  渲染结果（读取后立刻渲染），也可以是混合列表的句柄（读取后还原
  这一段属性中所有的 id 再混合）；树不假设是哪种

- **sc 分工**：值域持久（intern 表：id → attr/operator，id 永不回收
  除显式释放）+ 仲裁（arbiter 实现：合成/分支替换/移除/finalizer）
- **spantree 分工**：位置域持久（offset + 单 id，编辑平移/删除）——
  “替写者管理每次写的 offset，不挥发”
- id 0 = 未染色（对齐 sc style 0 = 空 attr 预置）
- 节点内合并 = 单 id 整数比较；跨容器不合并
- attr/operator 格式演进零波及 spantree

## 七、光标模型与导航语义

### 7.1 光标三态

| 态 | poff | off | paths |
|---|---|---|---|
| 正常态 | [0, 段长) | 段前字节 | 段槽 |
| 树尾态 | 末段长 | bytes - 末段长 | 末段槽 |
| 虚拟态 | ≥ 末段长 | bytes - 末段长 | 末段槽（树尾路径） |

- **虚拟态 = 树尾态 + 超出量入 poff**：off 恒 < bytes（i 恒有效）；
  linecache `lnu == breaks[i]` 同款。段内判定 = `poff < 段长` 单条件
- 虚拟态由 seek/locate/advance 越尾分支与 spI_pad 产生；**虚拟态回退
  入树须真实重定位**——不能走 backwardoff
- 树尾态（poff == 末段长）与虚拟态（poff > 末段长）区分

### 7.2 核心不变式

- **除非 Cursor 指向整树结尾，否则 Cursor 不指叶尾**
- **导航层（seek/locate/advance/next/prev）全部落段头/段内/树尾，
  零产出树中段尾态**——next 为 exclusive（跳过当前段落下一匹配段头）
- 编辑层逃逸（插入段尾/合并段尾落点）由各编辑函数出口
  `forwardoff(0)` 收尾（树尾豁免）
- `sp_checkcursor` 校验（pt/lc 同款）：offset >= bytes 豁免，否则
  poff < 段长

### 7.3 seek/locate/advance 调用清单（例外论证，禁其余调用点）

| 调用点 | 原语 | 理由 |
|---|---|---|
| sp_fill | sp_seek 构造 R | 构造区间右端；空树/树尾由 seek 落虚拟/树尾态 |
| spD_remove | sp_advance(&R, len) | R 为 C 的栈副本，构造区间右端的自然原语 |
| sp_clear | sp_seek(&C, T, 0) | 初始游标构造，非路径信息丢失 |

内部 Cursor helper 已全部改为相对维护：sp_prev 树头回退用
spK_lochead；spI_splitins 满容器分裂后光标本就在新段；sp_insert 用
spK_backwardoff 回插入点；sp_clear 不重定位 Cursor——全程无
locate/forwardoff/locend。

**sp_advance 实现**：advance = 相对 seek，不调 sp_locate。空树不再是
no-op——spantree 越尾进虚拟态，advance(5) 空树 = 虚拟 offset 5，与
seek(5) 一致（d==0 全路径幂等）。负向两分支 + 回绕比较：`-d > off` →
backwardoff(off)；否则 backwardoff(-d)——虚拟起点经 backwardoff 天然
完备。向前越尾 → locend + poff += (off+d − bytes)。

### 7.4 内部函数光标契约

| 函数 | 前置 | 后置 |
|---|---|---|
| spK_findleaf(C, l, &poff) | paths[0..l-1] 有效、off = 前缀路程 | 完整 paths、poff 段内、off 段前 |
| spK_locend(C) | 树非空 | 树尾态 |
| spK_forwardoff / backwardoff | 正常态、目标在树内 | 正常态、前进/后退 d |
| spD_remove(C, len) | 任意 | 光标于删除点（offset 不变，段尾态已收尾） |
| spI_insert(C, ins, useleft) | 光标于插入点（正常/树尾/虚拟三态；树中段尾态 assert 拒绝） | 插入段尾（poff=ins，恒；append 出口 forwardoff(0) 收尾，insert 经 advance 回插入点） |
| spI_splitins(C, len, id) | 光标于插入点 | 插入段尾（poff = len） |
| spK_seamleaf(C, right) | 任意 | 叶内相邻同 id 段已合并（i = 光标槽 + right） |
| spI_pad(C) | 任意 | 树尾态（offset 不变） |
| spF_filterleaf(C, len, in, right) | 区间头 | 区间尾（单出口；非树尾时 poff < 段长） |
| spF_filterrange(C, R, fl, in) | L 可含半叶、R 实光标、fl=spD_diverlevel(L,R) | L 落在 fill 尾 |
| spF_appendrt(C, rt) | C->poff == 0、rt 为待落段 | 段已落回、C 在插入内容尾 |
| spF_appendvirt(C, id, len) | 树尾/虚拟态 | fill 段尾（off = 段起点、poff = len） |
| spF_appendspan(C, id, len) | 链尾段（assert i ≥ cc-1） | 链尾（随 append 推进） |
| spD_stitch(L, rt) | 链尾段（mergeleaf 前提） | L 位置保持 |

### 7.5 导航语义：sp_next / sp_prev exclusive

- **SP_NONE**：`~(sp_Id)0`，sp_style/sp_next/sp_prev 的段尾/越尾/无
  效哨兵；id 0 是合法未染色值，不能兼作哨兵。fill/clear 拒绝 SP_NONE
  入树；arb 返回 SP_NONE 在 debug 下 assert、release 下收敛为 0
- **next(ns) = exclusive**（pt_next 同构）：跳过当前段，剪枝下降找下
  一匹配段，返回整段 plen = 段长、**落匹配段头**（poff = 0）；树尾/
  虚拟/空树（poff >= 段长）返 SP_NONE。匹配判定 =
  `(段 mask & bit(ns-1)) != 0`
- **消费循环**：`style 预查当前段（mask 判定）→ 处理 → next 步进`——
  peek 恒可用，无死循环，非匹配子树经 next 剪枝跳过
- **prev(ns≠0)**：poff > 0 时当前段匹配 → 返回当前段（plen = poff，
  落段头）；不匹配或段头 → 向前单步循环；树头返 SP_NONE。prev(0) =
  同款
- **树内严禁段尾态**：API 返回时光标不得处于树中段尾（poff == 段长
  且 offset < bytes）；编辑 API 出口 forwardoff(0) 收尾，树尾态豁免
- **“ns 已完全消失”通知 = 存在性检查**：消费循环首轮零匹配即消失，
  无需 pcount 出参

## 八、fill 算法（当前实现）

### 8.1 独有操作与铁律

编辑同步 splice（度量变化，piecetab edit/remove 对位）；染色写入
sp_fill（度量不变、单 id 覆写 + arbiter）是其他 B+ 树
（linecache/piecetab/marktree）都没有的。

铁律：

1. **rt 单数组**：只用 `S->rt`（`sp_Node *rt = S->rt`），禁本地 rt
   数组、禁 mt+rt 双数组
2. **filterrange 内禁 spI_ 系**：剥洋葱/摘挂/stitch 不得调
   spI_splitins/insertrt/splitchild/splitroot/fillrt；spI_pad 与
   filterleaf 豁免
3. **批量**：摘挂层循环批量；禁止逐段 seek
4. **一次扫描**：区间内容经剥洋葱线性处理

### 8.2 sp_fill 薄分流

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

- 虚拟/树尾起点直接走 `spF_appendvirt`，不做 R pad、不延长末叶
- 当前段内走单段 `spF_filterleaf`；跨段才构造 R 并进入 filterrange
- `filterrange` 返回时 L 落在原 R 段起点 / fill 尾，外部不需要
  locate / 二次 filterleaf

### 8.3 filterrange 三步算法

前置条件：R 是实光标；`fl = spD_diverlevel(L, R)` 且 `fl <= levels`；
L 可处于叶内任意位置（`spF_cutleaf` 负责把 L 右半段切进 rt）。

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

  2. 前缀剥填（核心）
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

### 8.4 关键不变式

1. `filterrange` 的输入不要求 `L->poff == 0`；`spF_cutleaf` 负责切
   L 右半
2. `filterrange` 返回时 L 落在原 R 段起点 / fill 尾，这是
   `spF_peelleaf` + `spD_stitch` 的自然结果；若现有 stitch 不满足，
   应修 stitch/peel 的光标推进，而不是在外部 locate
3. 全程无 R pad：虚拟尾段由 `spF_appendvirt` 作为独立新段插入
4. 剥填阶段节点数净减，因此不需要 fl 层以下的 makechain/扩根

### 8.5 机制：剥洋葱、append merge、前缀剥填

**剥洋葱原语**：

```
peel(rt):
  loop:
    if rt[0] 有数据（叶们）:
      逐段 arb -> append merge 回 L 链（arb 只对叶调用）
    else:
      自底向上找第一个有节点的层 k（k > 0）
      将 rt[k] 的一个节点的 children 下放到 rt[k-1]
      下放后 free 该节点
    until 剥完目标内容
```

容量保证：rt[k] 每次只下放一个节点的 children（≤ FANOUT 个）到空槽
→ 每槽恒 ≤ FANOUT。

**append merge（lcB_append 语义，密铺）**：

- “先尽量填叶子，如果叶子满了，垂单链树，然后继续填叶子”——从开始
  append 的地方起没有任何空隙，会被填成满树
- **光标随 append 推至链尾**（lcB_append 语义：调用前提 = 光标在叶
  容器末段 assert i>=cc-1，结束后 paths 指向最后填的段）——stitch
  以链尾光标调用
- “垂单链树” = makeroom/findroom：从 L 叶容器向上找第一个“右侧有空
  间”的**已切层** → 该层起每层建一个新节点（单链）→ 新叶容器 → 继续
  填
- 满树情况下 append merge 是**摘 L 右的严格逆操作**

**前缀剥填**（核心递归，统一 mid 与 R 左）：

```
peeldown(l, rt):                 # 层 l 的内容已在 rt[levels-l]
  1. [剥前缀] 对前缀所有节点完整剥洋葱回填（arb + append merge 回 L 链）
  2. [R 前移] memmove 让 R 到 children[0]（前缀剥完，槽压缩）
  3. [下放]   R->children 全部放进 rt[levels-l-1]
              （R->paths[l+1] 同步更新）
              下放后 R 空壳（cc=0）free——R 路径剥左后无内容
  4. [新前缀] R->paths[l+1] 指向的槽之前的孩子们 = 新前缀
  5. [递归]   对层 l+1、rt、新前缀递归调用 peeldown——直到叶层
  6. [叶层]   l = levels-1 或 fl = levels：
              剥 R->paths[levels] 前的叶们（arb + append merge 回 L 链）；
              R 段 + R 右段们（R 残留）留在 rt[0]
  终止：rt 中只剩后缀们 + R 残留段们
```

### 8.6 refcount 写回规则

- `spF_filterleaf` 裂叶分支：若 `left > 0` 先 `spA_born(oid)` 复制左
  半引用；右半与 nid 段经 `spI_fillrt` / `spF_appendrt` 落回
- `spF_cutleaf` / `spF_peelleaf` 的裂段同样先 `spA_born(oid)` 补一份
  引用
- 合并两个同 id 段时（`spI_fillrt` 左邻合并 / `spF_appendspan` /
  `spF_appendvirt` merge 分支）必须 `spA_died` 一次
- 新段插入不额外调 born/died；新 id 的引用由 `spA_born` / `spA_arb`
  负责
- `spF_append` 剥叶时逐段 `spA_arb(C, in, old, &m, 0)`，`!nid` 时
  `m = 0`；`spF_peelleaf` 虚拟分支用 zero-id 段使 arb 看到 old=0

### 8.7 helper 抽象（实现映射）

- `spF_appendvirt(C, id, len)`：树尾/虚拟起点 fill 入口。构造 rt（有
  pad 先放 `(id=0, len=pad)`，再放 `(nid=spA_born(id), len=len)`）→
  spI_fillrt → spF_appendrt → Cursor 指向 fill 段（off = 段起点，
  poff = len）
- `spF_filterleaf(C, len, in, right)`：单段染写；`right` 控制是否与
  右邻 `spK_seamleaf(C, 0)` 合并
- `spI_fillrt(C, id, len, m)`：向 `S->rt` 追加/合并一段；返回 0 = 合
  并/复用，1 = 新增槽；左邻合并必须 `C->off += len`
- `spF_appendrt(C, rt)`：`assert(C->poff == 0)`；把 rt 落回 L 链（满
  时 `spI_makeroom` 再插），推进 C 到插入内容尾
- `spF_append(C, in, n, keep)` / `spF_flushrt(rt, n)`：剥叶回填；
  `keep` 为真时不 flush rt[0]
- `spF_peel(C, in, n, k, keep)`：剥洋葱；`k == 0` 回填叶子，`k > 0`
  下放 children 并 free 空壳
- `spF_peeldown(C, R, in, l)`：前缀剥填下钻循环
- `spF_peelleaf(C, R, in)`：处理 R 段本身，含虚拟区 zero-id 段
- `spF_cutleaf(C, rt)` / `spF_cutright(C, rt, fl)`：切 L 右半 / 切 L
  路径各层右兄弟进 rt，并 spM_up 记账
- `spF_filterrange(C, R, fl, in)`：三步总入口
- `spD_stitch`：缝回 R 右并保证 L 落在 fill 尾；underfill 时参考
  `lcD_stitch` 做 foldnode + rebalance

### 8.8 核心论证（密铺）

1. **密铺**：append merge 是密铺；arb 只改 id 不改变段长 + 节点内同
   id 合并 → 剥回段数 ≤ 摘出段数 → 叶容器数 ≤ 摘出叶容器数 → 链层
   k 节点数 ≤ 摘出前层 k 节点数 → 摘挂阶段每层节点数净减少
2. **fl 层不溢出**：链层 k ≤ 摘出层 k → 垂链 findroom 总能找到已切
   层 → 摘挂阶段不可能要求 <fl 层的 makeroom
3. **stitch 兜底**：fill 不依赖 stitch 的正确性承诺；本算法输出 =
   逆操作场景，无溢出
4. **严格段数守恒（节点内守恒）**：摘挂内容 = 整段们 → 剥回不溢出
5. **arb 契约**：合并决策对每段恰一次；合并/死亡经 `arb(0, id)`、
   复制新生经 `arb(id, 0)`（§十）；arb 调用带 `sp_Mask *mask`
   in/out，id == 0 时树强制 mask = 0
6. **rt 容量**：逐节点下放 → 每槽恒 ≤ FANOUT
7. **复杂度**：O(定位 + 段数 × arbiter + 摘挂层循环)；一次扫描
8. **R 残留挂回 = 逆操作**：R 路径空壳全删 → 挂回内容 = 后缀们 + R
   残留段们 → 各层空槽恰好够，无溢出无扩根

### 8.9 落地细节

1. **findroom（fill 版）**：自叶容器向上找第一个 **cc < FANOUT** 的已
   切层（上溯上限 parent(fl)），makechain 挂到 cc 位置
2. **stitch 复用**：直接复用 spD_stitch；调用前提 = 光标位于链尾段
   （lcD_stitch assert i >= cc-1 同源）
3. **光标维护**：前缀剥填期间 R->paths[l+1..] 指向 rt 槽并同步更新；
   递归完成后 R Cursor 废弃；L 光标由 spF_append 推至链尾
4. **append merge 原语**：spF_ 版（lcB_append 框架），必须含节点内
   同 id 合并
5. **filterleaf 裂叶路径**：构造 rt（左半段 + nid 段）后统一经
   spF_appendrt 落回
6. **度量记账**：摘挂切出每层 spM_up 减度量，剥回 append 与 stitch
   加回；arb 只改 id 不改变段长 → 全程净 0
7. **reserve 顺序与预算**：reserve 必须在 pad 之前。fill 预算 =
   6*levels+7；insert/append = 3*levels+4；remove = 4*levels+5；
   splice 无独立预算（remove 与 edit 各自 reserve）

### 8.10 边界情况

- 空前缀：直接 R 前移 + 下放 + 递归
- 空后缀：R 右森林 = R 残留段们（rt[0]）
- 空 mid：直接进入 R 处理
- 树尾 fill：R 段右半 0 长**不产生**；L 段 `pl == 0` 时左半同理
- 虚拟 fill（R > bytes，含空树）：走 appendvirt 独立新段插入
- 满树验证例子：fanout=4，fl 层 = [L,N1,R,N2] 全满，fl=0（根）——
  剥回 + 密铺后根 = [L,N1',R']，stitch 挂 N2 + R 残留 → 根 cc = 4，
  全程无溢出无扩根

## 九、ns 标记与按 ns 操作

### 9.1 模型与 API

命名 ns（namespace，与 marktree 一致）；**0 = 无归属**；1..上限有效
（上限 = sp_Mask 位宽：64 位平台 64、32 位平台 32——C89 无 long
long，sp_Mask = size_t）。位布局（bit(ns-1)）对用户隐藏，全部经
sp_addns/sp_delns。

```c
typedef size_t sp_Mask;   /* ns 位集；位布局对用户隐藏 */
#define SP_MASK_BITS (sizeof(sp_Mask) * CHAR_BIT)

/* mask 操作：唯一合法入口。ns == 0 / 越界 -> SP_ERRPARAM。 */
SP_API int sp_addns(sp_Mask *mask, int ns);
SP_API int sp_delns(sp_Mask *mask, int ns);

/* Arbiter：mask 是 in/out。in = old 的 ns 集；out = 返回 id 的精确
 * ns 集——经 sp_addns/sp_delns 操作，不直接写位。id == 0（清）时返
 * 回 0。参数序（ud, id, old, mask）。 */
typedef sp_Id sp_Arbiterf(void *ud, sp_Id id, sp_Id old, sp_Mask *mask);

/* fill 签名不变：ns 语义全进 id（op 载荷）；fill 不管 ns，range 内
 * 每段 arb。 */
SP_API int sp_fill(sp_Cursor *C, sp_Id id, size_t len);

/* ns 过滤迭代（mask 聚合剪枝下降），返回值改为 sp_Id 值；ns == 0 =
 * 无过滤；无匹配/段尾返回 SP_NONE。sp_style 同步值返回并吸收
 * sp_stylemask 的 mask 出参（pmask != NULL 时返回段精确 ns 集）。
 * ns 域 = [0, SP_MASK_BITS]；超界 ns 视作查询空域：返回 SP_NONE、
 * plen = 0、光标不动。 */
SP_API sp_Id sp_next(sp_Cursor *C, int ns, size_t *plen);
SP_API sp_Id sp_prev(sp_Cursor *C, int ns, size_t *plen);
SP_API sp_Id sp_style(sp_Cursor *C, size_t *plen, sp_Mask *pmask);

/* 剪枝清除：对 mask 含 ns 位的每叶恰一次调 arb(ud, id, old, &m)
 * 并写回（写回规则同 fill：id 0 → mask 0）；非匹配叶不通知 arb。
 * ns 越界 -> SP_ERRPARAM；空树早退 SP_OK。 */
SP_API int sp_clear(sp_Tree *T, int ns, sp_Id id);
```

**树写回规则**：`new = arb(ud, in, old, &m)`（m 初值 = 段当前 mask）
→ 段 id = new，mask = (new == 0) ? 0 : m——id 0 公理：mask 恒 0。

**arb 契约**：out mask = 返回 id 的精确 ns 集（域 [1..SP_MASK_BITS]；
op 载荷 ns 不限，属 arb 私有解码）。建议基于 in mask 增量操作。mask
报错 = 剪枝假阴性 = arb 实现 bug（树对 mask 零校验，checktree 只验聚
合一致性）。

### 9.2 聚合与维护（第三度量通道）

- `sp_Node` 增 `sp_Mask mask[SP_FANOUT]` 并排数组：叶槽 = 叶精确 ns
  集；内槽 = OR(子槽)。并排数组使 spN_copy/move memcpy 自动覆盖批量
  搬位
- `spM_up` 统一 remask：每爬升层 bytes += db 后调 `spM_remask(p, i)`
  （l < levels 时重算 OR(子槽)，l == levels 叶槽不动）；**无 db==0
  早退**——纯 mask 传播 = `spM_up(C, levels-1, 0)`
- `spM_up` 只爬光标路径——路径外的槽 mask 更新全部手工：
  spI_onepiece（0）、spI_fillrt（新/旧槽）、spI_splitins/inherit
  （双继承）、spI_splitroot/splitchild（OR）、spF_cutleaf（复制原
  mask）、spF_filterleaf（写回 m + `spM_up(C, levels-1, 0)`）、
  spF_appendspan（新槽 mask / 合并 OR）、spK_seamleaf 与
  spD_seambound（OR + died）、spD_mergeleaf（OR）、spD_makechain
  （bytes 与 mask 置 0）、spD_stitchnode（每轮 remask）、
  spD_foldnode（合并/平衡 OR + 末尾 `spM_up(C, l-1, 0)`）、
  spD_cutrange（末尾 `spM_up(L, levels-1, 0)`）、spC_clearnode
  （无条件刷新）、spD_rebalance 根塌缩（struct 赋值自动）
- 编辑继承：splice/append/insert 新段 mask = 继承段 mask 全量；
  remove 槽随 spN_move 搬移
- 语义 = **精确集合**：mask 无 ns 位 ⇒ 子树必无该 ns 段；不是 bloom，
  删除减位由 arb 报告驱动
- 编辑漂移：splice 平移段，聚合沿 splice 路径重算
- fill 不剪枝；next/prev 剪枝靠聚合

### 9.3 消费端语义

- **del_ns(ns)** = `sp_clear(T, ns, op_clear_ns(ns))`（热 ns，剪枝
  O(匹配叶)）；**op 载荷 ns 无限**（冷 ns 走 `fill(C, op_clear_ns,
  bytes)` 全扫）
- **del_object(id)** = `fill(C, op_del_object, 区间)`（arb 段内解码精
  确匹配删分支）——删最后贡献后 out mask 自然无该 ns 位
- **find/查询** = sp_next/sp_prev(ns) 迭代（mask 剪枝）；**冷 ns 查
  询** = `sp_next(0)` 全扫 + 消费端 intern 解码谓词（身份层职责）
- 消费端词汇 = ns + id；mask 只出现在 arb 实现里

### 9.4 sp_clear 算法

先例：piecetab.h `ptC_nexthole`（剪枝下降）+ `ptC_freeze`（容器内批
量物化 + 压缩 + 纯 mask 传播 + rebalance）。sp_clear 拒绝逐段
filterleaf/fill 方案。

```
sp_clear(T, ns, id):
  1. [参数] ns ∈ [1, SP_MASK_BITS]，T 非空；空树早退
  2. [游标] C = seek(T, 0)
  3. [peek] style 查当前段 mask：无 ns 位 → sp_next(ns) 剪枝下降
     跳至下一匹配段（向前索引步进——arb 可能返回仍含 ns 位的 id，
     重扫会重访已处理叶）；无 → 返回
     有 → 处理该段所在容器（exclusive：next 跳过已 peek 段）
  4. [批量 arb] 容器内所有匹配叶恰一次（先全部 arb 完再动结构）：
      nid = arb(ud, id, old, &m)；nid == old → 不写；否则
      setid + mask = (nid == 0) ? 0 : m
  5. [容器内合并] 同 id 相邻叶合并扫描——此刻均已 arb，吸收无欠账
  6. [跨容器边界不合并] 容器边界同 id 段原样保留
  7. [传播与折叠] spM_up(C, levels-1, 0) 纯 mask 传播；
     容器欠满走 rebalance（内含 foldnode）；spC_clearnode 把 Cursor
     放到容器末段，外层 sp_next(ns) 前进
```

- **Cursor 定位**：全程不调 backwardoff/forwardoff/locend
- **复杂度**：O(匹配叶容器数 × (FANOUT + 匹配叶 × arb) + 合并 + 折
  叠)；非匹配子树零下降零 arb。**全程零分配**
- **欠账零不变式**：每匹配叶恰一次 arb；任何合并不吸收“未 arb 的匹配
  叶”
- **与 fill 的语义差**：fill 对区间内每段必调 arb；sp_clear 只调匹配
  叶
- **arb 返回仍匹配 id**：向前索引步进保证不重访

## 十、id 生命周期契约（arb 三态）

**三态契约**：

```
arb(ud, in, old, mask):
    ret = in && old ? merge(in, old) : (in ? in : 0)
    if (ret != 0) refcnt[ret] += 1   /* +1 先于 −1；ret==old 两笔抵消 */
    if (old != 0) refcnt[old] -= 1
    return ret
```

| 形态 | 语义 | 契约 |
|---|---|---|
| `arb(0, old)` | 死亡：一个持有 old 的段槽消失（合并/删除/trim 归零/freetree）；真清写 fill(0) 同形 | ret=0 → 仅 old--；返回 0 |
| `arb(in, 0)` | 新生：一个持有 in 的段槽出现（写空段/分裂碎片/继承） | ret=in → 仅 in++；**必须原样返回 in** |
| `arb(in, old)` | 合并决策 | ret=merge(in,old)；出口统一规则 +ret/−old（ret==old 两笔自然抵消）；分裂碎片计数由 filterleaf 的保护/补片承担 |

**树侧事件清单**（全部经 arb；id 0 不计不发；`T->arb == NULL` 时全部
静默）：

| 站点 | 调用 |
|---|---|
| spN_purge（加树参；k==0 分支逐叶） | `arb(0, id)`——统一覆盖 cutrange ×3 与 freetree |
| 合并吸收（spK_seamleaf / spD_seambound / spD_mergeleaf / spF_appendspan 并入 / spC_clearnode 并入 / spI_fillrt rt 内合并） | `arb(0, id)`/次 |
| spD_cutpiece 整槽删除、spD_trimright/trimleft 减至 0 | `arb(0, id)` |
| spI_fillrt 新段与右半槽 | `arb(id, 0)`，返回值写入槽；rt 内合并补 `arb(0, id)` 抵消 |
| spF_filterleaf | 见下 |
| spF_cutleaf / spF_peelleaf | `arb(id, 0)` 补片 |
| spF_cutright / spF_peeldown / spD_stitch | 静默（逻辑移动零事件） |

**filterleaf 保护/补片/cancel**：

```
k = (poff > 0) + (poff + len < n)         /* 存活片段数 */
if (k >= 1 && sid != 0) arb(sid, 0)       /* 保护：出口 −1 永不归零 */
nid = arb(in, sid)                        /* 出口双向计数 */
if (nid == sid && mask 未变):
    if (k >= 1 && sid) arb(0, sid)        /* cancel，净 0 */
    return
if (k == 2 && sid) arb(sid, 0)            /* 补第二片 */
分裂（静默）→ 写回 nid → 合并（arb(0, id)）
```

- **早退保留**：no-change 零结构操作，仅多 2 次回调
- **refcnt 中途可暂时超前树状态 1**（保护 +1 至分裂落地或 cancel 才兑
  现）；k=0 时出口 −1 归零 = 整段死亡，合法
- **计数精确性**：k=0/1/2 部分覆写净 −1/0/+1；ret==old 净 0；复用净
  +1；append/insert 继承 +2/−2 成对净 0；remove/freetree 全 −1

## 十一、Lua 绑定摘要

**全项目方向**：linecache/piecetab/spantree 最终都上 Lua 绑定，Demo
以 Lua 库形式交付——接口设计以 Lua 暴露形态为准绳。

- 目标定为 **nvim extmark 染色子集方言**：set/get/del/clear + 染色类
  opts + gravity 语义；不承诺渲染注入类与行为长尾
- 实现路径：身份层实现 extmark 对象模型（id/区间/ns 分组），染色输出
  经 arbiter 写入。**gravity 在身份层模拟**（left→删除点、right→新
  文本后），spantree 本体不引入 gravity
- **ns 动态无限由身份层承载**：映射到树内 ns 热槽
  （1..SP_MASK_BITS，mask 通道）；冷 ns 查询 = `sp_next(0)` 全扫 +
  intern 解码谓词，零树改动
- `ephemeral` + decoration provider 与本设计“快层不进树 + arbiter 合
  成”同构
- **hint/vtext 调和条款**：vtext 作**注入节点**挂树——带宽度参与坐
  标，payload 透传；hint 不进 sc intern
- 即使最终不逐字兼容，extmark API 也是身份层 API 的**免费需求规格**

### 11.1 eph（ephemeral）层（绑定层实现）

eph 完全在绑定层（lua/spantree.c）实现，不进 spantree 公共 API：

- eph = 每 ns 一个 sv_ 平铺 segment 数组（memmove 维护节点内合并），
  不进 B+ 树、不占 mask 位、不参与 arb 写时折叠、不随编辑漂移（编辑
  动词 = 全清）
- 无参读 = 树流 + 各 eph 层边界切分 merge；每子区间 **attr 黑盒覆
  盖**（comp:attr(树 id) + 各 eph attr 按层序覆盖 → comp:intern 幂
  等）
- C 库无新增读 API；新增依赖 = **arb 三态契约与全量调用点**
- **eph 不变量**：
  1. 数组恒有序、不交叠、相邻异 id；fill 覆盖切分 + 规范化合并
  2. 树编辑动词（splice/append/insert/remove）= 先全清 eph；普通 ns
     的 fill/clear/reprio 不清
  3. eph fill/clear = 零树操作、零 epoch
  4. 合成层序：p<0 上、p>=0 下（0 层 = 表层）；eph 间 (prio, regseq)
     升序，后层覆盖前层

## 十二、开放问题、风险与测试策略

### 12.1 开放问题 / 暂缓项

- **cdir 暂缓**：undo 回放继承方向 best-effort 统一默认，窗口期染错
  由 `hl:reset` 整树重染兜底（editor.lua:903）。hunk 分拆方案否决—
  —坐标漂移未解（spantree_cdir_analysis.md:60）。需求明确再回炉
- ns 热槽复用策略待后续定；冷 ns 语义已由身份层承载

### 12.2 风险与测试策略

- **方向语义贯穿调用链**：粘贴、redo、sam 命令、脚本编辑都须正确选
  append/insert。用错不崩溃只染错 → **差分测试**：随机编辑序列后全量
  重染对比 spantree 状态
- **写放大**：突变重染真重写段；换取读端零合成。渲染读多写少，配合
  viewport 懒染色控制重染范围
- **mask 聚合差分**：随机 fill/splice/append/insert/remove 后
  sp_next(ns) 遍历与全段扫描对照（fanout4/8）
- **arb 精确报告**：多 ns 段、del_object 删最后贡献后零匹配
- **id == 0 公理**：arb 报矛盾 mask → 树强制 0
- **剪枝**：mask 无 ns 位时零匹配、零下降
- **sp_addns/sp_delns**：ns 0/越界/负数 → SP_ERRPARAM；往返一致
- **编辑漂移/继承**：splice、append、insert 后 mask 正确
- **filtered 迭代语义**：next 跳过当前段落段头；消费循环无死循环；
  prev 中段态（plen=poff 落段头）；越界 ns 返 SP_NONE 光标不动；树内
  段尾态禁则
- **sp_clear**：arb 调用数 == 匹配叶数；与 fill(op_clear_ns, bytes)
  差分 id/mask 流一致；吸收合并欠账零；arb 返回仍匹配 id 不重访；
  checktree；ns 越界/空树/arb NULL；GB 级性能冒烟
- **id 生命周期**：三态分派各形态；计数差分 fuzz；部分覆写 k=0/1/2
  与 ret==old/mask 变；保护/cancel 对称；purge 全量死亡
- **eph（绑定侧）**：覆盖切分/规范化/幂等覆盖、区间清/全清、合成流、
  滚动残留两窗口、8 个编辑动词清空、eph fill 零 epoch
- 序列化 tree/mcompare：合并跨容器待合并段后与模型比对；fuzz 多 seed
  + ASan

## 十三、历史与决议记录（存档）

> 本节只作历史存档，供追溯与防重议；**当前实现一律以正文为准**。
> 此处出现的版本号、日期、废弃方案不代表当前设计。

### 13.1 方案演进：层向量 → arbiter 单层

| 方案 | 概述 | 结论 |
|---|---|---|
| 多树 | 每层一棵薄 spantree + 合成函数 + 缓存 | 冗余存 k 份位置结构、splice k+1 次；降级为插件便利库候选 |
| ~~A. 单树层向量~~ | 段属性 = `attr[SPAN_LAYERS]`，读端 fold | 被 E 取代（2026-08-12）：层合并困难 + arbiter 无损合成表达力 ≥ 层 |
| B. intern 精化 | 层向量 hash-cons 成小整数 id | 已被 E 吸收（id 空间即 intern） |
| C. 拉模式 | 写者注册回调，重合成时向源拉取 | 思想吸收进 E 的 operator |
| D. 区间外包 | 树内存 id，树外按区间存层数据 | 否决：外部数据须索引回位置，offset 会漂 |
| **E. arbiter 单层** | 树段 = `(len, sp_Id)`；写入经 arbiter 回调；id 空间 = attr ∪ 操作符 | 定案（2026-08-12） |
| piece 携带属性 | 染色存进 piece table | 否决：piece 边界与染色边界不同源，强耦合污染 |

相关历史决议：2026-08-12 方案 E 取代 A；id 0 = 未染色；不做“树级整体
偏移”（曾拟，用户否决）；hint/vtext 调和条款；cdir 暂缓；2026-08-14
Lua 绑定与 editor 接入定案（spantree.h 零改动）。

### 13.2 fill 算法迭代史（v1–v10）

- **v1**：定位→边界分裂→覆写→3 检查点（逐段遍历低效，弃）
- **v2**：cutrange 变体摘区间 → mt + mergek 洋葱合并（度量失配 + mt 冲突，弃）
- **v3**：现场剥洋葱（边界裂段剥回 +1/+2 段，满树 append 溢出 → 错序）
- **v4**：边界裂后置——摘挂只处理整段们，严格段数守恒；表述修正为 [iL+1..cc] 全切 + 前缀剥填递归
- **v5 前缀剥填**（定案）：fl 层全切进 rt + 前缀剥填 + stitch 挂回 R 右森林；密铺论证保证不溢出
- **v6**：删 R 路径空壳；append merge 含节点内同 id 合并；光标 = L 链 seek 到 off+len；树尾 fill 不保留 0 长半段
- **v7**：阶段 1 仅当 fl < levels；后置裂复用两次 filterleaf；stitch 以链尾光标；findroom 放宽为第一个 cc<FANOUT 的已切层
- **v8**（2026-08-14）：恢复虚拟 fill pad 语义；C 虚拟 → pad id0；R 虚拟 → pad + arb(0,0) notice；R == bytes 不 pad
- **v9**（2026-08-20）：去掉 R pad 与 fill 内 locate；filterrange 接受 L 整叶/半叶开头，三步 + 自身负责最终光标位置
- **v10**（2026-08-22，当前实现）：落地为 spantree.h，删去 `spF_fillto`/`spF_fillrange`/`spF_splitleaf`；`sp_fill` 薄分流；filterrange 内部 clear rt → cutleaf → cutright → peel → peeldown → peelleaf → stitch；`spF_peel` 带 `keep`，虚拟区用 zero-id 段剥填

### 13.3 ns 标记迭代史

- v1 fill 带 ns 参数 + 操作式 mask 维护（与 arb 自由返回无法甄别，弃）；v2 arb 出参精确 mask / ns 通道投影（mask 布局泄漏，弃）；**v3 定案**：fill 签名不变，arb 加 `sp_Mask *mask` in/out 出参，经 sp_addns/sp_delns 操作
- 否决记录：id2node + refkey（需 parent 指针）；bloom（位不可清空）；mask + 溢出位（others 债）；ownerf（merged 多 ns）；id 高 6 位编码 ns（多 ns 编码不下）；ns 通道投影（接口三重奏）
- 2026-08-15：初案 + 三轮审核（v3）+ 四轮审核（v4：spM_up 统一 remask、维护清单、ns 域、sp_clear、filtered next inclusive）；2026-08-16：filtered next 落点改“下一段头”（v5）随后回滚，定案 exclusive（v6）；虚拟态统一

### 13.4 规范形与跨容器合并

- 2026-08-12 原决议：全局相邻同属性段 merge；2026-08-17（用户定案）：放宽为“节点内必须合并，节点边界允许不合并”，跨容器合并义务整体废弃（删除 spD_mergeleft/dropleftchain/foldbelow 与 spI_mergeleft/mergeright/foldchain 等）
- 2026-08-18：叶容器内合并统一为 piecetab 同构——新增 spK_seamleaf 与 spD_seambound；删除 spD_foldleft/foldright/foldbelow/dropleftchain/mergeleft、spI_foldchain/neighbor/absorbleft/remaskleft/mergeleft/absorbright/remaskright/mergeright/merge、spC_peekright
- **spD_mergeleft 存档**（2026-08-15 定案，2026-08-17 废弃）：左邻容器尾段拉入 rt[0] 首段；叉点（fork）概念；pull-more 保证健康或空；dropleftchain 删空/折欠满；foldleft/foldright 返回 1=并入、0=平衡；光标落点 `off -= bj, poff = bj`
- **跨容器合并树结构维护存档**（2026-08-14~17）：merge 光标不变量、foldchain 阈值分层（fanout8 中间层 FANOUT/2）等，随规范形放宽废弃

### 13.5 其他历史决议与教训

- 2026-08-13：坐标系与术语定案（层号 l、fl、rt[k] 层语义）；2026-08-14：光标三态与内部函数契约、sp_advance 不调 sp_locate、reserve 预算、splice = remove + edit；2026-08-16：id 生命周期三态契约（report_sp_idref.md）、eph 层、pt_checkcursor 禁树中段尾、ptD_rmleaf 裂段光标逃逸修复
- 2026-08-15 实施修订：删 `!changed && !db` 早停——每层无条件重算 OR + 爬到顶
- 历史测试教训：多次迭代会覆盖早期定案语义，重写文档须对照 #if 0 测试（fill_virtual 的 pad 语义曾因 v7 误删而被 v8 恢复）

