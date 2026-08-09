# highlighter 设计（语法高亮分块调度库）

> 状态：讨论定案（v2 重构），待实施。背景：editor.lua 的
> `hl.build_regions` 是 piece 交替假高亮（`kind = i % 2`），无真实语义。
> 目标：设计 `highlighter.h` —— stb 风格独立库，抽象语法高亮引擎
> （tree-sitter / LPeg / TextMate 等），**一上来支持区域建树**（渲染
> 大文件时只解析部分、只查询 viewport，成本与文件大小无关），在
> editor.lua 中验证真实语法高亮。
>
> 调研背景：`notes/research_highlighter.md`（四类引擎输出归一化）、
> tree-sitter 源码（~/Work/Sources/tree-sitter）、neovim 渲染源码
> （~/Work/Sources/neovim）。设计方法论：**全量路径反推**——从 editor.lua
> 消费端需求 → Lua 绑定接口 → 高亮器实际能力 → 库服务范围。
>
> v2 重构动因：v1 把库设计成薄壳（产出整理 + 名字映射），其中映射在
> Lua 层更合适（哈希表），裁剪/排序引擎天然能做（set_byte_range），
> 库无实质工作。v2 定位：**分块调度器**——块管理、引擎实例生命周期、
> edit/request 路由、区域建树。

## 一、定位

highlighter.h 是**区域建树的分块调度器**：

- **文档按固定大小分块，每块一个引擎实例**（parser/tree/query 均为
  块内独立）——建树成本 O(chunk)，与文件大小无关
- request(viewport) 只触及覆盖块 → 只解析覆盖块、只查询 viewport
- 引擎注入式（对齐 termfeed `tf_setlookup` 惯例）：库本体零外部依赖
- **不存储染色结果**（产出流直交调用者，调用者写 cellgrid）
- **不依赖 cellgrid**（零耦合，产出字节级 spans）
- **不做 capture 名 → style 映射**（归 Lua 绑定层，哈希表天然适合）
- **不做产出合并**（相邻同 style 段合并归 Lua 层，一次线性扫描）

与 spantree 的关系：spantree 是染色结果持久存储（旁路需求）。highlighter
不依赖 spantree——neovim 依赖 marktree 是其 Lua 无 grid 接口所致；本项目
cellgrid 直接可写，无需中间存储。spantree 落地后可作为产出流消费者。

## 二、全量路径（设计依据）

### 2.1 editor.lua 消费端需求

现有渲染管线（editor.lua Section 3 + render）：

```
hl.build_regions(doc)     -- 全文 region 列表（现为 piece 交替假高亮）
hl.line_segments(regions, line_start, line_end)  -- 行内段裁剪
render_line(g, row, col, text, segs, tabstop)    -- 单遍切换 style + batch putline
```

需求形态：**文档 + 行字节区间 → (start, len, style_id) 段列表**；
编辑后增量重染；按文件类型选引擎；调用者写 cellgrid。

### 2.2 neovim 渲染路径对照（合理性验证）

neovim 流程：`_on_win(topline, botline)` → 只对可见行跑 query
（`iter_captures` 带范围，iter 缓存跨行）→ capture → hl id（缓存）
→ ephemeral extmark 中间存储 → drawline 逐字符查 extmark + 优先级合并。

| 环节 | neovim | editor.lua | 判定 |
|---|---|---|---|
| 可见区惰性 | `_on_win` 只染可见行 | 每帧只算可见行 | 同构 ✅ |
| 行内段渲染 | 逐字符查 extmark + 多层优先级 | 段列表预裁剪 + batch putline | 同构且更简 ✅（无多层叠加需求） |
| 范围 query | `iter_captures` 带范围，iter 缓存跨行 | **每行独立计算** | 改进点 ⚠️ |
| capture → hl | `hl_cache[capture]` 缓存 | 无 | 改进点 ⚠️（Lua 层映射缓存） |

**结论：editor.lua 接口形态合理**（与 neovim 同构）。计划内改进：
1. **一次 request 可见字节区间**（对齐 iter 跨行缓存），行裁剪留 editor.lua
2. **capture 名 → style id 映射缓存**（Lua 表缓存，对齐 hl_cache）
3. 保留 `render_line` 单遍切换逻辑（已验证正确形态）

neovim 传统 syntax 确认：legacy syntax 引擎（src/nvim/syntax.c）属行状态
收敛模型（TextMate 同类，research_highlighter.md 2.3 已覆盖），与
tree-sitter 互斥；归一化接口结论不受影响，无实现动机。

### 2.3 高亮器实际能力（接口下界）

| 能力 | tree-sitter (0.26) | LPeg/Scintillua | TextMate |
|---|---|---|---|
| 输出 | `next_capture` → capture 名 | `{tag1,end1,...}` 扁平数组 | 行内 token[] + 行尾状态 |
| 增量 | `ts_tree_edit` + 重 parse + changed_ranges | 无（全量重 lex 区间） | 行状态收敛 |
| 区间查询 | `set_byte_range` ✅ | lex 输入即区间 | tokenizeLine 按行 |
| 异步 | `progress_callback` | — | 后台线程 |

**归一化共识**：引擎输出 =「(区间, 类型) 有序流」；输入 =
`notify_edit(start, old_len, new_len)` + `request(range)`。

**tree-sitter 关键设施**（供适配器使用）：
- `TSInput.read` 回调 —— 文本访问（见第三节数据输入）
- `ts_parser_parse_string` / `ts_parser_parse` —— 块内容解析
- `ts_tree_edit` + `ts_parser_parse(old_tree)` —— 块内增量
- `ts_query_cursor_set_byte_range` —— 区间查询（viewport 直接支持）

**多语言支撑点**：tree-sitter 社区标准 capture 名（`@keyword`/`@string`/
`@comment`...）跨语言统一；LPeg tag 名同词表 → **style 映射表 Lua 层
一份**，多语言/多引擎共用。

## 三、引擎接口（C 面）

引擎 = **一个块实例**的原子操作。对齐 cellgrid `cg_Diff` 惯例
（函数表 + self 指针，状态子类化，生命周期调用者/库管理）：

```c
/* spans yield: called by the engine during request; name = capture/tag
 * name (NOT a style id -- mapping is the Lua binding's job) */
typedef int hs_Sinkf(void *ud, size_t start, size_t end,
                     const char *name, size_t namelen);

typedef struct hs_Engine hs_Engine;

/* one engine instance serves one chunk (parser + tree + query state
 * lives in the adapter's subclass; hs_Engine is its first member) */
struct hs_Engine {
    int (*edit)(hs_Engine *E, size_t start, size_t old_len, size_t new_len);
    int (*request)(hs_Engine *E, size_t start, size_t end,
                   hs_Sinkf *sink, void *sinkud);
    void (*close)(hs_Engine *E);
};

/* engine factory: create one instance for a new chunk */
typedef hs_Engine *hs_Openf(void *ud);
```

- **edit**：块内编辑平移（增量 parse 前提；适配器内 `ts_tree_edit`），
  dirty 标记是引擎内务（request 时自决是否重 parse）
- **request**：保证该块已 parse 最新文本，query 产出 [start, end) 区间
  的 capture 名流，经 sink 回调调用者
- **close**：块实例销毁（块数变化时库调用）
- **openf**：工厂——库建新块时调用创建实例；语言绑定在 openf 的 ud 里
  （适配器按 ud 选语言）
- **数据输入**：引擎经**自己的 reader** 读文本——适配器构造时注入
  （tree-sitter 适配器把 piecetab 访问器封装进 TSInput.read 回调，
  pt_read 零拷贝适配）；库不碰文本数据，只管理块边界

## 四、库服务范围（分块调度）

**块模型**：块边界按 `chunksize` 从 0 对齐固定划分，块 i 覆盖
`[i*chunksize, min((i+1)*chunksize, len))`。编辑只改变文档长度
（块数增减）和块内内容（平移），**块号不变**——实例数量稳定：
内容变化走增量 parse（同一实例 E->edit 平移），仅块数增减时
创建/销毁末尾实例。

| 职责 | 说明 |
|---|---|
| 生命周期 | `hs_open(hs_Openf *openf, void *openud, size_t chunksize, hs_Allocf *f, void *aud)`；`hs_close` 销毁全部实例 |
| 长度跟踪 | `hs_edit(hs, start, old_len, new_len)` 更新文档长度，块数增减 |
| edit 路由 | 受影响块 [start, start+max(old,new)) 覆盖的块逐个 `E->edit`（区间裁剪到块内） |
| request 路由 | `hs_request(hs, start, end, sink, sinkud)` 定位覆盖块，逐块 `E->request`（区间裁剪到块∩请求区间），sink 直通（一层回调） |
| 产出裁剪 | 引擎产出超出请求区间时裁剪到 [start, end)（防御，引擎 set_byte_range 已限制） |

**明确不做**（归绑定/Lua 层或引擎）：

| 不做 | 归属 |
|---|---|
| capture 名 → style 映射 | Lua 层（哈希表） |
| 相邻同 style 段合并 | Lua 层（一次线性扫描） |
| 染色存储 | 调用者（cellgrid / 未来 spantree） |
| changed_ranges 暴露 | 引擎内务；viewport 惰性已够 |
| 异步/滞后染色 | 同步引擎第一步满足；`progress_callback` 留适配器 |
| 多层优先级叠加 | 单层足够；多源叠加归染色合成层（未来） |

**错误处理**：`HS_OK (0)` / `HS_ERRPARAM (-1)` / `HS_ERRMEM (-2)`
（对齐 CG_OK/TF_OK 惯例）；引擎返回非 0 时 request 中止并回传。

## 五、Lua 面设计（反推自 editor.lua 用法）

```lua
local hs = require("highlighter")

-- openf 为鸭子对象：open() 返回引擎实例 {edit=fn, request=fn, close=fn}
-- （tree-sitter 适配器绑定 / Lua fake 皆可）
local h = hs.new(openf, { chunk = 32768 })   -- 块大小可配

h:edit(start, old_len, new_len)              -- 编辑通知
local raw = h:request(s, e)                  -- → {{start, len, name}, ...}
```

**映射 + 合并（Lua 层）**：

```lua
local stylemap = { keyword = 1, string = 2, comment = 3 }  -- capture 名 → style id
-- 一次线性扫描：查表 + 相邻同 style 合并 → {{start, len, style}, ...}
local spans = merge_styles(raw, stylemap)
```

- 绑定层薄收集：名字流 → {start, len, name} 数组（C 面 sink 收集）
- 映射缓存：`local cache = setmetatable({}, {__index = stylemap})` 对齐
  neovim `hl_cache`（Lua 表天然缓存）
- 测试：Lua fake engine（鸭子类型，对齐 fake term 注入模式）

## 六、editor.lua 接入计划

1. **hl 模块替换**：`hl.build_regions` 删除；`hl.line_segments` 保留
   （输入改 spans 流）；新增合并/映射 helper（第五节）
2. **渲染接入**（Ed:render）：一次 `hl.request(scroll_start, visible_end)`
   拿全可见区 raw → 映射合并 → 逐行 `line_segments` 裁剪 →
   `render_line` 不变
3. **编辑通知**：doc edit 集中路径调 `ed.hl:edit(start, old_len, new_len)`
   （insert x/dd/undo/redo/open_line/:e 全量重建）；:e 换文件重建 highlighter
4. **引擎创建**：`Ed.open/new` 按扩展名选语言（第一步：c/lua 两类 + nil
   无高亮）；stylemap 注册（DIFF_STYLE 扩展 + STYLE 常量）
5. **样式表**：DIFF_STYLE 增加语法高亮 style id → CSI 映射（颜色表）

## 七、文件布局

```
highlighter.h            -- stb 本体：分块调度（块表 + 路由 + 裁剪，零依赖）
treesitter.h             -- stb 适配器：IMPLEMENTATION 时链 libtree-sitter
lua/highlighter.c        -- 本体绑定（薄收集，含 openf 鸭子接口）
lua/treesitter.c         -- 适配器绑定（Lua 侧注入 openf）
tests/highlighter_test.c -- fake openf + 块调度逻辑，100% 行覆盖
tests/treesitter_test.c  -- 适配器测试（链 libtree-sitter + grammar）
lua/tests/hs_test.lua    -- Lua 面测试（fake engine 注入）
```

外部依赖：`libtree-sitter`（brew 0.26.11 已装）；grammar 仓库
（tree-sitter-c / tree-sitter-lua）clone 后编译 `src/parser.c`
（现代 grammar 仓库直接带生成好的 C 源码）。

## 八、测试策略

- **highlighter.h**：fake openf（记录实例创建/销毁）——块管理（边界、
  增删、edit 路由）、request 路由（覆盖块、区间裁剪）、错误路径，
  100% 行覆盖铁律，零外部依赖
- **treesitter.h**：真实 tree-sitter + grammar——块内增量
  （edit → parse）、区间查询、跨块一致性（块边界处语法截断的
  预期行为：块边界语法结构被截断 → 高亮缺失，记录为已知妥协）
- **Lua 面**：hs_test.lua fake engine + editor_test.lua 真实接入断言
  （grid cell style 断言）
- 差分测试（可选）：随机编辑序列后全量重染对比

## 九、实施顺序

1. `highlighter.h` + `tests/highlighter_test.c`（分块调度，无依赖）
2. `lua/highlighter.c` + `hs_test.lua`
3. `treesitter.h` 适配器 + 测试（拉 grammar 依赖）
4. `lua/treesitter.c` 绑定
5. editor.lua 接入（六节）+ editor_test.lua 高亮断言
6. README 更新

## 十、已知妥协与未来

**块边界语法截断**：块内独立建树，跨块语法结构（如多行字符串跨块）
在边界处截断 → 该处高亮缺失。缓解：chunksize 可配（语法结构通常
远小于块）；未来可在边界加重叠解析（块重叠 reparse 边界区）。

- 异步引擎（`progress_callback` + 滞后染色）
- 多语言注入（块内嵌语言树，injection）
- spantree 旁路接入（产出流消费者）
- TextMate/LPeg 适配器（名字空间统一走 Lua stylemap）
