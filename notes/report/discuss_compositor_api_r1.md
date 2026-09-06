# Compositor API 讨论快照 r1

> 状态：讨论已收敛，定案见 `notes/design_compositor.md`。本文保留
> 讨论过程；§6.5 是旧草稿，最终 API 与语义以 design 文档为准。
> 后续定案变更（nsclaim、无 nsmax、无 weak tree/nsmap、clearrange、
> raw cons 链无上限等）以 design 文档 §9 为准。

## 0. 首要共识：mark 模型（已定）

### 0.1 mark 与 attr 分开维护

- **mark**：compositor 维护，委托 ops 存储；应用层写/查/删；
  编辑漂移、identity、事件都在 mark 这一侧。
- **attr**：compositor 不解释、不维护实体，只保存不透明 attr id；
  绑定层保存实体并组装；styled 时把 attr id 返还消费者。
- **唯一数据关联**：mark 记录可携带 0..1 个 attr id，只是 mark
  数据里的一个字段。attr 不反向知道 mark；二者不共享生命周期，
  删除一方不影响另一方。
- **存储路径分离**：ops/树/事件流只出现 mark id；attr id 存储
  不出现 mark id。

### 0.2 mark id：统一 `cp_Id`，两种语义

|             | object id                    | token id                              |
| ----------- | ---------------------------- | ------------------------------------- |
| 创建 ns     | identity / sensitive         | 普通 ns                               |
| 是什么的 id | 一个逻辑 mark 对象           | 一个 `(ns, attr)` 写 token            |
| 唯一性      | 每次 `cp_mark` 新对象        | 同 ns 同 attr 共享                    |
| 区间        | 完整逻辑区间，内部碎片不可见 | 无对象区间；可被切开/合并/漂移        |
| attr        | 可选；0 = 不关联             | 必须非 0；attr 是 token 的存在目的    |
| 删除        | 精确删除该对象               | 删除该 token 在所属 ns 的全部现存染色 |
| 区间查询    | `cp_range` 可查              | `cp_range` 恒 ERRPARAM                |
| 对标        | Neovim extmark               | 高亮/染色层（tree-sitter 等）         |

`cp_Attr == 0` = NOATTR，也是“透明/无贡献”的唯一表达；不设独立的
透明 attr id。任何会覆盖下层的状态都是普通 attr id。

### 0.3 mark API（已定）

```c
int     cp_mark(cp_State *S, cp_NS ns, cp_Attr attr, cp_Range r, cp_Id *out);
int     cp_unmark(cp_State *S, cp_Id id);
int     cp_range(cp_State *S, cp_NS ns, cp_Id id, cp_Range *out);
cp_Attr cp_attr(cp_State *S, cp_NS ns, cp_Id id);
```

- `cp_mark`：
  - object ns：创建对象；attr 可选，0 = 不关联渲染；
  - token ns：创建染色区域；attr == 0 → ERRPARAM；
  - 恒返回 `cp_Id`，不设 `CP_NOID`。
- `cp_unmark`：
  - object ns：删除对象；1 = 删除成功，0 = 不存在/失效；
  - token ns：删除该 token 在所属 ns 的全部染色；返回清除段数，
    0 = 无现存段。
- `cp_range`：
  - object ns：查对象当前完整区间，写入 `out`；
  - token ns：恒 ERRPARAM；
  - id 不存在/失效：ERRPARAM。
- `cp_attr`：
  - 两类 ns 都返回关联 attr；
  - 0 = 无 attr / 参数错误；透明与无 attr 同义，不区分。
- 没有 `cp_clearbyattr`：token ns 的 `cp_unmark(token id)` 就是按
  token 清染色；attr 永远不是删除选择器。

## 1. 背景与项目约束

- `lua/spantree.c` 已 2500+ 行，内部事实上是三块：`cp_`（语义策略）、
  `sv_`（eph spanvec）、`lst_`（Lua 集成）。
- 目标：抽出与 `spantree.h` 平行的 `compositor.h`。绑定层平行组装
  `spantree.h + compositor.h + 本地 spanvec`，而不是
  Lua -> compositor -> spantree 的垫片。
- 层级：compositor.h = 实现者；绑定层（spantree.c）= 组装层；
  editor.lua = 应用/呈现层；屏幕前人 = 最终用户。
- 设计优先级：接口自洽 > 实现迁移便利；绑定层禁止访问 compositor
  私有接口；OOM 后库必须可用且状态一致。

## 2. 其余已定共识

1. `cp_init / cp_free / cp_reset`；State 即 Compositor；无池化，无
   `cp_new/cp_delete`。
2. 一个 State 维护多个 NS；NS 绑定 ops：`cp_nsnew(S, ops, flags)`；
   `cp_NS` 是整数；flags 类型叫 `cp_Flags`。
3. `cp_Store` 是不透明指针；调用时现场构造 `{ ops, S }` 传入回调；
   `#define cp_ops(st) (*(cp_StoreOps **)(void *)(st))`；用户后端把
   `cp_StoreOps` 放结构体首字段，直接 `(Backend *)cp_ops(st)`。
4. `cp_Attr` 是全局不透明 attr id；`cp_Id` 属于 ns。两个类型不混用。
5. 名字映射、Lua attr 表 intern/canon 归绑定层；compositor 不做
   string key -> id 映射。
6. State 自维护只增 extent；driver 不提供 bytes；绑定需要真实文本
   长度时 `cp_setextent` 注入。
7. ops 服务面固定写/清/编辑三族：`write / clear / clearall /
   append / insert / remove`。没有 bytes，没有 edit enum。
   `cp_splice` 在公开层由 remove + append 组合。
8. 事件函数直接调用，首参 `cp_Store*`：`cp_born / cp_died /
   cp_touch / cp_mergewrite / cp_mergeclear`。不用 setevents vtable。
9. 必须支持多个迭代器/游标并存；编辑后旧迭代器经 epoch 失效；
   编辑者自己的 cursor 保持“re-seek 后可用”的语义。
10. Lua 读接口保持 for-in 惰性迭代器；禁止用 Lua 表做收集。
11. compositor 公开 API 必须完整覆盖绑定层需要的全部能力；绑定层
    不得再写一套 lcp 访问私有接口。
12. `cp_spanvec(S)` 是内嵌的第二 ops（验证平台），其实现是否动态
    分配不影响接口表现。
13. OOM 后状态一致是铁律：每个 mutator 必须定义失败返回、失败后
    哪些逻辑已发生/未发生、不变量是否成立。spantree.h 的 reserve
    语义（OOM 时树不变）是参照模型。
14. priority 归 attr：`cp_setpriority(S, attr, prio)`；blend 按
    priority 升序输入，后项覆盖前项。不同 attr 同 priority 时按
    attr id 数值做确定性 tie-break（非语义）。

## 3. 语义已定 API

### 3.1 State / NS

```c
int   cp_init(cp_State *S, cp_Allocf *f, void *ud);
void  cp_free(cp_State *S);
void  cp_reset(cp_State *S);

cp_NS cp_nsnew(cp_State *S, cp_StoreOps *ops, cp_Flags flags);
void  cp_nsdel(cp_State *S, cp_NS ns);

size_t cp_extent(const cp_State *S);
int    cp_setextent(cp_State *S, size_t n);
```

flags：`0`（token/普通）、`CP_IDENTITY`、`CP_SENSITIVE`。eph 语义由
spanvec ops 自身表达（编辑即清空），不占 flag。

### 3.2 cp_Store / cp_StoreOps

```c
typedef struct cp_Store cp_Store; /* opaque; first field cp_StoreOps* */

#define cp_ops(st) (*(cp_StoreOps **)(void *)(st))

struct cp_StoreOps {
    int (*write)(cp_Store *st, cp_NS ns, cp_Id id, cp_Range r);
    int (*clear)(cp_Store *st, cp_NS ns, cp_Range r);
    int (*clearall)(cp_Store *st, cp_NS ns);

    int (*append)(cp_Store *st, size_t pos, size_t ins);
    int (*insert)(cp_Store *st, size_t pos, size_t ins);
    int (*remove)(cp_Store *st, size_t pos, size_t len);

    /* 读原语：值类型已定（§3.6），函数形状待 API 起草 */
};
```

ops 的 mark 生命周期协议（首参 `cp_Store*`；只传 mark id，不传
attr）：

```c
cp_Id cp_born(cp_Store *st, cp_Id id);
void  cp_died(cp_Store *st, cp_Id id);
void  cp_touch(cp_Store *st, cp_Id id);
cp_Id cp_mergewrite(cp_Store *st, cp_NS ns, cp_Id in, cp_Id old);
cp_Id cp_mergeclear(cp_Store *st, cp_NS ns, cp_Id old);
```

### 3.3 attr

- `cp_Attr == 0` = NOATTR = 透明/无贡献；不设独立透明 attr id；
- compositor 只分配/保存 attr id，不解释 attr 内容，不查实体；
- attr id -> 实体映射、intern/canon、实体生命周期都在绑定层；
- attr 的消费出口是 styled：blend 时把 attr id 返还消费者；
- attr 分配函数的具体名字/生命周期见 §4（未定）。

priority（已定）：

```c
int cp_setpriority(cp_State *S, cp_Attr attr, cp_Prio prio);
```

- priority 是 attr id 的排序属性；blend 按 priority 升序输入，
  后项覆盖前项；
- 同 attr 的贡献同级；需要不同级就用不同 attr id（绑定层决定
  token 分配粒度）；
- 不同 attr 同 priority：按 attr id 数值升序，只保证确定性，
  不构成语义保证；
- `cp_Prio` 类型与默认 priority 值未定（§4）。

### 3.4 mark

mark API 与语义已定，见 §0.3。此处只补实现约束：

- mark 记录由 compositor 维护，委托 ops 存储；ops/树里只有 mark id；
- token id 对应现有 `cp_op(WRITE, ns, attr)` 的 op token；
- object id 对应 identity/sensitive ns 的 mark 对象；
- `notes/design_spantree_identity.md` 的
  `mark(ns, attr_or_id, off, len) -> id` 由绑定层用
  `cp_mark / cp_unmark / cp_range / cp_attr` 组装。

### 3.5 编辑

```c
int cp_splice(cp_State *S, size_t pos, size_t del, size_t ins);
int cp_append(cp_State *S, size_t pos, size_t ins);
int cp_insert(cp_State *S, size_t pos, size_t ins);
int cp_remove(cp_State *S, size_t pos, size_t len);
```

`splice = remove + append`；OOM 全有/全无契约见 §4。

### 3.6 迭代器值类型（已定）

无析构值类型 cursor；后端 cursor 以编译期 union 注入。O1c 的
`unsigned char cur[CP_ITER_BUF]` 形态否决：char buffer 对齐为 1，
转成后端 Cursor 指针是未定义行为。

compositor.h 内：

```c
typedef struct cp_SVCursor { ... } cp_SVCursor;

typedef union cp_OpsCursor cp_OpsCursor; /* tag 归用户，include 前定义 */

typedef union cp_StoreCursor {
    cp_SVCursor  sv;   /* 内嵌 spanvec 槽 */
    cp_OpsCursor ops;  /* 用户后端 union */
} cp_StoreCursor;
```

用户侧（典型：`my_editor_comp.h` 配置头）：

```c
#include "spantree.h"
#include "xxtree.h"

union cp_OpsCursor {     /* 只定义 tag，不要 typedef（C89） */
    sp_Cursor span;
    xx_Cursor xx;
};

#include "compositor.h"
```

- 未先定义 `union cp_OpsCursor` 就直接 include compositor.h 时，
  `cp_OpsCursor ops;` 报 incomplete type；
- 误写 `struct cp_OpsCursor` 会因 tag kind 不匹配编译失败；
- ops 回调同 TU 内直接访问 `slot->sv` / `slot->ops.span`；
- 所有 TU 必须经同一配置头（`my_editor_comp.h`）间接包含
  compositor.h，保证 `cp_StoreCursor` 布局一致。

### 3.7 读接口语义（已定）

> 注：当前公开 read API 形态见 §6.5；mark 的窗口过滤语义仍在，
> 由调用方用 `cp_seek / cp_next` 表达，不再设 query 参数。

两类读对象分开：

- **mark 迭代器**：输出 mark；`{range, id, attr?}`。id 恒有效：
  普通 ns 是 token id，identity/sensitive 是 object id；attr 只是
  mark 记录的字段。查询语义：
  - overlap：与窗口相交，输出完整 mark；
  - inside：完全在窗口内；
  - all：整层。
- **styled 迭代器**：只收集携带 attr 的 mark，把 attr id 返还
  消费者做 blend；attr 0（无关联）不进入 styled。输出形态未定
  （§4）。

## 4. 形态或语义未定项

> 注：O2–O8 已有续议处置，见 §6；冲突处以 §6 为准。

### O1 styled blend 模型

前提：styled 只处理携带 attr（attr != 0）的 mark；无 attr mark
不进入 styled。该过滤不改变 mark/attr CRUD 独立。

候选：

- **A**：2D 展平成 1D，同 range 多 attr 重复输出，消费者自己分组；
- **B**：真 2D 迭代器（外层 range 组，内层 attr 贡献）；
- **C**：`cp_Mergef` 回调，compositor 每组现场合成一个 attr；
- **D**：B 为 compositor 契约，mergef 只是绑定/编辑器自己包的便利。

当前倾向：B/D（compositor 只收集和排序贡献，blend 归消费者）；
但“合并算 compositor 职责还是消费者职责”尚未拍板，C 未排除。

### O2 公开 Cursor API 具体函数

绑定层不能访问私有接口，因此 Lua 的
`seek/locate/advance/offset/style/next/prev/mark/clear/splice/append/
insert/remove` 必须由 compositor 公开层等价提供，包括 epoch 失效与
“编辑者自身 re-seek 后仍可用”。cursor 值类型已定（§3.6），具体
函数形状待 API 起草。

### O3 spanvec ops 上的 identity/sensitive

接口要求 `cp_mark` 对所有 ops 一视同仁。spanvec adapter 若支持
object mark，必须补 born/died/touch 事件；若不支持，需显式定义
ops 能力并返回确定错误。二选一未定。

### O4 OOM 后状态契约

- spantree.h 现状：reserve 在结构变更前做，OOM 返回错误且树不变；
  这是 compositor 的参照。
- `cp_splice = remove + append` 的 append OOM 时是否要求 all-or-nothing
  未定；若要求，需要在公开层预检或改为 ops 级 splice。
- 每个 mutator 都要写下：失败返回、已发生/未发生的逻辑变更、
  不变量状态。

### O5 styled 输出顺序细节

位置升序已定；priority 排序已定（attr priority 升序，后项覆盖前
项，tie-break 见 §3.3）。同组内同 attr 是否去重，待 O1 形态确定
后一起定。

### O6 命名与小项

- mark API 四个名字已定：`cp_mark / cp_unmark / cp_range / cp_attr`；
- attr id 的分配函数名与生命周期未定（`cp_attralloc` 为候选）；
- `cp_Prio` 类型与默认 priority 值未定；
- `cp_overlap / cp_inside / cp_all` 语义已定，名字可商酌；
- `cp_Flags` 的 flag 常量名未定；
- 迭代器公开类型名待 API 起草（值类型已由 §3.6 定案）。

### O7 迭代器 slot 拓扑

值类型已定；以下实施细节未定：

- `cp_StyledCursor` 内 slot 拓扑：单个 `cp_StoreCursor` 还是每 ns
  一路；
- eph ns 超过 64 时 spanvec 是否走 stateless；
- 各后端在 `union cp_OpsCursor` 中的成员名契约。

### O8 Lua 绑定迁移

- 现状：普通 ns 的 Lua `mark` 返回 attr id；`unmark(attr_id)` 在
  `lua/spantree.c` 中全树扫描 `cp_expand` 比较 attr 后逐段
  `sp_fill(CLEAR, ns)`，且跨 ns 删除同 attr。
- 新模型：普通 ns 返回 token id；`cp_unmark(token id)` 按 token 删
  该 ns 全部染色；compositor 不提供跨 ns 按 attr 删除。
- 旧 Lua 绑定还要把 ns-priority 迁移为 attr-priority（绑定层给各
  ns 使用的 attr 分配对应 priority，或用不同 token 模拟）。
- editor.lua 无 `unmark` 消费者；该分支主要是测试维持的旧 API。
  绑定层是否保留旧 `unmark(attr_id)` 兼容（用公开 mark 迭代器 +
  `cp_unmark` 组合）未定。

## 5. 关键证据

- 双后端现状：普通 ns 共享 sp_Tree（arbiter/op/composite），eph
  per-NS sv 列表（`lua/spantree.c` cp_/sv_/lst_ 三段）。
- token id 现状：`cp_op(WRITE, ns, attr)` 按 `(ns, attr)` intern 出
  op id；object id 现状：`cp_markalloc` 分配 mark id。
- Neovim extmark 的 priority 绑定在每个 mark 的 decoration 实例上，
  不是 hl_group / ns：`src/nvim/api/extmark.c:720-727` 解析，
  `src/nvim/decoration_defs.h` 存储。在我们的映射里 attr = Decor
  对象，token 粒度由绑定层决定，所以 attr-priority 不损失其表达
  能力。
- 迭代器值类型已用当前 `spantree.h` + C89 探针验证：tag-only
  `union cp_OpsCursor` 注入后 `cp_StoreCursor` 自动取 `sp_Cursor` 的
  大小/对齐（默认 152 字节）；漏定义时在 `cp_OpsCursor ops;` 处报
  incomplete type。
- mark 接口兼容：`notes/design_spantree_identity.md` §2。
- 多迭代器/编辑失效：`lua/tests/spantree_test.lua`
  `testEpochGuard`、`testSpanEpochGuard`、`testSelfEditSync`。
- 渲染消费者：`editor.lua` `_render_line` 的
  `for ... in self.tree:styled(lo, text_end + 1)`。
- per-mark attr 消费者：`editor.lua` 的 vtext/sem/diag/piece/visual
  mark 调用。
- 旧 `unmark(attr_id)` 证据：`lua/spantree.c` `Ltree_unmark` 的普通
  id 分支 + `testUnmark` / `testUnmarkAttrDeadLoop`；editor.lua 无
  unmark 调用。
- 禁止表收集与禁止绑定访问私有接口：本讨论新增的项目约束。

## 6. 续议记录（第二轮；与上文冲突处以本节为准）

> 状态：仍为讨论快照，未定案。本节是最近一轮讨论的上下文快照，
> 覆盖/改写部分 §3 与 §4。

### 6.1 O2–O8 处置

- **O2（关闭）**：绑定层同时绑 spantree.h + compositor.h；attr 的
  intern/retrieve 统一由绑定层提供。公开 Cursor API 只按当前功能
  需要提供；不需要的 seek/locate/... 不绑。不兼容当前 Lua 绑定接口。
  §2.11 与 O2 不矛盾，原文保留。
- **O3（关闭，最终修正）**：spanvec 完整支持 token/object 语义；
  `CP_IDENTITY | CP_SENSITIVE` 合法；`cp_commit` 清空该 ns 全部
  内容，sensitive 由清空统一覆盖。
- **O4（移出接口讨论）**：多 ops 部分失败是实现问题，不是接口问题；
  接口阶段不定义跨 store 事务。将来实现需要时再决定是否加 ops 级
  splice。
- **O5（关闭）**：styled 同组同 attr 去重。
- **O6（部分关闭）**：`cp_Prio` 不存在，`cp_setpriority(S, attr,
  int)` 直接用 `int`。overlap/inside/all 见 §6.3，read API 重开。
- **O7（关闭）**：slot 拓扑为实现细节，不进 API。
- **O8（关闭）**：不考虑旧 Lua API 兼容。
- **签名铁律（新增）**：任何签名单行 ≤80，禁止折行；需要折行的
  签名视为 API 形态有问题。

### 6.2 R1 架构结论

- compositor **不做编辑分发**；StoreOps 暂不含编辑回调。备注：将来
  需要时直接加回独立编辑槽位即可，不影响其余接口。
- 文档编辑由绑定层编排：调用各后端原生编辑 API（spantree 为
  `sp_splice / sp_append / sp_insert / sp_remove`），完成后调
  `cp_commit`。
- `cp_commit(S)` 是 compositor 唯一编辑边界：提交 sensitive 失效、
  `++epoch`；公开 `cp_epoch(S)`。
- compositor 不维护 extent：删除 `cp_extent / cp_setextent`。字节
  长度归 caller / backend。
- spanvec 样板间不接收 push 编辑事件：每次入口比较 `cp_epoch`，变化
  时对旧段报 `cp_died` 并清空（pull 失效）。
- StoreOps 当前面：`write / clear / clearid / clearall / seek / next /
  prev`。`clearid` 对应 `sp_clear(T, ns, id)`，是 identity
  `cp_unmark` 的委托点；`clearall` 独立，不用 range 哨兵模拟全清。
- flags 保留 ns 级：`cp_nsnew(S, store, flags)`；
  `CP_IDENTITY`、`CP_SENSITIVE`（修饰位，必须带 identity）。
  identity/sensitive 是 ns 类别，`cp_mark` 分配 id 前必须知道；
  下沉 ops 只换存储位置并破坏同质性，不成立。
- `cp_Attr`：compositor 只存储/透传，不分配、不解释、不管生命周期；
  `0 = NOATTR`。
- 当前区间类型：`cp_Range { size_t start, end; }` 半开，对齐
  pt_Range。

### 6.3 read API 结论（重叠前提下）

- **mark 允许重叠**；这是 compositor API 语义。spantree.c 的
  同 ns 不重叠是后端实现限制，不是 compositor 约束。
- **token/object 共用同一套 API 签名**；两者的语义差别维持 r1
  §0.3 原文，不在本轮改动：
  - `cp_unmark`：object 返回 1/0；token 返回清除段数。
  - `cp_range`：object 返回完整逻辑区间；token 返回 `CP_ERRPARAM`。
  - `cp_attr`：两类都返回关联 attr，0 = 无 attr / 参数错误。
- `cp_Id` 全局唯一；`cp_unmark / cp_range / cp_attr` 只收 id。
- mark 迭代器 = start 序扫描，不按 end 推进：
  `cp_seek(MC,S,ns,pos)` 定位到 `(start asc, id asc)` 序下第一个
  `end > pos` 的项；`cp_next` 按该序返回并推进。
  - 覆盖 pos：seek(pos) 后循环，`start <= pos` 的项即覆盖项；
  - 窗口相交 [s,e)：seek(s) 后循环到 `start >= e`，过滤 `end > s`；
  - 全部：seek(0) 循环到返回 0。
  因此 **overlap/inside/all 不进 API**。
- token ns 迭代输出 run；object ns 迭代输出完整对象（r1 §3.7）。
- styled = O1 B：`cp_seekstyled / cp_nextstyled` 输出扁平 item
  `{range, attr, last}`；组 = 贡献集合恒定的最大区间；组内
  `(priority asc, attr asc)`、同 attr 去重、`last` 标记组尾；
  mergef 是绑定层便利，不进 compositor。
- `cp_Styled` 是**二维展平为一维**：
  - `range` = 当前组区间。组本身按“贡献集合恒定”切段，所以同组
    item 的 `range` 必然相同；若按原始 mark 区间输出，同组 item 的
    `range` 就会不同（那是 O1 A，已排除）。compositor **不按调用方
    窗口 [lo,hi) 预裁剪**，窗口裁剪由消费者做；
  - `attr` = 组内一个贡献（非 0）；
  - `last == 1` = 本组最后一个贡献。消费者按收到顺序收集，收到
    `last` 后做 blend；顺序即覆盖顺序，后者覆盖前者；
  - 无贡献区间不输出。
- 例：A=[0,20) attr1 prio0；B=[5,10) attr2 prio1。输出流：
  `[0,5) attr1 last=1`；
  `[5,10) attr1 last=0`、`[5,10) attr2 last=1`；
  `[10,20) attr1 last=1`。
- 消费者（画 [lo,hi)）：

  ```c
  cp_StyledCursor sc;
  cp_Styled   it;
  int             r;

  cp_seekstyled(&sc, S, lo);
  while ((r = cp_nextstyled(&sc, &it)) == 1) {
      if (it.range.start >= hi) break;
      add_contribution(&blend, it.attr);       /* 消费者自己的收集器 */
      if (it.last) {
          draw(blend_attr(&blend),
               clip(it.range, lo, hi));        /* 消费者裁剪窗口 */
          reset_blend(&blend);
      }
  }
  if (r < 0) return r;                         /* CP_ESTALE 等 */
  ```

### 6.4 O1–O8 当前结论

| 项  | 当前结论                                                                                                                    |
| --- | --------------------------------------------------------------------------------------------------------------------------- |
| O1  | B：2D 组式，扁平 item + last；C 排除；mergef 归绑定层                                                                       |
| O2  | 绑定层同时绑 spantree.h + compositor.h；compositor 公开读面只有 mark/styled 两套 seek/next；cursor 功能按需绑，不兼容旧 Lua |
| O3  | spanvec 支持 object 语义；commit 清空全部内容；flags 合法                                                                    |
| O4  | 非接口问题；跨 store 事务与 ops 级 splice 留实现阶段                                                                        |
| O5  | 同组同 attr 去重                                                                                                            |
| O6  | priority=int（默认 0）；overlap/inside/all 移除；flags=CP_IDENTITY/CP_SENSITIVE；迭代类型 cp_MarkCursor/cp_StyledCursor     |
| O7  | slot 拓扑为实现；公开的只有 CP_STORE_MAX 与 cp_StoreCursor 值类型                                                           |
| O8  | 不兼容旧 Lua API；旧 unmark(attr) 不保留                                                                                    |

- 签名铁律：签名单行 ≤80，禁止折行；折行说明 API 形态有问题。
- `cp_setpriority` 未设置过的 attr 默认 priority 0。

### 6.5 API 快照已定案

最终 API 与语义见 `notes/design_compositor.md` §3/§4。相对本节旧草稿
的结论变化：

- `cp_Store` 公开面保持 opaque；实现段定义
  `struct cp_Store { cp_StoreOps *ops; cp_State *S; };`。后端首字段
  内嵌 `cp_StoreOps`，注册传 `cp_StoreOps *`，
  `(Backend *)cp_ops(st)` 恢复后端。
- 存储级 raw id 类型是 `cp_Leaf`，放在 StoreOps provider 段；
  `cp_Seg.id` 和事件参数用 `cp_Leaf`，应用层公开 `cp_Id` 与之分离。
- StoreOps 面为 `write / clearleaf / clearall / seek / next / prev`；
  事件面新增 `cp_mergecut`。
- `cp_spanvec` 返回 `cp_StoreOps *`，且 spanvec 完整支持 object
  语义；`CP_IDENTITY | CP_SENSITIVE` 注册合法，commit 清空全部内容。
- `cp_State` 一文档一个；Lua 绑定层任意创建。
- 无区间 clear；`cp_nsclear` 是唯一清层入口。
- `cp_MarkCursor / cp_StyledCursor` 增加实现所需 inline 状态；
  styled 组归并用实现段 `cpV_` 动态数组，贡献数无编译期上限。
