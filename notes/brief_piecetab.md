# piecetab.h 项目总览

> 供 Agent 快速了解项目全貌。可 grep 查询的信息不载此文，提供查询命令。
> 设计原始文档 `notes/design_piecetab_v2.md` 部分命名已漂移，**以本文与代码为准**（见 §十一）。

## 一、项目概要

单头文件 C89 库，前缀 `pt_`。以 B+ 树维护纯字节级 piece table，
支持 COW 快照（Buffer）、可变编辑态（hole 叶）、事务 commit/rollback。

**功用**: 文本编辑器 buffer 核心。行/字符映射完全外置给 `linecache.h`（前缀 `lc_`），
piecetab 不管行、不管编码——clean octet。两库独立，用户自行组合（详见
`notes/design_piecetab_v2.md` §八）。

```bash
# 查行数
wc -l piecetab.h

# 查测试用例数
grep -c 'X(' tests/pt_test4.c
```

编译参数（测试用）: `-DPT_FANOUT=4 -DPT_PAGE_SIZE=512 -DPT_MAX_HOLESIZE=16`
以极小扇出迫树分裂。

## 二、数据结构

### 叶 = piece（两种，无独立结构体槽位）

- **literal**: 不具所有权的裸 `char*`，指向用户内存或 arena 中冻结数据。零额外开销。
- **hole**: 编辑态可变叶，池化定容缓存。长度记于父 `bytes[i]`，自身无长度字段：

```c
typedef struct pt_Hole { char data[PT_MAX_HOLESIZE]; } pt_Hole;
```

### pt_Node — 内节点（兼叶容器）

```c
typedef struct pt_Node {
    struct pt_Node *children[PT_FANOUT]; /* 子节点 或 叶数据指针 */
    size_t          bytes[PT_FANOUT];    /* 内层=子树累计, 叶层=piece长 */
    pt_Mask         mask;                /* 位图: 叶层=是hole, 内层=子树含hole */
    pt_Ver          version;             /* COW 版本 vs tree root */
    unsigned short  child_count;
} pt_Node;
```

- `pt_Mask` = `size_t` 单字位图（非数组），故 `PT_FANOUT <= PT_MASK_BITS`（静态断言）
- 叶容器（levels 层的父）children[i] 实为 `char*` 或 `pt_Hole*`，由 mask 位辨识

### pt_Tree — 树（Buffer 即 `const pt_Tree *`）

```c
typedef struct pt_Tree {
    pt_Node         root;   /* 嵌入（非指针） */
    pt_State       *S;
    struct pt_Tree *from;   /* fork 来源, COW 生命周期链, 终止于 S->empty */
    pt_Arena        arena;  /* 本树 literal 数据 arena（惰性） */
    size_t          bytes;  /* 总字节 O(1) */
    unsigned        refc;
    unsigned short  levels; /* 0 = root 即叶容器 */
} pt_Tree;
```

### pt_Cursor — 游标（导航 + 编辑，公共头暴露）

```c
struct pt_Cursor {
    struct pt_Node **paths[PT_MAX_LEVEL]; /* 根→叶路径槽位指针 */
    struct pt_Tree  *tree;
    size_t           poff;  /* piece 内偏移 */
    size_t           off;   /* 当前 piece 之前累计字节 */
    int              dirty; /* 编辑态(transient)标志 */
};
```

- `paths[l]` 恒指向 `ptK_parent(C, l)->children` 某槽；`paths[levels]` 指叶槽
- 绝对偏移 = `pt_offset(C)` = off + poff
- 由 `pt_seek` 构造（清 dirty）；`pt_locate`/`pt_advance` 保留 dirty

### 层级模型（易误解，必读）

```
levels=2 的树: 层号 -1=root(嵌入), 0..levels-1=内节点, levels 槽位=叶(piece)
ptK_parent(C, levels) = 叶容器(pt_Node)，其 children[] 直接存数据指针
```

### pt_State / pt_Pool / pt_Arena

- `pt_State`: allocf + 三池（nodes/holes/trees）+ `rt[PT_MAX_LEVEL]` 缝合暂存节点
  + `empty` 哨兵树（零分配）+ `max_version` 全局 COW 计数
- `pt_Pool`: 页式池分配器（同 lc_Pool）。`ptP_reserve(n)` 事务预分配防 OOM，
  `ptP_ralloc` 从预留取（`assert(freed)` 保不缺）
- `pt_Arena`: 块链 arena（current=有空位链, full=满块链），存冻结 literal 数据。
  每树独享，随树释放

## 三、关键常量

| 符号              | 默认  | pt_test4 值 | 含义                |
| ----------------- | ----- | ----------- | ------------------- |
| `PT_FANOUT`       | 62    | 4           | 节点最大子数（≤64） |
| `PT_MAX_HOLESIZE` | 64    | 16          | hole 容量（字节）   |
| `PT_MAX_LEVEL`    | 16    | 16          | 最大树深            |
| `PT_PAGE_SIZE`    | 65536 | 512         | 池分配器页大小      |
| `PT_ARENA_SIZE`   | 1024  | 1024        | arena 块最小容量    |

半满阈值 = FANOUT/2。`PT_FANOUT >= 4` 静态断言（makeroom 最多需 2 空槽）。

## 四、核心宏

```c
#define ptK_levels(C)    ((C)->tree->levels)
#define ptK_bytes(C)     ((C)->tree->bytes)
#define ptK_parent(C, l) ((l) > 0 ? *(C)->paths[(l) - 1] : &(C)->tree->root)
#define ptK_idx(C, p, l) ((int)((C)->paths[(l)] - (p)->children))
#define ptN_cc(n)        ((int)(n)->child_count)
#define ptN_hole(p, i)   ((pt_Hole *)((p)->children[i]))
#define ptN_lit(p, i)    ((const char *)((p)->children[i]))
#define ptM_mask(n)      (((pt_Mask)1 << (n)) - 1)  /* 低 n 位掩码 */
```

## 五、公共 API

```bash
# 查完整 API 声明
grep '^PT_API' piecetab.h
```

| 类别     | 函数                                                 | 功用                                                                    |
| -------- | ---------------------------------------------------- | ----------------------------------------------------------------------- |
| 生命周期 | `pt_open`, `pt_close`, `pt_reset`, `pt_getallocf`    | 状态管理                                                                |
| Buffer   | `pt_empty`, `pt_from`, `pt_retain`, `pt_release`     | 构造/引用计数。`pt_empty` 返回哨兵零分配                                |
| 查询     | `pt_bytes`, `pt_version`                             | 树级汇总                                                                |
| 定位     | `pt_seek`(构造器,清dirty), `pt_locate`, `pt_advance` | 游标                                                                    |
| 读       | `pt_read`, `pt_piece`, `pt_next`, `pt_prev`          | piece 遍历："移动后返回落脚点"语义                                      |
| 编辑     | `pt_edit`                                            | splice via hole（**copy 语义**，追尾）                                  |
| 编辑     | `pt_insert`/`pt_append`/`pt_splice`/`pt_remove`      | literal **引用语义**（s 须存活至释放）；insert 原位, append/splice 追尾 |
| 事务     | `pt_commit`, `pt_rollback`                           | 见 §八                                                                  |
| arena    | `pt_reserve`, `pt_scratch`, `pt_literal`             | 直写 arena：预留 / 查询写入头 / 消费为 literal                          |

- `pt_edit(C, del, s, len)`: len ≤ PT_MAX_HOLESIZE；命中 hole 尾/首直接 memmove 追加
- `pt_insert` = `pt_append` + `pt_advance(-len)`
- 物理连续 literal 追加自动合并（`ptN_lit(p,i)+bytes[i] == s`）

## 六、内部函数命名体系

```bash
grep '^static' piecetab.h
```

| 前缀   | 全称        | 职责                  | 代表函数                                                                                                                                                                                                              |
| ------ | ----------- | --------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ptK_` | Kurser      | 游标导航 + 编辑态入口 | `findleaf`, `locend`, `forwardoff`, `backwardoff`; `markdirty`, `cow`, `beginedit`（COW 类，归属存疑，见命名议题）                                                                                                    |
| `ptI_` | Insert      | 插入/裂叶             | `onepiece`, `splitroot`, `splitchild`, `fillrt`, `stitchrt`, `splitins`（对应 lc 的 `lcB_`）                                                                                                                          |
| `ptD_` | Delete      | 删除/平衡/缝合        | `trimleft`, `trimright`, `cutrange`, `cutpiece`, `rmleaf`, `rmrange`, `makechain`, `findroom`, `backwardnode`, `balancenode`, `foldnode`, `rebalance`, `mergeleaf`, `stitch`, `stitchnode`, `checkstitch`, `cowpaths` |
| `ptM_` | Mask/Metric | 位图 + 度量传播       | `ishole`, `sethole`, `iterhole`（mask）; `up`（度量+mask 一体向上传播）                                                                                                                                               |
| `ptN_` | Node        | 节点运作              | `sumbytes`, `copy`, `move`, `remove`, `makespace`, `purge`（版本感知递归释放）                                                                                                                                        |
| `ptH_` | Hole        | hole 操作             | `new`, `append`, `remove`（宏 `fit`, `reserve`）                                                                                                                                                                      |
| `ptC_` | Commit      | 冻结                  | `holebytes`, `freezeleaf`, `freeze`                                                                                                                                                                                   |
| `ptA_` | Arena       | arena                 | `alloc`, `destroy`                                                                                                                                                                                                    |
| `ptP_` | Pool        | 池                    | `init`, `destroy`, `alloc`, `ralloc`, `free`, `reserve`                                                                                                                                                               |
| `ptS_` | State       | 状态                  | `defallocf`                                                                                                                                                                                                           |

与 linecache 的差异：插入类用 `I`（lc 用 `B`=Break）；`M` 兼管 mask 与 metric；
新增 `H`/`C`/`A`/`S` 类。

## 七、mask 维护铁律

任何引入/移除 hole 或改变子树 hole 分布的编辑，必须保证 mask 沿 paths 上推。
`ptM_up(C, l, db)` 一体完成：度量差 `db` 累加 + 逐层重算 mask 位
（`db==0` 且 mask 无变化时提前剪枝）。`ptM_up(C, levels, 0)` 惯用法 = 纯 mask 修正。

## 八、COW / 事务模型

- **transient**: 首次编辑时 `ptK_markdirty` fork 新 tree（`version = ++max_version`,
  `from = 旧树` 并 retain），仅 Cursor 持有，外部不可达
- **节点 COW**: `ptK_cow` — `node->version != root->version` 则复制并修 paths；
  `ptK_beginedit` = reserve + markdirty + 沿 paths 全线 cow
- **from 链**: 保证共享节点存活；`pt_release` 归零时 `ptN_purge` 只删
  `version == 本树 version` 的私有节点，再沿 from 链回溯；链终止于 `S->empty` 哨兵
- **commit**: `ptC_holebytes` 汇总 → `pt_reserve` arena → `ptC_freeze` DFS
  （借内层 mask 跳过无 hole 子树，且顺手消耗内层 mask）将 hole 数据冻入 arena、
  换为 literal 指针 → 清 dirty 返回 Buffer
- **rollback**: `from->refc == 1` 则源树必死 → cursor 失效（tree=NULL）；
  否则回到 from。契约：使用者负责持有要回退到的 buffer（design v2 §4.4.1）

## 九、编辑流程概要

### ptI_splitins（pt_edit / pt_append 共用插入路径）

1. `ptI_fillrt` 在 `S->rt[0]` 装配新 piece（+ 原 piece 裂出的右半），
   need = 1 或 2 槽
2. 叶容器有空位 (`m = min(need, 空位)`) 直接 makespace+copy；不足则
   `ptI_stitchrt`: 自底向上寻非满层（全满 `ptI_splitroot` 加深）→
   逐层 `ptI_splitchild` → 缝入剩余槽
3. `ptM_up` 分段传播度量差

### pt_remove

1. 右游标 R = C + advance(len)；`ptD_cowpaths` COW 两路径并求分叉层 `fl`
2. 同叶 → `ptD_rmleaf`（`ptD_cutpiece` 叶内四情形切割；literal 中裂需
   makeroom 式裂层）
3. 跨叶 → `ptD_rmrange`: `trimright(L)`/`trimleft(R)` →
   `ptD_cutrange`（洋葱剥层：L 右侧删除、R 右余存入 `rt[k]`）→
   `ptD_stitch`（`mergeleaf` 试合并断口叶 → `stitchnode` 逐层缝回 rt →
   `rebalance` 收尾，游标落删除点）
4. `rt[k]` 索引恒守 `k = levels - l`（洋葱层铁律，同 lc）
5. **批量插入遗产已删**（lc 的 scan/append 扩树路径 pt 不存在）：
   `ptD_makechain` 不再支持根加深（`from < 0`），`ptD_findroom` 不再
   搬运断层右余——remove-only stitch 语境下二者均不可达，以 assert
   守卫。若未来引入批量插入 API，须从 git 历史/linecache 恢复。

## 十、测试

```bash
just pt               # 编译运行 pt_test4 (FANOUT=4, ASAN+UBSAN)
just pt <prefix>      # 运行名称以 prefix 开头的测试
just pt @<name>       # 只运行首个匹配
just pt-cov           # 覆盖率（lcov）
just pt-lines         # 未覆盖行源码
```

- 测试骨架 `tests/pt_tests.h`：`TESTS(X)` 宏列举 + `PT_TEST_MAIN`
- 覆盖率现状：100% 行 / 100% 函数（`just pt-cov`），分支约 91%
- **树构造 DSL**: `treeV(levels, innerV(leafV(litV("ab"), holeV("cd"))))`
  直接搭树；`editV` 搭树后置 dirty
- **断言工具**: `pt_asserttree(buffer, levels, root)` 全树精确比对（优先用，
  避免松散判定）；`pt_checktree` 不变式校验（cc 半满、bytes 和、mask 一致）；
  `pt_checkcursor(C, off)` 游标不变式
- `oom_alloc(&count)` 注入 OOM；`pt_localfill` 用栈 buffer 填池 freelist
- `pt_dumptree` 打树；调试插 `pt_log`（fprintf stderr）

## 十一、设计文档漂移对照（design_piecetab_v2.md → 代码）

| 设计文档                     | 实际代码                                                                                        |
| ---------------------------- | ----------------------------------------------------------------------------------------------- |
| `pt_newstate`                | `pt_open`                                                                                       |
| `pt_buffer(S, *plen)`        | `pt_scratch(C, *plen)` + `pt_reserve(C, len)` + `pt_literal(C, len)`（cursor 级，绑定树 arena） |
| `PT_HOLE_CAP`                | `PT_MAX_HOLESIZE`                                                                               |
| `pt_Hole { n; data[] }`      | `pt_Hole { data[] }`（长度存父 bytes[i]）                                                       |
| `mask[PT_MASK_SIZE]` 数组    | 单 `pt_Mask`（size_t），FANOUT ≤ 64                                                             |
| `pt_Offset` / `pt_Size`      | `pt_Delta`（ptrdiff_t）/ `size_t`                                                               |
| `ptM_upbytes` + `ptM_upmask` | 合并为 `ptM_up`                                                                                 |
| scratch 全局共享             | arena 每树独享（`pt_Tree.arena`）                                                               |
| refcount 字段 `refcount`     | `refc`                                                                                          |

## 十二、相关文档

| 文档                          | 内容                                     |
| ----------------------------- | ---------------------------------------- |
| `notes/design_piecetab_v2.md` | 架构设计（命名漂移见 §十一，模型仍准确） |
| `notes/brief_linecache.md`    | 姊妹库 linecache 总览（变量命名表通用）  |
| `notes/guide_debug.md`        | 调试指引                                 |

本地文档（未随仓库发布）：`notes/plans/` 存未实现设计（分代 arena、marktree），
`notes/archive/` 存历史开发计划/审核/研究记录。

## 十三、变量命名规范

> 铁律：新增函数必须从表取名。基础表沿用 `notes/brief_linecache.md` §十三
> （`l`/`k`/`i`/`cc`/`db`/`rm`/`rt`/`sC`/`fl`/`kl`/`cL`/`cR` 等全部通用）。
> piecetab 特有增补：

| 变量     | 含义                               | 上下文                       |
| -------- | ---------------------------------- | ---------------------------- |
| `C`      | pt_Cursor（公共 API 与内部统一）   | 几乎所有函数                 |
| `L`/`R`  | 左/右游标                          | remove 双游标                |
| `b`      | pt_Buffer                          | 公共 API                     |
| `t`/`nt` | pt_Tree / new tree                 | 生命周期函数                 |
| `poff`   | piece 内偏移（cursor 字段）        |                              |
| `po`     | poff 快照                          | `ptI_fillrt`, `ptI_splitins` |
| `h`      | hole 标志(int) 或 pt_Hole*         | 插入路径 / hole 操作         |
| `ph`     | previous-piece hole 标志           | `ptI_fillrt`, `ptI_splitins` |
| `m`      | pt_Mask 迭代副本 / min 槽数        | `ptM_iterhole` / `splitins`  |
| `v`      | pt_Ver                             | `ptN_purge`                  |
| `nn`     | new node                           | COW/makechain                |
| `nd`     | node destination（cow 后当前节点） | `ptI_splitchild`             |
| `nw`     | new right node（分裂右半）         | `splitroot`/`splitchild`     |
| `pp`     | 分裂后左半 / pt_Block** 游走指针   | `splitroot` / `pt_reserve`   |
