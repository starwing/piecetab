# lc_insert 中间插入设计记录

## 1. 问题与设计目标

`lc_scan` 已实现 B+ 度量树之批量建树（Bulk Loading），然仅能于树尾追加行断点。文本编辑器之中间插入操作，需将新行断点正确嵌入树中任意位置，其后数据及度量须相应后移。

**设计目标**：

1. 复用 `lcB_Ctx` 批量加载管线（fill-flush 循环），以 `lcB_fill`/`lcB_flush` 为核心，代价最小化
2. 支持于树中任意游标位置插入行断点
3. 若 scanner 无新换行符，仅调整当前行长（`lcB_skipinsert`）
4. 若插入位置为树尾，自动退化为 `lc_scan` 路径（`lcB_init`）

## 2. `at_end` 字段的必要性分析

### 2.1 问题本质

`lcB_flush` 是 fill-flush 管线的核心函数，负责将 pend 层暂存的子节点合并入树。insert 与 scan 在 flush 阶段的行为差异仅在于 **pend 子节点插入父节点的位置**：

- **scan（末尾追加）**：插入到 `parent->child_count`（父节点末尾）
- **insert（中间插入）**：插入到游标路径所指位置之后（`paths[l] - parent->children + 1`）

### 2.2 为何需要 `at_end` 而非间接推断

若弃 `at_end`，需通过以下方式间接判断模式：

| 方案 | 问题 |
|------|------|
| 检查 `c->bytes == 0`（空树） | 非空树尾插入（trailing=true）亦需末尾追加，但树不空 |
| 检查游标是否指向树尾 | 需额外遍历或维护尾标记；`lcB_init` 已调 `lcK_locend` 将游标定位至树尾，与 `lcB_initat` 之游标位置在逻辑上等价，无法仅凭游标区分 |
| 保留原始游标副本比对 | 需额外存储，且 `lcB_init` 之游标为非空树时已移至尾，无法还原"原始位置" |

`at_end` 是一个简洁的布尔标记，在 `lcB_init` 中设为 1，`lcB_initat` 中通过 `memset` 零化默认为 0。仅一条 `if/else` 即可区分两种模式，无间接推断之忧。

### 2.3 使用位置

`at_end` 仅被 `lcB_flush`（`linecache.h:1109`）读取，控制 `at` 之计算：

```c
at = x->at_end ? (int)parent->child_count
               : (int)(x->c.paths[l] - parent->children) + 1;
```

- `at_end=1`：`at = child_count`，pend 数据追加入父节点末尾
- `at_end=0`：`at = paths[l] - children + 1`，pend 数据插入至游标路径所指子节点之右侧

### 2.4 中间插入为何是 `paths[l] + 1` 而非 `paths[l]`？

因为 `lc_insert` 之插入语义为 **在游标右侧插入**。例如游标在叶级 children[i] 处，新数据应位于 children[i] 与 children[i+1] 之间，故 `at = i + 1`。

### 2.5 结论

`at_end` 必要且合理。其代价为 `lcB_Ctx` 中一个 `int` 字段（4 字节），换得 flush 逻辑中清晰无误的模式区分，设计得当。

## 3. `lc_insert` 完整流程

```
lc_insert(C, e, scanner, ud)
│
├─ 1. 参数校验
│   C != NULL && C->tree != NULL && scanner != NULL
│
├─ 2. 保存旧状态
│   old_off = lc_offset(C)   ← 插入前字节偏移
│   old_bytes = c->bytes     ← 插入前树总字节数
│
├─ 3. 判定 trailing（树尾退化）
│   trailing = (old_off >= old_bytes || root.child_count == 0)
│
├─ 4. 扫描首个 break
│   if (!trailing && scanner(ud, c->bytes) == 0)
│     → lcB_skipinsert(C, e) → return LC_OK
│     （scanner 无新换行符，仅将 e 字节加入当前行长）
│
├─ 5. 初始化 lcB_Ctx
│   trailing ? lcB_init(&x, c)      → at_end=1, locend
│            : lcB_initat(&x, C)    → at_end=0, 浅拷贝游标
│
├─ 6. 应用首个 break（仅中间插入）
│   if (!trailing) lcB_applyfirst(&x, br, e)
│     │
│     ├─ 6a. lcB_splitleafat(&x)
│     │     游标所在叶一分为二：
│     │     - col>0 → 行中裂：左叶保留 col 字节，右叶自 col 后起
│     │     - col==0 → 行首裂：游标处起全移入右叶
│     │     右叶数据存入 x->rt_leaf/x->rt_bytes/x->rt_breaks
│     │     父节点度量扣除右叶数据
│     │
│     └─ 6b. 处理首个 break 值 br
│            - col>0 → 左叶当前行追加 br 字节，度量上修
│            - col==0 → 分配新叶存 br，放入 pend[lv] 作为首个子节点
│            第一个 break 的 br 已消耗，但 e 个字符落于右叶末行
│            → 将 e 追加到 x->rt_leaf 之最末行
│
├─ 7. fill-flush 循环（B+ 树 bulk loading）
│   for (lv = levels, r = lcB_fill(x, lv, scanner, ud); r > 0;
│        lv = levels, r = lcB_fill(x, lv, scanner, ud)) {
│       lcB_checkpendroot(&x)   ← 惰性分配 pend_root（首次 flush 前）
│       lcB_flush(&x, lv)       ← 自底向上将 pend 并入树
│   }
│
│   关键：lcB_flush 自 lv 层向下至 0：
│   - 若 at_end=0，插入位置 at = paths[l] - children + 1
│   - pend[l] 合并入父节点 at 处
│   - 若溢出，新节点 n 承载剩余数据，推入 pend[l-1]
│   - l==0 溢出则 root_push（树深+1）
│
├─ 8. 最终 flush
│   有 rt_leaf（中间插入且 rt_leaf 未处理）：
│     lcB_pushrt(&x, lv)        ← 将 rt_leaf 推入 pend[lv] 之末
│     lcB_packleafs(&x, lv)     ← 尝试将 pend 叶合并入当前叶
│       - total ≤ LEAF_FANOUT → 合并成功
│       - total > LEAF_FANOUT → 走 lcB_flush(&x, lv) 正常插入
│   无 rt_leaf（末尾追加或 rt_leaf 已在循环中处理）：
│     lcB_flush(&x, lv)
│
├─ 9. 清理
│   释放 pend_root（若已分配）
│   失败时释放 pend[] 中所有已分配子节点
│
└─ 10. 游标回位
     lc_seek(C, c, old_off + (c->bytes - old_bytes))
     trailing → C->col += e   （末尾模式下 col 随新文本前移）
     return LC_OK
```

### 3.1 末尾退化路径

当 `old_off >= old_bytes`（游标在文件尾）或 `root.child_count == 0`（空树）时，`trailing = true`，`lc_insert` 退化为与 `lc_scan` 相同的路径：

```
trailing → lcB_init(&x, c)   (at_end=1)
         → fill-flush 循环   (at = child_count, 追加到尾)
         → lcB_flush          (无 rt_leaf 分支)
         → lc_seek + C->col += e
```

此退化免除了裂叶之开销，且 `scanner` 首个返回值直接进入 `lcB_fill` 循环，无额外处理。

### 3.2 快速路径：`lcB_skipinsert`

当 scanner 首次调用即返回 0（插入文本中不含换行符）时，无需任何 B+ 树结构变更，仅需将 e 字节加到当前行长并上修度量：

```c
// 非末尾 + scanner 返回 0 → 无新换行
if (!trailing && !(br = scanner(ud, c->bytes)))
    return lcB_skipinsert(C, e);
```

`lcB_skipinsert` 所做：
- 若 `e > 0` 且 `C->lidx` 在有效范围内，当前叶段追加 e 字节
- `lcM_up` 向上逐层传播字节增量
- `C->col += e` 游标列偏移前移

## 4. 关键辅助函数

### 4.1 `lcB_splitleafat` — 游标处裂叶

输入：`lcB_Ctx *x`（含游标 x->c）
输出：`x->rt_leaf`（右半叶）、`x->rt_bytes`（右叶字节）、`x->rt_breaks`（右叶段数）；返回移出之段数 n

```
游标在某叶 Leaf[li] 中，位置为 lidx，列偏移为 col
叶原有 count 段数据 bytes[0..count)

   col > 0（行中）              col == 0（行首）
   ┌──────┬──────────┐        ┌──────┬──────────┐
   │ 保留 │ 移出     │        │ 保留 │ 移出     │
   │col字节│orig-col  │        │ (无) │全段      │
   │      │+后续段   │        │      │          │
   └──────┴──────────┘        └──────┴──────────┘
   左叶段数=lidx+1              左叶段数=lidx
   移出段数=n-1                 移出段数=n
```

操作后：
- 左叶（原叶）度量缩减 db 字节、dl 段
- 父节点 `bytes[li]` 和 `breaks[li]` 相应缩减
- 自 lv-1 层向上传播度量变化（`lcM_up`）
- 右叶指针、字节、段数存入 `x->rt_leaf/rt_bytes/rt_breaks`

**关键：** splitleafat 不将右叶插入任何 pend 或父节点，仅暂存于 x 中。右叶在后续 `lcB_applyfirst` 中接受首个 e 字节追加，并在最终 flush 时通过 `lcB_pushrt` 进入 pend 或 `lcB_packleafs` 尝试合并。

### 4.2 `lcB_applyfirst` — 应用首个 break

输入：`lcB_Ctx *x`、`unsigned br`（首个扫描返回值）、`int e`（插入字节数）

流程：
1. 调 `lcB_splitleafat(x)` 裂叶
2. 断言 n > 0（必有数据移出，否则游标在叶末=末尾退化路径不应至此）
3. 处理首个 break 值 `br`：
   - `col > 0`：左叶当前段追加 br 字节，度量上修（`lcM_up`）
   - `col == 0`：分配新叶存 br，作为 `pend[lv]` 首个子节点
4. 无论 col 为何值，`e` 字节追加至 `x->rt_leaf` 最末行：`rt_leaf->bytes[rt_breaks-1] += e`

**语义**：首个 break 代表"插入文本中第一行"之长度。若 col>0 则在当前行后接续；若 col==0 则开始新行。e 个字符（插入文本总长）落于末行（即插入文本之最后一行）。

### 4.3 `lcB_packleafs` — 尝试合并 pend 叶至当前叶

输入：`lcB_Ctx *x`、`int lv`（叶层层级）

逻辑：
- 计算 pend[lv] 中所有子叶（含刚被 pushrt 之 rt_leaf）的总段数
- 若 total ≤ LC_LEAF_FANOUT：将所有段数据拷入游标当前所在叶，释放 pend 中已合并叶
- 若 total > LC_LEAF_FANOUT：返回 0，由调用者走 `lcB_flush` 正常插入

**价值**：避免裂叶后产生的不完整叶碎片。当右裂叶段数 + pend 中新叶段数 + 当前叶段数 ≤ 叶容量时，可将所有新数据合并回当前叶，省去额外叶分配。

## 5. 与 `lc_scan` 之对比

| 维度 | `lc_scan` | `lc_insert` |
|------|-----------|-------------|
| 入口参数 | `lc_Cache *c, scanner, ud` | `lc_Cursor *C, int e, scanner, ud` |
| 初始化 | `lcB_init` (at_end=1, locend) | `lcB_initat` (at_end=0) 或尾部退化→`lcB_init` |
| 首个 break | 直接进入 fill 循环 | `lcB_applyfirst`（裂叶 + 处理 br + 追 e 至 rt_leaf） |
| 无 break 短路 | N/A | `lcB_skipinsert`（仅行长调整，无树变更） |
| flush 插入位置 | 父节点末尾 (at = child_count) | 游标之后 (at = paths[l] - children + 1) |
| 右裂叶处理 | N/A | pushrt + packleafs / flush |
| 完事后 | 无回位 | `lc_seek` 回位 + trailing 时 col 前移 |
| 代码行数 | 9 行 | 30 行 |

## 6. 边界情形容错

### 6.1 空树插入

`c->root.child_count == 0` → `trailing = true` → `lcB_init` → 退化为 scan 路径。首个 break 进入 `lcB_fill`，自根叶起正常构建。

### 6.2 树尾插入

`old_off >= old_bytes` → `trailing = true` → 同空树退化。免裂叶、免 applyfirst。

### 6.3 叶末行首插入（col == 0, lidx == breaks[li]）

`lcB_splitleafat` 中 `n = count - C->lidx`。若 `n == 0`，说明游标恰在叶最末段之后——此时无数据需裂出，返回 0。但此情况已由 `trailing` 判定捕捉（`old_off >= old_bytes` → trailing），故不会经此路径。代码以 `assert(n > 0)` 卫护。

### 6.4 scanner 立即返回 0

`lcB_skipinsert` 处理：当前行增加 e 字节，度量上修，无树结构变更。适用于插入纯文本（无换行符）于行内。

### 6.5 内存不足

- `lcB_splitleafat` 中 `lc_poolalloc` 失败 → 返回 -1
- `lcB_applyfirst` 中 `lc_poolalloc` 失败 → 返回 LC_ERRMEM
- `lcB_fill` 中分配叶失败 → 返回 LC_ERRMEM
- 所有失败路径均通过 `lcN_freechildren` 释放已分配的 pend 节点

## 7. 故障回滚保障

`lc_insert` 的失败处理分两层：

1. **pend 节点释放**：若 `r != LC_OK`，遍历 `pend[0..lv]` 逐层释放已分配子节点（`lcN_freechildren`）
2. **pend_root 释放**：无论成功与否，若已惰性分配则释放（`lc_poolfree`）

关键设计：flush 阶段若有数据已通过 `lcB_merge` 合并入树，该部分数据不可回滚。但因 bulk loading 之特性，flush 成功后的数据已完整属于树，度量一致，故不必回滚。

## 8. 测试覆盖

见 `tests/lc_test4.c` 中以下测试用例（共 14 个）：

| 测试 | 场景 |
|------|------|
| `test_insert_param_null` | 空指针参数校验 |
| `test_insert_empty` | 空树插入 |
| `test_insert_empty_noop` | 空树 + scanner 返回 0 |
| `test_insert_single_leaf` | 单叶中插入 |
| `test_insert_col_mid` | 行内插入（col>0） |
| `test_insert_noop` | 纯文本插入（skipinsert 路径，scanner 返回 0） |
| `test_insert_trailing` | 树尾插入（退化路径） |
| `test_insert_leaf_split` | 插入触发叶分裂 |
| `test_insert_cursor_pos` | 验证游标最终位置正确 |
| `test_insert_many` | 大批量插入（fill-flush 循环） |
| `test_insert_no_scanner` | 非空树插入但 scanner 首个即返回 0 |
| `test_insert_oom_trailing` | 树尾 OOM |
| `test_insert_oom_normal` | 中间插入 OOM（叶分配失败） |
| `test_insert_oom_col0` | 行首插入 OOM（col==0 路径） |

另有 `tests/lc_test8.c` 中 LC_FANOUT=8 环境之对应测试。

运行：`just dbg`（LC_FANOUT=4）或 `just dbg_lc8`（LC_FANOUT=8）
