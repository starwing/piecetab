# 调试指南（给 Agent 用）

> 基于多次 splice_brute 调试实战的教训总结。
> 目标：少走弯路，快速定位根因。

---

## 一、铁律

1. **概念不清就问**——任何你无法通过读代码直接推理的信息，不要猜，直接问用户。
   猜错的成本远大于多问一句。
2. **隔离再定位**——组合操作（如 splice = remove + append）先拆分，单独验证每步，
   确定问题在哪一步引入的。
3. **日志佐证一切**——对你的推理不要自信。在每个关键点插 `pt_log` 打印实际值，
   用日志证据代替猜测。
4. **先跑最小复现**——如果 brute 测试有大量组合，先缩小到单个失败参数
   （如 `if (!(pos==3)) continue;`），快速迭代。

---

## 二、工作流

### 步骤 1：缩小范围

```
just pt @splice_brute          # 只跑这个测试（@ 前缀 = 只匹配第一个）
```

如果 brute 循环太大，在测试中添加过滤：

```c
if (!(pos == 3 && del == 3 && ins == 1)) continue;
```

### 步骤 2：拆分操作找引入点

对于 `pt_splice`（= remove + append），先单独跑 remove 看树是否健康：

```c
pt_Cursor C2;
maketree(S, &C2, pos);
r = pt_remove(&C2, del);
assert(r == PT_OK);
assert(pt_checktree(C2.tree)); /* ← 关键：隔离 remove */
pt_release(C2.tree);
```

如果 remove 后树是对的，问题在 append。否则在 remove。

### 步骤 3：对比参考实现找差异

**遇到复杂算法（stitch、foldnode、rebalance）时，第一步应该是与参考实现逐行 diff。**

本项目 `linecache.h` 是经过全量测试的正确参考实现。piecetab 的 remove/stitch 子系统是对 linecache 的移植。
应在调试早期就用 `diff` 对比两个代码库的对应函数，逐行确认参数语义是否一致。

本例中，piecetab `ptD_foldnode` 的 `ptD_balancenode` 调用：

```c
// piecetab (错误)
dn = ptD_balancenode(ns, *ns != o, &ds);

// linecache (正确)
dn = lcD_balancenode(ns, (*ns == o), ds);
```

第二个参数的语义（left=1 表示游标在左节点）在两个版本中是**相同**的，但 piecetab 传的条件写反了。
如果一开始就逐行对比 linecache，这个 bug 一眼就能发现。

### 步骤 4：听用户对问题定位的判断

用户对代码的宏观理解远超 agent。当用户说"这个问题不太可能是 X 引起的"时，
应该**立刻调整分析方向**而不是继续在 X 上深挖。

本案例中用户多次提示：
- "cutrange 几乎不可能是引起 underfill 的原因"
- "underfill 基本上是 stitchnode 那边导致的"
- "你应该踏实一点，先加日志"

但 agent 在 cutrange 上消耗了大量时间。

### 步骤 5：插日志验证（缩小到可疑函数后）

在可疑函数的入口和关键分支打印参数：

```c
pt_log("[foldnode] ENTER l=%d i=%d cl=%d cr=%d o=%s\n",
       l, i, cl, cr, (*ns == o) ? "L" : "R");
```

**日志应分层递进**：先看函数级（谁调了谁），再看值级（参数是什么），最后看分支级（进了哪个路径）。

---

## 三、常见弯路与对策

| 弯路 | 表现 | 对策 |
|------|------|------|
| **纠结编译器细节** | 花大量时间分析求值顺序、UB | 先看代码是否存在概念问题。编译器行为几乎不会是你调试的问题根因 |
| **误读数据结构** | 以为 paths[levels] 还有一层 pt_Node | 读设计文档的术语规范（§一.1），理解 `*C->paths[levels]` = piece 指针 |
| **不看文档猜概念** | 对"叶"、"叶容器"有自己的理解 | 找 `notes/design_piecetab_v2.md` 中的术语表，与代码相互验证 |
| **妄想一次修好** | 看到多个错误后一把梭 | 隔离后逐个击破（如先修 mask，再修 underfull） |
| **不问用户硬猜** | 对着错误输出反复推理但不求证 | 用户知道代码意图，远比你自己逆推高效 |
| **不听用户的方向判断** | 用户说"不太可能是 X"，agent 继续在 X 上修 | 用户的宏观理解远超 agent，**立刻转向** |
| **不看参考实现** | 有 linecache.h 却不逐行对比 | 第一步就 diff，单字符 bug 不值得花几轮推理 |
| **禁手术刀式修复** | 看到症状在 A 处就想在 A 处修补 | 先理解数据流本该是怎样的，再找源头 |
| **禁 orig+restore** | 用 pt_locate / re-seek 事后修正 cursor | 在数据流中自然达到正确值，不要事后补救 |
| **擅自套用 linecache 差异** | 看到 linecache 没有 upmask 就删 piecetab 的 upmask | piecetab 有 hole/literal 双叶类型，mask 是必需的；差异先问用户 |
| **忽视 paths 不一致窗口** | findroom 后部分 paths 指向旧子树，仍调全层级 upmask | upmask 需起始层级参数；先确认 paths 一致性范围再判断遍历层级 |

---

## 四、Agent 典型案例：splice_brute 调试回顾

### 错误 1：忽视用户的明确禁止

用户说了"先给根因和方案，不要修复"，agent 立刻开始改代码。用户说了"严禁 orig+restore 模式"，
agent 用了 `pt_locate()` 做 cursor 事后补救。

**教训**：用户的规则是硬约束，不是建议。违反的代价是方案被否决、信任丧失。

### 错误 2：没有先逐行对比 linecache

piecetab 的 stitch/foldnode/rebalance 是对 linecache 的移植。agent 被用户提醒"对比 linecache.h"
多次，但始终没有系统性地逐行 diff 两个文件中的对应函数。

如果做了 diff，`ptD_balancenode(ns, *ns != o, &ds)` vs `lcD_balancenode(ns, (*ns == o), ds)`
这个单字符差异会立刻被发现。

**教训**：有参考实现时，diff 是第一优先级。不要试图从零理解复杂算法。

### 错误 3：在错误的地方深挖

用户多次提示"underfill 是 stitchnode 的问题"，但 agent 在 cutrange、trimright 上做了大量
分析和实验性修复（级联删除、cursor repair等），全都在错误的方向上。

**教训**：用户的宏观判断值得信任。当你花了 3 轮以上还在同一个子问题（如 cutrange）上修时，
应该停下来问自己：用户说的方向对不对？

### 错误 4：修复症状而非根因

agent 看到 `ptD_foldnode` merge 路径 cr=0 时 cursor 越界，第一反应是"在 merge 里加 cr==0 的特判"。
这是修复症状。真正的根因是 foldnode 的 balance 方向参数写反了，导致数据分布错误，
进而创造了 cr=0 的 merge 场景。

**教训**：看到异常状态（cr=0, p.cc=0 等）时，追问"这个异常状态是怎么产生的"，
而不是"如何在这个异常状态下不崩溃"。

### 错误 5：日志不加在正确的层级

agent 的日志最初加在 cutrange 的函数级（入口/出口），后来才加到 stitchnode 的洋葱序循环中。
应该在**所有可疑函数**的关键路径上同时加日志，而不是逐层深入。

**教训**：日志应该一次铺开到所有可疑区域，而不是"先在 A 打，A 没问题再去 B"。多区域同时日志
能一次性看到完整执行流。

### 错误 6：不看设计文档，擅自对比 linecache 做错误推理

agent 看到 linecache 的 `lcD_stitchnode` 中没有 `lcM_upmask` 调用，就推断 piecetab 中对应的
`ptM_upmask` 是多余的直接删除。然而 linecache 没有 hole/literal 双叶类型（只有 lc_Leaf），
不需要 mask 位图，所以不需要 upmask。piecetab 因 hole 类型必须维护 mask，upmask 是**必需的**。

**教训**：
- **不要假设 linecache 里没有的就是多余的** — piecetab 有 hole/literal 双叶类型，多了 mask 位图维护需求
- **遇到 linecache 和 piecetab 代码不一致时，先问用户原因**，不要自己直接下结论
- **不要在不理解的代码上做删除操作** — 违背铁律"不准私修"、"先给根因和方案再动"

### 错误 7：paths 不一致时调用 upmask 导致崩溃 — 应识别调用点安全性

stitchnode 洋葱序中 `findroom`/`makechain` 修改了部分 paths（from..to 层级）后，深层 paths（to+1..levels）
仍然指向旧子树。此时调用 `ptM_upmask`（从 levels-1 开始遍历全路径）会因深层路径指向错误节点而崩溃。

**根因**：`ptM_upmask` 没有层级范围参数，始终从 levels-1 向下遍历，在路径不一致时必然越界。
正确做法是给 `ptM_upmask` 加起始层级参数，仅在路径一致的范围内传播 mask。

**教训**：在 stitchnode 的 findroom 调用点之后，cursor paths 只有从某个层级之上是一致的。
`ptM_upbytes` 有范围参数（l..0），但 `ptM_upmask` 没有——这是移植时遗漏的差异。调试时
应先确认**paths 在哪个层级开始不一致**，再判断 upmask 的遍历范围是否正确。

---

## 五、本项目特定常识

### 叶 / 叶容器

```
levels = 3 的树：

  -1:  root               ptK_parent(C, 0) 才能取到
   0:  内节点              *C->paths[0]
   1:  内节点              *C->paths[1]
   2:  叶容器              *C->paths[2] = ptK_parent(C, 3)
   3:  叶 = piece          *C->paths[3] = literal/hole 指针
```

- **叶** = piece = `*C->paths[levels]`，不是 pt_Node
- **叶容器** = `ptK_parent(C, levels)`，其 children 直接存 piece 指针
- 详见 `notes/design_piecetab_v2.md` §一.1

### mask 维护铁律

`ptI_splitins` 只更新度量（upbytes），不做 mask 传播（upmask）。
**调用方负责在 `ptI_splitins` 后调用 `ptM_upmask`**。违者出现 `[chk] MASK ... has_hole=1` 类错误。

### 参考实现

`linecache.h` 是 piecetab 的 remove/stitch 子系统的参考实现。遇到相关 bug 时，
第一步应该是 `diff` 两个文件中的对应函数。

### 调试打印宏

```c
#define pt_log(...) fprintf(stderr, __VA_ARGS__)
```

定义在 `tests/pt_tests.h:18`，piecetab.h 中可直接使用（测试编译时已先 include pt_tests.h）。
LSP 可能报错 `implicit declaration`，忽略即可。

### 运行测试

```bash
just pt                    # 跑 piecetab.h 的全量测试
just pt @test_name         # 只跑第一个匹配
just pt test_prefix        # 跑所有前缀匹配
just lc                    # 跑 linecache.h 的全量测试
just lc @test_name         # 跑 linecache.h 的全量测试
just lc test_prefix        # 跑 linecache.h 的全量测试
```

---

## 六、总结迭代模板

调试结束后，你对用户汇报应包含：

1. **根因**：什么导致的（精确到行号）
2. **修复**：改了什么（精确到 diff）
3. **证据**：日志/测试输出证明修复有效
4. **影响**：全量跑后的状态（多少 OK，有无新失败）

---

## 七、Agent 案例 2：rmleaf 末尾越界 + pt_read 性能优化

### 案例：ptD_rmleaf 删除末尾 piece 后 cursor idx 越界

**症状**：`pt_checkcursor` 报 `PATHS[3] invalid idx=3 cc=3`，offset 本身正确(254)。

**根因**：pos=254 (leaf 内偏移6, idx=3, poff=0)，del=2 恰好删掉整个 children[3]。
`ptD_cutpiece` 调用 `ptN_remove` 将 cc 从 4 降到 3，但 paths[3] 仍指向已删除的 slot。
rmleaf 只处理了 `cc==0`（空 leaf）的情况，没处理"删除末尾 piece 但 leaf 非空"。

**修复**：
1. 对照 linecache 的 `lcD_foldleaf:602-604` 发现 foldleaf 第一件事就是检查 `i==cc` 并回退。
2. 对照 `ptD_stitch:1160-1163` 发现 rmrange 路径有相同回退逻辑，rmleaf 遗漏了。
3. 在 `ptD_cutpiece` + `ptM_up` 之后、`cc==0` 之前，添加末尾回退：
   ```c
   else if (ptK_idx(C, p, l) == ptN_cc(p)) {
       C->paths[l] -= 1;
       C->poff = p->bytes[ptN_cc(p) - 1];
       C->off -= p->bytes[ptN_cc(p) - 1];
   }
   ```
4. 同步将 rmleaf 接口从双游标 `(L,R)` 改为单游标 `(C, del)`，`R->poff` 由 `C->poff + del` 推算。

**教训**：
- **两条删除路径的 cursor 收尾逻辑必须对齐**（rmleaf vs rmrange）
- **offset 正确不代表 paths 有效**，需逐层检查索引有效性
- **有参考实现时先 diff**：linecache foldleaf 已有 i==cc 回退，对比就能发现缺失

### 案例：pt_next 语义不对称导致 pt_read 笨拙

**症状**：pt_read 需同时用 pt_piece（探大小）+ pt_next（消费+前进），双函数组合冗余。

**根因**：pt_next 是"返回当前、再前进"（post 语义），pt_prev 是"先后退、再返回落脚点"（pre 语义）。
两者不对称，且 pt_next 的返回值与 pt_piece 完全重叠（都是当前 piece 数据）。

**修复**：将 pt_next 改为"前进后返回落脚点"，与 pt_prev 完全对称。
- 实现：前进操作不变，尾调用 `pt_piece(C, plen)` 返回新位置数据。
- pt_read 简化为标准遍历惯用法：
  ```c
  for (p = pt_piece(C, &n); len > 0 && n > 0; p = pt_next(C, &n)) {
      m = pt_min(n, len);
      memcpy(buf, p, m), buf += m, len -= m, total += m;
      if (m < n) { C->poff += m; break; }
  }
  ```
- 遍历惯用法：`for (p = pt_piece(C,&n); n; p = pt_next(C,&n))`

**教训**：
- **API 对称性比便利性重要**：next/prev 语义应镜像，否则调用方被迫用不同模式
- **返回值不要和另一个函数重叠**：pt_next 返回当前 piece 数据 = pt_piece 的职责
- **性能问题可能是 API 坏味道的信号**：pt_read 笨拙指向 pt_next 设计问题

### 案例：性能 profiling 用 clock() 快速定位瓶颈

**方法**：在测试循环中分阶段累加 `clock()` 差值，最终一次性打印各阶段总耗时。
```c
unsigned long cats[5] = {0}; clock_t t;
t = clock(); /* phase A */ cats[0] += clock() - t;
...
pt_log("[prof] A=%lu B=%lu C=%lu\n", cats[0], cats[1], cats[2]);
```

**教训**：
- **数据先于优化**：用户直觉"校验慢"被数据证实——content verify 占 44%
- **pt_read 瓶颈是 pt_advance 而非 memcpy**：每个 piece 都做 O(depth*FANOUT) 定位，用 pt_next (O(depth)) 替代可减半
- **别猜瓶颈在哪，上 clock() 再动手**
