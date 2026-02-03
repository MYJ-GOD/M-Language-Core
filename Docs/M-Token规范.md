M-Token 规范
1. 总体

M-Token 是一种 结构化、可执行、可验证 的指令序列格式。

面向 AI 生成 / 人类审计 / 虚拟机执行。

执行模型：栈机。

所有指令与参数均使用 Varint 编码。

有符号数使用 ZigZag + Varint。

2. 指令空间划分
区间	用途
0–99	Core（冻结，对外 ABI）
100–199	Extension / IR（实现内部，可选）
200–239	平台 / 硬件扩展
240–255	实验 / 保留

对外发布的 M-Token 只允许使用 Core 指令。

3. Core 指令集（0–99，冻结）
3.1 控制流（10–18）
Opcode	名称	语法
10	B	block begin
11	E	block end
12	IF	<cond>,IF,B<then>,E,B<else>,E
13	WH	<cond>,WH,B<body>,E
14	FR	<init>,<cond>,<inc>,FR,B<body>,E
15	FN	FN,<arity>,B<body>,E
16	RT	RT,<value>
17	CL	CL,<func_id>,<argc>,<arg...>
18	PH	placeholder
3.2 数据与变量（30–33）
Opcode	名称	行为
30	LIT	push literal
31	V	read local (DeBruijn index)
32	LET	write local
33	SET	write global
3.3 比较（40–44）

LT / GT / LE / GE / EQ

3.4 算术 / 位运算（50–58）
Opcode	名称
50	ADD
51	SUB
52	MUL
53	DIV
54	AND
55	OR
56	XOR
57	SHL
58	SHR
3.5 数组（60–63）
Opcode	名称	栈行为
60	LEN	arr → len
61	GET	arr,idx → val
62	PUT	arr,idx,val → arr
63	SWP	a,b → b,a
3.6 栈操作（64–66）

DUP / DRP / ROT

3.7 硬件 IO（70–71）
Opcode	名称
70	IOW
71	IOR
3.8 系统（80–83）
Opcode	名称
80	GTWAY
81	WAIT
82	HALT
83	TRACE
4. Extension / IR 指令（100–199）
4.1 跳转（低级控制流）
Opcode	名称	参数
100	JMP	svarint offset
101	JZ	svarint offset
102	JNZ	svarint offset
4.2 算术扩展

MOD / NEG / NOT

4.3 数组 / 内存扩展

NEWARR / IDX / STO

4.4 调试 / 执行控制

GC / BP / STEP

5. 值与类型语义（Value Model）

核心数值类型：i64

布尔语义：0 = false，非 0 = true

所有算术操作使用 二进制补码 wrap-around

DIV：除 0 trap

SHL / SHR：移位量 b 使用 b & 63

比较运算结果为 0 / 1

数组为 引用语义

数组越界访问 trap

6. 栈效应与安全
6.1 栈效应规则

每条指令具有确定的栈输入/输出

栈下溢 → trap

栈上溢（超过 stack_limit）→ trap

6.2 结构化一致性规则

IF：then 与 else 的净栈效应必须一致

WH / FR：循环体的净栈效应必须为 0

B…E 为语句块，不产生隐式返回值

RT：返回值来自栈顶，其余栈内容丢弃

7. 函数与作用域（Frame Model）

FN,<arity> 创建新的调用帧

参数绑定至 local[0..arity-1]

其余局部变量初始化为 0

V / LET 访问越界 → trap

LET 允许写入参数位

RT 结束当前帧并返回值

调用深度超过 call_depth_limit → trap

8. 跳转 IR 语义（Extension）

offset 单位：opcode token 索引

相对基准：当前指令之后的下一条指令

JZ / JNZ：消耗栈顶条件值

跳转目标必须位于 opcode 边界

跳转越界 → trap

9. 授权与 Capability（GTWAY）

GTWAY,<cap_id> 将 capability 加入当前执行会话

IOW / IOR 执行前必须验证 capability

capability 默认按 device_id 授权

未授权 IO → trap

授权仅在当前执行会话内有效

10. Fault / Trap 模型
10.1 常见 Fault 类型

STACK_UNDERFLOW / STACK_OVERFLOW

BAD_OPCODE / BAD_VARINT

PC_OOB

DIV_BY_ZERO

LOCAL_OOB / GLOBAL_OOB

ARRAY_OOB

STEP_LIMIT / GAS_LIMIT

UNAUTHORIZED_IO

10.2 Trap 行为

立即停止执行

返回 fault code + PC

HALT 为正常终止，fault 为异常终止

11. Static Validation（验证器规范）

验证器 必须拒绝执行 不合法字节码，至少检查：

opcode 合法

varint 编码完整

B/E 完整匹配

IF/WH/FR 结构正确

栈效应在所有路径合法

跳转不越界

locals / globals 访问合法

IO 操作具备授权

12. Gas 与执行限制

VM 必须支持 step_limit

gas_limit 可选

gas 为平台无关固定表

gas 用尽 → trap

13. TRACE 与调试

TRACE,<level> 不得影响程序语义

输出至少包含：PC、opcode

可选输出：栈顶值、locals 概要

TRACE 不得产生 IO 副作用

14. 确定性

Core 指令必须确定性

非确定性行为只能通过 IO 实现

同字节码 + 同输入 → 同结果

15. 约束

Core 指令编号 永不重定义

Extension 指令 不得占用 Core 编号

Core 结构化指令 可 lowering 为 IR

lowering 不得改变可观测语义（IO / TRACE）

16. 合法性

一段 M-Token 程序是合法的，当且仅当：

所有 opcode 属于允许区间

结构化块 B/E 完整匹配

跳转 offset 合法

栈高度在所有路径上合法

