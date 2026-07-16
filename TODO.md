# Roadmap

> 当前状态：`linecache.h` 与 `piecetab.h` 功能完整，
> 行覆盖 100%、分支覆盖 ~90%。以下为未来方向。

## ~~内存回收：分代 Arena~~ — 已完成

per-tree arena（`pt_reserve`/`pt_scratch`/`pt_literal`）、release 整链回收、
`pt_compact`（区间判定 external/internal + lc_scan 式批量建树）均已落地。
设计推导见本地设计文档（未随仓库发布）。

## Mark 系统：marktree

vis 风格编辑器 mark 库。单头 C89，前缀 `mk_`，基于 gap 编码 B+ 树，
支持点标记与区间标记（配对）。参考 Neovim `marktree.c`。

## 可选优化

- hole 叶缝合时的合并/平分（linecache `d` 机制在 piecetab 的对应物）
- 批量插入 API（`lc_append` 对应物；`ptD_makechain` 根加深已随
  `pt_compact` 恢复，剩余为 findroom 右余搬运与公共 API 封装）
- commit `pt_reserve(total)` 触发新块时旧头块尾部残余被跳过（release 时
  才回收）——实测有内存压力再改 freeze 逐 hole 分配

## 发布准备

- [ ] README badges 与 Features 核对
- [ ] CI（GitHub Actions：build + test + coverage）
