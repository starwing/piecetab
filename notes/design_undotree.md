# undotree.h 设计（独立单头库）

> 定位：**版本图 + 编辑日志 + 差分服务**，库族第三成员（linecache、
> piecetab 之后），前缀 `ut_`。骑在 pt_Buffer COW 之上（payload 提供
> 任意版本完整快照），**非** COW 的替代品。
> 状态：**已实现，重构中（Vec→环状单链表 + mempool）**。

## 一、定位与边界

- **零依赖**：不碰文本、不依赖 piecetab/linecache。只管
  `(off, del, ins)` 记录与区间代数。
- **单头文件 stb 风格**：`UT_NS_BEGIN/END`、`UT_STATIC_API`/
  `UT_IMPLEMENTATION`、`UT_API` 导出宏。C89 + `-pedantic`。
- **payload = 不透明指针**：`typedef struct ut_Payload ut_Payload;`
  调用方 cast 存入（绑定层放 retained pt_Buffer）。释放经
  `ut_Cleaner` 回调，undotree 不解释 payload。
- **消费者模型**：消费者自持 `ut_Vid`，按需调 `ut_diff` 拉
  差分自行 apply。fresh 状态通过 `ut_freshvid(S)` 哨兵表达。
- **hunk ≅ tree-sitter TSInputEdit**（start/old_end/new_end 三坐标
  = pa/pdel/cins 的另一写法）。

```
pt_Buffer COW        任意 vid 完整快照（文本真相源）
      ↓ payload
undotree.h           版本图 + journal + 规范化 + compose
                     ut_diff(from, to) → hunk list
      ↓                     ↓
lc_Cache 消费者       SpanTree 消费者（未来）
forward 直插          删旧 span + 标脏重解析
```

## 二、数据结构

### ut_State

```c
struct ut_State {
    ut_Node     base;       /* must be first; sentinel for ut_freshvid   */
    ut_Alloc   *allocf;     /* lua_Alloc-style realloc                    */
    void       *ud;         /* userdata for allocf/cleaner                */
    ut_Pool     pool;       /* node object pool (from linecache mempool)  */
    ut_Cleaner *cleaner;    /* payload release callback                   */
    void       *cud;        /* userdata for cleaner                       */
    ut_Hunk    *scratch;    /* vec: diff/compose temp buffer              */
};
```

`base` 字段必须是第一个——`ut_freshvid(S)` = `(ut_Vid)(S)`，首字段地址
即为 `ut_Node*`，用作 fresh 哨兵。`depth=0, parent=NULL, last_child=NULL`
使所有导航 API 对哨兵自然安全。

### ut_Pool — Node / Tree 内存池

从 `linecache.h` 引入的池分配器（`lc_Pool` 的独立副本，前缀 `utP_`），
同时池化 `ut_Node` 和 `ut_Tree`（照抄 linecache/piecetab 的树也 mempool 模式）。
页式分配 + freelist 回收，配合环状单链表结构达成：

- **Node/Tree 地址终身稳定**（无 realloc），payload（`pt_Buffer`）引用安全
- **deletree 释放的对象回 freelist**，下个 commit 直接复用，避免反复
  `malloc`/`free`
- **极简设计**：不引入 `utP_reserve`、`freed_obj` 计数器（比 linecache 更简单）。
  分配时 freelist 非空则 pop，否则新申请 page。OOM 仅发生于 page 分配。
- **测试 OOM**：用 `ut_drainpool`/`ut_refillpool` 清空 freelist，再用 `oom_alloc`
  使 page 分配失败

```c
/* pool 结构（无 freed_obj 计数器，无 reserve） */
typedef struct { size_t obj_size; void *freed; void *pages; } ut_Pool;
#define UT_PAGE_SIZE 65536
```

> 为什么之前 Vec 时代不用 mempool？Vec nodes 用 swap-last 释放，children
> 指针频繁变化。环状单链表的 node 指针稳定，天然适合池化。

### ut_Tree

```c
struct ut_Tree {
    ut_Node   root;       /* root node (embedded, 不独立分配)      */
    ut_State *S;          /* owning state                           */
    ut_Node  *current;    /* current head node                      */
    ut_Entry *journal;    /* vec of uncommitted edits               */
    int       diffhn;     /* pending diff hunk count, -1 = none     */
};
```

root 嵌入 Tree 体内（不独立分配），`ut_root(T)` = `&T->root`。
journal 是 `ut_Entry` 的 vec（header 前置），每条 `{ off, del, ins }`。

### ut_Node

```c
struct ut_Node {
    ut_Node    *parent;      /* parent node (NULL at root)              */
    ut_Node    *last_child;  /* ring: youngest (last-committed) child   */
    ut_Node    *next_sib;    /* ring: → older sibling (oldest wraps to   */
                             /*        youngest, forming a cycle)        */
    ut_Payload *payload;     /* caller snapshot (e.g. pt_Buffer)        */
    ut_Hunk    *h;           /* vec: parent→this changeset              */
    int         depth;       /* distance from root                       */
    int         child_count; /* number of children (O(1) for macro)      */
};
```

**环状单链表**：`last_child` 指向最年轻子节点（最后 commit 的）。
`next_sib` 沿**更老**方向（youngest → middle → oldest），
最老节点的 `next_sib` 回绕到 youngest 形成闭环。

插新子节点（O(1) tail-insert）：
```
新节点->next_sib = parent->last_child->next_sib;  /* 继承旧 first_child */
parent->last_child->next_sib = 新节点;             /* 旧 youngest → 新    */
parent->last_child = 新节点;                       /* 更新 youngest 指针  */
```

结果 invariant：`parent->last_child->next_sib == oldest_child` 始终成立。

**内存对比**（单深树 A→B→C→D，4 节点）：

| 方案 | Node 大小 | 额外开销 | 总内存 |
|------|----------|---------|--------|
| Vec  | 40B + children vec(24B/node×3) + menvec overhead | vec header = 16B×3 | ~220B |
| 环链 | 48B × 4 | 无 | 192B |

省 ~13%，源于免除 vec header(16B) 和 capacity 冗余。

### ut_Hunk / ut_Entry

```c
struct ut_Hunk  { size_t pa, ca, pdel, cins; }; /* 四元组: M→N 差分 */
struct ut_Entry { size_t off, del, ins;       }; /* journal 条目      */
```

语义：M 的 `[pa, pa+pdel)` 被替换为 N 的 `[ca, ca+cins)`。pa/pdel 在 M 侧坐标系，ca/cins 在 N 侧。

### Vec 基础设施（仅用于 hunk 和 journal）

```c
typedef struct { unsigned len, cap; } utV_Header;

#define utV_init(A)     ((A) = NULL)
#define utV_len(A)      ((A) ? utV_hdr(A)->len : 0u)
#define utV_free(S, A)  ((A) && utV_resize_(S, (void**)&(A), 0, sizeof(*(A))))
#define utV_push(S, A, V)  /* 返回 UT_OK/UT_ERRMEM */
```

header 前置：`| utV_Header[len,cap] | elem[0] ... |`。A 指向 `elem[0]`，
`utV_hdr(A)` = `(utV_Header*)(A) - 1`。

> Vec 仅用于 journal 和 hunk 列表（这两个天然是线性序列）。
> Node 的子节点管理改用环状单链表。

### ut_Vid — 版本句柄

`typedef const ut_Node *ut_Vid` 不透明指针。地址终身稳定（无 realloc、无修剪），
终身有效至 deltree。整数 vid↔ut_Vid 映射归绑定层自建。

### fresh 哨兵

```c
#define ut_freshvid(S) ((ut_Vid)(S))
```

## 三、fresh 位点协议

| from | to | 场景 |
|---|---|---|
| commit | fresh | 消费者追平草稿 |
| fresh | commit | undo 回退 |
| commit | commit | 正常 diff |
| fresh | fresh | 恒等 |

`ut_diff(T, from, to)` 两个参数均为 `ut_Vid`。内部四阶段 compose：
`[inv(fresh)] + from→LCA⁻¹ + LCA→to + [fresh]`。

`ut_switch` 遇 fresh 拒绝（UT_ERRPARAM），强制调用方先 `ut_discard`。

## 四、API 面

```c
typedef void *ut_Alloc(void *ud, void *p, size_t osize, size_t nsize);
typedef void  ut_Cleaner(void *ud, ut_Payload *p);
typedef struct ut_State   ut_State;
typedef struct ut_Tree    ut_Tree;
typedef const struct ut_Node *ut_Vid;
typedef struct ut_Payload ut_Payload;
typedef struct ut_Hunk { size_t pa, ca, pdel, cins; } ut_Hunk;

#define UT_OK        (0)
#define UT_ERRPARAM  (-1)
#define UT_ERRMEM    (-2)

/* state lifecycle */
UT_API ut_State *ut_open(ut_Alloc *allocf, void *ud);
UT_API void      ut_close(ut_State *S);
UT_API void      ut_setcleaner(ut_State *S, ut_Cleaner *f, void *ud);

/* tree lifecycle */
UT_API ut_Tree *ut_newtree(ut_State *S, ut_Payload *pl);
UT_API void     ut_deltree(ut_State *S, ut_Tree *T);

/* journal */
#define ut_freshcount(T)  ((T) ? (int)utV_len((T)->journal) : 0)
UT_API int    ut_record(ut_Tree *T, size_t off, size_t del, size_t ins);
UT_API void   ut_unrecord(ut_Tree *T, unsigned n);

/* versioning */
UT_API ut_Vid ut_commit(ut_Tree *T, ut_Payload *p);
UT_API int    ut_discard(ut_Tree *T);
UT_API int    ut_switch(ut_Tree *T, ut_Vid v);

/* navigate (macros) */
#define ut_parent(v)      ((v) ? (v)->parent : NULL)
#define ut_payload(v)     ((v) ? (v)->payload : NULL)
#define ut_childcount(v)  ((v) ? (int)(v)->child_count : 0)
#define ut_firstchild(v)  ((v) && (v)->last_child ? (v)->last_child->next_sib : NULL)
#define ut_lastchild(v)   ((v) ? (v)->last_child : NULL)
#define ut_nextsib(c)     ((c) ? (c)->next_sib : NULL)
#define ut_root(T)        ((T) ? &(T)->root : NULL)
#define ut_current(T)     ((T) ? (T)->current : NULL)
UT_API ut_Vid ut_ancestor(ut_Vid a, ut_Vid b);

/* time-walk (for :earlier / :later undo-branch navigation) */
UT_API ut_Vid ut_younger(ut_Vid v);
UT_API ut_Vid ut_older(ut_Vid v);

/* diff */
#define ut_freshvid(S)    ((ut_Vid)(S))
UT_API int               ut_diff(ut_Tree *T, ut_Vid from, ut_Vid to);
UT_API int               ut_freshdiff(ut_Tree *T, int i, int j);
UT_API const ut_Hunk    *ut_hunks(ut_Tree *T, size_t *pn);
```

### ut_younger / ut_older（:earlier / :later 导航）

见[十、`ut_younger` / `ut_older` 时间序遍历算法](#十ut_younger--ut_older-时间序遍历算法)。
兄弟数极少（单深树=1，分支=2-3），找 prev-sibling 沿环遍历 O(k) 实际是 O(1)。
`:earlier`/`:later` 是用户手动触发，非热路径，不加 `prev_sib` 指针。

## 五、核心算法

### 5.1 规范化（journal → hunk list）

逐条 journal entry `{ off, del, ins }` 转为单一 hunk，链式 compose
归并。`utH_normalize(T, out, s, e)` 对 `T->journal[s..e)` 子范围逐条合并。

### 5.2 取逆

`inv(pa, ca, pdel, cins) = (ca, pa, cins, pdel)`。分配新数组逐条交换。

### 5.3 compose（A: X→Y ∘ B: Y→Z → X→Z）

A 的 ca/cins 与 B 的 pa/pdel 同在 Y 坐标系。双指针在 Y 坐标归并：

```
ut_Merge {
    x2y (A), y2z (B):    输入 hunk vec
    out:                 输出 hunk vec（X→Z）
    xoff = ΣΔA(seen):    X→Y 累计偏移
    zoff = ΣΔB(seen):    Y→Z 累计偏移
}
```

三分支：

| 条件 | 操作 | 说明 |
|---|---|---|
| A_i.ca+cins < B_j.pa | `utH_emitX2Y` | A 在前：ca += zoff（Y→Z 映射） |
| B_j.pa+pdel < A_i.ca | `utH_emitY2Z` | B 在前：pa -= xoff（Y→X 映射） |
| 相交 | `utH_emitcross` | 合并：`surv = A.cins - B.pdel`（幸存/溢出） |
| 零效果过滤 | 跳过 | `emitcross`/`emitX2Y`/`emitY2Z` 内 `assert(pdel||cins)` 防御 |

### 5.4 diff

```
ut_diff(from, to):
  1. 解析 freshvid：from/to 各可能为哨兵
  2. 如有 fresh，先 normalize journal → fresh 段
  3. LCA = ut_ancestor
  4. 四阶段 compose：
     [inv(fresh)] + from→LCA 各段取逆 + LCA→to 各段原样 + [fresh]
  5. 结果写入 S->scratch，返回 hunks 数
```

### 5.5 deltree 非递归释放

沿环状单链表释放：从 root 下钻到最叶子，逐节点释放 children 环 +
hunk vec，回 freelist，上溯父节点。

## 六、消费者指引

lc_Cache 消费者的 forward 单遍直插：对每 hunk 从右到左处理，
读源恒 = 迁移终点 payload。spantree 消费者（未来）：TSInputEdit 同构。

## 七、错误路径

- `ut_record`：journal 扩容失败 → UT_ERRMEM，无副作用。
- `ut_commit`：规范化/节点池分配失败 → NULL，journal 保留、树不变，可重试。
  — 节点直接从 pool 分配，freelist 非空则 O(1) pop，否则触发 page 分配。
- `ut_diff`：失败 → UT_ERRMEM，scratch 不变无副作用。
- `ut_deltree`：迭代沿环释放，非递归、零额外分配。每节点 payload 过 cleaner。

## 八、内部命名分类码

| 码 | 含义 | 示例 |
|---|---|---|
| N | Node/树管理 | `utN_alloc`, `utN_freechildren` |
| H | Hunk 代数 | `utH_compose`, `utH_emitX2Y` |
| V | Vec 数组（hunk/journal） | `utV_push`, `utV_free` |
| S | State/分配器 | `utS_defallocf` |
| P | Pool 节点池 | `utP_alloc`, `utP_init` |
| M | Merge 上下文 | `ut_Merge`, `utH_mergewalk` |
| D | Diff 上下文 | `ut_DX`, `utD_calc` |

> 尾部下划线（如 `utV_resize_`、`utV_grow_`）表示私有函数：仅通过宏
> (`utV_free`/`utV_reserve`) 间接调用，禁止直接使用。

## 九、已决项

- ut_Vid = const ut_Node* 不透明指针
- ut_freshvid(S) = (ut_Vid)(S) 哨兵，无 ut_Pos.k
- ut_switch 遇 fresh 拒绝（UT_ERRPARAM）
- LCA depth 双指针硬算，跨树自然返 NULL
- 不做修剪（pt_compact 丢历史 = newtree 重开）
- **Node 子节点用环状单链表**（`last_child` + `next_sib` + `child_count`）
  — 符合单深树场景，免 vec header 开销，commit O(1) tail-insert
- **mempool 池化 node 和 tree 分配**（从 linecache.h 搬 `lc_Pool`，前缀 `utP_`）
  — 节点/Tree 地址终身稳定，释放回 freelist 复用
  — 极简设计：无 `utP_reserve`，无 `freed_obj` 计数器。OOM 仅发生于缺 page 时
  — 测试 OOM：`ut_drainpool`/`ut_refillpool` + `oom_alloc`
- **ut_younger/ut_older** 前序遍历算法（函数，非宏）— 见第十节
  — 兄弟数极少，不加 `prev_sib` 指针
- **移除 `ut_child(v, i)` 按索引访问**，改用 `ut_firstchild`/`ut_nextsib` + `child_count` 遍历
- 树序列化（持久 undo）远期考虑

## 十、`ut_younger` / `ut_older` 时间序遍历算法

undotree 时间是**前序**（pre-order），子节点按 commit 先后（oldest → youngest）。
环状链表 `next_sib` 指向更老的兄弟：

```
parent.last_child → [youngest=N] → [N-1] → ... → [1=oldest] ─┐
                    ↑                                          │
                    └──────────────────────────────────────────┘
```

定义：
- `oldest_child(p)` = `p->last_child->next_sib`（若 p 有子节点）
- `youngest_child(p)` = `p->last_child`
- `prev_sib(v)` = 沿环找到 `s->next_sib == v` 的那个 `s`（即更年轻的兄弟，O(k)，k=兄弟数 ≤ 一般 2-3）
- `older_sib(v)` = `v->next_sib`（除非 v 是 oldest，此时 next_sib 回到 youngest）

### `ut_younger(v)` — 时间序后继（Vim `:later`）

```
1. if v has children: return oldest_child(v)
2. // v is a leaf
   while v->parent != NULL:
     if v != v->parent->last_child:  // v has younger siblings
       return prev_sib(v)            // the immediately younger sibling
     v = v->parent                   // v was youngest, ascend
   return NULL                       // last node in time order
```

### `ut_older(v)` — 时间序前驱（Vim `:earlier`）

```
1. if v->parent == NULL: return NULL            // root, no predecessor
2. if v == oldest_child(v->parent):             // v is the oldest sibling
     return v->parent
3. // v has older siblings
   bro = v->next_sib                            // older sibling
   // drill down to the deepest youngest leaf
   while (bro->last_child != NULL):
     bro = bro->last_child
   return bro
```

### 示例验证

**单深链 root → c1 → c2 → c3**（仅1个孩子每层）
时间序：root, c1, c2, c3

| v | ut_younger(v) | ut_older(v) |
|---|---|---|
| root | c1（仅子节点） | NULL |
| c1 | c2 | root |
| c2 | c3 | c1 |
| c3 | NULL | c2 |

**分支 root → c1(oldest), c2(youngest)**（无孙）
时间序：root, c1, c2

| v | ut_younger(v) | ut_older(v) |
|---|---|---|
| root | c1（oldest child） | NULL |
| c1 | c2（prev_sib = last_child） | root（oldest sib→parent） |
| c2 | NULL | c1（older_sib） |

**分支 root → c1(oldest,有子gc), c2(middle), c3(youngest)**
时间序：root, c1, gc, c2, c3

| v | ut_younger(v) | ut_older(v) |
|---|---|---|
| root | c1 | NULL |
| c1 | gc（oldest_child） | root（oldest sib→parent） |
| gc | c2（leaf, 上溯到c1, prev_sib(c1)=c2） | c1 |
| c2 | c3（prev_sib） | gc（older_sib=c1, 下钻到gc） |
| c3 | NULL | c2 |

## 十一、测试

`tests/ut_test.c`（79 个测试）：

覆盖生命周期、journal、commit、switch/discard、branch、LCA、深链、
OOM 路径、normalize merge/overlap/no-op、compose 各分支、diff 各种组合、
zero entry 过滤、cleaner 非 NULL、mergewalk 三路 + tail loop 等。

运行：`just ut` / `just ut-cov`

目标：100% 行覆盖 / 100% 函数覆盖 / ≥90% 分支覆盖。
