# Roadmap

> 当前状态：`linecache.h` 与 `piecetab.h` 功能完整，
> 行覆盖 100%、分支覆盖 ~90%。以下为未来方向。

## 下一步优先级序列（2026-07 决议）

> 原则：**先让 C 库学会说 Lua，再用 Lua 造下一个 C 库**。
> Lua 边界是产品本体（toolkit 架构），最大未验证风险在边界不在性能。

1. **linecache + piecetab Lua 绑定 + Lua 测试样例**
   → 产出绑定惯例（cursor 生命周期/错误形态/字符串所有权）+ 接口
   摩擦清单；spantree 从第一天按惯例设计
2. **bench 骨架**（防回归级：just 一键 + 固定场景；不调优）
3. **spantree C 本体**（按绑定惯例设计 API）+ **混合器/身份层 Lua
   原型**（开放问题——默认属性、仲裁细则——在原型中澄清）
4. **tree-sitter 适配**接入（验证引擎接口 + split 分块试验）
5. 真实负载就位后：**FANOUT 调优 + rope/piecetable 对比 bench**
   （三库同骨架，一次调优覆盖）

## ~~内存回收：分代 Arena~~ — 已完成

per-tree arena（`pt_reserve`/`pt_scratch`/`pt_literal`）、release 整链回收、
`pt_compact`（区间判定 external/internal + lc_scan 式批量建树）均已落地。
设计推导见本地设计文档（未随仓库发布）。

## Mark/高亮系统：spantree

~~marktree（gap 编码 B+ 树点标记）~~ → 设计演进为 **spantree**：字节
覆盖属性段树（linecache 骨架 + 编译期分层属性向量），无 gravity（操作
决定继承）。决议与开放问题见 `notes/design_spantree.md`，调研背景见
`notes/research_marktree.md`、`notes/research_highlighter.md`。

- [ ] spantree 本体（linecache 叶元素扩为 `(len, attr[SPAN_LAYERS])`
      + 规范形 merge）
- [ ] 层写入 API（`layer_write` 区间替换）与读端级联
- [ ] 混合器（插件拉模式仲裁）
- [ ] 高亮引擎适配：首选 tree-sitter（验证最苛刻接口 + split 分块），
      次选 Scintillua LPeg（若嵌 Lua）
- [ ] 身份层（extmark 对象模型，含 gravity 漂移模拟）

## Lua 绑定（最终 Demo 形态）

linecache / piecetab / spantree 均需 Lua 绑定，**Demo 以 Lua 库形式
交付——接口设计以 Lua 暴露形态为准绳**。

- [ ] 三库 Lua 绑定 API 草案（先定语义后定签名）
- [ ] extmark 染色子集方言兼容（见 design_spantree.md 第八节：
      set/get/del/clear + 染色 opts + gravity 模拟；渲染注入类不承诺）

## 可选优化

- hole 叶缝合时的合并/平分（linecache `d` 机制在 piecetab 的对应物）
- 批量插入 API（`lc_append` 对应物；`ptD_makechain` 根加深已随
  `pt_compact` 恢复，剩余为 findroom 右余搬运与公共 API 封装）
- commit `pt_reserve(total)` 触发新块时旧头块尾部残余被跳过（release 时
  才回收）——实测有内存压力再改 freeze 逐 hole 分配

## 发布准备

- [ ] README badges 与 Features 核对
- [ ] CI（GitHub Actions：build + test + coverage）
