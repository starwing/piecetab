# 代码精炼要诀

本文件记录 piecetab 代码精炼（refine）实验中的核心经验。

## 核心信条

**精炼不是为了减行数，而是为了减杂质。** 25行限制是工具，不是目标。为了凑行数引入 flag、拆分函数、或制造短路逻辑，都属于庸工。改完若不如改前清晰，就是倒退。

## 七大原则

### 1. 自然数据流 > 人工 flag

| 坏 | 好 |
|---|---|
| 先设 flag 表示状态，后面再检查 | 计算一个值，让数据本身告诉你答案 |

**例：`ptD_mergeleaf`**

坏版本：
```c
if (可以合并左右) merged = 1;
else if (左右都是 hole) {
    if (左边有空间) { 部分合并; goto done; }
    else { 完全合并; merged = 1; }
}
done: if (!merged) { /* 不合并路径 */ }
```

好版本：
```c
if (左右都是 hole) d = pt_min(右大小, 左剩余空间);
ptH_append(左, d);
if (d < 右大小) ptH_remove(右, d);  // 部分 → 右有剩
else ptP_free(右), merged = 1;       // 满 → 右被吃光
if (!merged) { /* 不合并路径 */ }
```

`d` 的值自然告诉你这是部分合并还是完全合并，无需额外检查"空间够不够"。

**注意：** `merged` 在这里仍然是必要的——它记录"是否合并了整片右叶"，这是数据流无法直接回答的布尔问题。关键区别在于：`merged` 是**计算的结论**，不是**预设的标记**。

### 2. 先计算，后判断

把复杂条件中的公共子表达式提到前面，让判断变得简短。

```c
// 繁
if (!ptM_ishole(p, cc-1) && !ptM_ishole(rt, 0) && ...)

// 简
hL = ptM_ishole(p, cc-1), hR = ptM_ishole(rt, 0);
if (!hL && !hR && ...)
```

### 3. 用宏，不用 cast

```c
// 坏
(const char *)p->data
(const char *)ptN_hole(rt, 0)->data

// 好
ptN_lit(p, cc-1)
ptN_hole(rt, 0)->data
```

宏不仅短，而且语义自明。

### 4. 统一 > 特例

不要为边界情况写单独的分支。把它融进主循环。

**例：`ptC_freeze` 中的 `levels==0`：**

坏版本：
```c
if (tree->levels == 0) { 单独处理; return; }
for (l = tree->levels - 1; l >= 0; l--) { ... }
```

好版本：
```c
for (l = tree->levels; l > 0;) {
    --l;
    if (l >= tree->levels) break;  // levels==0 时立即 break
    ...
}
```

### 5. 逗号表达式替代单行块

纯赋值的单行 `if` 块，用 `,` 取代 `{ }`：

```c
// 6行
if (del > 0) {
    r = pt_remove(C, del);
    if (r != PT_OK) return r;
}

// 1行
if (del > 0 && (r = pt_remove(C, del)) != PT_OK) return r;
```

### 6. 变量少即是多

删掉只出现一次的栈变量：

```c
// 引入 levels 只是为了省几次函数调用 —— 不值得
int levels = ptK_levels(C);
... levels ... levels ... levels ...

// 直接用函数调用，或声明在 for 里
for (int l = ptK_levels(C); l > 0;) { ... }
```

但**不要重复计算有副作用的表达式**——如果函数无副作用且编译器能优化，重复调用比引入临时变量更清晰。

### 7. 命名里藏领域

```c
ptK_cowedit  → ptD_cowpaths  // 它是删除流程中的 COW 路径分裂，不是通用工具
de            → d              // 简称够用时就不需要全称
```

函数名反映分类（D=删除, K=光标操作）而非机制（cow）。

## 四种浪费

| 浪费 | 典型表现 | 代价 |
|---|---|---|
| **预制 flag** | 先设 flag → 后面再检查 | 阅读时需要跟踪 flag 状态机 |
| **特例分支** | `if (levels == 0)` 提前 return | 两条路径理解成本翻倍 |
| **多余变量** | `levels` 声明在函数开头，函数后半节不出现 | 心理负担 |
| **大块分支** | `if { r=...; if(r!=OK) return r; }` | 占用屏幕行 |

## 精髓一句话

**计算自然的值，让值引导决策。不要预设答案（flag），然后去核对。**
