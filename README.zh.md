# ![Piecetab](logo.svg)

[![Build](https://github.com/starwing/piecetab/actions/workflows/test.yml/badge.svg)](https://github.com/starwing/piecetab/actions/workflows/test.yml) [![Coverage Status](https://coveralls.io/repos/github/starwing/piecetab/badge.svg?branch=master)](https://coveralls.io/github/starwing/piecetab?branch=master)

[English](README.md) | **中文**

三个轻量级 stb 风格单头文件 C89 库，用于构建高性能文本编辑器 buffer：

- **`piecetab.h`** — 基于 B+ 树的字节级 piece table，支持写时复制（COW）
  快照、事务化编辑、零拷贝读取。
- **`linecache.h`** — 计量 B+ 树（Metric B+ Tree），维护字节偏移 ↔ 行号
  映射，在高频编辑下保持行号缓存。
- **`undotree.h`** — 基于区间代数的版本图 + 编辑日志 + 差分服务，骑在
  `pt_Buffer` COW 快照之上。

三库相互独立、可自由组合：piecetab 只存字节（"clean octet"——不管行、
不管编码），linecache 只管行断点，undotree 管理版本图并为任意两版本计算差分。
三者组合即得支持 O(log n) 偏移 ↔ 行号双向导航和 undo/redo 的完整编辑器 buffer。

## 动机

本项目源于对**高性能、低延迟文本 buffer** 的需求——在高频编辑、大文件、
复杂内容下保持可预测的行为：

- 插入/删除负载下性能稳定
- 廉价快照，支撑 undo/redo 与异步消费者
- 紧凑的单头文件实现，便于嵌入

## 特性

### piecetab.h

- **不可变 Buffer + COW**：`pt_Buffer` 是带引用计数的快照；游标上的首次编辑
  fork 出私有 transient 树，`pt_commit` 冻结为新 Buffer，`pt_rollback` 丢弃
- **两种 piece**：零拷贝 *literal*（引用用户内存）与池化可变 *hole*
  （原位吸收小编辑）
- **事务化 OOM 安全**：编辑前预留池对象；返回 `PT_ERRMEM` 时结构保持
  一致，游标仍然有效
- **arena 直写 literal**：`pt_reserve` / `pt_scratch` / `pt_literal`
  将字节直接写入树的 arena，免二次拷贝
- **分代压缩**：每个编辑代独占自己的 arena；`pt_compact` 产出独立的
  紧凑新 Buffer——旧代持有的字节拷入紧凑新 arena，外部内存（如大文件
  mmap）原指针引用不拷贝，release 旧链即回收其全部内存

### linecache.h

- **计量 B+ 树**：每子树双计字节数与行数，双向 O(log n) 导航
- **批量加载**：`lc_scan` 通过 scanner 回调自底向上建树，远快于逐行插入
- **完整编辑**：单点行断插入（`lc_markbreak`）、区间删除（`lc_remove`）、
  删插字节（`lc_splice`）、中部文本插入（`lc_insert` / `lc_append`），
  全部支持 OOM 完整回滚

### undotree.h

- **版本图**：不可变快照树（`ut_Node`），每节点携带与 parent 的 changeset
  (hunk 列表) 和不透明 payload（如 `pt_Buffer`）
- **编辑日志**：未提交编辑以 `(off, del, ins)` 三元组暂存，commit 时规范化
  为 hunk 列表
- **Hunk 代数**：compose（X→Y ∘ Y→Z → X→Z）、取逆、规范化等区间变更运算
- **Fresh-vid 协议**：`ut_freshvid(S)` 哨兵表示未提交状态；
  `ut_diff(from, to)` 经四阶段 compose 处理任意 committed 版本 + fresh
  端点的组合

## 快速上手

三库皆为 stb 风格：任意处包含头文件，在恰好一个编译单元中定义
`*_IMPLEMENTATION` 宏。

### piecetab.h

```c
#define PT_IMPLEMENTATION
#include "piecetab.h"

int main(void) {
    pt_State *S = pt_open(NULL, NULL);        /* 默认分配器 */
    pt_Buffer src, out;
    pt_Cursor C;
    char      buf[32];
    size_t    n;

    src = pt_from(S, "hello world", 11);      /* 零拷贝 buffer */
    pt_seek(&C, src, 5);
    pt_insert(&C, ",", 1);                    /* 引用语义 */
    out = pt_commit(&C);                      /* 冻结为新 buffer */

    pt_seek(&C, out, 0);
    n = pt_read(&C, buf, sizeof(buf));        /* "hello, world" */

    pt_release(src);
    pt_release(out);
    pt_close(S);
    return (int)n;
}
```

### linecache.h

```c
#define LC_IMPLEMENTATION
#include "linecache.h"
#include <string.h>

/* scanner 返回下一行长度（含 '\n'），返回 0 结束 */
static unsigned scan(void *ud, size_t pos) {
    const char **s = (const char **)ud;
    const char  *nl = strchr(*s, '\n');
    unsigned     len;
    (void)pos;
    if (nl == NULL) return 0;
    len = (unsigned)(nl - *s) + 1;
    *s += len;
    return len;
}

int main(void) {
    const char *text = "one\ntwo\nthree\n";
    lc_State *S = lc_open(NULL, NULL);
    lc_Cache *c = lc_newcache(S);
    lc_Cursor C;

    lc_scan(c, scan, &text);           /* 批量加载行断点 */
    lc_seekline(&C, c, 2);             /* 定位第 2 行行首 */
    /* lc_offset(&C) == 8, lc_breaks(c) == 3 */

    lc_close(S);                       /* 释放全部 cache */
    return 0;
}
```

## API 总览

### piecetab.h

| 类别     | 函数                                                                                     |
| -------- | ---------------------------------------------------------------------------------------- |
| 生命周期 | `pt_open`, `pt_close`, `pt_reset`, `pt_getallocf`                                        |
| Buffer   | `pt_empty`, `pt_from`, `pt_compact`, `pt_retain`, `pt_release`                           |
| 查询     | `pt_bytes`, `pt_version`                                                                 |
| 游标     | `pt_seek`, `pt_locate`, `pt_advance`, `pt_offset`                                        |
| 读取     | `pt_read`, `pt_piece`, `pt_next`, `pt_prev`                                              |
| 编辑     | `pt_edit`（拷贝语义），`pt_insert` / `pt_append` / `pt_splice` / `pt_remove`（引用语义） |
| 事务     | `pt_commit`, `pt_rollback`                                                               |
| Arena    | `pt_reserve`, `pt_scratch`, `pt_literal`                                                 |

引用语义编辑（`pt_insert` 等）**不拷贝**输入字节——调用者须保证内存在
任何引用它的 Buffer 存活期间有效。`pt_edit` 拷贝进 hole piece（单次
`len <= PT_MAX_HOLESIZE`）。

### linecache.h

| 类别     | 函数                                                                                 |
| -------- | ------------------------------------------------------------------------------------ |
| 生命周期 | `lc_open`, `lc_close`, `lc_reset`                                                    |
| Cache    | `lc_newcache`, `lc_delcache`                                                         |
| 批量     | `lc_scan`                                                                            |
| 查询     | `lc_breaks`, `lc_bytes`                                                              |
| 游标     | `lc_seek`, `lc_seekline`, `lc_locate`, `lc_locline`, `lc_advance`, `lc_advline`      |
| 查询     | `lc_offset`, `lc_line`, `lc_col`, `lc_lineoffset`, `lc_linelen`                      |
| 编辑     | `lc_markbreak`, `lc_clearbreaks`, `lc_remove`, `lc_splice`, `lc_insert`, `lc_append` |

### undotree.h

| 类别     | 函数                                                                       |
| -------- | -------------------------------------------------------------------------- |
| 生命周期 | `ut_open`, `ut_close`, `ut_setcleaner`                                     |
| 树       | `ut_newtree`, `ut_deltree`                                                 |
| Journal  | `ut_record`, `ut_unrecord`, `ut_freshcount`, `ut_discard`                  |
| 版本     | `ut_commit`, `ut_switch`                                                   |
| 导航     | `ut_root`, `ut_current`, `ut_parent`, `ut_payload`, `ut_childcount`       |
| 导航     | `ut_firstchild`, `ut_lastchild`, `ut_nextsib`, `ut_younger`, `ut_older`   |
| 导航     | `ut_ancestor`                                                              |
| Diff     | `ut_freshvid`, `ut_diff`, `ut_freshdiff`, `ut_hunks`, `ut_mapoffset`    |

完整 API 参考见 [`docs/piecetab.zh.md`](docs/piecetab.zh.md)、
[`docs/linecache.zh.md`](docs/linecache.zh.md) 与
[`docs/undotree.zh.md`](docs/undotree.zh.md)。

## 配置

在包含实现之前覆盖以下宏：

| 宏                              | 默认  | 含义                     |
| ------------------------------- | ----- | ------------------------ |
| `PT_FANOUT` / `LC_FANOUT`       | 62    | 节点最大子数             |
| `LC_LEAF_FANOUT`                | 62    | 叶最大行数               |
| `PT_MAX_HOLESIZE`               | 64    | hole piece 容量          |
| `PT_MAX_LEVEL` / `LC_MAX_LEVEL` | 16    | 最大树深                 |
| `PT_PAGE_SIZE` / `LC_PAGE_SIZE` | 65536 | 池分配器页大小           |
| `PT_ARENA_SIZE`                 | 1024  | arena 块最小容量         |
| `PT_COMPACT_RANGES`             | 64    | compact 区间数组初始容量 |
| `UT_PAGE_SIZE`                  | 65536 | undotree: 池分配器页大小     |

三库均可在 `*_open` 时传入自定义分配器（`lc_Alloc` / `pt_Alloc`
/ `ut_Alloc`，Lua 风格 realloc 签名）。

## 文档

- [`docs/piecetab.zh.md`](docs/piecetab.zh.md) — piecetab API 参考与实现笔记
- [`docs/linecache.zh.md`](docs/linecache.zh.md) — linecache API 参考与实现笔记
- [`docs/undotree.zh.md`](docs/undotree.zh.md) — undotree API 参考与集成指引
- [`notes/`](notes/) — 设计文档：架构总览（`brief_*.md`）、算法设计
  （`design_*.md`）、区间删除算法演进史

## 测试

测试以极小扇出（4）配合 ASan/UBSan 运行以逼树分裂，并有 lcov 覆盖率
构建。三个头文件均保持 **100% 行/函数覆盖**与约 90% 分支覆盖。

```sh
just lc     # linecache 测试
just pt     # piecetab 测试
just ut     # undotree 测试
just cov    # 覆盖率报告
```

编码规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 许可证

[MIT](LICENSE)，与 Lua 相同。
