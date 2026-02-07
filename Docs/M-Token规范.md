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
Opcode	名称	参数	行为
30	LIT	<value>	将字面量推入栈
31	V	<index>	读取局部变量（DeBruijn index）
32	LET	<index>,<value>	写入局部变量
33	SET	<name_id>,<value>	写入全局变量

3.3 比较（40–44）
Opcode	名称	栈行为
40	LT	a,b → a<b (1:true, 0:false)
41	GT	a,b → a>b (1:true, 0:false)
42	LE	a,b → a<=b (1:true, 0:false)
43	GE	a,b → a>=b (1:true, 0:false)
44	EQ	a,b → a==b (1:true, 0:false)

3.4 算术 / 位运算（50–58）
Opcode	名称	栈行为
50	ADD	a,b → a+b
51	SUB	a,b → a-b
52	MUL	a,b → a*b
53	DIV	a,b → a/b
54	AND	a,b → a&b
55	OR	a,b → a|b
56	XOR	a,b → a^b
57	SHL	a,b → a<<b
58	SHR	a,b → a>>b

3.5 数组（60–63）
Opcode	名称	栈行为
60	LEN	<array_ref> → <length>
61	GET	<array_ref>,<index> → <element>
62	PUT	<array_ref>,<index>,<value> → <array_ref>
63	SWP	<a>,<b> → <b>,<a>

3.6 栈操作（64–66）
Opcode	名称	栈行为
64	DUP	<a> → <a>,<a>	（复制栈顶）
65	DRP	<a> → （丢弃栈顶）
66	ROT	<a>,<b>,<c> → <b>,<c>,<a>	（旋转栈顶3个）

3.7 硬件 IO（70–71）
重要：所有 IO 操作必须先用 GTWAY 授权，否则会返回 UNAUTHORIZED 错误。

Opcode	名称	参数	栈行为	说明
70	IOW	<device_id>,<value>	<value>,<device_id> → （弹出2个）	写入设备
71	IOR	<device_id>	（弹出1个）→ <value>	读取设备

设备 ID 列表（平台相关）：
| device_id | 类型   | 名称         | 说明               |
|-----------|--------|--------------|--------------------|
| 1         | SENSOR | Water_Level  | 水位传感器 (A0)    |
| 2         | SENSOR | DHT11_Temp   | 温度传感器 (D4)    |
| 3         | SENSOR | DHT11_Humidity| 湿度传感器 (D4)  |
| 5         | ACTUATOR | Relay_1   | 继电器1 (D1)       |
| 6         | ACTUATOR | Relay_2   | 继电器2 (D2)       |

3.8 系统（80–83）
Opcode	名称	参数	行为
80	GTWAY	<cap_id>	将 capability 加入当前执行会话
81	WAIT	<milliseconds>	延迟指定毫秒数
82	HALT		正常终止执行
83	TRACE	<level>	调试跟踪（不产生 IO 副作用）

3.9 内存管理（200–201，平台扩展）
Opcode	名称	栈行为	说明
200	ALLOC	<size> → <ptr>	在堆上分配内存
201	FREE	<ptr> →	释放之前分配的内存

注：仅在支持堆分配的平台可用。

4. Extension / IR 指令（100–199）
4.1 跳转（低级控制流）
Opcode	名称	参数
100	JMP	<svarint offset>
101	JZ	<svarint offset>
102	JNZ	<svarint offset>

4.2 算术扩展
Opcode	名称	栈行为
110	MOD	a,b → a%b
111	NEG	<a> → <-a>
112	NOT	<a> → <~a>
113	NEQ	a,b → a!=b (1:true, 0:false)

4.3 数组 / 内存扩展
120	NEWARR	<size> → <array_ref>
121	IDX	<array_ref>,<index> → <element>
122	STO	<array_ref>,<index>,<value> → <array_ref>

4.4 调试 / 执行控制
130	GC		垃圾回收
131	BP	<id>	断点
132	STEP		单步执行

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

capability 默认按 device_id 授权（cap_id = device_id）

未授权 IO → trap

授权仅在当前执行会话内有效

10. 完整示例

10.1 读取水位传感器
```m-token
[80, 1, 30, 1, 71, 1, 82]
```
解码：
| Token  | 含义           |
|--------|----------------|
| 80     | GTWAY          |
| 1      | cap_id=1       |
| 30     | LIT            |
| 1      | value=1        |
| 71     | IOR            |
| 1      | device_id=1    |
| 82     | HALT           |

栈操作：push(1) → IOR(1) → pop → result

10.2 打开继电器1
```m-token
[80, 5, 30, 1, 70, 5, 82]
```
解码：
| Token  | 含义           |
|--------|----------------|
| 80     | GTWAY          |
| 5      | cap_id=5       |
| 30     | LIT            |
| 1      | value=1        |
| 70     | IOW            |
| 5      | device_id=5    |
| 82     | HALT           |

10.3 关闭继电器2
```m-token
[80, 6, 30, 0, 70, 6, 82]
```

10.4 读取温度并判断
```m-token
[80, 2, 30, 1, 71, 2, 30, 30, 44, 82]
```
解码：
| Token | 含义           |
|-------|----------------|
| 80    | GTWAY          |
| 2     | cap_id=2       |
| 30    | LIT            |
| 1     | cap_id=1       |
| 71    | IOR            |
| 2     | device_id=2    |
| 30    | LIT            |
| 30    | value=30       |
| 44    | EQ (==)        |
| 82    | HALT           |
结果：1 (true) 如果温度 == 30，否则 0 (false)

11. Fault / Trap 模型
11.1 常见 Fault 类型

STACK_OVERFLOW / STACK_UNDERFLOW

BAD_OPCODE / BAD_VARINT

PC_OOB

DIV_BY_ZERO

LOCAL_OOB / GLOBAL_OOB

ARRAY_OOB

STEP_LIMIT / GAS_LIMIT

UNAUTHORIZED_IO（IO 未授权）

UNKNOWN_OP（未知 opcode）

11.2 Trap 行为

立即停止执行

返回 fault code + PC

HALT 为正常终止，fault 为异常终止

12. Static Validation（验证器规范）

验证器 必须拒绝执行 不合法字节码，至少检查：

opcode 合法

varint 编码完整

B/E 完整匹配

IF/WH/FR 结构正确

栈效应在所有路径合法

跳转不越界

locals / globals 访问合法

IO 操作具备授权（前面必须有对应的 GTWAY）

13. Gas 与执行限制

VM 必须支持 step_limit

gas_limit 可选

gas 为平台无关固定表

gas 用尽 → trap

14. TRACE 与调试

TRACE,<level> 不得影响程序语义

输出至少包含：PC、opcode

可选输出：栈顶值、locals 概要

TRACE 不得产生 IO 副作用

15. 确定性

Core 指令必须确定性

非确定性行为只能通过 IO 实现

同字节码 + 同输入 → 同结果

16. 约束

Core 指令编号 永不重定义

Extension 指令 不得占用 Core 编号

Core 结构化指令 可 lowering 为 IR

lowering 不得改变可观测语义（IO / TRACE）

17. 合法性

一段 M-Token 程序是合法的，当且仅当：

所有 opcode 属于允许区间

结构化块 B/E 完整匹配

跳转 offset 合法

栈高度在所有路径上合法

