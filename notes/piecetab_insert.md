# piecetab insert 算法

`piecetab.h:1046-1063` (`pt_insert`) 是入口，`piecetab.h:764-1063` 为全部 insert 代码。

## 核心设计：pend[] 延迟插入 (lazy cascading)

insert 不逐 piece 从根向下遍历插入，而是从叶层自底向上建树：

1. **叶层收集 piece**：`ptI_fill` 将输入文本逐片段填入 `pend[levels]`（叶层 pend），每片段 ≤ PT_PIECE_MAXSIZE (65535)
2. **叶层溢出则上推**：`ptI_flush` 将 pend[l] 推入树中；若 parent[l] 满，将溢出部分打包为新 Node，推入 `pend[l-1]`（上一层 pend）
3. **递归上推到根**：若所有上层皆满，最终 `ptI_splitroot` 分裂根为新三层结构

结果：一次 `pt_insert` 自底向上逐层建树，避免了逐 piece 的 O(log n) 根遍历。

此算法与 B+ 树 bulk loading 之"右路径分裂"本质相通——新数据始终追加于树尾或特定插入点，pend 即为右路径上各级待消耗节点的缓冲区。

## 数据结构

```
pt_Insert {
    pt_Cursor *c;                        -- 插入位置的光标
    const char *s;                        -- 待插入文本
    size_t len;                           -- 剩余文本长度（递减消耗）

    pt_Node *parents[PT_MAX_LEVEL];       -- 沿路径每层的父节点（初始化时快照）
    pt_Node  pend[PT_MAX_LEVEL];          -- 每层的待插入节点（核心缓冲区）
    pt_Node *pend_root;                   -- 惰性预分配的空 Node，仅 splitroot 用
}
```

### pend[] 详解

`pend[l]` 乃核心缓冲区，每一层 l 各自拥之。其作用机制：

- **叶层 (l=levels)**：`pend[levels]` 收集 Piece（由 `ptI_fill` 填入）。Piece 实为 `pt_Piece*`，但存于 `children[]` 数组中（因叶层 `children[]` 存储 Piece）。
- **内层 (l < levels)**：`pend[l]` 收集新创建的 Node（由 `ptI_flush` 在 `ptI_splitinsert` 后上推）。这些 Node 本身已是子树根（其 children 为下层溢出打包而成）。
- **容量**：pend[l] 最多能容纳 PT_MAX_FANOUT 个子节点（同普通 Node），但 pend 不要求填满——它只在 `ptI_flush` 时将内容"倒入"父节点。
- **生命期**：pend[l].child_count 在 flush 成功后归零（`pend[l].child_count = 0`），内容已移入父节点或打包为新 Node 上推。

### 为何 pend 而非直接填充 parent？

| 直接填充 parent | pend 缓冲后 flush |
|----------------|-------------------|
| 需先 split 腾空间，再填——两遍遍历 | 先填 pend，flush 一次完成拆分+插入 |
| 分裂时数据已半写入 parent，回滚复杂 | pend 未写入树，OOM 可安全清理 |
| 耦合 fill 与 split 逻辑 | fill 只管创建 Piece，flush 只管并入树 |

### parents[l] 之角色

`parents[l]` 在 `ptI_init` 时快照——记录插入路径上每层的原始父节点。其后树虽变（level 增、节点裂），parents 不变。其用途：

1. **度量更新**：`ptI_adddelta`/`ptI_subdelta` 沿 parents 链逐层更新 bytes/breaks/chars
2. **cursor 同步**：flush 后 cursor 的 paths 需更新以指向正确的父节点 children 槽
3. **COW 溯源**：`ptI_init` 中依 parents 判断节点版本，决定是否 clone

## 关键常量

| 符号 | 值 | 说明 |
|------|-----|------|
| PT_MAX_FANOUT | 62 | 节点最大子数（内节点和根节点共用） |
| PT_PIECE_MAXSIZE | 65535 | 单片最大字节数 |
| PT_PIECE_MAXLINES | 58 | 单片最大行数 |
| PT_MAX_LEVEL | 16 | 树最大深度 |

## 函数详解

### 1. `pt_insert(c, s, len)` — 入口 (line 1046)

```
1. len==0 → 直接返回
2. ptI_init(&x, c, s, len) — 初始化上下文（COW、splitpiece、fillroot）
3. while (x.len > 0):                        -- 还有输入文本未消耗
     a. ptI_checkpendroot(&x)                -- 惰性确保 pend_root 已分配
     b. ptI_fill(&x, &x.pend[l])             -- 填叶层 pend 至满或文本耗尽
     c. ptI_flush(&x, l, movegap=1)          -- 将 pend 自 l 层起向上逐层推入树
     d. l = ptC_levels(c)                    -- 重新获取 levels（splitroot 可能增长）
4. 错误处理：ptI_disposepend 清理所有层的 pend 中已分配节点
5. 释放 pend_root（若已分配）
```

### 2. `ptI_init(x, c, s, len)` — 初始化 (line 857)

```
1. 若 cursor 未脏 (dirty==0): 创建新快照（COW），version++
2. 若树为空 (bytes==0):
     ptI_fillroot(x) — 直接在 root 上填 piece，光标移至树尾
     注意：fillroot 后 pend 为空，后续 while 循环在 pend[levels] 工作
3. 从根到叶遍历所有层 (l=0..levels):
     a. 记录 parents[l] = 当前层父节点
     b. pend[l].child_count = 0（清空 pend）
     c. COW: 若 cs[i]->version != snap->version，clone 该节点并重定向 paths[l+1]
        — clone 确保编辑不影响共享的旧快照
4. 若插入位置在 piece 中间（poff != 0 && poff != size）：
     ptI_splitpiece(x) — 将当前 piece 切为左、右两半，右半入 pend[levels]
```

**COW 关键**：`ptI_init` 中仅 clone 当前路径上的节点（每层最多一个），非全树复制。子树中非路径上的节点保持共享。

### 3. `ptI_fill(ctx, n)` — 填节点 (line 790)

将输入文本逐段转为 Piece 填入节点 `n` 的空槽（从 child_count 位置起）。

```
限制: 填至 (a) 节点满 (child_count == PT_MAX_FANOUT) 或 (b) 输入文本耗尽

——零拷贝机制——
每片 Piece 之 data 指针直接指向输入缓冲区 ctx->s（不作 memcpy）：
  p->data = sl.s   (sl = {ctx->s, min(ctx->len, PT_PIECE_MAXSIZE)})

——回调扫描换行——
S->linesf(S->lines_ud, sl, &li) 负责:
  - 扫描 sl 中的换行符，将 "换行位置+1" 写入 p->ends[]
  - 返回实际消耗的字节数 (需 ≤ sl.len)
  - li.chars 为字符数，li.breaks 为行数

——度量记录——
n->bytes[i] = 实际消耗字节数
n->breaks[i] = li.breaks
n->chars[i] = li.chars
```

**为何 linesf 可返回 < sl.len**：输入文本可能无换行——若扫描到 sl.len 仍无换行，回调在 PT_PIECE_MAXSIZE 处自发截断。这保证每片 ≤ 65535 字节且 ≤ 58 行。

### 4. `ptI_flush(x, l, movegap)` — 级联刷新 (line 1018)

将 pend[l] 中的节点推入树，从指定层 l 向上逐层处理。此乃 pend 算法的核心调度器。

```
for (; l >= 0; --l):
    1. 尝试 ptI_simple(x, l, movegap)：
       — 若 parent[l] 有足够空槽容纳 pend[l]（child_count + pend <= PT_MAX_FANOUT）
       — 直接插入 pend 到 parent，更新度量，光标前移，返回
       — 此为最常见路径（90%+ 情况）

    2. simple 失败（parent[l] 已满）→ 走分裂路径：
       a. n = 分配新 Node（将成为 parent[l] 的兄弟节点）
       b. ptI_splitinsert(x, l, n) — 分裂：将 pend[l] + parent[l]现有右侧子节点
          重新分配，parent[l] 填满至 PT_MAX_FANOUT，剩余放入 n
       c. 若 l == 0（处理到根层）：
          ptI_splitroot(x, n) — 根分裂为 [旧根, n]，levels+1，返回
       d. 否则（内层）：
          将 n 推入 pend[l-1].children[p++]，继续循环处理上一层
```

**movegap 参数**：insert 时 movegap=1，光标前进到插入后的位置（跳过刚插入的内容）。remove 时 movegap=0，光标不动。

### 5. `ptI_simple(x, l, movegap)` — 简单插入 (line 976)

快速路径：parent 有足够空间时使用，避免不必要的节点分配和分裂。

```
1. 若 pend[l].child_count + parent->child_count > PT_MAX_FANOUT → 返回 0（走分裂）
2. m = ptI_sumrange(pend[l], 0, pend[l].child_count) — 求和 pend 中所有子节点度量
3. ptI_makespace(parent, i+1, pend.child_count) — 在插入位置腾出空间（memmove 右移）
4. ptI_move(parent, i+1, pend, 0, pend.child_count) — 移入 parent
5. ptI_adddelta(x, l, m) — 沿 parents 链更新所有祖先层度量
6. 若 l == levels: ptM_addmetrics(&snap->metrics, m) — 更新树总度量
7. 若 movegap: cursor->paths[l] 前移 pend.child_count 个槽
8. pend[l].child_count = 0
```

**度量更新链路**：parent[l-1] → ... → parent[0] → snap->metrics（整树）

### 6. `ptI_splitinsert(x, l, next)` — 分裂插入 (line 991)

当 parent[l] 已满时，将 pend[l] 中的内容分流：优先填入 parent[l] 空余，剩下归入 next。

```
前提: parent->child_count + pend.child_count > PT_MAX_FANOUT
      (否则 simple 已处理)

1. ptI_takegapright(x, l, &suffix) — 将 parent[l] 中 i 右侧所有子节点移入 suffix
   — suffix 保存了 parent 中因分裂可能被挤出的数据
   — 同时 ptI_subdelta 减去了被移出部分在祖先层的度量

2. space = PT_MAX_FANOUT - parent->child_count  -- parent 剩余空槽数

3. pmove = min(space, pend.child_count)  -- 从 pend 尽可能多移入 parent
   将 pend[0..pmove) 移入 parent[i+1..]，更新度量

4. 若 space 还有余（pend 不够填满）：
   smove = min(space - pmove, suffix.child_count)  -- 从 suffix 补入 parent
   将 suffix[0..smove) 移入 parent

5. 剩余未入 parent 的 pend[pmove..) + suffix[smove..) → 全部移入 next
   — next 成为新的内节点，持有挤出的所有子节点

6. pend[l].child_count = 0（已全部移出）
```

**结果**：parent 被填至恰好 PT_MAX_FANOUT 子。next 持有其余，在调用者（flush）中被推入上一层 pend 或触发 splitroot。

### 7. `ptI_splitroot(x, r)` — 分裂根 (line 955)

```
前条件: l==0 时 flush 检测到 root 满，且 r 为打包后的新节点
        pend_root 已由 ptI_checkpendroot 惰性分配

1. 若 levels+1 >= PT_MAX_LEVEL → 返回 PT_ERRFULL（树深达上限）
2. l = pend_root（预分配的空 Node），*l = snap->root  — 旧根变左子
3. snap->root.children[0] = l  (旧根)
   snap->root.children[1] = r  (新分裂节点)
   snap->root.child_count = 2
4. ptN_setmetrics 计算新根两子的度量（各有 bytes/breaks/chars）
5. paths 数组下移一层：memmove(paths+1, paths, (levels+1)*sizeof(...))
   parents 数组同理下移
6. 若 parents[1] == r：cursor 原路径在新根的右侧
   否则 paths[1] 需重新指向旧根 l 中对应槽
7. snap->levels += 1
   pend[新levels].child_count = 0（为新层创建空 pend）
```

**注意**：splitroot 后 levels 增加了 1，pend 数组索引亦相应增长。外层的 while 循环会重新获取 `l = ptC_levels(c)` 以适配新高度。

### 8. `ptI_splitpiece(x)` — 切分 Piece (line 831)

插入位置若在 Piece 中间（非边界），需将该 Piece 切为左右两半。此操作在 `ptI_init` 中执行。

```
1. 若 poff == 0（piece 始于插入点）或 poff == size（piece 终于插入点）
   → 无需切分，返回 OK

2. r = pt_poolalloc 分配新 Piece（右半）

3. 若有行断点 (breaks > 0):
   — 将 l->ends[pbr..breaks) 的偏移减去 poff 后复制到 r->ends[]
   — parent->breaks[i] 更新为 pbr（左半行数）
   — pend->breaks[0] = breaks - pbr（右半行数）

4. 若无行断点 (breaks == 0 或 pbr == 0):
   — 以 charsf 回调计算左右半的字符数（因单行无换行偏移可参照）
   — parent->chars[i] = chars_of_left_part
   — pend->chars[0] = chars - parent->chars[i]

5. parent->bytes[i] = poff（左半字节数）
   pend->bytes[0] = size - poff（右半字节数）

6. r->version = snap->version
   r->data = l->data + poff  — 零拷贝：右半 Piece 直指原数据偏移位置

7. pend->children[0] = r, pend->child_count = 1  — 右半入 pend[levels]
```

**COW 考量**：左半 Piece 被直接修改（度量更新），这是一致性合法的（因其 version 被覆写，不再被其他快照引用）。

### 9. `ptI_checkpendroot(x)` — 惰性预分配 (line 882)

```
if (pend_root 已分配) return OK;
pend_root = pt_poolalloc(S, &S->nodes);  // 仅分配一次
return pend_root ? OK : ERRMEM;
```

惰性分配策略：大多数 insert 不会触达 splitroot（仅在树满了 62*62*...*62 子节点后才需裂根），故不必在 init 时一律分配。仅在 while 循环中需要时方分配。

### 10. `ptI_disposepend(x, l)` — 错误清理 (line 1033)

insert 失败（OOM）时清理 pend[l] 中已分配的所有子节点。

```
for i = 0 .. pend[l].child_count:
    if l == 0:              -- 叶层 pend：children 是 Piece，释放 Piece
        pt_poolfree(&S->pieces, pend[l].children[i])
    else:                   -- 内层 pend：children 是 Node 子树
        ptI_freenodes(S, children[i], l-1)  -- 递归释放整棵子树
        pt_poolfree(&S->nodes, children[i]) -- 释放 Node 自身
```

**为何叶子节点是 l==0**：pend[0] 即叶层（当 levels==0 时），但注意 `ptI_disposepend` 的外层循环是 `for (l = 0; l <= levels; ++l)`，遍历所有层。叶层 pend 存 Piece（因该层 children 实为 pt_Piece*），内层 pend 存 Node。

### 辅助函数

| 函数 | 行 | 功能 |
|------|-----|------|
| `ptI_makespace(d, i, n)` | 889 | 在 d->children[i] 处腾出 n 个槽（memmove 右移） |
| `ptI_move(d, di, s, si, c)` | 898 | 从 s[si:] 移 c 个子节点到 d[di:]（memcpy + 增 child_count） |
| `ptI_adddelta(x, l, m)` | 919 | 将度量 m 加到 l 层及以上所有祖先（沿 parents 链） |
| `ptI_subdelta(x, l, m)` | 926 | 从 l 层及以上所有祖先减去度量 m |
| `ptI_takegapright(x, l, suffix)` | 933 | 将 parent[l] 中 i+1 右侧所有子节点移至 suffix |
| `ptI_packright(x, l, start, suffix)` | 943 | 将 pend[l][start..] + suffix 打包为一个新 Node（未用，预留） |
| `ptI_sumrange(n, start, count)` | 810 | 求 n 的 [start, start+count) 子节点度量之和 |
| `ptI_fillroot(x)` | 821 | 空树专用：直接在 root 上 ptI_fill，光标移至树尾 |
| `ptI_freenodes(S, n, rl)` | 906 | 递归释放 Node 子树：rl=0 仅释 Piece，rl>0 递归 |

## 执行流程示例

### 空树插入 200KB 文本

树为空 → `ptI_init` 中 `ptI_fillroot` 直接在 root 填 4 个 Piece（200KB/65535≈4），全部在 root 内完成。树高为 0（叶即根），无任何分裂。

### 插入 10MB 文本至空树

**Round 1**: `ptI_fill(&pend[0])` — 填 62 个 Piece 到 pend[0]
→ `ptI_flush(l=0)`: `ptI_simple` 成功（root 空，可容 62）→ 62 Piece 入 root

**Round 2**: `ptI_fill(&pend[0])` — 再填 62 Piece
→ `ptI_flush(l=0)`: `ptI_simple` 失败（124 > 62）
→ `ptI_splitinsert` 分配新 Node n2：root 留原有 62 + 部分 pend，余入 n2
→ l==0 → `ptI_splitroot`: root 变为 [旧root(62子), n2(新子)]
→ levels 变为 1

**Round 3~N**: `ptI_fill(&pend[1])` — 填叶层 pend
→ `ptI_flush(l=1)`: simple on root（root 仅 2 子，可容更多）
  → 若 root 满 (2 + pend > 62) → splitinsert → 创建 n3 → pend[0] += n3
  → flush 继续处理 l=0 → simple 或 splitroot

递归终至全文本消耗完毕。

### 非空树插入

光标在树中某位置 → `ptI_init` 执行 COW clone → `ptI_splitpiece` 切开当前 piece
→ pend[levels] 已有 piece 右半 → `ptI_fill` 追加新文本 piece
→ `ptI_flush` 将 pend 级联推入树中，过程同上。

## 避免 OOM 之技巧

### 1. 惰性分配 pend_root

`pend_root` 仅在 `ptI_checkpendroot` 中用到方分配，大多数 insert 不触 splitroot，故省一分配。

### 2. 零拷贝 Piece

`ptI_fill` 中 Piece.data 直指输入缓冲区，无 memcpy。此既省内存，亦省时间。代价：调用方须保输入缓冲区在快照生命期内有效。

### 3. 分段消耗 (partial consumption)

`ptI_fill` 可部分消耗输入文本——若 linesf 回调在某处因 maxlines/maxsize 截断，后续 while 循环继续消耗剩余。这意味着插入不会因单次分配失败而全部回滚——已消耗的内容已写入 pend，仍在树中有效（见"非原子性"语义，README.md）。

### 4. 预分配 + 全或无 (linecache 教训)

linecache 之 `lcB_makeroom` 采用"先计数 → 全预分配 → 再施行 → 释余者"策略，确保分裂要么全成功要么全不发生（树不变）。piecetab 当前未用此策略——`ptI_flush` 若中途 OOM，`ptI_disposepend` 仅释放 pend 中节点，已入树的 Piece 不可回滚。此乃有意为之（非原子性语义）。

### 5. 池分配器避免碎片

`pt_Pool` 按固定尺寸页（PT_PAGE_SIZE=65536）分配，空闲链表回收。同尺寸对象（Node、Piece、Tree）在同一池中，无内存碎片。

## 与 linecache 的不同

| 特性 | piecetab | linecache |
|------|----------|-----------|
| 节点类型 | Node（统一叶和内） | Leaf + Node（分离） |
| 叶层存储 | Piece (指针+ends数组) | Leaf (直接存度量) |
| 数据 | 分段 Piece（≤65535字节），零拷贝 | 无数据，仅行宽度量 |
| 插入算法 | pend 级联 + COW | 逐条写入 + 实时分裂 |
| 度量计算 | linesf 回调扫描输入 | 调用方直接提供 br 位置 |
| 批处理 | pend 填满自动批处理 | 逐条（lc_scan 逐条回调 markbreak） |
| 空树 bulk load | pend 式自底向上（fillroot + pend cascade） | lcB_scan_empty（pend 式自底向上） |
| 非空树插入 | pend 级联（同空树，仅多了 splitpiece） | 逐条游标追加（lcB_fitleaf → lcB_makeroom） |
| OOM 语义 | 非原子：部分成功部分失败 | 预分配后全或无 |
| 填充率 | 叶满 (~100%) | 裂叶取中点 (~50%) |
