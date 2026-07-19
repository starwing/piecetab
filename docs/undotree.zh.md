# undotree.h API 参考

[English](undotree.md) | **中文**

> 单头文件 C89 库，提供基于 `(off, del, ins)` 区间代数的版本图 + 编辑日志 +
> 差分服务。前缀 `ut_`。骑在 `pt_Buffer` COW 快照之上（payload 携带任意版本的
> 完整文本）；**非** COW 的替代品。

---

## 一、数据类型

### 错误码

| 宏            | 值  | 含义           |
| ------------- | --- | -------------- |
| `UT_OK`       | 0   | 成功           |
| `UT_ERRPARAM` | -1  | 空指针或参数错 |
| `UT_ERRMEM`   | -2  | 内存分配失败   |

### ut_Alloc — 分配器

```c
typedef void *ut_Alloc(void *ud, void *p, size_t osize, size_t nsize);
```

realloc 语义。`p=NULL, osize=0` 时分配新块；`nsize=0` 时释放 `p`。
默认 `utS_defallocf` 封装 `realloc`，失败则 `abort()`。

### ut_State — 内存上下文

```c
typedef struct ut_State ut_State;
```

持有分配器回调 + 用户数据、可选的 `ut_Cleaner` 回调、以及 `ut_diff`/
`ut_hunks` 共用的 `scratch` 缓冲区。

### ut_Cleaner — Payload 释放回调

```c
typedef void ut_Cleaner(void *ud, ut_Payload *p);
```

`ut_deltree`（及 `utN_freechildren`）释放每个节点前调用此回调处理其 payload。
通过 `ut_setcleaner` 注册。

### ut_Payload — 不透明版本快照

```c
typedef struct ut_Payload ut_Payload;
```

不透明指针。调用方将快照（如 `pt_Buffer`）cast 存入节点 payload。
undotree 不解释其内容。

### ut_Vid — 版本句柄

```c
typedef const struct ut_Node *ut_Vid;
```

版本节点的不透明指针。地址在树生命周期内稳定（无修剪）。传递给
`ut_diff`、导航宏、`ut_switch`。

### ut_Hunk — 变更段

```c
typedef struct ut_Hunk {
    size_t pa;   /* parent: 删除起始偏移            */
    size_t ca;   /* child:  插入起始偏移            */
    size_t pdel; /* parent: 删除字节数               */
    size_t cins; /* child:  插入字节数               */
} ut_Hunk;
```

语义（parent → child）："parent 的 `[pa, pa+pdel)` 被替换为 child 的
`[ca, ca+cins)`"。`pa`/`pdel` 在 parent 侧坐标系，`ca`/`cins` 在 child 侧。

### ut_Entry — Journal 条目

```c
typedef struct { size_t off, del, ins; } ut_Entry;
```

commit 前的单次编辑：在偏移 `off` 处删除 `del` 字节、插入 `ins` 字节。

---

## 二、生命周期 API

### ut_open

```c
UT_API ut_State *ut_open(ut_Alloc *allocf, void *ud);
```

创建 undotree 状态。传 `NULL` 使用默认分配器。OOM 返回 `NULL`。

### ut_close

```c
UT_API void ut_close(ut_State *S);
```

关闭状态。释放 `scratch`。`S=NULL` 无操作。**不会**释放树——请先调
`ut_deltree`。

### ut_setcleaner

```c
UT_API void ut_setcleaner(ut_State *S, ut_Cleaner *f, void *ud);
```

注册 payload 释放回调。节点销毁时（`ut_deltree`）调用。
`S=NULL` 无操作。

---

## 三、树生命周期

### ut_newtree

```c
UT_API ut_Tree *ut_newtree(ut_State *S, ut_Payload *pl);
```

创建版本树。`pl` 为根节点 payload。`S` 为 NULL 或 OOM 返回 `NULL`。

### ut_deltree

```c
UT_API void ut_deltree(ut_State *S, ut_Tree *T);
```

销毁树及其所有节点。每个节点的 payload 经 `cleaner` 回调。
使用迭代指针遍历——非递归、零额外分配。`T=NULL` 无操作。

---

## 四、Journal API

编辑以 `(off, del, ins)` 三元组记录，commit 前暂存于 journal。

### ut_record

```c
UT_API int ut_record(ut_Tree *T, size_t off, size_t del, size_t ins);
```

追加一条编辑到 journal。失败时无副作用。返回 `UT_OK` 或 `UT_ERRMEM`。

### ut_unrecord

```c
UT_API void ut_unrecord(ut_Tree *T, unsigned n);
```

弹出最近 n 条 journal 条目。n == 0 或 journal 为空时无操作。n 超出 journal 长度时清空所有条目。`T` 为 NULL 无操作。

### ut_freshcount

```c
#define ut_freshcount(T) ((T) ? (int)utV_len((T)->journal) : 0)
```

未提交 journal 条目数。

### ut_discard

```c
UT_API int ut_discard(ut_Tree *T);
```

丢弃所有未提交 journal 条目（计数归零）。同时将 `diffhn` 复位为 -1
（使待命 diff 失效）。

---

## 五、版本管理 API

### ut_commit

```c
UT_API ut_Vid ut_commit(ut_Tree *T, ut_Payload *pl);
```

提交当前 journal：规范化为 hunk 列表，创建新节点并挂到 `current` 下，
清空 journal，将 `current` 设为新节点。`pl` 为新节点的 payload
（例如编辑后的 `pt_Buffer`）。

返回新 `ut_Vid`，失败返回 `NULL`（OOM — journal 保留、树不变、可重试）。

无 journal 条目时仍创建节点（空 hunk 列表）。

### ut_switch

```c
UT_API int ut_switch(ut_Tree *T, ut_Vid v);
```

将 `current` 移至另一版本节点。`ut_freshvid(S)` 会被显式拒绝
（`UT_ERRPARAM`）——请先调 `ut_discard`。journal 非空时也返回
`UT_ERRPARAM`。

---

## 六、导航

所有宏对 NULL 输入安全。

| 宏 / 函数                               | 返回值          |
| --------------------------------------- | --------------- |
| `ut_root(T)`                            | `&T->root`      |
| `ut_current(T)`                         | `T->current`    |
| `ut_parent(v)`                          | `v->parent`     |
| `ut_payload(v)`                         | `v->payload`    |
| `ut_childcount(v)`                      | `int`: 子节点数 |
| `ut_firstchild(v)`                      | 最老子节点      |
| `ut_lastchild(v)`                       | 最幼子节点      |
| `ut_nextsib(c)`                         | 下个更幼兄弟    |
| `ut_younger(v)`                         | 时间序下个节点  |
| `ut_older(v)`                           | 时间序上个节点  |

### ut_ancestor

```c
UT_API ut_Vid ut_ancestor(ut_Vid a, ut_Vid b);
```

最近公共祖先。depth 对齐后双指针向上追赶。跨树或 NULL 输入返回 `NULL`。

---

## 七、Diff API

### ut_freshvid

```c
#define ut_freshvid(S) ((ut_Vid)(S))
```

哨兵，表示未提交的 journal 状态。作为 `from`/`to` 传给 `ut_diff` 时，
以 `T->current` 替换该端点并前插/追加 journal 内容。

### ut_diff

```c
UT_API int ut_diff(ut_Tree *T, ut_Vid from, ut_Vid to);
```

计算两版本间差分。`from` 和 `to` 均可使用 `ut_freshvid(S)` 代表未提交状态。

返回 hunk 数（≥0）或负错误码。结果存入 `S->scratch`，下次同一 State 上的
`ut_diff` 之前有效。

内部四阶段 compose：
`[inv(journal)] + from→LCA⁻¹ + LCA→to + [journal]`

### ut_freshdiff

```c
UT_API int ut_freshdiff(ut_Tree *T, int i, int j);
```

计算 journal [i, j) 区间内编辑的差分。`i`/`j` 为 journal 快照索引：
`fresh(i)` = 应用了 `journal[0..i)` 后的状态。`i < j` 时正向计算推进
diff；`i > j` 时反向计算回退 diff（对 `journal[j..i)` 归一化后取逆）。
`i`/`j` 超出 `[0, journal_len]` 时自动 clamp。

返回 hunk 数（≥0）或负错误码。结果存入 `S->scratch`，可通过 `ut_hunks`
获取。与 `ut_diff` 共用 scratch。

### ut_hunks

```c
UT_API const ut_Hunk *ut_hunks(ut_Tree *T, size_t *pn);
```

获取当前 hunk 列表。若存在待命 diff（`diffhn ≥ 0`），返回 `S->scratch`；
否则返回 `current->h`（当前节点到 parent 的已提交 changeset）。

hunk 数写入 `*pn`（`pn` 为 NULL 时略过写入）。`T` 为 NULL 返回 `NULL`。

---

## 八、集成指引

### 编辑流程

```
ut_record(T, off, del, ins)    -- 记录编辑（可失败，无副作用）
pt_splice(...)                 -- 在 buffer 上执行编辑
ut_commit(T, new_buffer)       -- 冻结为版本节点
```

`pt_splice` 失败时：`ut_unrecord(T, 1)` 弹出失败条目。

### 消费者流程（如 linecache）

```
ut_diff(T, last_applied_vid, ut_freshvid(S))  -- 获取待处理变更
h = ut_hunks(T, &hn);
for (i = hn-1; i >= 0; i--)                   -- 从右到左 apply
    lc_splice(cache, h[i].pa, h[i].pdel, h[i].cins);
```

---

## 九、配置

| 宏              | 默认  | 含义                 |
| --------------- | ----- | -------------------- |
| `UT_PAGE_SIZE`  | 65536 | 池分配器页大小       |

`UT_STATIC_API`（三库共用）将 API 函数设为 static。
undotree 使用动态 vec，无扇出或容量宏。

---

## 十、许可证

[MIT](../LICENSE)，与 Lua 相同。
