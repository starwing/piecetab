# bench 设计案

> 状态：已实现 piecetab 与 spantree 两套（bench/ + bench/scripts/ + bench/justfile），各自完整 FANOUT 扫描已跑/进行中（PT 64 因 `ptM_mask(64)` UB 排除）。
> 范围：harness + piecetab FANOUT 扫描（4..63 全整数）+ spantree FANOUT 扫描（4..63 全整数）。
> 原则：性能基准与正确性压力分离；bench 用 release 构建，stress 用 ASan/UBSan。
> 坐标系：结构基数驱动（piece 数 / 行数 / span 数 / 版本数），不是文件字节数。

## 一、目标

1. 建立可复用的 C89 benchmark harness：
   - 支持 `-O2 -DNDEBUG` release 构建；
   - 支持同一份 bench 源码以不同编译期宏（如 `PT_FANOUT`）重复编译；
   - 每次运行输出 JSON，包含机器/编译器/flags/git/参数/逐 case 数据；
   - 后续可直接扩展 linecache / spantree / undotree。
2. 以 **public API 为探测维度**，不把内部函数名当作 bench case 名。
   - 内部函数是同一个 API 不同调用路径的实现细节；
   - 例如 `pt_seek`、`pt_locate`、`pt_advance`、`pt_read`、`pt_next`、
     `pt_edit`、`pt_insert`、`pt_append`、`pt_splice`、`pt_remove`、
     `pt_commit`、`pt_rollback`、`pt_compact` 各自是独立维度。
3. 第一期产出 piecetab 在 `PT_FANOUT ∈ [4, 63]` 全整数上的性能曲线。
   - 不预设 2/4/8/16/32/64；
   - 如果 13 最优，就接受 13。

## 二、总体架构

```text
bench/
  bench.h           # C89 harness：CLI、计时、统计、JSON 输出
  bench_pt.c        # piecetab API 维度 cases
  bench_sp.c        # spantree API 维度 cases
  bench_data.h      # piecetab 确定性 corpus 生成器（结构基数驱动）
  justfile          # recipes: all/smoke/pt-sweep/sp-sweep/pt-plot/sp-plot/clean
  scripts/
    bench_sweep.sh    # piecetab 编译矩阵：FANOUT 循环 → 运行 → 收集 JSON
    bench_sweep_sp.sh # spantree 编译矩阵：SP_FANOUT 循环 → 运行 → 收集 JSON
    plot_bench.py     # 读 JSON → 生成曲线图（PT/SP 通用）
    analyze_sp.py     # spantree sweep 聚合分析
  build/pt/          # piecetab 编译产物（gitignore）
  build/sp/          # spantree 编译产物（gitignore）
  results/pt/        # piecetab 每次 sweep 的 JSON（gitignore）
  results/sp/        # spantree 每次 sweep 的 JSON（gitignore）
  reports/pt/        # piecetab 图表和报告（gitignore）
  reports/sp/        # spantree 图表和报告（gitignore）
```

数据流：

```text
bench/scripts/bench_sweep.sh
  └─ for F in $(seq 4 63)
       ├─ cc -DPT_FANOUT=$F -DPT_MAX_LEVEL=$safe -O2 -DNDEBUG ... \
       │    -o bench/build/pt/pt_fanout_$F bench/bench_pt.c
       └─ ./bench/build/pt/pt_fanout_$F --json bench/results/pt/pt_fanout_$F.json
            └─ bench/results/pt/pt_fanout_*.json
                 └─ bench/scripts/plot_bench.py
                      └─ bench/reports/pt/pt_fanout_*.svg/png + summary table

bench/scripts/bench_sweep_sp.sh
  └─ for F in $(seq 4 63)
       ├─ cc -DSP_FANOUT=$F -DSP_MAX_LEVEL=$safe -O2 -DNDEBUG ... \
       │    -o bench/build/sp/sp_fanout_$F bench/bench_sp.c
       └─ ./bench/build/sp/sp_fanout_$F --json bench/results/sp/sp_fanout_$F.json
            └─ bench/results/sp/sp_fanout_*.json
                 └─ bench/scripts/plot_bench.py
                      └─ bench/reports/sp/sp_fanout_*.svg/png + summary table
```
                 └─ bench/scripts/plot_bench.py
                      └─ bench/reports/pt_fanout_*.svg/png + summary table
```

## 三、Harness 设计（bench.h）

### 3.1 对外接口

```c
typedef struct bench_Case {
    const char *name;              /* 形如 "pt_seek" */
    int (*setup)(void **ud, const bench_Params *p);
    int (*run)(void *ud, long iters);   /* 执行 iters 次被测操作 */
    void (*teardown)(void *ud);
} bench_Case;
```

- `name` 必须对应 public API 或明确 API 组合，禁止用内部函数名。
- 每个 case 可选 `setup/teardown`，用于在每轮迭代之间重建状态。
- 被测操作放在 `run` 内，由 harness 统一 warmup + 多轮计时。

### 3.2 CLI

```text
./pt_fanout_13 --json bench/results/pt_fanout_13.json \
               --seed 1 \
               --case pt_seek \
               --iters 100000 \
               --rounds 7
```

- `--case` 可多次传入；不传则跑全部。
- `--json` 指定输出文件；缺省输出到 stdout。
- `--seed` 固定 corpus 与随机访问序列。
- `--rounds` 每 case 轮数，默认 7。
- `--iters` 每轮操作数，默认由 case 提供。

### 3.3 计时与统计

- 计时用 `clock_gettime(CLOCK_MONOTONIC)`；macOS 封装 `mach_absolute_time`。
- 每个 case：
  1. warmup 若干次，触发分支预测/缓存预热；
  2. 跑 `rounds` 轮；
  3. 记录每轮总耗时，换算 `ns/op`；
  4. 输出 `median`、`min`、`p10`、`p90`。
- 不取平均值作为主指标，避免 outlier 污染。

### 3.4 JSON schema

```json
{
  "benchmark": "pt_fanout_sweep",
  "git_commit": "979a480...",
  "compiler": "gcc (Homebrew GCC 14) 14.2.0",
  "cflags": "-O2 -DNDEBUG -DPT_FANOUT=13 -std=c89 -pedantic -Wall -Wextra",
  "machine": {"os": "macOS", "cpu": "Apple M3", "ram": "32GB"},
  "params": {"PT_FANOUT": 13, "seed": 1},
  "cases": [
    {
      "name": "pt_seek",
      "corpus": "fragmented_100k",
      "iters": 100000,
      "rounds": 7,
      "ns_per_op": 123.4,
      "median_ns": 123.0,
      "min_ns": 121.5,
      "p10_ns": 121.8,
      "p90_ns": 125.2,
      "tree_height": 4,
      "node_count": 1234
    }
  ]
}
```

- `params` 记录编译期宏值，绘图脚本据此作为 x 轴。
- 可选记录 `tree_height`、`node_count` 等结构指标，用于解释曲线。

## 四、Corpus 设计（bench_data.h）

### 4.1 生成原则

- 固定 seed，确定性。
- 主变量是结构基数：piece 数、编辑次数、访问分布。
- 内容字节数不作为性能变量；超大内容用 mmap/虚拟指针表示，不实际分配。

### 4.2 piecetab 第一期 corpus

| corpus | 结构 | 说明 |
|---|---|---|
| `single_piece` | 1 个 literal piece | mmap 大文件等价物 |
| `fragmented_N` | N 个 literal piece | 碎片化；N 取 1K/10K/100K/1M |
| `edited_N` | 从 1 piece 经 N 次随机编辑生成 | 真实编辑历史 |

每个 corpus 提供：

- 构造好的 `pt_Buffer`；
- 随机访问序列（预生成，避免 RNG 开销混入计时）；
- 随机编辑序列（off/del/ins 分布可配）。

## 五、piecetab API 维度 cases

所有 case 名以 public API 命名。每个 case 内部可以说明“该 API 的典型调用路径”，
但 bench 输出/图表只按 API 维度分组。

### 5.1 导航/读取

| case | 操作 | 说明 |
|---|---|---|
| `pt_from` | 接管一个外部指针 | adoption cost；不进入编辑性能叙事 |
| `pt_seek` | 从随机偏移构造游标 | `pt_seek(C, b, off)` |
| `pt_locate` | 在已绑定游标上随机定位 | `pt_locate(C, off)` |
| `pt_advance` | 随机正/负步进 | `pt_advance(C, d)` |
| `pt_read` | seek 后读取一段 | `pt_seek` + `pt_read` |
| `pt_next` | 顺序遍历全部 piece | 全量扫描 |
| `pt_prev` | 逆序遍历 | 全量反向扫描 |

### 5.2 编辑

| case | 操作 | 说明 |
|---|---|---|
| `pt_edit` | 小编辑（hole 路径），不 commit | 测量编辑本身 |
| `pt_edit_commit` | 小编辑 + `pt_commit` | 编辑 + 冻结 |
| `pt_insert` | 插入外部 literal | 引用语义 |
| `pt_append` | 尾部追加 literal | 引用语义 |
| `pt_splice` | 区间删插 | 大编辑路径 |
| `pt_remove` | 区间删除 | 删除/平衡路径 |

### 5.3 事务/批量

| case | 操作 | 说明 |
|---|---|---|
| `pt_commit` | 对已 dirty 树 commit | freeze 路径 |
| `pt_rollback` | 对已 dirty 树 rollback | COW 回退 |
| `pt_compact` | 对编辑后的树 compact | 批量建树 |

### 5.4 状态重置

- 编辑类 case 每轮之间必须把树重置到同一初始结构，否则测量的是“累积碎片化”而不是 API 成本。
- harness 的 `setup/teardown` 负责重建，不把重建时间计入 case。

## 六、FANOUT 扫描（bench_sweep.sh）

### 6.1 编译矩阵

```bash
for f in $(seq 4 63); do
  cc $CFLAGS -DPT_FANOUT=$f -DPT_MAX_LEVEL=$safe -O2 -DNDEBUG \
     -I. -Ibench -o bench/build/pt_fanout_$f bench/bench_pt.c
  # $safe 来自 docs/max_levels.md：每个 FANOUT 在 64 位下不可能溢出的 MAX_LEVELS
  ./bench/build/pt_fanout_$f --seed 1 \
     --json bench/results/pt_fanout_$f.json
done
```

- `PT_FANOUT` 全整数 4..63，共 60 个二进制（64 排除：`ptM_mask(64)` UB）。
- 每个二进制只编译一次，跑全部 case。
- 若运行时间过长，可先用 coarse 集 `4 6 8 10 13 16 20 24 32 40 48 56 63`
  定位大致区间，再对最优区间做全整数精扫；但第一期默认直接全整数。

### 6.2 收集

- 每个 JSON 文件自带 `params.PT_FANOUT`。
- `bench/results/` 保留原始 JSON，作为报告数据源。

## 七、绘图（plot_bench.py）

### 7.1 输入

- `bench/results/pt_fanout_*.json` 一组文件。
- 按 `params.PT_FANOUT` 排序。

### 7.2 输出

- 对每个 API case（如 `pt_seek`、`pt_edit_commit`、`pt_compact`）生成一张图：
  - x 轴：`PT_FANOUT`
  - y 轴：`ns_per_op`
  - 每个 corpus 一条线。
- 同时输出一张总览图，按 case 分面（subplot）。
- 格式：SVG + PNG，存 `bench/reports/`。

### 7.3 图表内容

- 图上标注最小点对应的 FANOUT。
- 附机器/编译器/flags 信息。
- 原始数据仍在 JSON，便于后续做表格或统计检验。

## 八、与 stress 的关系

- `bench` 只做性能测量：release 构建、无 sanitizer、固定机器。
- `stress` 做正确性验证：ASan/UBSan、fuzz、OOM、大基数。
- 两者共享 corpus 生成思路，但构建与运行完全分开。

## 九、后续扩展

- linecache：`LC_FANOUT` / `LC_LEAF_FANOUT` 双参数扫描。
- spantree：`SP_FANOUT` 扫描 + `sp_fill` 大 span 场景。
- undotree：无 fanout，但可复用 harness 做版本数/分支数曲线。
- 端到端：`misc/bench_lsp.lua`（对应 `notes/plans/plan_lsp_integration.md` Task 8）。

## 十、第一期验收

1. `just bench/all` 能编译并运行默认 FANOUT 的 piecetab 全部 API cases。
2. `just bench/sweep` 能产出 `bench/results/pt_fanout_4.json` … `bench/results/pt_fanout_64.json`。
3. `python3 bench/scripts/plot_bench.py bench/results/pt_fanout_*.json --out bench/reports/` 能生成曲线图。
4. 报告 `notes/reports/bench_tuning_pt.md` 包含：
   - 各 API 维度曲线；
   - 每个维度最优 FANOUT；
   - 综合建议（可能是一个区间，而不是单一值）；
   - 是否修改默认 `PT_FANOUT=31` 的建议。
