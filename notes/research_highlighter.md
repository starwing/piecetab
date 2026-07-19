# 语法高亮器输出与增量更新调研

> 调研对象：tree-sitter（neovim 集成源码）、vis/Scintillua LPeg lexer
> （本地源码）、VSCode TextMate grammar、LSP semantic tokens（neovim
> 客户端源码）。
> 调研目的：确定 spantree（`notes/design_spantree.md`）上层高亮引擎的
> 输出形态与增量策略，回答"引擎输出什么、编辑后如何避免全文重 parse"。
> 源码位置：`~/Work/Sources/vis`、`~/Work/Sources/neovim`。

## 一、四种引擎概览

| 引擎 | 输出形态 | 增量单位 | 持久状态 |
|---|---|---|---|
| tree-sitter | 持久 CST + query captures → (range, capture) 流 | changed ranges（结构共享重 parse） | 整棵语法树 |
| vis (LPeg lexer) | 扁平 token 数组 `{tag1, end1, tag2, end2, ...}` | **无增量**：viewport+horizon 每帧全量重 lex | 无 |
| VSCode TextMate | 逐行 token[]（行内 offset + scope 栈）+ 行尾状态 | 行级：状态缓存 + 收敛即停 | 每行尾 rule stack |
| LSP semantic tokens | uint 数组，5 个一组差分编码 | 协议级 delta（数组 splice）+ range 请求 | 服务器持有（resultId） |

## 二、各论

### 2.1 tree-sitter

**输出**：持久具体语法树（CST）。节点内部存**相对位置**（与 marktree
同思想，支持编辑平移）。高亮不是树本身，而是 query 层：
`highlights.scm` 的 S-expression pattern 把节点映射为 capture
（`@keyword`、`@function`），query cursor 按区间迭代产出
(node range, capture) 流。

**增量三步**：

1. `tree:edit(start_byte, old_end_byte, new_end_byte, start_point,
   old_end_point, new_end_point)` —— 把编辑告知旧树，节点位置平移
   （languagetree.lua:1196，由 buffer `on_bytes` 回调驱动，
   languagetree.lua:1269-1301）
2. `parser:parse(old_tree, input)` —— 以旧树为基础重 parse，**未受
   影响的子树直接结构共享复用**，成本 ~O(编辑影响范围)
3. `ts_tree_get_changed_ranges(old, new)` —— 返回新旧树差异区间列表，
   只重染这些区间（highlighter.lua:253-265 `on_changedtree` →
   按 range 触发 redraw）

**渲染端**：neovim 用 decoration provider 的 `on_win`/`on_line`，只对
可见行跑 query 并发 ephemeral extmark（highlighter.lua:551
`_on_win(_, _, buf, topline, botline)`），且 `parse({topline, botline})`
支持**按区间 parse**。注入语言（injection）= 递归子
LanguageTree，各语言独立成树（languagetree.lua:705）。

**特点**：容忍语法错误（错误恢复，局部错误不毁全树）；编辑中不完整代码
可用。弱点：树概念上覆盖全文，4GB 文件整树内存/首次 parse 不可行——
nvim 靠 range parse + 异步缓解，但模型本质是全文树。

### 2.2 vis（Scintillua LPeg lexer）

**输出**：`lexer:lex(data, 1)` 返回扁平数组
`{tag1, end1, tag2, end2, ...}`（tag=token 名，end=结束位置后一位，
1-based）。vis-std.lua:66。token 名经 `_TAGS` 映射到 style，
`win:style(style, start, end)` 逐段上色（vis-std.lua:69-80）。

**增量：没有。** 每次 `WIN_HIGHLIGHT` 事件（每次重绘）：

```
lex_start = viewport.start - horizon   -- horizon 默认 32KB，可配置
data = file:content(lex_start .. viewport_end)
tokens = lexer:lex(data)               -- 全量 lex 这一小段
只对落入 viewport 的 token 调 win:style
```

（vis-std.lua:51-81；horizon 定义 vis-std.lua:45-49）

**horizon 是"回看赌博"**：从视口前 32KB 开始 lex，赌 lexer 状态到达
视口时已收敛（处理跨视口的多行注释/字符串）。源码自注
`TODO: improve heuristic for initial style`——超长多行结构会染错。

**特点**：零持久状态、实现极简；**对巨文件天然免疫**——每帧成本恒为
O(horizon + viewport)，与文件大小无关。代价：每帧重复劳动（无缓存）、
horizon 不足时染错。

### 2.3 VSCode TextMate grammar

**输出**：按行。`tokenizeLine(lineText, prevState)` → 行内 token 数组
（offset + scope 栈，如 `["source.c", "string.quoted", ...]`）+
行尾状态（rule stack）。scope 栈经 theme 规则匹配得最终样式。引擎是
Oniguruma 正则状态机。

**增量：行状态收敛模型**。每行尾 rule stack 被缓存；编辑第 n 行后从
n 行重新 tokenize，**当某行算出的行尾状态与缓存相等时停止**。典型几行
内收敛；最坏（如改动打开/闭合多行字符串的界符）传播到文件尾。后台
线程分批 tokenize，避免阻塞输入。

本质是 Scintilla 同款模型的严谨版：行 = 增量单位，行边界状态 = 收敛
判据。vis 的 horizon 是此模型去掉状态缓存后的赌博近似。

### 2.4 LSP semantic tokens

**输出**：uint 数组，每 5 个一组
`(deltaLine, deltaStartChar, length, tokenType, tokenModifiers)`——
**token 间差分编码**（协议压缩；解码见 semantic_tokens.lua:103-129，
`delta_line==0` 时 start_char 也是相对前一 token）。tokenType/modifiers
是索引，指向握手时协商的 legend 表。

**增量：三种请求**：

1. `full`——全量数组
2. `full/delta`——客户端带上次 `resultId`，服务器返回
   `edits[] = {start, deleteCount, data}`，即**对上次 uint 数组的
   splice 操作列表**（客户端应用见 semantic_tokens.lua:530-546：排序
   edits、逐段拼接新数组）
3. `range`——只请求指定区间（viewport 快速上屏用）

**滞后语义**：编辑到响应之间存在空窗，客户端保留旧结果继续显示（位置
近似），响应到达且 version 匹配才替换。语义层**叠加**在语法层之上
（优先级更高），是补充不是替代。

## 三、共性归纳：增量三板斧 + 一个正交手段

1. **状态收敛**（行级，TextMate/Scintilla）：缓存每行边界状态，编辑后
   从受影响行重跑至状态与缓存吻合。适合正则/状态机 lexer。前提：状态
   可比较、可序列化。
2. **结构共享**（树级，tree-sitter）：旧树 edit 平移 → 重 parse 复用
   未变子树 → changed_ranges 给出最小重染区间。
3. **差分传输**（协议级，LSP）：计算方持有上次结果，传输 splice 编辑。
4. **viewport 惰性**（正交，全员采用）：vis 只 lex 视口+horizon；nvim
   `on_win` 只 parse/query 可见行；LSP 有 range 请求。**没有引擎真正
   全文实时重算**。

三板斧的统一本质：**找到"编辑影响的最小区间"，区间外结果复用**。
差别只在定界手段——状态相等（行）、子树相等（树）、服务器 diff（协议）。

## 四、对 spantree / piecetab 的结论

1. **输出可归一**：四种输出都归约为"(区间, 类型) 有序流"——正是
   spantree 染色写入的输入形态。spantree 写接口定为一种即可：
   **区间替换**（重染 [a,b) 为新段序列，lc_splice 式），引擎给出的
   changed_ranges / 收敛区间 / delta 全部映射到它。
2. **分块方案与 4GB 目标最合的先例是 vis + TextMate 的合体**：
   vis 证明 O(viewport+lookback) 每帧可行且零状态；TextMate 行状态
   收敛是其严谨化（用状态缓存替代 horizon 赌博）。piecetab 的按文件
   类型分块 = 把"状态安全点"从行边界抬到块边界：块首状态已知（或定义
   为初始态），块内引擎自选（LPeg / tree-sitter / TextMate 兼容层），
   编辑后块内重染、状态不收敛则传播到下一块。块表本身可用 spantree
   存（块即段）。
3. **"删 A 露下层"的栈语义定位**：tree-sitter 模型内天然（重 parse 后
   injection 层消失、外层接管）；分块模型内 = 块重染。真正需要保留
   栈/层的是**多来源叠加**（语法 + LSP 语义 + 搜索/选区），这属于
   染色合成层（身份层）职责，跟单个引擎的增量机制无关——引擎只管吐
   本层的 (区间,类型) 流。
4. **编辑与染色天然异步**：所有引擎都是"编辑先行、染色滞后"，空窗期
   旧染色靠**位置平移**维持近似显示。spantree 度量树平移免费，正好
   支撑此模式：旧段随编辑自动平移（内容近似、位置精确），重染结果
   到达后区间替换。这反过来验证了 spantree 选度量树的正确性。
5. **tree-sitter 若要支持**：不能全文建树，只能块内建树（块 = parse
   单位，块间无跨块语法结构的妥协），或仅对小文件启用整树模式。
   LSP semantic tokens 天然区间化（range 请求），与分块无冲突。

## 五、参考源码索引

| 主题 | 位置 |
|---|---|
| vis 每帧重 lex + horizon | vis/lua/vis-std.lua:45-81 |
| vis lexer 输出格式 | vis/lua/vis-std.lua:66-80, vis/lua/lexer.lua |
| ts 编辑平移 | neovim/runtime/lua/vim/treesitter/languagetree.lua:1184-1301 (`_edit`, `_on_bytes`) |
| ts 区间 parse + 注入递归 | languagetree.lua:637-710 (`parse`, `_parse`) |
| ts changed ranges → 重染 | neovim/runtime/lua/vim/treesitter/highlighter.lua:253-265 (`on_changedtree`) |
| ts viewport 渲染 | highlighter.lua:551-610 (`_on_win`) |
| LSP token 差分解码 | neovim/runtime/lua/vim/lsp/semantic_tokens.lua:84-129 (`tokens_to_ranges`) |
| LSP delta 应用 | semantic_tokens.lua:504-574 (`process_response`) |
