# spantree 设计（讨论决议）

> 源自 marktree 调研讨论，背景分析见 `notes/research_marktree.md`
> （尤其第六节定位方式对比、第七节 extmark 模型）。
> 状态：**核心结构与层模型均已定案**（单树 + 编译期分层属性向量）；
> 待定项见开放问题。
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

**与 linecache 完全同构**——"linecache 给每行加一个 id"：

- linecache 实际只记录"当前是一行，行长 L>0"这一相对计量信息；
  "换行符在末尾"只是约定，实现不依赖
- spantree：段长即度量（L>0 满足计量假设），叶元素从 `unsigned` 长度
  变为 `(len, attr[SPAN_LAYERS])`（层向量见第六节）
- B+ 骨架、池分配、reserve 事务、splice、lc_remove/stitch、scan 批量
  加载全部继承
- 文本删除 = 段缩短/消失，**无批量删问题**；编辑平移 = 度量树前缀和
  天然免费（对比 marktree splice 每层改 O(2T) 个 key）

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

- 树级**整体偏移**支持只存部分区域（如 viewport 附近）的染色结果——
  4GB+ 文件不可能全文解析/染色
- "未染色"可作为一种属性值，在全覆盖模型内自然表达

## 五、明确不做（有意取舍的表达力边界）

| 不做 | 理由 / 归属 |
|---|---|
| 标记身份（按 id 删改查） | 段是染色结果非对象；归身份层 |
| id→node 反查（id2node） | 前提是 node 有 parent 指针（反查后须上行重建路径）；linecache 骨架无 parent，加哈希表也无用 |
| 0 长度标记（书签/锚点） | L>0 是模型公理；归身份层 |
| 同层覆盖可逆 | 同层写入即覆盖，有损是层内固有语义；**跨层**恢复由层向量天然支持（见第六节） |
| 动态层 | 层数编译期定死（`SPAN_LAYERS`）；插件共享固定层 + 混合器仲裁 |
| 树内 parent 指针 | 当前无需求方（区间外包已否决、intern 外包位置无关）。逃生通道：改 `lcN_parent` + 结构操作全面维护即可加，但侵入面广（split/balance/fold/stitch 处处修正，参照 marktree），无 parent 跑通则有 parent 只会更简单 |

## 六、层模型（已定案：单树 + 编译期分层属性向量）

### 6.1 为什么必须有层：单写者 vs 多写者

语法引擎能独占直写，只因它是单写者（自带缓冲，可全权重写任意区间）。
一旦出现第二个写者（插件染色、搜索高亮），单值 last-writer-wins 立刻
破产：插件染的区间被引擎重染冲掉，且无法恢复。**层是多写者组合的必需
仲裁机制**。

### 6.2 方案空间探索（记录，防重议）

| 方案 | 概述 | 结论 |
|---|---|---|
| 多树 | 每层一棵薄 spantree + 合成函数 + 最终树缓存 | 工程可行但第一性原理缺陷：**位置空间只有一份，层是属性空间的维度**——k 棵树冗余存 k 份位置结构、splice k+1 次。降级为插件便利库候选（保底） |
| **A. 单树层向量** | 一棵树，段属性 = `attr[SPAN_LAYERS]` | **定案**。位置事实存一次；层更新只改本层槽位；跨层恢复天然（他层槽位不动）；碎片与多树的最终合成树相同，非相对劣势 |
| B. intern 精化 | 层向量 hash-cons 成小整数 id，外部表 key=**层组合内容**（位置无关，不需 offset/parent） | A 的 payload 压缩后手，非独立方案。注意与"区间外包"区分 |
| C. 拉模式 | 写者注册回调，重合成时向源拉取 | 思想吸收：有缓冲的源（引擎）直写；无缓冲的源（插件）走混合器拉取 |
| 区间外包 | 树内存 id，树外按区间存层数据 | **否决**：外部数据须索引回位置，offset 会漂、node 指针无 parent 不可解引用 |
| piece 携带属性 | 染色存进 piece table | **否决**：piece 边界由编辑历史决定，染色边界由语法决定，两种边界不同源，强耦合互相污染 |

### 6.3 层分配（编译期固定）

| 层 | 写者 | 写入方式 |
|---|---|---|
| L0 语法/语义 | 引擎（自带缓冲） | 直写，区间替换 |
| L1 插件/自定义 | 多插件回调 → **混合器**写入 | 混合器仲裁同层重叠 |
| L2/L3 搜索等 | 各功能 | 区间替换 |
| 快层（光标/选区/光标行） | 渲染器 | **不进树**，每帧直叠 |

### 6.4 四条核心语义

1. **写原语唯一**：`layer_write(layer, range, spans[])` = 该层 [range)
   全量替换（引擎重染输出天然是"区间+全部新段"；清除 = 写"无"值）。
   层间互不可见——写 L1 不读不碰 L0。
2. **混合器是拉模式**：重染 [a,b) 时向各插件回调拉取片段，按注册序/
   优先级仲裁后一次写入 L1。插件不持位置结构，漂移问题消解于回调时机。
3. **级联在读端**：渲染查询对 k 个槽做固定 fold（k 编译期定，小循环），
   partial-style 语义：上层设置的字段覆盖、未设置透传（同 neovim
   `hl_combine_attr`）。
4. **方向继承按整向量**：append/insert 继承邻段全向量，各层同时继承，
   无需按层特判。

### 6.5 身份层遗留归属

需要独立身份/生命周期的重叠标记（书签、诊断对象）仍归上层独立结构，
选型由语法高亮引擎/LSP 集成细节决定（未定）。层向量已覆盖"跨层删 B
恢复 A"，身份层的职责收窄为：对象生命周期管理 + 向对应层输出染色。

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
  L1 混合器。**gravity 在身份层模拟**（漂移收敛规则见
  research_marktree.md 3.2 节：left→删除点、right→新文本后），
  spantree 本体不引入 gravity
- **ns 映射到混合器分组而非层**：ns 动态无限、层编译期固定，全部 ns
  进 L1，ns 作仲裁分组键
- `ephemeral` + decoration provider 与本设计"快层不进树 + 混合器
  拉模式"同构，理念兼容零成本
- 即使最终不逐字兼容，extmark API 也是身份层 API 的**免费需求规格**
  （经考验、文档完善），按其语义骨架设计避免闭门造车

## 九、开放问题

1. ~~属性表达：合成值 vs 按层分离值~~ **已决议：分层值（方案 A）**，
   理由从性能升格为第一性原理（层是属性维度非结构维度），见第六节。
2. **默认属性**：空文档、文档两端 append、全删后初始态的属性定义。
3. **规范形维护**：相邻同属性段须 merge（stitch 缝合点加同属性检查，
   判定为整向量比较）。piecetab 的 piece merge + checktree 已验证同类
   机制，照搬经验即可。
4. **混合器仲裁细则**：同层内插件间优先级/注册序语义、拉取回调的
   时序约定。
5. **层槽具体分配**：L2/L3 的语义指派（搜索、选区持久化？）与
   `SPAN_LAYERS` 取值。

## 十、风险与测试策略

- **方向语义贯穿调用链**：粘贴、redo、sam 命令、脚本编辑都须正确选
  append/insert。用错不崩溃只染错（软错误）→ **差分测试**：随机编辑
  序列后全量重染对比 spantree 状态。
- **写放大**：突变重染真重写段（extmark 只挤压位置）；换取读端零合成。
  渲染读多写少（见 research_marktree.md 第六节负载分析），交换划算；
  配合 viewport 懒染色控制重染范围。
