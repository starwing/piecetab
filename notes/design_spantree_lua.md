# spantree Lua 绑定设计（v4 定案）

> 状态：**v4 定案（2026-08-16，全案重写）**。v3（2026-08-16）
> 引入 eph 层；v4 推翻重建（触发：comp 域与 ns/id 体系的层级错
> 位——id 公用而 ns 私有；合成 id 构成与树段的 mask 双键撞车）：
>
> - **单对象**：`sp.new()` → Tree，**内嵌 Comp**——无独立
>   compositor 工厂；需要纯样式服务 = new 一个 Tree 只用样式
>   接口。一渲染窗（cellgrid）+ 一 Doc = 一 Tree。
> - **spantree.c 三块**：`cp_`（compositor 完整实现：attr intern
>   + ns 注册 + op 空间 + 合成 id 体系，全局）、`sv_`（minispan：
>   arrayvec 平铺 (off, id, len) 列表，无 mask）、`lst`（绑定层：
>   cp + sv + sp 统合，最终 Lua 接口）。
> - **ns/op/合成体系在 cp（全局）**——插件对每个文件一视同仁，
>   多树共享注册、优先级、op 与合成 id。
> - **id 分区**：普通区（attr id 与 op id 混排，树段区分靠 mask）
>   与合成区（≥ COMP_ID_START，稠密 k + freelist）。
> - **全 id refcnt**（树段引用计数）；合成 id ref 0 → 回收复用。
> - **合成 id = cons 链**（arb 二分性 → 左斜链）：每合成 id 固定
>   两槽 parents，展开即压缩（链长 ≤ 64 恒界）；链结构 hash 复用。
> - **用户 id 模型**：用户接触的一切 id 都是**非合成 id**（attr
>   id，intern 所得、attr() 可查）。合成 id / op id 是内部模拟
>   细节，用户零感知——span/next/prev 输出拆解后的 attr id、
>   styled() 输出渲染合成（返回的 id 同样 attr() 可查）。树内存
>   merged id 不变（既定方案），库有能力从 merged id 反推非合成
>   id（cons 链解码）——给用户提供非合成 id 的遍历/删除/新增
>   接口，内部用 merged id + spantree + sv + cp 模拟 Neovim style
>   行为。
> - **读路径双接口**：`span`（原 segments；标记流——拆解输出，
>   (id, off, len) 重叠可多出）与 `styled`（渲染合成流——四元组
>   合成结果）；cursor 的 next/prev/style 走标记流语义（拆解）。
> - **extmark 功能对齐**：span = get_marks（按 ns/区间遍历单个
>   标记）、unmark(id) = 按 id 删标记、clear(ns) = 批量清——
>   功能仿 Neovim，接口自拟。
>
> 依赖标注：spantree.h **API 无修改**，arb 回调按**三态契约**
> （design_spantree.md §9.7，2026-08-16 定案）保证"id 进出树必过
> arb"：新生 `arb(id, 0)`（写空段/分裂碎片/继承，必须原样返回
> id）、死亡 `arb(0, id)`（合并/删除/trim 归零/freetree）、合并
> `arb(id, old)`（出口双向计数；filterleaf 以保护 +1 防瞬态
> ref==0 误回收）。refcnt 全部 C 侧维护（无 Lua 调用路径）。

## 一、定位

单模块 `require "spantree"`，**单对象 Tree**（内嵌 Comp，无独立
compositor）。C 侧三块：

- **cp_**（compositor）：attr 表 ↔ style id 的 intern/逆查/字段
  白名单 + ns 注册表（名字制、优先级）+ op 空间 + 合成 id 体系
  （cons 链 + refcnt + 构成 hash）。**全局**——多 Tree 共享同一
  cp 态（实现 = 全局单例态或按需共享，实施定）。
- **sv_**（minispan）：naive 平铺 span 列表（arrayvec 存 off/id/
  len），接口尽量兼容 sp_（fill/clear/upper/lower/next/prev），
  **无 mask、无 arbiter、无 B+ 结构**。eph 层 = 每 ns 一个 sv_
  列表。
- **lst**（绑定层）：cp + sv + sp 统合，对外 Lua 接口。前缀 lst
  （完整功能绑定，非 spantree 专属）。

ns 即写者（extmark 同构：namespace = 贡献者身份）：merge map =
`(ns → attr)` 单槽对；ns 层按优先级折叠（高覆盖低，同优先级按
注册序，后注册覆盖）。ns 0 = 无归属（非注册槽，最低层，mask 0）。
ns/优先级注册于 cp（全局）。

eph 层（ns mode = "ephemeral"）：**不进树**。每 eph ns 一个 sv_
平铺 segment 列表（规范形：有序、不交叠、相邻同 id 合并）。
带 ns 读 = 直接读对应层；无参读 = 树流 + 各 eph 层 merge 合成：
p<0 的 eph 层叠树之上、p>=0 的叠树之下。eph ns 数量不限（不占
mask 位）；任何树编辑动词自动清空全部 eph 层。

**用户视角（Neovim style，v4）**：用户看到 ns、写 attr 表
（intern 得 id、fill 用 id）、span/next/prev 拿回**非合成 id**
（attr() 查回自己的 table）、styled 拿渲染合成结果。内部一切
合成概念（合成 id、op id、cons 链、refcnt）对用户透明——用户
接触的 id 与 Neovim 一样都是单个标记的 id。

**id 域模型（v4）**：

```
普通区 [0, COMP_ID_START)：attr id（用户面 id——intern 所得）与
  op id（(ns, attr) 全局 intern，内部用）混排——区分不靠 id 域而
  靠树段 mask：mask 0 = 平铺 attr 段、mask 非 0 且 id 普通 =
  单 ns op 段
合成区 [COMP_ID_START, ...)：cons 链节点（内部用，用户零感知）——
  k = id - COMP_ID_START 稠密 + freelist 复用
```

- **拆解输出 = attr id**：span/next/prev 的"非合成 id" = 构成里
  每个 op 的 attr 面（attr() 可查）；用户 unmark 也按 attr id。
- `t:attr(id)` 两区统一查（attr id 与合成 id 都返回 table——渲染
  端对 styled() 的返回 id 同样可查），零迁移。
- **eph 合成不进合成区**：styled() 的 eph 折叠 = attr 黑盒覆盖 +
  普通 intern（hash 幂等）——无生命周期、零 refcnt 参与。
- 合成区只服务树内持久 merged（写路径 arb 产物）。

**现有实现可复用清单**（v3 代码搬进 v4，避免重复开发）：

| v3 位置 | v4 去向 | 状态 |
|---|---|---|
| lcp_Comp 的 intern/canon/by_attr hash/setfields/attr（109-430 行） | cp_ 整体迁移 | 逻辑不变 |
| lsp_ephfill/lsp_ephclear/lsp_ephupper/lsp_ephlower/lsp_ephnorm | sv_ 整体迁移 | 算法不变（§3.4） |
| lsp_mergecalc 的边界扫描 + attr 覆盖 fold | lst 读路径 | 删 pairs 拆解、改 attr 黑盒 |
| epoch 守卫/cursor seek 锚定/iter 模式/arb pcall 事务性 | lst | 语义不变 |
| ns 名字制/flags 解析/查询双返回 | lst（注册表上移 cp） | 语义不变 + 全局化 |
| TestEph 全部用例、TestTree/TestMerge 大部分 | 沿用 | 合成流断言保持 |
| sp_stylemask 并入 sp_style | sp_style 加 pmask 出参（NULL 跳过），C 测试改 stylemask 用例 |

## 二、API 定案

```lua
local sp = require "spantree"

-- 单对象：Tree 内嵌 Comp（无独立 compositor 工厂；需要纯样式服务
--   = new 一个 Tree 只用样式接口）
local t = sp.new()          -- 一渲染窗 + 一 Doc = 一 Tree；cp 态
                            --   全局共享（多 Tree 共享 ns/op/合成
                            --   id 体系）

-- 样式接口（cp 面，Tree 直供——editor 现用 comp:intern/attr 迁移
--   为 t:intern/t:attr，零语义变化）
t:setfields(fields)         -- 字段白名单（乱序可，C 侧排序一次）；
                            --   默认 SGR 全集；未列字段忽略
t:intern(attr) → id         -- attr id（**用户唯一 id 面**）；canon +
                            --   hash 复用；0 = 空 attr 预置；__hash
                            --   元方法短路（返回值须字符串）直接作键
t:attr(id) → table          -- 逆查（两区统一——attr id 与 styled()
                            --   返回的 id 均可查；不拷贝，调用者
                            --   不得改）

-- ns 层（= 写者；名字制，ns id 不暴露；注册于 cp = 全局——任一
--   Tree 注册后共享该 cp 态的 Tree 均可见）
t:namespace(name) → p, mode -- 查询：优先级 + mode（nil = 普通、
                            --   "ephemeral" = 挥发）；未注册 = 单返 nil
t:namespace(name, p) → oldp -- 注册（p 数字，必传）；已注册 = 改优先
                            --   级（普通 ns 全树重折叠；eph ns 零树
                            --   操作）并返回旧优先级；首次注册 nil
t:namespace(name, p, flags) → oldp
                            -- flags 字符串，逐字符解析："c" = 严格
                            --   注册（撞名报错）、"e" = ephemeral
                            --   （可组合 "ce"/"ec"）；非法字符报错；
                            --   无 "c" 撞名 = 改注册（mode 不同 =
                            --   注销重注册，旧层数据随注销清除）
t:namespace(name, nil) → oldp
                            -- 注销：普通 ns = 各共享树 sp_clear 剪枝
                            --   清该 ns？——**v4 待定**：注销只清当前
                            --   树的层 + 注册表全局移除（他树残留段
                            --   变孤儿构成，展开时该 ns 已无注册
                            --   优先级——按 op 内嵌 ns 号处理）；
                            --   eph ns = 清空该 ns 列表并释放；
                            --   未知名报错；返回旧优先级

-- tree: 染色/编辑（offset 制，内部栈游标；fill 返 id，其余返 self）
t:bytes() → n
t:mark(ns, attr_or_id, off, len) → id
                            -- **载荷双形态**：attr table（内部
                            --   intern）或 attr id（t:intern 所得）；
                            --   **返回 attr id**（extmark set 返 id
                            --   同款——用户存它供 unmark/复用）；
                            --   ns = 名字（须已注册，未知名报错）；
                            --   nil = 无归属（ns 0）；eph ns → 写该
                            --   ns 的 sv_ 列表（覆盖切分 + 规范形），
                            --   零树操作、零 epoch；普通 ns → 树
                            --   fill
t:clear(ns)                 -- 全树清该层（普通 = sp_clear 剪枝；
                            --   eph = 清空该 ns 列表；未知名报错）
t:clear(ns, off, len)       -- 区间清该层（eph = 列表区间裁剪；
                            --   未知名报错）
t:clear(nil, off, len)      -- 区间全清（树 + 全部 eph 列表）
t:clear()                   -- 全树全清（树 + 全部 eph 列表）
t:splice(off, del, ins)     -- 删 del 插 ins（继承左段）；**自动
                            --   清空全部 eph 列表**
t:append(off, ins)          -- 插 ins（继承左）；自动清 eph
t:insert(off, ins)          -- 插 ins（继承右）；自动清 eph
t:remove(off, len)          -- 删 [off, off+len)；自动清 eph

-- tree: 读——双接口（v4）：span = 标记流（拆解）、styled = 样式流
t:span(ns?, off, len) → iter  -- **标记流**：for off, len, table, id in
                              --   ...（四元组；id 殿后，用户可忽略）；
                              --   id = **非合成 id（attr id）**、
                              --   table = 对应 attr 表（免再查）；
                              --   拆解输出：一个 merged 段含多标记时
                              --   **重叠多出**（同区间逐标记输出，按
                              --   优先级序）；ns 过滤 = 只出该 ns 的
                              --   槽（eph ns = sv_ 列表步进，无拆
                              --   解）；未知名报错；2 参 = (off, len)；
                              --   **get_marks 功能** = 本接口
t:styled(off, len) → iter   -- **渲染合成流**：for off, len, table, id
                              --   in ...（四元组）；table = 合成 attr
                              --   表、id = 合成结果（树段 id / 平铺
                              --   id / eph 折叠 intern id——attr() 均
                              --   可查，用户可直接忽略）；语义沿 v3
                              --   合成流（树 + 全部 eph 层边界切分
                              --   fold）
t:cursor() → c                    -- 新 cursor 句柄（创建于树头 off=0）
t:seek(off[, c]) → c              -- 游标入口：无 c → new；有 c →
                                  --   复用它定位到本树 off（悬垂 c
                                  --   亦可——seek 重建 paths）

-- tree: 标记删除（extmark del 功能）

t:unmark(id) → n         -- 按 **attr id** 全树扫描：段构成含该
                            --   id（op 的 attr 面匹配）→ 清该槽
                            --   （arb CLEAR(ns)），返回清除段数；
                            --   id 可重复用（同 table 多标记全删）；
                            --   未知 id 返 0

-- cursor: 定位（seek 可跨树重绑定；均返回 self）
c:seek(t, off)             -- 重定位（sp_seek 重建 paths，编辑后 seek =
                          --   复活）；换树同时换 uservalue 锚定
c:locate(off) / c:advance(d)   -- 增量移动（近跳）
c:offset() → off

-- cursor: 读——标记流语义（拆解输出，同 span 四元组）
c:style() → off, len, table, id | nil -- 当前标记（cursor 位置处按
                            --   段内标记索引取一个）；id = attr id
c:next(ns?) → off, len, table, id | nil -- 下一标记：同段下一槽（标
                            --   记索引 +1）或下一段首槽；ns = 普通
                            --   → 只出该 ns 槽（sp_next(ns) 剪枝 +
                            --   拆解）；eph ns → sv_ 步进；nil = 任
                            --   意标记
c:prev(ns?) → off, len, table, id | nil -- 对称向前（标记索引 -1 /
                            --   前段末槽）

-- cursor: 游标制染色/编辑（C 原生动词零 seek；普通 ns 编辑 epoch++
--         自同步；eph fill/clear 零 epoch；返回 self；收尾位置 = C
--         语义：append 落插入段尾、insert 回插入点、splice 于插入
--         段尾、remove 光标不动、fill 落填充段尾（poff=段长 →
--         style() 返 nil））
c:mark(ns, attr_or_id, len) → id
                            -- 载荷双形态（table/attr id），返 attr
                            --   id；eph ns → 写该 ns 列表
c:clear(len)                -- 从游标全清 len 字节（树 op 0 + 全部
                            --   eph 列表区间裁剪）
c:clear(ns, len)            -- 从游标清该层 len 字节（eph = 列表区间
                            --   裁剪；普通 = sp_fill CLEAR op）
c:splice(del, ins)  c:append(ins)  c:insert(ins)
c:remove(len)               -- 内部第二游标 seek+advance(len)，
                            --   sp_remove(&C, &R)；编辑动词自动清
                            --   空全部 eph 列表
                            -- 注：c:clear 无无参形态——全树清是树级
                            --   剪枝（t:clear(ns) / t:clear()）
```

- 返回 nil 对齐 C 值返回 0（sp_style/sp_next/sp_prev 段尾/越尾）。
- **读路径双接口语义**（v4）：span/next/prev/style = 标记流——
  **拆解输出 attr id**（cons 链展开取槽；一个 merged 段多标记 =
  重叠多出，按优先级序）；styled = 渲染流——树段 id 直出 + eph
  层按边界切分 fold（attr 黑盒覆盖 + 普通 intern 幂等）。cursor
  增**段内标记索引**状态（同段多标记逐个输出；epoch 守卫同款，
  任何树编辑后失效）。
- styled 合成语义：树段 + 全部 eph 层按边界切分 merge；每子区间
  fold = 层序 p>=0 的 eph（升序）→ 树 → p<0 的 eph（升序），后
  层覆盖前层。fold = attr 黑盒覆盖（attr(树 id) 与各 eph 层 attr
  表按层序覆盖 → intern 幂等）——零构成拆解、零 mapof、零 mask；
  单层区间直返原 id 零合成。
- 读路径分层：span/styled = 快路径（内建游标，1 userdata/次调用，
  O(1) 步进）；cursor = 复用句柄（创建后反复 seek/编辑/next，免
  反复分配）。无状态每调用 sp_seek 的原语（span/find）否决——
  每段 O(levels) seek 不可接受。
- 遍历产出 id 经 `t:attr(id)` 解码——两区统一（attr id 与 styled
  返回的 id 均可查），渲染路径（editor.lua Ed:csi）改走 styled。
- epoch 语义（v1 沿革）：**树编辑** = epoch++（含普通 ns 的 fill/
  clear、reprio 重折叠）；eph fill/clear = 零 epoch（树未动，读路径
  逐读重算零缓存）。cursor 创建/seek/自身编辑后同步 c->epoch =
  t->epoch（自身编辑后仍可用）；**其它** cursor 读/编辑前校验不等
  → 抛 LSP_EPOCH_MSG；seek 后即复活。
- **id 失效语义（v4 新增）**：任何树编辑后旧段 id 即失效（合成
  id 可能被回收复用）——禁跨编辑缓存 styled 返回的 id；attr id
  跨编辑稳定（intern 语义），用户持 attr id 安全。
- arb 事务性（lua_pcall 包裹）沿 v1。

## 三、内部结构（C 侧定案）

### 3.1 数据结构

```
cp_（compositor，全局态——所有 Tree 共享；v4 大改）
├── 普通区：ref_attrs（registry 表）——attr id ↔ attr 表；by_attr
│     hash（canon/__hash 键）——intern 复用（同 attr 同 id）
├── ns 注册表（全局）：ns_byid（ref_nsb：name → ns id）+ prio/
│     regseq C 数组（动态 grow；id ≤ SP_MASK_BITS 普通 ns——mask
│     位 ns-1、id > SP_MASK_BITS eph ns）+ nsstack freelist +
│     nsnext/ephnext
├── op 空间（全局）：(ns, attr) → op id——byop 表 + opkind/opns/
│     opattr C 数组（grow）。op id ∈ 普通区（与 attr id 混排，
│     树段区分靠 mask）。op id 从普通区高位往下分配？——**实施定**
│     （attr intern 单调递增，op 若共用同一计数器则混排无害）
├── refcnt：unsigned *refcnt（全 id 域——普通区 + 合成区统一
│     数组；= 树段引用计数）
├── 合成区（id ≥ COMP_ID_START，k = id - COMP_ID_START 稠密）：
│     cp_Pairs { sp_Id a, b } *chain ——每合成 id 两槽：a = 链头
│     （合成 id 或 op/attr id 起点）、b = op id 叶子；零变长分配
│     unsigned *idfree ——合成 id 槽 freelist（链式）
└── 构成 hash：链结构 → 合成 id 的复用索引（展开沿途算 hash，
      同链同 id ref++）
```

```
lst_Tree userdata（v4：减 ns/op/mapof，加 sv）
├── cp：cp_ 态引用（全局共享——注册表级 ref 或静态态，实施定）
├── T：sp_Tree *（段存储 + arbiter + 编辑同步 + mask 剪枝）
├── sv：sv_List 数组（每 eph ns 一个；索引 = nsid -
│      SP_MASK_BITS - 1）
└── epoch / refs
```

```
lst_Cur userdata（v3 沿革，前缀 lst）
├── sp_Cursor C
├── lst_Tree *tree        -- 当前绑定树（epoch 校验快路径）
├── size_t epoch          -- 创建/seek/自身编辑时同步 t->epoch
├── size_t endoff / int nsid -- span/styled 迭代器专用（nsid = 0 合成
│       流 / 普通 ns / eph ns 三分支）
├── size_t mcur           -- 合成流迭代位置（仅 styled 迭代器用）
└── uservalue 表 {tree = 当前树 userdata} -- GC 锚定
```

- **refcnt 语义**：= 树段引用计数（新建不 +1）。**三态契约**
  （树保证 id 进出树必过 arb，design_spantree §9.7）：
  `arb(id, 0)` = 新生（写空段/分裂/继承）→ ref++；
  `arb(0, id)` = 死亡（合并/删除/trim 归零/freetree）→ ref--；
  `arb(id, old)` = 合并 → **出口无条件双向计数**：
  `ret != 0 → ++ref[ret]；old != 0 → --ref[old]`（先加后减，
  ret==old 净 0 自洽；时序结构问题已由 C 侧解决，无 ret!=old
  分支）。filterleaf 的保护 +1（碎片预支）与早退 cancel（抵消
  保护）保留（§9.7）。
  **合成 id ref 0 → 回收**：id 槽入 freelist + 链头节点（a）ref--
  级联释放（链节点同为合成区槽，递归）；叶子（op/attr id）ref--
  但普通区**暂不回收**（开放项——调用方句柄悬垂不可判定，见
  §七）。依赖 spantree.h 的 arb 全量调用点（§3.2 清单）。
- **cons 链不变量**：每合成 id 两槽（a = 链头、b = op 叶子；a 可为
  op/attr id 或合成 id）；链 = 压缩形态（展开去重排序后重建）→
  **链长 ≤ ns 数（64）**，解码 O(64) 恒界。展开即压缩：每次 arb
  解码必展开 → 产去重排序构成列表 → 新链按压缩形态重建——历史
  不累积（同 ns 反复覆盖被截断）。
- **构成复用**：展开后构成 → 链结构 hash → 同链已存在 → 复用该
  合成 id（ref++），零新节点。复用率高场景 = 同 attr 反复 fill
  （lsp 快照重推）。
- **单槽优化**：构成列表 n==1 且 ns>0 → 直接返回 op id（零合成
  分配）；n==1 且 ns==0 → 返回 attr id（平铺）；n==0 → 0。
- **id 分区与树段解码**：段 id < COMP_ID_START = 普通区——mask
  非 0 = 单 ns op 段、mask 0 = 平铺 attr 段；id ≥ COMP_ID_START
  = 合成段。t:attr 两区统一（合成 id 的 attr = 折叠表）。
- **优先级**：prio/regseq 在 comp（全局）；折叠序 = 展开后按
  (prio, regseq) 排序（ns 0 恒最前）。改优先级 = 改 prio[] + 各
  共享树全树 fill(REORDER)（arb 每段重折叠）；eph ns 零树操作。
- **ns 槽复用**：普通 ns 注销推 id 入 nsstack（全局）；eph ns 注
  销 = 数组释放槽清零不复用。mode 变更 = 注销 + 重注册。**注销
  与他树残留**：注册表全局移除后他树残留段的构成仍含该 ns——
  展开按 op 内嵌 ns 号处理（优先级缺失 = 视为 0/最底），下一轮
  arb 重折叠自然收敛（待实施验证）。

### 3.2 arbiter（三态契约 + cons 链流程）

**树的调用点契约**（C 侧保证，id 进出树必过 arb）：

| 路径 | 调用 | 说明 |
|---|---|---|
| 新生（写空段/分裂碎片/继承） | `arb(id, 0)` | 返回值（= id）写入新块；arb 负责 ref++ |
| 死亡（合并/删除/trim 归零/freetree） | `arb(0, id)` | 返回值（0）被丢弃（真清写写回 0）；arb 负责 ref-- |
| 更新（合并） | `arb(id, old)` | **出口无条件双向计数**：`ret != 0 → ++ref[ret]；old != 0 → --ref[old]`（先加后减，ret==old 净 0）；filterleaf 以保护 `arb(old, 0)`（k≥1 碎片预支）+ 早退 cancel `arb(0, old)` 支撑时序（§9.7） |
| pad（虚拟段） | `arb(0, 0)` | 空操作，返回值与 mask 弃用 |

**绑定层 arb 实现（伪代码定案）**：

```
arb(ud, in, old, mask):                    /* 三态合一 */
    ret = in && old ? pcall(merge)(in, old) : (in ? in : 0)
    if (ret != 0) refcnt[ret] += 1          /* 出口无条件双向计数
                                               （先加后减，ret==old
                                               净 0 自洽） */
    if (old != 0) refcnt[old] -= 1
    return ret
```

- merge 仅当 in != 0 && old != 0 时调：展开 old 链（≤64）→
  kind(in) 槽操作（WRITE 同 ns 覆盖后写胜 / CLEAR 删槽 / REORDER
  排序）→ 列表空 → 0；单槽 ns==0 → attr id（平铺）；单槽 ns>0
  → op id（零合成）；否则链 hash 查 → 命中复用 → 未命中建新链
  （每构成项一个合成区槽，cons 左斜）→ 新合成 id。

- 展开：从合成 id 沿 a 链走到普通区（op/attr id），收集 op 列表；
  b 槽恒为 op 叶子。链 = 压缩形态，展开 O(链长 ≤ 64)。
- 建链顺序 = 构成列表按优先级升序 cons（((o1, o2), o3)... 左斜）；
  链 hash = 沿途 (a, b) 对序列 FNV-1a——同构同 hash。
- 链节点 churn：未命中建 k 个新槽（k = 构成数 ≤ 64）——槽
  freelist 复用，8B/槽。
- ref 时序保证：filterleaf 在合并 arb 前先发保护 `arb(old, 0)`
  （k≥1 碎片预支）→ 出口 --old 有保护垫底；早退时 cancel `arb(0,
  old)` 抵消保护（出口 ++ret/--old 净 0 + 保护 +1 + cancel -1 =
  净 0）；k=0 时出口归零 = 整段死亡（arb 保留 old 入链则建链的
  结构 ref++ 已先行防住）。refcnt 在 op 中途可暂时超前树状态 1
  （保护 +1 至分裂落地或 cancel 才兑现），收敛于 op 结束。
- arb 内**零用户 Lua 代码执行**（不调 __hash/元方法等，仅 Lua C
  API 的 registry 表 rawget/rawset——op/链 hash 表操作）；refcnt
  维护零 Lua 调用路径（纯 C 数组 + 回收级联，在 pcall 之外）。
  合并路径 pcall 包裹（沿 v1 事务性：Lua C API 的 OOM 长跳转不
  穿透 sp_fill，失败 = 段保持旧值零 refcnt 扰动）。
- mask 出参 = 折叠结果精确 ns 集（arb 自解码自答，树零校验）；
  in-mask 参数 v4 后不再用于解码（cons 链 id 直取），仅兼容
  签名。
- sp_clear（注销路径）对匹配叶恰调一次 arb(CLEAR(ns), old, &m)。

### 3.3 命名与报错

- 方法名 namespace/fill/clear 与 sp_ 前缀 C API（sp_addns/sp_delns）
  零冲突（v1 占位名 newns/delns 弃用）。
- 报错文案（luaL_error）：
  - `"spantree: namespace limit reached"`（普通 ns 满槽）
  - `"spantree: unknown namespace"`（fill/clear/span/styled/next/prev/
    注销的未知名）
  - `"spantree: namespace already registered"`（严格注册撞名）
  - `"spantree: invalid namespace flags"`（flags 含非法字符）
  - 沿用 `"spantree: invalid offset"` / `LSP_EPOCH_MSG` / sp 错误码
    文案。

### 3.4 eph 层算法定案

**fill**（`sv_fill(L, off, len, id)`）：

```
s = 首个 off[i] + len[i] > off 的段   -- 二分
t = 首个 off[i] >= off + len 的段     -- 二分
left = off[s] < off（左段半覆盖）；right = 尾段端 > off+len（右悬垂）
left ∧ right ∧ t == s+1 → 单段一拆二（悬垂段尾部后移一位插入）
否则：左段裁剪（len[s] = off - off[s]）、右段悬垂裁剪
      （off[t-1] = off+len 截头）、中段 memmove 删除
新段 (off, len, id) memmove 插入（右悬垂时同插）
规范化 sv_norm：仅当 **相邻且同 id**（off[j-1]+len[j-1]
  == off[j]）才合并——清除制造的是洞不是邻接；每侧至多一步
```

- 不变量：数组恒有序、不交叠、**相邻（contiguous）异 id**；同 id
  覆盖写 = 幂等。O(n) memmove（视口级）。
- 实施修订（2026-08-16）：norm 无 contiguity 判定时跨洞误并
  （[0,6)X [12,20)X 被并成 [0,14)）；fill 无 right 悬垂时新段
  覆盖未移走元素（插入前须先整体右移）。

**clear**（`sv_clear(L, off, len)`）：重叠段 [s, t)；单段双
裁剪 = 一拆二；左裁剪/右悬垂裁剪（顺序：先右后左，右裁剪须用
原始长度）；中段 memmove 删除。**清除恒留洞**——clear 不可能
制造相邻同 id（无 norm 需求，2026-08-16 实施证实后删去）。

**clear-all**（`sv_resetall(t)`）：每层 n = 0（cap 保留）。

**合并 fold**（`lst_mergecalc(L, t, treeid, ts, te, x)`，v4）：

```
边界扫描（每层，与 prio 组无关）：
  upper(x) 越尾 → 尾段末端 cap s
  段起点 > x → 段起点 cap e；前段末端 cap s
  覆盖 x → 段起点 cap s、段末端 cap e
无 eph 覆盖 → 直返 treeid（零合成零 intern）
fold = attr 黑盒覆盖：空表 → 按层序合并 p>=0 eph attr
  （升序）→ t:attr(treeid) → p<0 eph attr（升序），后层覆盖
  前层 → intern（普通区幂等，无合成区参与、零 refcnt）
```

- 层序：p>=0 eph 叠树之下、p<0 eph 叠树之上（0 层 = 表层）；
  eph 间按 (prio, regseq)。styled 的树段 = 折叠黑盒，不查链、
  不查 mask——v3 的 lst_oldpairs/stylemask 依赖退场（styled 路径）。
- p 边界：p<0 归上组、p>=0 归下组（p 域 Lua number 全权，浮点
  负值合法）。
- 幂等性：同组合同 attr 覆盖表 → intern 同 id（跨帧稳定，
  testMergedIdCache 语义保持）。

**标记流拆解**（span / cursor next/prev/style，v4 新增）：

```
拆解一段（treeid 合成区 id）：cons 链展开 → 构成 op 列表（≤64，
  按优先级序）→ 每 op 输出 (attr id, off, len)——attr id = op 的
  attr 面（cp 反查）。树段 id 普通区：mask 非 0 = 单 op（一个
  标记）、mask 0 = 平铺（ns 0 标记，attr id = 段 id）
ns 过滤：构成里该 ns 的槽单出；无该 ns → 段跳过
```

- span 迭代器 = 段游标（sp_next 步进）+ 段内标记索引；同段多
  标记重叠多出（同 off/len，不同 attr id，按优先级序）。
- cursor 增 `midx`（段内标记索引）状态：style = 当前位置当前
  标记；next = 同段 midx+1 或下一段首标记（inclusive 语义沿
  v3）；prev 对称。普通 ns 过滤 = sp_next(ns) 剪枝 + 拆解取该
  ns 槽；eph ns = sv_ 步进（单标记天然，无拆解）。
- 拆解输出的 off = 标记区间（段内该标记覆盖范围 = 段全长——
  单段单槽制，标记区间 = 段区间）。

**编辑清空**：树级 splice/append/insert/remove 与游标级同名动词
先 sv_resetall 再动树；普通 ns 的 fill/clear/reprio 不清 eph
（树段未位移，eph 位置仍正确）。

### 3.5 sv：minispan（v4 拆分）

eph 列表逻辑独立为 sv_ 前缀 minispan——arrayvec 平铺段列表、
接口尽量兼容 sp_（无 B+ 结构、无 arbiter、**无 mask**）：

```
sv_List { size_t *off; size_t *len; unsigned *id; size_t n, cap; }
sv_fill(L, off, len, id)   -- 覆盖切分 + memmove + 规范形（§3.4
                              fill 定案：contiguous 判定、右悬垂
                              插入顺序）
sv_clear(L, off, len)      -- 区间裁剪（单段双裁剪一拆二、先右后
                              左、清除恒留洞）
sv_upper(L, x) / sv_lower(L, x)   -- 二分定位
sv_cover(L, x, &id, &ps, &pe)     -- 覆盖查询
```

- lst_Tree 持 sv_List 数组（每 eph ns 一个）；eph fill/clear/
  读 = sv_ 调用。v3 的 lsp_eph* 内嵌逻辑整体迁移（§一复用清单）。
- 文件组织：lua/spantree.c 分三块——cp_ 段（compositor）、sv_
  段（minispan）、lst 段（绑定统合）；拆多文件与否实施时定
  （先 section，拆分是纯机械搬迁）。
- 动机：eph 与树的实现差异（naive vs B+）不应混在 lst_Tree 内；
  sv_ 独立后 minispan 自成一库（后续系统可用），lst_Tree 统合
  （cp + spantree 树 + sv）——对应 Doc 统合全家桶的架构形态。

## 四、API 摩擦检查（沿 v1 §四，随 v4 修订）

对照 spantree.h 逐项核对（2026-08-16 修订）：

| 议题 | 结论 | 证据 |
|---|---|---|
| arb 签名 (id, old, mask) | 绑定随迁（ltree_arb 四参，返回 sp_Id） | §9.2 |
| arb(0,0) pad 语义回调 | 同 v1：in == 0 分支返回 0，返回值与 mask 弃用 | §3.2 |
| sp_next/sp_prev/sp_style 值返回 | 绑定迁移值接收 + ns 透传（v1 编译已断，必迁） | §9.2 |
| **id 生命周期（v4 新依赖）** | arb 三态契约（design_spantree §9.7）：树保证新生 arb(id,0)/死亡 arb(0,id)/合并 arb(id,old) 出口无条件双向计数 + filterleaf 保护/cancel | §3.2 |
| 按 ns 查区间 | span(ns)/next(ns)/prev(ns) = 拆解取该 ns 槽（attr id 输出）；eph ns 走 sv_ 二分 | §9.2/§3.4 |
| 按 ns 全树清 | namespace(name, nil) = sp_clear(T, nsn, CLEAR(ns))；eph = sv_ 清空；ns 注册表全局移除 | §9.4 |
| 编辑自动清 eph | 树 4 + 游标 4 编辑动词先 sv_resetall；普通 ns fill/clear 不清 | §3.4 |
| 游标编辑后悬垂 | epoch 守卫（沿 v1）；eph fill 零 epoch（树未动） | design_luabind #17 |
| op 载荷 ns 跨树 | **v4 消除**：ns/op 全局于 cp（v1 病灶的最终解） | §3.1 |
| 合成读的树段 mask | **v4 消除**：styled = attr 黑盒（不拆解）；span/next/prev = cons 链解码（id 直取链）——sp_stylemask 退场 | §3.4 |
| 标记按 id 删 | unmark(id) = 扫描 + 段构成含该 attr id → arb CLEAR(ns) 清槽（id2node 否决的扫描代价） | §二/§3.4 |

## 五、editor.lua 接入（沿 v1 §五，退场记录保留）

> v1 §5 的树写者接入方案已退场（research_spantree_usage.md 终局
> 结论）：editor.lua 现只用 comp:intern/comp:attr（hl/lsp/piece/
> visual 直叠合成）。**v4 迁移**：`sp.compositor()` 删除 →
> `local t = sp.new()`（内嵌 cp），`comp:intern/attr/setfields` →
> `t:intern/t:attr/t:setfields`——零语义变化；editor 的 comp 变量
> 改为 Tree 句柄（Ed.newcompositor → 建 Tree，样式接口透传）；
> **渲染读改 styled 四元组**（Ed:csi 走 t:styled(s, e)，table
> 直用免查）。
> 真消费者（身份层/extmark）到账后按 ns 即写者模型接入：
> t:namespace(name, prio) 注册层、local id = t:mark(name, attr,
> off, len)（返 id 供 unmark/复用）染色、t:clear(name) 清层、
> t:unmark(id) 按 id 扫描清。
> **hl 接入预案**（Q1 调研，reports/research_sp_highlight_ns.md
> §五）：lsp sem/diag 进树（普通 ns 快照 fill）、hl 进 eph
> （t:namespace("hl", 1, "e") + 每帧 t:clear("hl") + 逐 span
> t:mark("hl", attr, off, len) + t:styled(s, e) 样式流读）。

## 六、测试与验收

- `lua/tests/spantree_test.lua` **按 v4 全量重写**（luaunit，双
  运行时）：TestStyle（cp 面经 Tree：intern 复用/顺序无关/nil
  跳过/逆查两区/__hash 短路/__hash 非字符串报错/setfields——
  沿 TestCompositor 用例，入口改 t:）、TestNs（沿 v3 + **全局
  共享**：两 Tree 的 ns 注册互见/优先级互见/注销全局移除）、
  TestEph 沿 v3 语义改 styled 四元组断言（合成流幂等保持）、
  TestTree/TestMerge 沿 v2；**TestSpan 新增**（拆解输出 attr id/
  table 四元组、多标记重叠多出、优先级序、ns 过滤单槽、eph ns
  步进、cursor 标记索引状态（同段多标记 next/prev 逐个出）、
  style 当前标记）、**TestIds 新增**（分区判定：平铺/op/合成三
  类段 id 的 mask 解码、单槽 op 直存零合成、refcnt 随 arb 增减、
  合成 id 回收复用（段清空后同构成重 fill 拿回同 id）、链展开
  压缩不变量（行为断言：重复 100 次覆盖后解码正确且新分配槽数
  有界）、构成复用（同构成 ref++）、释放级联（段清空后链节点回
  收）、unmark 按 attr id 清槽返计数/未知 id 返 0/部分命中/
  跨 ns 同 attr 全删）、**TestArbRelease 依赖项**（remove/splice
  删段 ref--：C 侧补通知后启用）。
- justfile（lua/justfile 沿用）：`sp` / `sp-cov` / `sp-lines` /
  `sp-unbranched`。覆盖率铁律：spantree.c 行 100%、分支 ≥95%
  （豁免清单附 evidence）。
- `lua/spantree.d.lua` @meta 同步（单对象 API 全重写：Tree 合并
  Compositor 方法、span/styled/unmark、四元组返回、id 失效
  语义注释）。
- C 侧：spantree.h arb 三态契约与全量调用点（新生/死亡/合并出口
  双向 + filterleaf 保护/cancel）+ 对应 fanout4/8 用例；
  sp_stylemask 并入 sp_style（pmask 出参，NULL 跳过），C 测试改
  stylemask 用例。
- `just lua/test` 全绿 + `just luals` 零诊断；**editor_test.lua
  按 §五 迁移**（comp 来源与调用点全改 t:、渲染走 styled）。
- 文档同步：design_spantree.md §9.2 更新（sp_style 吸收
  sp_stylemask、arb 三态契约）；本文件 §四表已更新。

## 七、开放项（沿 v1）

- **普通区 id 回收**：refcnt 维护（树段引用），ref 0 暂不回收——
  调用方句柄（Lua 变量/eph 数组/渲染缓存）悬垂不可判定；机制
  齐（refcnt 全 id 域），回收触发策略（comp gc / 显式 API）后
  定。工作集可接受（op/attr 组合数 = 插件用色数）。
- ns 注销与他树残留段的收敛语义（§3.1 待实施验证）。
- ns 优先级域/类型：Lua number 全权（整数/浮点均可，比较即序）。
- 名字串校验：非空字符串（luaL_checklstring + 空串报错）。
- eph（sv_）数组 cap 保留不回收（清空 = n 置零；段数峰值视口
  级）。eph ns 注销槽不复用（数量不限）。
- 合成链的链结构 hash 细节（FNV-1a 常数/表容量/探测策略）实施定。
