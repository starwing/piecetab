# Roadmap

> 当前状态：`linecache.h` 与 `piecetab.h` 功能完整，
> 行覆盖 100%、分支覆盖 ~90%。以下为未来方向。

## 内存回收：分代 Arena

让 piecetab 在保持 immutable/COW/zero-copy 核心的同时，支持
"当可变数据结构使用"并具备 Buffer 级内存回收能力。
设计推导与接口草案见本地设计文档（未随仓库发布）。

## Mark 系统：marktree

vis 风格编辑器 mark 库。单头 C89，前缀 `mk_`，基于 gap 编码 B+ 树，
支持点标记与区间标记（配对）。参考 Neovim `marktree.c`。

## 可选优化

- hole 叶缝合时的合并/平分（linecache `d` 机制在 piecetab 的对应物）
- 批量插入 API（bulk loading，可从 linecache `lc_scan` 经验恢复）

## 发布准备

- [ ] README badges 与 Features 核对
- [ ] CI（GitHub Actions：build + test + coverage）
