# lua/piecetab.c 全量审查报告（2026-08）

> 944 行 Lua C 绑定（buffer/cursor/doc）。审查维度：状态机正确性、边界、精炼、规范。
> 已修复项带 ✓；存疑项待决策。

## 已修复

### 1. ✓ lpt_readline 缓冲区越界（严重，段错误）
- 原实现每 piece 只 `luaL_prepbuffer` 一次，`buf[n++]` 无上界——单 piece 超过
  `LUAL_BUFFERSIZE`(8192) 的文档（如 `pt.doc(长串.."\n"):read("l")`）写越界，
  实测 2 万字节长行直接 signal 11。
- 修复：piece 内按 `LUAL_BUFFERSIZE` 分块写入（每块 addsize 后重新 prepbuffer）。
  顺带删除 `n` 变量（行字节归零语义并入块循环）。
- 测试：`TestDoc.testReadLongLine`（pt_test.lua）。

### 2. ✓ lpt_seekpos "end" 负偏移无符号回绕
- 原 `n -= (size_t)(-off)`：`seek("end", -999)` 对 11 字节文档 → 回绕成巨大数 →
  定位到尾部（应为开头 0）。
- 修复：`(size_t)(-off) >= n ? 0 : n - (size_t)(-off)`。
- 测试：`TestDoc.testSeekEndNegativeClamp`。

### 3. ✓ lpt_newdoc ut_newtree OOM 泄漏
- `ut_newtree` 失败时 payload `b` 无归属（doc 的 __gc 因 `!d->ut` 直接返回），
  泄漏。修复：失败路径先 `pt_release(b)`。

## 存疑项（全部已闭环）

### 4. ✓ lpt_switch 吞 hunkapply 失败
- 原 `r = lpt_hunkapply(...)` 失败（仅 OOM）不检查，继续 ut_switch + seek。
- 修复：失败抛错且不 switch（事务语义：失败 = 无操作，cursor 保持绑 src，
  lcvid 不更新 → 下次查询重 diff 懒恢复）。与 undo/commit 风格统一。

### 5. ✓ Ldoc_commit 中途 OOM 后 doc 状态损坏
- 原 `pt_commit` 成功后 hunkapply 失败 → 抛错，d->C 已 detach（tree=NULL）。
- 修复：hunkapply 失败时先 `pt_seek(&d->C, b, off)` 重绑定再抛——b 仍存活，
  重试 commit 可成功（clean commit 返回同内容，journal 重规范化收敛）。
  ut_commit 失败路径保持 detached（pt_commit 对 NULL tree 安全返回 NULL，
  重试安全失败，不崩）。

### 6. ✓ Lbuf_pieceiter 显式 delete 后 use-after-free
- 修复：迭代器改为 `lpt_PieceIter` userdata，自持 `pt_retain` 的树引用
  （__gc/__close 幂等释放）；`pieces()` 返回 4 值（iterator, nil, nil,
  closing），5.4+ 循环退出经 __close 确定性释放，5.1-5.3 走 __gc。
  外部 `b:delete()` 杀不死迭代器持有的树；close 后调用迭代器返回 0
  （`it->b == NULL` 守卫，不碰悬垂 cursor）。

### 7. ✓ breaks() 返回语义与设计文档表述张力
- 代码行为（断数 + 尾残段修正 = editor 行号语义）与设计文档"断数直通
  lc_breaks；行数恒 = breaks+1"表述冲突。代码正确（editor 实际用法
  breaks()-1 为最后行号，测试固化），更新 design_luabind.md §五/§7.1
  表述为"返回行数（断数 + 残段修正；尾 \n 虚拟空行不计）"。

### 8. ✓ Ldoc_edit/Lcur_edit 负数 del
- 原负数 cast 回绕靠 C 层 clamp 兜底，行为模糊且与 read/seek 的 argcheck
  不一致。修复：6 处（edit/remove/splice × doc/cursor）统一
  `luaL_argcheck(>= 0, "amount must be non-negative")`。
- 测试：`TestDoc.testEditNegativeAmount`。

## 精炼（已做）

### 9. ✓ Lpt_from 复用 lpt_tobuffer
- 原 12 行重复逻辑（seek empty + toliteral + insert + commit）缩为 3 行
  （`*lpt_newbuffer(L) = lpt_tobuffer(L, 1)`）。语义逐项等价（空串 → pt_empty、
  OOM 路径一致）。

### 10. ✓ Lbuf_read 删多余变量
- `n = (size_t)cnt` 一次性变量内联为 `(size_t)cnt`。

## 未发现问题的区域

- **lck/lcvid 状态机**：fresh>0 时 lcvid==cur 恒成立（undo/switch 前必丢草稿、
  commit 消耗 journal），docsync 版本 diff 不会在 fresh 非空时执行——读源
  坐标一致，无错位。undo/redo/earlier/later 的 hunkapply 读源 = 目标节点
  payload，坐标匹配。
- **hunkapply OOM 恢复**：截断 [hs[0].pa, lc_bytes) + lcvid 不更新 → 下次
  重 diff，符合设计 §6.5/§7.3 摩擦 #13。
- **错误路径**：pt 编辑 ERRMEM 事务语义下 ut_unrecord 回退 journal 正确。
- **边界**：LPT_UNL 无限值、空文档、光标尾部、残段 trailing、read 族 EOF
  nil 协议均正确。
- **兼容层**：5.1/5.2 shim、LuaJIT 适配、__close/__gc 幂等均合规。
