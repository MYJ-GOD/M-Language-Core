# M-Language Core (v2.0)

面向 AI 的结构化字节码与虚拟机实现，遵循 M-Token 规范，提供可验证、可审计、可授权的指令流以连接 AI 与硬件。

---

## 项目定位

M-Language 由三层构成：

1) **意图层**：人类或 AI 的高层任务描述
2) **M-Token 层**：全 varint 编码的结构化指令序列
3) **MVM 层**：执行与安全约束（step/call_depth/stack/授权/越界检查）

当前仓库专注于 **M-Token + MVM** 的核心实现、验证器与可执行示例。

---

## 当前实现进度

### 已完成

- **MVM 虚拟机核心**：全 varint 解码、栈机执行、调用帧管理、token index 映射（opcode index <-> byte offset）
- **Core 指令集 (0-99)**：B/E/IF/WH/FN/RT/CL/PH, LIT/V/LET/SET, LT/GT/LE/GE/EQ, ADD/SUB/MUL/DIV/AND/OR/XOR/SHL/SHR, LEN/GET/PUT/SWP, DUP/DRP/ROT, IOW/IOR, GTWAY/WAIT/HALT/TRACE
- **Extension 指令 (100-199)**：JMP/JZ/JNZ（svarint opcode index offset）, MOD/NEG/NOT/NEQ, NEWARR/IDX/STO, GC/BP/STEP（调试/控制）, DO/DWHL/WHIL（旧式循环构造）
- **平台扩展 (200-239)**：ALLOC/FREE
- **安全约束**：
  - Step limit（最大执行步数）
  - Gas limit（可选资源计费）
  - Call depth limit（函数调用深度限制，默认 32）
  - Stack limit（运行时栈大小限制）
  - 栈/局部/全局/PC 越界检查
- **授权机制**：GTWAY,<cap_id>（按 device_id 授权），IOW/IOR 均需 capability
- **字节码验证器（增强版）**：opcode/varint/B-E 匹配/IF/WH/FR 结构、跳转边界、locals/globals、栈效应与 IO 授权检查
- **调试工具**：反汇编、执行追踪、TRACE、断点/单步
- **内存管理**：动态内存分配 (ALLOC/FREE) + 简易 GC
- **测试套件**：17 个示例程序（算术、比较、变量、嵌套函数、JZ/JMP 循环、数组、IO、GC、断点/单步等）

### 部分完成 / 已知限制

- **WH/FR 结构化循环**：VM 在加载期自动 lowering 到 JZ/JMP 以保证语义正确
- **验证器**：尚未覆盖完整控制流可达性（CFG）分析
- **GC**：当前仅标记 `M_TYPE_REF`，数组为 `M_TYPE_ARRAY` 时未纳入 root 标记（后续需完善）
- **JIT**：接口存在但为 stub

### 进行中

- 更完善的栈效应验证
- 更多故障码测试覆盖
- 文档与规范持续同步

---

## 工程结构

```text
M-Language-Core/
├── include/
│   ├── m_vm.h            # VM 数据结构、指令/错误码定义
│   └── validator.h       # 字节码验证器接口
├── src/
│   ├── m_vm.c            # 虚拟机核心实现
│   ├── disasm.c          # 反汇编与追踪输出
│   ├── validator.c       # 字节码验证器实现
│   └── main.c            # 测试套件与示例程序
├── M-Token规范.md        # M-Token 指令集规范
├── M 语言体系完整大纲.md  # 体系结构说明
└── README.md
```

---

## 快速运行

本项目是标准的 C 工程，直接编译 `main.c` 可运行测试套件。

### Windows + MSVC

```powershell
cl /I include src\m_vm.c src\disasm.c src\validator.c src\main.c
.\main.exe
```

### Linux/macOS + GCC/Clang

```bash
gcc -I include src/m_vm.c src/disasm.c src/validator.c src/main.c -o main
./main
```

### 运行输出

- 反汇编结果
- 执行结果/故障码
- 执行追踪摘要（如启用）

---

## 指令表

**说明**：所有 opcode 与参数均采用 LEB128 varint 编码。下表为逻辑编号。

### Core 指令集 (0-99，冻结)

| 分组 | 指令 | 值 | 作用 |
| :-- | :-- | :-- | :-- |
| **控制流** | `B` / `E` | 10 / 11 | 结构化块起止 |
| | `IF` | 12 | 条件：`<cond>,IF,B<t>,E,B<e>,E` |
| | `WH` | 13 | 循环：`<cond>,WH,B<body>,E` |
| | `FR` | 14 | 循环：`<i>,<c>,<inc>,FR,B<body>,E` |
| | `FN` | 15 | 函数定义：`<arity>,B<body>,E` |
| | `RT` | 16 | 返回：`<value>` |
| | `CL` | 17 | 调用：`<func_id>,<argc>,<args...>` |
| | `PH` | 18 | 占位符 |
| **数据** | `LIT` | 30 | 字面量入栈（支持 zigzag i64 负数） |
| | `V` | 31 | 读局部（DeBruijn 索引） |
| | `LET` | 32 | 写局部 |
| | `SET` | 33 | 写全局 |
| **比较** | `LT` / `GT` / `LE` / `GE` / `EQ` | 40-44 | 比较运算，结果 0/1 |
| **算术** | `ADD` / `SUB` / `MUL` / `DIV` | 50-53 | 算术运算 |
| | `AND` | 54 | 按位与 |
| **位运算** | `OR` / `XOR` / `SHL` / `SHR` | 55-58 | 位运算（SHR/SHL 移位量自动 mask 63） |
| **数组** | `LEN` / `GET` / `PUT` / `SWP` | 60-63 | 数组长度/读取/写入/交换 |
| **栈操作** | `DUP` / `DRP` / `ROT` | 64-66 | 复制/丢弃/旋转 |
| **硬件IO** | `IOW` / `IOR` | 70 / 71 | IO 写 / IO 读 |
| **系统** | `GTWAY` / `WAIT` / `HALT` / `TRACE` | 80-83 | 授权/延时/停止/追踪 |

### Extension 指令 (100-199，可选)

| 指令 | 值 | 作用 |
| :-- | :-- | :-- |
| `JMP` | 100 | 无条件跳转（svarint opcode index offset） |
| `JZ` | 101 | 条件为零跳转 |
| `JNZ` | 102 | 条件非零跳转 |
| `MOD` | 110 | 取模（C 语义，符号与被除数一致） |
| `NEG` | 111 | 取反 |
| `NOT` | 112 | 按位取反 |
| `NEQ` | 113 | 不等于 |
| `NEWARR` | 120 | 创建数组 |
| `IDX` / `STO` | 121-122 | 数组索引/存储 |
| `GC` / `BP` / `STEP` | 130-132 | 调试/执行控制（位于 Extension 区间） |

### 平台扩展 (200-239)

| 指令 | 值 | 作用 |
| :-- | :-- | :-- |
| `ALLOC` / `FREE` | 200 / 201 | 动态内存分配/释放 |

完整定义见 `include/m_vm.h` 与 `M-Token规范.md`。

---

## 核心语义

### 值与类型

- **核心数值类型**：i64（int64_t）
- **布尔语义**：0 = false，非 0 = true
- **算术运算**：二进制补码 wrap-around
- **除法**：除零 trap
- **移位**：移位量自动应用 `& 63` mask
- **数组**：引用语义，越界访问 trap

### 执行约束

- **Step limit**：限制最大执行步数
- **Call depth limit**：限制函数调用深度（默认 32）
- **Stack limit**：运行时可配置的栈大小限制
- **Gas limit**：可选资源计费
- **IO 授权**：GTWAY 后 IO 操作生效

### 故障模型

所有约束违规触发 trap，立即终止执行并返回 fault code：

| 故障 | 场景 |
| :-- | :-- |
| `STACK_OVERFLOW` | 栈上溢/超出 stack_limit |
| `STACK_UNDERFLOW` | 栈下溢 |
| `PC_OOB` | 程序计数器越界 |
| `DIV_BY_ZERO` | 除零 |
| `LOCALS_OOB` / `GLOBALS_OOB` | 变量访问越界 |
| `ARRAY_OOB` | 数组索引越界 |
| `CALL_DEPTH_LIMIT` | 函数调用深度超限 |
| `UNAUTHORIZED_IO` | 未授权的 IO 操作 |

---

## 字节码验证器

M-Language 提供静态验证器 `m_validate()`，在执行前检查字节码合法性：

```c
#include "validator.h"

M_ValidatorResult result = m_validate(code, code_len);
if (!result.valid) {
    printf("Validation failed at PC %d: %s\n", result.pc, result.msg);
}
```

### Core-only 验证（对外发布）

```c
M_ValidatorResult result = m_validate_core_only(code, code_len);
```

### 验证检查项

1. opcode 合法性（当前允许 0-255）
2. varint 编码完整性
3. B/E 块匹配
4. locals 访问不越界

---

## 运行输出示例

```
M Language Virtual Machine - Test Suite
========================================

Program: Arithmetic (5 + 3 * 2)
Bytecode size: 9 bytes
=== Disassembly ===
LIT 5
LIT 3
LIT 2
MUL
ADD
=== Execution ===
Execution result: fault=NONE, steps=6, result=11

Program: While Loop (sum 1 to 5)
...
```

---

## 里程碑

### 已完成 (v2.0)

- M-Token 规范与 VM 1:1 对齐
- Core/Extension/Platform 指令空间分离
- WHILE/FOR 循环编译降级支持
- 结构化字节码验证器
- Call depth / Stack limit 运行时检查
- LIT zigzag i64 负数支持
- 完整故障码覆盖

### 中期目标

- 更完善的栈效应验证
- 跨平台移植（MVM 嵌入式版本）
- AI 到 M-Token 编译工具链

### 长期方向

- JIT 编译支持
- 高级优化（常量折叠、死代码消除）
- 多设备协作执行

---

## 贡献指南

- **代码风格**：保持现有 C 风格，新增功能先写 demo 到 `main.c`
- **测试要求**：新增指令或 VM 行为需补充测试用例
- **提交规范**：避免引入无关依赖
- **文档同步**：变更 opcode/语义时需更新 `M-Token规范.md`

---

## 版权与许可

Copyright (c) 2026 MYJ-GOD. All Rights Reserved.

MIT License
