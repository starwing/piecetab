# 代码精炼要诀

本文件记录 piecetab / termfeed 代码精炼（refine）实验中的核心经验。

## 核心信条

**精炼不是为了减行数，而是为了减杂质。** 25行限制是工具，不是目标。为了凑行数引入 flag、拆分函数、或制造短路逻辑，都属于庸工。改完若不如改前清晰，就是倒退。

## 七大原则

### 1. 自然数据流 > 人工 flag

| 坏 | 好 |
|---|---|
| 先设 flag 表示状态，后面再检查 | 计算一个值，让数据本身告诉你答案 |

**例：`ptD_mergeleaf`**

坏版本：
```c
if (可以合并左右) merged = 1;
else if (左右都是 hole) {
    if (左边有空间) { 部分合并; goto done; }
    else { 完全合并; merged = 1; }
}
done: if (!merged) { /* 不合并路径 */ }
```

好版本：
```c
if (左右都是 hole) d = pt_min(右大小, 左剩余空间);
ptH_append(左, d);
if (d < 右大小) ptH_remove(右, d);  // 部分 → 右有剩
else ptP_free(右), merged = 1;       // 满 → 右被吃光
if (!merged) { /* 不合并路径 */ }
```

`d` 的值自然告诉你这是部分合并还是完全合并，无需额外检查"空间够不够"。

**注意：** `merged` 在这里仍然是必要的——它记录"是否合并了整片右叶"，这是数据流无法直接回答的布尔问题。关键区别在于：`merged` 是**计算的结论**，不是**预设的标记**。

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

**例：`ptC_freeze` 中的 `levels==0`：**

坏版本：
```c
if (tree->levels == 0) { 单独处理; return; }
for (l = tree->levels - 1; l >= 0; l--) { ... }
```

好版本：
```c
for (l = tree->levels; l > 0;) {
    --l;
    if (l >= tree->levels) break;  // levels==0 时立即 break
    ...
}
```

### 5. 逗号表达式替代单行块

纯赋值的单行 `if` 块，用 `,` 取代 `{ }`：

```c
// 6行
if (del > 0) {
    r = pt_remove(C, del);
    if (r != PT_OK) return r;
}

// 1行
if (del > 0 && (r = pt_remove(C, del)) != PT_OK) return r;
```

### 6. 变量少即是多

删掉只出现一次的栈变量：

```c
// 引入 levels 只是为了省几次函数调用 —— 不值得
int levels = ptK_levels(C);
... levels ... levels ... levels ...

// 直接用函数调用，或声明在 for 里
for (int l = ptK_levels(C); l > 0;) { ... }
```

但**不要重复计算有副作用的表达式**——如果函数无副作用且编译器能优化，重复调用比引入临时变量更清晰。

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

### 13. 函数尽量带返回值

返回值 = 成功/失败（TF_OK/AGAIN/ERRMEM...），"提交语义"（状态复位、
计数更新）用逗号缩到 return 后：

```c
// 繁：6 行
if (b == 0x07 || b == 0x9c) { tfD_cskey(S, key); return TF_OK; }
// 简：1 行
if (b == 0x07 || b == 0x9c) return tfD_cskey(S, key), TF_OK;
```

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

**注释最容易与实现不同步**——design 文档里 `cs_len -= 2` 的 ST 检测
描述与代码 `-= 1` 长期不一致（brief_refine §16 早已记录此 bug，文档
却未同步）。改代码后必须同步 design 与精炼文档。

### 25. 性能先于简练——API 形态上杜绝 O(n²)

**简练清晰是为了更好理解最优化实现，不能本末倒置**。refine 时
"看着顺眼"的接口可能是性能陷阱:

- **反例**: `tfC_arg(S, n, dflt)` 按索引从 buf 头重扫——调用方写成
  `tfC_arg(S,1) + tfC_arg(S,2) + tfC_arg(S,3)` 时是 O(n²)，且"按索引
  随机访问流式序列"的 API 形态**诱导**调用方写出 O(n²) 代码
- **正解**: 参数区是一次性消费的序列 → 迭代器
  `tfC_nextarg(S, &f, &len)`——f/len 构成 in/out 游标（NULL 起步），
  内部用已知长度跳过当前字段（零重扫 O(n)），调用方"默认值先行 +
  逐字段取值"
- **迭代器形态三连坑（都踩过）**: ① 返回字段头指针 → f 按值传与
  in/out plen 方向混杂，签名撒谎；最终 `int` + `const char **pf`
  两 in/out 对称。② 耗尽后 f=NULL 会**重启**（NULL 兼首调/耗尽双重
  语义）→ 耗尽置 f=end 幂等返回 0。③ 界面臃肿（v 数组+Args struct
  +dflt 契约）是抽象错误的信号——首版 `tfC_scan` 因"读字段+循环+
  子参数+raw"全包而复杂，切回迭代器后 scan/Args 整个消失，净减
  ~130 行
- 判定标准: 每次调用是否"从头重扫"？同一数据是否被重复定位？
  是 → 改成单次扫描/迭代器（brief_refine §9 的均摊论证同源）

### 26. 重构前先列"隐性行为清单"，靠全量测试当回归网

tfC_arg→tfC_scan 重构中，`v[2]` 缺字段未填 dflt 的 bug 被既有测试
（`\x1b[27;99~` → UNKNOWN_CSI，字段 3 不存在 → 必须 dflt）当场抓住。
**隐性契约的保障不是头注释**（头注释同样会与实现不同步——design 里
`cs_len -= 2` 错一年就是例证），而是三级:

1. **消除**（最优先）——重构使约束成为自然数据流。如 nextarg 的
   "耗尽后不重启"被 `if (tfC_nextarg(...))` 循环模式天然免疫，
   约束根本不存在; "f1 恒存在"是 case 匹配的结构保证（见 §26b）
2. **使用处/实现处注释**——约束绑定生效位置，改代码必然看到。
   如 `tfK_utf8` 的 verbatim 语义注在 tfU_decode 调用行旁
3. **测试兜底**——只负责验证，不该是唯一保障

- 重构前从被删代码/注释提炼行为清单: 缺字段→dflt、空字段→dflt、
  intermediate 截断、final 吞进末字段、前导非数字跳过、X10 判定
- 每项映射到既有测试（无测试的项补新断言，如 `nextarg_edge` 的控制
  字节/initial '?' 分支）
- **注意区分"可达分支"与"结构保证分支"**: 迭代器版首轮遗留 4 个
  f==NULL 死分支，分析后证明**纯 final / initial+final 的 case 中
  final 恒构成字段 1**（intermediate 打头会使 cmd 带 intermediate
  而不匹配该 case）——这类结构不变量可直接去掉检查，不必补测试
- 先跑基线 → 重构 → 全量测试 + 覆盖率对比（行 100%、分支 ≥90% 不倒退）

### 27. key 赋值分类 → tfK_ builders（三坑都踩过）

把零散的 `key->type = ...` 按 tf_Type 分类，每个静态类型一个 builder:
`tfK_unicode(key, cp)` / `tfK_keysym(key, sym)` / `tfK_function(key, n)`。
**event/modifiers 不入 builder**（来源多变，统一入参反而臃肿）——踩坑:

- **event 冲突**: builder 初版强制 `event=PRESS`，覆盖了先行的
  `tfD_event`（kitty 的 REPEAT 被压回 PRESS，测试当场抓住）。正解:
  builder 只设 type+d+utf8；`tf_readkey` 开头统一
  `event=PRESS, modifiers=0` 兜底（replay 分支曾丢 modifiers 清零）
- **decode 原样路径例外**: `tfU_decode` 对 5/6 字节序列（cp>0x10FFFF）
  原样拷贝 utf8，而 `tfU_encodecp` 压成 FFFD——测试固化"宽容保留"。
  decode 路径（utf8start/utf8/parseplain）**不用 builder**（encodecp
  语义），其余 encodecp 路径全用
- **动态 type 例外**: trie/kitty 表命中（type 运行时决定）、CS 类型
  （DCS/OSC/APC）、特殊报告（POSITION/MODEREPORT/KITTYREPORT）——
  builder 不适用，保持原样
- **太通用的名字是坏味道**: 内联掉 `tfD_setkey`（原 6 行缩成 1 行
  调用后名字就只剩占位）——一行 helper 若名字比函数体还重，直接内联

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

### 29. 裸 return 只留单语句

块状裸 return（`if (x) { stmt; ...; return; }`）用**提交语义**消除：
函数带返回值，块改为 `return 表达式, 状态;` 单语句或 if/else 链
（`tfC_kitty` 的 `cp<0` 块、`kittyfind` 块均如此收编）。单语句
`if (x) return;` 不算裸 return——递归基例、OOM 尽力而为、API 参数
校验等天然形态可保留，不强行改。
