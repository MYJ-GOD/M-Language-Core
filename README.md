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
│   ├── include/    # 头文件
│   ├── src/       # 源码
│   └── README.md
├── MCP/           # MCP Server（Python）
│   ├── server.py  # MCP Server 主程序
│   ├── skills.py  # Skills 实现
│   └── README.md
├── Examples/      # 使用示例
│   └── 01_water_level_demo/
├── Docs/         # 规范文档
│   ├── M-Token规范.md
│   ├── mcp和skills.md
│   └── README.md
└── README.md     # 本文件
```

## 快速开始

### 1. 构建 MVM

```bash
# Windows (Visual Studio)
MSBuild.exe Core/M-Language-Core.sln /p:Configuration=Debug /p:Platform=x64

# Linux/macOS
gcc -o mvm Core/src/m_vm.c Core/src/disasm.c Core/src/main.c Core/src/validator.c -I Core/include -O2
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

- [M-Token 规范](./Docs/M-Token规范.md) - 指令集完整参考
- [MCP & Skills](./Docs/mcp和skills.md) - AI 接入规范
- [架构概述](./Docs/M语言体系完整大纲.md) - 三层架构说明

## 示例

- [水位监控示例](./Examples/01_water_level_demo/) - 完整业务场景

## 许可证

MIT
