# M-Language Core (v1.3)

面向 AI 的极简字节码与虚拟机实现，目标是用可验证、可审计、可授权的指令流连接 AI 与硬件。

---

## 项目定位

M-Language 由三层构成：

1) 意图层：人类或 AI 的高层任务描述  
2) M-Token 层：全 varint 编码的极简指令序列  
3) MVM 层：执行与安全约束（step/gas/授权/越界检查）

当前仓库专注于 **M-Token + MVM** 的核心实现与可执行示例。

---

## 当前实现进度

已完成：
- MVM 虚拟机核心（全 varint 解码、栈机执行）
- 指令集：算术/比较/位运算/栈操作/变量/函数/控制流/数组/IO
- 安全约束：step limit、gas limit、栈/局部/全局/PC 越界检查
- 授权机制：GTWAY 指令（授权后单次 IO 生效）
- 调试工具：字节码反汇编 + 执行追踪摘要
- 测试套件：8 个示例程序（算术/比较/变量/函数/循环/位运算/栈/IO）

进行中（文档级别已定义，代码后续可逐步补齐）：
- 更严格的结构化控制流验证
- 更完善的错误码覆盖与测试矩阵
- 与 M-Token 规范的一致性校验与版本化

---

## 工程结构

```text
M-Language-Core/
├── include/
│   └── m_vm.h            # VM 数据结构、指令/错误码定义
├── src/
│   ├── m_vm.c            # 虚拟机核心实现
│   ├── disasm.c          # 反汇编与追踪输出
│   └── mian.c            # 测试套件与示例程序
├── M-Token规范.md
├── M 语言体系完整大纲.md
├── 基本构思.txt
└── README.md
```

---

## 快速运行

本项目是标准的 C 工程，直接编译 `mian.c` 可运行测试套件。

Windows + MSVC（示例）：
```powershell
cl /I include src\m_vm.c src\disasm.c src\mian.c
```

运行后将输出：
- 反汇编结果
- 执行结果/故障码
-（可选）执行追踪摘要

---

## 指令表（核心摘录）

说明：所有 opcode 与参数均采用 LEB128 varint 编码。下表为**逻辑编号**（即解码后的 opcode 值）。

| 分组 | 指令 | 值 | 作用 |
| :-- | :-- | :-- | :-- |
| Control | `B` / `E` | 10 / 11 | 结构化块起止 |
| Control | `IF` | 12 | 条件结构：`<cond>,IF,B,<then>,E,B,<else>,E` |
| Control | `JZ` / `JMP` | 13 / 14 | 条件跳转 / 无条件跳转 |
| Control | `FN` / `CL` / `RT` | 15 / 17 / 16 | 函数定义 / 调用 / 返回 |
| Data | `LIT` | 30 | 字面量入栈 |
| Data | `V` / `LET` / `SET` | 31 / 32 / 33 | 读局部 / 写局部 / 写全局 |
| Compare | `LT` `GT` `LE` `GE` `EQ` | 40-44 | 比较指令 |
| Math | `ADD` `SUB` `MUL` `DIV` | 50-53 | 算术运算 |
| Bit | `AND` `OR` `XOR` `SHL` `SHR` | 54-58 | 位运算 |
| Array | `LEN` `GET` `PUT` | 60-62 | 数组长度 / 读 / 写 |
| Stack | `DUP` `DRP` `ROT` `SWP` | 64-66 / 63 | 栈操作 |
| IO | `IOW` / `IOR` | 70 / 71 | IO 写 / IO 读 |
| System | `GTWAY` `WAIT` `TRACE` `HALT` | 80-83 / 82 | 授权 / 延时 / 追踪 / 停止 |

完整定义见 `include/m_vm.h` 与 `M-Token规范.md`。

---

## 指令与执行模型（简述）

- **编码**：所有 opcode 与操作数均为 LEB128 varint  
- **执行模型**：栈式为主，函数调用使用局部变量帧  
- **授权**：GTWAY 后允许单次 IOW，执行后授权自动失效  
- **约束**：step limit + gas limit + 多类边界检查  

更完整说明请看 `M-Token规范.md` 与 `M 语言体系完整大纲.md`。

---

## 运行输出示例（节选）

以下为运行测试套件时的典型输出片段（实际输出与编译环境、日志开关有关）：

```text
M Language Virtual Machine - Test Suite
Program: Arithmetic (5 + 3 * 2)
Bytecode size: 9 bytes
... disassembly ...
Execution result: fault=NONE, steps=6, result=11

Program: Function (add 5 + 3)
... disassembly ...
Execution Trace Summary
Completed: YES
Steps: 8
Result: 8
```

---

## 里程碑方向（简化版）

短期：
- M-Token 规范与 VM 行为的 1:1 对齐
- 增加更多测试覆盖（边界/故障码/非法字节码）

中期：
- 字节码验证器（静态检查）
- 更完善的调试与可视化工具

长期：
- 面向嵌入式的裁剪版 MVM
- AI 到 M-Token 的编译/约束生成工具链

---

## 贡献指南（简版）

- 代码风格：保持现有 C 风格与命名方式；新增功能先写小型 demo 到 `mian.c`
- 测试要求：新增指令或 VM 行为需补充最小可运行样例
- 提交内容：避免引入与核心 VM 无关的大型依赖
- 文档同步：变更 opcode/语义时需更新 `M-Token规范.md`

如果需要更细的协作流程或版本规范，可以在此基础上扩展。

---

## 版权与许可

Copyright (c) 2026 MYJ-GOD. All Rights Reserved.

开源许可：MIT License
