# libtermkey 调研报告

## 源码位置

- 上游: `/Users/sw/Work/Sources/libtermkey` (Neovim 托管仓库)
- Vendor: `/Users/sw/Work/Sources/neovim/src/nvim/tui/termkey`

## 一、Neovim vendor 后的改动总览

### 1.1 删掉的文件

- `Makefile` — Neovim 用 CMake 构建
- `demo.c`, `demo-async.c`, `demo-glib.c` — 示例/测试程序
- `t/` — 测试套件（Neovim 用自己的测试框架）
- `man/` — 手册
- `termkey.h.in` — 模板头，改由代码生成 (`termkey.h.generated.h`)
- `termkey.pc.sh` — pkg-config 生成脚本

### 1.2 新增的文件

- `termkey_defs.h` — 从 `termkey.h` 拆出类型定义+`TermKeyTI`/`TermKeyCsi` struct
- `driver-csi.h`, `driver-ti.h` — 驱动头的独立声明（只 include generated header）
- `README` — 简短说明

### 1.3 文件拆分

原始单一 `termkey.h.in` (249 行) 被拆为:

```
termkey.h           (8 行) — 仅 include termkey_defs.h + generated header
termkey_defs.h      (215 行) — 所有类型/enum/struct/ext slot
termkey-internal.h  (109 行) — internal struct + inline helper
├── TermKey 主 struct
├── TermKeyDriver struct
├── TermKeyDriverNode
├── termkey_key_get_linecol + termkey_key_set_linecol (inline)
└── 注意: 不再 extern 声明 driver，改为各 driver 头文件声明
```

### 1.4 核心架构变更

| 维度 | 原始 libtermkey | Neovim vendor |
|------|----------------|--------------|
| I/O 模型 | `read()`/`poll()` 阻塞式 | libuv 事件循环，仅支持 `push_bytes` |
| terminfo | unibilium 或 curses `setupterm` | Neovim 内置 `TerminfoEntry` 表 |
| 内存管理 | raw `malloc`/`free`/`realloc` | `xmalloc`/`xfree`/`xrealloc` (OOM abort) |
| termkey_new | 两个构造器 `(fd, flags)` + `(term, flags)` | 仅 `termkey_new_abstract(term, flags)` |
| driver 注册 | extern 声明在 termkey-internal.h | 各自 .h 声明，driver struct 定义在 termkey.c |
| codepoint 类型 | `long` | `int` |
| UTF-8 工具 | 自带 `utf8_seqlen`/`fill_utf8`/`parse_utf8` | 复用 Neovim `utf_char2bytes`/`utf_char2len`/`mbyte.h` |
| UTF-8 非法码点 | `UTF8_INVALID = 0xFFFD` | `UNICODE_INVALID` (Neovim 全局常量) |

### 1.5 公共 API 删除

删除了以下不再需要或与 Neovim 架构冲突的 API:
- `termkey_new(int fd, ...)` — I/O 层由 libuv 处理
- `termkey_waitkey` — 阻塞式 poll() 被事件循环替代
- `termkey_advisereadable` — 数据推送由 libuv 回调驱动
- `termkey_strpkey` — Neovim 不需要字符串→Key 反解析
- `termkey_keycmp` — 未使用
- `termkey_keyname2sym` — 未使用
- `termkey_check_version` — 版本检查无意义
- `termkey_get_fd`/`is_started`/`get_flags`/`get_waittime`/`set_waittime` — I/O 相关域
- `termkey_snprint_key` — 旧名称向后兼容别名

### 1.6 公共 API 新增

- `TermKeyEvent` enum (PRESS/REPEAT/RELEASE) — kitty 键盘协议事件类型
- `TERMKEY_TYPE_APC` — 新增 APC 控制字符串类型
- `TERMKEY_FLAG_KEEPC0` — 保留原始 C0 控制码
- `TermKeyCsiParam` struct — CSI 参数的 zero-copy 表示
- `termkey_interpret_csi_param` — 解析单个 CSI 参数（含子参数 : 分隔）
- 参数化 driver 入口: `new_driver(TermKey *tk, TerminfoEntry *term)`, 各自命名
- `peekkey_csi` / `peekkey_ti` — 驱动入口函数改名

### 1.7 内部实现变更

**driver-csi.c 主要变更:**

1. **CSI 参数 zero-copy 解析**: 原始版本将 CSI 参数解析为 `long arg[]` 数组（数值拷贝）。
   Neovim 改为 `TermKeyCsiParam params[]` — 仅存指向 buffer 内原始字节的指针和长度，
   通过 `termkey_interpret_csi_param()` 按需解析。支持子参数（`:` 分隔）用于 kitty 协议。

2. **按键事件协议**: 新增 `parse_key_event()` 将 CSI u 的子参数映射为 `TermKeyEvent`。
   所有 handler 签名从 `(long *arg, int args)` 改为 `(TermKeyCsiParam *params, int nparams)`。

3. **APC 序列支持**: `peekkey_ctrlstring` 新增 0x5f/0x9f 分支处理 APC，
   替换原始的二元判断 `(CHARAT(introlen-1) & 0x1f) == 0x10 ? DCS : OSC` 为 switch-case。

4. **鼠标按钮扩展**: `termkey_interpret_mouse` 新增 case 128/129 分支支持按钮 8-9。

5. **驱动入口重命名**: `new_driver`→`new_driver_csi`, `free_driver`→`free_driver_csi`,
   `peekkey`→`peekkey_csi`。内部 CSI handler 另名为 `peekkey_csi_csi`。

6. **interpret 系列函数移出**: `termkey_interpret_mouse`/`_position`/`_modereport`
   从 driver-csi.c 移到 termkey.c（在原始版本中也在 driver-csi.c 末但 Neovim 搬到主文件）。

**driver-ti.c 主要变更:**

1. **TerminfoEntry 替代 unibilium**: 原版 `unibi_term *unibi` 或 `char *term` 变量
   替换为 `TerminfoEntry *ti` 统一接口。`new_driver_ti` 不再调用 `unibi_from_term`
   或 `setupterm`，而是接收外部准备好的 `TerminfoEntry`。

2. **键表裁剪**: 大量 terminfo 键被注释掉（标"not recognized by nvim"或"redundant"）:
   `begin`→`beg`, `cancel`, `close`, `command`, `copy`, `down`, `enter`, `exit`,
   `help`, `mark`, `message`, `move`, `next`, `open`, `options`, `previous`,
   `print`, `redo`, `reference`, `refresh`, `replace`, `restart`, `resume`,
   `save`, `up`。保留仅: backspace, beg, btab, clear, dc, end, find, home,
   ic, left, npage, ppage, right, select, suspend, undo。

3. **键加载重构**: `try_load_terminfo_key` 接收 `(bool fn_nr, int key, bool shift)`
   参数，通过 `ti->ti->keys[key]` 或 `ti->ti->f_keys[key]` 索引查表，
   而非通过字符串名调用 `unibi_get_str_by_name`。

4. **鼠标支持彻底禁用**: 整段 mouse loading 被放在 `if (false)` 中。
   Neovim 仅使用 driver-csi 处理鼠标。

5. **驱动入口重命名**: `new_driver`→`new_driver_ti`, `free_driver`→`free_driver_ti`,
   `start_driver`→`start_driver_ti`, `stop_driver`→`stop_driver_ti`,
   `peekkey`→`peekkey_ti`。

**termkey.c 主要变更:**

1. **构造器精简**: 移除 `termkey_new(int fd)`，仅保留 `termkey_new_abstract(TerminfoEntry *term, int flags)`。
   `termkey_init` 签名改为接收 `TerminfoEntry *term` 而非 `const char *term`。

2. **`termkey_interpret_string` 移入**: 从 driver-csi.c 移至 termkey.c，
   新增 APC 类型检查。

3. **编码工具外移**: 使用 `utf_char2bytes`/`utf_char2len` 替代自带的 `fill_utf8`/`utf8_seqlen`。
   `parse_utf8` 改为返回 `int` codepoint。

4. **driver struct 定义移动**: `termkey_driver_csi` 和 `termkey_driver_ti` 的实例化
   从各自驱动文件移至 termkey.c，使驱动 .c 文件成为纯实现。

5. **`FALLTHROUGH` 宏**: switch-case fallthrough 显式标注。

6. **`key->event` 默认值**: `peekkey` 中设置 `key->event = TERMKEY_EVENT_PRESS`。

## 二、driver_csi 与 driver_ti 功能/实现差异

### 2.1 driver_csi (CSI 驱动)

**功能**: 硬编码解析标准 ANSI CSI/SS3 转义序列。它不需要 terminfo 数据库。

**覆盖的序列类型:**
- SS3 (ESC O + cmd): 方向键、功能键 F1-F4、小键盘键
- CSI basic: 方向键变体、Home/End、Shift-Tab
- CSI ~ (number + ~): 功能键 (F1-F20)、编辑键 (Find/Insert/Delete/Select/PgUp/PgDn/Home/End)
- CSI u (kitty): 扩展 Unicode 按键，支持任意修饰键组合
- CSI M/m: 鼠标事件 (X10、rxvt、SGR 编码)
- CSI R: 光标位置报告
- CSI $y: 模式状态报告
- DCS/OSC/APC: 控制字符串（Neovim 新增 APC）

**实现手法:**
1. **查找表驱动**: `csi_handlers[64]` 数组按 CSI 命令字 (0x40-0x7F) 索引 handler 函数指针
2. **参数解析**: 从 buffer 中直接扫描，构建参数数组。Neovim 改为 zero-copy 的 pointer+length 方式
3. **键注册**: `register_keys()` 在首次 driver 创建时静态初始化键表
4. **不回退到 terminfo**: 不理解序列直接返回 NONE

### 2.2 driver_ti (terminfo 驱动)

**功能**: 从 terminfo 数据库动态加载键盘序列，构建 trie 用于高效查找。

**覆盖的序列:**
- 所有 terminfo `key_*` 和 `key_s*` 标准键序列
- 所有 terminfo `key_f*` 功能键序列
- `key_mouse` (X10 编码)，但 Neovim 中已禁用
- 启动/停止时发送 `keypad_xmit`(smkx)/`keypad_local`(rmkx)

**实现手法:**
1. **Trie 结构**: `trie_node_arr` (256 路子节点 extent map) + `trie_node_key` (叶子)
2. **惰性加载**: `new_driver` 不构建 trie，`start_driver` 首次调用 `load_terminfo` 才构建
3. **压缩**: `compress_trie` 将全 256 路 root 压缩到实际使用的 min/max 范围
4. **匹配:** 字节推进 trie，不完整匹配返回 AGAIN，无匹配返回 NONE

### 2.4 全部识别的键序列表

| 键 | 序列格式 | CSI 驱动 | TI 驱动 | 说明 |
|----|---------|:------:|:------:|------|
| Up | `\e[A` (CSI) / `\eOA` (SS3) | ✓ | ✓(原版) | Neovim TI 注释"redundant" |
| Down | `\e[B` / `\eOB` | ✓ | ✓(原版) | Neovim TI 注释"redundant" |
| Right | `\e[C` / `\eOC` | ✓ | ✓ | |
| Left | `\e[D` / `\eOD` | ✓ | ✓ | |
| Home | `\e[H` / `\eOH` / `\e[7~` | ✓ | ✓ | |
| End | `\e[F` / `\eOF` / `\e[8~` | ✓ | ✓ | |
| Begin | `\e[E` / `\eOE` | ✓ | ✓ | TI: `key_beg` |
| PgUp | `\e[5~` | ✓ | ✓ | TI: `key_ppage` |
| PgDn | `\e[6~` | ✓ | ✓ | TI: `key_npage` |
| Insert | `\e[2~` | ✓ | ✓ | TI: `key_ic` |
| Delete | `\e[3~` | ✓ | ✓ | TI: `key_dc` |
| Select | `\e[4~` | ✓ | ✓ | |
| Find | `\e[1~` | ✓ | ✓ | |
| Backspace | C0 (0x08/0x7f) | — | ✓ | TI: `key_backspace` |
| Tab | C0 (0x09) | — | — | C0 层 `register_c0(TAB, 0x09)` |
| Enter | C0 (0x0d) | — | ✓(原版) | Neovim TI 注释"redundant" |
| Escape | C0 (0x1b) | — | — | C0 层 `register_c0(ESCAPE, 0x1b)` |
| Shift-Tab | `\e[Z` | ✓ | ✓ | TI: `key_btab`(shifted) |
| Clear | — | — | ✓ | TI: `key_clear` |
| Suspend | — | — | ✓ | TI: `key_suspend` |
| Undo | — | — | ✓ | TI: `key_undo` |
| Cancel | — | — | ✓(原版) | Neovim TI 注释 |
| Close | — | — | ✓(原版) | Neovim TI 注释 |
| Command | — | — | ✓(原版) | Neovim TI 注释 |
| Copy | — | — | ✓(原版) | Neovim TI 注释 |
| Exit | — | — | ✓(原版) | Neovim TI 注释 |
| Help | — | — | ✓(原版) | Neovim TI 注释 |
| Mark | — | — | ✓(原版) | Neovim TI 注释 |
| Message | — | — | ✓(原版) | Neovim TI 注释 |
| Move | — | — | ✓(原版) | Neovim TI 注释 |
| Open | — | — | ✓(原版) | Neovim TI 注释 |
| Options | — | — | ✓(原版) | Neovim TI 注释 |
| Print | — | — | ✓(原版) | Neovim TI 注释 |
| Redo | — | — | ✓(原版) | Neovim TI 注释 |
| Reference | — | — | ✓(原版) | Neovim TI 注释 |
| Refresh | — | — | ✓(原版) | Neovim TI 注释 |
| Replace | — | — | ✓(原版) | Neovim TI 注释 |
| Restart | — | — | ✓(原版) | Neovim TI 注释 |
| Resume | — | — | ✓(原版) | Neovim TI 注释 |
| Save | — | — | ✓(原版) | Neovim TI 注释 |
| "Next" | — | — | ✓(原版) | TI: `key_next`→PgDn 别名 |
| "Previous" | — | — | ✓(原版) | TI: `key_previous`→PgUp 别名 |
| F1-F4 | `\eOP`~`\eOS` (SS3) / `\e[11~`~`\e[14~` | ✓ | ✓ | TI 有 `key_f1`~`key_f4` |
| F5-F20 | `\e[15~`~`\e[34~` | ✓ | ✓ | TI 有 `key_f5`~`key_f20` |
| F21-F63 | — | — | ✓ | 仅 TI 提供 |
| KP0-KP9 | `\eOp`~`\eOy` (SS3) 或 `\e[1~`变体 | ✓ | — | TI 无独立 KP 键定义 |
| KPEnter | `\eOM` (SS3) | ✓ | — | |
| KPPlus | `\eOk` (SS3) | ✓ | — | |
| KPMinus | `\eOm` (SS3) | ✓ | — | |
| KPMult | `\eOj` (SS3) | ✓ | — | |
| KPDiv | `\eOo` (SS3) | ✓ | — | |
| KPComma | `\eOl` (SS3) | ✓ | — | |
| KPPeriod | `\eOn` (SS3) | ✓ | — | |
| KPEquals | `\eOX` (SS3) | ✓ | — | |
| Unicode (kitty) | `\e[u` (CSI u) | ✓ | — | codepoint + modifiers |
| Unicode (UTF-8) | 单字节 / 多字节序列 | — | — | peekkey_simple |
| Mouse X10 | `\e[M` + 3 bytes | ✓ | ✓(原版) | TI 仅当 `key_mouse="\e[M"` |
| Mouse rxvt | `\e[M...` (CSI params) | ✓ | — | |
| Mouse SGR | `\e[<...M/m` | ✓ | — | release 用 `m` |
| Position report | `\e[N;MR` | ✓ | — | |
| Mode report | `\e[?N$y` / `\e[N$y` | ✓ | — | |
| DCS | `\eP...\e\\` / `\eP...\a` | ✓ | — | |
| OSC | `\e]...\e\\` / `\e]...\a` | ✓ | — | |
| APC | `\e_...\e\\` / `\e_...\a` | ✓(Neovim) | — | Neovim vendor 新增 |
| Alt+key | `\e` + 单字节 (失配 CSI/SS3 后) | — | — | peekkey_simple |

**重叠冲突**: TI 有 `key_up`/`key_down` 但 CSI 也有对应序列。
Neovim 裁剪 TI 以消除冲突。TI 和 CSI 都有的条目用 TI 优先匹配
（TI 先于 CSI 被调用），但 TI 的序列必须与 CSI 不同（若相同
TI 先匹配到即输出，CSI 没机会运行）。

### 2.5 设计启示

- ↑/↓/←/→、Home/End、PgUp/PgDn、Insert/Delete 都有 CSI 和 TI 两套。
  实际终端发送的序列取决于终端设置的 keypad mode（smkx/rmkx）。
  TI driver 的 start_driver 发送 smkx 开启应用键模式。
- TI 覆盖的键大部分是"罕见终端自定义"——标准终端中这些键的序列
  与 CSI 驱动一致。Neovim 裁剪证实了这一点。
- driver_ti 的核心价值在 F21-F63（CSI 只到 F20）和特殊键如
  Clear/Suspend/Undo（gVim 等 GUI 模拟终端使用）。

### 3.1 内部 buffer 模型

libtermkey 维护一个内部 byte buffer (`tk->buffer`)，数据通过
`push_bytes` 拷贝进入。buffer 中有三个关键索引:

```
tk->buffer:  [...consumed...][...unconsumed...][...free...]
                              ^                ^
                         buffstart         buffstart+buffcount
```

- `buffstart`: 未消费数据的起始偏移
- `buffcount`: 未消费数据的字节数
- `buffsize`: 总容量

`peekkey` 通过 `CHARAT(i)` 宏（即 `tk->buffer[tk->buffstart + i]`）读取。
`eat_bytes` 消费 n 字节: `buffstart += n; buffcount -= n`。
buffer 首部消耗过半时触发 `memcpy` 滑动窗口。

### 3.2 解析调用链

```
termkey_getkey / termkey_getkey_force
  └─ peekkey (force=0 or 1)
       ├─ driver_ti.peekkey_ti  → trie 匹配 terminfo 序列
       ├─ driver_csi.peekkey_csi → 硬编码 CSI/SS3 序列
       ├─ 任一驱动返回 KEY/EOF/ERROR → 立即返回
       ├─ 任意驱动返回 AGAIN (force=0) → 记录, 继续下一驱动
       └─ 所有驱返回 NONE → peekkey_simple (UTF-8/C0/ESC raw 处理)
```

驱动顺序: driver_ti → driver_csi。ti 优先匹配 terminfo 自定义序列，
csi 处理标准 CSI/SS3。最后一个驱动返回 `KEY` 则消费。

### 3.3 AGAIN 语义

单个 `\e` 和 不完整 CSI (`\e[2`) **都返回 AGAIN**，无法从返回值区分。

| buffer 内容 | driver_ti | driver_csi | peekkey_simple | 结果 |
|------------|-----------|-----------|----------------|------|
| `\e`       | AGAIN(1字节, trie 有前缀) | NONE | AGAIN(ESC 可能是前缀) | AGAIN |
| `\e[2`     | NONE | AGAIN(CSI 参数未终结) | (不到) | AGAIN |
| `\eO`      | AGAIN(SS3 等命令字) | NONE | (不到) | AGAIN |

AGAIN 后的处理:
- `getkey` (non-force): 不消费字节，返回 AGAIN。同时调 `peekkey(force=1)` 预热
  key 但不消费（让调用方看到"force 时会得到什么"）
- `getkey_force`: 调 `peekkey(force=1)`，有 KEY 则消费之
- `waitkey`: poll(fd, waittime) → 超时则调 `getkey_force`

`force=1` 时各驱动的行为:
- driver_ti: trie 部分匹配 → 仍然是 NONE（不强制输出）
- driver_csi: CSI 部分匹配 → 仍然是 NONE
- peekkey_simple: 孤 `\e` → 输出 ESC 键；孤 UTF-8 续字节 → 输出非法码点

### 3.4 hightide 机制

`peekkey` 中 driver_ti 匹配到某个 terminfo 键（如 `\e[24~`），但
之后 driver_csi 也匹配到了更早的终止点。为了防止 driver_csi 覆盖
driver_ti 的结果，用 `hightide` 记录"已找到匹配但后续数据可能
让另一驱动重新解释"。

具体: driver_ti 找到 trie node 是 `TYPE_KEY` 时，记录当前位置为
`hightide`。driver_csi 后若也返回 KEY，比较 `nbytes`。如果 csi
的 nbytes 更短，退回到 ti 的结果。

实际上 Neovim 的写法更直接——它把 `hightide` 放在 `peekkey` 开头处理：
```c
if (tk->hightide) {
    tk->buffstart += tk->hightide;
    tk->buffcount -= tk->hightide;
    tk->hightide = 0;
}
```
即"跳过上次被 hightide 标记的数据"。

### 3.5 Neovim 的实际用法

Neovim 删除了 `waitkey` 和 `advisereadable`，I/O 走 libuv。

**数据流** (`input.c`):
```
libuv read callback
  → handle_raw_buffer
    → termkey_push_bytes(input->tk, ptr, to_use)
    → tk_getkeys(input, false)
```

**tk_getkeys 流程** (`input.c:439-498`):
```c
while ((result = tk_getkey(input->tk, &key, force)) == TERMKEY_RES_KEY) {
    // 处理 key...
}
if (result != TERMKEY_RES_AGAIN) return;
// AGAIN: 部分序列在 buffer 中, 设 timer 等更多数据
if (input->ttimeout && input->ttimeoutlen >= 0) {
    uv_timer_start(&input->timer_handle, tinput_timer_cb, ttimeoutlen, 0);
} else {
    tk_getkeys(input, true);  // 不设 timer，立即 force
}
```

**timer 回调** (`input.c:488-498`):
```c
static void tinput_timer_cb(uv_timer_t *handle) {
    // （先处理可能的新 raw buffer 数据）
    tk_getkeys(input, true);  // force 模式，超时了给结果
    tinput_flush(input);
}
```

**force 使用场景**:
1. `ttimeout` 启用且 `ttimeoutlen >= 0`: timer 超时后 force
2. `ttimeout` 禁用: 立即 force

force 的作用: 区分"ESC 单独按下"和"ESC 是更长序列的前缀"——
等待 ttimeoutlen 后仍无新数据，则将 ESC/不完整序列强制输出。

### 3.6 需要保留的 API（功能完整替换）

对比 Neovim 删除和保留的 API，termfeed 需要覆盖:

| API | Neovim 保留? | termfeed 需要? | 说明 |
|-----|-------------|---------------|------|
| `termkey_new_abstract` | 是 | 是 | 构造器 |
| `termkey_free`/`destroy` | 是 | 是 | 析构 |
| `termkey_getkey` | 是 | 是 | 非 force 解析 |
| `termkey_getkey_force` | 是 | 是 | force 解析 |
| `termkey_waitkey` | 否(删除) | 是 | 阻塞等待(termfeed 独立使用需要) |
| `termkey_push_bytes` | 是 | 否 | 替换为 reader 适配 |
| `termkey_advisereadable` | 否(删除) | 否 | I/O 层 |
| `termkey_set_flags`/`get_flags` | 是 | 是 | flag 管理 |
| `termkey_get_canonflags`/`set_canonflags` | 是 | 是 | canon 管理 |
| `termkey_get_buffer_size`/`set_buffer_size` | 是 | 是(?) | buffer 大小，reader 模型下可能不需要 |
| `termkey_strfkey` | 是 | 是 | Key→字符串 |
| `termkey_strpkey` | 否(删除) | 是 | 字符串→Key (完整替换需要) |
| `termkey_keycmp` | 否(删除) | 是 | Key 比较 |
| `termkey_keyname2sym`/`lookup_keyname` | 否(删除) | 是 | name 查找 |
| `termkey_register_keyname`/`get_keyname` | 是 | 是 | name 注册/查询 |
| `interpret_mouse/position/modereport` | 是 | 是 | 解释函数 |
| `interpret_string` | 是 | 是 | 控制字符串 |
| `termkey_canonicalise` | 是 | 是 | 规范化 |
| `hook_terminfo_getstr` | 是 | 是 | terminfo 回调 |
| `start`/`stop`/`is_started` | 是 | 是(?) | driver 生命周期 |
| `get_fd` | 否(删除) | 否 | I/O 层 |

### 3.7 Neovim 中运行时序列注册的使用情况

Neovim 仅使用静态的 `register_keys()` (driver-csi 内置键表初始化)
和 `termkey_register_keyname` (keynames 数组初始化)。无运行时注册
自定义字节序列的接口。因此 termfeed 不提供运行时序列注册。

## 四、kitty keyboard protocol 调研

kitty keyboard protocol 是一个渐进增强的键盘输入协议，分为五个增强
级别，通过 DCS/push/CSI u 序列在终端和程序间协商启用。

来源: https://sw.kovidgoyal.net/kitty/keyboard-protocol/

### 4.1 增强级别

| 级别 | flag | 含义 |
|------|------|------|
| 0 | — | 基础: 标准 CSI/SS3 + legacy 编码 |
| 1 | `0b1` | **Disambiguate**: 区分 `\e` + letter vs Alt+letter, Ctrl+letter vs CSI 前缀, 等 |
| 2 | `0b10` | **Report event types**: 为所有键附加 press/repeat/release 事件 |
| 3 | `0b100` | **Report alternate keys**: Shift 视为 modifier, 非 printable 键用 CSI u 替代 legacy |
| 4 | `0b1000` | **All keys as escape codes**: 所有 printable 键用 CSI u 编码，替代 bare UTF-8 |

级别 1-3 启用时，CSI u 编码形式: `\e[codepoint;modifiers;event u`
级别 4 启用时格式: `\e[codepoint;modifiers;event;text u`

### 4.2 CSI u 编码格式

```
\e [ codepoint ; modifiers ; event ; text u
```

- `codepoint`: Unicode 码点 (0..10FFFF)
- `modifiers`: 1+bitmask (Shift=1, Alt=2, Ctrl=4, Super/Hyper/Meta/NumLock/...=8..128)
- `event`: 1=press, 2=repeat, 3=release (仅 flag 0b10 启用时)
- `text`: UTF-8 码点文本 (仅 flag 0b1000 启用时，用于 dead key 组合)

modifier bitmask 还支持 CapsLock=256, NumLock=512, 且 Shift 在 printable 键中默认不报告
（协议用 `u`/`~` 开关区分）。通过 `~` 替代 `u` 结尾表示"Shift 视为 modifier"。

### 4.3 Disambiguate (级别 1) 解决的关键冲突

| 输入 | 无协议时编码 | Disambiguate 后 |
|------|------------|----------------|
| Ctrl+I | `0x09` (Tab) | `\e[105;5u` (CSI u: codepoint='i'=105, Ctrl=5) |
| Ctrl+[ | `0x1b` (Escape) | `\e[91;5u` |
| Ctrl+M | `0x0d` (Enter) | `\e[109;5u` |
| Ctrl+Shift+I | `0x09` (Tab) | `\e[73;5u` (codepoint='I'=73) |
| Alt+Enter | `\e` + `0x0d` | `\e[13;3u` (Alt=3) |
| Alt+letter | `\e` + UTF-8 byte (歧义) | `\e[codepoint;3u` |
| 孤 ESC 键 | `\e` (需超时判定) | `\e[27;1u` 或 level 4: `\e[27;1~` |

**结论: 级别 1 消除了大部分 ESC 超时需求**。ESC 键本身被编码为 `\e[27;1u`，不再依赖超时区分。

### 4.4 非 Unicode 键的编码

kitty 定义了功能键名称空间 (KITTY_FUNCTION_KEYS，如 CapsLock=57358, NumLock=57359 等)，
使用 CSI u 的 codepoint > 57344 (0xE000) 的私有区域。

libtermkey 使用 `TERMKEY_TYPE_FUNCTION` + `key->code.number` 编码功能键。

### 4.5 与 libtermkey/Neovim 的兼容性

Neovim vendor libtermkey 已支持 kitty 协议的核心部分:

| kitty 特性 | Neovim libtermkey 支持? | 说明 |
|-----------|----------------------|------|
| CSI u 基本格式 (`codepoint;modifiers u`) | ✓ | `handle_csi_u` 解析 modifiers bitmask |
| 事件类型 (press/repeat/release) | ✓ | `parse_key_event` 解析子参数 `:3` |
| 子参数 `:` 分隔 | ✓ | `TermKeyCsiParam` 的 subparam 支持 |
| Shift 作为 modifier (CSI u 中 modifiers 含 Shift) | ✓ | modifier bitmask 完整支持 |
| `~` vs `u` 复合按键 | ✓ | `handle_csi_u` 检查 terminator byte |
| Disambiguate (Ctrl+letter, Alt+letter 等) | ✓ | kitty 启用时由终端编码，parser 被动接收 |
| All keys as escape codes (级别 4) | ? | 理论兼容 — 只要 codepoint 合法就解析 |
| 报告关联文本 (text field) | 否 | Neovim 未实现 |

**termfeed 需要补充支持的内容**:

1. **级别 4 text 字段**: `\e[codepoint;modifiers;event;text u`, 解析 text 为辅助信息
2. **功能键名称空间**: 57344+ 的 codepoint 按 kitty 定义映射到键名
3. **完整事件流**: 确保 press/repeat/release 三态对 `tf_Event` 的映射正确

### 4.6 fixterms 对比

kitty 协议文档有一节 **"Bugs in fixterms"**，列出 fixterms 的问题:

1. fixterms 设计为**替代**现有编码，而非**扩展** — 默认 behavior 会发 `CSI ... ~` 替代传统的 `CSI A/B/C/D`（光标键），**破坏向后兼容**
2. fixterms 建议用 8-bit CSI (0x9b) 替代 7-bit (`\e[`)，"reserve 0x1b for Escape key" — 但这严重破坏已有协议（tmux/screen/shell 都期望 7-bit CSI）
3. fixterms 要求 Shift 在所有 printable 键中都报告为 modifier — 但大多数场景不需要区分 `A` 和 `Shift+A`（shift 是获得大写的必要操作）
4. fixterms 的 `CSI ~` modifier 参数含义不清晰（`;1`=无修饰 vs kitty 的 `;1`=无修饰 对齐）

**kitty 对 fixterms 的核心批评**: fixterms 默认行为改变太激进，kitty 采用**渐进增强**——未协商时不改变任何现有行为。termfeed 应遵循此策略。

### 4.7 termfeed 的 kitty 支持建议

1. **CSI u 编码完全支持** — 已有，对齐 Neovim libtermkey 的 `handle_csi_u`
2. **事件类型 (press/repeat/release)** — 已有，通过 `tf_Event` 输出
3. **级别 4 text 字段** — 新增，在 `tf_Key` 的 `utf8[7]` 或单独字段反映
4. **功能键名称空间 (>57344)** — map 到 `tf_Sym` 或 `TF_TYPE_FUNCTION`
5. **不主动发送协议协商** — 这是终端控制层面的事，termfeed 只解析输入
6. **向后兼容 legacy 编码** — 即使 kitty 协议启用，terminal 仍可能按 legacy 发送某些键；CSI/SS3/legacy 解析器全部保留
