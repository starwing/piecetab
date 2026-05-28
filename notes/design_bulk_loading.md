# lc_scan Bulk Loading 实现记录

## 最终设计（仿 piecetab pend 段式）

**核心**：scanner 返回值直写 Leaf（按需分配），不预收集入 `unsigned[]` 中间数组。叶满则推入 `lcB_item` 动态数组（pend 段），scanner 耗尽后自底向上建树。

### 新增/替换函数

| 函数 | 行数 | 说明 |
|------|------|------|
| `lcB_fillleaf` | 19 | 分配 Leaf，scanner 直填至满或耗尽。返回 `(Node*, ne, ob)` 或 NULL |
| `lcB_buildlevel` | 18 | 自底向上：从 `lcB_item[]` 建上一层内节点（复用旧逻辑） |
| `lcB_additem` | 14 | 将 `(Node*, bytes, breaks)` 推入动态 items 数组（自动扩容） |
| `lcB_setroot` | 12 | 从 items 自底向上建树，结果写入 root Node |
| `lcB_scan_empty` | 28 | 空树入口：fillleaf → additem → setroot → 设树度量 |
| `lc_scan` (重写) | 19 | 空树调 `lcB_scan_empty`，非空树游标逐条追加（无 buf） |

### 删除函数

`lcB_makeleaf`, `lcB_scanbuf`, `lcB_setemptytree`（旧逻辑：预收集全量 buf 再建树）

### 保留函数

`lcB_fitleaf`, `lcB_putbreak`（markbreak 共用）

## 测试结果

- **91 测试**：85 原有 + 6 新增，全部通过
- **覆盖率**：99.5% (769/773 行)，100% 函数
- **格式**：clang-format 通过
- **ASAN/UBSAN**：无报错

### 新增测试

| 测试 | 说明 |
|------|------|
| `test_scan_no_input` | 空树 + scanner 返回 0 → LC_OK |
| `test_scan_one_leaf` | scanner 输出恰满一叶 → levels=0, cc=1 |
| `test_scan_bulk_many` | 120 条目建多级树 → levels≥2 |
| `test_scan_append_many` | 非空树追加 → 度量正确累加 |
| `test_scan_oom_items` | items 数组分配失败 → LC_ERRMEM |
| `test_scan_oom_build` | 内节点创建失败 → LC_ERRMEM |

### 未覆盖行（4 行，预存）

`DA:475`, `DA:696`, `DA:697`, `DA:720` — lcK_backwardline / lcD_shiftnode / lcD_foldleaf 中，与批量加载无关。

## 填充率验证

120 条目 (LC_LEAF_FANOUT=4, LC_FANOUT=4)：
- 30 叶全满 (100%)
- 8 内节点 (7 满 100%, 1 半满 50%)
- 根节点 2 子 (50%)

旧算法只能 ~50% 叶满填。
