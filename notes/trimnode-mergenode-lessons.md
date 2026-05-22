# trimnode/mergenode 重构教训

## 1. combine 后禁 memmove/rmchild

mergeleaf combine 合并右叶入左叶后，右叶须释放。吾用 `lcD_rmchild(pr, ir)` 以 memmove 删父节点中死子——错。

**正解**：`R->paths[lr] += 1`。cursor 自然推移越过死子，死子留于父节点 children[]，由上层 mergenode 以 `sr` 偏移在合并时跳过。

此乃 cursor 状态维护之核心：数据变则 cursor 随之而变，不倚事后重定位。mergeleaf combine 以 `paths[l] += 1`；mergenode combine 同。

## 2. trimnode 不做 compact

trimnode(left=1) 释放 cursor 左侧死子后再调 lcN_compact——错。

**正解**：trimnode 之责唯释放死子并更新度量。死子应留于数组，由后续 mergenode 以 `sr` 偏移自然处理。在 trimnode 补 compact 为反向修正，遮掩下游 bug 之根因。

犹 `trimleaf → mergeleaf` 契约：trimleaf 留死段（lidx=0），mergeleaf 以 `R->lidx` 为 sr 偏移跳过。节点层同理：trimnode 留死子，mergenode 以 `R->paths[l+1] - sib->children` 为 sr 偏移跳过。

## 3. helper 提取层级

吾提取 `lcD_redistribute(10 params)` 和 `lcD_absorbnode(11 params)`——错。

**正解**：提取底层通用操作 `lcN_copy(d,di,s,si,n)` 和 `lcN_move(d,di,si,n)` 封装 memcpy/memmove + 断言。高层策略逻辑（combine/redistribute）保留于 `lcD_mergenode` 内，不拆分。

策略函数需访问调用者全部局部变量（L,R,l,pl,il,pr,ir,cur,cur_cc,sib,sr,sib_cc），拆则参数泛滥。底层 helper 只做通用数组操作，参数仅 5 个，签名简洁。

## 4. 左/右参数语义一致

trimnode 与 trimleaf 之 `left` 参数须同义：
- `left=1`：删 cursor 左侧
- `left=0`：删 cursor 右侧

吾初版 trimnode 中 `left=0` 为删右，与 trimleaf 反，致 splicerange 调参混乱。已正。
