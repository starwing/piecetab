# 代码精炼要诀

本文件记录 piecetab / termfeed 代码精炼（refine）实验中的核心经验。

## 核心信条

**精炼不是为了减行数，而是为了减杂质。** 25行限制是工具，不是目标。为了凑行数引入 flag、拆分函数、或制造短路逻辑，都属于庸工。改完若不如改前清晰，就是倒退。

## 七大原则

### 1. 自然数据流 > 人工 flag

| 坏 | 好 |
|---|---|
| 先设 flag 表示状态，后面再检查 | 计算一个值，让数据本身告诉你答案 |

**例：`ptD_mergeleaf`**（坏：预制 merged 再核对；好：`d` 的值自然
告诉你部分/完全合并，无需额外检查"空间够不够"）：

```c
// 坏                             // 好
if (可以合并) merged = 1;         if (左右都是 hole) d = pt_min(右大小, 左剩余空间);
else if (左右都是 hole) {         ptH_append(左, d);
    ...; merged = 1;              if (d < 右大小) ptH_remove(右, d);
}                                 else ptP_free(右), merged = 1;
if (!merged) { /* 不合并路径 */ }
```

**注意：** `merged` 仍然是必要的——它记录"是否合并了整片右叶"，
这是数据流无法直接回答的布尔问题。关键区别：`merged` 是**计算的
结论**，不是**预设的标记**。

### 2. 先计算，后判断

把复杂条件中的公共子表达式提到前面，让判断变得简短。

```c
// 繁
if (!ptM_ishole(p, cc-1) && !ptM_ishole(rt, 0) && ...)

// 简
hL = ptM_ishole(p, cc-1), hR = ptM_ishole(rt, 0);
if (!hL && !hR && ...)
```

### 3. 用宏，不用 cast

```c
// 坏
(const char *)p->data
(const char *)ptN_hole(rt, 0)->data

// 好
ptN_lit(p, cc-1)
ptN_hole(rt, 0)->data
```

宏不仅短，而且语义自明。

### 4. 统一 > 特例

不要为边界情况写单独的分支。把它融进主循环。

**例：`ptC_freeze` 的 `levels==0`**：坏 = 单独 `if (levels == 0) return;`
分支；好 = 主循环天然处理：

```c
for (l = tree->levels; l > 0;) {
    --l;
    if (l >= tree->levels) break;  // levels==0 时立即 break
    ...
}
```

（同类：不可达的 switch `default:` 消除，越界兜底改主路径后单行 if——§34 实例）

### 5. 逗号表达式替代单行块

纯赋值的单行 `if` 块用 `,` 取代 `{ }`；条件内赋值带错误传播：
`if (del > 0 && (r = pt_remove(C, del)) != PT_OK) return r;`
（同类：§12 赋值连句、§13 return 提交语义）

### 6. 变量少即是多

删掉只出现一次的栈变量（直接用函数调用，或声明在 for 里）——但**不要
重复计算有副作用的表达式**：函数无副作用且编译器能优化时，重复调用比
引入临时变量更清晰。

### 7. 命名里藏领域

```c
ptK_cowedit  → ptD_cowpaths  // 它是删除流程中的 COW 路径分裂，不是通用工具
de            → d              // 简称够用时就不需要全称
```

函数名反映分类（D=删除, K=光标操作）而非机制（cow）。

## 四种浪费

| 浪费 | 典型表现 | 代价 |
|---|---|---|
| **预制 flag** | 先设 flag → 后面再检查 | 阅读时需要跟踪 flag 状态机 |
| **特例分支** | `if (levels == 0)` 提前 return | 两条路径理解成本翻倍 |
| **多余变量** | `levels` 声明在函数开头，函数后半节不出现 | 心理负担 |
| **大块分支** | `if { r=...; if(r!=OK) return r; }` | 占用屏幕行 |
| **重复注释** | 同一函数上方两行相同说明 | 编辑残留，误导读者以为有区别 |

## 精髓一句话

**计算自然的值，让值引导决策。不要预设答案（flag），然后去核对。**

## demo 集成教训（spantree 退场复盘，2026-08-15）

- **集成 C 库前先做生产者分类**：视口纯函数（每帧可重算：hl/search）、
  版本化快照（整体替换：LSP sem/diag）、标记即忘（写一次/漂移/删即
  弃：插件痕迹/外部工具标记）——库语义必须与生产者类型对位，硬塞 =
  反模式（如每帧 clear+refill 把树当帧缓存，持久/漂移/合成全闲置）。
- 反模式信号：消费端每帧对库做"清空+重写"= 库选错了或用法错了。
- 否定需求调研要重证据：arb(0,0)/arb(x,0)/删除通知曾以"机械层已有"
  轻率否定——用户问的是上一层（全周期生命周期管理），只验机械层
  覆盖即下"不需要"结论 = 草率。正确路径：先推消费者语义，再看库
  缺什么。
- 集成实验退场也是产出：库+绑定全测保留、demo 退场、结论入 TODO
  储备条目——"需要才上"不是空话；上了发现不需要就退，文档留痕。

## termfeed 沉淀（stb 单文件库通用风格）

### 11. 循环递增一律 ++i

前缀递增是全库铁律（piecetab.h 全部 ++i）。后置 `i++` 仅当递增表达式的值
被使用。termfeed 曾混用 `i++`，已统一。`out[j++] = c` 这类"写入并前进"
的复合操作可保留后置——它是复合语义，不是纯递增。

### 12. 同类赋值用逗号表达式组织

同一逻辑单元（同类字段、同一 category）的赋值连成一句：

```c
key->type = TF_TYPE_UNICODE, key->event = TF_EVENT_PRESS;
key->modifiers = 0;
if (b <= 0x7F)
    key->d.codepoint = b, key->utf8[0] = (char)b, key->utf8[1] = '\0';
else
    key->d.codepoint = 0xFFFD, memcpy(key->utf8, "\xEF\xBF\xBD", 4);
```

多行赋值暗示"独立步骤"，逗号表达"一起发生"（原子修改的语义指引），
且省行数。散开写仅当赋值之间夹着计算/判断。

### 13. 函数尽量带返回值；裸 return 只留单语句

返回值 = 成功/失败（TF_OK/AGAIN/ERRMEM...），"提交语义"（状态复位、
计数更新）用逗号缩到 return 后：

```c
// 繁：6 行
if (b == 0x07 || b == 0x9c) { tfD_cskey(S, key); return TF_OK; }
// 简：1 行
if (b == 0x07 || b == 0x9c) return tfD_cskey(S, key), TF_OK;
```

**提交语句主动用——动机不是缩行**（虽然大多顺带缩行），而是**视觉语义**：
`return expr, 状态;` 表明"我这个 return 就是为了做 expr 这件事"——读代码
时一眼看到 return 的意图。块状裸 return（`if (x) { stmt; ...; return; }`）
同样用提交语义收编为单语句或 if/else 链（`tfC_kitty` 的 `cp<0` 块、
`kittyfind` 块均如此）。单语句 `if (x) return;` 不算裸 return——递归基例、
OOM 尽力而为、API 参数校验等天然形态可保留，不强行改。

**但单 return 优先于提交语义**：若能调整代码逻辑使函数**只有一个
return**（值在最后统一返回），那比提交语义更强——提交语义仍是"多出口"
（多出口是必要的才用，单出口是更好的设计）。

### 14. 少用 flag，用自然逻辑拆分 if

flag 只带 1 bit 信息，降低信息密度。同类判断各自提前 return：

```c
// 坏：term 预置标记，后面再查
term = (b == 0x07 || b == 0x9c);
if (!term && b == 0x5c && ...) term = 1;
if (term) ...
// 好：每个终止条件直接 return
if (b == 0x07 || b == 0x9c) return tfD_cskey(S, key), TF_OK;
if (b == 0x5c && ...) return tfD_cskey(S, key), TF_OK;
```

### 15. 重复分支问"同一数据的不同来源？"

三个函数共享 code 解码 → 抽参数化 helper，入口各剩 2-4 行：

```c
static void tfM_fill(tf_Key *key, int code, int col, int line, int rel);
static void tfM_decodeX10(tf_Key *key, const char *raw) { /* raw 解坐标 */ }
static void tfM_csi(tf_State *S, tf_Key *key, int rel) { /* 参数解坐标 */ }
```

SS3 光标键复用 CSI 的 `tfC_cursorkey`；F1-F4 用 `cmd - 'P' + 1` 公式。
展开的 if/switch 先问"这是同一数据的不同来源吗"——是则合并，否则保持
（如 `tfC_nargs` 与 `tfC_field` 的 intermediate 语义有微妙差异，判定
等价但保守不合并）。

### 16. bug 教训（termfeed 两例）

- **减法要数清缓冲边界**：ST 检测路径的当前字节（`\`）从未进 cs_buf，
  只应 `cs_len -= 1`。原 `-= 2` 多减 1 字节，且测试注释固化了错误期望
  （"ab" 而非 "abc"）——review 时要追问"为什么少一个字节"，别接受测试
  注释当真相。
- **贪婪解析要留退出路径**：modifier 名匹配允许 `next == '>'` 时，
  `<C-C>` 的第二个 C 被吃成 modifier，键名空 → 解析失败。modifier
  只在分隔符（`-`/空格）前成立，键名优先。

### 17. 测试期望会固化行为

测试注释断言"内容应为 X"时，先质疑 X 是否符合协议语义，再信实现。
多轮 review + 100% 覆盖率不保证语义正确——只保证行为稳定。

## termfeed 二次沉淀（格式对抗与命名）

### 18. 循环形态判定

- **有 init/cond/inc 完整三要素 → for**（如 `tfC_field` 的 field 定位）
- **纯"跳过到某条件"** → while 或 for+continue（`modget` 的分隔符跳过）
- **终止条件单一且可前置** → `while (cond)` 而非 `for(;;)`——循环条件在
  头部是信息密度；`for(;;)` 只留给多体内退出点的循环（`ptC_freeze` 式
  双 return，写不出单一前置条件）
- **`for(;;) { if (X) break; ... }` 中 X 明显可前置 → 必须写 while**：
  break 式骨架的退出条件藏在体内中部，读者要扫过整循环才知道何时
  退出；`while (!X)` 把退出语义提到头部。多退出点不构成豁免理由——
  体内另有 return 不影响主退出条件前置（sp_next 上升：`for(;;){ if
  (findslot < cc) break; ...}` → `while (findslot == cc) { ... }`，
  体内仍有 `--l < 0` return 早退）
- **`while (cond) { ...; update; }` 尾有循环变量更新 → 写
  `for (; cond; update) { ... }`**：update 进 for 头，与 cond 相邻，
  循环控制集中一处；体只剩业务逻辑（sp_next 下降：
  `while (++l <= levels) { p = ...; i = ...; paths = ...; }` →
  `for (; ++l <= levels; paths = ...) i = findslot(p = ..., ...)`）。
  **例外**：update 非纯循环推进（夹业务副作用）或体与 update 有
  长依赖时保持 while
- **空 for 体用 `continue` 不用 `;`**——clang-format 对空 for 体振荡

### 19. 工具宏命名与位置

- **命名**：全大写、不带命名空间字母（`TF_NAMEBUF_SZ` 而非 `tfK_NAMESZ`）
- **位置**：全项目通用的函数式宏集中放 implementation 前部（include 后、
  NS_BEGIN 前）；数据表宏（`TF_SYMS`/`TF_KITTYKEYS`）与常量宏留在绑定
  位——**只有全项目通用的才挪前部**
- 表元素计数宏一处定义全库复用，且让 `for` 条件保持单行（免 ntbl 变量）

### 20. 查表循环内避免运行时 strlen

`strlen(tfK_modtable[i].name)` 的指针来自运行时索引，编译器无法静态
折叠（-O2 展开+传播可能但不保证）。表字段存 `len`，初始化用
`sizeof(name) - 1`（编译期常量），宏包一层免重复写名字：

```c
#define TFK_MOD(name, mod) {(name), (int)(sizeof(name) - 1), (mod)}
```

### 21. 单行函数

clang-format 只拆不合：手写单行函数必须 clang-format off 保护。仅用于
真正简单（~90 字符内）的函数；长函数硬压单行反而伤可读性。
**off 只允许保护单行函数本体，禁止为覆盖率保护单行 if/表达式**
（覆盖率用 §30 的"先计算 + 单行 if + helper 共用"解决；off 注释带
附加文字（`/* clang-format off (说明) */`）会被 clang-format 静默忽略，
指令必须纯 `/* clang-format off */`）。

**单行函数两行式**（个人风格，off 保护）：实现单行但签名+实现合计超
80 列、分两行各不超时，写成

```c
int func(lua_State *L, const char *s)
{ /* implement ... */ }
```

签名一行 + `{ 实现 }` 一行——实现行的 `{ }` 自带语义（一眼看到函数
体范围），比"签名 + 缩进体 + 右括号"三行更紧凑。**只在实现真正简单
时用**（单表达式级别）；多语句/分支必须正常展开。

### 22. 对抗 clang-format 的三招（长条件/嵌套三元被拆坏时）

- **取反 if 条件**，让单行短分支在 if 侧而非 else（`if (!(fmt & F))`）
- **嵌套三元被拆成错位缩进** → "逐步覆盖"：先赋默认值，再按优先级
  从低到高覆盖（`tok = "A-"; if (LONGMOD) tok = "Alt-"; if (ALTISMETA) tok = "M-";`）
- **多条件 if 超 80 列被拆** → 预计算标志变量，if 恢复单行
  （`int up = A && B; if (up && C) ...`，C89 允许块内连续声明）

### 23. 写字符串用带截断 helper，不用 snprintf("%s")

同函数族的 `writes(buf, len, s)` 与 `snprintf("%s")` 完全等价（拷贝 +
截断 + NUL + 返回字节数），且免 size_t cast（`tfK_modput` 例）。

### 24. 函数头注释全删——先改实现，再谈注释

**函数头不写注释**（termfeed 全库已清）。删前先问"为什么写"，
按序处置:

1. **代码晦涩 → 改实现**（首要手段！注释是晦涩实现的下策）
2. **输入契约**（调用方必须满足）→ `assert`
3. **输出契约**（调用方依赖的返回/副作用语义）→ 函数体内注释
   （return 附近，不占函数头）
4. **"做什么"** → 写进 design 文档
5. **代码自明** → 直接删

**体内零散多行注释同款处置**：压成单行只留必要内容；不重要直接
删；长篇论证（顺序理由、预算推导、不变量）在 design 文档强调，
代码只留一行索引式短注（2026-08-16 spantree 全库清理：8 处多行 →
7 单行 + 1 删）。多行注释的存在本身是信号——先问"这信息值几行"。

**注释最容易与实现不同步**——design 文档里 `cs_len -= 2` 的 ST 检测
描述与代码 `-= 1` 长期不一致（brief_refine §16 早已记录此 bug，文档
却未同步）。改代码后必须同步 design 与精炼文档。

### 25. 性能先于简练——API 形态上杜绝 O(n²)

**简练清晰是为了更好理解最优化实现，不能本末倒置**。"看着顺眼"的接口
可能是性能陷阱：`tfC_arg(S, n, dflt)` 按索引从 buf 头重扫——调用方写成
`tfC_arg(S,1) + tfC_arg(S,2) + ...` 即 O(n²)，且该 API 形态**诱导**调用方
写出 O(n²) 代码。正解：参数区是一次性消费的序列 → 迭代器
`tfC_nextarg(S, &f, &len)`（f/len 构成 in/out 游标，内部已知长度跳过，
零重扫 O(n)）。**判定标准：每次调用是否"从头重扫"？同一数据是否被
重复定位？是 → 改成单次扫描/迭代器**（§9 均摊论证同源）。

迭代器形态三连坑（都踩过）：① 返回字段头指针 → f 按值传与 in/out plen
方向混杂，签名撒谎；最终 `int` + `const char **pf` 两 in/out 对称。
② 耗尽后 f=NULL 会**重启**（NULL 兼首调/耗尽双重语义）→ 耗尽置 f=end
幂等返回 0。③ 界面臃肿（v 数组+Args struct+dflt 契约）是抽象错误的信号
——首版 `tfC_scan` 全包而复杂，切回迭代器后 scan/Args 整个消失，净减 ~130 行。

### 26. 重构前先列"隐性行为清单"，靠全量测试当回归网

tfC_arg→tfC_scan 重构中，`v[2]` 缺字段未填 dflt 的 bug 被既有测试
（`\x1b[27;99~` → UNKNOWN_CSI）当场抓住。**隐性契约的保障不是头注释**
（头注释与实现不同步——design 里 `cs_len -= 2` 错一年就是例证），而是三级:

1. **消除**（最优先）——重构使约束成为自然数据流。如 nextarg 的
   "耗尽后不重启"被 `if (tfC_nextarg(...))` 循环模式天然免疫；"f1 恒存在"
   是 case 匹配的结构保证
2. **使用处/实现处注释**——约束绑定生效位置，改代码必然看到
3. **测试兜底**——只负责验证，不该是唯一保障

- 重构前从被删代码/注释提炼行为清单（缺字段→dflt、空字段→dflt、
  intermediate 截断、final 吞末字段、前导非数字跳过、X10 判定），
  每项映射到既有测试（无测试的补新断言）
- **区分"可达分支"与"结构保证分支"**：迭代器版首轮遗留 4 个 f==NULL
  死分支，证明纯 final/initial+final 的 case 中 final 恒构成字段 1
  （intermediate 打头会带 intermediate 而不匹配该 case）——结构不变量
  可直接去掉检查，不必补测试
- 先跑基线 → 重构 → 全量测试 + 覆盖率对比（行 100%、分支 ≥90% 不倒退）

### 27. key 赋值分类 → tfK_ builders（三坑都踩过）

把零散的 `key->type = ...` 按 tf_Type 分类，每个静态类型一个 builder:
`tfK_unicode(key, cp)` / `tfK_keysym(key, sym)` / `tfK_function(key, n)`。
**event/modifiers 不入 builder**（来源多变，统一入参反而臃肿）——踩坑:

- **event 冲突**: builder 初版强制 `event=PRESS`，覆盖了先行的 `tfD_event`
  （kitty REPEAT 被压回 PRESS，测试当场抓住）。正解: builder 只设
  type+d+utf8；`tf_readkey` 开头统一 `event=PRESS, modifiers=0` 兜底
- **decode 原样路径例外**: `tfU_decode` 对 5/6 字节序列原样拷贝 utf8，
  而 `tfU_encodecp` 压成 FFFD——测试固化"宽容保留"。decode 路径
  （utf8start/utf8/parseplain）**不用 builder**，其余 encodecp 路径全用
- **动态 type 例外**: trie/kitty 表命中、CS 类型（DCS/OSC/APC）、特殊报告
  ——builder 不适用，保持原样
- **太通用的名字是坏味道**: 内联掉 `tfD_setkey`——一行 helper 若名字比
  函数体还重，直接内联

## 设计阶段教训（commit freeze 重构复盘）

### 8. 行数预算按伪代码 ×2 估

设计文档预算"净增 24 行"，实际计划内函数就写了 ~50 行——伪代码不含
C89 声明块、错误传播、assert、断行。**预算失真一倍是常态**，要么按
伪代码行数 ×2 报，要么设计文档直接写 C 骨架。

### 9. 迭代算法必须在设计期做均摊分析

首版 freeze"每个叶容器从 root 重新下降找 hole"——实现简单但 O(H·L)，
review 被打回返工。**凡是"循环里重复定位"的设计，写设计文档时就要给
均摊论证**；给不出的，改为"从当前位置继续"的迭代器。

### 10. 上升+下降合并为单 iter 函数，参照既有导航原语

树上"找下一个 X"应写成**一个** pt_next 式函数（ascend 找可行层 →
descend 落点），而非 find/next/descend 三个小函数分散逻辑。
首次调用用哨兵初始化统一（如 `paths[0] = root.children` 后
`nexthole(C, 0)` 即"从头找"），消灭 findfirst/findnext 两套入口。

### 28. 混合初始化声明：非初始化变量在前

一行声明若混合初始化与非初始化变量，**非初始化放前面**——读者必须先
扫过非初始化变量（需要留意），初始化变量自解释可略过；若初始化打头，
后面的裸变量容易被漏看。如 `int len, cp = -1, mods = 0;`（len 在前）正，
~~`int i = 0, cp, r;`~~（cp/r 漏看风险）错。

### 30. 覆盖率缺口先问"能消除吗"，再问"能覆盖吗"

**教训（treesitter.c 绑定 44.9% → 100% 行/94.1% 分支）**：第一轮写豁免清单
是错的——绝大多数缺口是**可以改写掉的逻辑**，不是不可达代码。手段按序：

1. **行覆盖转分支覆盖（第一招）**——单行 `if (cond) return ...;` 条件与
   return 同行：**条件求值即该行已执行**（gcov 计行），return 是否执行
   只影响分支（软要求）。拆成两行则 return 行未执行 = 行缺口；clang-format
   会拆超 80 列的单行 if → return 部分必须短：错误推送抽 helper 且
   **helper 由可测路径共用**（fopen 失败与 fwrite 失败共用 `lyy_ioerr`，
   fopen 可测即覆盖 helper 体）；`werr` 式先计算再判断
   （`int werr = fwrite(...) != len; if (werr) return ...;`——werr 是计算
   结论非 flag）。**禁 clang-format off 保单行**（§21：off 只保护单行函数本体）
2. **相信 API，删防御检查**——TS/Lua 保证的枚举、越界、非 NULL 契约，
   检查分支删掉（`lts_checkenum`、error_type/quantifier 越界、`tname==NULL`、
   `L==NULL`、`name != NULL` 三元）。删前确认契约在 API 文档（严格输入，
   放松输出）。
3. **统一 > 特例**——条件分支幂等时无条件化：缓存刷新 `else if (tree 不同)`
   → 恒刷新（同树幂等，异树修 stale）。
4. **尾调用消除 else 块**——`if/else if/else` 改提前 return +
   `return luaL_typeerror(...)` 尾调用（error 函数不返回，编译器不报）。
5. **死分支删除**——恒真条件（所有 userdata 有 metatable →
   `lua_getmetatable` 去 if；luaopen 单次 → 9 处 `if (luaL_newmetatable)`
   去 if；所有对象有 mt → `luaL_getmetatable` 直查）。
6. **三元替代 if/break**——循环内 `if (end) seg=end+1; else break;`
   → `seg = end ? end+1 : seg+seglen`（三元两路均可执行）。
7. **不可达 default 消除**——code 恒在枚举内时 `default:` 行永不执行 →
   删 default，越界兜底放 switch 后单行 if（`if (code < 1 || code > 13) ...`，
   条件求值即覆盖——§34 实例）。
8. **测试盲区先查参数求值顺序**——`c:exec(q, t.root)` 里 `t.root` 先 error，
   `lts_checkquery` 根本没执行——先确认目标行真的可达，再补测试。
9. **gcov 行号映射**——条件编译（`#if LUA_VERSION_NUM < 502`）分支、宏展开
   分支的未覆盖行可能是映射偏移，先跑测试确认再决定豁免（§34：X macro
   展开代码映射到宏调用行，执行调用即整行覆盖）。

豁免清单只留：结构保证（fields 表恒函数）、平台分支（Windows `\\`、
5.1 fopen）、依赖外部配置的分支（cpath 段顺序）。最终报告见
`notes/reports/coverage_treesitter.md`。

## Lua 绑定沉淀（yyjson 绑定复盘，2026-08-11）

### 31. 单行 if 不加大括号

`if (cond) stmt;` 单语句**不加 `{}`**（两行块 → 一行）；else 同理。
仅当块内多语句才保留大括号。

### 33. case label 不用大括号，声明提函数头

`case X: { ... }` 是杂质——变量声明**全部提到函数头**（C89 限制下
本来就在函数头），case 体无声明即无需大括号：

```c
static int lyy_pushval(lua_State *L, yyjson_val *val, int depth) {
    yyjson_type     type = yyjson_get_type(val);
    yyjson_arr_iter aiter;
    ...
    switch (type) {
    case YYJSON_TYPE_ARR:
        aiter = yyjson_arr_iter_with(val);
        ...
        break;
```

声明**尽量消除**（一次使用内联进表达式：`lua_setfield(L, -2,
yyjson_get_str(key))`），只留必须的（循环游标/输出参数）。

### 34. X macro 集中错误码定义，且不损行覆盖

`#define LY_ERRORS(X) X(1, name1) X(2, name2) ...` 一处维护 code+name，
switch 内 `#define LY_ERROR_CASE(code, name) case code: pushliteral(#name)` 展开。
**宏展开代码的 gcov 行号映射到宏调用行（`LY_ERRORS(LY_ERROR_CASE)` 那一行）
——执行 switch 即整行覆盖，13 个 case 无需逐个触发**。配套：不可达的
`default:`（code 恒在枚举内）消除，越界兜底改为 switch 后单行 if
（`if (err->code < 1 || err->code > 13) ...`——条件求值即覆盖）。

### 35. LuaJIT 的 lua_tointegerx 截断浮点

**LuaJIT 的 `lua_tointegerx(3.5)` 返回 3 且 isint=true**（不检查小数），
5.5 语义正确——跨运行时行为不一致。绑定内**自实现统一版**
（`lyy_tointegerx`：`n == (double)(lua_Integer)n` 判断），不依赖平台
API 语义。**凡是跨 5.1/5.5/LuaJIT 的 API，先实测语义差异再直接用**。

### 36. lua_next 循环：pop 后栈索引语义变化（重构坑）

`while (lua_next(L, idx))` 循环内栈为 `[table, key, value]`——**pop
value 后 key 到栈顶（-1）**，`-2` 变成外层数据。重构把"检查 key"内联进
条件时**误把检查挪到 pop 之后** → `lua_type(-2)` 检查的是 value/外层
数据 → 数组全被误判对象（测试当场抓住）。规则：**栈索引敏感的取值/检查
必须发生在 pop 之前**；重构后跑全量测试（回归网价值，§26）。

### 37. Lua 绑定错误返回：稳定字符串名 + @alias 限死

解码失败返回 `nil, err_name, err_pos`（三值，不堆四值）——err 是
**稳定字符串**（`"unexpected_end"` 等，非自由文本），`.d.lua` 用
`@alias yyjson.err_code "..." | "..."` 限死全集——用户可
`err == "unexpected_end"` 比较，LuaLS 校验拼写。**不导出数字常量表**
（14 个函数的小 API 不配错误表）；编码错误码用 `lua_pushliteral`
（编译期 strlen，5.5 优化）；错误名表结构体带编译期 len 同样达标。

### 38. null 哨兵 = registry 固定表（身份比较）

JSON null → **registry 里的固定空表**（luaopen 建一次，`yyjson.null`
暴露），decode null 推同表、encode 用 `lua_rawequal` 身份比较识别。
对比 lightuserdata：表身份比较**两路都可测**（哨兵表 vs 普通表），
无"Lua 无法制造 lightuserdata"的结构分支。**表是 Lua 语言原生可造的
唯一对象 → 哨兵优先选表**。

### 39. clang-format 悬挂续行 = 代码坏味，改代码消除

逗号表达式 return / 多操作数续行被 clang-format 拆成远列悬空对齐
（`return ...,` 换行后末操作数孤行悬空缩进）= **悬挂写法**，接受即
放弃可读性，也是列宽微调就会崩的脆弱形态。修法：**改代码而非调
格式**——提行为条件赋值 + 单行 return（sp_next 尾：`if (bit)
C->poff = p->bytes[i];` 再单行 return），或重组表达式让续行自然
落在语句内部。禁 clang-format off 保单行（§21）。
（实例随 2026-08-16 sp_next 重构已替换为 assert+记账逗号链，原则不变）

### 40. 发现意外修改：先评估来源，不确定就问，禁止自作主张保留/回滚

**教训（2026-08-16 pt_next）**：回滚 pt_next 时看到一处"意外"文本
（`return (C->poff += bc, (void)(plen && (*plen = 0))), NULL;`——
前置 void 的 clang-analyzer 友好写法），误以为 formatter 改写，回滚
为 HEAD 原文。实际是**用户改的**（用户在其他处已用前置 void 写法
应对 clang-analyzer 警告，此处是其改漏的残留），白回滚一场，又得
改回来。规则：**diff 与预期不符时先正确评估——语义等价性、风格
倾向、可能来源（用户/formatter/其他进程）——拿不准就停下来询问，
绝不允许对未知修改自作主张保留或回滚**。

### 41. 绑定层内部函数不带分类码（统一 `xx_` 前缀）

分类码（`xxX_name`）只属**库头文件**（piecetab.h 等单头文件库）；
绑定层（lua/*.c）内部工具函数一律 `xx_` 前缀——lpt_charstep 而非
lptM_charstep（教训 2026-08-18 piecetab.c：Coder 产出 lptM_byteat/
lptK_seekchar 违反绑定层惯例，绑定层无多模块混用，分类码纯噪声）。

### 42. 顺序字节读取复用单一 Cursor 推进，禁逐字节 seek 重定位

读多字节序列（utf8 回扫/步进）必须**一次定位 + Cursor 顺序推进**
（pt_read 每次调用推进状态、跨 piece 安全；pt_advance 支持负 delta），
禁止每字节 pt_seek/locate 重建 Cursor——每次 seek 丢弃 Cursor 连续
状态（AGENTS.md 铁律：Cursor helper 严禁 locate）且重做定位。utf8
后退：一次定位后回扫 ≤4 字节窗口（pt_advance 负回退 + pt_read 顺序
读 + 尾部找 lead），窗口 4 字节必含 lead（utf8 最长 4 字节/3 续字节）。
（教训 2026-08-18 piecetab.c seek("char")：原实现后退每字节一次
pt_seek，O(4n) 次定位 → 改单 Cursor 顺序走）
