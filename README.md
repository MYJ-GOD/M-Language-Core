# M-Language Core (v1.3)

**An AI-Native Instruction Set Architecture (ISA) for Secure Logic Injection and Low-Entropy Hardware Control.**

---

## 🌟 项目愿景 (The Vision)

**M-Language** 旨在打破 AI 与物理硬件之间的壁垒。它不再依赖臃肿的操作系统或静态固件，而是允许 AI 将动态的**意图（Intent）**直接转化为极简的**指令流（Bytecode）**，实现“逻辑即插即用”。

> **核心价值**：在仅有 1KB RAM 的设备上，实现受安全保护的、由 AI 驱动的动态行为控制。

---

## 🛠 技术规范 (Technical Specification)

本项目定义了一套完整的二进制编码协议，旨在实现极致的体积压缩与解析效率。

### 1. 核心操作码 (Opcode Map)

| 类型 | 助记符 | 编码 (Hex) | 功能描述 |
| :--- | :--- | :--- | :--- |
| **Data** | `LIT` | 0x30 | 立即数压栈 (支持 Varint 变长编码) |
| | `V` | 0x31 | 读取变量 (De Bruijn Index 作用域) |
| | `LET` | 0x32 | 存入变量池 |
| **Logic** | `GT` / `LT` | 0x41 / 0x40 | 大于 / 小于比较 |
| | `EQ` | 0x44 | 等值判断 |
| **Math** | `ADD` / `SUB` | 0x50 / 0x51 | 加法 / 减法 |
| **Flow** | `IF` | 0x12 | 条件跳转分支 (基于栈顶布尔值) |
| | `FN` / `RT` | 0x15 / `0x16` | 函数定义起始 / 返回 |
| **Sys** | **`GATE`** | **0x8F** | **[核心] 安全鉴权网关** |
| | `PUT` | 0x62 | 物理 IO 写入 (受 GATE 保护) |

### 2. 执行模型 (Execution Model)
* **评估模式**: Stack/SSA Hybrid (栈式评估与静态单赋值混合)。
* **变量管理**: De Bruijn Index (无名变量作用域)。
* **数据编码**: 全指令集采用 LEB128 Varint 编码。

---

## 🛡 安全机制：GATE 指令 (The GATE Protocol)

这是 M-Language 区别于传统 ISA 的核心创新。为了防止 AI 产生逻辑故障（如幻觉代码）导致硬件损毁，我们定义了**指令级熔断机制**：

1. **拦截**：所有涉及物理修改的指令（如 PUT）在 VM 内部默认处于锁定状态。
2. **鉴权**：指令流必须前置 0x8F (GATE) 并配合正确的动态密钥方可临时解锁。
3. **单次生效**：高危指令执行完毕后，鉴权位立即失效。任何后续敏感操作必须重新发起 GATE 校验。

---

## 🚀 快速开始 (Quick Start)

### 工程结构
```text
M-Language-Core/
├── include/
│   └── m_vm.h       # ISA 架构与数据结构定义
├── src/
│   └── m_vm.c       # 虚拟机核心运行引擎
└── examples/
    └── robot_logic.c # AI 注入逻辑示例
逻辑注入示例 (C Implementation)
C
// 示例逻辑：如果传感器 A (Var 0) > 20，则解锁 GATE 并开启 0xDD 端口
uint8_t ai_bytecode[] = { 
    0x31, 0x00,             // V, 0
    0x30, 0x14,             // LIT, 20
    0x41,                   // GT
    0x12, 0x09,             // IF, 9
    0x8F, 0xE8, 0x0F,       // GATE, 2024
    0x30, 0x01, 0x30, 0xDD, // Data
    0x62,                   // PUT
    0x16                    // RT
};
m_vm_run(&vm);
⚖️ 版权与防御性公开声明 (Legal & Patent Notice)
Copyright (c) 2026 [MYJ-GOD]. All Rights Reserved.

发明优先权声明：本项目所阐述之“AI 意图驱动的极简指令集”、“GATE 指令级熔断机制”及“De Bruijn 索引在流式字节码中的应用”均为本人原创发明。

现有技术 (Prior Art)：此仓库的初始提交日期（2026-02-01）具有全球公认的法律效力。本项目旨在作为“现有技术”公开，以防止任何个人或机构在此日期之后对相同架构申请独占性专利。

开源许可：本项目基于 MIT License 开源。