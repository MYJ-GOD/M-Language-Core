# M-Token 规范文档

## 1. 概述

M-Token 是一种专为 AI 设计的压缩字节码语言，用于 AI 与硬件之间的高效通信。

### 设计原则

- **全 Varint 编码**：所有 token 使用变长整数编码
- **极致压缩**：每个 token 占用最小字节数
- **AI 友好**：指令集简洁统一，便于 AI 生成和理解
- **栈机架构**：基于栈的执行模型，兼容 SSA 优化

---

## 2. 数据编码

### 2.1 Varint (无符号变长整数)

使用 LEB128 变长编码：

```
值范围        编码
────────────  ─────────────────────────────
0 - 127       1 字节:  [0xxxxxxx] (bit 7 = 0)
128 - 16511   2 字节:  [1xxxxxxx][0xxxxxxx]
16512 - ...   更多字节...
```

示例：
```
0      → [0x00]
127    → [0x7F]
128    → [0x80, 0x01]
255    → [0xFF, 0x01]
1000   → [0xE8, 0x07]
```

### 2.2 Zigzag (有符号整数)

将负数映射为无符号整数：

```c
// 编码
uint32_t encode_zigzag(int32_t n) {
    return (n << 1) ^ (n >> 31);
}

// 解码
int32_t decode_zigzag(uint32_t n) {
    return (n >> 1) ^ -(int32_t)(n & 1);
}
```

示例：
```
0      → [0x00]
-1     → [0x01]
1      → [0x02]
-2     → [0x03]
127    → [0xFE]
-127   → [0xFD]
```

---

## 3. 操作码表

### 3.1 控制流 (10-18)

| Opcode | 名称 | 格式 | 说明 |
|--------|------|------|------|
| 10 | B | - | 块开始 (block begin) |
| 11 | E | - | 块结束 (block end) |
| 12 | IF | `<cond>,IF,B<then>E,B<else>E` | 条件语句 |
| 13 | WH | `<cond>,WH,B<body>E` | While 循环 |
| 14 | FR | `<init>,<cond>,<inc>,FR,B<body>E` | For 循环 |
| 15 | FN | `<arity>,B<body>,E` | 函数定义 |
| 16 | RT | `<value>` | 函数返回 |
| 17 | CL | `<func_id>,<argc>,<arg0>..<argN>` | 函数调用 |
| 18 | PH | - | 占位符 |

### 3.2 数据操作 (30-33)

| Opcode | 名称 | 格式 | 说明 |
|--------|------|------|------|
| 30 | LIT | `<value>` | 压入字面量 |
| 31 | V | `<index>` | 读取局部变量 (DeBruijn 索引) |
| 32 | LET | `<index>,<value>` | 写入局部变量 |
| 33 | SET | `<name_id>,<value>` | 写入全局变量 |

### 3.3 比较运算 (40-44)

| Opcode | 名称 | 栈操作 | 说明 |
|--------|------|--------|------|
| 40 | LT | a,b → (a<b) | 小于 |
| 41 | GT | a,b → (a>b) | 大于 |
| 42 | LE | a,b → (a<=b) | 小于等于 |
| 43 | GE | a,b → (a>=b) | 大于等于 |
| 44 | EQ | a,b → (a==b) | 等于 |

### 3.4 算术运算 (50-58)

| Opcode | 名称 | 栈操作 | 说明 |
|--------|------|--------|------|
| 50 | ADD | a,b → (a+b) | 加法 |
| 51 | SUB | a,b → (a-b) | 减法 |
| 52 | MUL | a,b → (a*b) | 乘法 |
| 53 | DIV | a,b → (a/b) | 除法 |
| 54 | AND | a,b → (a&b) | 按位与 |
| 55 | OR | a,b → (a\|b) | 按位或 |
| 56 | XOR | a,b → (a^b) | 按位异或 |
| 57 | SHL | a,b → (a<<b) | 左移 |
| 58 | SHR | a,b → (a>>b) | 右移 |

### 3.5 数组操作 (60-63)

| Opcode | 名称 | 栈操作 | 说明 |
|--------|------|--------|------|
| 60 | LEN | arr → len | 数组长度 |
| 61 | GET | arr,idx → val | 数组读取 |
| 62 | PUT | arr,idx,val → arr | 数组写入 |
| 63 | SWP | a,b → b,a | 交换栈顶两元素 |

### 3.6 栈操作 (64-66)

| Opcode | 名称 | 栈操作 | 说明 |
|--------|------|--------|------|
| 64 | DUP | a → a,a | 复制栈顶 |
| 65 | DRP | a → (pop) | 丢弃栈顶 |
| 66 | ROT | a,b,c → b,c,a | 旋转栈顶3元素 |

### 3.7 硬件 IO (70-71)

| Opcode | 名称 | 格式 | 说明 |
|--------|------|------|------|
| 70 | IOW | `<device_id>,<value>` | 硬件写入 |
| 71 | IOR | `<device_id>` → value | 硬件读取 |

### 3.8 系统操作 (80-83)

| Opcode | 名称 | 格式 | 说明 |
|--------|------|------|------|
| 80 | GTWAY | `<key>` | 安全授权 |
| 81 | WAIT | `<milliseconds>` | 延时等待 |
| 82 | HALT | - | 停止执行 |
| 83 | TRACE | `<level>` | 调试追踪 |

---

## 4. 语法格式

### 4.1 函数定义

```
FN,<arity>,B<body>,E

示例:
  FN,2,B,V0,V1,ADD,RT,E
  ; 定义函数: add(a, b) = a + b
  ; 参数数量 arity = 2
```

### 4.2 函数调用

```
CL,<func_id>,<argc>,<arg0>,<arg1>,...

示例:
  CL,5,2,10,20
  ; 调用 func_5，参数个数=2，参数为 10 和 20
```

### 4.3 条件语句

```
<cond>,IF,B<then>,E,B<else>,E

示例:
  LIT,10,LIT,5,GT,IF,B,LIT,1,E,B,LIT,0,E
  ; if (10 > 5) { 1 } else { 0 }
```

### 4.4 While 循环

```
<cond>,WH,B<body>,E

示例:
  LIT,0,LET,0,LIT,10,LET,1,V0,LT,WH,B,V0,V1,ADD,LET,1,V0,LIT,1,ADD,LET,0,E
  ; sum = 0; i = 0; while (i < 10) { sum += i; i++ }
```

### 4.5 变量作用域 (DeBruijn 索引)

使用 DeBruijn 索引替代变量名：

```
V,<index>
LET,<index>,<value>
```

- 索引 0: 最近作用域的变量
- 索引 1: 上一级作用域的变量
- 以此类推

---

## 5. 字节码示例

### 5.1 简单计算: 5 + 3 * 2

```
字节码 (十六进制):
  [1E] [05] [1E] [03] [1E] [02] [34] [32] [52]
  
  1E = LIT
  05 = 5
  1E = LIT
  03 = 3
  1E = LIT
  02 = 2
  34 = MUL
  32 = ADD
  52 = HALT
```

### 5.2 函数调用

```
定义: fn add(a,b) { a + b }
字节码:
  [0F] [02] [0A]  [1E] [00] [1E] [01] [32] [10] [0B]
  [1E] [05] [1E] [03] [11] [05] [52]
  
  0F 02    = FN, arity=2
  0A       = B (block begin)
  1E 00    = V 0 (a)
  1E 01    = V 1 (b)
  32       = ADD
  10       = RT
  0B       = E (block end)
  
  1E 05    = LIT 5
  1E 03    = LIT 3
  11 05    = CL func_0, 2 args
  52       = HALT
```

---

## 6. 执行模型

### 6.1 栈机架构

- **数据栈**: 用于计算和传递参数
- **返回栈**: 用于 CALL/RET
- **局部变量池**: 存储局部变量 (DeBruijn 索引)
- **全局变量池**: 存储全局变量

### 6.2 执行流程

```
while (pc < code_len && running) {
    op = decode_varint(code, &pc);
    handler = TABLE[op];
    handler(vm);
    steps++;
    if (steps > step_limit) fault;
}
```

---

## 7. 安全机制

### 7.1 执行限制

| 限制 | 说明 | 默认值 |
|------|------|--------|
| step_limit | 最大执行步数 | 1,000,000 |
| gas_limit | 最大 gas 消耗 | 0 (无限制) |
| stack_size | 数据栈大小 | 256 |
| locals_size | 局部变量数量 | 64 |
| globals_size | 全局变量数量 | 128 |

### 7.2 Gas 消耗

| 指令类型 | Gas 消耗 |
|----------|----------|
| 栈操作 (DUP, DRP, SWP) | 1 |
| 算术运算 (ADD, SUB) | 1 |
| 乘除 (MUL, DIV) | 3-5 |
| 函数调用 (CL) | 5 |
| 硬件 IO (IOW, IOR) | 3-5 |
| 无操作 (B, E, HALT) | 0 |

---

## 8. AI 集成

### 8.1 生成 M-Token 的提示词模板

```markdown
生成 M-Token 字节码来完成以下任务:

任务描述: [任务说明]

要求:
- 使用 M-Token 语法
- 所有操作使用 varint 编码
- 变量使用 DeBruijn 索引

输出格式:
- 每行一个 token: [opcode],<参数>
- 最后一行: HALT

示例:
LIT,5
LIT,3
ADD
HALT
```

### 8.2 执行与反馈

```c
// AI 生成字节码
uint8_t bytecode[] = { ... };

// VM 执行
M_VM vm;
m_vm_init(&vm, bytecode, len, io_write, io_read, NULL, NULL);
m_vm_set_step_limit(&vm, 10000);

int result = m_vm_run(&vm);

// 反馈给 AI
if (result < 0) {
    // 错误: m_vm_fault_string(vm.fault)
} else {
    // 成功: vm.stack[vm.sp] 是结果
}
```

---

## 9. 参考实现

- `m_vm.h`: VM 头文件定义
- `m_vm.c`: VM 解释器实现
- `disasm.c`: 反编译器
- `mian.c`: 测试用例

---

## 10. 修订历史

| 版本 | 日期 | 说明 |
|------|------|------|
| 1.0 | 2026-02-01 | 初始规范 |

