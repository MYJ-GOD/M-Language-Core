# M-Language

M 语言是一个三层架构的 AI-硬件通信协议：

```
┌─────────────────────────────────────────┐
│  Intent Layer (自然语言)                 │
│  "如果水位超过 500，开启继电器 10 秒"     │
└────────────────┬────────────────────────┘
                 ↓
┌─────────────────────────────────────────┐
│  M-Token Layer (Core 指令集)            │
│  [30, 1, 71, 30, 500, 41, 12, 10, ...] │
└────────────────┬────────────────────────┘
                 ↓
┌─────────────────────────────────────────┐
│  MVM Layer (栈机虚拟机)                 │
│  执行、验证、安全约束                   │
└─────────────────────────────────────────┘
```

## 项目结构

```
M-Language-Core/
├── Core/           # MVM 虚拟机核心（C 语言）
│   ├── include/    # 头文件 (m_vm.h, disasm.h, validator.h)
│   └── src/        # 源码 (m_vm.c, disasm.c, main.c, validator.c)
├── MCP/            # MCP Server（Python）
│   ├── server.py   # MCP Server 主程序
│   ├── skills.py   # 3 个核心 Skills 实现
│   ├── protocol.md # 串口通信协议
│   └── requirements.txt
├── Examples/       # 使用示例
│   └── 01_water_level_demo/
├── Docs/           # 规范文档
│   ├── M-Token规范.md      # 指令集完整参考
│   ├── mcp和skills.md      # AI 接入规范
│   └── M语言体系完整大纲.md # 三层架构说明
└── README.md       # 本文件
```

## 快速开始

### 1. 构建 MVM

```bash
# Windows (Visual Studio)
MSBuild.exe M-Language-Core.sln /p:Configuration=Debug /p:Platform=x64

# Linux/macOS
gcc -o mvm src/m_vm.c src/disasm.c src/main.c src/validator.c -I include -O2
```

### 2. 运行测试

```bash
./x64/Debug/M-Language-Core.exe
```

### 3. 使用 MCP Server

```bash
cd MCP
pip install -r requirements.txt
python server.py --port COM3
```

## 文档

| 文件 | 描述 |
|------|------|
| [Docs/M-Token规范.md](./Docs/M-Token规范.md) | 指令集完整参考 |
| [Docs/mcp和skills.md](./Docs/mcp和skills.md) | AI 接入规范 |
| [Docs/M语言体系完整大纲.md](./Docs/M语言体系完整大纲.md) | 三层架构说明 |

## 示例

| 示例 | 描述 |
|------|------|
| [Examples/01_water_level_demo/](./Examples/01_water_level_demo/) | 水位监控完整场景 |

## 核心组件

### Core (MVM 虚拟机)

栈机架构，执行 M-Token 字节码：
- `include/m_vm.h` - VM 数据结构、opcode 定义
- `src/m_vm.c` - VM 核心实现
- `src/validator.c` - 静态验证器
- `src/disasm.c` - 反汇编器

### MCP (Python Server)

AI 与硬件的桥梁：
- `server.py` - MCP Server 主程序
- `skills.py` - 3 个核心 Skills
- `protocol.md` - 串口通信协议

### Examples (使用示例)

AI 提示词 + 预期 Token 输出：
- `system_prompt.txt` - AI 提示词
- `*.json` - 预期 M-Token + 验证数据

## 许可证

MIT
