# undotree.h 项目总览

> 供 Agent 快速了解项目全貌。可 grep 查询的信息不载此文，提供查询命令。

## 一、项目概要

单头文件 C89 库，前缀 `ut_`。以 `(off, del, ins)` 区间代数维护**版本图 + 编辑日志 + 差分服务**，是库族第三成员（piecetab、linecache 之后）。

**功用**: 骑在 `pt_Buffer` COW 之上（payload 提供任意版本完整快照），**非** COW 的替代品。消费者自持 `ut_Vid`，按需调 `ut_diff` 拉差分自行 apply。

```bash
# 查行数
wc -l undotree.h

# 查测试用例数
grep -c 'X(' tests/ut_test.c
```

## 二、数据结构

### ut_State — 全局状态

```c
struct ut_State {
    ut_Node     base;       /* must be first; sentinel for ut_freshvid */
    ut_Alloc   *allocf;
    void       *ud;
    ut_Pool     pool;       /* node object pool (from linecache mempool) */
    ut_Cleaner *cleaner;
    void       *cud;
    ut_Hunk    *scratch;    /* vec: diff/compose temp buffer */
};
```

`base` 首字段 → `ut_freshvid(S) = (ut_Vid)(S)` 哨兵。
`pool` 从 linecache.h 搬入的页式池分配器，专用于 `ut_Node`。

### ut_Tree — 版本树

```c
struct ut_Tree {
    ut_Node   root;       /* 嵌入（非指针）          */
    ut_State *S;
    ut_Node  *current;    /* 当前 head               */
    ut_Entry *journal;    /* vec: 未提交编辑          */
    int       diffhn;     /* 待命 diff hunk 数, -1=无 */
};
```

### ut_Node — 版本节点（环状单链表）

```c
struct ut_Node {
    ut_Node    *parent;       /* parent (NULL at root)            */
    ut_Node    *last_child;   /* ring: youngest child              */
    ut_Node    *next_sib;     /* ring: → next younger sibling      */
    ut_Payload *payload;      /* snapshot (e.g. pt_Buffer)         */
    ut_Hunk    *h;            /* vec: parent→this changeset        */
    int         depth;        /* distance from root                */
    int         child_count;  /* O(1) child count                  */
};
```

环状单链表：`last_child->next_sib` = 最老兄弟（first_child）。
`next_sib` 方向为"更年轻"。插新子节点在 `last_child` 之后 → O(1)。

### ut_Hunk / ut_Entry / ut_Vid

```c
struct ut_Hunk  { size_t pa, ca, pdel, cins; };
struct ut_Entry { size_t off, del, ins;       };
typedef const ut_Node *ut_Vid;
```

### Vec 基础设施（仅用于 hunk 和 journal）

header 前置：`| utV_Header[len,cap] | elem[0] ... |`。

```c
#define utV_init(A)     ((A) = NULL)
#define utV_len(A)      ((A) ? utV_hdr(A)->len : 0u)
#define utV_free(S, A)  ((A) && utV_resize_(S, (void **)&(A), 0, sizeof(*(A))))
#define utV_push(S, A, V)  /* UT_OK/UT_ERRMEM */
```

> 尾部下划线（`utV_resize_`、`utV_grow_`）表示私有函数，仅通过宏间接调用。
> Vec 仅服务于线性序列（journal、hunk list）。Node 子节点改用环状单链表。

### Node 内存池（ut_Pool）

从 linecache.h 搬入的 `lc_Pool`（前缀 `utP_`）。页式分配 + freelist 回收，
配合环状单链表结构达成节点地址终身稳定和复用。

## 三、公共 API

```bash
grep '^UT_API' undotree.h
```

| 类别     | 函数/宏                                                       | 功用                              |
| -------- | ------------------------------------------------------------- | --------------------------------- |
| 生命周期 | `ut_open`, `ut_close`                                         | 状态管理                          |
| 生命周期 | `ut_setcleaner`                                               | 设置 payload 释放回调             |
| 树       | `ut_newtree`, `ut_deltree`                                    | 树生命周期                        |
| journal  | `ut_record`, `ut_unrecord`, `ut_freshcount`, `ut_discard`     | 编辑日志 (off,del,ins)            |
| 版本     | `ut_commit`, `ut_switch`                                      | commit/切换 head                  |
| 导航     | `ut_parent`, `ut_payload`, `ut_childcount`, `ut_firstchild`   | 树遍历（宏）                      |
| 导航     | `ut_nextsib`, `ut_younger`, `ut_older`                        | 兄弟/时间序跳转（宏）             |
| 导航     | `ut_root`, `ut_current`, `ut_ancestor`                        | 根/head/LCA                       |
| diff     | `ut_freshvid`, `ut_diff`, `ut_freshdiff`, `ut_hunks`          | 差分计算 + 结果查询               |

### ut_younger / ut_older（:earlier/:later 跳转）

环状单链表天然支持。兄弟按 commit 先后排列，"更年轻"方向 = `next_sib`。

```
ut_younger(v):  向"更新"方向跳
  v->last_child         → youngest child（下钻）
  (v is oldest sib)     → v->parent（上溯）
  otherwise             → youngest sibling（同辈最新）

ut_older(v):    向"更旧"方向跳
  (v has children)      → oldest child（下钻至最老）
  (v is youngest sib)   → v->parent（上溯）
  otherwise             → next older sibling（沿环前驱, O(k)≈O(1)）
```

兄弟数极少（单深树=1，分支=2-3），prev-sibling O(k) 实际是 O(1)。

## 四、fresh 位点协议

去除 `ut_Pos.k` 偏移量，fresh 用哨兵 `ut_freshvid(S)` 表达：

| from | to | 场景 |
|---|---|---|
| commit | fresh | 消费者追平草稿 |
| fresh | commit | undo 回退 |
| commit | commit | 正常 diff |
| fresh | fresh | 恒等 |

`ut_diff` 内部四阶段 compose：`[inv(fresh)] + from→LCA⁻¹ + LCA→to + [fresh]`。

## 五、核心算法

### 5.1 规范化（journal → hunk list）

逐条 journal entry 链式 compose 归并。`utH_normalize` 逐条 compose，每轮失败时释放旧累积结果 + 新部分结果。

### 5.2 取逆

`inv(pa, ca, pdel, cins) = (ca, pa, cins, pdel)`。

### 5.3 compose（A: X→Y ∘ B: Y→Z → X→Z）

双指针在 Y 坐标归并，三分支：
- A 在前（`A.ca+cins < B.pa`）→ emit A
- B 在前（`B.pa+pdel < A.ca`）→ emit B
- 相交 → emitcross

`ut_Merge` 跟踪 `xoff = ΣΔA`、`zoff = ΣΔB` 累计偏移。

### 5.4 diff

1. 解析 freshvid 哨兵
2. 如有 fresh，先 normalize journal → fresh 段
3. LCA = ut_ancestor（depth 双指针硬算，跨树返 NULL）
4. 四阶段 compose
5. 结果存入 S->scratch

### 5.5 deltree 非递归释放

迭代指针遍历（从叶子向上 swap-last 删除），非递归、零额外分配。

## 六、内部命名体系

```bash
grep '^static' undotree.h
```

| 前缀 | 职责         | 代表函数                             |
| ---- | ------------ | ------------------------------------ |
| `N`  | Node/树管理  | `utN_alloc`, `utN_freechildren`     |
| `H`  | Hunk 代数    | `utH_compose`, `utH_mergewalk`      |
| `V`  | Vec 动态数组 | `utV_resize_`, `utV_grow_`（私有宏） |
| `S`  | State/分配器 | `utS_defallocf`                      |
| `P`  | Pool 节点池  | `utP_init`, `utP_alloc`              |
| `M`  | Merge 上下文 | `ut_Merge`                           |
| `D`  | Diff 上下文  | `ut_DX`, `utD_calc`                  |

## 七、错误路径

- `ut_record`：journal 扩容失败 → UT_ERRMEM，无副作用
- `ut_commit`：规范化失败 → NULL，journal 保留、树不变，可重试
- `ut_diff`：失败 → UT_ERRMEM，scratch 不变无副作用
- `ut_switch`：拒绝 freshvid（UT_ERRPARAM），防意外切换至未提交状态

## 八、测试

```bash
just ut           # 编译运行 ut_test (ASAN+UBSAN)
just ut <prefix>  # 运行名称以 prefix 开头的测试
just ut-cov       # 覆盖率
just ut-lines     # 未覆盖行源码
```

79 个测试。

## 九、相关文档

| 文档                          | 内容                   |
| ----------------------------- | ---------------------- |
| `notes/design_undotree.md`    | 架构设计               |
| `docs/undotree.md`            | API 参考手册           |
| `notes/brief_piecetab.md`     | 姊妹库 piecetab 总览   |
| `notes/brief_linecache.md`    | 姊妹库 linecache 总览  |

## 十、边界行为速查表（供绑定层胶水参考）

### 10.1 零参数 / NULL / 空 journal

| 函数 | 零/NULL 行为 |
|------|-------------|
| `ut_record(T, off, 0, 0)` | 返回 UT_OK，**不记录 entry**（no-op 不产生 journal） |
| `ut_unrecord(T, 0)` | 安全 no-op（len - 0 = len） |
| `ut_freshcount(T)` | T==NULL 返回 0 |
| `ut_discard(T)` | T==NULL→ERRPARAM；否则清除 journal（len=0，不释放内存） |
| `ut_commit(T, p)` | journal 空也**创建新节点**（快照节点）；失败返回 NULL |
| `ut_switch(T, v)` | v==NULL→ERRPARAM；v==freshvid→ERRPARAM；journal 非空→ERRPARAM；v==T->current 允许（no-op） |
| `ut_diff(T, from, to)` | from/to 可是 freshvid 哨兵；返回 UT_OK/ERRPARAM/ERRMEM；hunk 数通过 ut_hunks 获取 |
| `ut_freshdiff(T, i, j)` | [i,j) 半开区间；i==j→空 diff；i>j→取逆 |
| `ut_hunks(T, &nh)` | T==NULL 返回 NULL；返回内部指针（下次 diff 覆盖）；diffhn<0 时返回 current->h |
| `ut_deltree(S, T)` | T==NULL 直接返回；非递归释放所有节点+payload（通过 cleaner） |
| `ut_setcleaner(S, f, ud)` | S==NULL 不操作 |
| `ut_close(S)` | S==NULL 直接返回；**不调用 ut_deltree**（调用者责任） |

### 10.2 关键语义

- **ut_record 的 no-op 过滤**：del=0 && ins=0 时不产生条目。绑定层不需要先判断
  `if (del > 0 || len > 0)`。
- **ut_switch 在 journal 非空时拒绝**。undo 丢弃草稿必须先 ut_discard。
- **freshvid 哨兵**：`ut_freshvid(S) = (ut_Vid)(S)`，在 ut_diff 中表"当前+journal"，
  ut_switch 拒绝之。
- **ut_hunks 返回内部指针**：life 到下次 diff/freshdiff 调用。调用者应立即消费。
- **payload cleaner**：签名 `void (*)(void *ud, ut_Payload *p)`，ud 来自 ut_setcleaner。
- **ut_deltree 释放 payload**（通过 cleaner），再释放 journal 和 T 自身。
- **ut_close 不调 ut_deltree**：调用者必须先 deltree 再 close，否则 payload 泄露。
