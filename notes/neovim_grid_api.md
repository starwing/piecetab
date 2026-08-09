# Neovim Grid API

## 1. 核心数据结构

### `ScreenGrid` (grid_defs.h:44-100)

每个 ScreenGrid 代表一个矩形屏幕区域。内部存储三块平行数组 + line_offset 间接寻址：

| 字段 | 类型 | 用途 |
|------|------|------|
| `chars[]` | `schar_T` (uint32_t) | 每个 cell 的 UTF-8 内容（≤4 字节嵌入，>4 走 glyph_cache） |
| `attrs[]` | `sattr_T` (int32_t) | 每个 cell 的 highlight attribute id |
| `vcols[]` | `colnr_T` | 每个 cell 对应的 buffer virtual column |
| `line_offset[]` | `size_t*` | 行起点索引（通过旋转 offset 实现快速滚动，不需拷贝数据） |

**关键设计：**
- 双宽字符：左 cell 存字符，右 cell 存空串 (`0`)
- 全屏滚动通过旋转 `line_offset[]` 实现（`grid_ins_lines` / `grid_del_lines`），而非 memmove 整块数据
- `throttled` 标志：只内部绘制，不向 UI 层发送更新（用于 msg_grid 等场景）
- `comp_*` 字段：属于 compositor 子系统，外部不应读写

### `GridView` (grid_defs.h:107-111)

```c
typedef struct {
  ScreenGrid *target;  // 实际绘制的 ScreenGrid
  int row_offset;
  int col_offset;
} GridView;
```

视图封装：`grid_adjust()` 将视口坐标转换为 target grid 的绝对坐标。多 grid 模式（`kUIMultigrid`）下每个窗口有独立 `ScreenGrid`，单 grid 模式下所有窗口共享 `default_grid`。

---

## 2. Grid API 完整清单

### 2.1 生命周期管理

| API | 文件:行 | 职责 | 调用方 |
|-----|---------|------|--------|
| `grid_alloc()` | grid.c:842 | 分配 rows×cols 的 cells，支持 resize 时从旧 grid copy | `win_grid_alloc()` |
| `grid_free()` | grid.c:899 | 释放所有内存 | `win_grid_alloc()` / `free_all_mem()` |
| `grid_assign_handle()` | grid.c:993 | 分配唯一 handle ID | 创建 grid 时 |
| `grid_invalidate()` | grid.c:321 | 将所有 attrs 设为 -1（强制下次全量重绘） | resize/配置变更 |
| `grid_clear_line()` | grid.c:311 | 用空格填充特定行范围 | scroll 操作内部 |

### 2.2 行级绘制（linebuf 批处理）

这是 grid 的核心绘制模式：先批量填充 linebuf，然后一次性 diff + flush。

| API | 文件:行 | 职责 |
|-----|---------|------|
| `grid_line_start()` | grid.c:363 | 开始一行绘制，设置全局 linebuf 状态 |
| `grid_line_puts()` | grid.c:439 | 写入 UTF-8 文本（处理多字节/宽字符）到 linebuf |
| `grid_line_put_schar()` | grid.c:416 | 写入单个 schar 到 linebuf |
| `grid_line_fill()` | grid.c:504 | 用重复字符填充 linebuf 范围 |
| `grid_line_clear_end()` | grid.c:524 | 标记需要清除的行尾范围（不写入 linebuf，而是记录清除参数） |
| `grid_line_getchar()` | grid.c:401 | 读取当前已显示在屏幕上的字符（用于字符替换判断） |
| `grid_line_mirror()` | grid.c:541 | 将 linebuf 内容左右镜像（`rightleft` 模式） |
| `grid_line_cursor_goto()` | grid.c:536 | 设置光标位置 |
| `grid_line_flush()` | grid.c:590 | 将 linebuf 与 grid 做 diff，调用 `grid_put_linebuf()` 发送最小更新 |
| `grid_line_flush_if_valid_row()` | grid.c:608 | 安全性 flush（避免因无效 row 崩溃） |

**绘制流程：**
```
grid_line_start() → 多次 grid_line_puts() / grid_line_put_schar()
                  → 可选 grid_line_clear_end() / grid_line_mirror()
                  → grid_line_flush()
```

### 2.3 滚动操作

| API | 文件:行 | 职责 |
|-----|---------|------|
| `grid_ins_lines()` | grid.c:1009 | 插入 line_count 行（通过旋转 line_offset），发送 `grid_scroll` UI 事件 |
| `grid_del_lines()` | grid.c:1049 | 删除 line_count 行（旋转 line_offset），发送 `grid_scroll` UI 事件 |

**优化：** 当 width = grid->cols（全宽）时，只旋转 line_offset 指针数组；
当 width < grid->cols（部分宽）时，用 memmove 按行拷贝。

### 2.4 杂项

| API | 文件:行 | 职责 |
|-----|---------|------|
| `grid_clear()` | grid.c:621 | 用 `grid_line_start()` + `grid_line_clear_end()` + `grid_line_flush()` 清空矩形区域 |
| `grid_getchar()` | grid.c:334 | 读取 grid 中指定位置的一个字符 |
| `grid_adjust()` | grid.c:65 | GridView → 目标 grid 坐标转换 |
| `grid_draw_border()` | grid.c:1141 | 绘制浮动窗口边框（使用 grid_line 原语） |
| `win_grid_alloc()` | grid.c:932 | 分配/调整窗口 grid，根据 multigrid 模式选择 target |

### 2.5 schar 工具函数

| API | 文件:行 | 职责 |
|-----|---------|------|
| `schar_from_str()` | grid.c:72 | 从 C 字符串创建 schar_T |
| `schar_from_buf()` | grid.c:83 | 从 buffer + 长度创建 schar_T（≤4 字节内联，>4 入 glyph_cache） |
| `schar_from_char()` | grid.c:1239 | 从 Unicode codepoint 创建 schar_T |
| `schar_get()` | grid.c:150 | schar_T → C 字符串（设 NUL 终止） |
| `schar_get_adv()` | grid.c:158 | schar_T → 字符串（不设 NUL，推进指针） |
| `schar_len()` | grid.c:174 | 获取 schar 的字节长度 |
| `schar_cells()` | grid.c:185 | 获取 schar 占用的显示列数（委托给 `utf_ptr2cells()`） |
| `schar_get_ascii()` | grid.c:218 | 若为 ASCII 返回字符，否则返回 NUL |
| `schar_cache_clear_if_full()` | grid.c:110 | 当 glyph_cache 超过阈值时清空 |
| `schar_cache_clear()` | grid.c:121 | 清空 glyph_cache |

---

## 3. 字符/编码处理分工

### 3.1 由声调方 (caller) 负责

| 职责 | 说明 |
|------|------|
| 文本断字 | caller 需要正确传递整个字符序列，grid_line_puts 内部做多字节解析 |
| 颜色/highlight 选择 | caller 传递 attr id，grid 层只做存储和比较 |
| 行号/Vcol 计算 | caller 在 linebuf_vcol 数组中填入值（grid_put_linebuf 原样写入 grid） |
| 虚拟文本拼接 | drawline.c 中由 win_line() 完成文本组合后调用 grid_line_puts |
| 决定何时开始/结束一行 | caller 必须配对 grid_line_start / grid_line_flush |
| 决定清空范围 | caller 通过 grid_line_clear_end 标记 |
| window 布局/位置 | grid 层不关心窗口叠放关系 |

### 3.2 由 grid 层负责

| 职责 | 说明 |
|------|------|
| UTF-8 多字节解析 | `grid_line_puts` 内部调用 `utfc_ptr2len` 逐字符解析 |
| 宽字符 (CJK/emoji) 处理 | 通过 `utf_ptr2cells()` 检测双宽，右 cell 填 0 |
| 截断处理 | 行末只剩 1 列但下个字符需要 2 列时会显示 `>` |
| 双宽覆盖保护 | 写单宽字符覆盖双宽字符右半时，自动将旧左半变 `>` |
| diff 最小化 | `grid_put_linebuf()` 逐 cell 比较，只发送变化部分 |
| 阿拉伯文 shaping | `line_do_arabic_shape()` 处理 `p_arshape` |
| 行镜像 | `grid_line_mirror()` 处理 `rightleft` |
| glyph_cache | 超过 4 字节的字符通过 hash 表缓存 |
| 滚动优化 | 通过 line_offset 旋转避免数据拷贝 |
| blending | `thru` 透明检测和 hl_blend_attrs 调用 |

### 3.3 多字节/宽度检测函数 (mbyte.c)

| API | 文件:行 | 职责 |
|-----|---------|------|
| `utf_ptr2len()` | mbyte.c:918 | 单 codepoint 字节长度 |
| `utf_ptr2len_len()` | mbyte.c:948 | 带长度限制的单 codepoint 字节长度 |
| `utfc_ptr2len()` | mbyte.c:972 | 包含 composing chars 的完整字符字节长度 |
| `utfc_ptr2len_len()` | mbyte.c:1010 | 带长度限制的完整字符字节长度 |
| `utf_ptr2cells()` | mbyte.c:494 | 指针处字符的显示宽度 |
| `utf_ptr2cells_len()` | mbyte.c:594 | 带长度限制的显示宽度 |
| `utf_char2cells()` | mbyte.c:457 | codepoint → 显示宽度（核心：查 utf8proc、ambw、emoji） |
| `utf_char2bytes()` | — | codepoint → UTF-8 编码 |
| `mb_string2cells()` | mbyte.c:630 | 整个字符串的显示宽度 |
| `utfc_ptrlen2schar()` | mbyte.c:879 | 已知长度 UTF-8 → schar_T + first codepoint |

**宽度判断规则 (utf_char2cells)：**
1. ASCII (< 0x80) → 1
2. 不可打印 → 4 (xx) 或 6 (<xxxx>)
3. utf8proc charwidth == 2 → 2（CJK 等）
4. `p_ambw == 'double'` + ambiguous_width → 2
5. `p_emoji` + emoji-like + ≥ 0x1F000 → 2
6. 其他 → 1

---

## 4. Diff 机制

### 4.1 行级 diff（grid_put_linebuf）

核心函数 `grid_put_linebuf()` (grid.c:667) 不依赖脏标记或区域追踪，而是逐 cell 比较：

```
linebuf_char[col] vs grid->chars[off]   → 内容不同？
linebuf_attr[col] vs grid->attrs[off]   → 属性不同？
linebuf_char[col+1] == 0 且 != grid->chars[off+1]  → 双宽字符右半不同？

任一不同 → grid_char_needs_redraw() 返回 true
```

优化点：
- `start_dirty` / `end_dirty` 追踪连续脏区间，合并相邻变化为一次 `ui_line()` 调用
- 清除操作（空格填充）单独追踪为 `clear_dirty_start` / `clear_end`
- 当清除区域和绘制区域有缝隙时（`clear_dirty_start >= start_dirty - 5`），合并为一次调用
- 当 `kOptRdbFlagNodelta` 设置时，无条件全量重绘

### 4.2 选择性区域输出

`ui_line()` (ui.c:468) 接收 `startcol`、`endcol`、`clearcol` 三个坐标：
- `[startcol, endcol)` — 实际有内容的区域
- `[endcol, clearcol)` — 需要清空的区域（填充背景色）

这允许一次调用同时表示"绘制 A 并清除 B"。

### 4.3 远程 UI 的 grid_line 转换

`remote_ui_raw_line()` (api/ui.c:750) 将内部 `raw_line` 转换为 msgpack 格式的 `grid_line`：
1. 遍历 cells，合并相邻的相同内容+高亮为 run-length 编码
2. 每个元素：`[text, hl_id?, repeat?]`
3. 当 buffer 接近满时主动拆分事件（`ncells_pending >= 500`）
4. 空格序列会被省略（单独用 clearcol 处理）

### 4.4 compositor 层 diff

`ui_compositor.c` 中的 `compose_line()` 负责将多层 grid（浮动窗口、popupmenu 等）合成到 default_grid：
1. 按 `comp_index` 从下到上遍历 layers
2. 取最上层可见 grid 的字符
3. `blending` 属性触发 `hl_blend_attrs()` 半透明混合
4. 双宽字符被遮挡的处理（可见半宽 → 空格）
5. 如果当前 grid 被上层完全覆盖（`curgrid_covered_above()`），跳过原始 `raw_line`，强制走合成路径

---

## 5. Grid 隐藏的底层细节

调用方不需要关心的实现细节：

| 隐藏细节 | 说明 |
|----------|------|
| **glyph_cache** | >4 字节字符通过全局 hash 表缓存，清空时会通过 `UPD_CLEAR` 强制全量重绘 |
| **line_offset 旋转** | 滚动不拷贝数据，只旋转指针数组 |
| **rc 半透明混合** | `winblend`/`pumblend` 在 compositor 层处理 |
| **msgpack 编码** | `remote_ui_raw_line` 做 text/hl/repeat 压缩，内部不感知 |
| **dual-grid 模式** | `win_grid_alloc` 根据 `kUIMultigrid` 自动切换 target |
| **exmode_active hack** | grid_char_needs_redraw 中特殊处理 exmode（标记为 TODO） |
| **throttled 模式** | msg_grid 先内部绘制不发送 UI 事件，到 flush 时再发送 |
| **Arab shaping** | `line_do_arabic_shape` 在 grid_put_linebuf 内部透明完成 |
| **双宽字符右半** | 始终存 0，比较时特殊处理 |

## 6. UI 事件协议 (msgpack)

### 主要 grid 事件

| 事件 | 参数 | 说明 |
|------|------|------|
| `grid_resize` | grid, width, height | 网格尺寸变化 |
| `grid_clear` | grid | 清空整个网格 |
| `grid_cursor_goto` | grid, row, col | 设置光标 |
| `grid_line` | grid, row, col_start, cells[], wrap | 绘制一行（run-length 编码） |
| `grid_scroll` | grid, top, bot, left, right, rows, cols | 滚动区域 |
| `grid_destroy` | grid | 销毁网格 |
| `raw_line` | grid, row, startcol, endcol, clearcol, clearattr, flags, chunk, attrs | 内部密集格式（不导出给 remote UI） |
| `win_pos` | grid, window, startrow, startcol, width, height | 窗口位置 |
| `win_float_pos` | grid, window, anchor, ... | 浮动窗口位置 |
| `msg_set_pos` | grid, row, scrolled, sep_char, zindex, compindex | 消息区域位置 |

## 7. 整体架构分层

```
┌─────────────────────────────────────────┐
│  drawline.c / win_update()              │ ← 文本准备、高亮、组合
│  → 填充 linebuf_char/linebuf_attr       │
├─────────────────────────────────────────┤
│  grid.c: grid_line_puts / grid_put_linebuf│ ← diff、滚动优化、Arab shaping
│  → 更新 ScreenGrid.chars/attrs          │
├─────────────────────────────────────────┤
│  ui.c: ui_line()                        │ ← 发送 raw_line 事件
├─────────────────┬───────────────────────┤
│  ui_compositor   │  api/ui.c (remote)    │
│  → 多层合成      │  → msgpack 编码       │
│  → blend/blend   │  → run-length 压缩    │
├─────────────────┴───────────────────────┤
│  tui/tui.c (TUI)  │  external UI client  │
│  → ugrid 绘制      │  → 任意前端          │
└─────────────────────────────────────────┘
```
