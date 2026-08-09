# spantree cdir 推导分析

## 问题

journal entry 有 `(off, del, ins, left?)`。compose 后 hunk 为 `(pa, ca, pdel, cins)`。
需要给 hunk 加上 `cdir`，推导规则满足 compose 结合律。

## compose 算法回顾（undotree.h:419-449）

三分支：

```
emitX2Y: A_i.ca+cins < B_j.pa → A 在前，输出 A 的 hunk（ca += zoff）
emitY2Z: B_j.pa+pdel < A_i.ca → B 在前，输出 B 的 hunk（pa -= xoff）
emitcross: 相交 → surv = A.cins−B.pdel
  cins = max(0,surv) + B.cins  // A 幸存 + B 新增
```

关键：emitcross 中 `cins` 是两部分拼起来的。surv 来自 A 的 insert（其 cdir=A.cdir），B.cins 来自 B 的 insert（cdir=B.cdir）。当 surv>0 且 B.cins>0 且两者 cdir 不同时，单一 cdir 无法同时编码两部分的方向。

## 推导

### 简单情况

| 分支 | cdir 输出 | 理由 |
|------|-----------|------|
| emitX2Y (A) | A.cdir | A 是唯一贡献者 |
| emitY2Z (B) | B.cdir | B 是唯一贡献者 |
| emitcross, surv≤0 | B.cdir | cins 全来自 B |
| emitcross, surv>0, B.cins=0 | A.cdir | cins 全来自 A |

### 难例：emitcross, surv>0, B.cins>0, A.cdir≠B.cdir

例子：
- X 文本: "abc"K | "def"P （两段，pos 3 是边界）
- A: append "XX" at pos 3, cdir=left  → 继承 K
- B: at pos 4, del=1("X"), ins=1("y"), cdir=right → "y" 继承 P

顺序应用：
```
After A: [0,5)K | [5,8)P  ("abcXXdef")
After B: [0,4)K | [4,7)P  ("abcXydef")  ← "y" 继承了 P
```

compose 后 hunk: (pa=3, ca=3, pdel=0, cins=2) — 插入 2 字节。
- cdir=left → 都继承 K → 错误（"y" 应该是 P）
- cdir=right → 都继承 P → 错误（"X" 应该是 K）

**单 cdir 在此情形下不可能正确。**

### 分拆方案

将 emitcross 拆为两个 hunk（右到左应用）：
```
hunk1: (pa=X_start,     ca=Z_start,     pdel=0, cins=surv,  cdir=A.cdir)
hunk2: (pa=X_start+surv, ca=Z_start+surv, pdel=0, cins=B.cins, cdir=B.cdir)
```
R-to-L apply: 先 #2 继承 X 侧 pa 处的右/左 span，再 #1 在前继承。

但问题：B 的 cdir 语义是在 **Y 坐标**下定义的（B 插入的"左/右"指 Y 中的 span）。而 hunk2 的 pa 在 **X 坐标**，两边的 span 在 A 尚未 apply 时未必相同。除非 spantree 的当前状态（X 版）中 pa 位置的 span 恰好在 A 插入后也与 Y 版中 B 的插入位置 span 一致。这对子区间插入成立（段内继承同属性）、对跨段边界插入不成立。

### compose 不改 cdir 的条件

如果限制"所有插入都在段内且非跨门槛"，则 A 插完后该区域整个都是同属性，B 的 cdir（左或右）都是同一属性。此时单 cdir 任意取 A.cdir 或 B.cdir 都正确。但这是强假设，不通用。

## 结论

**cdir 在 compose 的 emitcross 情形下无法通过简单规则无损推导**。需要：

- (a) 分拆 hunks（如果允许在 compose 输出中插入额外 hunks）
- (b) 或者承认 cdir 在 compose 后会丢信息，依赖回染修复
