# sc（style compositor）孵化记录

> 状态：**已 C 化进 spantree.c（design_spantree_lua.md）**。本文档记录
> sc 孵化期形态与设计决策——editor.lua 现用 `sp.compositor()`（行为
> 等价），合成由 spantree 树 arbiter 承担。

## 一、定位

真实世界的 attr 字段化属性（fg/bg/布尔 attr 键）→ 唯一 32-bit handle。
cellgrid 的 style id 是**不透明 handle**（零改动），sc 负责 attr → handle
转换 + 逆查 + 动态 CSI 生成。

**架构原则**（用户裁定）：attr 层是自由空间（Lua table 任意混合/计算，
甚至混合背景色），sc 是**覆盖结束后的唯一一次编码**——合成器在 attr
空间做 partial fold，sc 只做终端 intern。

## 二、实现形态（editor.lua Section 3）

```
sc（editor.Sc 类，Term 式内嵌，Ed.newsc() 工厂）
├── intern(attr) → id    规范形 key + hash 复用；style 0 = 空 attr 预置
├── attr(id) → attr      逆查（不拷贝，调用者不得改）
└── csi(id) → SGR        动态生成："\27[0m" + 排序后的属性码
```

- **attr 表示**：`{fg=色值, bg=色值, bold=true, ...}`；fg/bg = 256 索引
  整数或 `{r,g,b}` 真彩表；布尔键 = 置位属性。nil/false = 未设置（跳
  过规范化与合成）。
- **规范形**：排序 `"k:v"` 串拼接（真彩表 → `"k:rgb(r,g,b)"`）；字段序
  无关、false/nil 等价于无。
- **SGR 顺序确定性**：codes 收集后 `table.sort`（attr 数字码在前，38
  fg 次之，48 bg 最后）——pairs 遍历序随机，不排序输出不稳定。
- **合成器 merge_layers**（editor 内，非 sc 职责）：k 层 spans →
  边界收集 → 逐段键级 partial fold（高层设的键覆盖、低层透传）。
- **写者协议**：`query_region(s, e) → {offset, length, attr}[]`
  （0-based）。ts 写者有缓冲（增量树，直写层）；piece 写者无缓冲
  （拉模式现场扫描，交替灰底）。

## 三、决策记录

| 议题 | 决策 | 理由 |
|---|---|---|
| intern 粒度 | 组合级（完整 attr 状态一次哈希） | attr 层自由混合/计算后一次编码；位域方案（字段级 intern + 拼装）否决——真彩/hint text 编不进固定位域，"选"语义做不了计算 |
| cellgrid style 语义 | 保持不透明 handle，**零改动** | handle 本就是抽象；绑定 `lcg_styleof` 改 `lua_geti`（`lua53_geti` shim，5.1/5.2/LuaJIT 用 gettable 模拟）使 diff 接受 __index 代理表 |
| 高盖低 | **禁止** | 合成必须 partial 字段合并；L1 盖 L0 是错误语义 |
| 渲染接入点 | 合成在 query 层完成，render_line/line_segments 零改动 | 合成后 spans 带 handle，下游不变 |
| 颜色空间 | 值形态决定 CSI 输出（整数=256 索引、{r,g,b}=真彩），不预选 | 零预决策；LSP semantic tokens 给 RGB 时直接可用 |

## 四、演进点（后续评估）

- **CSI 缓存**：sc 内部 id→SGR 串缓存可加（当前每帧每 handle 重拼串，
  editor demo 规模无感；`sc:csi` 是纯函数调用频率 = style 变化次数）
- **满表策略**：32-bit 递增分配，工作集内用不完；LRU 淘汰免（YAGNI）
- **自定义 fold**：merge_layers 目前键级 partial 硬编码；计算混合
  （如背景色混合）走自定义 fold 回调——原型未显需求，先不定 API
- **C 化评估点**：sc 接口固化后判定——
  - canon 规范化 + hash intern + 逆查 = 通用表问题（stb 候选）
  - SGR 生成 = Lua 特有（Lua C 模块候选）
  - merge_layers = 区间代数（与 spantree 层折叠同族，spantree 落地时
    一起评估）

## 五、与 hint text 解耦（记录）

hint text（virt_text）是**渲染注入**（向渲染流插内容），**不进 sc
intern**——sc 只管染色字段。多层高亮与本特性无依赖关系；hint text
后续基于多层基础单独设计（design_spantree.md §8.1 "渲染注入不承诺"
是否推翻待 hint 设计时裁定）。

## 六、测试

editor_test.lua：TestSc（intern 复用/区异/顺序无关/未设置跳过/逆查/
CSI 6 项）+ TestLayers（piece 交替/命令切换/合成/跨行/跨 piece 字符串
段 6 项）。全部 grid cell style 断言（双写者共存、键级 partial）。

**教训**：测试类定义在 `os.exit(lu.LuaUnit.run())` 之后 → 永不执行。
luaunit 收集在 run() 时遍历 _G——测试组必须定义在 os.exit 之前。
