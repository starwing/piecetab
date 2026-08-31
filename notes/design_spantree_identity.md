# spantree 身份层设计（identity ns）

> Status: 定案（2026-09-01，与 `notes/discuss_spantree_id.md` 讨论收敛）。
> 本文描述身份层 v1 的语义与实现边界；实现与测试排期见
> `notes/plans/plan_spantree_identity.md`。

## 0. 定位

`spantree.h` 存结果 id，不解释 id；`lua/spantree.c` 的 arbiter 负责
合成与生命周期。当前绑定层把 `mark()` 的返回 id 默认成 attr id，缺少
`design_spantree.md` §6.3 所规划的身份层，导致：

- 同样式只能共享同一 id，无法区分“同一样式的多个对象”；
- 无法按对象精确删除；
- 编辑触碰对象时无法把“对象被编辑”这个事实通知绑定层。

本文补上这一层。v1 的唯一消费者是 `editor.lua` 的搜索高亮；设计按
“identity ns = 自动维护位置的 span 列表”展开，不追求复刻 Neovim
extmark API。

## 1. 语义契约

声明为 identity 的 namespace（下称 identity ns）是一个逻辑对象列表：

```text
(mark_id, off, len, attr)
```

1. `mark(ns, attr, off, len)` 创建对象并返回全局唯一、稳定的
   `mark_id`；`mark_id` 不编码 ns，所属 ns 由 C 记录可查。
2. **身份可遍历**：`span(ns)` 输出完整逻辑 mark，不输出树内被其他层
   切碎的 run。同一 mark 被其他层切多段时只输出一次。
3. **身份可直操**：`unmark(mark_id)` 精确删除该对象；`mark_id`
   不存在或已失效时返回 0。
4. **pos/len 自动维护**：文本编辑平移、收缩或扩展这些 mark；对象
   身份跨编辑稳定，直到被删除。
5. **content-sensitive ns**：该 ns 内任一 mark 的字节区间长度被编辑
   改变，或它的任一树叶死亡，整个 mark 自动消失；纯平移不消失。
6. `clear(ns)` 清空整个列表。

v1 非目标：同 ns 多 mark 重叠、gravity、0 长度 mark、用户指定
mark_id、on_edit 回调、核心 id 索引。

## 2. Lua API

```lua
comp:namespace(name, p, flags)
-- flags 新字符：
--   "i" = identity ns（v1 禁止与 "e" 同用）
--   "s" = content-sensitive（identity 前提下有效）
--   原有 "c"（严格注册）、"e"（ephemeral）不变
-- 已注册 ns 改 identity 属性：先注销（会清层）再注册；不允许就地
--   切换 identity 属性。

t:mark(ns, attr_or_id, off, len) -> id
-- identity ns：分配并返回 mark_id；len 必须 > 0。
-- 普通 ns：行为不变，返回 attr id。

t:span(ns?, off, len) -> iter
-- identity ns：for off, len, attr, mark_id in ... 输出完整 mark；
--   范围参数 [off, off+len) 过滤“相交的 mark”，但输出完整区间。
-- 普通 ns：行为不变（树内 run 流）。

t:unmark(id) -> count
-- id 是存活 mark_id：精确删除该 mark，返回 1；不存在/已失效返回 0。
-- id 是普通 attr id：保留原“按样式删除”语义（死循环 bug 一并修复）。

t:clear(ns[, off, len])
-- 行为不变。identity ns 的区间 clear 仍按字节裁剪；被裁成多段的
--   对象保持同一 mark_id，unmark(mark_id) 删除全部分段。
```

v1 不提供 `t:invalidate(ns, off, del)`：search 直接注册
`"is"`，由绑定层在编辑前自动失效（§5）。显式失效原语列为 v2。

## 3. spantree.h 变更

### 3.1 arbiter 四态

内部事件助手按语义命名，绑定层只看到四个 arb 调用形态：

| 助手 | arb 调用 | 语义 |
|---|---|---|
| `spA_born` | `arb(id, 0)` | 新叶出现 |
| `spA_died` | `arb(0, id)` | 叶死亡/合并吸收 |
| `spA_merge` | `arb(in, old)`，`in != old` | fill/clear 的合成决策 |
| `spA_touch` | `arb(id, id)` | 叶字节长度被编辑改变 |

`spA_merge` 内短路：`in == old` 时直接返回 `old`，不调用 arbiter。
fill/clear 的 no-op 同 id 路径因此不会产生 touch 事件。绑定层收到的
`arb(id, id)` 只可能来自 `spA_touch`。

### 3.2 touch 契约（v1）

- 触发：一次公开编辑（append/insert/splice/remove）中，某叶的
  `(id, len)` 变为 `(id, len')`，`id != 0` 且 `len' != len`。
- 不触发：纯平移、内部因其他层造成的切分/合并（总长不变）、
  fill/clear（长度不变）、叶死亡（走 died）。
- 调用时机：字节长度更新之后、后续结构操作之前；树可能处于编辑
  中途。
- v1 契约：arbiter 必须原样返回 `id`，mask 不得改变；`spA_touch`
  不写回、不改结构。每次公开编辑最多 3 次 touch（删除边界最多 2
  次 trim + 插入最多 1 次 grow）。

### 3.3 touch 调用点

| 站点 | 场景 |
|---|---|
| `spI_grow` | 当前叶增长 |
| `spI_growleft` | 左邻叶增长（含深层最左叶分支） |
| `spD_trimright` | 左边界叶缩短 |
| `spD_trimleft` | 右边界叶缩短 |
| `spD_cutpiece` 部分删除分支 | 叶内删除缩短 |

### 3.4 v2 预留（不在 v1 实现）

`spA_touch` 可扩展为允许 arbiter 返回新 id 并写回；届时需要处理：

- 新 id 与左右邻叶同 id 时的叶容器内合并（可能触发新 touch）；
- 合并后叶容器 underfill 的 foldnode/rebalance；
- id 变换时的 mask 与引用计数同步。

## 4. 身份叶与生命周期（lua/spantree.c）

### 4.1 mark id 域

- 64 位 `sp_Id` 前提（编译期守卫）。
- `CP_MARK_BASE = 1 << 48`；
- `mark_id = CP_MARK_BASE | (generation << 28) | slot`；
- slot 28 位（最多约 2.68 亿个存活 mark）、generation 20 位；
- composite id 域必须恒小于 `CP_MARK_BASE`（`cpP_slotalloc` 守卫）。
- slot 可回收：freelist + generation 递增；旧 handle 因 generation
  失配失效，不会误删新 mark。

### 4.2 mark 记录

```c
typedef struct cp_Mark {
    cp_NS   ns;     /* 所属 namespace（0 不可能出现） */
    cp_Attr attr;   /* 视觉 attr id */
    unsigned gen;   /* 当前 generation */
    unsigned refcnt;/* 树内引用数（直接叶 + cons 链子叶） */
    unsigned idfree;/* freelist 链接：下一槽 + 1，0 = 尾 */
} cp_Mark;
```

`cp_State` 增加 dense 槽向量 `marks` 与 `markfree` 头。

### 4.3 生命周期骑在 arb 三态上

- `cp_refup / cp_refdown` 增加 mark 域分支；归零即回收槽位，
  generation 递增。
- `cpP_chain` 构造 cons 链时对 mark 子叶 refup；`cpP_release`
  释放链时经现有 `cp_refdown(a/b)` 自然回收。
- 直接叶：`arb(id,0)` birth refup，`arb(0,id)` died refdown。
- 合成叶：`arb(in,old)` 的 `+ret/-old` 与子叶 refs 对称。
- 结果：显式 `unmark`、被同 ns 新 mark 覆盖、`clear(ns)`、文本删除、
  树 GC 全部经 refcnt 归零回收，无泄漏，无特例。

### 4.4 解码

- `cp_attr / cp_segmask / cp_expand` 增加 mark 域分支：先验 slot 与
  generation，再返回记录中的 attr/ns。
- 新增 `cp_expleaf`：把任意树 id（attr / op / mark / composite）展开
  成 `cp_Leaf { id, ns, attr }` 平铺列表，保留**实际叶 id**。

## 5. leaf-preserving 合成

`cp_merge / cp_composite / namespace reprio 全树重折叠` 全部改走
`cp_expleaf`，不再用 `(ns, attr)` 序列 hash-cons 身份：

1. `cp_applyleaf` 按 op 种类作用在叶列表上：
   - `WRITE`（普通 op）：替换/追加对应 ns 槽，叶 id 为 op id；
   - mark 写：替换/追加对应 ns 槽，叶 id 为 mark id；
   - `CLEAR`：删除对应 ns 的全部叶；
   - `REORDER`：叶列表原样保留，只重排序重建。
2. 重建规则：
   - `n == 0`：返回 0；
   - `n == 1`：**直接返回该叶 id**（identity 保留的关键；不再
     canonicalize 成共享 op id）；
   - `n > 1`：按叶 id 列表构造 cons 链，链结构 hash key 使用叶 id
     序列（mark 叶唯一，因此不会与共享 op 链混淆）。
3. `cpP_chain` 接受 `cp_Leaf` 列表构造，并对 mark 子叶 refup。
4. `lst_arb_merge` 不变：`cp_refup(ret)` 后 `cp_refdown(old)`。

不变量：一个 mark 叶无论被多少其他层覆盖、经过多少次 reprio 全树
重折叠，其 id 都在 cons 链中存活，直到 ns 槽被清除或对象被删除。

## 6. content-sensitive 自动失效（绑定层）

- `cp_NSReg` 增加 flags 位：`CP_NSF_IDENTITY`、`CP_NSF_SENSITIVE`。
- `lst_Tree` 增加固定待删队列：容量 `SP_MASK_BITS * 3`（3 次 touch ×
  每个 touch 叶至多含每 ns 一个 mark，ns ≤ 64）。
- `lst_touch(ud, id)`：
  1. `cp_expleaf(id)`；
  2. 对每个叶，若其 ns 带 `SENSITIVE` 且叶 id 是 mark id，将 mark id
     去重压入队列；
  3. 零 Lua 调用、零分配、不触碰树。
- 所有编辑包装器（tree 与 cursor 两面的 splice/append/insert/remove）
  统一：
  1. 清队列；
  2. 调用核心编辑动词（touch 事件入队）；
  3. 对队列中每个 mark id 调 `lst_remove_mark` 精确删除；
  4. `epoch += 1`。
- 搜索接入方式：`comp:namespace("search", p, "is")`。编辑器无需再
  手动调用失效原语。

## 7. 精确删除与遍历

### 7.1 unmark(mark_id)

1. 校验 mark record：id 域、slot、generation；不合法返回 0。
2. 由 record 得到 ns；`sp_seek(0)` 后 `sp_next(C, ns)` mask 剪枝扫
   树，用 `cp_expleaf` 找含目标 mark id 的段，累计最小起点与最大
   终点。
3. 命中则在 `[min, max)` 上 `sp_fill(C, op_clear_ns, max-min)`。
   v1 同 ns mark 不重叠，该区间内该 ns 只有目标对象；其他层由
   arbiter 无损保留。
4. 返回 1/0。复杂度 O(含该 ns 的树段数)，v1 接受；核心 id 索引
   列为 v2。

### 7.2 span(ns) 完整 mark 遍历

- identity ns 遍历按“连续 `sp_next(ns)` 段中同一 mark id”分组，
  输出完整 `[start, end)`；
- 范围查询从 `off` 定位，并向前取一个匹配段以捕获起点在 `off` 之前
  但相交的 mark；输出不裁剪；
- 普通 ns 遍历路径不变。

## 8. v2 / 明确不做

- `spA_touch` 返回新 id 写回 + 叶内合并/underfill 处理；
- on_edit 回调（带新旧坐标）；
- id→span 缓存或核心 id 索引，把精确删除降到 O(log n)；
- 同 ns 多 mark 重叠、gravity、0 长度 mark、用户指定 id；
- `t:invalidate` 显式原语。

## 9. 验收要点

见 `notes/plans/plan_spantree_identity.md` 测试清单。
