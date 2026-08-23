# highlighter 设计（多策略调度框架 + 森林管理）——【草案】

> **状态：草案（愿景文档），v4 方法论转变，待 Lua 原型验证。** 经用户
> 讨论裁定：当前所有设计为「雏形」——概念方向对（纯机制、零策略；
> Engine/Provider/CaptureMap 概念；多层感知；森林 LRU），但接口细节
> 未经实现验证，**不得直接进入 C 实现**。
>
> **v4 方法论转变（用户裁定）**：采用 cellgrid 孵化路径——**先在
> editor.lua 里完整实现 tree-sitter 高亮（Lua 原型）→ 从中抽接口抽象
> → 在此基础上做 C 模块（treesitter.h 适配器）→ 最后抽其中的抽象做
> highlighter.h**。抽象失败则停在 C 绑定层（直接绑 tree-sitter，
> 不实现 highlighter.h）。
>
> 本文件当前价值：记录讨论收获（概念模型、tree-sitter 接口全景、
> 实证结论、虚表设计候选），作为 Lua 原型抽抽象时的参考。各节标注
> 状态：【定案】= 讨论确认；【候选】= 待原型验证。
>
> 背景：editor.lua 的 `hl.build_regions` 是 piece 交替假高亮。目标：
> 抽象语法高亮引擎（tree-sitter / LPeg / TextMate / 模糊 parser），
> 最终目标是**渲染超大文件**：建树成本与文件大小无关。
>
> v3 重构动因（保留）：v2「固定大小分块 + 块 = 引擎实例」经实验证伪——
> tree-sitter 从任意中间位置建树 = ERROR 垃圾树（exp1 实证：注释中间
> 起 parse，根节点全 ERROR），neovim 的区域 parse 仅 injection 专用
> （主语言整树 + 3ms 异步分片，大文件内存无保护）。v3 改为**结构边界
> 分段**：粗模型定位「完整结构单元」（函数体/ifdef 块/注释块），
> 细模型从段起点（重置点）精确解析——exp2 实证：函数体 `{...}`、
> `#if..#endif`、注释块单独 parse **全部 0 ERROR**。
>
> 调研依据：`notes/research_highlighter.md`（四类引擎归一化）、
> tree-sitter + tree-sitter-c 源码与实验（~/Work/Sources/）、neovim
> 渲染源码、vis/Scintillua 源码、vim `:syn sync` 机制（先例）。

## 一、定位

highlighter.h 是**多策略调度框架**，不为具体策略买单，为策略提供机制：

- **双模型引擎**：粗模型（快、近似、分段）+ 细模型（准、慢、精解析），
  同一引擎实例可提供两面（tree-sitter 适配器 = 模糊分段器 + ts 精解析）
- **四个机制**：粗→细两级解析、重置点、模式选择、后台解析预留
- **森林管理**：段 → 细引擎实例的 LRU 缓存（树复用，滚动不重建）
- **style_id 直出**：引擎产出调用者定义的 style id（非名字）——
  映射表在 Lua 绑定层（哈希表天然适合），C 库零哈希
- **不存储染色结果**（产出流直交调用者）、**不依赖 cellgrid**
- 引擎注入式（termfeed 惯例）：库本体零外部依赖

## 二、实证结论（设计基石）

| 实验 | 结论 |
|---|---|
| exp1：included_ranges / 截断文本从结构**中间**起 parse | ERROR 垃圾树（树根缩为区域、注释被当代码）——tree-sitter 无「带状态中间起」能力 |
| exp2：完整结构单元（`{...}` / `#if..#endif` / 注释块 / struct）单独 parse | **全部 0 ERROR**——段起点 = 语法安全点（重置点），tree-sitter 从初始态解析完整结构单元正确建树 |
| neovim 源码 | 主语言 region = 全文整树；区域 parse 仅 injection（区域边界 = 宿主语言结构边界 = 安全点）；大文件 = 3ms 异步分片 + 无内存保护 |
| vis/Scintillua | horizon 赌博（无状态重 lex，结构在 horizon 内正确否则染错）；vim `:syn sync` 同族先例（fromstart/minlines/match） |

**核心洞察**：任何引擎的「中间安全起点」都必须是**语法结构边界**——
行模型（LPeg/TextMate）的安全点是行尾状态收敛（近似），tree-sitter 的
安全点是**完整结构单元开头**（精确）。粗模型的工作就是找到这些边界。

## 三、引擎接口（C 面）

```c
/* styles are caller-defined opaque ids; the engine emits them directly
 * (the Lua binding maps capture names -> ids at adapter setup) */
typedef int hs_Sinkf(void *ud, size_t start, size_t end, unsigned style);

/* one coarse structure segment: [start, end) is a complete syntactic
 * unit; parse() may restart from segment starts (= reset points) */
typedef struct hs_Segment {
    size_t  start, end;
    unsigned style;   /* coarse style for the segment (0 = normal) */
} hs_Segment;

typedef struct hs_Engine hs_Engine;

struct hs_Engine {
    /* coarse pass: (re)segment the document; engine keeps segment
     * state, `dirty` = only resegment from `from` on */
    int (*segments)(hs_Engine *E, size_t from, int dirty,
                    hs_Segment *out, int n, int *pn);
    /* fine pass: parse [start, end) precisely; `reset` = a segment
     * start inside (or before) the range, safe restart point */
    int (*parse)(hs_Engine *E, size_t start, size_t end, size_t reset,
                 hs_Sinkf *sink, void *sinkud);
    /* edit notify: translate positions (trees move, segments invalidate) */
    int (*edit)(hs_Engine *E, size_t start, size_t old_len, size_t new_len);
    void (*close)(hs_Engine *E);
};

/* engine factory: create an instance bound to one language/doc */
typedef hs_Engine *hs_Openf(void *ud);
```

- **segments**：粗模型全文扫描（O(文件)，但成本极低——`{}`/`#if`/注释
  扫描器，微秒~毫秒级）；产出结构单元列表；`dirty`/`from` 支持增量
  重扫（编辑后局部重扫）
- **parse**：细模型段内精解析；`reset` = 段起点（安全重启点）；
  tree-sitter 适配器内部 = included_ranges 或独立 parse
  （段内文本 = 完整结构单元，exp2 已验证正确）
- **edit**：库路由到引擎（树平移 + 段失效）
- 数据输入：引擎经自己的 reader 读文本（适配器构造时注入，
  tree-sitter 封装进 TSInput.read，pt_read 零拷贝）

## 四、库服务范围（机制提供者）

### 机制一：粗→细两级解析

```
粗模型 segments() 全文分段（O(文件) 低常数）
  → 段表：结构单元列表（函数体/ifdef 块/注释/声明）
  → viewport 覆盖段 → 细模型 parse(段, 段起点)
  → 产出合并（细覆盖粗，粗兜底段内空白）
```

- 段 = **完整结构单元**（粗 parser 找 `{}` 配对、`#if/#endif`、
  注释边界、顶层声明边界）
- 段起点 = 重置点 = 细模型安全重启位置（exp2 验证）
- 建树成本 O(viewport 覆盖段)，与文件大小无关

### 机制二：重置点

- 粗模型产出段起点列表；细模型从最近的段起点开始解析
- 对齐 vim `:syn sync`：`fromstart`（全量）/ `match`（重置点匹配）/
  `minlines`（回看下限）——vim 的 sync 匹配即重置点先例
- 段起点必须是**结构边界**（非行首——行首对 tree-sitter 非安全点，
  exp1 实证）；行模型引擎的「重置点」= 行尾状态收敛（引擎内务）

### 机制三：模式选择（用户可配）

| 模式 | 语义 | 适用 |
|---|---|---|
| `HS_MODE_AUTO` | 粗→细（默认） | 任意大小 |
| `HS_MODE_FULL` | 细模型直接全文（跳过粗） | 用户指定小文件 |
| `HS_MODE_COARSE` | 仅粗模型 | 超大文件、性能优先 |

### 机制四：后台解析（预留，接口已兼容）

- parse 回调式（sink）天然异步友好；引擎可声明后台能力
- 库提供异步调度协议（第一次实现可同步；`progress_callback` /
  分片续解析是 tree-sitter 适配器内务，neovim 3ms 分片为参照）

### 森林管理（LRU）

- 库持有「段 → 细引擎实例」映射 + LRU 淘汰（上限可配）
- 滚动复用：段未失效（无 edit）→ 不重 parse，实例重用
- edit → 受影响段失效 + 位置平移；段合并/分裂（编辑改变结构）由
  segments 增量重扫发现
- 森林上限 = 可配（内存预算）；淘汰最久未用段实例

### 明确不做

| 不做 | 归属 |
|---|---|
| capture 名 → style 映射 | Lua 绑定层（表 → 适配器 capture 数组，O(1) 查） |
| 相邻同 style 合并 | Lua 层（线性扫描） |
| 染色存储 | 调用者（cellgrid / 未来 spantree） |
| 具体粗模型实现 | 引擎（tree-sitter 适配器的模糊分段器） |
| 具体线程调度 | 平台层（接口预留，第一步同步） |

**错误处理**：`HS_OK (0)` / `HS_ERRPARAM (-1)` / `HS_ERRMEM (-2)`。

## 五、Lua 面设计

```lua
local hs = require("highlighter")

-- engine 为鸭子对象（适配器绑定 / Lua fake），需提供 segments/parse/edit/close
local h = hs.new(engine, {
  mode = "auto",          -- "auto"|"full"|"coarse"
  cache = { max = 64 },   -- 段森林 LRU 上限
})

h:edit(start, old_len, new_len)      -- 编辑通知
for start, len, style in h:request(s, e) do  -- 迭代器（不收集数组）
  -- 直接写 cellgrid 或收集
end
```

**capture → style 映射（绑定层）**：

```lua
-- 适配器构造时：Lua 表 → 适配器内部 capture_id → style_id 数组
local engine = ts.new("c", {
  keyword = 1, string = 2, comment = 3, function_ = 4,
})
-- 绑定层编译 query 后按 capture 名查 Lua 表构建数组，sink 直出 style_id
```

- 迭代器模式（closure 式）：request 不收集数组，sink 逐段产出
- fake engine（鸭子类型）供 Lua 测试，零依赖

## 六、editor.lua 接入计划

1. **hl 模块替换**：`hl.build_regions` 删除；新增 `hl.request(ed, s, e)`
   迭代器包装 + 行裁剪（`line_segments` 保留，输入改 spans 流）
2. **渲染接入**（Ed:render）：一次 `hl.request(scroll_start, visible_end)`
   遍历迭代器 → 逐行 `line_segments` 裁剪 → `render_line` 不变
3. **编辑通知**：doc edit 集中路径调 `ed.hl:edit(...)`；:e 换文件重建
4. **引擎创建**：`Ed.open/new` 按扩展名选语言（c/lua 两类 + nil 无高亮）；
   模式 = auto（小文件编辑器场景可 full）；stylemap 注册
5. **样式表**：DIFF_STYLE 扩展语法高亮 style id → CSI 映射

## 七、文件布局

```
highlighter.h            -- stb 本体：机制框架（段表 + LRU 森林 + 路由）
treesitter.h             -- stb 适配器：模糊分段器 + ts 精解析
lua/highlighter.c        -- 本体绑定（迭代器 + openf 鸭子接口）
lua/treesitter.c         -- 适配器绑定（capture→style 数组构建）
tests/highlighter_test.c -- fake 双模型引擎，100% 行覆盖
tests/treesitter_test.c  -- 适配器测试（真实 grammar，分段/重置点/增量）
lua/tests/hs_test.lua    -- Lua 面测试（fake engine）
```

外部依赖：`libtree-sitter`（brew 0.26.11）+ grammar 仓库（tree-sitter-c
等，src/parser.c 直接编译）。

## 八、测试策略

- **highlighter.h**：fake 双模型引擎——段表管理（分段/失效/平移/合并）、
  LRU 淘汰、路由（模式选择、viewport→段）、编辑通知、错误路径
- **treesitter.h**：真实 grammar——粗分段（`{}`/ifdef/注释边界）、
  重置点（exp2 场景：段起点 parse 0 ERROR）、段内增量、跨段一致性
- **Lua 面**：hs_test.lua fake engine 迭代器测试 + editor_test.lua
  真实接入（grid cell style 断言）
- 差分测试（可选）：随机编辑序列后全量重染对比

## 九、实施顺序

1. `highlighter.h` + `tests/highlighter_test.c`（机制框架，无依赖）
2. `lua/highlighter.c` + `hs_test.lua`（迭代器 + 映射）
3. `treesitter.h` 适配器（模糊分段器 + ts 精解析）+ 测试
4. `lua/treesitter.c` 绑定（capture→style 数组）
5. editor.lua 接入 + editor_test.lua 高亮断言
6. README 更新

## 十、未来（接口不排斥）

- 后台解析调度（分片续解析，neovim 3ms 参照）
- 行模型引擎（LPeg/TextMate）适配器——重置点 = 行尾状态收敛
- 多语言注入（宿主语言段 → 注入语言细模型，injection 区域 =
  天然结构边界）
- spantree 旁路接入（产出流消费者）
- 粗模型增量重扫优化（编辑局部 resegment）

## 十一、讨论收获（v4 记录，供 Lua 原型抽抽象参考）

### 11.1 概念模型【定案】（原型中体现，接口待抽）

```
用户策略层（Lua 可实现）—— 组合机制：何时粗扫/精解析/如何同步/淘汰
  └─ highlighter 机制层 —— Provider 注册（内容访问，库持有）
        Engine 虚表注册（多层多实例）· 段表（维护+查询+失效）
        森林 LRU（上限可配）· 多层合并（优先级叠加）· 解析请求（区域+重置点）
          └─ hs_Engine 虚表（引擎 = 纯算法，不碰内容，库喂数据）
                └─ Provider：seek/read（字节级 + 行级）
                     CaptureMap（Lua 绑定层）：引擎 capture → 通用名 → style_id
```

- **纯机制、零策略**【定案】：highlighter.h 只给原子机制；策略由上层
  组合，**Lua 绑定层可预设打包常见策略**（如「指定同步机制」接口），
  避免 Lua 用户写模板代码
- **Provider 独立注册**【定案】：Engine 不提供内容支持；库持 Provider，
  从 Provider 取数据喂引擎；引擎不直接接触内容
- **无版本概念**【定案】：永远当前状态高亮；段表/森林暴露查询，
  同步方式由用户决定
- **多层感知**【定案】：每层 = 一个高亮源（不同渲染器），Lua 表可
  反向绑定成 hs_Engine（手写高亮器）

### 11.2 tree-sitter 查询接口全景【定案，虚表设计依据】

Query：`new/capture_name_for_id/quantifier/predicates/pattern 特性/
disable_capture/disable_pattern`
Cursor：`exec/set_byte_range(相交)/set_containing_byte_range(包含)/
set_point_range/next_match/next_capture/remove_match/set_max_start_depth/
set_match_limit`
Node：`start/end_byte/point、child/sibling 遍历、first_named_child_for_byte、
descendant_for_byte_range、has_error/has_changes`
增量：`ts_tree_edit + ts_tree_get_changed_ranges`
Parser：`parse/parse_with_options(progress)/set_included_ranges/set_language`

关键观察：
- 区域查询两种语义：**相交**（match 与范围相交即返回）vs **包含**
  （节点完整落在范围内）——虚表需明确用哪种
- `next_capture` = 按位置有序捕获流（渲染形态）；`next_match` = 按模式分组
- `descendant_for_byte_range` = 区域/重置点定位根基
- `remove_match` = 多层叠加去重机制
- `match_limit` = 引擎防爆闸门

### 11.3 虚表设计候选【候选，Lua 原型后裁定】

- 查询形态：range 拉（主）+ point 点查询（byte → style，Lua 手写
  高亮器兜底）+ line 行查询（TextMate 形态）——三种是否全进虚表待定
- capture 名 → style_id 映射链路（绑定层）：引擎 capture 名 → 通用名
  （@keyword 等）→ style_id；单表直映不足以覆盖多引擎归一
- sink 产出 style_id（非名字）：绑定层构建 capture_id → style_id 数组
  （O(1) 查表），C 库零哈希
- 内容通道：库把 Provider 包装成 reader（hs_Readf）传引擎 parse，
  引擎纯消费

### 11.4 原型经验（2026-08-10，editor.lua tree-sitter 高亮落地后）【定案】

**最小 API 面（editor.lua 实际用到）**：
- `ts.require(name)` + `ts.parser.new()` + `parser.language = lang`
- `parser:parse(oldtree, string)`（增量 + 全量）
- `tree:edit(9 参)`（字节 + 行列双坐标）、`tree.root`
- `lang:query(src)`、`query:exec(node)`、`cursor:set_byte_range`、
  `cursor:next_capture()`、`cursor[i]`（node）、`cursor:captures(i)`（node, capture_id）、
  `query:capture_name_for_id(id)`
- **未用到**：point 查询、行查询、match_limit、included_ranges、异步——
  原型「整树 + viewport 查询」即可满足 editor demo

**绑定 API 语义坑（已踩，文档须注明）**：
- **TSInput 回调的 byte 是 1-based**（绑定 `lts_pushindex` +1 惯例）——
  首次实现回调 `buf:read(byte)` 错位 1 字节，树整体偏移
- **`next_capture` 返回 match 内已消费捕获序号**（非 capture id！源码
  `state->consumed_capture_count`）——capture 名须经 `c:captures(ci)` 的
  第二返回值（真实 capture id，1-based）取
- **`"int"` 等 primitive_type 关键字无法字符串匹配**（grammar 内部 token
  折叠进父节点，query 编译报 node_type 错）——须匹配 `(primitive_type)`
- **`doc:buffer()` 默认返回 live buffer（含 uncommitted fresh 编辑）**；
  `doc:buffer(vid)` 才返回 committed 版本快照

**编辑通知接线痛点**：
- point 换算需 `doc:seek`（移动光标）——on_edit 内要保存/恢复
- 编辑路径分散（n.x/backspace/delete/Enter/Tab/open_line/insert_key/
  undo/redo/:e）——**收敛为 `Ed:docedit` 单漏斗** + undo/redo/:e 全量
  reset（重建树）——原型可接受，未来可 diff
- 增量 parse 依赖 `tree:edit` 平移正确；`doc:buffer` 坑导致树错位后
  增量 parse 保留旧结构（"eturn" 残留 keyword 高亮）——**确保 parse
  内容与 edit 平移一致**是增量正确的关键

**capture → style 映射实际形态**：
- editor.lua 用**单表**（capture 名 → STYLE）——c/lua 同构 capture 名
  （@keyword/@string/@comment/@function）——**单表够用**，11.3 的
  「通用名归一」在原型未显现需求（多引擎未接）
- query 源 = Lua 内嵌子集（HL_QUERIES），每语言一份——高亮器与语言
  query 强耦合（editor 内嵌），C 库抽离时应参数化

**粗→细/重置点在原型的出现形态**：无——editor demo 文件小，整树 +
viewport 查询够用。大文件路径未验证，虚表候选（11.3）保持不变。

**抽象成败判断（初步）**：editor.lua 的高亮逻辑（调度/映射/增量）约
120 行 Lua——**调度层有真实工作**（dirty 管理、edit 平移、viewport
查询、增量 parse），但**与 tree-sitter 绑定强耦合**（query 源、capture
语义、edit 参数）。抽 highlighter.h 的价值 = 把「edit 平移 + 区域请求 +
增量」机制化；抽抽象前须先解决 11.3 的虚表候选（Provider 喂内容、
引擎原子操作）与上述 API 语义坑的对应。**结论：抽象方向成立，但机制
设计须以本节的 API 语义为输入**。
